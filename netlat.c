// SPDX-License-Identifier: GPL-3.0-only
/* netlat.c - network latency to the router and to the internet.
 *
 * Measured as the TCP handshake time: a non-blocking connect() and the time
 * until the other side answers (SYN -> SYN-ACK, or a refusal - a refusal is
 * also an answer, one round trip). That is one network round trip, like a
 * ping, without needing raw ICMP sockets. Every 5 s, one try per target,
 * 1 s timeout (= lost). Router: the console's default gateway (NetCtl),
 * ports 80 / 53 / 443 until one answers. Internet: 1.1.1.1:443, then
 * 8.8.8.8:443. Stats over the last 12 tries (1 minute). Always measured
 * while the switch (Settings / Latency panel) is on, whatever page is open. */
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>

#include "netlat.h"
#include "sysinfo.h"
#include "jbuf.h"

#define EVERY_S   5
#define TIMEOUT_MS 1000
#define HIST      12

struct target {
  char host[48];
  int port;
  double v[HIST];      /* ms, -1 = lost */
  int n, head;
};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct target g_gw, g_inet;
/* on / off switch (Settings): off = no connection tried at all. Saved as a file. */
/* off by default (a fresh install measures nothing); on = this file exists */
#define ON_FILE  "/data/heatline/lat_on"
#define OLD_FILE "/data/heatline/lat_off"   /* earlier builds: on by default, this file = off */
static int g_on = 0;
static volatile int g_now;            /* refresh button: measure at the next second */
void netlat_now(void) { g_now = 1; }

void netlat_set(int on) {
  if(!on) unlink(ON_FILE);
  else { mkdir("/data/heatline", 0755); FILE *fp = fopen(ON_FILE, "w"); if(fp) { fputs("1\n", fp); fclose(fp); } }
  pthread_mutex_lock(&g_lock);
  g_on = on ? 1 : 0;
  if(!on) { memset(&g_gw, 0, sizeof g_gw); memset(&g_inet, 0, sizeof g_inet); }
  pthread_mutex_unlock(&g_lock);
}

static double mono_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* ms until an answer, -1 = no answer within the timeout, -2 = could not try */
static double tcp_rtt(const char *ip, int port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  if(inet_pton(AF_INET, ip, &a.sin_addr) != 1) return -2;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0) return -2;
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  double t0 = mono_ms(), r = -1;
  int rc = connect(fd, (struct sockaddr *)&a, sizeof a);
  if(rc == 0) {
    r = mono_ms() - t0;
  } else if(errno == ECONNREFUSED) {
    r = mono_ms() - t0;
  } else if(errno == EINPROGRESS) {
    struct pollfd p = {fd, POLLOUT, 0};
    if(poll(&p, 1, TIMEOUT_MS) > 0) {
      double t1 = mono_ms();
      int err = 0;
      socklen_t el = sizeof err;
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
      if(err == 0 || err == ECONNREFUSED || err == ECONNRESET) r = t1 - t0;
    }
  } else {
    r = -2;
  }
  close(fd);
  return r;
}

static void push(struct target *t, double v) {
  t->v[t->head] = v;
  t->head = (t->head + 1) % HIST;
  if(t->n < HIST) t->n++;
}

static void *worker(void *arg) {
  (void)arg;
  static const int GW_PORTS[] = {80, 53, 443};
  static const char *INET[] = {"1.1.1.1", "8.8.8.8"};
  int gp = 0, ip = 0;
  char last_gw[48] = "";
  sleep(3);
  double next = 0;
  int idle = 1;
  for(;;) {
    pthread_mutex_lock(&g_lock);
    int active = g_on;                            /* switch on = always measured */
    if(active && idle) {                          /* switched back on: start a fresh minute */
      memset(&g_gw, 0, sizeof g_gw);
      memset(&g_inet, 0, sizeof g_inet);
    }
    pthread_mutex_unlock(&g_lock);
    if(!active) { idle = 1; last_gw[0] = '\0'; sleep(1); continue; }   /* nothing measured */
    if(idle) { idle = 0; next = 0; }
    if(mono_ms() < next && !g_now) { sleep(1); continue; }
    g_now = 0;
    next = mono_ms() + EVERY_S * 1000.0;
    char gw[48] = "";
    sysinfo_gateway(gw, sizeof gw);
    if(strcmp(gw, last_gw)) {                      /* new router: start over */
      snprintf(last_gw, sizeof last_gw, "%s", gw);
      pthread_mutex_lock(&g_lock);
      memset(&g_gw, 0, sizeof g_gw);
      pthread_mutex_unlock(&g_lock);
      gp = 0;
    }
    if(gw[0]) {
      double r = -2;
      for(int k = 0; k < 3; k++) {                 /* the port that answered last first */
        int p = (gp + k) % 3;
        r = tcp_rtt(gw, GW_PORTS[p]);
        if(r >= 0) { gp = p; break; }
        if(r == -2) break;
      }
      pthread_mutex_lock(&g_lock);
      snprintf(g_gw.host, sizeof g_gw.host, "%s", gw);
      g_gw.port = GW_PORTS[gp];
      if(r != -2 && g_on) push(&g_gw, r);
      pthread_mutex_unlock(&g_lock);
    }
    double r = tcp_rtt(INET[ip], 443);
    if(r < 0) {                                    /* lost: try the other server once */
      double r2 = tcp_rtt(INET[ip ^ 1], 443);
      if(r2 >= 0) { ip ^= 1; r = r2; }
    }
    pthread_mutex_lock(&g_lock);
    snprintf(g_inet.host, sizeof g_inet.host, "%s", INET[ip]);
    g_inet.port = 443;
    if(r != -2 && g_on) push(&g_inet, r);
    pthread_mutex_unlock(&g_lock);
    sleep(1);
  }
  return NULL;
}

void netlat_start(void) {
  pthread_t th;
  unlink(OLD_FILE);
  g_on = access(ON_FILE, F_OK) == 0;
  if(pthread_create(&th, NULL, worker, NULL) == 0) pthread_detach(th);
}

static int tjson(char *b, int size, const char *key, const struct target *t) {
  double last = -1, mn = -1, mx = -1, sum = 0, jit = 0, prev = -1;
  int ok = 0, lost = 0, nj = 0;
  for(int k = 0; k < t->n; k++) {                  /* oldest first */
    double v = t->v[(t->head + HIST - t->n + k) % HIST];
    if(k == t->n - 1) last = v;
    if(v < 0) { lost++; prev = -1; continue; }
    ok++;
    sum += v;
    if(mn < 0 || v < mn) mn = v;
    if(v > mx) mx = v;
    if(prev >= 0) { jit += v > prev ? v - prev : prev - v; nj++; }
    prev = v;
  }
  return jcat(b, size, 0, "\"%s\":{\"host\":\"%s\",\"port\":%d,\"last\":%.2f,\"min\":%.2f,\"avg\":%.2f,\"max\":%.2f,"
                  "\"jitter\":%.2f,\"lost\":%d,\"tries\":%d}",
                  key, t->host, t->port, last, mn, ok ? sum / ok : -1.0, mx, nj ? jit / nj : -1.0, lost, t->n);
}

/* Last value and jitter (mean change between tries) of one target, ms; -1 = none. */
static void tstat(const struct target *t, double *last, double *jit) {
  double prev = -1, sum = 0;
  int nj = 0;
  *last = -1; *jit = -1;
  for(int k = 0; k < t->n; k++) {                  /* oldest first */
    double v = t->v[(t->head + HIST - t->n + k) % HIST];
    if(k == t->n - 1) *last = v;
    if(v < 0) { prev = -1; continue; }
    if(prev >= 0) { sum += v > prev ? v - prev : prev - v; nj++; }
    prev = v;
  }
  if(nj) *jit = sum / nj;
}

int netlat_overlay(double v[4]) {
  pthread_mutex_lock(&g_lock);
  int on = g_on;
  if(on) { tstat(&g_gw, &v[0], &v[1]); tstat(&g_inet, &v[2], &v[3]); }
  pthread_mutex_unlock(&g_lock);
  return on;
}

int netlat_json(char *buf, int size) {
  int n = 0;
  pthread_mutex_lock(&g_lock);
  n = jcat(buf, size, n, "\"lat\":{\"on\":%s,\"method\":\"tcp\",\"every_s\":%d,\"timeout_ms\":%d,", g_on ? "true" : "false", EVERY_S, TIMEOUT_MS);
  n = jclamp(n + (long long)tjson(buf + n, size - n, "gw", &g_gw), size);
  n = jcat(buf, size, n, ",");
  n = jclamp(n + (long long)tjson(buf + n, size - n, "inet", &g_inet), size);
  n = jcat(buf, size, n, "},");
  pthread_mutex_unlock(&g_lock);
  return n < size ? n : size - 1;
}
