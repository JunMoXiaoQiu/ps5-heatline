// SPDX-License-Identifier: GPL-3.0-only
/* fan.c - "Fan control" page: the features of ps5-fan-control (owendswang,
 * github.com/owendswang/ps5-fan-control, GPL-3.0), written again for Heatline.
 *
 * What it does (same as ps5-fan-control v0.3):
 *  - Sets the temperature threshold the PS5's own fan driver uses, through
 *    /dev/icc_fan, ioctl 0xC01C8F07 with a 10-byte buffer whose byte 5 is the
 *    threshold in degrees C. It does not set a fan speed or a fan curve: the
 *    system fan driver still decides the speed, starting from this threshold.
 *  - Threshold 30..90 C (86..194 F); an optional separate threshold for the
 *    home screen (no game = no Big App running); re-applied every "interval"
 *    seconds (1..3600, default 7); optional PS5 status notification each
 *    interval (GPU / CPU / target / fan, as ps5-fan-control shows it) -
 *    in Heatline only once, right after the settings are saved & applied.
 *  - Settings use ps5-fan-control's file format and keys (enable,
 *    use_fahrenheit, target_temperature, xmb_target_temperature, show_status,
 *    interval), stored in /data/heatline/fan_control.ini.
 * Differences, for safety:
 *  - Nothing is written to /dev/icc_fan until fan control is turned on in
 *    the page (ps5-fan-control applies 70 C by default when it has no file).
 *  - If a ps5-fan-control.elf process is running, Heatline does not write
 *    (the two would fight) and the page says so.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fan.h"
#include "jbuf.h"

void notify(const char *msg);   /* main.c */
int sceKernelGetCpuTemperature(int *) __attribute__((weak));
int sceKernelGetSocSensorTemperature(int, int *) __attribute__((weak));
int sceKernelGetCurrentFanDuty(uint16_t *duty, void *scratch) __attribute__((weak));
int sceSystemServiceGetAppIdOfRunningBigApp(void) __attribute__((weak));

#define CFG_DIR   "/data/heatline"
#define CFG_FILE  CFG_DIR "/fan_control.ini"
#define FAN_DEV   "/dev/icc_fan"
#define FAN_IOCTL_SET_THRESHOLD 0xC01C8F07

typedef struct {
  int enable, use_f, target, xmb, interval, status;   /* target / xmb in the chosen unit; xmb 0 = off */
} cfg_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static cfg_t g_cfg = {0, 0, 70, 0, 7, 0};
static int g_saved;                 /* a settings file exists */
static int g_conflict;              /* ps5-fan-control.elf is running */
static int g_last_ok = -1, g_last_c = 0, g_last_menu = 0, g_apply_now, g_notify_once;
static long long g_last_at;
static char g_last_msg[96] = "";

static int lo(int f) { return f ? 86 : 30; }
static int hi(int f) { return f ? 194 : 90; }
static int to_c(int v, int f) { return f ? (v - 32) * 5 / 9 : v; }

static int valid(const cfg_t *c) {
  if(c->enable != 0 && c->enable != 1) return 0;
  if(c->use_f != 0 && c->use_f != 1) return 0;
  if(c->status != 0 && c->status != 1) return 0;
  if(c->target < lo(c->use_f) || c->target > hi(c->use_f)) return 0;
  if(c->xmb != 0 && (c->xmb < lo(c->use_f) || c->xmb > hi(c->use_f))) return 0;
  if(c->interval < 1 || c->interval > 3600) return 0;
  return 1;
}

static void load(void) {
  FILE *f = fopen(CFG_FILE, "r");
  if(!f) return;
  cfg_t c = {1, 0, 70, 0, 7, 0};          /* ps5-fan-control defaults for missing keys */
  char line[256];
  while(fgets(line, sizeof line, f)) {
    char *p = line;
    while(*p == ' ' || *p == '\t') p++;
    if(*p == '#' || *p == ';' || *p == '[') continue;
    char *eq = strchr(p, '=');
    if(!eq) continue;
    *eq = 0;
    char key[40];
    int v;
    if(sscanf(p, "%39s", key) != 1 || sscanf(eq + 1, "%d", &v) != 1) continue;
    if(!strcmp(key, "enable")) c.enable = v;
    else if(!strcmp(key, "use_fahrenheit")) c.use_f = v;
    else if(!strcmp(key, "target_temperature")) c.target = v;
    else if(!strcmp(key, "xmb_target_temperature")) c.xmb = v;
    else if(!strcmp(key, "interval")) c.interval = v;
    else if(!strcmp(key, "show_status")) c.status = v;
  }
  fclose(f);
  if(valid(&c)) { g_cfg = c; g_saved = 1; }
  else { g_saved = 1; snprintf(g_last_msg, sizeof g_last_msg, "Saved settings were invalid: fan control left off"); g_cfg.enable = 0; }
}

static int save(const cfg_t *c) {
  mkdir(CFG_DIR, 0755);
  FILE *f = fopen(CFG_FILE ".tmp", "w");
  if(!f) return -1;
  fprintf(f, "# Heatline fan control (same keys as ps5-fan-control)\n"
             "enable=%d\nuse_fahrenheit=%d\ntarget_temperature=%d\nxmb_target_temperature=%d\nshow_status=%d\ninterval=%d\n",
          c->enable, c->use_f, c->target, c->xmb, c->status, c->interval);
  if(fclose(f)) return -1;
  return rename(CFG_FILE ".tmp", CFG_FILE);
}

/* other fan-control payload running? (process list, same raw offsets as main.c) */
static int other_daemon(void) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t len = 0;
  if(sysctl(mib, 4, NULL, &len, NULL, 0)) return 0;
  len += len / 2;
  char *buf = malloc(len);
  if(!buf) return 0;
  int found = 0;
  if(!sysctl(mib, 4, buf, &len, NULL, 0)) {
    for(char *p = buf; p < buf + len;) {
      int ss = *(int *)p;
      if(ss <= 0) break;
      char name[32];
      memcpy(name, p + 447, sizeof name - 1);
      name[sizeof name - 1] = 0;
      if(!strcmp(name, "ps5-fan-control.elf")) found = 1;
      p += ss;
    }
  }
  free(buf);
  return found;
}

static int in_menu(void) {
  return sceSystemServiceGetAppIdOfRunningBigApp && sceSystemServiceGetAppIdOfRunningBigApp() <= 0;
}

static char g_last_buf[10];
static time_t g_last_buf_at;
int fan_last_write(unsigned char out[10], long long *at) {
  memcpy(out, g_last_buf, 10);
  *at = (long long)g_last_buf_at;
  return g_last_buf_at ? 0 : -1;
}

static int write_threshold(int c, char *why, size_t wn) {
  int fd = open(FAN_DEV, O_RDONLY, 0);
  if(fd < 0) { snprintf(why, wn, "could not open " FAN_DEV " (errno %d)", errno); return -1; }
  char data[10] = {0};
  data[5] = (char)c;
  int r = ioctl(fd, FAN_IOCTL_SET_THRESHOLD, data);
  memcpy(g_last_buf, data, sizeof g_last_buf);       /* for the raw view */
  g_last_buf_at = time(NULL);
  int e = errno;
  close(fd);
  if(r < 0) { snprintf(why, wn, "the fan driver refused the threshold (errno %d)", e); return -1; }
  return 0;
}

static void status_notification(const cfg_t *c, int active_c) {
  int cpu = -1, soc = -1;
  uint16_t duty = 0;
  uint8_t scratch[64] = {0};
  int cpu_ok = sceKernelGetCpuTemperature && sceKernelGetCpuTemperature(&cpu) == 0 && cpu >= 0 && cpu <= 130;
  int soc_ok = sceKernelGetSocSensorTemperature && sceKernelGetSocSensorTemperature(0, &soc) == 0 && soc >= 0 && soc <= 130;
  int fan_ok = sceKernelGetCurrentFanDuty && sceKernelGetCurrentFanDuty(&duty, scratch) == 0 && duty <= 1024;
  const char *u = c->use_f ? "F" : "C";
  int k = c->use_f;
#define T(x) (k ? (x) * 9 / 5 + 32 : (x))
  char m[160], a[16], b[16], d[16];
  if(soc_ok) snprintf(a, sizeof a, "%d%s", T(soc), u); else snprintf(a, sizeof a, "--%s", u);
  if(cpu_ok) snprintf(b, sizeof b, "%d%s", T(cpu), u); else snprintf(b, sizeof b, "--%s", u);
  if(fan_ok) snprintf(d, sizeof d, "%.1f%%", duty * 100.0 / 1024); else snprintf(d, sizeof d, "--%%");
  snprintf(m, sizeof m, "Heatline fan threshold applied\nGPU %s\nCPU %s\nTARGET %d%s\nFAN %s", a, b, T(active_c), u, d);
#undef T
  notify(m);
}

static void *worker(void *arg) {
  (void)arg;
  pthread_mutex_lock(&g_lock);
  for(;;) {
    cfg_t c = g_cfg;
    pthread_mutex_unlock(&g_lock);
    int conflict = other_daemon();
    int menu = in_menu();
    int active = (c.xmb > 0 && menu) ? to_c(c.xmb, c.use_f) : to_c(c.target, c.use_f);
    char why[96] = "";
    int did = 0, ok = 0;
    if(c.enable && !conflict) {
      did = 1;
      ok = write_threshold(active, why, sizeof why) == 0;
    }
    pthread_mutex_lock(&g_lock);
    int say = g_notify_once;
    g_notify_once = 0;
    pthread_mutex_unlock(&g_lock);
    /* status notification only right after the settings were saved & applied
     * (ps5-fan-control shows it every interval; that was too noisy) */
    if(c.status && say) status_notification(&c, active);
    pthread_mutex_lock(&g_lock);
    g_conflict = conflict;
    if(did) {
      if(ok != g_last_ok && !ok) notify("Heatline fan threshold: could not set the threshold");
      g_last_ok = ok;
      g_last_c = active;
      g_last_menu = menu;
      g_last_at = (long long)time(NULL);
      snprintf(g_last_msg, sizeof g_last_msg, "%s", ok ? "" : why);
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += c.interval;
    while(!g_apply_now) {
      if(pthread_cond_timedwait(&g_wake, &g_lock, &until) == ETIMEDOUT) break;
    }
    g_apply_now = 0;
  }
  return NULL;
}

void fan_start(void) {
  load();
  pthread_t th;
  if(pthread_create(&th, NULL, worker, NULL) == 0) pthread_detach(th);
}

/* query: enable=..&f=..&target=..&xmb=..&interval=..&status=.. ; returns 0 or an error text */
const char *fan_set(const char *q) {
  cfg_t c;
  pthread_mutex_lock(&g_lock);
  c = g_cfg;
  pthread_mutex_unlock(&g_lock);
  struct { const char *k; int *v; } keys[] = {{"enable=", &c.enable}, {"f=", &c.use_f}, {"target=", &c.target},
                                              {"xmb=", &c.xmb}, {"interval=", &c.interval}, {"status=", &c.status}};
  for(size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
    const char *p = q;
    size_t kl = strlen(keys[i].k);
    while((p = strstr(p, keys[i].k))) {
      if(p == q || p[-1] == '&' || p[-1] == '?') { *keys[i].v = atoi(p + kl); break; }
      p += kl;
    }
  }
  if(!valid(&c)) return "invalid value (threshold 30-90 C / 86-194 F, interval 1-3600 s)";
  if(save(&c)) return "could not save the settings on the console";
  pthread_mutex_lock(&g_lock);
  g_cfg = c;
  g_saved = 1;
  g_apply_now = 1;
  g_notify_once = 1;
  pthread_cond_signal(&g_wake);
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

void fan_apply_now(void) {
  pthread_mutex_lock(&g_lock);
  g_apply_now = 1;
  pthread_cond_signal(&g_wake);
  pthread_mutex_unlock(&g_lock);
}

int fan_json(char *buf, int size) {
  pthread_mutex_lock(&g_lock);
  cfg_t c = g_cfg;
  int menu = in_menu();
  int active = (c.xmb > 0 && menu) ? to_c(c.xmb, c.use_f) : to_c(c.target, c.use_f);
  int n = jcat(buf, size, 0,
                   "\"fanctl\":{\"enable\":%d,\"f\":%d,\"target\":%d,\"xmb\":%d,\"interval\":%d,\"status\":%d,"
                   "\"saved\":%s,\"conflict\":%s,\"menu\":%s,\"active_c\":%d,"
                   "\"last\":{\"ok\":%d,\"c\":%d,\"menu\":%s,\"at\":%lld,\"msg\":\"%s\"}},",
                   c.enable, c.use_f, c.target, c.xmb, c.interval, c.status,
                   g_saved ? "true" : "false", g_conflict ? "true" : "false", menu ? "true" : "false", active,
                   g_last_ok, g_last_c, g_last_menu ? "true" : "false", g_last_at, g_last_msg);
  pthread_mutex_unlock(&g_lock);
  return n < size ? n : size - 1;
}
