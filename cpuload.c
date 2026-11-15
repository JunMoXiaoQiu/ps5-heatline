// SPDX-License-Identifier: GPL-3.0-only
/* CPU usage per logical CPU.
 *
 * Method (ps5-hwinfo, ps5-exporter, PS5 Monitor, Common FPS HUD):
 * sceKernelGetCpuUsage(entries, &count) fills the list of every thread on the
 * console with its user and system CPU time. Each logical CPU has a kernel
 * idle thread, "SceIdleCpu0".."SceIdleCpu15" (logical CPU 13 is named
 * "SceIdleCpuRv"). Usage of a CPU = 1 - (idle time gained / wall time).
 *
 * Notes from those projects (FW 13.60):
 *  - the kernel writes the WHOLE thread list and ignores a too-small count
 *    (64 offered, 1438 written), so the buffer is large;
 *  - sceKernelGetCpuUsageAll answers 0x80020016 on 13.60, so it is not used.
 *
 * Sampled once per second on its own thread, independent of how often the
 * page refreshes; the HTTP handler only copies the last result.
 */
#include "cpuload.h"
#include "selfstat.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ps5/klog.h>
#include "jbuf.h"

int sceKernelGetCpuUsage(void *entries, int32_t *count)  __attribute__((weak));
int sceKernelGetThreadName(uint32_t tid, char *name)     __attribute__((weak));

typedef struct {
  uint32_t pid, tid;
  int64_t  user_sec, user_usec;
  int64_t  sys_sec, sys_usec;
} usage_entry_t;
_Static_assert(sizeof(usage_entry_t) == 40, "usage entry is 40 bytes");

#define MAX_ENTRIES 8192          /* ~1400 threads seen; 320 KB, allocated once */

static usage_entry_t *g_buf;
static uint32_t g_idle_tid[CPULOAD_MAX];
static double   g_prev_idle[CPULOAD_MAX];
static bool     g_have_prev;
static struct timespec g_prev_wall;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_pct[CPULOAD_MAX];     /* -1 = unknown */
static int g_avg = -1;
static int g_ncpu = 0;
static bool g_ok = false;

static double
secs(const usage_entry_t *e) {
  return (double)(e->user_sec + e->sys_sec) + (double)(e->user_usec + e->sys_usec) / 1e6;
}

/* Find the idle thread of each logical CPU. Returns how many were found. */
static int
resolve_idle(int n) {
  char name[128];
  int found = 0;
  memset(g_idle_tid, 0, sizeof g_idle_tid);
  for(int i = 0; i < n && found < CPULOAD_MAX; i++) {
    int lane = -1;
    memset(name, 0, sizeof name);
    if(sceKernelGetThreadName(g_buf[i].tid, name)) {
      continue;
    }
    name[sizeof name - 1] = '\0';
    if(!strcmp(name, "SceIdleCpuRv")) {
      lane = 13;
    } else if(sscanf(name, "SceIdleCpu%d", &lane) != 1) {
      continue;
    }
    if(lane >= 0 && lane < CPULOAD_MAX && !g_idle_tid[lane]) {
      g_idle_tid[lane] = g_buf[i].tid;
      found++;
    }
  }
  return found;
}

/* Raw data view (/raw?p=cpu): its own call into its own buffer, only when
 * asked; returns the 40-byte entries of the 16 idle threads (hex) and the
 * number of threads listed. The once-per-second sampler is not touched. */
int
cpuload_raw_json(char *out, int size) {
  if(!sceKernelGetCpuUsage) return jcat(out, size, 0, "\"thread_list\":null");
  usage_entry_t *b = malloc(sizeof(usage_entry_t) * MAX_ENTRIES);
  if(!b) return jcat(out, size, 0, "\"thread_list\":null");
  int32_t n = MAX_ENTRIES;
  memset(b, 0, sizeof(usage_entry_t) * MAX_ENTRIES);
  int rc = sceKernelGetCpuUsage(b, &n);
  int w = jcat(out, size, 0, "\"thread_list\":{\"rc\":\"0x%08x\",\"threads\":%d,\"entry_bytes\":40,\"idle\":[", (unsigned)rc, rc ? 0 : n);
  for(int l = 0; l < CPULOAD_MAX && !rc && n > 0 && n <= MAX_ENTRIES; l++) {
    uint32_t tid = g_idle_tid[l];
    const usage_entry_t *e = NULL;
    for(int i = 0; tid && i < n; i++) if(b[i].tid == tid) { e = &b[i]; break; }
    w = jcat(out, size, w, "%s{\"cpu\":%d,\"tid\":%u,\"hex\":\"", l ? "," : "", l + 1, tid);
    for(int k = 0; e && k < (int)sizeof *e && w < size - 4; k++) w = jcat(out, size, w, "%02x", ((const uint8_t *)e)[k]);
    w = jcat(out, size, w, "\"}");
  }
  w = jcat(out, size, w, "]}");
  free(b);
  return w < size ? w : size - 1;
}

static void
sample(void) {
  int32_t n = MAX_ENTRIES;
  struct timespec now;

  memset(g_buf, 0, sizeof(usage_entry_t) * MAX_ENTRIES);
  if(sceKernelGetCpuUsage(g_buf, &n) || n <= 0 || n > MAX_ENTRIES) {
    return;
  }
  clock_gettime(CLOCK_MONOTONIC, &now);

  /* idle time per lane; re-resolve the idle threads if one went missing */
  double idle[CPULOAD_MAX];
  bool have[CPULOAD_MAX];
  int nfound = 0;
  for(int pass = 0; pass < 2; pass++) {
    nfound = 0;
    for(int l = 0; l < CPULOAD_MAX; l++) {
      have[l] = false;
      if(!g_idle_tid[l]) continue;
      for(int i = 0; i < n; i++) {
        if(g_buf[i].tid == g_idle_tid[l]) {
          idle[l] = secs(&g_buf[i]);
          have[l] = true;
          nfound++;
          break;
        }
      }
    }
    if(nfound && pass == 0) {
      bool missing = false;
      for(int l = 0; l < CPULOAD_MAX; l++) missing |= g_idle_tid[l] && !have[l];
      if(!missing) break;
    }
    if(pass == 0) {
      if(resolve_idle(n) == 0) {
        return;
      }
      g_have_prev = false;            /* new thread ids: start a fresh delta */
    }
  }

  if(g_have_prev) {
    double wall = (double)(now.tv_sec - g_prev_wall.tv_sec) +
                  (double)(now.tv_nsec - g_prev_wall.tv_nsec) / 1e9;
    if(wall > 0.05) {
      int pct[CPULOAD_MAX], sum = 0, cnt = 0, ncpu = 0;
      for(int l = 0; l < CPULOAD_MAX; l++) {
        pct[l] = -1;
        if(!have[l]) continue;
        ncpu = l + 1;
        double r = (idle[l] - g_prev_idle[l]) / wall;
        if(r < 0) r = 0;
        if(r > 1) r = 1;
        pct[l] = (int)((1.0 - r) * 100.0 + 0.5);
        sum += pct[l];
        cnt++;
      }
      pthread_mutex_lock(&g_lock);
      memcpy(g_pct, pct, sizeof pct);
      g_avg = cnt ? (sum + cnt / 2) / cnt : -1;
      g_ncpu = ncpu;
      g_ok = cnt > 0;
      pthread_mutex_unlock(&g_lock);
    }
  }
  for(int l = 0; l < CPULOAD_MAX; l++) {
    g_prev_idle[l] = have[l] ? idle[l] : 0;
  }
  g_prev_wall = now;
  g_have_prev = true;
}

static void *
cpuload_thread(void *arg) {
  (void)arg;
  for(;;) {
    sample();
    selfstat_tick();             /* Heatline's own CPU / memory (selfstat.c) */
    usleep(1000 * 1000);
  }
  return NULL;
}

void
cpuload_start(void) {
  pthread_t th;
  for(int l = 0; l < CPULOAD_MAX; l++) g_pct[l] = -1;
  if(!sceKernelGetCpuUsage || !sceKernelGetThreadName) {
    klog_puts("[heatline] cpu usage: API not available");
    return;
  }
  if(!(g_buf = malloc(sizeof(usage_entry_t) * MAX_ENTRIES))) {
    return;
  }
  if(pthread_create(&th, NULL, cpuload_thread, NULL)) {
    free(g_buf);
    g_buf = NULL;
    return;
  }
  pthread_detach(th);
}

int
cpuload_json(char *buf, size_t size) {
  int n = 0;
  pthread_mutex_lock(&g_lock);
  if(!g_ok) {
    n = jcat(buf, size, 0, "\"cpu_usage\":null,");
  } else {
    n = jcat(buf, size, n, "\"cpu_usage\":{\"avg\":%d,\"threads\":[", g_avg);
    for(int l = 0; l < g_ncpu; l++) {
      if(g_pct[l] < 0) n = jcat(buf, size, n, "%snull", l ? "," : "");
      else             n = jcat(buf, size, n, "%s%d", l ? "," : "", g_pct[l]);
    }
    n = jcat(buf, size, n, "]},");
  }
  pthread_mutex_unlock(&g_lock);
  return n;
}

void
cpuload_snapshot(int out[CPULOAD_MAX]) {
  pthread_mutex_lock(&g_lock);
  for(int l = 0; l < CPULOAD_MAX; l++) out[l] = g_ok ? g_pct[l] : -1;
  pthread_mutex_unlock(&g_lock);
}
