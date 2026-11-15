// SPDX-License-Identifier: GPL-3.0-only
#include "app.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#define TITLE_ID "HTLN00001"
#define APP_DIR  "/user/app/" TITLE_ID
#define SYS_DIR  APP_DIR "/sce_sys"

/* Embed assets/param.json and assets/icon0.png in the ELF. */
#define INCASSET(name, file)                                                   \
  __asm__(".section .rodata\n"                                                 \
          ".global " #name "\n"                                                \
          ".global " #name "_size\n"                                           \
          ".align 16\n" #name ":\n"                                            \
          ".incbin \"" file "\"\n" #name "_end:\n"                             \
          ".align 8\n" #name "_size:\n"                                        \
          ".quad " #name "_end - " #name "\n"                                  \
          ".previous\n");                                                      \
  extern const uint8_t name[];                                                 \
  extern const size_t name##_size;

INCASSET(app_param_json, "assets/param.json");
INCASSET(app_icon0_png, "assets/icon0.png");

void notify(const char *msg);        /* main.c */

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *);


static int
mkdir_p(const char *path) {
  char tmp[256];
  snprintf(tmp, sizeof tmp, "%s", path);
  for(char *p = tmp + 1; *p; p++) {
    if(*p == '/') {
      *p = '\0';
      if(mkdir(tmp, 0755) && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  if(mkdir(tmp, 0755) && errno != EEXIST) return -1;
  return 0;
}


static int
write_file(const char *path, const uint8_t *data, size_t size) {
  FILE *f = fopen(path, "w");
  if(!f) return -1;
  int ok = fwrite(data, size, 1, f) == 1;
  if(fclose(f)) ok = 0;
  return ok ? 0 : -1;
}


/* 1 if the file is missing or differs from `data`. */
static int
differs(const char *path, const uint8_t *data, size_t size) {
  struct stat st;
  if(stat(path, &st) || (size_t)st.st_size != size) return 1;
  FILE *f = fopen(path, "r");
  if(!f) return 1;
  uint8_t *buf = malloc(size);
  int diff = 1;
  if(buf && fread(buf, 1, size, f) == size) diff = memcmp(buf, data, size) != 0;
  free(buf);
  fclose(f);
  return diff;
}


static int
register_app(void) {
  int (*install_title_dir)(const char *, const char *, void *) = 0;
  uint32_t handle;
  if(!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
    install_title_dir = (void *)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
  }
  if(install_title_dir) {
    return install_title_dir(TITLE_ID, "/user/app/", 0);
  }
  return sceAppInstUtilAppInstallAll(0);
}


int
app_install_if_needed(void) {
  struct stat st;
  int exists = stat(APP_DIR, &st) == 0;

  if(exists &&
     !differs(SYS_DIR "/param.json", app_param_json, app_param_json_size) &&
     !differs(SYS_DIR "/icon0.png", app_icon0_png, app_icon0_png_size)) {
    return 0;                                  /* already installed, same files */
  }

  int err = sceAppInstUtilInitialize();
  if(err) {
    klog_printf("[heatline] sceAppInstUtilInitialize: 0x%08X\n", err);
    return -1;
  }
  if(mkdir_p(SYS_DIR) ||
     write_file(SYS_DIR "/param.json", app_param_json, app_param_json_size) ||
     write_file(SYS_DIR "/icon0.png", app_icon0_png, app_icon0_png_size)) {
    klog_printf("[heatline] could not write %s (errno %d)\n", SYS_DIR, errno);
    sceAppInstUtilTerminate();
    return -1;
  }
  err = register_app();
  sceAppInstUtilTerminate();
  if(err) {
    klog_printf("[heatline] app install: 0x%08X\n", err);
    return -1;
  }
  notify(exists ? "Heatline tile updated" : "Heatline tile added to the home screen");
  return 0;
}
