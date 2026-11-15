// SPDX-License-Identifier: GPL-3.0-only
/* selfstat.c - what Heatline itself costs the PS5 (System page).
 *
 * CPU: getrusage(RUSAGE_SELF) = user + system CPU time of all Heatline
 * threads, sampled once per second (from the CPU usage thread); shown as %
 * of ONE logical CPU over the last 10 s and since start (the PS5 has 16).
 * Memory: ru_maxrss = peak resident memory of the process (KB on FreeBSD).
 * Network: requests and bytes sent by the web server, counted in main.c.
 */
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "selfstat.h"
#include "jbuf.h"

#define RING 11                      /* 10 s window */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_t[RING], g_cpu[RING], g_req[RING], g_bytes[RING];
static int g_n, g_head;
static int64_t g_t0, g_cpu0;
static long g_maxrss_kb = -1;
static volatile uint64_t g_req_total, g_bytes_total;

static int64_t mono_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

void selfstat_count(uint64_t bytes) {
  __atomic_add_fetch(&g_req_total, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_bytes_total, bytes, __ATOMIC_RELAXED);
}

void selfstat_tick(void) {
  struct rusage ru;
  if(getrusage(RUSAGE_SELF, &ru) != 0) return;
  int64_t cpu = (int64_t)ru.ru_utime.tv_sec * 1000000 + ru.ru_utime.tv_usec +
                (int64_t)ru.ru_stime.tv_sec * 1000000 + ru.ru_stime.tv_usec;
  int64_t now = mono_us();
  pthread_mutex_lock(&g_lock);
  if(!g_t0) { g_t0 = now; g_cpu0 = cpu; }
  g_t[g_head] = now;
  g_cpu[g_head] = cpu;
  g_req[g_head] = (int64_t)__atomic_load_n(&g_req_total, __ATOMIC_RELAXED);
  g_bytes[g_head] = (int64_t)__atomic_load_n(&g_bytes_total, __ATOMIC_RELAXED);
  g_head = (g_head + 1) % RING;
  if(g_n < RING) g_n++;
  g_maxrss_kb = ru.ru_maxrss;
  pthread_mutex_unlock(&g_lock);
}

int selfstat_json(char *buf, int size) {
  pthread_mutex_lock(&g_lock);
  int n;
  if(g_n < 2) {
    n = jcat(buf, size, 0, "\"self\":{\"ok\":false},");
  } else {
    int newest = (g_head + RING - 1) % RING, oldest = (g_head + RING - g_n) % RING;
    double span = (double)(g_t[newest] - g_t[oldest]);
    double cpu10 = span > 0 ? (double)(g_cpu[newest] - g_cpu[oldest]) / span * 100.0 : -1;
    double all = g_t[newest] > g_t0 ? (double)(g_cpu[newest] - g_cpu0) / (double)(g_t[newest] - g_t0) * 100.0 : -1;
    double rps = span > 0 ? (double)(g_req[newest] - g_req[oldest]) * 1e6 / span : -1;
    double bps = span > 0 ? (double)(g_bytes[newest] - g_bytes[oldest]) * 1e6 / span : -1;
    n = jcat(buf, size, 0,
                 "\"self\":{\"ok\":true,\"cpu10_pct\":%.3f,\"cpu_all_pct\":%.3f,\"cpu_s\":%.2f,"
                 "\"maxrss_kb\":%ld,\"req_s\":%.2f,\"bytes_s\":%.0f,\"req_total\":%llu,\"bytes_total\":%llu,\"up_s\":%.0f},",
                 cpu10, all, (double)g_cpu[newest] / 1e6, g_maxrss_kb, rps, bps,
                 (unsigned long long)g_req_total, (unsigned long long)g_bytes_total,
                 (double)(g_t[newest] - g_t0) / 1e6);
  }
  pthread_mutex_unlock(&g_lock);
  return n < size ? n : size - 1;
}
