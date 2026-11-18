// SPDX-License-Identifier: GPL-3.0-only
/* sysinfo.c - "System" and "Network" pages (fields and sources as PS5 Monitor 1.4.8).
 *
 * Hardware:  hw.machine, hw.ncpu (sysctl); model and firmware come from main.c.
 * Extended:  uptime = now - kern.boottime (PS5 Monitor's fallback; the ICC
 *            operating-time call is not used: PS5 Upload Suite removed it as
 *            destabilizing), boot / shutdown count = sceKernelIccGetPowerNumber-
 *            OfBootShutdown (u64: low word boots, high word shutdowns, decoded
 *            like PS5 Monitor), thermal status = sceKernelIccGetThermalAlert
 *            (0 = Normal), BD drive = sceKernelIccGetBDPowerState.
 * Network:   sceNetCtlGetState / sceNetCtlGetInfo (codes as PS5 Monitor); the
 *            interface is the one holding the IPv4 address (getifaddrs).
 *
 * The ICC and NetCtl calls have no published signatures, so (like PS5 Monitor)
 * every call writes into an oversized zeroed buffer and runs behind a SIGSEGV /
 * SIGBUS guard, on a background thread: a bad call or a hang never touches the
 * web server. Static values are read once, the rest every 30 s.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_mib.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sysinfo.h"
#include "jbuf.h"

int sceKernelIccGetPowerNumberOfBootShutdown(void *out) __attribute__((weak));
int sceKernelIccGetThermalAlert(void *out) __attribute__((weak));
int sceKernelIccGetBDPowerState(void *out) __attribute__((weak));
int sceNetCtlInit(void) __attribute__((weak));
int sceNetCtlGetState(int *state) __attribute__((weak));
int sceNetCtlGetInfo(int code, void *info) __attribute__((weak));

enum { NI_DEVICE = 1, NI_ETHER_ADDR = 2, NI_MTU = 3, NI_DHCP_HOSTNAME = 12, NI_IP_ADDRESS = 14,
       NI_NETMASK = 15, NI_DEFAULT_ROUTE = 16, NI_PRIMARY_DNS = 17, NI_SECONDARY_DNS = 18 };

/* ---- crash guard (per thread) */
static __thread sigjmp_buf t_jb;
static __thread volatile int t_armed;

static void fault(int sig) {
  if(t_armed) {
    t_armed = 0;
    siglongjmp(t_jb, sig);
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

#define GUARDED(rc, call) do { \
    if(sigsetjmp(t_jb, 1) == 0) { t_armed = 1; rc = (call); t_armed = 0; } \
    else { rc = -999; } \
  } while(0)

/* ---- state */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_machine[32], g_net[1024], g_ext[256];
static char g_iface[32];                 /* interface holding the IPv4 address */
/* live network speed: interface byte counters (same source as PS5 Monitor:
 * FreeBSD ifmib sysctl, else getifaddrs AF_LINK if_data), sampled every 1 s */
static double g_rx_bps = -1, g_tx_bps = -1;
static char g_rate_if[32], g_rate_src[16];
static int g_ncpu;

static void sysctl_str(const char *name, char *out, size_t n) {
  size_t len = n - 1;
  if(sysctlbyname(name, out, &len, NULL, 0) != 0) out[0] = 0;
  else out[len < n ? len : n - 1] = 0;
}

static void json_str(char *out, size_t n, const char *s) {   /* minimal escaping */
  size_t j = 0;
  for(size_t i = 0; s[i] && j + 2 < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if(c < 0x20 || c >= 0x7f || c == '"' || c == '\\') continue;
    out[j++] = c;
  }
  out[j] = 0;
}

static void read_icc(void) {
  static uint8_t b[256];
  int rc;
  char bs[96] = "null", th[16] = "null", bd[16] = "null";
  if(sceKernelIccGetPowerNumberOfBootShutdown) {
    memset(b, 0, sizeof b);
    GUARDED(rc, sceKernelIccGetPowerNumberOfBootShutdown(b));
    if(rc == 0) {
      uint64_t raw;
      memcpy(&raw, b, 8);
      uint32_t lo = (uint32_t)raw, hi = (uint32_t)(raw >> 32), boot = lo, shut = hi;
      if(!boot && hi) boot = hi;           /* PS5 Monitor: {pad, count} layouts */
      if(!shut && lo) shut = lo;
      snprintf(bs, sizeof bs, "{\"boot\":%u,\"shutdown\":%u,\"raw\":\"%016llx\"}", boot, shut, (unsigned long long)raw);
    }
  }
  if(sceKernelIccGetThermalAlert) {
    memset(b, 0, sizeof b);
    GUARDED(rc, sceKernelIccGetThermalAlert(b));
    if(rc == 0) { int v; memcpy(&v, b, 4); snprintf(th, sizeof th, "%d", v); }
  }
  if(sceKernelIccGetBDPowerState) {
    memset(b, 0, sizeof b);
    GUARDED(rc, sceKernelIccGetBDPowerState(b));
    if(rc == 0) snprintf(bd, sizeof bd, "%u", b[0]);   /* ps5-hwinfo: uint8_t out */
  }
  pthread_mutex_lock(&g_lock);
  snprintf(g_ext, sizeof g_ext, "\"bootshut\":%s,\"thermal_alert\":%s,\"bd_power\":%s", bs, th, bd);
  pthread_mutex_unlock(&g_lock);
}

static char g_gw[48];          /* default gateway (router), for the latency probe */

void sysinfo_gateway(char *out, int size) {
  pthread_mutex_lock(&g_lock);
  snprintf(out, size, "%s", g_gw);
  pthread_mutex_unlock(&g_lock);
}

/* runs fn(ctx) with the fault guard: 0 = returned normally, -1 = it crashed
 * (SIGSEGV / SIGBUS / SIGILL caught). For calls whose arguments are not
 * documented (display.c). The handler is installed by the worker at start. */
int sysinfo_guarded_call(int (*fn)(void *), void *ctx) {
  int rc;
  GUARDED(rc, fn(ctx));
  return rc == -999 ? -1 : 0;
}

static int nc_info(int code, uint8_t *buf) {   /* buf: 2048 bytes (the info union) */
  int rc;
  memset(buf, 0, 2048);
  GUARDED(rc, sceNetCtlGetInfo(code, buf));
  return rc;
}

int sysinfo_netctl_raw(int code, unsigned char *buf2048) {   /* raw view */
  return nc_info(code, buf2048);
}

static void read_net(void) {
  static int inited = -1;
  static uint8_t buf[2048];
  char out[1024];
  int n = 0, rc;
  if(!sceNetCtlInit || !sceNetCtlGetInfo || !sceNetCtlGetState) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_net, sizeof g_net, "\"ok\":false");
    pthread_mutex_unlock(&g_lock);
    return;
  }
  if(inited < 0) { GUARDED(rc, sceNetCtlInit()); inited = rc == 0; }
  if(!inited) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_net, sizeof g_net, "\"ok\":false");
    pthread_mutex_unlock(&g_lock);
    return;
  }
  static int st;
  st = -1;
  GUARDED(rc, sceNetCtlGetState(&st));
  n = jcat(out, sizeof out, n, "\"ok\":true,\"state\":%d", rc == 0 ? st : -1);
  static const struct { int code; const char *key; } strs[] = {
    {NI_DHCP_HOSTNAME, "hostname"}, {NI_IP_ADDRESS, "ip"}, {NI_NETMASK, "netmask"},
    {NI_DEFAULT_ROUTE, "gateway"}, {NI_PRIMARY_DNS, "dns1"}, {NI_SECONDARY_DNS, "dns2"}};
  char ip[64] = "";
  for(size_t i = 0; i < sizeof strs / sizeof strs[0]; i++) {
    char v[80] = "";
    if(nc_info(strs[i].code, buf) == 0) { buf[63] = 0; json_str(v, sizeof v, (char *)buf); }
    if(strs[i].code == NI_IP_ADDRESS) snprintf(ip, sizeof ip, "%s", v);
    if(strs[i].code == NI_DEFAULT_ROUTE) {
      pthread_mutex_lock(&g_lock);
      snprintf(g_gw, sizeof g_gw, "%s", v);
      pthread_mutex_unlock(&g_lock);
    }
    n = jcat(out, sizeof out, n, ",\"%s\":\"%s\"", strs[i].key, v);
  }
  if(nc_info(NI_ETHER_ADDR, buf) == 0)
    n = jcat(out, sizeof out, n, ",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"",
                  buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
  if(nc_info(NI_MTU, buf) == 0) { int v; memcpy(&v, buf, 4); n = jcat(out, sizeof out, n, ",\"mtu\":%d", v); }
  if(nc_info(NI_DEVICE, buf) == 0) { int v; memcpy(&v, buf, 4); n = jcat(out, sizeof out, n, ",\"device\":%d", v); }
  /* interface = the one that holds the IPv4 address */
  char ifname[32] = "";
  struct ifaddrs *ifa = NULL;
  if(ip[0] && getifaddrs(&ifa) == 0) {
    for(struct ifaddrs *p = ifa; p; p = p->ifa_next) {
      if(!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !p->ifa_name) continue;
      char a[INET_ADDRSTRLEN];
      if(inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, a, sizeof a) && !strcmp(a, ip)) {
        json_str(ifname, sizeof ifname, p->ifa_name);
        break;
      }
    }
    freeifaddrs(ifa);
  }
  n = jcat(out, sizeof out, n, ",\"iface\":\"%s\"", ifname);
  pthread_mutex_lock(&g_lock);
  snprintf(g_iface, sizeof g_iface, "%s", ifname);
  snprintf(g_net, sizeof g_net, "%s", out);
  pthread_mutex_unlock(&g_lock);
}

static volatile int g_refresh_now;
void sysinfo_refresh(void) { g_refresh_now = 1; }

static void *worker(void *arg) {
  (void)arg;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = fault;
  sa.sa_flags = SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  char m[32];
  sysctl_str("hw.machine", m, sizeof m);
  int ncpu = 0;
  size_t len = sizeof ncpu;
  if(sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) != 0) ncpu = 0;
  pthread_mutex_lock(&g_lock);
  json_str(g_machine, sizeof g_machine, m);
  g_ncpu = ncpu;
  pthread_mutex_unlock(&g_lock);
  for(;;) {
    read_icc();
    read_net();
    for(int i = 0; i < 30 && !g_refresh_now; i++) sleep(1);   /* 30 s, or at once on request */
    g_refresh_now = 0;
  }
  return NULL;
}

/* bytes received / sent by one interface (want = name, or "" = busiest non-lo) */
static int if_counters(const char *want, uint64_t *rx, uint64_t *tx, char *name, size_t nn, const char **src) {
  uint64_t best = 0;
  int found = 0;
  int ifcount = 0;
  size_t len = sizeof ifcount;
  int cmib[5] = {CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_SYSTEM, IFMIB_IFCOUNT};
  if(sysctl(cmib, 5, &ifcount, &len, NULL, 0) == 0 && ifcount > 0 && ifcount <= 256) {
    for(int row = 1; row <= ifcount; row++) {
      struct ifmibdata md;
      int mib[6] = {CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_IFDATA, row, IFDATA_GENERAL};
      memset(&md, 0, sizeof md);
      len = sizeof md;
      if(sysctl(mib, 6, &md, &len, NULL, 0) != 0 || !md.ifmd_name[0] || !strncmp(md.ifmd_name, "lo", 2)) continue;
      uint64_t i = md.ifmd_data.ifi_ibytes, o = md.ifmd_data.ifi_obytes;
      if(want[0] ? !strcmp(md.ifmd_name, want) : i + o >= best) {
        best = i + o; *rx = i; *tx = o; found = 1;
        snprintf(name, nn, "%s", md.ifmd_name);
        *src = "ifmib";
        if(want[0]) break;
      }
    }
    if(found && (*rx || *tx)) return 1;
  }
  struct ifaddrs *ifa = NULL;
  if(getifaddrs(&ifa) != 0) return found;
  for(struct ifaddrs *p = ifa; p; p = p->ifa_next) {
    if(!p->ifa_addr || p->ifa_addr->sa_family != AF_LINK || !p->ifa_data || !p->ifa_name || !strncmp(p->ifa_name, "lo", 2)) continue;
    struct if_data *d = (struct if_data *)p->ifa_data;
    uint64_t i = d->ifi_ibytes, o = d->ifi_obytes;
    if(want[0] ? !strcmp(p->ifa_name, want) : i + o >= best) {
      best = i + o; *rx = i; *tx = o; found = 1;
      snprintf(name, nn, "%s", p->ifa_name);
      *src = "getifaddrs";
      if(want[0]) break;
    }
  }
  freeifaddrs(ifa);
  return found;
}

static int64_t mono_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void *rate_worker(void *arg) {
  (void)arg;
  uint64_t prx = 0, ptx = 0;
  int64_t pt = 0;
  char pname[32] = "";
  for(;;) {
    char want[32], name[32] = "";
    pthread_mutex_lock(&g_lock);
    snprintf(want, sizeof want, "%s", g_iface);
    pthread_mutex_unlock(&g_lock);
    uint64_t rx = 0, tx = 0;
    const char *src = "";
    int ok = if_counters(want, &rx, &tx, name, sizeof name, &src);
    int64_t t = mono_us();
    double r = -1, w = -1;
    if(ok && pt && !strcmp(name, pname) && rx >= prx && tx >= ptx && t > pt) {
      r = (double)(rx - prx) * 1e6 / (double)(t - pt);
      w = (double)(tx - ptx) * 1e6 / (double)(t - pt);
    }
    pthread_mutex_lock(&g_lock);
    g_rx_bps = r;
    g_tx_bps = w;
    snprintf(g_rate_if, sizeof g_rate_if, "%s", ok ? name : "");
    snprintf(g_rate_src, sizeof g_rate_src, "%s", ok ? src : "");
    pthread_mutex_unlock(&g_lock);
    if(ok) { prx = rx; ptx = tx; pt = t; snprintf(pname, sizeof pname, "%s", name); }
    sleep(1);
  }
  return NULL;
}

void sysinfo_start(void) {
  snprintf(g_net, sizeof g_net, "\"ok\":false,\"pending\":true");
  snprintf(g_ext, sizeof g_ext, "\"pending\":true");
  pthread_t th;
  if(pthread_create(&th, NULL, worker, NULL) == 0) pthread_detach(th);
  if(pthread_create(&th, NULL, rate_worker, NULL) == 0) pthread_detach(th);
}
int
sysinfo_rates(double *rx_bps, double *tx_bps) {
  pthread_mutex_lock(&g_lock);
  int ok = g_rx_bps >= 0 && g_tx_bps >= 0;
  if(ok) { *rx_bps = g_rx_bps; *tx_bps = g_tx_bps; }
  pthread_mutex_unlock(&g_lock);
  return ok;
}


int sysinfo_json(char *buf, int size) {
  long long up = -1;
  struct timeval bt;
  size_t len = sizeof bt;
  if(sysctlbyname("kern.boottime", &bt, &len, NULL, 0) == 0 && bt.tv_sec > 0) {
    struct timeval now;
    gettimeofday(&now, NULL);
    if(now.tv_sec > bt.tv_sec) up = now.tv_sec - bt.tv_sec;
  }
  pthread_mutex_lock(&g_lock);
  int n = jcat(buf, size, 0, "\"sys\":{\"machine\":\"%s\",\"ncpu\":%d,\"uptime_s\":%lld,%s},\"net\":{%s,"
                   "\"rate\":{\"rx_bps\":%.0f,\"tx_bps\":%.0f,\"iface\":\"%s\",\"src\":\"%s\"}},",
                   g_machine, g_ncpu, up, g_ext, g_net, g_rx_bps, g_tx_bps, g_rate_if, g_rate_src);
  pthread_mutex_unlock(&g_lock);
  return n < size ? n : size - 1;
}
