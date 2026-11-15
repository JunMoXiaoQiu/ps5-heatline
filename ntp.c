// SPDX-License-Identifier: GPL-3.0-only
/* ntp.c - console date & time sync over the internet (System page).
 *
 * Same job and checks as "PS5 Date/Time Sync" by Deckerr97
 * (github.com/kerrdec97/ps5-date-time-sync), written again here for Heatline:
 *  - SNTP v4 client request to, in order: Cloudflare's numeric addresses
 *    162.159.200.1 and 162.159.200.123 (works without DNS), then
 *    time.cloudflare.com, pool.ntp.org, time.google.com; 2 attempts each,
 *    3 s timeout.
 *  - The reply is accepted only if it comes from the server asked, echoes our
 *    transmit timestamp, is version 4 / server mode, stratum 1..15, leap
 *    indicator not "unsynchronised", and lands between 2000 and 2100.
 *  - Half the measured round trip is added; the clock is set with
 *    settimeofday and read back (must agree within 2 s). The console's time
 *    zone is not touched.
 * Additions for Heatline: "check" measures the offset without changing the
 * clock; an optional automatic sync when Heatline starts (stored as the file
 * /data/heatline/ntp_autosync). Everything runs on a background thread.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ntp.h"
#include "jbuf.h"

void notify(const char *msg);   /* main.c */

#define AUTO_DIR  "/data/heatline"
#define AUTO_FILE AUTO_DIR "/ntp_autosync"
#define NTP_EPOCH 2208988800ULL  /* 1900 -> 1970 */

static const char *SERVERS[] = {"162.159.200.1", "162.159.200.123", "time.cloudflare.com",
                                "pool.ntp.org", "time.google.com"};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_running;
static struct {
  char action[8];      /* "check" | "sync" */
  char state[8];       /* idle | running | ok | fail */
  char server[48];
  char msg[160];
  int stratum, rtt_ms, auto_run;
  long long offset_ms; /* internet time - console time, before any change */
  long long at;        /* UTC seconds of the result */
} g = {"", "idle", "", "", 0, 0, 0, 0, 0};

static int64_t mono_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int resolve(const char *host, struct sockaddr_in *a) {
  memset(a, 0, sizeof *a);
  a->sin_family = AF_INET;
  a->sin_port = htons(123);
  if(inet_pton(AF_INET, host, &a->sin_addr) == 1) return 0;
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  if(getaddrinfo(host, NULL, &hints, &res) || !res) return -1;
  int ok = -1;
  for(struct addrinfo *r = res; r; r = r->ai_next)
    if(r->ai_addr && r->ai_addrlen >= sizeof(struct sockaddr_in)) {
      a->sin_addr = ((struct sockaddr_in *)r->ai_addr)->sin_addr;
      ok = 0;
      break;
    }
  freeaddrinfo(res);
  return ok;
}

static uint8_t g_last_pkt[48];
static time_t g_last_pkt_at;
int ntp_last_packet(unsigned char out[48], long long *at) {
  memcpy(out, g_last_pkt, 48);
  *at = (long long)g_last_pkt_at;
  return g_last_pkt_at ? 0 : -1;
}

/* one SNTP exchange; out = internet time (RTT/2 corrected), *rtt_us, *stratum */
static int query(const struct sockaddr_in *srv, struct timeval *out, int64_t *rtt_us, int *stratum, char *why, size_t wn) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if(fd < 0) { snprintf(why, wn, "socket failed (errno %d)", errno); return -1; }
  struct timeval to = {3, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
  uint8_t req[48], rsp[512];
  memset(req, 0, sizeof req);
  req[0] = (4 << 3) | 3;                         /* LI 0, version 4, client */
  struct timeval now;
  gettimeofday(&now, NULL);
  uint32_t ts = (uint32_t)((uint64_t)now.tv_sec + NTP_EPOCH);
  uint32_t tf = (uint32_t)(((uint64_t)now.tv_usec << 32) / 1000000);
  if(!ts && !tf) tf = 1;
  be32(req + 40, ts);
  be32(req + 44, tf);
  int64_t t0 = mono_us();
  if(sendto(fd, req, sizeof req, 0, (const struct sockaddr *)srv, sizeof *srv) != (ssize_t)sizeof req) {
    snprintf(why, wn, "send failed (errno %d)", errno);
    close(fd);
    return -1;
  }
  struct sockaddr_in from;
  socklen_t fl = sizeof from;
  ssize_t n = recvfrom(fd, rsp, sizeof rsp, 0, (struct sockaddr *)&from, &fl);
  int e = errno;
  int64_t t1 = mono_us();
  close(fd);
  if(n < 0) { snprintf(why, wn, e == EAGAIN || e == EWOULDBLOCK ? "no answer (UDP port 123 blocked?)" : "receive failed (errno %d)", e); return -1; }
  if(n < 48) { snprintf(why, wn, "short answer"); return -1; }
  if(from.sin_addr.s_addr != srv->sin_addr.s_addr || from.sin_port != srv->sin_port) { snprintf(why, wn, "answer from an unexpected address"); return -1; }
  if(memcmp(rsp + 24, req + 40, 8)) { snprintf(why, wn, "answer does not match the request"); return -1; }
  if(n >= 48) { memcpy(g_last_pkt, rsp, 48); g_last_pkt_at = time(NULL); }   /* for the raw view */
  int li = rsp[0] >> 6, ver = (rsp[0] >> 3) & 7, mode = rsp[0] & 7, st = rsp[1];
  if(li == 3 || ver != 4 || mode != 4 || st == 0 || st >= 16) { snprintf(why, wn, "invalid answer (LI %d, v%d, mode %d, stratum %d)", li, ver, mode, st); return -1; }
  uint64_t sec = rd32(rsp + 40);
  uint32_t frac = rd32(rsp + 44);
  if(!sec && !frac) { snprintf(why, wn, "empty timestamp"); return -1; }
  if(sec < NTP_EPOCH) sec += 1ULL << 32;          /* NTP era 1 (after 2036) */
  int64_t unix_s = (int64_t)(sec - NTP_EPOCH);
  if(unix_s < 946684800LL || unix_s > 4102444800LL) { snprintf(why, wn, "date outside 2000-2100"); return -1; }
  int64_t rtt = t1 > t0 ? t1 - t0 : 0;
  int64_t us = unix_s * 1000000 + (int64_t)(((uint64_t)frac * 1000000) >> 32) + rtt / 2;
  out->tv_sec = (time_t)(us / 1000000);
  out->tv_usec = (suseconds_t)(us % 1000000);
  *rtt_us = rtt;
  *stratum = st;
  return 0;
}

static void finish(const char *state, const char *msg) {
  pthread_mutex_lock(&g_lock);
  snprintf(g.state, sizeof g.state, "%s", state);
  snprintf(g.msg, sizeof g.msg, "%s", msg);
  g.at = (long long)time(NULL);
  pthread_mutex_unlock(&g_lock);
}

static void *worker(void *arg) {
  int do_set = (int)(intptr_t)arg;
  struct timeval net;
  int64_t rtt = 0;
  int st = 0, got = 0;
  char why[96] = "no server reachable", srvname[48] = "";
  for(size_t i = 0; i < sizeof SERVERS / sizeof SERVERS[0] && !got; i++) {
    struct sockaddr_in a;
    if(resolve(SERVERS[i], &a)) { snprintf(why, sizeof why, "DNS lookup failed for %s", SERVERS[i]); continue; }
    for(int k = 0; k < 2 && !got; k++) {
      if(query(&a, &net, &rtt, &st, why, sizeof why) == 0) { got = 1; snprintf(srvname, sizeof srvname, "%s", SERVERS[i]); }
      else if(k == 0) sleep(1);
    }
  }
  if(!got) {
    char m[160];
    snprintf(m, sizeof m, "No valid answer from any time server: %s", why);
    finish("fail", m);
    g_running = 0;
    return NULL;
  }
  struct timeval before;
  gettimeofday(&before, NULL);
  long long off = ((long long)net.tv_sec - before.tv_sec) * 1000 + ((long long)net.tv_usec - before.tv_usec) / 1000;
  pthread_mutex_lock(&g_lock);
  snprintf(g.server, sizeof g.server, "%s", srvname);
  g.stratum = st;
  g.rtt_ms = (int)(rtt / 1000);
  g.offset_ms = off;
  pthread_mutex_unlock(&g_lock);
  if(!do_set) {
    finish("ok", "Checked: the clock was not changed");
    g_running = 0;
    return NULL;
  }
  if(settimeofday(&net, NULL) != 0) {
    char m[160];
    snprintf(m, sizeof m, "The PS5 refused to set the clock (settimeofday errno %d)", errno);
    finish("fail", m);
    g_running = 0;
    return NULL;
  }
  struct timeval after;
  gettimeofday(&after, NULL);
  long long d = ((long long)after.tv_sec - net.tv_sec) * 1000000 + ((long long)after.tv_usec - net.tv_usec);
  if(d < 0) d = -d;
  if(d > 2000000) {
    finish("fail", "Clock set, but reading it back did not match (more than 2 s off)");
  } else {
    finish("ok", "Clock synchronised");
    notify("Heatline: date & time synchronised");
  }
  g_running = 0;
  return NULL;
}

int ntp_start(int do_set) {
  if(__atomic_exchange_n(&g_running, 1, __ATOMIC_SEQ_CST)) return -1;   /* one at a time */
  pthread_mutex_lock(&g_lock);
  snprintf(g.action, sizeof g.action, "%s", do_set ? "sync" : "check");
  snprintf(g.state, sizeof g.state, "running");
  g.msg[0] = 0;
  pthread_mutex_unlock(&g_lock);
  pthread_t th;
  if(pthread_create(&th, NULL, worker, (void *)(intptr_t)do_set)) {
    finish("fail", "could not start");
    g_running = 0;
    return -1;
  }
  pthread_detach(th);
  return 0;
}

int ntp_autosync_get(void) {
  struct stat s;
  return stat(AUTO_FILE, &s) == 0;
}

int ntp_autosync_set(int on) {
  if(!on) return unlink(AUTO_FILE) == 0 || errno == ENOENT ? 0 : -1;
  mkdir(AUTO_DIR, 0755);
  FILE *f = fopen(AUTO_FILE, "w");
  if(!f) return -1;
  fputs("1\n", f);
  fclose(f);
  return 0;
}

void ntp_boot(void) {
  if(!ntp_autosync_get()) return;
  pthread_mutex_lock(&g_lock);
  g.auto_run = 1;
  pthread_mutex_unlock(&g_lock);
  ntp_start(1);
}

int ntp_json(char *buf, int size) {
  pthread_mutex_lock(&g_lock);
  int n = jcat(buf, size, 0,
                   "\"ntp\":{\"action\":\"%s\",\"state\":\"%s\",\"server\":\"%s\",\"stratum\":%d,\"rtt_ms\":%d,"
                   "\"offset_ms\":%lld,\"at\":%lld,\"msg\":\"%s\",\"auto\":%s,\"auto_run\":%s},",
                   g.action, g.state, g.server, g.stratum, g.rtt_ms, g.offset_ms, g.at, g.msg,
                   ntp_autosync_get() ? "true" : "false", g.auto_run ? "true" : "false");
  pthread_mutex_unlock(&g_lock);
  return n < size ? n : size - 1;
}
