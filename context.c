// SPDX-License-Identifier: GPL-3.0-only
/* Foreground context: is a game in front (GAME) or the PS5 system (OS)?
 *
 * Method from PS5 Monitor (monitor_perf.c): the kernel message buffer
 * (sysctl kern.msgbuf) holds the shell's log lines. The last
 * "SetControllerFocus(<appid>)" says which app owns the controller;
 * "OnFocusActiveSceneChanged ... -> [scene]" names the shell scene in front.
 *  - focus == the running game's app id          -> GAME
 *  - focus == 7 (the home screen, SceShellUI)      -> OS
 *  - scene "AppScreen" / "ApplicationScreenScene"  -> GAME
 *  - scene NPXS40002 / NPXS40003 / ReactModalScene / FocusCapture -> OS
 *  - no game running at all                        -> OS
 *  - otherwise                                     -> unknown
 * The game's app id comes from sceSystemServiceGetAppIdOfRunningBigApp
 * (the same id the FPS code uses). Read at most once per second.
 */
#include "context.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include "jbuf.h"

int sceSystemServiceGetAppIdOfRunningBigApp(void) __attribute__((weak));

#define SHELL_HOME_APP_ID 0x7u
#define MSGBUF_MAX (512u * 1024u)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t g_last;
static char   g_json[256] = "\"context\":{\"value\":\"unknown\"},";

static void
parse(const char *buf, size_t len, long *focus, char scene[96]) {
  const char *cur = buf, *end = buf + len;
  *focus = -1;
  scene[0] = '\0';
  while(cur < end && *cur) {
    const char *nl = memchr(cur, '\n', (size_t)(end - cur));
    size_t ln = nl ? (size_t)(nl - cur) : strnlen(cur, (size_t)(end - cur));
    char line[512];
    if(ln >= sizeof line) ln = sizeof line - 1;
    memcpy(line, cur, ln);
    line[ln] = '\0';

    const char *p = strstr(line, "SetControllerFocus(");
    if(p) {
      p += strlen("SetControllerFocus(");
      if(*p != '-') {
        char *e = NULL;
        unsigned long v = strtoul(p, &e, 0);
        if(e != p) *focus = (long)v;
      }
    }
    if(strstr(line, "OnFocusActiveSceneChanged")) {
      const char *a = strstr(line, "-> [");
      if(a) {
        a += 4;
        const char *b = strchr(a, ']');
        size_t n = b ? (size_t)(b - a) : strlen(a);
        if(n > 95) n = 95;
        memcpy(scene, a, n);
        scene[n] = '\0';
      }
    }
    if(!nl) break;
    cur = nl + 1;
  }
}

static void
clean(char *s) {
  for(; *s; s++) if(*s == '"' || *s == '\\' || (unsigned char)*s < 0x20) *s = ' ';
}

static void
refresh(void) {
  int appid = sceSystemServiceGetAppIdOfRunningBigApp ? sceSystemServiceGetAppIdOfRunningBigApp() : -1;
  long focus = -1;
  char scene[96] = "";
  const char *value = "unknown";

  size_t size = 0;
  char *buf = NULL;
  if(sysctlbyname("kern.msgbuf", NULL, &size, NULL, 0) != 0 || size == 0) size = 256u * 1024u;
  if(size > MSGBUF_MAX) size = MSGBUF_MAX;
  if((buf = calloc(1, size + 1))) {
    if(sysctlbyname("kern.msgbuf", buf, &size, NULL, 0) == 0) {
      buf[size] = '\0';
      parse(buf, size, &focus, scene);
    }
    free(buf);
  }

  bool game = appid > 0;
  bool app_scene = strstr(scene, "AppScreen") || strstr(scene, "ApplicationScreenScene");
  bool os_scene = strstr(scene, "NPXS40002") || strstr(scene, "NPXS40003") ||
                  strstr(scene, "ReactModalScene") || strstr(scene, "FocusCapture");
  if(!game) value = "OS";
  else if(focus >= 0 && (unsigned long)focus == (unsigned long)appid) value = "GAME";
  else if(focus == (long)SHELL_HOME_APP_ID) value = "OS";
  else if(app_scene) value = "GAME";
  else if(os_scene) value = "OS";

  clean(scene);
  pthread_mutex_lock(&g_lock);
  snprintf(g_json, sizeof g_json,
           "\"context\":{\"value\":\"%s\",\"game_appid\":%d,\"focus\":%ld,\"scene\":\"%s\"},",
           value, appid, focus, scene);
  pthread_mutex_unlock(&g_lock);
}

int
context_json(char *buf, size_t size) {
  time_t now = time(NULL);
  if(now != g_last) {                      /* at most once per second */
    g_last = now;
    refresh();
  }
  pthread_mutex_lock(&g_lock);
  int n = jcat(buf, size, 0, "%s", g_json);
  pthread_mutex_unlock(&g_lock);
  return n;
}
