// SPDX-License-Identifier: GPL-3.0-only
/* osd.c - in-game overlay (draws metrics over the game itself).
 *
 * The drawing is done by the vendored SimpleFPS / Common-FPS engine, built as
 * libheatline_overlay.a and driven through osd_overlay.h. That engine finds
 * SceShellUI (whose layer the console composites over the game, the same path
 * trophy pop-ups take), injects a small Mono renderer into it, and draws the
 * FPS there. On FW 11.xx-13.xx it reaches ShellUI through ptrace + kernel R/W.
 *
 * This file owns the Heatline side: the saved on/off flag, the background
 * thread, starting/stopping the engine from the dashboard toggle, a read-only
 * ShellUI probe for the dashboard, and the "overlay" object in /api.
 *
 *   WARNING: the engine writes into the home-screen process. A fault there
 *   crashes the home screen, not the game. Heatline is otherwise read-only;
 *   this feature is opt-in for that reason, and a restart removes the overlay.
 */
#include "osd.h"
#include "osd_overlay.h"
#include "fps.h"
#include "jbuf.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

#include <ps5/mdbg.h>
#include <ps5/kernel.h>

#define ON_FILE   "/data/heatline/fps_overlay"
#define LAYOUT_FILE "/data/heatline/overlay_layout"   /* the saved layout text */
#define STYLE_FILE  "/data/heatline/overlay_style"    /* corner margin_x margin_y font opacity */
#define LAYOUT_MAX  96                 /* longest accepted layout text */
#define SHELL_NAME "SceShellUI"
#define TICK_US   (1000 * 1000)        /* the overlay updates once a second */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int   g_on = -1;                /* -1 = flag not read yet */
static char  g_status[64] = "off";     /* short human status shown on the dashboard */
static int   g_shown_fps = -1;         /* last FPS pushed to screen, -1 = nothing shown */

/* Read-only ShellUI probe results (exposed in /api as "probe"). This is the
 * safe test of the MDBG read path: it reads ShellUI's main-module header and
 * reports what it saw. It NEVER writes. Nothing here patches anything. */
static pid_t    g_probe_pid  = -1;     /* ShellUI pid last seen, -1 = none */
static intptr_t g_probe_base = 0;      /* ShellUI main-module base, 0 = not resolved */
static int      g_probe_read = -1;     /* -1 not tried, 0 read failed, 1 read ok */
static int      g_probe_elf  = 0;      /* 1 if the first bytes are an ELF header */

/* thread-only state (touched by the osd thread) */
static int   t_engine_started;         /* overlay engine worker launched once */

static void
set_status(const char *s) {
  pthread_mutex_lock(&g_lock);
  snprintf(g_status, sizeof g_status, "%s", s);
  pthread_mutex_unlock(&g_lock);
}

static void
set_shown(int fps) {
  pthread_mutex_lock(&g_lock);
  g_shown_fps = fps;
  pthread_mutex_unlock(&g_lock);
}

/* Find the SceShellUI process id via the kernel's own process list (sysctl),
 * so there are no firmware-specific struct offsets here. Returns the pid or
 * -1. ShellUI is restarted by the system from time to time; the caller
 * re-checks and rebinds when the pid changes. */
static pid_t
find_shell(void) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t len = 0;
  if(sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || len == 0) return -1;
  len += len / 8 + 4096;                 /* headroom: the list can grow between the two calls */
  char *buf = malloc(len);
  if(!buf) return -1;
  pid_t found = -1;
  if(sysctl(mib, 4, buf, &len, NULL, 0) == 0) {
    char *p = buf, *end = buf + len;
    while(p + sizeof(struct kinfo_proc) <= end) {
      struct kinfo_proc *kp = (struct kinfo_proc *)p;
      int sz = kp->ki_structsize > 0 ? kp->ki_structsize : (int)sizeof *kp;
      if(kp->ki_pid > 0 && !strncmp(kp->ki_comm, SHELL_NAME, sizeof kp->ki_comm)) {
        found = kp->ki_pid;
        break;
      }
      p += sz;
    }
  }
  free(buf);
  return found;
}

/* READ-ONLY probe: resolve ShellUI's main-module base and read its header
 * over MDBG. Proves the debug read path works on this console before anything
 * is ever written. Writes nothing. Records results for /api. */
static void
probe_shell(pid_t pid) {
  intptr_t base = kernel_dynlib_mapbase_addr(pid, 0);   /* handle 0 = main module */
  int read_ok = 0, is_elf = 0;
  if(base) {
    unsigned char hdr[4] = {0};
    if(mdbg_copyout(pid, base, hdr, sizeof hdr) == 0) {
      read_ok = 1;
      is_elf = (hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F');
    }
  }
  pthread_mutex_lock(&g_lock);
  g_probe_pid = pid;
  g_probe_base = base;
  g_probe_read = base ? read_ok : -1;
  g_probe_elf = is_elf;
  pthread_mutex_unlock(&g_lock);
}

/* One cycle of the thread while the overlay is switched on. The real drawing
 * is done by the vendored engine (libheatline_overlay.a): overlay_start()
 * launches its worker, which finds ShellUI, injects the Mono renderer once a
 * game has been in front for a few seconds, and draws the FPS over the game. Here
 * we only kick it off once and keep the dashboard's read-only probe fresh. */
static void
tick_on(void) {
  if(!t_engine_started) {
    t_engine_started = overlay_start() == 0 ? 1 : -1;
  }

  pid_t shell = find_shell();
  if(shell > 0) probe_shell(shell);       /* read-only diagnostics for the dashboard */

  if(t_engine_started == 1) {
    int fps = 0;
    if(fps_now_value(&fps)) {
      char s[64];
      snprintf(s, sizeof s, "overlay engine running - %d FPS", fps);
      set_status(s);
      set_shown(fps);
    } else {
      set_status(shell > 0 ? "overlay engine running (no game in front)"
                           : "overlay engine running (home screen not found)");
      set_shown(-1);
    }
  } else {
    set_status("overlay engine failed to start");
    set_shown(-1);
  }
}

static void *
thread_main(void *arg) {
  (void)arg;
  for(;;) {
    int on;
    pthread_mutex_lock(&g_lock);
    on = g_on == 1;
    pthread_mutex_unlock(&g_lock);

    if(on) {
      tick_on();
    } else {
      /* The engine worker has no clean stop (SimpleFPS parity: a restart
       * removes the overlay). overlay_stop() is best effort; the saved flag
       * keeps the overlay from starting on the next launch. */
      if(t_engine_started == 1) overlay_stop();
      set_status("off");
      set_shown(-1);
    }
    usleep(TICK_US);
  }
  return NULL;
}

/* Layout text allowed characters: metric names (letters, digits, '.') plus the
 * separators. Anything else is refused before it reaches the engine or a file. */
static int
layout_text_ok(const char *s) {
  size_t n = strlen(s);
  if(n == 0 || n > LAYOUT_MAX) return 0;
  for(size_t i = 0; i < n; i++) {
    char c = s[i];
    if(!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
         || c == '.' || c == ',' || c == ';' || c == ' ')) return 0;
  }
  return 1;
}

int
osd_set_layout(const char *spec) {
  if(!spec || !layout_text_ok(spec)) return -1;
  if(overlay_set_layout(spec) != 0) return -1;   /* no known metric in it */
  char canon[LAYOUT_MAX + 8];
  if(overlay_get_layout(canon, sizeof canon) <= 0) return -1;
  mkdir("/data/heatline", 0755);
  FILE *fp = fopen(LAYOUT_FILE, "w");
  if(fp) { fputs(canon, fp); fputc('\n', fp); fclose(fp); }
  return 0;
}

int
osd_set_style(int corner, int mx, int my, int font, int opacity) {
  if(overlay_set_style(corner, mx, my, font, opacity) != 0) return -1;
  int st[5];
  overlay_get_style(st);                 /* the clamped values actually in force */
  mkdir("/data/heatline", 0755);
  FILE *fp = fopen(STYLE_FILE, "w");
  if(fp) { fprintf(fp, "%d %d %d %d %d\n", st[0], st[1], st[2], st[3], st[4]); fclose(fp); }
  return 0;
}

/* Apply the position/size saved by an earlier session, if any. */
static void
style_load(void) {
  FILE *fp = fopen(STYLE_FILE, "r");
  if(!fp) return;
  int c, mx, my, f, o;
  if(fscanf(fp, "%d %d %d %d %d", &c, &mx, &my, &f, &o) == 5) overlay_set_style(c, mx, my, f, o);
  fclose(fp);
}

/* Apply the layout saved by an earlier session, if any. */
static void
layout_load(void) {
  FILE *fp = fopen(LAYOUT_FILE, "r");
  if(!fp) return;
  char buf[LAYOUT_MAX + 8] = "";
  if(fgets(buf, sizeof buf, fp)) {
    buf[strcspn(buf, "\r\n")] = '\0';
    if(layout_text_ok(buf)) overlay_set_layout(buf);
  }
  fclose(fp);
}

int
osd_get(void) {
  pthread_mutex_lock(&g_lock);
  if(g_on < 0) {                           /* read the saved flag once */
    struct stat st;
    g_on = stat(ON_FILE, &st) == 0 ? 1 : 0;
  }
  int on = g_on;
  pthread_mutex_unlock(&g_lock);
  return on;
}

void
osd_set(int on) {
  if(on) {
    mkdir("/data/heatline", 0755);
    FILE *fp = fopen(ON_FILE, "w");
    if(fp) { fputs("1\n", fp); fclose(fp); }
  } else {
    unlink(ON_FILE);
  }
  pthread_mutex_lock(&g_lock);
  g_on = on ? 1 : 0;
  if(!on) snprintf(g_status, sizeof g_status, "off");
  pthread_mutex_unlock(&g_lock);
}

int
osd_start(void) {
  /* SAFE BUILD: the overlay is session-only. It is ALWAYS forced off at
   * startup, whatever was saved, so loading (or reloading) Heatline never
   * re-triggers the ShellUI injection on its own. The overlay runs only after
   * an explicit toggle during this session. This is deliberate: the engine
   * injects into the home screen, and an automatic retry after a crash could
   * hang the console again. */
  unlink(ON_FILE);
  pthread_mutex_lock(&g_lock);
  g_on = 0;
  snprintf(g_status, sizeof g_status, "off");
  pthread_mutex_unlock(&g_lock);
  layout_load();                         /* the layout and style are kept across restarts */
  style_load();

  pthread_t th;
  if(pthread_create(&th, NULL, thread_main, NULL) != 0) return -1;
  pthread_detach(th);
  return 0;
}

/* Read-only: concatenate the overlay engine's /data logs into buf. Temporary
 * diagnostic for the background-box class probe. */
int
osd_log_dump(char *buf, size_t size) {
  static const char *const files[] = {
    "/data/CommonFPS_v1_2_1_shellui.log",
    "/data/CommonFPS_v1_2_1.log",
    NULL,
  };
  int n = 0;
  for(int i = 0; files[i] && n < (int)size - 64; i++) {
    FILE *fp = fopen(files[i], "r");
    n += snprintf(buf + n, size - n, "===== %s =====\n", files[i]);
    if(!fp) { n += snprintf(buf + n, size - n, "(not present)\n\n"); continue; }
    size_t got = fread(buf + n, 1, size - n - 2, fp);
    fclose(fp);
    n += (int)got;
    n += snprintf(buf + n, size - n, "\n\n");
  }
  return n;
}

int
osd_json(char *buf, size_t size) {
  int on = osd_get();
  char status[64];
  int shown;
  pid_t ppid;
  intptr_t pbase;
  int pread, pelf;
  pthread_mutex_lock(&g_lock);
  snprintf(status, sizeof status, "%s", g_status);
  shown = g_shown_fps;
  ppid = g_probe_pid;
  pbase = g_probe_base;
  pread = g_probe_read;
  pelf = g_probe_elf;
  pthread_mutex_unlock(&g_lock);

  int n = jcat(buf, size, 0, "\"overlay\":{\"on\":%s,\"configured\":true,\"status\":\"",
               on ? "true" : "false");
  /* status is from a fixed set of our own strings, but escape defensively */
  for(const char *p = status; *p && n < (int)size - 2; p++)
    n = jcat(buf, size, n, "%c", (*p == '"' || *p == '\\') ? ' ' : *p);
  if(shown >= 0) n = jcat(buf, size, n, "\",\"fps\":%d,", shown);
  else           n = jcat(buf, size, n, "\",\"fps\":null,");

  /* the current layout text: only A-Z 0-9 . , ; (canonical form), no escaping */
  char layout[LAYOUT_MAX + 8] = "";
  overlay_get_layout(layout, sizeof layout);
  n = jcat(buf, size, n, "\"layout\":\"%s\",", layout);

  int st[5];
  overlay_get_style(st);
  n = jcat(buf, size, n, "\"style\":{\"corner\":%d,\"mx\":%d,\"my\":%d,\"font\":%d,\"opacity\":%d},",
           st[0], st[1], st[2], st[3], st[4]);

  /* read-only probe of ShellUI (foundation test for the MDBG path) */
  n = jcat(buf, size, n, "\"probe\":{\"shell_pid\":%d,", (int)ppid);
  if(pbase) n = jcat(buf, size, n, "\"module_base\":\"0x%llx\",", (unsigned long long)pbase);
  else      n = jcat(buf, size, n, "\"module_base\":null,");
  n = jcat(buf, size, n, "\"read\":%s,\"elf\":%s}},",
           pread == 1 ? "\"ok\"" : pread == 0 ? "\"fail\"" : "null",
           pelf ? "true" : "false");
  return n;
}
