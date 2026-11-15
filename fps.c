// SPDX-License-Identifier: GPL-3.0-only
#include "fps.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>
#include "jbuf.h"

int sceSystemServiceGetAppIdOfRunningBigApp(void) __attribute__((weak));
int sceLncUtilGetAppTitleId(uint32_t appid, char *title) __attribute__((weak));
uint64_t sceKernelGetTscFrequency(void) __attribute__((weak));

/* Auth id used by PS5 Thermal Doctor / Cooling & System Center. */
#define AUTHID_SYS 0x4801000000000013ULL

#define DCE_IOCTL     0x0000000080308217UL
#define DCE_IOCTL_SX  0xFFFFFFFF80308217UL

typedef struct {
  uint64_t selector;
  uint64_t mask;
  uint64_t output;
  uint64_t reserved[3];
} dce_arg_t;
_Static_assert(sizeof(dce_arg_t) == 0x30, "dce ioctl expects 0x30 bytes");

/* Frame timing.
 * The display driver's answer holds the frame (flip) counter at offset 0x08
 * and, at offset 0x20, the time of the last frame on the CPU timestamp
 * counter (same clock as sceKernelGetTscFrequency). Found with Heatline's
 * diagnostics on a CFI-2016 / FW 13.60: it changes only with the counter and
 * advances at exactly the TSC rate. Offset 0x18 holds the same moment in
 * microseconds and is used if the TSC frequency is unavailable.
 *
 * We read every POLL_US. A frame's duration comes from the console's
 * timestamps, so the read rate does not affect accuracy; it only has to be
 * faster than the frame rate (PS5 output is at most 120 Hz = 8.3 ms) so that
 * each frame is seen on its own. */
#define OFF_FLIPS    0x08
/* Timestamp candidates, all seen moving only with the counter in the
 * diagnostics data: tried in this order for each new frame; the first one
 * that advanced is used. If none moved, Heatline's own clock is used. */
static const struct { int off; int tsc; const char *name; } TS_SRC[] = {
  {0x20, 1, "0x20"}, {0x38, 1, "0x38"}, {0x18, 0, "0x18"}, {0x40, 1, "0x40"}, {0x48, 1, "0x48"},
};
#define N_TS_SRC (int)(sizeof TS_SRC / sizeof TS_SRC[0])
#define POLL_US      4000         /* 250 reads/s while a game runs */
#define MAX_GAP_US   250000       /* a longer frame is a pause, not a frame */

/* Session frame-time distribution: 0.01 ms bins up to 250 ms. */
#define FT_BIN_US    10
#define FT_BINS      (MAX_GAP_US / FT_BIN_US + 1)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool     g_game;
static char     g_title[24] = "";
static char     g_title_name[128] = "";   /* game name from the console's app metadata, if found */
static char     g_title_src[24] = "";     /* where it came from */
static char     g_status[96] = "starting";
static uint64_t g_seq;           /* +1 each time a new 1 s summary is ready */
static double   g_fps_now = -1;  /* frames shown during the last second */
static double   g_ft_avg = -1;   /* mean frame time of the last second, ms */
static double   g_ft_worst = -1; /* longest frame of the last second, ms */
static double   g_min_fps, g_max_fps;   /* lowest / highest 1 s FPS */
static uint32_t g_ft_hist[FT_BINS];
static uint64_t g_frames;        /* frames counted in the session */
static uint64_t g_measured;      /* ...of which timed one by one */
static uint64_t g_merged;        /* ...seen two or more at once: no own time */
static double   g_meas_us;       /* total duration of the frames timed one by one */
static double   g_time_us;       /* session time, pauses excluded */
static int      g_reset_req;
static const char *g_clock = "console";      /* last timestamp source used */
static uint64_t g_estimated;                 /* frames timed with Heatline's clock */

/* Real-time metrics, 1:1 with CapFrameX's overlay "Real-time" metrics
 * (CapFrameX OnlineMetricService + FrametimeStatisticProvider, read from its
 * source): every frame time goes into a window holding the last
 * RT_WIN_S = 20 s (CapFrameX's default "metric interval"; a frame leaves when
 * newest - its time > 20 s); stuttering uses its own 5 s window. If more than
 * 2 s pass between two frames, all windows are cleared (CapFrameX resets its
 * buffers after 2 s without a frame). Recomputed every second (CapFrameX's
 * default overlay refresh). 32768 slots = 20 s at 1638 FPS. */
#define RT_WIN_S     20.0
#define STUT_WIN_S   5.0
#define STUT_FACTOR  2.5            /* CapFrameX default stuttering factor */
#define RESET_GAP_US 2000000.0
#define RING         32768
static double   g_ring_ft[RING];   /* frame durations, µs */
static double   g_ring_t[RING];    /* frame end times, µs */
static uint32_t g_ring_head, g_ring_len;
static double   g_rt_avg = -1, g_rt_p1 = -1, g_rt_p02 = -1, g_rt_low1 = -1, g_rt_low02 = -1, g_rt_stut = -1;
/* "more metrics" over the same 20 s window, same CapFrameX formulas
 * (GetFpsMetricValue): P5 / Median / P95 / P99 / P0.1 = R-8 quantiles of the
 * FPS values, 0.1% low (average), x% low "integral", min / max frame time,
 * adaptive standard deviation of the FPS (500 ms time-based moving average). */
static double   g_x_p5 = -1, g_x_p50 = -1, g_x_p95 = -1, g_x_p99 = -1, g_x_p01 = -1, g_x_low01 = -1;
static double   g_x_il1 = -1, g_x_il02 = -1, g_x_il01 = -1, g_x_ftmin = -1, g_x_ftmax = -1, g_x_astd = -1;
/* the "more metrics" are only computed when turned on in the page (saved as
 * the file below): off = no extra quantiles, no adaptive deviation pass */
#define MORE_FILE "/data/heatline/fps_more"
static int      g_more = -1;          /* -1 = not read yet */
static uint64_t g_rt_frames, g_rt_split;
static uint64_t g_split;          /* frames whose time was shared out (2+ flips between two reads) */

/* Time below an FPS target, 1:1 with CapFrameX GetFpsThresholdTimes /
 * GetFpsThresholdCounts (same target list, "not reversed"): for each target T,
 * the sum of the frame times longer than 1000 / T ms, and their count. CapFrameX
 * divides by the sum of all frame times; here that is the session time
 * (g_time_us, pauses > 250 ms left out like the other session values). */
static const double FPS_TH[] = {240, 144, 120, 90, 75, 60, 45, 30, 15, 10};
#define N_TH (int)(sizeof FPS_TH / sizeof FPS_TH[0])
static double   g_below_us[N_TH];
static double   g_below_base_us;  /* session time counted while "more metrics" was on */
static uint64_t g_below_n[N_TH];

/* Stutter count. CapFrameX GetStutteringCountPercentage: frames longer than
 * 2.5 x the mean frame time of the sequence. Applied to the same 5 s window
 * as the real-time stuttering time (count + share of frames). Session count:
 * each frame is judged once, at the 1 s update after it arrived, against the
 * 5 s mean at that moment (Heatline's running rule - CapFrameX has no
 * real-time stutter count). */
static uint64_t g_rt_stut_n, g_rt_stut_frames, g_stut_sess;
static uint32_t g_new_frames;     /* frames pushed since the last 1 s update */
static uint64_t g_ring_seq;       /* total frames ever pushed (frame-time graph cursor) */

static int g_dce_fd = -1;
static int g_dce_sx;


static uint64_t
now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}


static void
set_status(const char *s) {
  pthread_mutex_lock(&g_lock);
  snprintf(g_status, sizeof g_status, "%s", s);
  pthread_mutex_unlock(&g_lock);
}


static int
escalate(void) {
  pid_t pid = getpid();
  int rc = 0;
  if(!kernel_get_proc(pid)) return -1;
  if(kernel_set_ucred_uid(pid, 0))   rc = -1;
  if(kernel_set_ucred_ruid(pid, 0))  rc = -1;
  if(kernel_set_ucred_svuid(pid, 0)) rc = -1;
  if(kernel_set_ucred_rgid(pid, 0))  rc = -1;
  if(kernel_set_ucred_svgid(pid, 0)) rc = -1;
  intptr_t root = kernel_get_root_vnode();
  if(root) {
    if(kernel_set_proc_rootdir(pid, root)) rc = -1;
    if(kernel_set_proc_jaildir(pid, root)) rc = -1;
  }
  if(kernel_set_ucred_authid(pid, AUTHID_SYS)) rc = -1;
  uint8_t caps[16];
  memset(caps, 0xff, sizeof caps);
  if(kernel_set_ucred_caps(pid, caps)) rc = -1;
  uint8_t attrs[32];
  memset(attrs, 0, sizeof attrs);
  attrs[0] = 0x80;
  if(kernel_set_ucred_attrs(pid, attrs)) rc = -1;
  return rc;
}


/* Serialises /dev/dce access (the fd and the sign-extension flag are shared). */
static pthread_mutex_t g_dce_lock = PTHREAD_MUTEX_INITIALIZER;

/* Raw read: fills all 0x60 bytes the driver returns. 0 on success; -errno on
 * failure. */
static int
dce_read_raw(uint8_t out[0x60]) {
  int rc = -EINVAL;
  pthread_mutex_lock(&g_dce_lock);
  if(g_dce_fd < 0) {
    g_dce_fd = open("/dev/dce", O_RDWR);
    if(g_dce_fd < 0) {
      rc = -errno;
      pthread_mutex_unlock(&g_dce_lock);
      return rc;
    }
  }
  for(int attempt = 0; attempt < 2; attempt++) {
    dce_arg_t arg;
    memset(out, 0, 0x60);
    memset(&arg, 0, sizeof arg);
    arg.selector = 0x10000000AULL;
    arg.mask     = 0x8000000000ULL;
    arg.output   = (uint64_t)(uintptr_t)out;
    if(ioctl(g_dce_fd, g_dce_sx ? DCE_IOCTL_SX : DCE_IOCTL, &arg) == 0) {
      rc = 0;
      break;
    }
    int e = errno;
    if(e == EINVAL && !g_dce_sx) {
      g_dce_sx = 1;
      continue;
    }
    if(e == EBADF) {
      close(g_dce_fd);
      g_dce_fd = -1;
    }
    rc = -e;
    break;
  }
  pthread_mutex_unlock(&g_dce_lock);
  return rc;
}


/* Raw data view (/raw?p=game): one extra read of the driver's answer, only
 * when asked. The 4 ms sampler is not touched. */
int
fps_dce_raw(uint8_t out[0x60]) {
  return dce_read_raw(out);
}

/* Fallback title lookup: the running game's image is mounted at
 * /mnt/sandbox/pfsmnt/<TITLE>-app0 (method from PS5 Thermal Doctor). */
static int
title_from_pfsmnt(char *out, size_t n) {
  DIR *d = opendir("/mnt/sandbox/pfsmnt");
  struct dirent *e;
  int found = 0;
  if(!d) return 0;
  while((e = readdir(d))) {
    size_t len = strlen(e->d_name);
    if(len > 5 && !strcmp(e->d_name + len - 5, "-app0") && len - 5 < n) {
      memcpy(out, e->d_name, len - 5);
      out[len - 5] = '\0';
      found = 1;
      break;
    }
  }
  closedir(d);
  return found;
}


static int
game_probe(char *title, size_t n) {
  int appid = -1;
  title[0] = '\0';
  if(sceSystemServiceGetAppIdOfRunningBigApp) {
    appid = sceSystemServiceGetAppIdOfRunningBigApp();
  }
  if(appid > 0) {
    /* Title ID from the app id (method used by PS5 Thermal Doctor). */
    char buf[32] = {0};
    if(sceLncUtilGetAppTitleId &&
       sceLncUtilGetAppTitleId((uint32_t)appid, buf) == 0 && buf[0]) {
      int ok = 1;
      for(char *p = buf; *p; p++) {
        if(!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
             (*p >= 'a' && *p <= 'z'))) ok = 0;
      }
      if(ok) {
        snprintf(title, n, "%.15s", buf);
        return 1;
      }
    }
    if(!title_from_pfsmnt(title, n)) snprintf(title, n, "APP%d", appid);
    return 1;
  }
  if(appid == 0) return 0;
  return title_from_pfsmnt(title, n);
}


static void
session_clear_locked(void) {
  memset(g_ft_hist, 0, sizeof g_ft_hist);
  g_min_fps = -1;
  g_max_fps = -1;
  g_frames = 0;
  g_measured = 0;
  g_merged = 0;
  g_meas_us = 0;
  g_estimated = 0;
  g_time_us = 0;
  g_fps_now = -1;
  g_ft_avg = -1;
  g_ft_worst = -1;
  g_ring_head = g_ring_len = 0;
  g_rt_avg = g_rt_p1 = g_rt_p02 = g_rt_low1 = g_rt_low02 = g_rt_stut = -1;
  g_x_p5 = g_x_p50 = g_x_p95 = g_x_p99 = g_x_p01 = g_x_low01 = -1;
  g_x_il1 = g_x_il02 = g_x_il01 = g_x_ftmin = g_x_ftmax = g_x_astd = -1;
  g_rt_frames = g_rt_split = 0;
  g_split = 0;
  memset(g_below_us, 0, sizeof g_below_us);
  memset(g_below_n, 0, sizeof g_below_n);
  g_below_base_us = 0;
  g_rt_stut_n = g_rt_stut_frames = g_stut_sess = 0;
  g_new_frames = 0;
}


static int
cmp_asc(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}


/* MathNet.Numerics Quantile (R-8), as CapFrameX calls it:
 * h = (n + 1/3) tau + 1/3; below / above the ends -> min / max, else linear
 * between the floor(h)-th and next value (1-based). `x` sorted ascending. */
static double
quantile_r8(const double *x, size_t n, double tau) {
  if(!n) return -1;
  double h = ((double)n + 1.0 / 3.0) * tau + 1.0 / 3.0;
  long hf = (long)h;
  if(hf <= 0 || tau == 0.0) return x[0];
  if(hf >= (long)n || tau == 1.0) return x[n - 1];
  double a = x[hf - 1], b = x[hf];
  return a + (h - (double)hf) * (b - a);
}

/* C# Math.Round(v, digits, MidpointRounding.AwayFromZero) */
static double
round_away(double v, int digits) {
  double m = digits == 1 ? 10.0 : 1.0;
  double s = v * m;
  double r = s < 0 ? -floor(-s + 0.5) : floor(s + 0.5);
  return r / m;
}

/* x% low "average" (CapFrameX OnePercentLowAverage / ZerodotTwoPercentLowAverage):
 * 1000 / mean of the frame times >= their (1 - x) quantile. `ft` sorted ascending, ms. */
static double
low_average(const double *ft, size_t n, double x) {
  double q = quantile_r8(ft, n, 1.0 - x), sum = 0;
  size_t k = 0;
  for(size_t i = 0; i < n; i++) if(ft[i] >= q) { sum += ft[i]; k++; }
  return k && sum > 0 ? 1000.0 / (sum / (double)k) : -1;
}

/* x% low "integral" (CapFrameX GetPercentageHighIntegralSequence): frame
 * times sorted longest first, summed until they reach x of the total time;
 * the frame time where that happens, as FPS. `ft` sorted ascending, ms. */
static double
low_integral(const double *ft, size_t n, double x, double total) {
  double target = total * x, acc = 0;
  size_t idx = n - 1;
  for(size_t k = 0; k < n; k++) {
    idx = n - 1 - k;
    acc += ft[idx];
    if(acc >= target) break;
  }
  return ft[idx] > 0 ? 1000.0 / ft[idx] : -1;
}

/* CapFrameX GetFpsAdaptiveStandardDeviation, window 500 ms (its default
 * IntervalAverageWindowTime): residuals of each frame's FPS against the mean
 * FPS of the frames in the trailing 500 ms; sqrt(sum r^2 / (n - 1)).
 * `ft` in chronological order, ms. */
static double
adaptive_std(const double *ft, size_t n, double window_ms) {
  if(n < 2) return -1;
  size_t ws = 0;
  double wft = 0, wfps = 0, sq = 0;
  for(size_t i = 0; i < n; i++) {
    double fps = 1000.0 / ft[i];
    wft += ft[i];
    wfps += fps;
    while(ws < i && wft - ft[ws] >= window_ms) {
      wft -= ft[ws];
      wfps -= 1000.0 / ft[ws];
      ws++;
    }
    double r = fps - wfps / (double)(i - ws + 1);
    sq += r * r;
  }
  return sqrt(sq / (double)(n - 1));
}

static void
ring_push_locked(double ft_us, double t_us, int split) {
  g_ring_ft[g_ring_head] = ft_us;
  g_ring_t[g_ring_head] = t_us;
  g_ring_head = (g_ring_head + 1) % RING;
  if(g_ring_len < RING) g_ring_len++;
  if(g_new_frames < RING) g_new_frames++;
  g_ring_seq++;
  if(split) g_split++;
}

/* CapFrameX's GetFpsMetricValue + overlay rounding, recomputed every second.
 * Caller holds the lock. */
static void
rt_update_locked(void) {
  static double ft[RING], fps[RING], chrono[RING];
  size_t n = 0, ns = 0;
  if(!g_ring_len) {
    g_rt_avg = g_rt_p1 = g_rt_p02 = g_rt_low1 = g_rt_low02 = g_rt_stut = -1;
    g_rt_frames = g_rt_split = 0;
    g_rt_stut_n = g_rt_stut_frames = 0;
    g_new_frames = 0;
    return;
  }
  uint32_t newest = (g_ring_head + RING - 1) % RING;
  double tn = g_ring_t[newest], sum = 0, ssum = 0, sstut = 0;
  for(uint32_t k = 0; k < g_ring_len; k++) {          /* newest first */
    uint32_t i = (g_ring_head + RING - 1 - k) % RING;
    if(tn - g_ring_t[i] > RT_WIN_S * 1e6) break;
    double v = g_ring_ft[i] / 1000.0;                 /* ms, as PresentMon */
    if(!(v > 0)) continue;                            /* CopyValidTimings */
    ft[n++] = v;
    sum += v;
  }
  g_rt_frames = n;
  if(!n) return;
  if(g_more > 0) for(size_t i = 0; i < n; i++) chrono[i] = ft[n - 1 - i];   /* oldest first */
  /* Average = count * 1000 / sum of frame times */
  double avg = (double)n * 1000.0 / sum;
  /* Percentiles on the FPS values (1000 / ft), R-8 quantile */
  for(size_t i = 0; i < n; i++) fps[i] = 1000.0 / ft[i];
  qsort(fps, n, sizeof fps[0], cmp_asc);
  double p1 = quantile_r8(fps, n, 0.01), p02 = quantile_r8(fps, n, 0.002);
  /* x% low average on the frame times */
  qsort(ft, n, sizeof ft[0], cmp_asc);
  double l1 = low_average(ft, n, 0.01), l02 = low_average(ft, n, 0.002);
  /* Stuttering time: 5 s window, frames longer than 2.5 x the mean */
  for(uint32_t k = 0; k < g_ring_len; k++) {
    uint32_t i = (g_ring_head + RING - 1 - k) % RING;
    if(tn - g_ring_t[i] > STUT_WIN_S * 1e6) break;
    double v = g_ring_ft[i] / 1000.0;
    if(!(v > 0)) continue;
    ssum += v;
    ns++;
  }
  if(ns) {
    double mean = ssum / (double)ns;
    uint64_t cnt = 0;
    for(uint32_t k = 0; k < g_ring_len; k++) {
      uint32_t i = (g_ring_head + RING - 1 - k) % RING;
      if(tn - g_ring_t[i] > STUT_WIN_S * 1e6) break;
      double v = g_ring_ft[i] / 1000.0;
      if(v > STUT_FACTOR * mean) {
        sstut += v;
        cnt++;
        if(g_more > 0 && k < g_new_frames) g_stut_sess++;   /* new since the last update */
      }
    }
    g_rt_stut_n = cnt;
  }
  g_rt_stut_frames = ns;
  g_new_frames = 0;
  /* GetFpsMetricValue rounds to 1 decimal (FpsValuesRoundingDigits = 1);
   * the overlay then shows FPS values rounded to whole numbers and the
   * stuttering time to 1 decimal - the page does that last step. */
  g_rt_avg = round_away(avg, 1);
  g_rt_p1 = round_away(p1, 1);
  g_rt_p02 = round_away(p02, 1);
  g_rt_low1 = round_away(l1, 1);
  g_rt_low02 = round_away(l02, 1);
  g_rt_stut = ns ? 100.0 * sstut / ssum : -1;
  if(g_more <= 0) {
    g_x_p5 = g_x_p50 = g_x_p95 = g_x_p99 = g_x_p01 = g_x_low01 = -1;
    g_x_il1 = g_x_il02 = g_x_il01 = g_x_ftmin = g_x_ftmax = g_x_astd = -1;
    return;
  }
  /* more metrics (fps[] and ft[] are sorted ascending here) */
  g_x_p5 = round_away(quantile_r8(fps, n, 0.05), 1);
  g_x_p50 = round_away(quantile_r8(fps, n, 0.5), 1);
  g_x_p95 = round_away(quantile_r8(fps, n, 0.95), 1);
  g_x_p99 = round_away(quantile_r8(fps, n, 0.99), 1);
  g_x_p01 = round_away(quantile_r8(fps, n, 0.001), 1);
  g_x_low01 = round_away(low_average(ft, n, 0.001), 1);
  g_x_il1 = round_away(low_integral(ft, n, 0.01, sum), 1);
  g_x_il02 = round_away(low_integral(ft, n, 0.002, sum), 1);
  g_x_il01 = round_away(low_integral(ft, n, 0.001, sum), 1);
  g_x_ftmin = ft[0];
  g_x_ftmax = ft[n - 1];
  double as = adaptive_std(chrono, n, 500.0);
  g_x_astd = as >= 0 ? round_away(as, 1) : -1;
}


/* ---- game name from the console's own app metadata (not verified on every
 * firmware): /system_data/priv/appmeta/<ID>/param.json (PS5 games:
 * localizedParameters -> defaultLanguage -> titleName), else param.sfo (PS4
 * games: TITLE), same in /user/appmeta. Read once when the game changes. */
static void
put_utf8(char **o, char *end, unsigned cp) {
  char *p = *o;
  if(cp < 0x80) { if(p + 1 < end) *p++ = (char)cp; }
  else if(cp < 0x800) { if(p + 2 < end) { *p++ = (char)(0xC0 | cp >> 6); *p++ = (char)(0x80 | (cp & 63)); } }
  else { if(p + 3 < end) { *p++ = (char)(0xE0 | cp >> 12); *p++ = (char)(0x80 | ((cp >> 6) & 63)); *p++ = (char)(0x80 | (cp & 63)); } }
  *o = p;
}

/* copy the JSON string value starting after its opening quote */
static void
json_value(const char *s, char *out, size_t n) {
  char *o = out, *end = out + n;
  while(*s && *s != '"') {
    if(*s == '\\' && s[1]) {
      s++;
      if(*s == 'u' && s[1] && s[2] && s[3] && s[4]) {
        char h[5] = {s[1], s[2], s[3], s[4], 0};
        put_utf8(&o, end, (unsigned)strtoul(h, NULL, 16));
        s += 5;
        continue;
      }
      char c = *s == 'n' ? ' ' : *s == 't' ? ' ' : *s;
      if(o + 1 < end) *o++ = c;
      s++;
      continue;
    }
    if(o + 1 < end) *o++ = *s;
    s++;
  }
  *o = 0;
}

static const char *
json_key(const char *from, const char *key) {   /* -> first char of the value string */
  char pat[48];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(from, pat);
  if(!p) return NULL;
  p += strlen(pat);
  while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') p++;
  return *p == '"' ? p + 1 : NULL;
}

static int
name_from_json(const char *path, char *out, size_t n) {
  FILE *fp = fopen(path, "r");
  if(!fp) return 0;
  static char buf[65536];
  size_t len = fread(buf, 1, sizeof buf - 1, fp);
  fclose(fp);
  buf[len] = 0;
  const char *loc = strstr(buf, "\"localizedParameters\"");
  const char *base = loc ? loc : buf, *v;
  char lang[16] = "";
  if(loc && (v = json_key(loc, "defaultLanguage"))) json_value(v, lang, sizeof lang);
  if(lang[0]) {
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\"", lang);
    const char *blk = strstr(base + 1, pat);
    while(blk && blk[strlen(pat)] != ':' && blk[strlen(pat)] != ' ') blk = strstr(blk + 1, pat);   /* the object key, not the value */
    if(blk && (v = json_key(blk, "titleName"))) { json_value(v, out, n); if(out[0]) return 1; }
  }
  if((v = json_key(base, "titleName"))) { json_value(v, out, n); if(out[0]) return 1; }
  return 0;
}

static int
name_from_sfo(const char *path, char *out, size_t n) {
  FILE *fp = fopen(path, "rb");
  if(!fp) return 0;
  static unsigned char b[65536];
  size_t len = fread(b, 1, sizeof b, fp);
  fclose(fp);
  if(len < 20 || memcmp(b, "\0PSF", 4)) return 0;
  uint32_t keys = b[8] | b[9] << 8 | b[10] << 16 | (uint32_t)b[11] << 24;
  uint32_t data = b[12] | b[13] << 8 | b[14] << 16 | (uint32_t)b[15] << 24;
  uint32_t cnt = b[16] | b[17] << 8 | b[18] << 16 | (uint32_t)b[19] << 24;
  for(uint32_t i = 0; i < cnt && 20 + i * 16 + 16 <= len; i++) {
    unsigned char *e = b + 20 + i * 16;
    uint32_t ko = e[0] | e[1] << 8, dl = e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24;
    uint32_t doff = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
    if(keys + ko >= len || data + doff + dl > len) continue;
    if(strcmp((char *)b + keys + ko, "TITLE")) continue;
    size_t k = dl < n ? dl : n - 1;
    memcpy(out, b + data + doff, k);
    out[k] = 0;
    return out[0] != 0;
  }
  return 0;
}

static void
lookup_title_name(const char *id, char *out, size_t n, char *src, size_t sn) {
  static const char *dirs[] = {"/system_data/priv/appmeta", "/user/appmeta"};
  char p[160];
  out[0] = src[0] = 0;
  if(!id[0]) return;
  for(int d = 0; d < 2; d++) {
    snprintf(p, sizeof p, "%s/%s/param.json", dirs[d], id);
    if(name_from_json(p, out, n)) { snprintf(src, sn, "param.json"); return; }
    snprintf(p, sizeof p, "%s/%s/param.sfo", dirs[d], id);
    if(name_from_sfo(p, out, n)) { snprintf(src, sn, "param.sfo"); return; }
  }
}

static void
json_esc(char *out, size_t n, const char *s) {
  size_t j = 0;
  for(; *s && j + 7 < n; s++) {
    unsigned char c = (unsigned char)*s;
    if(c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = (char)c; }
    else if(c < 0x20) out[j++] = ' ';
    else out[j++] = (char)c;
  }
  out[j] = 0;
}

void
fps_set_more(int on) {
  if(on) {
    mkdir("/data/heatline", 0755);
    FILE *fp = fopen(MORE_FILE, "w");
    if(fp) { fputs("1\n", fp); fclose(fp); }
  } else {
    unlink(MORE_FILE);
  }
  pthread_mutex_lock(&g_lock);
  g_more = on ? 1 : 0;
  pthread_mutex_unlock(&g_lock);
}

static uint64_t
rd64(const uint8_t *p) {
  uint64_t v;
  memcpy(&v, p, sizeof v);
  return v;
}


static void *
sampler(void *arg) {
  char title[24];
  uint64_t prev_flips = 0, prev_flip_t = 0, next_probe = 0;   /* prev_flip_t: read that saw the last new frame */
  uint64_t prev_ts[N_TS_SRC] = {0};
  uint64_t win_start = 0, win_flips = 0, win_frames = 0;
  double win_time = 0, win_worst = 0;
  double rt_t = 0;                    /* real-time window clock: sum of frame times, µs */
  bool have_prev = false, in_game = false;
  int fails = 0;
  double hz = sceKernelGetTscFrequency ? (double)sceKernelGetTscFrequency() : 0;

  (void)arg;
  for(;;) {
    uint64_t t = now_us();

    if(t >= next_probe) {
      next_probe = t + 1000000;
      int g = game_probe(title, sizeof title);
      char nm[128] = "", ns[24] = "";
      int changed = g && (!in_game || strcmp(title, g_title));
      if(changed) lookup_title_name(title, nm, sizeof nm, ns, sizeof ns);   /* file read, outside the lock */
      pthread_mutex_lock(&g_lock);
      if(changed) {
        snprintf(g_title, sizeof g_title, "%s", title);
        snprintf(g_title_name, sizeof g_title_name, "%s", nm);
        snprintf(g_title_src, sizeof g_title_src, "%s", ns);
        session_clear_locked();
        have_prev = false;
        fails = 0;
      }
      if(!g) {
        g_title[0] = '\0';
        g_title_name[0] = g_title_src[0] = '\0';
        g_fps_now = -1;
        snprintf(g_status, sizeof g_status, "no game running");
      }
      g_game = g;
      in_game = g;
      pthread_mutex_unlock(&g_lock);
    }

    if(!in_game) {
      have_prev = false;
      usleep(200 * 1000);
      continue;
    }

    pthread_mutex_lock(&g_lock);
    if(g_reset_req) {
      g_reset_req = 0;
      session_clear_locked();
      have_prev = false;
    }
    pthread_mutex_unlock(&g_lock);

    uint8_t out[0x60];
    int rc = dce_read_raw(out);
    if(rc) {
      char s[96];
      snprintf(s, sizeof s, "/dev/dce unavailable (errno %d)", -rc);
      set_status(s);
      have_prev = false;
      usleep(++fails >= 5 ? 2 * 1000 * 1000 : 100 * 1000);   /* keep the log quiet */
      continue;
    }
    fails = 0;
    t = now_us();
    uint64_t flips = rd64(out + OFF_FLIPS);
    uint64_t ts[N_TS_SRC];
    for(int k = 0; k < N_TS_SRC; k++) ts[k] = rd64(out + TS_SRC[k].off);

    if(!have_prev || flips < prev_flips) {
      have_prev = true;
      win_start = t;
      win_flips = win_frames = 0;
      win_time = win_worst = 0;
      prev_flip_t = t;
      set_status("ok");
    } else if(flips > prev_flips) {
      uint64_t d = flips - prev_flips;
      double dt = -1;
      const char *src = "heatline";
      /* first console timestamp that moved forward by a plausible amount */
      for(int k = 0; k < N_TS_SRC && dt < 0; k++) {
        if(ts[k] <= prev_ts[k]) continue;
        double scale = TS_SRC[k].tsc ? (hz > 0 ? 1e6 / hz : -1) : 1.0;
        if(scale <= 0) continue;
        double v = (double)(ts[k] - prev_ts[k]) * scale;
        if(v > 0 && v <= (double)MAX_GAP_US * d) { dt = v; src = TS_SRC[k].name; }
      }
      bool estimated = dt < 0;
      if(estimated) dt = (double)(t - prev_flip_t);      /* own clock, ±POLL_US */
      win_flips += d;
      pthread_mutex_lock(&g_lock);
      g_clock = src;
      if(dt > 0 && dt <= (double)MAX_GAP_US * d) {        /* else: a pause */
        g_frames += d;
        g_time_us += dt;
        if(g_more > 0) {                                  /* only with "more metrics" on */
          double each = dt / (double)d;                   /* 2+ flips share the time */
          g_below_base_us += dt;
          for(int k = 0; k < N_TH; k++)
            if(each > 1e6 / FPS_TH[k]) { g_below_us[k] += dt; g_below_n[k] += d; }
        }
        win_frames += d;
        win_time += dt;
        if(d == 1) {                                      /* timed on its own */
          int bin = (int)(dt / FT_BIN_US);
          if(bin >= FT_BINS) bin = FT_BINS - 1;
          g_ft_hist[bin]++;
          g_measured++;
          g_meas_us += dt;
          if(estimated) g_estimated++;
          if(dt > win_worst) win_worst = dt;
        } else {
          g_merged += d;                                  /* no per-frame time */
        }
      }                                                   /* else: a pause */
      /* real-time window (CapFrameX rules): every frame, up to 2 s each; a
       * longer gap clears the window. 2+ flips between two reads share the
       * measured time equally (their sum stays exact). */
      if(dt > 0) {
        double each = dt / (double)d;
        if(each > RESET_GAP_US) {
          g_ring_head = g_ring_len = 0;
          g_new_frames = 0;
        } else {
          for(uint64_t k = 0; k < d; k++) {
            rt_t += each;
            ring_push_locked(each, rt_t, d > 1);
          }
        }
      }
      pthread_mutex_unlock(&g_lock);
    }
    if(flips != prev_flips) prev_flip_t = t;
    prev_flips = flips;
    for(int k = 0; k < N_TS_SRC; k++) prev_ts[k] = ts[k];

    if(t - win_start >= 1000000) {                        /* 1 s summary */
      pthread_mutex_lock(&g_lock);
      /* FPS of the second = frames / their own duration on the console's
       * clock. Counting flips between two 4 ms reads could catch one frame
       * too many (121 on a 120 Hz screen); the console timestamps cannot.
       * If the second held a pause (> 250 ms frame, not timed), fall back to
       * flips / elapsed so the pause still shows as a low value. */
      if(win_frames && win_time >= 500000.0)
        g_fps_now = (double)win_frames * 1e6 / win_time;
      else
        g_fps_now = (double)win_flips * 1e6 / (double)(t - win_start);
      g_ft_avg = win_frames ? win_time / win_frames / 1000.0 : -1;
      g_ft_worst = win_worst > 0 ? win_worst / 1000.0 : -1;
      rt_update_locked();
      if(win_frames && win_time / win_frames <= MAX_GAP_US) {
        if(g_min_fps < 0 || g_fps_now < g_min_fps) g_min_fps = g_fps_now;
        if(g_fps_now > g_max_fps) g_max_fps = g_fps_now;
      }
      g_seq++;
      pthread_mutex_unlock(&g_lock);
      win_start = t;
      win_flips = win_frames = 0;
      win_time = win_worst = 0;
    }

    usleep(POLL_US);
  }
  return NULL;
}


/* Time-based "x% low", as in CapFrameX (since 1.5.3) and MSI Afterburner:
 * walk the frames from the slowest, adding their durations until the sum
 * reaches `frac` of the total time; the FPS of the frame that reaches it is
 * the result ("above X FPS for 99% of the time"). Caller holds the lock. */
static double
low_time_locked(double frac, uint64_t min_frames) {
  if(g_measured < min_frames || g_meas_us <= 0) return -1;
  double target = g_meas_us * frac, sum = 0;
  for(int b = FT_BINS - 1; b >= 0; b--) {
    if(!g_ft_hist[b]) continue;
    double ft = (double)b * FT_BIN_US + FT_BIN_US / 2.0;   /* bin centre, µs */
    double need = (target - sum) / ft;                     /* frames still needed */
    if((double)g_ft_hist[b] >= need) return 1e6 / ft;
    sum += (double)g_ft_hist[b] * ft;
  }
  return -1;
}


int
fps_json(char *buf, size_t size) {
  int n;
  pthread_mutex_lock(&g_lock);
  if(!g_game) {
    n = jcat(buf, size, 0, "\"game\":{\"running\":false,\"more_on\":%s,\"status\":\"%s\"}",
                 g_more > 0 ? "true" : "false", g_status);
  } else {
    double avg = g_time_us > 0 ? (double)g_frames * 1e6 / g_time_us : -1;
    char tn_esc[260];
    json_esc(tn_esc, sizeof tn_esc, g_title_name);
    char below[640];
    int bn = snprintf(below, sizeof below, "\"below\":{\"fps\":[");
    for(int k = 0; k < N_TH; k++) bn = jcat(below, sizeof below, bn, "%s%.0f", k ? "," : "", FPS_TH[k]);
    bn = jcat(below, sizeof below, bn, "],\"ms\":[");
    for(int k = 0; k < N_TH; k++) bn = jcat(below, sizeof below, bn, "%s%.1f", k ? "," : "", g_below_us[k] / 1000.0);
    bn = jcat(below, sizeof below, bn, "],\"n\":[");
    for(int k = 0; k < N_TH; k++) bn = jcat(below, sizeof below, bn, "%s%llu", k ? "," : "", (unsigned long long)g_below_n[k]);
    snprintf(below + bn, sizeof below - bn, "],\"time_ms\":%.1f}", g_below_base_us / 1000.0);
    n = jcat(buf, size, 0,
                 "\"game\":{\"running\":true,\"title\":\"%s\",\"title_name\":\"%s\",\"title_name_src\":\"%s\",\"status\":\"%s\","
                 "\"seq\":%llu,\"fps\":%.1f,\"avg\":%.1f,\"ft_ms\":%.2f,\"ft_worst_ms\":%.2f,"
                 "\"min\":%.1f,\"max\":%.1f,\"low1\":%.1f,\"low01\":%.1f,"
                 "\"rt\":{\"win_s\":%.0f,\"avg\":%.1f,\"p1\":%.1f,\"p02\":%.1f,\"low1\":%.1f,\"low02\":%.1f,"
                 "\"stut_pct\":%.3f,\"stut_win_s\":%.0f,\"stut_factor\":%.1f,\"frames\":%llu,\"split_total\":%llu,"
                 "\"p5\":%.1f,\"p50\":%.1f,\"p95\":%.1f,\"p99\":%.1f,\"p01\":%.1f,\"low01\":%.1f,"
                 "\"il1\":%.1f,\"il02\":%.1f,\"il01\":%.1f,\"ft_min\":%.3f,\"ft_max\":%.3f,\"astd\":%.1f,\"more_on\":%s,"
                 "\"stut_n\":%llu,\"stut_frames\":%llu},"
                 "\"stut_session\":%llu,\"ring_seq\":%llu,%s,"
                 "\"frames\":%llu,\"measured\":%llu,\"merged\":%llu,\"estimated\":%llu,\"secs\":%.1f,\"clock\":\"%s\"}",
                 g_title, tn_esc, g_title_src, g_status, (unsigned long long)g_seq, g_fps_now, avg, g_ft_avg, g_ft_worst,
                 g_min_fps, g_max_fps, low_time_locked(0.01, 100), low_time_locked(0.001, 1000),
                 RT_WIN_S, g_rt_avg, g_rt_p1, g_rt_p02, g_rt_low1, g_rt_low02,
                 g_rt_stut, STUT_WIN_S, STUT_FACTOR, (unsigned long long)g_rt_frames, (unsigned long long)g_split,
                 g_x_p5, g_x_p50, g_x_p95, g_x_p99, g_x_p01, g_x_low01,
                 g_x_il1, g_x_il02, g_x_il01, g_x_ftmin, g_x_ftmax, g_x_astd, g_more > 0 ? "true" : "false",
                 (unsigned long long)g_rt_stut_n, (unsigned long long)g_rt_stut_frames,
                 (unsigned long long)g_stut_sess, (unsigned long long)g_ring_seq, below,
                 (unsigned long long)g_frames, (unsigned long long)g_measured,
                 (unsigned long long)g_merged, (unsigned long long)g_estimated, g_time_us / 1e6, g_clock);
  }
  pthread_mutex_unlock(&g_lock);
  return n;
}


/* Frame times for the live graph: the frames pushed after cursor `since`
 * (newest `max` at most), oldest first, ms. Same values as the real-time
 * window (2+ flips between two reads share their time). */
/* the running game for other modules: "" when none. Name JSON-escaped. */
void
fps_game(char *id, size_t idn, char *name, size_t namen) {
  pthread_mutex_lock(&g_lock);
  snprintf(id, idn, "%s", g_game ? g_title : "");
  json_esc(name, namen, g_game ? g_title_name : "");
  pthread_mutex_unlock(&g_lock);
}

int
fps_ft_json(char *buf, size_t size, unsigned long long since) {
  int n;
  pthread_mutex_lock(&g_lock);
  uint64_t seq = g_ring_seq, avail = seq > since ? seq - since : 0;
  int lost = avail > g_ring_len;
  if(avail > g_ring_len) avail = g_ring_len;
  uint64_t max = (size - 96) / 8;                    /* "123.45," worst case + header */
  if(max > 1500) max = 1500;
  if(avail > max) { avail = max; lost = 1; }
  if(!g_game) avail = 0;
  n = jcat(buf, size, 0, "{\"running\":%s,\"seq\":%llu,\"lost\":%s,\"ft\":[",
               g_game ? "true" : "false", (unsigned long long)seq, lost ? "true" : "false");
  for(uint64_t k = avail; k > 0 && n < (int)size - 16; k--) {
    uint32_t i = (g_ring_head + RING - (uint32_t)k) % RING;
    n = jcat(buf, size, n, "%s%.2f", k == avail ? "" : ",", g_ring_ft[i] / 1000.0);
  }
  n = jcat(buf, size, n, "]}");
  pthread_mutex_unlock(&g_lock);
  return n < (int)size ? n : (int)size - 1;
}


void
fps_reset(void) {
  pthread_mutex_lock(&g_lock);
  g_reset_req = 1;
  pthread_mutex_unlock(&g_lock);
}


int
fps_start(void) {
  pthread_t th;
  g_more = access(MORE_FILE, F_OK) == 0;   /* "more metrics" off unless turned on before */
  if(escalate()) {
    klog_puts("[heatline] privilege escalation partly failed");
  }
  if(pthread_create(&th, NULL, sampler, NULL)) {
    set_status("could not start FPS thread");
    return -1;
  }
  pthread_detach(th);
  return 0;
}

