// SPDX-License-Identifier: GPL-3.0-only
/* Heatline: tiny HTTP server that exposes PS5 temperatures, fan, clocks and FPS.
 *
 * Open http://<PS5 IP>:8776/ in a browser on the same network.
 *   /      live dashboard (HTML page embedded in the payload)
 *   /api   JSON snapshot of all sensors
 *
 * Built with ps5-payload-sdk. Send to elfldr (port 9021).
 */

#include <sys/types.h>
#include <sys/proc.h>
#include <sys/user.h>
#include <sys/sysctl.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "app.h"
#include "fps.h"
#include "osd.h"
#include "osd_overlay.h"
#include "cpuload.h"
#include "context.h"
#include "storage.h"
#include "sysinfo.h"
#include "netlat.h"
#include "display.h"
#include "raw.h"
#include "ntp.h"
#include "fan.h"
#include "selfstat.h"
#include "common_fps/overlay_metric_list.h"   /* OVM_* ids shared with the overlay engine */
#include "page_html.h"
#include "jbuf.h"

#define HTTP_PORT 8776

#define APP_NAME  "Heatline"
/* Set by the Makefile from the VERSION file */
#ifndef HEATLINE_VERSION
#define HEATLINE_VERSION "0.0.0-dev"
#endif
#define LOG_TAG   "[heatline] "
#define PROC_NAME "heatline.elf"
/* Name used by builds before the rename; still recognised so a new Heatline
 * can replace an old instance that is already running. */
#define LEGACY_PROC_NAME "ps5-telemetry.elf"
/* Channels 0..7: the same set PS5 Thermal Doctor reads, and confirmed working
 * on this console (FW 13.60). An earlier build that read 0..64 caused a kernel
 * panic, so nothing above 7 is ever read. */
static const int SAFE_CH[] = {0, 1, 2, 3, 4, 5, 6, 7};

/* libkernel_sys. Signatures and decoding follow the SDK hwinfo sample,
 * PS5 Thermal Doctor (src/platform.c) and ps5-new-overlay. Weak, so a symbol
 * missing on some firmware only loses that reading. */
int  sceKernelGetHwModelName(char *)                               __attribute__((weak));
int  sceKernelGetHwSerialNumber(char *)                            __attribute__((weak));
long sceKernelGetCpuFrequency(void)                                __attribute__((weak));
int  sceKernelGetCpuTemperature(int *)                             __attribute__((weak));
int  sceKernelGetSocSensorTemperature(int, int *)                  __attribute__((weak));
int  sceKernelGetCurrentFanDuty(uint16_t *duty, void *scratch)     __attribute__((weak));
int  sceKernelGetSocPowerConsumption(void *out)                    __attribute__((weak));
int  sceKernelGetSocClock(uint32_t *out_raw_domains)               __attribute__((weak));
/* console clock: UTC -> local time with the console's own time zone settings
 * (PS4/PS5 libkernel: int fn(time_t utc, time_t *local, tz *, int *dst_sec)) */
int  sceKernelConvertUtcToLocaltime(time_t utc, time_t *local, void *tz, int *dst) __attribute__((weak));
int  sceKernelGetCpuCoreClock(int *out_mhz)                        __attribute__((weak));
/* RAM, method from PS5 Monitor 1.4.8 (also used by ps5-hwinfo / ps5-exporter):
 * get_page_table_stats(vm, type, &total_MiB, &free_MiB). vm 0 type 1 = the
 * system's memory, vm 1 type 1 = the game's (only counted from 512 MiB:
 * without a game it shows a few tens of MiB). type 2 views are GPU mappings
 * of the same pages: never added. Physical total = 16 GiB (unified GDDR6). */
int  get_page_table_stats(int vm, int type, int *total, int *free)  __attribute__((weak));

/* Same notification call as the SDK's hello_world sample (libkernel only) */
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);


void
notify(const char *msg) {
  notify_request_t req;

  memset(&req, 0, sizeof req);
  strncpy(req.message, msg, sizeof req.message - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
  klog_printf(LOG_TAG "%s\n", msg);
}




/* Does process `pid` own a socket bound locally to HTTP_PORT? */
static int
owns_port(pid_t pid) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_FILEDESC, pid};
  size_t len = 0;
  int found = 0;

  if(sysctl(mib, 4, NULL, &len, NULL, 0) || !len) {
    return -1;
  }
  len += len / 2;
  char *buf = malloc(len);
  if(!buf) {
    return -1;
  }
  if(sysctl(mib, 4, buf, &len, NULL, 0)) {
    free(buf);
    return -1;
  }
  for(char *p = buf; p < buf + len;) {
    struct kinfo_file *kf = (struct kinfo_file *)p;
    if(kf->kf_structsize <= 0) {
      break;
    }
    if(kf->kf_type == KF_TYPE_SOCKET && kf->kf_sock_domain == AF_INET) {
      struct sockaddr_in *sin = (struct sockaddr_in *)&kf->kf_sa_local;
      if(ntohs(sin->sin_port) == HTTP_PORT) {
        found = 1;
        break;
      }
    }
    p += kf->kf_structsize;
  }
  free(buf);
  return found;
}


/* Stop an older Heatline (or pre-rename ps5-telemetry) still holding the port. elfldr names payloads
 * via thr_set_name, so match on the thread name (ki_tdname), like
 * ps5-fan-control does. Kills:
 *  - any process named PROC_NAME or LEGACY_PROC_NAME (builds name themselves), and
 *  - a "payload.elf" (elfldr default for nc) that owns a socket on HTTP_PORT;
 *    if sockets can't be inspected, only when it is the single candidate.
 * Returns number of processes killed. */
static int
stop_old_instance(void) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t len = 0;
  int killed = 0;
  pid_t me = getpid();
  pid_t unsure[16];
  int n_unsure = 0, n_candidates = 0;

  if(sysctl(mib, 4, NULL, &len, NULL, 0)) {
    klog_perror(LOG_TAG "sysctl proc");
    return 0;
  }
  len += len / 2;
  char *buf = malloc(len);
  if(!buf) {
    return 0;
  }
  if(sysctl(mib, 4, buf, &len, NULL, 0)) {
    klog_perror(LOG_TAG "sysctl proc");
    free(buf);
    return 0;
  }
  for(char *p = buf; p < buf + len;) {
    /* Raw offsets as used by ps5-fan-control on real hardware: the PS5
     * kinfo_proc layout differs from the SDK header after ki_pid. */
    int structsize = *(int *)p;
    if(structsize <= 0) {
      break;
    }
    pid_t pid = *(pid_t *)(p + 72);
    char tdname[32];
    memcpy(tdname, p + 447, sizeof tdname - 1);
    tdname[sizeof tdname - 1] = '\0';
    p += structsize;
    if(pid == me) {
      continue;
    }

    int kill_it = 0;
    if(!strcmp(tdname, PROC_NAME) || !strcmp(tdname, LEGACY_PROC_NAME)) {
      kill_it = 1;
    } else if(!strcmp(tdname, "payload.elf")) {
      n_candidates++;
      int own = owns_port(pid);
      if(own == 1) {
        kill_it = 1;
      } else if(own < 0 && n_unsure < 16) {
        unsure[n_unsure++] = pid;
      }
    }
    if(kill_it) {
      klog_printf(LOG_TAG "stopping old instance pid %d (%s)\n",
                  pid, tdname);
      if(!kill(pid, SIGKILL)) {
        killed++;
      } else {
        klog_perror(LOG_TAG "kill");
      }
    }
  }
  free(buf);

  if(!killed && n_candidates == 1 && n_unsure == 1) {
    klog_printf(LOG_TAG "stopping single payload.elf pid %d\n", unsure[0]);
    if(!kill(unsure[0], SIGKILL)) {
      killed++;
    }
  }
  return killed;
}


static long
uptime_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}


static void
json_soc(char *buf, size_t size, int *n, int ch, bool *first) {
  int t = 0;
  if(sceKernelGetSocSensorTemperature(ch, &t) != 0) {
    return;
  }
  *n = jcat(buf, size, *n, "%s[%d,%d]", *first ? "" : ",", ch, t);
  *first = false;
}


/* Replace anything that would break the JSON string. */
static void
json_clean(char *s) {
  for(char *p = s; *p; p++) {
    if(*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) {
      *p = ' ';
    }
  }
}


/* Build the JSON snapshot. Returns body length. */
static int
build_json(char *buf, size_t size, bool ext) {
  char model[256] = "";
  int n = 0, t = 0;
  bool first = true;

  /* Serial number: read once (same call as the SDK hwinfo sample). Large
   * zeroed buffer since the API takes no size. Empty if unavailable. */
  static char serial[1024];
  static bool serial_read = false;
  /* Firmware from libSceLibcInternal (SDK helper, e.g. 0x13600000 = 13.60);
   * the SDK itself relies on it to pick per-firmware kernel offsets. */
  uint32_t fw = kernel_get_fw_version();

  if(sceKernelGetHwModelName) {
    sceKernelGetHwModelName(model);
  }
  json_clean(model);
  if(!serial_read) {
    serial_read = true;
    if(!sceKernelGetHwSerialNumber || sceKernelGetHwSerialNumber(serial)) {
      serial[0] = '\0';
    }
    serial[64] = '\0';
    json_clean(serial);
  }

  n = jcat(buf, size, n, "{\"t\":%ld,\"version\":\"%s\",\"model\":\"%s\",\"ext\":%s,",
                uptime_ms(), HEATLINE_VERSION, model, ext ? "true" : "false");
  if(fw) {
    n = jcat(buf, size, n, "\"fw\":\"%x.%02x\",", (fw >> 24) & 0xff, (fw >> 16) & 0xff);
  } else {
    n = jcat(buf, size, n, "\"fw\":null,");
  }
  n = jcat(buf, size, n, "\"serial\":\"%s\",", serial);

  if(sceKernelGetCpuTemperature && sceKernelGetCpuTemperature(&t) == 0) {
    n = jcat(buf, size, n, "\"cpu_temp\":%d,", t);
  } else {
    n = jcat(buf, size, n, "\"cpu_temp\":null,");
  }

  /* CPU clock AS REPORTED (sceKernelGetCpuCoreClock): NOT the live clock - a timed
   * IMUL test measured 3070-3465 MHz on cores 5-8 while this reported 2560 / 3200.
   * Only exported to the CSV (cpu_reported_*). Original note:
   * per physical core (sceKernelGetCpuCoreClock,
   * ps5-hwinfo / ps5-new-overlay; 8 values written on this console, the rest
   * left untouched). cpu_mhz = highest core. Falls back to the single value of
   * sceKernelGetCpuFrequency if the per-core call fails. Oversized buffer
   * pre-filled with -1 since the API takes no size. */
  int cpu_mhz = 0, ncores = 0;
  int cores[64];
  for(int c = 0; c < 64; c++) cores[c] = -1;
  if(sceKernelGetCpuCoreClock && sceKernelGetCpuCoreClock(cores) == 0) {
    for(int c = 0; c < 16; c++) {
      if(cores[c] <= 0 || cores[c] >= 10000) break;
      ncores = c + 1;
      if(cores[c] > cpu_mhz) cpu_mhz = cores[c];
    }
  }
  bool cpu_live = cpu_mhz > 0;
  if(!cpu_live && sceKernelGetCpuFrequency) {
    cpu_mhz = (int)(sceKernelGetCpuFrequency() / (1000 * 1000));
  }
  /* Order: the API lists the 4 cores the system runs on (threads 9-16) first
   * and the 4 game cores (threads 1-8) last. Seen on FW 13.60: in the menu
   * values 1-4 read 3200 while threads 9-16 are the busy ones and threads
   * 1-8 idle, values 5-8 read 800; a pinned busy loop on threads 10-16 ran at
   * >= 2.3 GHz. Re-ordered here so cpu_cores[k] is core k+1 (threads 2k+1,
   * 2k+2). The order inside each group of 4 is assumed to be the same. */
  n = jcat(buf, size, n, "\"cpu_cores\":[");
  for(int c = 0; c < ncores; c++) {
    int src = ncores == 8 ? (c + 4) % 8 : c;
    n = jcat(buf, size, n, "%s%d", c ? "," : "", cores[src]);
  }
  n = jcat(buf, size, n, "],");
  n = jclamp(n + (long long)cpuload_json(buf + n, size - n), size);
  n = jclamp(n + (long long)context_json(buf + n, size - n), size);
  n = jclamp(n + (long long)osd_json(buf + n, size - n), size);
  {
    time_t utc = time(NULL), local = utc;
    static uint8_t tzbuf[64];
    static int dst[4];
    int ok = sceKernelConvertUtcToLocaltime && sceKernelConvertUtcToLocaltime(utc, &local, tzbuf, dst) == 0;
    long off = ok ? (long)(local - utc) : 0;
    if(off < -14 * 3600 || off > 14 * 3600) { ok = 0; off = 0; }   /* sanity: real zones are within +-14 h */
    n = jcat(buf, size, n, "\"clock\":{\"utc\":%lld,\"offset_s\":%ld,\"tz_ok\":%s},",
                  (long long)utc, off, ok ? "true" : "false");
  }
  n = jclamp(n + (long long)storage_json(buf + n, size - n), size);
  n = jclamp(n + (long long)sysinfo_json(buf + n, size - n), size);
  n = jclamp(n + (long long)netlat_json(buf + n, size - n), size);
  n = jclamp(n + (long long)display_json(buf + n, size - n), size);
  n = jclamp(n + (long long)ntp_json(buf + n, size - n), size);
  n = jclamp(n + (long long)fan_json(buf + n, size - n), size);
  n = jclamp(n + (long long)selfstat_json(buf + n, size - n), size);
  uint32_t clk[64] = {0};
  uint32_t gpu_mhz = 0;
  if(sceKernelGetSocClock && sceKernelGetSocClock(clk) == 0 && clk[20] < 10000) {
    gpu_mhz = clk[20];
  }
  n = jcat(buf, size, n,
                "\"cpu_mhz\":%d,\"cpu_mhz_live\":%s,\"gpu_mhz\":%u,",
                cpu_mhz, cpu_live ? "true" : "false", gpu_mhz);

  /* Fan: raw duty 0..1024; 0xFFFF right after rest mode. */
  uint16_t duty = 0xFFFF;
  uint8_t scratch[64] = {0};
  if(sceKernelGetCurrentFanDuty &&
     sceKernelGetCurrentFanDuty(&duty, scratch) == 0 && duty <= 1024) {
    n = jcat(buf, size, n, "\"fan_raw\":%u,", duty);
  } else {
    n = jcat(buf, size, n, "\"fan_raw\":null,");
  }

  /* Power: 8 rails of {mW, mV, mA}. Community names: gpu_core, gpu_io, cpu_soc,
   * cpu_io, memory x4; measured here: rail 0 follows the GPU, rail 1 the CPU.
   * Per Thermal Doctor, the CPU rails can be a peak-hold value, not live. */
  uint32_t pw[64];
  bool pw_ok = false;
  memset(pw, 0, sizeof pw);
  if(sceKernelGetSocPowerConsumption && sceKernelGetSocPowerConsumption(pw) == 0) {
    pw_ok = true;
    for(int r = 0; r < 8; r++) {
      if(pw[r*3] > 400000u || pw[r*3+1] > 2500u || pw[r*3+2] > 400000u) {
        pw_ok = false;                    /* unexpected layout */
      }
    }
  }
  if(pw_ok) {
    n = jcat(buf, size, n, "\"power_lanes_mw\":[%u,%u,%u,%u,%u,%u,%u,%u],",
                  pw[0], pw[3], pw[6], pw[9], pw[12], pw[15], pw[18], pw[21]);
    /* Rail 0 = GPU, rail 1 = CPU: measured on FW 13.60 (CFI-2016). A CPU-only
     * busy thread switched ON OFF ON ON OFF OFF ON OFF (2-s steps) moved rail 1
     * by +0.84 W on all 8 steps while rail 0 stayed flat; rail 0 follows the
     * GPU (3.2 W menu, ~93 W in a game). Rails 2-3 only update now and then
     * (rarely change: frozen 28 s in the menu, unchanged 6 min in a game) and rails 4-7 read 0: not used. */
    n = jcat(buf, size, n, "\"power_mw\":{\"gpu\":%u,\"cpu\":%u},", pw[0], pw[3]);
    /* same call, the other two words of each rail: mV and mA (ps5-hwinfo /
     * ps5-exporter layout; P = V x I holds per rail). Rail 0 GPU, rail 1 CPU. */
    n = jcat(buf, size, n, "\"volt_mv\":{\"gpu\":%u,\"cpu\":%u},\"cur_ma\":{\"gpu\":%u,\"cpu\":%u},",
                  pw[1], pw[4], pw[2], pw[5]);
    n = jcat(buf, size, n, "\"power_lanes_mv\":[%u,%u,%u,%u,%u,%u,%u,%u],\"power_lanes_ma\":[%u,%u,%u,%u,%u,%u,%u,%u],",
                  pw[1], pw[4], pw[7], pw[10], pw[13], pw[16], pw[19], pw[22],
                  pw[2], pw[5], pw[8], pw[11], pw[14], pw[17], pw[20], pw[23]);
  } else {
    n = jcat(buf, size, n, "\"power_mw\":null,");
  }

  if(get_page_table_stats) {
    int t[4] = {0}, f[4] = {0}, rc[4];
    static const int VM[4] = {0, 0, 1, 1}, TY[4] = {1, 2, 1, 2};
    for(int i = 0; i < 4; i++) rc[i] = get_page_table_stats(VM[i], TY[i], &t[i], &f[i]);
    bool sys_ok = rc[0] == 0 && t[0] > 0 && f[0] >= 0 && f[0] <= t[0];
    bool game_ok = rc[2] == 0 && t[2] > 0 && f[2] >= 0 && f[2] <= t[2];
    int sys_used = sys_ok ? t[0] - f[0] : -1;
    int game_used = game_ok ? t[2] - f[2] : 0;
    if(game_used < 512) game_used = 0;
    n = jcat(buf, size, n, "\"ram\":{\"total_mb\":16384,\"sys_mb\":%d,\"game_mb\":%d,\"raw\":[", sys_used, game_used);
    for(int i = 0; i < 4; i++) {
      n = jcat(buf, size, n, "%s[%d,%d,%d]", i ? "," : "", rc[i], t[i], f[i]);
    }
    n = jcat(buf, size, n, "]},");
  }
  n = jclamp(n + (long long)fps_json(buf + n, size - n), size);
  n = jcat(buf, size, n, ",\"soc\":[");
  if(sceKernelGetSocSensorTemperature) {
    (void)ext;
    for(size_t i = 0; i < sizeof SAFE_CH / sizeof SAFE_CH[0]; i++) {
      json_soc(buf, size, &n, SAFE_CH[i], &first);
    }
  }
  n = jcat(buf, size, n, "]}");
  return n;
}


/* ------------------------------------------------------------------------
 * In-game overlay: the current value of every metric the overlay can show.
 *
 * Called by the overlay engine about once a second. Fills v[OVM_*] and sets
 * have[OVM_*] for each metric read right now; the ids come from
 * common_fps/overlay_metric_list.h, the same list the engine builds its
 * registry from. Reads the same sources, with the same sanity checks, as
 * build_json above. Returns 0, or -1 if `count` is not the OVM_COUNT this build
 * knows (engine and Heatline out of step: nothing is filled).
 * ------------------------------------------------------------------------ */
#define OV_PUT(id, val) do { v[OVM_##id] = (double)(val); have[OVM_##id] = 1; } while(0)
#define OV_PUT_IF(id, val) do { double x_ = (double)(val); if(x_ >= 0) OV_PUT(id, x_); } while(0)

int
overlay_collect_metrics(double *v, unsigned char *have, int count) {
  if(count != OVM_COUNT) return -1;
  memset(have, 0, (size_t)count);

  /* game performance */
  fps_overlay_t f;
  if(fps_overlay_all(&f)) {
    OV_PUT_IF(FPS, f.fps);       OV_PUT_IF(AVG, f.avg);     OV_PUT_IF(P1, f.p1);
    OV_PUT_IF(P02, f.p02);       OV_PUT_IF(LOW1, f.low1);   OV_PUT_IF(LOW02, f.low02);
    OV_PUT_IF(FT, f.ft);         OV_PUT_IF(FTWST, f.ft_worst);
    OV_PUT_IF(SAVG, f.savg);     OV_PUT_IF(SMIN, f.smin);   OV_PUT_IF(SMAX, f.smax);
    OV_PUT_IF(STUT, f.stut_pct); OV_PUT_IF(STUTN, f.stut_n);
    OV_PUT_IF(P5, f.p5);         OV_PUT_IF(P50, f.p50);     OV_PUT_IF(P95, f.p95);
    OV_PUT_IF(P99, f.p99);       OV_PUT_IF(P01, f.p01);     OV_PUT_IF(LOW01, f.low01);
    OV_PUT_IF(IL1, f.il1);       OV_PUT_IF(IL02, f.il02);   OV_PUT_IF(IL01, f.il01);
    OV_PUT_IF(ASTD, f.astd);     OV_PUT_IF(FTMIN, f.ftmin); OV_PUT_IF(FTMAX, f.ftmax);
  }

  /* temperatures: whole degrees C; 0 and very high values are "no reading" */
  int t = 0;
  if(sceKernelGetCpuTemperature && sceKernelGetCpuTemperature(&t) == 0 && t > 0 && t < 150) {
    OV_PUT(CPUT, t);
  }
  if(sceKernelGetSocSensorTemperature) {
    int hot = 0;
    for(size_t i = 0; i < sizeof SAFE_CH / sizeof SAFE_CH[0]; i++) {
      int st = 0;
      if(sceKernelGetSocSensorTemperature(SAFE_CH[i], &st) != 0 || st <= 0 || st >= 150) continue;
      if(SAFE_CH[i] == 0) OV_PUT(SOCT, st);       /* channel 0 = "SoC temp" */
      else if(st > hot) hot = st;                  /* channels 1-7: hottest */
    }
    if(hot > 0) OV_PUT(HOTT, hot);
  }

  /* clocks: CPU = highest core clock as reported, GPU = SoC clock domain 20 */
  int cores[64];
  for(int c = 0; c < 64; c++) cores[c] = -1;
  if(sceKernelGetCpuCoreClock && sceKernelGetCpuCoreClock(cores) == 0) {
    int mhz = 0;
    for(int c = 0; c < 16; c++) {
      if(cores[c] <= 0 || cores[c] >= 10000) break;
      if(cores[c] > mhz) mhz = cores[c];
    }
    if(mhz > 0) OV_PUT(CPUMHZ, mhz);
  }
  /* per core, in the dashboard's order: the API lists the 4 system cores first
   * and the 4 game cores last, so core k+1 is entry (k + 4) % 8 of 8 */
  {
    int ncores = 0;
    for(int c = 0; c < 16; c++) {
      if(cores[c] <= 0 || cores[c] >= 10000) break;
      ncores = c + 1;
    }
    if(ncores == 8) {
      static const int CORE_MHZ_ID[8] = {OVM_C1MHZ, OVM_C2MHZ, OVM_C3MHZ, OVM_C4MHZ,
                                          OVM_C5MHZ, OVM_C6MHZ, OVM_C7MHZ, OVM_C8MHZ};
      for(int c = 0; c < 8; c++) {
        v[CORE_MHZ_ID[c]] = cores[(c + 4) % 8];
        have[CORE_MHZ_ID[c]] = 1;
      }
    }
  }
  uint32_t clk[64] = {0};
  if(sceKernelGetSocClock && sceKernelGetSocClock(clk) == 0 && clk[20] > 0 && clk[20] < 10000) {
    OV_PUT(GPUMHZ, clk[20]);
  }

  /* power: rail 0 = GPU, rail 1 = CPU; {mW, mV, mA} per rail, same layout check
   * as build_json */
  uint32_t pw[64];
  memset(pw, 0, sizeof pw);
  if(sceKernelGetSocPowerConsumption && sceKernelGetSocPowerConsumption(pw) == 0) {
    bool pw_ok = true;
    for(int r = 0; r < 8; r++) {
      if(pw[r*3] > 400000u || pw[r*3+1] > 2500u || pw[r*3+2] > 400000u) pw_ok = false;
    }
    if(pw_ok) {
      OV_PUT(GPUW, pw[0] / 1000.0);   OV_PUT(CPUW, pw[3] / 1000.0);
      OV_PUT(TOTW, (pw[0] + pw[3]) / 1000.0);
      OV_PUT(GVOLT, pw[1] / 1000.0);  OV_PUT(CVOLT, pw[4] / 1000.0);
      OV_PUT(GAMP, pw[2] / 1000.0);   OV_PUT(CAMP, pw[5] / 1000.0);
    }
  }

  /* fan: duty 0..1024 as a percentage; 0xFFFF right after rest mode = no reading */
  uint16_t duty = 0xFFFF;
  uint8_t scratch[64] = {0};
  if(sceKernelGetCurrentFanDuty && sceKernelGetCurrentFanDuty(&duty, scratch) == 0 && duty <= 1024) {
    OV_PUT(FAN, duty * 100.0 / 1024.0);
  }

  /* memory: MiB in use (system pool, game pool) as GiB */
  if(get_page_table_stats) {
    int tt = 0, ff = 0;
    if(get_page_table_stats(0, 1, &tt, &ff) == 0 && tt > 0 && ff >= 0 && ff <= tt) {
      OV_PUT(RAMS, (tt - ff) / 1024.0);
    }
    tt = ff = 0;
    if(get_page_table_stats(1, 1, &tt, &ff) == 0 && tt > 0 && ff >= 0 && ff <= tt && tt - ff >= 512) {
      OV_PUT(RAMG, (tt - ff) / 1024.0);
    }
  }

  /* CPU load, network throughput, latency */
  OV_PUT_IF(LOAD, cpuload_avg());
  {
    /* per core: the mean of its two hardware threads (core k+1 = threads 2k, 2k+1) */
    static const int CORE_LOAD_ID[8] = {OVM_C1LOAD, OVM_C2LOAD, OVM_C3LOAD, OVM_C4LOAD,
                                         OVM_C5LOAD, OVM_C6LOAD, OVM_C7LOAD, OVM_C8LOAD};
    int th[CPULOAD_MAX];
    cpuload_snapshot(th);
    for(int c = 0; c < 8; c++) {
      int a = th[2 * c], b = th[2 * c + 1];
      if(a >= 0 && b >= 0) { v[CORE_LOAD_ID[c]] = (a + b) / 2.0; have[CORE_LOAD_ID[c]] = 1; }
      else if(a >= 0 || b >= 0) { v[CORE_LOAD_ID[c]] = a >= 0 ? a : b; have[CORE_LOAD_ID[c]] = 1; }
    }
  }
  double rx = 0, tx = 0;
  if(sysinfo_rates(&rx, &tx)) {          /* bytes/s -> Mbit/s */
    OV_PUT(RX, rx * 8 / 1e6);
    OV_PUT(TX, tx * 8 / 1e6);
  }
  double lat[4];
  if(netlat_overlay(lat)) {
    OV_PUT_IF(LATGW, lat[0]);  OV_PUT_IF(JITGW, lat[1]);
    OV_PUT_IF(LATNET, lat[2]); OV_PUT_IF(JITNET, lat[3]);
  }
  return 0;
}
#undef OV_PUT
#undef OV_PUT_IF


static void
send_all(int fd, const char *data, size_t len) {
  while(len > 0) {
    ssize_t w = write(fd, data, len);
    if(w <= 0) {
      return;
    }
    data += w;
    len -= w;
  }
}


static void
respond(int fd, const char *status, const char *type,
        const char *body, size_t len) {
  char hdr[256];
  int n = snprintf(hdr, sizeof hdr,
                   "HTTP/1.1 %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "Cache-Control: no-store\r\n"
                   "X-Content-Type-Options: nosniff\r\n"
                   "Connection: close\r\n\r\n", status, type, len);
  send_all(fd, hdr, n);
  send_all(fd, body, len);
  selfstat_count((uint64_t)n + len);
}


/*
 * Request protection.
 *
 * Heatline has no login: anyone on the local network can open the dashboard.
 * What these checks stop is a web page on ANOTHER site (opened on any phone or
 * computer on the same network) talking to Heatline behind the user's back:
 *
 * 1. No "Access-Control-Allow-Origin" header is sent, so other sites cannot
 *    read any answer (for example the serial number in /api).
 * 2. Every request that changes something must carry the header
 *    "X-Heatline: 1". A browser only lets another site add a custom header
 *    after a CORS preflight, which Heatline never approves (OPTIONS -> 405),
 *    so such requests can only come from Heatline's own page or from a tool
 *    like curl run by someone on the network.
 * 3. The Host header must be an IP address, localhost, a one-word name (ps5)
 *    or a .local name. This blocks DNS rebinding, where an attacker's domain
 *    name is pointed at the console to get around rule 1.
 */
#define ACTION_HEADER "X-Heatline"

/* Value of header `name` (case-insensitive) copied into out; 0 if found. */
static int
get_header(const char *req, const char *name, char *out, size_t size) {
  size_t nl = strlen(name);
  const char *p = strstr(req, "\r\n");
  while(p && p[2] && !(p[2] == '\r' && p[3] == '\n')) {
    p += 2;
    if(!strncasecmp(p, name, nl) && p[nl] == ':') {
      const char *v = p + nl + 1;
      while(*v == ' ' || *v == '\t') v++;
      size_t i = 0;
      while(v[i] && v[i] != '\r' && v[i] != '\n' && i + 1 < size) {
        out[i] = v[i];
        i++;
      }
      while(i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t')) i--;
      out[i] = '\0';
      return 0;
    }
    p = strstr(p, "\r\n");
  }
  return -1;
}

/* True if the Host header names this console directly (see rule 3). */
static bool
host_ok(const char *host) {
  char h[256];
  size_t len;

  if(host[0] == '[') {                     /* IPv6 literal, [::1]:8776 */
    return strchr(host, ']') != NULL;
  }
  snprintf(h, sizeof h, "%s", host);
  char *colon = strchr(h, ':');            /* drop :port */
  if(colon) *colon = '\0';
  len = strlen(h);
  if(len == 0) {
    return false;
  }
  for(size_t i = 0; i < len; i++) {
    h[i] = (char)tolower((unsigned char)h[i]);
  }
  if(!strcmp(h, "localhost")) {
    return true;
  }

  /* IPv4 literal: four numbers 0-255 */
  int parts = 0, val = -1;
  bool ipv4 = true;
  for(size_t i = 0; i <= len && ipv4; i++) {
    char c = h[i];
    if(c >= '0' && c <= '9') {
      val = (val < 0 ? 0 : val) * 10 + (c - '0');
      if(val > 255) ipv4 = false;
    } else if(c == '.' || c == '\0') {
      if(val < 0) ipv4 = false;
      parts++;
      val = -1;
    } else {
      ipv4 = false;
    }
  }
  if(ipv4 && parts == 4) {
    return true;
  }

  /* one-word local name (ps5) or an mDNS name (ps5.local) */
  for(size_t i = 0; i < len; i++) {
    char c = h[i];
    if(!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) {
      return false;
    }
  }
  if(!strchr(h, '.')) {
    return true;
  }
  return len > 6 && !strcmp(h + len - 6, ".local");
}

/* Endpoints that change something on the console (rule 2). */
static bool
is_action(const char *path) {
  static const char *const actions[] = {
    "/fan/set", "/fan/apply", "/ntp/check", "/ntp/sync", "/ntp/auto",
    "/display/log/clear", "/refresh", "/lat/on", "/fps/more", "/fps/reset",
    "/fps/overlay", "/overlay/layout", "/overlay/style", "/quit", NULL
  };
  for(int i = 0; actions[i]; i++) {
    if(!strcmp(path, actions[i])) return true;
  }
  return false;
}

/* Shown when /quit is opened in a browser: the stop needs one more click. */
static const char quit_page[] =
  "<!doctype html><meta charset=utf-8>"
  "<meta name=viewport content=\"width=device-width\">"
  "<title>Stop Heatline</title>"
  "<body style=\"font:16px system-ui,sans-serif;background:#111;color:#eee;"
  "display:grid;place-items:center;height:100vh;margin:0\">"
  "<div style=text-align:center><p>Stop Heatline on this console?</p>"
  "<button id=b style=\"font:inherit;padding:.6em 1.4em\">Stop Heatline</button>"
  "<p id=m></p></div><script>"
  "b.onclick=()=>fetch('/quit',{headers:{'" ACTION_HEADER "':'1'}})"
  ".then(r=>r.text()).then(t=>{m.textContent=t;b.disabled=true})"
  ".catch(()=>{m.textContent='Stopped.';b.disabled=true})"
  "</script>";


static void
handle_client(int fd) {
  char req[2048];
  char json[16384];
  char hval[256];
  ssize_t n;
  size_t got = 0;
  struct timeval tv = {3, 0};

  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

  /* read until the end of the headers (a request can arrive in pieces).
   * One 3 s deadline for the whole request, not 3 s per piece: Heatline
   * answers one connection at a time, so a client sending a byte now and
   * then must not be able to hold the server. */
  struct timespec t0, now;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  req[0] = '\0';
  while(got < sizeof req - 1) {
    if(got > 0) {
      clock_gettime(CLOCK_MONOTONIC, &now);
      long long left_us = 3000000LL - ((now.tv_sec - t0.tv_sec) * 1000000LL +
                                       (now.tv_nsec - t0.tv_nsec) / 1000);
      if(left_us <= 0) {
        break;
      }
      struct timeval left = {(time_t)(left_us / 1000000), (suseconds_t)(left_us % 1000000)};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &left, sizeof left);
    }
    n = read(fd, req + got, sizeof req - 1 - got);
    if(n <= 0) {
      break;
    }
    got += (size_t)n;
    req[got] = '\0';
    if(strstr(req, "\r\n\r\n")) {
      break;
    }
  }
  if(got == 0) {
    return;
  }

  if(strncmp(req, "GET ", 4)) {
    respond(fd, "405 Method Not Allowed", "text/plain", "GET only\n", 9);
    return;
  }

  /* headers are checked before the path is cut out of the buffer */
  if(get_header(req, "Host", hval, sizeof hval) || !host_ok(hval)) {
    static const char e[] = "Forbidden: open Heatline with the console's IP address\n";
    respond(fd, "403 Forbidden", "text/plain", e, sizeof e - 1);
    return;
  }
  bool trusted = !get_header(req, ACTION_HEADER, hval, sizeof hval) &&
                 !strcmp(hval, "1");

  char *path = req + 4;
  char *end = strchr(path, ' ');
  if(end) {
    *end = '\0';
  }
  bool ext = false, on = false;
  char qs[256] = "";
  char *q = strchr(path, '?');
  if(q) {
    ext = strstr(q, "ext=1") != NULL;
    on = strstr(q, "on=1") != NULL;
    snprintf(qs, sizeof qs, "%s", q + 1);
    *q = '\0';
  }

  if(is_action(path) && !trusted) {
    if(!strcmp(path, "/quit")) {           /* opened by hand: ask first */
      respond(fd, "200 OK", "text/html; charset=utf-8", quit_page, sizeof quit_page - 1);
    } else {
      static const char e[] = "Forbidden: missing " ACTION_HEADER " header\n";
      respond(fd, "403 Forbidden", "text/plain", e, sizeof e - 1);
    }
    return;
  }

  if(!strcmp(path, "/api")) {
    int len = build_json(json, sizeof json, ext);
    respond(fd, "200 OK", "application/json", json, len);
  } else if(!strcmp(path, "/overlay/log")) {   /* temporary diagnostic */
    size_t cap = 128 * 1024;
    char *b = malloc(cap);
    if(b) { int len = osd_log_dump(b, cap); respond(fd, "200 OK", "text/plain; charset=utf-8", b, len); free(b); }
    else respond(fd, "500 Internal Server Error", "text/plain", "no memory\n", 10);
  } else if(!strcmp(path, "/fan/set")) {
    const char *err = fan_set(qs);
    if(err) respond(fd, "400 Bad Request", "text/plain", err, strlen(err));
    else respond(fd, "200 OK", "text/plain", "ok\n", 3);
  } else if(!strcmp(path, "/fan/apply")) {
    fan_apply_now();
    respond(fd, "200 OK", "text/plain", "ok\n", 3);
  } else if(!strcmp(path, "/ntp/check") || !strcmp(path, "/ntp/sync")) {
    if(ntp_start(path[5] == 's')) respond(fd, "409 Conflict", "text/plain", "busy\n", 5);
    else respond(fd, "200 OK", "text/plain", "started\n", 8);
  } else if(!strcmp(path, "/ntp/auto")) {
    if(ntp_autosync_set(on)) respond(fd, "500 Internal Server Error", "text/plain", "could not save\n", 14);
    else respond(fd, "200 OK", "text/plain", "ok\n", 3);
  } else if(!strcmp(path, "/fps/ft")) {
    const char *s = strstr(qs, "since=");
    unsigned long long since = s ? strtoull(s + 6, NULL, 10) : 0;
    int len = fps_ft_json(json, sizeof json, since);
    respond(fd, "200 OK", "application/json", json, len);
  } else if(!strcmp(path, "/display/log")) {
    int len = 0;
    char *j = display_log_json(&len);
    if(j) { respond(fd, "200 OK", "application/json", j, len); free(j); }
    else respond(fd, "500 Internal Server Error", "text/plain", "no memory\n", 10);
  } else if(!strcmp(path, "/display/log/clear")) {
    display_log_clear();
    respond(fd, "200 OK", "text/plain", "ok\n", 3);
  } else if(!strcmp(path, "/display/read")) {
    display_read();                     /* refresh button: read now (1 s timer otherwise) */
    json[0] = '{';                      /* "display":{...},  ->  {"display":{...}} */
    int len = 1 + display_json(json + 1, sizeof json - 2);
    if(len > 1 && json[len - 1] == ',') json[len - 1] = '}';
    respond(fd, "200 OK", "application/json", json, len);
  } else if(!strcmp(path, "/raw")) {
    /* "Raw data" sections: the original answers for one panel, read now. The
     * page only asks while such a section is open. */
    char panel[16] = "";
    const char *pp = strstr(qs, "p=");
    if(pp) { int i = 0; pp += 2; while(pp[i] && pp[i] != '&' && i < 15) { panel[i] = pp[i]; i++; } panel[i] = '\0'; }
    int len = raw_json(panel, json, sizeof json);
    respond(fd, "200 OK", "application/json", json, len);
  } else if(!strcmp(path, "/refresh")) {
    /* refresh buttons: re-measure the values that run on a slow timer */
    if(strstr(qs, "net")) sysinfo_refresh();
    if(strstr(qs, "lat")) netlat_now();
    respond(fd, "200 OK", "text/plain", "ok\n", 3);
  } else if(!strcmp(path, "/lat/on")) {
    netlat_set(on);
    respond(fd, "200 OK", "text/plain", "ok\n", 3);
  } else if(!strcmp(path, "/fps/more")) {
    fps_set_more(on);
    respond(fd, "200 OK", "text/plain", "ok\n", 3);
  } else if(!strcmp(path, "/fps/reset")) {
    fps_reset();
    respond(fd, "200 OK", "text/plain", "ok\n", 3);
  } else if(!strcmp(path, "/overlay/style")) {
    /* ?corner=0..3&mx=&my=&font=&opacity= : where and how big the in-game overlay
     * is. A value left out keeps its current setting; each is clamped. */
    int st[5];
    overlay_get_style(st);
    static const char *const keys[5] = {"corner=", "mx=", "my=", "font=", "opacity="};
    for(int k = 0; k < 5; k++) {
      const char *p = strstr(qs, keys[k]);
      /* only at the start of a parameter, so "mx=" never matches inside "xmx=" */
      if(p && (p == qs || p[-1] == '&')) st[k] = atoi(p + strlen(keys[k]));
    }
    if(osd_set_style(st[0], st[1], st[2], st[3], st[4]) == 0) respond(fd, "200 OK", "text/plain", "ok\n", 3);
    else respond(fd, "400 Bad Request", "text/plain", "bad corner\n", 11);
  } else if(!strcmp(path, "/overlay/layout")) {
    /* ?spec=FPS;AVG;P1;P0.2  (percent-encoded by the page): which metrics the
     * in-game overlay shows and how they are arranged */
    char spec[160] = "";
    const char *sp = strstr(qs, "spec=");
    if(sp) {
      sp += 5;
      int i = 0;
      while(*sp && *sp != '&' && i < (int)sizeof spec - 1) {
        if(sp[0] == '%' && isxdigit((unsigned char)sp[1]) && isxdigit((unsigned char)sp[2])) {
          char h[3] = {sp[1], sp[2], 0};
          spec[i++] = (char)strtol(h, NULL, 16);
          sp += 3;
        } else {
          spec[i++] = *sp == '+' ? ' ' : *sp;
          sp++;
        }
      }
      spec[i] = '\0';
    }
    if(osd_set_layout(spec) == 0) respond(fd, "200 OK", "text/plain", "ok\n", 3);
    else respond(fd, "400 Bad Request", "text/plain", "no known metric in that layout\n", 31);
  } else if(!strcmp(path, "/fps/overlay")) {
    osd_set(on);                        /* on-screen FPS overlay over the game */
    respond(fd, "200 OK", "text/plain", "ok\n", 3);
  } else if(!strcmp(path, "/quit")) {
    static const char bye[] = APP_NAME " stopped\n";
    respond(fd, "200 OK", "text/plain", bye, sizeof bye - 1);
    notify(APP_NAME " stopped");
    close(fd);
    _exit(0);
  } else if(!strcmp(path, "/") || !strcmp(path, "/index.html")) {
    respond(fd, "200 OK", "text/html; charset=utf-8",
            (const char *)index_html, index_html_len);
  } else {
    respond(fd, "404 Not Found", "text/plain", "not found\n", 10);
  }
}


int
main(void) {
  struct sockaddr_in addr;
  char msg[128];
  int one = 1;
  int srv;

  klog_puts(LOG_TAG "start");
  syscall(SYS_thr_set_name, -1, PROC_NAME);
  fps_start();          /* also escalates privileges (needed for /dev/dce) */
  osd_start();          /* on-screen FPS overlay thread; applies the saved flag, off by default */
  cpuload_start();      /* CPU usage sampler, once per second */
  signal(SIGPIPE, SIG_IGN);
  sysinfo_start();
  display_start();        /* display output mode, every second */
  netlat_start();         /* router / internet latency, one try each per 5 s */
  ntp_boot();            /* date & time sync at start, if turned on */
  fan_start();           /* fan control: applies the saved threshold, if turned on */

  if((srv = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    klog_perror(LOG_TAG "socket");
    return 1;
  }
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(HTTP_PORT);

  int bound = !bind(srv, (struct sockaddr *)&addr, sizeof addr);
  if(!bound && errno == EADDRINUSE && stop_old_instance() > 0) {
    notify("Stopped old " APP_NAME ", restarting...");
    for(int i = 0; i < 20 && !bound; i++) {   /* wait up to ~4 s for the port */
      usleep(200 * 1000);
      bound = !bind(srv, (struct sockaddr *)&addr, sizeof addr);
    }
  }
  if(!bound || listen(srv, 8)) {
    snprintf(msg, sizeof msg,
             "Port %d busy and old instance could not be stopped", HTTP_PORT);
    notify(msg);
    close(srv);
    return 1;
  }

  klog_puts(LOG_TAG "v" HEATLINE_VERSION " listening");
  snprintf(msg, sizeof msg, APP_NAME " v" HEATLINE_VERSION " on port %d", HTTP_PORT);
  notify(msg);

  /* Home-screen tile with the Heatline icon; opens this dashboard in the
   * PS5 browser. Done after privileges were raised (fps_start). */
  if(app_install_if_needed()) {
    klog_puts(LOG_TAG "home-screen tile could not be installed");
  }

  for(;;) {
    int fd = accept(srv, 0, 0);
    if(fd < 0) {
      if(errno == EINTR) {
        continue;
      }
      klog_perror(LOG_TAG "accept");
      break;
    }
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    handle_client(fd);
    close(fd);
  }

  close(srv);
  return 0;
}
