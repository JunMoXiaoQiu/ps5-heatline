// SPDX-License-Identifier: GPL-3.0-only
/* raw.c - "Raw data" view of each panel's About: the original answers of the
 * console's functions, before Heatline decodes them.
 *
 * Runs only inside a /raw?p=<panel> request, which the page sends only while a
 * Raw data section is open. Nothing here runs on a timer and no background
 * thread copies anything for it: with every Raw data section closed, this
 * file costs nothing. Undocumented calls run under the crash guard. */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <ifaddrs.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "raw.h"
#include "fps.h"
#include "cpuload.h"
#include "sysinfo.h"
#include "ntp.h"
#include "fan.h"
#include "display.h"

int  sceKernelGetHwModelName(char *)                                __attribute__((weak));
int  sceKernelGetCpuTemperature(int *)                              __attribute__((weak));
int  sceKernelGetSocSensorTemperature(int, int *)                   __attribute__((weak));
int  sceKernelGetCurrentFanDuty(uint16_t *duty, void *scratch)      __attribute__((weak));
int  sceKernelGetSocPowerConsumption(void *out)                     __attribute__((weak));
int  sceKernelGetSocClock(uint32_t *out_raw_domains)                __attribute__((weak));
int  sceKernelGetCpuCoreClock(int *out_mhz)                         __attribute__((weak));
int  sceKernelConvertUtcToLocaltime(time_t utc, time_t *local, void *tz, int *dst) __attribute__((weak));
int  get_page_table_stats(int vm, int type, int *total, int *free)  __attribute__((weak));
int  sceKernelIccGetPowerNumberOfBootShutdown(void *out)            __attribute__((weak));
int  sceKernelIccGetThermalAlert(void *out)                         __attribute__((weak));
int  sceKernelIccGetBDPowerState(void *out)                         __attribute__((weak));
int  sceNetCtlGetState(int *state)                                  __attribute__((weak));

typedef struct { char *b; int size, n; } out_t;
static void put(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
#include "jbuf.h"
static void put(out_t *o, const char *fmt, ...) {
  if(o->n >= o->size - 1) return;
  va_list ap;
  va_start(ap, fmt);
  int w = vsnprintf(o->b + o->n, o->size - o->n, fmt, ap);
  va_end(ap);
  if(w > 0) o->n = o->n + w < o->size ? o->n + w : o->size - 1;
}
static void hex(out_t *o, const char *key, const void *p, int len) {
  put(o, "\"%s\":\"", key);
  for(int i = 0; i < len; i++) put(o, "%02x", ((const uint8_t *)p)[i]);
  put(o, "\",");
}
static void str(out_t *o, const char *key, const char *s) {
  put(o, "\"%s\":\"", key);
  for(; *s; s++) put(o, (*s == '"' || *s == '\\') ? "\\%c" : ((unsigned char)*s < 0x20 ? "?" : "%c"), *s);
  put(o, "\",");
}

/* guarded wrappers for the ICC calls (undocumented argument sizes) */
struct icc { int (*fn)(void *); uint8_t buf[256]; int rc; };
static int do_icc(void *c) { struct icc *x = c; x->rc = x->fn(x->buf); return 0; }
static void icc(out_t *o, const char *key, int (*fn)(void *), int show) {
  if(!fn) { put(o, "\"%s\":null,", key); return; }
  struct icc x;
  memset(&x, 0, sizeof x);
  x.fn = fn;
  if(sysinfo_guarded_call(do_icc, &x)) { put(o, "\"%s\":\"crashed (caught)\",", key); return; }
  put(o, "\"%s\":{\"rc\":\"0x%08x\",", key, (unsigned)x.rc);
  hex(o, "first_bytes", x.buf, show);
  o->n--;                                   /* drop the comma */
  put(o, "},");
}

static void p_therm(out_t *o) {
  int t = -1;
  if(sceKernelGetCpuTemperature && sceKernelGetCpuTemperature(&t) == 0) put(o, "\"sceKernelGetCpuTemperature\":%d,", t);
  else put(o, "\"sceKernelGetCpuTemperature\":null,");
  put(o, "\"sceKernelGetSocSensorTemperature\":[");
  for(int ch = 0; ch < 8; ch++) {           /* never above 7: 0-64 crashed the kernel */
    int v = -1;
    int rc = sceKernelGetSocSensorTemperature ? sceKernelGetSocSensorTemperature(ch, &v) : -1;
    put(o, "%s%d", ch ? "," : "", rc == 0 ? v : -1);
  }
  put(o, "],");
  uint32_t pw[64];
  memset(pw, 0, sizeof pw);
  if(sceKernelGetSocPowerConsumption && sceKernelGetSocPowerConsumption(pw) == 0) {
    put(o, "\"sceKernelGetSocPowerConsumption\":{\"bytes\":112,");
    hex(o, "hex", pw, 112);
    put(o, "\"u32\":[");
    for(int i = 0; i < 28; i++) put(o, "%s%u", i ? "," : "", pw[i]);
    put(o, "]},");
  } else put(o, "\"sceKernelGetSocPowerConsumption\":null,");
  uint16_t duty = 0;
  uint8_t scratch[64] = {0};
  if(sceKernelGetCurrentFanDuty && sceKernelGetCurrentFanDuty(&duty, scratch) == 0) {
    put(o, "\"sceKernelGetCurrentFanDuty\":{\"duty_u16\":%u,", duty);
    hex(o, "scratch_first_16_bytes", scratch, 16);
    o->n--;
    put(o, "},");
  } else put(o, "\"sceKernelGetCurrentFanDuty\":null,");
}

static void p_cpu(out_t *o) {
  int cores[64];
  for(int i = 0; i < 64; i++) cores[i] = -1;
  if(sceKernelGetCpuCoreClock && sceKernelGetCpuCoreClock(cores) == 0) {
    put(o, "\"sceKernelGetCpuCoreClock\":[");
    for(int i = 0; i < 16; i++) put(o, "%s%d", i ? "," : "", cores[i]);
    put(o, "],");
  } else put(o, "\"sceKernelGetCpuCoreClock\":null,");
  uint32_t clk[64];
  memset(clk, 0, sizeof clk);
  if(sceKernelGetSocClock && sceKernelGetSocClock(clk) == 0) {
    put(o, "\"sceKernelGetSocClock\":[");
    for(int i = 0; i < 26; i++) put(o, "%s%u", i ? "," : "", clk[i]);
    put(o, "],");
  } else put(o, "\"sceKernelGetSocClock\":null,");
  o->n = jclamp(o->n + (long long)cpuload_raw_json(o->b + o->n, o->size - o->n), o->size);
  put(o, ",");
}

static void p_game(out_t *o) {
  uint8_t a[0x60];
  int rc = fps_dce_raw(a);
  if(rc) { put(o, "\"dce_answer\":{\"error\":%d},", rc); return; }
  put(o, "\"dce_answer\":{\"bytes\":96,");
  hex(o, "hex", a, 0x60);
  put(o, "\"u64\":[");
  for(int i = 0; i < 12; i++) { uint64_t v; memcpy(&v, a + i * 8, 8); put(o, "%s\"%llu\"", i ? "," : "", (unsigned long long)v); }
  put(o, "]},");
}

static void sfs(out_t *o, const char *path) {
  struct statfs s;
  if(statfs(path, &s) != 0) { put(o, "\"%s\":null,", path); return; }
  put(o, "\"%s\":{\"f_bsize\":%llu,\"f_blocks\":%llu,\"f_bfree\":%llu,\"f_bavail\":%lld,\"f_files\":%llu,\"f_ffree\":%lld,",
      path, (unsigned long long)s.f_bsize, (unsigned long long)s.f_blocks, (unsigned long long)s.f_bfree,
      (long long)s.f_bavail, (unsigned long long)s.f_files, (long long)s.f_ffree);
  str(o, "f_fstypename", s.f_fstypename);
  str(o, "f_mntfromname", s.f_mntfromname);
  str(o, "f_mntonname", s.f_mntonname);
  o->n--;
  put(o, "},");
}

static void p_mem(out_t *o) {
  put(o, "\"get_page_table_stats\":[");
  static const int VM[4] = {0, 0, 1, 1}, TY[4] = {1, 2, 1, 2};
  for(int i = 0; i < 4; i++) {
    int t = -1, f = -1, rc = get_page_table_stats ? get_page_table_stats(VM[i], TY[i], &t, &f) : -1;
    put(o, "%s{\"vm\":%d,\"type\":%d,\"rc\":%d,\"total_mib\":%d,\"free_mib\":%d}", i ? "," : "", VM[i], TY[i], rc, t, f);
  }
  put(o, "],\"statfs\":{");
  int before = o->n;
  sfs(o, "/user"); sfs(o, "/system_data"); sfs(o, "/system_ex"); sfs(o, "/mnt/ext0"); sfs(o, "/mnt/ext1");
  if(o->n > before) o->n--;
  put(o, "},");
}

static void p_hw(out_t *o) {
  char m[64] = "";
  if(sceKernelGetHwModelName) sceKernelGetHwModelName(m);
  m[sizeof m - 1] = '\0';
  str(o, "sceKernelGetHwModelName", m);
  char mach[64] = "";
  size_t l = sizeof mach;
  sysctlbyname("hw.machine", mach, &l, NULL, 0);
  mach[sizeof mach - 1] = '\0';
  str(o, "hw.machine", mach);
  int n = -1;
  l = sizeof n;
  sysctlbyname("hw.ncpu", &n, &l, NULL, 0);
  put(o, "\"hw.ncpu\":%d,", n);
}

static void p_ext(out_t *o) {
  struct timeval bt = {0, 0};
  size_t l = sizeof bt;
  sysctlbyname("kern.boottime", &bt, &l, NULL, 0);
  put(o, "\"kern.boottime\":{\"tv_sec\":%lld,\"tv_usec\":%ld},", (long long)bt.tv_sec, (long)bt.tv_usec);
  icc(o, "sceKernelIccGetPowerNumberOfBootShutdown", (int (*)(void *))sceKernelIccGetPowerNumberOfBootShutdown, 16);
  icc(o, "sceKernelIccGetThermalAlert", (int (*)(void *))sceKernelIccGetThermalAlert, 8);
  icc(o, "sceKernelIccGetBDPowerState", (int (*)(void *))sceKernelIccGetBDPowerState, 4);
}

static void p_time(out_t *o) {
  time_t now = time(NULL), local = 0;
  int dst[4] = {-1, -1, -1, -1}, rc = -1;     /* same buffer sizes as Heatline's own call in main.c */
  uint8_t tz[64] = {0};
  if(sceKernelConvertUtcToLocaltime) rc = sceKernelConvertUtcToLocaltime(now, &local, tz, dst);
  put(o, "\"time\":%lld,\"sceKernelConvertUtcToLocaltime\":{\"rc\":%d,\"local\":%lld,\"dst\":[%d,%d,%d,%d],", (long long)now, rc, (long long)local, dst[0], dst[1], dst[2], dst[3]);
  hex(o, "tz_first_16_bytes", tz, 16);
  o->n--;
  put(o, "},");
  uint8_t pk[48];
  long long at = 0;
  if(ntp_last_packet(pk, &at) == 0) {
    put(o, "\"last_sntp_answer\":{\"at\":%lld,\"bytes\":48,", at);
    hex(o, "hex", pk, 48);
    o->n--;
    put(o, "},");
  } else put(o, "\"last_sntp_answer\":\"none yet (no check or sync since start)\",");
}

static void p_self(out_t *o) {
  struct rusage r;
  memset(&r, 0, sizeof r);
  getrusage(RUSAGE_SELF, &r);
  put(o, "\"getrusage\":{\"ru_utime\":[%lld,%ld],\"ru_stime\":[%lld,%ld],\"ru_maxrss\":%ld,\"ru_ixrss\":%ld,\"ru_idrss\":%ld,"
      "\"ru_isrss\":%ld,\"ru_minflt\":%ld,\"ru_majflt\":%ld,\"ru_nswap\":%ld,\"ru_inblock\":%ld,\"ru_oublock\":%ld,"
      "\"ru_msgsnd\":%ld,\"ru_msgrcv\":%ld,\"ru_nsignals\":%ld,\"ru_nvcsw\":%ld,\"ru_nivcsw\":%ld},",
      (long long)r.ru_utime.tv_sec, (long)r.ru_utime.tv_usec, (long long)r.ru_stime.tv_sec, (long)r.ru_stime.tv_usec,
      r.ru_maxrss, r.ru_ixrss, r.ru_idrss, r.ru_isrss, r.ru_minflt, r.ru_majflt, r.ru_nswap, r.ru_inblock, r.ru_oublock,
      r.ru_msgsnd, r.ru_msgrcv, r.ru_nsignals, r.ru_nvcsw, r.ru_nivcsw);
}

static void netctl(out_t *o, const int *codes, int nc) {
  static uint8_t buf[2048];
  if(sceNetCtlGetState) { int st = -1; int rc = sceNetCtlGetState(&st); put(o, "\"sceNetCtlGetState\":{\"rc\":%d,\"state\":%d},", rc, st); }
  put(o, "\"sceNetCtlGetInfo\":{");
  for(int i = 0; i < nc; i++) {
    int rc = sysinfo_netctl_raw(codes[i], buf);
    put(o, "%s\"%d\":{\"rc\":\"0x%08x\",", i ? "," : "", codes[i], (unsigned)rc);
    hex(o, "first_32_bytes", buf, 32);
    o->n--;
    put(o, "}");
  }
  put(o, "},");
}

static void p_traffic(out_t *o) {
  struct ifaddrs *ifs = NULL;
  if(getifaddrs(&ifs) != 0) { put(o, "\"getifaddrs\":null,"); return; }
  put(o, "\"getifaddrs_if_data\":{");
  int first = 1;
  for(struct ifaddrs *p = ifs; p; p = p->ifa_next) {
    if(!p->ifa_addr || p->ifa_addr->sa_family != AF_LINK || !p->ifa_data) continue;
    const struct if_data *d = p->ifa_data;
    put(o, "%s\"%s\":{\"ifi_ibytes\":%llu,\"ifi_obytes\":%llu,\"ifi_ipackets\":%llu,\"ifi_opackets\":%llu,\"ifi_ierrors\":%llu,"
        "\"ifi_oerrors\":%llu,\"ifi_imcasts\":%llu,\"ifi_omcasts\":%llu,\"ifi_iqdrops\":%llu,\"ifi_mtu\":%u,\"ifi_baudrate\":%llu}",
        first ? "" : ",", p->ifa_name, (unsigned long long)d->ifi_ibytes, (unsigned long long)d->ifi_obytes,
        (unsigned long long)d->ifi_ipackets, (unsigned long long)d->ifi_opackets, (unsigned long long)d->ifi_ierrors,
        (unsigned long long)d->ifi_oerrors, (unsigned long long)d->ifi_imcasts, (unsigned long long)d->ifi_omcasts,
        (unsigned long long)d->ifi_iqdrops, (unsigned)d->ifi_mtu, (unsigned long long)d->ifi_baudrate);
    first = 0;
  }
  freeifaddrs(ifs);
  put(o, "},");
}

static void p_fan(out_t *o) {
  uint8_t b[10];
  long long at = 0;
  if(fan_last_write(b, &at) == 0) {
    put(o, "\"last_icc_fan_write\":{\"ioctl\":\"0xC01C8F07\",\"at\":%lld,\"bytes\":10,", at);
    hex(o, "hex", b, 10);
    o->n--;
    put(o, "},");
  } else put(o, "\"last_icc_fan_write\":\"none yet (fan threshold off or not applied since start)\",");
}

int raw_json(const char *panel, char *buf, int size) {
  out_t o = {buf, size, 0};
  put(&o, "{\"panel\":\"%s\",", panel);
  if(!strcmp(panel, "therm")) p_therm(&o);
  else if(!strcmp(panel, "cpu")) p_cpu(&o);
  else if(!strcmp(panel, "game")) p_game(&o);
  else if(!strcmp(panel, "mem")) p_mem(&o);
  else if(!strcmp(panel, "hw")) p_hw(&o);
  else if(!strcmp(panel, "ext")) p_ext(&o);
  else if(!strcmp(panel, "time")) p_time(&o);
  else if(!strcmp(panel, "self")) p_self(&o);
  else if(!strcmp(panel, "conn")) { static const int c[] = {1, 12, 14, 15, 16}; netctl(&o, c, 5); }
  else if(!strcmp(panel, "dns")) { static const int c[] = {2, 3, 17, 18}; netctl(&o, c, 4); }
  else if(!strcmp(panel, "traffic")) p_traffic(&o);
  else if(!strcmp(panel, "fanst")) { p_therm(&o); }
  else if(!strcmp(panel, "fan")) p_fan(&o);
  else if(!strcmp(panel, "display")) { o.n = jclamp(o.n + (long long)display_json(o.b + o.n, o.size - o.n), o.size); }
  else put(&o, "\"note\":\"no raw source for this panel\",");
  if(o.n > 0 && o.b[o.n - 1] == ',') o.n--;
  put(&o, "}");
  return o.n;
}
