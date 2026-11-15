// SPDX-License-Identifier: GPL-3.0-only
/* display.c - display output mode (resolution / refresh), test stage.
 *
 * Source: sceVideoOutGetCurrentOutputMode_ in libSceVideoOut. Reported to
 * take a 64-bit bus id (0x7000 = the main output, "AvSetting" bus) and to
 * write a 32-byte record with the resolution and refresh rate (AnyPS5 PRs
 * #7259 / #16745, "PS5 React" on FW 13.60). The record layout is NOT
 * published, so this stage only reads the raw bytes, to be decoded against
 * what the console's settings show.
 *
 * Decoded on a CFI-2016, FW 13.60, by switching the console's settings
 * (Video Output) and reading again:
 *   u32 [0]   32 = size of the record
 *   byte 7    bits per pixel: 24 = 8 bits per colour, 36 = 12 bits
 *   byte 8    refresh rate code: 3 = 59.94 Hz, 13 = 119.88 Hz (same as the
 *             PS4 codes), 1 = 60 Hz with VRR (VRR Automatic, 120 Hz off, game
 *             at 60 Hz; the PS4 table's 23.98 Hz does not apply). All measured.
 *   byte 9    0x80 = VRR (measured with 120 Hz on and with it off)
 *   byte 16   resolution: 9 = 720p, 11 = 1080p, 15 = 1440p (measured),
 *             12 = 2160p (guess: no 4K screen to check)
 *   byte 4    colour format: 1 = RGB, 3 = YCbCr 4:2:2 (monitor: YCbCr422)
 *   byte 5    range: 1 = Limited, 2 = Full (YCbCr is always limited)
 *   byte 6    HDR: 2 = off (SDR), 13 = on with RGB, 14 = on with YCbCr 4:2:2
 *             (monitor: EOTF HDR-ST2084)
 *   Seen combinations: 1440p HDR 120 Hz -> YCbCr 4:2:2 (14); 1440p HDR 60 Hz,
 *   1080p HDR 120 Hz -> RGB (13). The console's HDMI info for this monitor
 *   lists HDR at 1440p as "60 Hz, 120 Hz (YUV422)", so 4:2:2 is its choice.
 *   byte 7    is the depth actually sent: Deep Color Output Off -> 24,
 *             Automatic -> 36 at 60 Hz (24 at 1440p 120 Hz)
 * Codes not listed are reported as unknown (JSON "unknown"), so new games,
 * screens and modes can be decoded from their raw answer.
 *
 * Safety: nothing runs at start. Only on request (/display/read): the library
 * is loaded into this process (no video handle is opened, no buffer, nothing
 * is set), the function looked up, and each call made under the fault guard. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "display.h"
#include "sysinfo.h"
#include "fps.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ps5/kernel.h>
#include <ps5/nid.h>
#include "jbuf.h"

int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags, void *opt, int *res);
int sceKernelDlsym(int handle, const char *sym, void **addr);

typedef int (*get_mode_fn)(uint64_t bus, void *out);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_json[1536] = "\"state\":\"never\"";
static int g_handle = -1;
static get_mode_fn g_fn;
static const char *g_how = "";
static int g_failed;                 /* load or lookup failed: do not try again */

struct load_ctx { const char *path; int handle; };
static int do_load(void *c) {
  struct load_ctx *l = c;
  int res = 0;
  l->handle = sceKernelLoadStartModule(l->path, 0, NULL, 0, NULL, &res);
  return 0;
}
/* lookup: the normal one first, then the SDK's kernel-side helpers by name
 * and by NID (as ps5-exporter does); the normal one answered 0x80020003
 * (not found) on FW 13.60 although the name hashes to the reported NID
 * W0iQAbGaTBg */
#define SYM "sceVideoOutGetCurrentOutputMode_"
struct sym_ctx { int handle; void *addr; int rc; int how; };
static int do_sym(void *c) {
  struct sym_ctx *s = c;
  s->rc = sceKernelDlsym(s->handle, SYM, &s->addr);
  if(s->rc == 0 && s->addr) { s->how = 1; return 0; }
  intptr_t a = kernel_dynlib_dlsym(getpid(), (uint32_t)s->handle, SYM);
  if(a) { s->addr = (void *)a; s->how = 2; return 0; }
  char nid[12];
  nid_encode(SYM, nid);
  a = kernel_dynlib_resolve(getpid(), (uint32_t)s->handle, nid);
  if(a) { s->addr = (void *)a; s->how = 3; return 0; }
  return 0;
}
struct call_ctx { uint64_t bus; int rc; uint8_t out[64]; };
static int do_call(void *c) {
  struct call_ctx *k = c;
  k->rc = g_fn(k->bus, k->out);
  return 0;
}

static int decode(char *out, int size, const uint8_t *b, int *nunk);
static void log_unknown(const char *dec, const char *hex);

static int hex(char *dst, int size, const uint8_t *b, int n) {
  int o = 0;
  for(int i = 0; i < n && o < size - 3; i++) o = jcat(dst, size, o, "%02x", b[i]);
  return o;
}

static int one_call(char *out, int size, uint64_t bus) {
  struct call_ctx k;
  memset(&k, 0, sizeof k);
  k.bus = bus;
  if(sysinfo_guarded_call(do_call, &k))
    return jcat(out, size, 0, "{\"bus\":\"0x%llx\",\"crashed\":true}", (unsigned long long)bus);
  char h[160];
  hex(h, sizeof h, k.out, 32);
  int changed = 0;                                  /* bytes past 32 written? */
  for(int i = 32; i < 64; i++) if(k.out[i]) changed = 1;
  uint32_t w[8];
  memcpy(w, k.out, sizeof w);
  char dec[300] = "";
  int nunk = 0;
  if(k.rc == 0 && k.out[0] == 32) decode(dec, sizeof dec, k.out, &nunk);
  if(nunk) log_unknown(dec, h);              /* a code not mapped yet: keep it */
  return jcat(out, size, 0, "{%s\"bus\":\"0x%llx\",\"rc\":\"0x%08x\",\"hex\":\"%s\",\"u32\":[%u,%u,%u,%u,%u,%u,%u,%u],\"wrote_past_32\":%s}",
                  dec, (unsigned long long)bus, (unsigned)k.rc, h, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                  changed ? "true" : "false");
}

struct res { int code, w, h, measured; };
static const struct res RES[] = {{9, 1280, 720, 1}, {11, 1920, 1080, 1}, {15, 2560, 1440, 1}, {12, 3840, 2160, 0}};
struct hz { int code; double hz; int measured; };
/* only measured codes: code 1 came back with VRR on and 120 Hz off, which is
 * not 23.98 Hz as in the PS4 table, so the PS4 table is not used here */
static const struct hz HZ[] = {{3, 59.94, 1}, {13, 119.88, 1}, {1, 60.0, 1}};   /* 1: 60 Hz with VRR */

static int decode(char *out, int size, const uint8_t *b, int *nunk) {
  int n = 0, rc = b[16], fc = b[8], bpp = b[7];
  const struct res *r = NULL;
  const struct hz *f = NULL;
  for(size_t i = 0; i < sizeof RES / sizeof RES[0]; i++) if(RES[i].code == rc) r = &RES[i];
  for(size_t i = 0; i < sizeof HZ / sizeof HZ[0]; i++) if(HZ[i].code == fc) f = &HZ[i];
  n = jcat(out, size, n, "\"res_code\":%d,", rc);
  if(r) n = jcat(out, size, n, "\"width\":%d,\"height\":%d,\"res_measured\":%s,", r->w, r->h, r->measured ? "true" : "false");
  n = jcat(out, size, n, "\"hz_code\":%d,", fc);
  if(f) n = jcat(out, size, n, "\"hz\":%.2f,\"hz_measured\":%s,", f->hz, f->measured ? "true" : "false");
  n = jcat(out, size, n, "\"vrr\":%s,\"bpp\":%d,", (b[9] & 0x80) ? "true" : "false", bpp);
  n = jcat(out, size, n, "\"range_code\":%d,\"hdr_code\":%d,\"fmt_code\":%d,", b[5], b[6], b[4]);
  if(b[5] == 1 || b[5] == 2) n = jcat(out, size, n, "\"rgb_range\":\"%s\",", b[5] == 1 ? "limited" : "full");
  if(b[6] == 2 || b[6] == 13 || b[6] == 14) n = jcat(out, size, n, "\"hdr\":%s,", b[6] != 2 ? "true" : "false");
  if(b[4] == 1 || b[4] == 3) n = jcat(out, size, n, "\"fmt\":\"%s\",", b[4] == 1 ? "rgb" : "ycbcr422");
  /* every field whose code is not in the tables above */
  int vrr_ok = (b[9] & 0x7f) == 0, bpp_ok = bpp == 24 || bpp == 30 || bpp == 36;
  *nunk = !r + !f + !(b[6] == 2 || b[6] == 13 || b[6] == 14) + !(b[4] == 1 || b[4] == 3) +
          !(b[5] == 1 || b[5] == 2) + !bpp_ok + !vrr_ok;
  n = jcat(out, size, n, "\"unknown\":[%s%s%s%s%s%s%s],",
                r ? "" : "\"resolution\",", f ? "" : "\"refresh\",",
                (b[6] == 2 || b[6] == 13 || b[6] == 14) ? "" : "\"hdr\",",
                (b[4] == 1 || b[4] == 3) ? "" : "\"format\",",
                (b[5] == 1 || b[5] == 2) ? "" : "\"range\",",
                bpp_ok ? "" : "\"depth\",", vrr_ok ? "" : "\"vrr\",");
  if(out[n - 3] == ',' && out[n - 2] == ']') { out[n - 3] = ']'; out[n - 2] = ','; out[--n] = '\0'; }
  if(bpp && bpp % 3 == 0) n = jcat(out, size, n, "\"bpc\":%d,", bpp / 3);
  return n;
}

static pthread_mutex_t g_read_lock = PTHREAD_MUTEX_INITIALIZER;
static void display_read_locked(void);
void display_read(void) {                     /* worker every second, or the refresh button */
  pthread_mutex_lock(&g_read_lock);
  if(!g_failed) display_read_locked();
  pthread_mutex_unlock(&g_read_lock);
}

static void display_read_locked(void) {
  char out[1536];
  int n = 0;
  static const char *how = "";
  if(!g_fn) {
    static const char *PATHS[] = {"libSceVideoOut.sprx", "/system/common/lib/libSceVideoOut.sprx",
                                  "/system/priv/lib/libSceVideoOut.sprx"};
    char tried[400] = "";
    int tn = 0;
    for(size_t i = 0; i < sizeof PATHS / sizeof PATHS[0] && g_handle < 0; i++) {
      struct load_ctx l = {PATHS[i], -1};
      int crashed = sysinfo_guarded_call(do_load, &l);
      tn = jcat(tried, sizeof tried, tn, "%s%s=%s0x%08x", tn ? ", " : "", PATHS[i],
                     crashed ? "crash " : "", (unsigned)l.handle);
      if(!crashed && l.handle >= 0) { g_handle = l.handle; how = PATHS[i]; }
    }
    if(g_handle < 0) {
      g_failed = 1;
      pthread_mutex_lock(&g_lock);
      snprintf(g_json, sizeof g_json, "\"state\":\"error\",\"step\":\"load\",\"tried\":\"%s\",\"at\":%lld", tried, (long long)time(NULL));
      pthread_mutex_unlock(&g_lock);
      return;
    }
    struct sym_ctx s = {g_handle, NULL, -1, 0};
    int crashed = sysinfo_guarded_call(do_sym, &s);
    if(crashed || !s.addr) {
      g_failed = 1;
      pthread_mutex_lock(&g_lock);
      snprintf(g_json, sizeof g_json, "\"state\":\"error\",\"step\":\"lookup\",\"dlsym_rc\":\"0x%08x\",\"kernel_by_name\":\"not found\",\"kernel_by_nid\":\"not found\",\"crashed\":%s,\"handle\":%d,\"at\":%lld",
               (unsigned)s.rc, crashed ? "true" : "false", g_handle, (long long)time(NULL));
      pthread_mutex_unlock(&g_lock);
      return;
    }
    g_fn = (get_mode_fn)s.addr;
    g_how = s.how == 1 ? "dlsym" : s.how == 2 ? "kernel, by name" : "kernel, by NID";
  }
  n = jcat(out, sizeof out, n, "\"state\":\"ok\",\"lib\":\"%s\",\"handle\":%d,\"found_by\":\"%s\",\"at\":%lld,\"calls\":[",
                how, g_handle, g_how, (long long)time(NULL));
  n = jclamp(n + (long long)one_call(out + n, sizeof out - n, 0x7000), sizeof out);   /* main output; nothing else is tried */
  snprintf(out + n, sizeof out - n, "]");
  pthread_mutex_lock(&g_lock);
  snprintf(g_json, sizeof g_json, "%s", out);
  pthread_mutex_unlock(&g_lock);
}

/* ---- log of unmapped codes, kept on the console
 * One JSON line per new combination (same raw answer + same game is written
 * once), last LOG_MAX lines kept. Holds the time, the game (or none), every
 * decoded value of that moment, the list of unknown fields and the raw bytes. */
#define LOG_FILE "/data/heatline/display_unknown.jsonl"
#define LOG_MAX  100
static char g_keys[LOG_MAX][96];
static int g_nkeys, g_keys_loaded;

static void key_of(char *key, size_t n, const char *title, const char *hex) {
  snprintf(key, n, "%s|%.64s", title, hex);
}

static void field(const char *line, const char *name, char *out, size_t n) {
  char pat[24];
  snprintf(pat, sizeof pat, "\"%s\":\"", name);
  const char *p = strstr(line, pat);
  out[0] = '\0';
  if(!p) return;
  p += strlen(pat);
  size_t i = 0;
  while(p[i] && p[i] != '"' && i + 1 < n) { out[i] = p[i]; i++; }
  out[i] = '\0';
}

static void keys_load(void) {
  g_keys_loaded = 1;
  FILE *fp = fopen(LOG_FILE, "r");
  if(!fp) return;
  char line[2048];
  while(fgets(line, sizeof line, fp)) {
    char t[24], h[80];
    field(line, "title", t, sizeof t);
    field(line, "hex", h, sizeof h);
    if(!h[0]) continue;
    if(g_nkeys == LOG_MAX) { memmove(g_keys[0], g_keys[1], sizeof g_keys[0] * (LOG_MAX - 1)); g_nkeys--; }
    key_of(g_keys[g_nkeys++], sizeof g_keys[0], t, h);
  }
  fclose(fp);
}

/* keeps the last LOG_MAX lines of the file */
static void log_trim(void) {
  FILE *fp = fopen(LOG_FILE, "r");
  if(!fp) return;
  static char lines[LOG_MAX + 1][2048];
  int n = 0;
  while(n <= LOG_MAX && fgets(lines[n], sizeof lines[0], fp)) n++;
  if(n <= LOG_MAX) { fclose(fp); return; }
  /* more than LOG_MAX: read the rest as a rolling window */
  char tmp[2048];
  while(fgets(tmp, sizeof tmp, fp)) {
    memmove(lines[0], lines[1], sizeof lines[0] * LOG_MAX);
    snprintf(lines[LOG_MAX], sizeof lines[0], "%s", tmp);
  }
  fclose(fp);
  fp = fopen(LOG_FILE ".tmp", "w");
  if(!fp) return;
  for(int i = 1; i <= LOG_MAX; i++) fputs(lines[i], fp);
  fclose(fp);
  rename(LOG_FILE ".tmp", LOG_FILE);
}

static void log_unknown(const char *dec, const char *hex) {
  char id[24], name[260];
  fps_game(id, sizeof id, name, sizeof name);
  if(!g_keys_loaded) keys_load();
  char key[96];
  key_of(key, sizeof key, id, hex);
  for(int i = 0; i < g_nkeys; i++) if(!strcmp(g_keys[i], key)) return;   /* already kept */
  if(g_nkeys == LOG_MAX) { memmove(g_keys[0], g_keys[1], sizeof g_keys[0] * (LOG_MAX - 1)); g_nkeys--; }
  snprintf(g_keys[g_nkeys++], sizeof g_keys[0], "%s", key);
  mkdir("/data/heatline", 0755);
  FILE *fp = fopen(LOG_FILE, "a");
  if(!fp) return;
  int dl = (int)strlen(dec);
  if(dl && dec[dl - 1] == ',') dl--;                 /* decoded fields end with a comma */
  fprintf(fp, "{\"at\":%lld,\"title\":\"%s\",\"name\":\"%s\",%.*s,\"hex\":\"%s\"}\n",
          (long long)time(NULL), id, name, dl, dec, hex);
  fclose(fp);
  log_trim();
}

/* the log as {"entries":[...]}: malloc'd, caller frees; NULL if out of memory */
char *display_log_json(int *len) {
  pthread_mutex_lock(&g_read_lock);
  FILE *fp = fopen(LOG_FILE, "r");
  size_t cap = 64, n = 0;
  const size_t total = cap + LOG_MAX * 2048;          /* room for LOG_MAX full lines */
  char *out = malloc(total);
  if(!out) { if(fp) fclose(fp); pthread_mutex_unlock(&g_read_lock); return NULL; }
  n += (size_t)sprintf(out, "{\"entries\":[");
  if(fp) {
    char line[2048];
    int first = 1;
    while(fgets(line, sizeof line, fp)) {
      size_t l = strlen(line);
      while(l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
      if(!l || line[0] != '{') continue;
      /* the file should hold at most LOG_MAX lines, but never trust it:
       * stop when the next line, its comma and the closing "]}" no longer fit */
      if(n + 1 + l + 3 > total) break;
      if(!first) out[n++] = ',';
      memcpy(out + n, line, l);
      n += l;
      first = 0;
    }
    fclose(fp);
  }
  n += (size_t)sprintf(out + n, "]}");
  pthread_mutex_unlock(&g_read_lock);
  *len = (int)n;
  return out;
}

void display_log_clear(void) {
  pthread_mutex_lock(&g_read_lock);
  unlink(LOG_FILE);
  g_nkeys = 0;
  g_keys_loaded = 1;
  pthread_mutex_unlock(&g_read_lock);
}

/* read every second; stops for good if the library or the function cannot be
 * reached, or if the call ever crashes (caught) */
static void *worker(void *arg) {
  (void)arg;
  sleep(2);
  while(!g_failed) {
    display_read();                          /* stops by itself once g_failed is set */
    pthread_mutex_lock(&g_lock);
    if(strstr(g_json, "\"crashed\":true")) g_failed = 1;
    pthread_mutex_unlock(&g_lock);
    sleep(1);
  }
  return NULL;
}

void display_start(void) {
  pthread_t th;
  if(pthread_create(&th, NULL, worker, NULL) == 0) pthread_detach(th);
}

int display_json(char *buf, int size) {
  pthread_mutex_lock(&g_lock);
  int n = jcat(buf, size, 0, "\"display\":{%s},", g_json);
  pthread_mutex_unlock(&g_lock);
  return n < size ? n : size - 1;
}
