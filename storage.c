// SPDX-License-Identifier: GPL-3.0-only
/* storage.c - console storage, M.2 and USB extended storage.
 *
 * Method from PS5 Monitor 1.4.8 (payload/main.c handle_list_storage and
 * payload/monitor_web.c handle_volumes), same formula:
 *
 *  Console storage, as Settings > Storage shows it (decimal GB): ShellCore's
 *  sceShellCoreUtilGetFreeSizeOfUserPartition; if it fails, a formula fitted
 *  on this console's raw values (see collect()). PS5 Monitor's shipped formula
 *  read 770.2 GB and its code-comment formula 826.3 GB where Settings said 848.0.
 *  M.2 SSD = statfs of /mnt/ext1, USB extended storage = /mnt/ext0
 *    (total = f_blocks * bsize, free = f_bavail * bsize).
 *
 * The partition probe can block on some device nodes, so (as in PS5 Monitor)
 * it runs once on a detached thread; until it has finished the fallback is
 * used. Values are refreshed at most every 5 s (PS5 Monitor: 5 s too).
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "storage.h"
#include "jbuf.h"

/* ShellCore's own numbers for Settings > Storage (undocumented; seen on a
 * CFI-2016, FW 13.60: rc 0, first slot = free bytes, second = size bytes,
 * equal to what Settings shows). Called with four 64-byte buffers in case it
 * takes more arguments than the two it writes. */
int sceShellCoreUtilGetFreeSizeOfUserPartition(void *a, void *b, void *c, void *d) __attribute__((weak));

static volatile int g_probe_state;          /* 0 idle, 1 running, 2 finished */
static int g_disk_ok;                       /* 1 = partition table read */
static const char *g_disk_src = "none";
static uint64_t g_non_user_parts, g_disk_overhang, g_media;

/* kern.geom.conftxt: "<level> <class> <name> <mediasize> ..." per provider */
static int geom_probe(void) {
  size_t sz = 0;
  if(sysctlbyname("kern.geom.conftxt", NULL, &sz, NULL, 0) != 0 || sz < 64 || sz > (1u << 20))
    return -1;
  char *txt = malloc(sz + 1);
  if(!txt) return -1;
  if(sysctlbyname("kern.geom.conftxt", txt, &sz, NULL, 0) != 0) {
    free(txt);
    return -1;
  }
  txt[sz] = 0;
  struct { uint64_t media, parts, biggest; int used; } disks[16];
  memset(disks, 0, sizeof disks);
  int cur = -1;
  char *save = NULL;
  for(char *ln = strtok_r(txt, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
    int lv;
    char cls[32], nm[64];
    unsigned long long ms;
    if(sscanf(ln, "%d %31s %63s %llu", &lv, cls, nm, &ms) < 4) continue;
    if(lv == 0 && !strcmp(cls, "DISK")) {
      for(cur = 0; cur < 16 && disks[cur].used; cur++);
      if(cur < 16) {
        disks[cur].used = 1;
        disks[cur].media = ms;
      } else {
        cur = -1;
      }
    } else if(cur >= 0 && lv > 0 && !strcmp(cls, "PART")) {
      disks[cur].parts += ms;
      if(ms > disks[cur].biggest) disks[cur].biggest = ms;
    }
  }
  free(txt);
  /* internal disk = the one holding the biggest partition (/user) */
  int best = -1;
  for(int i = 0; i < 16; i++)
    if(disks[i].used && (best < 0 || disks[i].biggest > disks[best].biggest)) best = i;
  if(best < 0 || !disks[best].parts || !disks[best].biggest) return -1;
  g_non_user_parts = disks[best].parts - disks[best].biggest;
  g_disk_overhang = disks[best].media > disks[best].parts ? disks[best].media - disks[best].parts : 0;
  g_media = disks[best].media;
  g_disk_src = "geom";
  return 1;
}

/* Fallback: the /dev/ssd0* nodes - backup GPT of the whole disk, or the sum
 * of the partition nodes' sizes (biggest = /user). */
static int dev_probe(void) {
  char devs[24][32];
  int ndev = 0;
  DIR *dd = opendir("/dev");
  if(dd) {
    struct dirent *de;
    while((de = readdir(dd)) && ndev < 24) {
      if(strncmp(de->d_name, "ssd0", 4)) continue;
      snprintf(devs[ndev++], 32, "/dev/%s", de->d_name);
    }
    closedir(dd);
  }
  uint64_t parts_sum = 0, parts_max = 0, disk_media = 0;
  for(int i = 0; i < ndev; i++) {
    int fd = open(devs[i], O_RDONLY | O_NONBLOCK);
    if(fd < 0) continue;
    off_t media = 0;
    u_int secsize = 512;
    if(ioctl(fd, DIOCGMEDIASIZE, &media) != 0 || media <= 0) { close(fd); continue; }
    ioctl(fd, DIOCGSECTORSIZE, &secsize);
    if(secsize < 512 || secsize > 65536) secsize = 512;
    if(strstr(devs[i], "ssd0.")) {
      parts_sum += (uint64_t)media;
      if((uint64_t)media > parts_max) parts_max = (uint64_t)media;
      close(fd);
      continue;
    }
    disk_media = (uint64_t)media;
    uint64_t sectors = (uint64_t)media / secsize;
    size_t tail = 34 * (size_t)secsize;
    uint8_t *buf = malloc(tail);
    if(!buf || sectors < 34) { free(buf); close(fd); continue; }
    ssize_t got = pread(fd, buf, tail, (off_t)((sectors - 34) * secsize));
    close(fd);
    if(got != (ssize_t)tail) { free(buf); continue; }
    /* backup header = last LBA, its 32 entry sectors right before it.
     * (PS5 Monitor reads the header one sector too early, so this path never
     * matches there; fixed here. The geom path above is tried first anyway.) */
    uint8_t *hdr = buf + 33 * (size_t)secsize;
    if(memcmp(hdr, "EFI PART", 8)) { free(buf); continue; }
    uint32_t nent, entsz;
    memcpy(&nent, hdr + 80, 4);
    memcpy(&entsz, hdr + 84, 4);
    if(nent == 0 || nent > 1024 || entsz < 128) { free(buf); continue; }
    uint64_t total_part = 0, biggest = 0;
    for(uint32_t e = 0; e < nent && (size_t)(e + 1) * entsz <= 32 * (size_t)secsize; e++) {
      uint8_t *en = buf + (size_t)secsize + (size_t)e * entsz;
      uint64_t type0, fl, ll;
      memcpy(&type0, en, 8);
      if(!type0) continue;
      memcpy(&fl, en + 32, 8);
      memcpy(&ll, en + 40, 8);
      if(ll < fl) continue;
      uint64_t s = (ll - fl + 1) * secsize;
      total_part += s;
      if(s > biggest) biggest = s;
    }
    free(buf);
    if(!total_part || !biggest) continue;
    g_non_user_parts = total_part - biggest;
    g_disk_overhang = (uint64_t)media > total_part ? (uint64_t)media - total_part : 0;
    g_media = (uint64_t)media;
    g_disk_src = "gpt";
    return 1;
  }
  if(parts_max) {
    g_non_user_parts = parts_sum - parts_max;
    g_disk_overhang = disk_media > parts_sum ? disk_media - parts_sum : 0;
    g_media = disk_media ? disk_media : parts_sum;
    g_disk_src = "dev";
    return 1;
  }
  return -1;
}

static void *probe_worker(void *arg) {
  (void)arg;
  g_disk_ok = geom_probe() > 0 || dev_probe() > 0;
  g_probe_state = 2;
  return NULL;
}

static void probe_start(void) {
  if(g_probe_state) return;
  g_probe_state = 1;
  pthread_t th;
  if(pthread_create(&th, NULL, probe_worker, NULL) == 0) pthread_detach(th);
  else g_probe_state = 0;
}

static int vol(const char *path, uint64_t *t, uint64_t *f) {
  struct statfs s;
  if(statfs(path, &s) != 0 || strcmp(s.f_mntonname, path)) return 0;   /* not mounted there */
  uint64_t bs = s.f_bsize > 0 ? (uint64_t)s.f_bsize : 1;
  *t = (uint64_t)s.f_blocks * bs;
  *f = (uint64_t)s.f_bavail * bs;
  if(*f > *t) *f = *t;
  return 1;
}


static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_cache[1024];
static time_t g_cache_t;

static void collect(void) {
  int n = 0, sz = sizeof g_cache;
  struct statfs sf;
  probe_start();
  if(statfs("/user", &sf) != 0) {
    snprintf(g_cache, sz, "\"storage\":{\"ok\":false},");
    return;
  }
  uint64_t bs = sf.f_bsize;
  uint64_t u_total = (uint64_t)sf.f_blocks * bs;
  uint64_t u_bfree = (uint64_t)sf.f_bfree * bs;
  uint64_t u_bavail = sf.f_bavail > 0 ? (uint64_t)sf.f_bavail * bs : u_bfree;
  uint64_t u_rsvd = u_bfree > u_bavail ? u_bfree - u_bavail : 0;
  uint64_t sd_t = 0, sd_f = 0, sx_t = 0, sx_f = 0;
  if(statfs("/system_data", &sf) == 0) {
    sd_t = (uint64_t)sf.f_blocks * sf.f_bsize;
    sd_f = (uint64_t)(sf.f_bavail > 0 ? sf.f_bavail : 0) * sf.f_bsize;
  }
  if(statfs("/system_ex", &sf) == 0) {
    sx_t = (uint64_t)sf.f_blocks * sf.f_bsize;
    sx_f = (uint64_t)(sf.f_bavail > 0 ? sf.f_bavail : 0) * sf.f_bsize;
  }
  /* Settings > Storage, fitted on a CFI-2016 (FW 13.60) from the raw values:
   *   Used = /user total - /user bavail          (its reserve counts as used)
   *   Size = /user total - the other SSD partitions - /system_data - /system_ex
   *   Free = Size - Used
   * Settings showed 848.0 / 492.5 used / 355.5 free; this gave 848.03 / 491.8 /
   * 356.2 (used and free taken 5 min apart). Needs the partition table; until
   * the probe answers (or if it fails) the visible system mounts stand in for
   * the other partitions and the values are flagged approx. */
  /* 1st choice: ShellCore (exactly what Settings shows) */
  int shell = 0;
  uint64_t sh_free = 0, sh_total = 0;
  if(sceShellCoreUtilGetFreeSizeOfUserPartition) {
    static uint8_t arg[4][64] __attribute__((aligned(16)));
    memset(arg, 0, sizeof arg);
    if(sceShellCoreUtilGetFreeSizeOfUserPartition(arg[0], arg[1], arg[2], arg[3]) == 0) {
      memcpy(&sh_free, arg[0], 8);
      memcpy(&sh_total, arg[1], 8);
      shell = sh_total > 0 && sh_free <= sh_total && sh_total < (1ull << 50);
    }
  }
  /* fallback: fitted formula (848.03 / 356.19 GB where Settings said 848.0 / 355.5) */
  int exact = g_probe_state == 2 && g_disk_ok;
  uint64_t other;
  if(exact) {
    other = g_non_user_parts;
  } else {
    static const char *sys[] = {"/system", "/preinst", "/system_tmp", "/update", "/preinst2",
                                "/safemode", "/minvsn_update", NULL};
    other = 0;
    for(int i = 0; sys[i]; i++)
      if(statfs(sys[i], &sf) == 0) other += (uint64_t)sf.f_blocks * sf.f_bsize;
  }
  uint64_t used = u_total > u_bavail ? u_total - u_bavail : 0;
  uint64_t cut = other + sd_t + sx_t;
  uint64_t total = u_total > cut ? u_total - cut : u_total;
  uint64_t free_b = total > used ? total - used : 0;
  (void)sd_f; (void)sx_f; (void)u_rsvd;
  if(shell) {
    total = sh_total;
    free_b = sh_free;
    used = total - free_b;
  }
  /* "Reserved / System" = the rest of the SSD, not counted in Settings' size */
  uint64_t media = exact ? g_media : 0;
  uint64_t reserved = media > total ? media - total : 0;
  n = jcat(g_cache, sz, n,
                "\"storage\":{\"ok\":true,\"exact\":%s,\"shellcore\":%s,\"src\":\"%s\",\"console\":{\"total\":%llu,\"used\":%llu,\"free\":%llu,\"reserved\":%llu},"
                "\"raw\":{\"user\":[%llu,%llu,%llu],\"sysdata\":[%llu,%llu],\"sysex\":[%llu,%llu],\"disk\":%llu,\"nonuser\":%llu}",
                (shell || exact) ? "true" : "false", shell ? "true" : "false", g_probe_state == 2 ? g_disk_src : "pending",
                (unsigned long long)total, (unsigned long long)used,
                (unsigned long long)free_b, (unsigned long long)reserved,
                (unsigned long long)u_total, (unsigned long long)u_bfree, (unsigned long long)u_bavail,
                (unsigned long long)sd_t, (unsigned long long)sd_f, (unsigned long long)sx_t, (unsigned long long)sx_f,
                (unsigned long long)media, (unsigned long long)(g_disk_ok ? g_non_user_parts : 0));
  static const struct { const char *key, *path; } ext[] = {{"m2", "/mnt/ext1"}, {"usb", "/mnt/ext0"}};
  for(int i = 0; i < 2; i++) {
    uint64_t t = 0, f = 0;
    int m = vol(ext[i].path, &t, &f);
    n = jcat(g_cache, sz, n, ",\"%s\":{\"mounted\":%s,\"total\":%llu,\"used\":%llu,\"free\":%llu}",
                  ext[i].key, m ? "true" : "false", (unsigned long long)t,
                  (unsigned long long)(t - f), (unsigned long long)f);
  }
  snprintf(g_cache + n, sz - n, "},");
}

int storage_json(char *buf, int size) {
  pthread_mutex_lock(&g_lock);
  time_t now = time(NULL);
  /* refresh every 5 s, and as soon as the partition probe has finished */
  static int seen_probe;
  if(!g_cache_t || now - g_cache_t >= 5 || (g_probe_state == 2 && !seen_probe)) {
    if(g_probe_state == 2) seen_probe = 1;
    collect();
    g_cache_t = now;
  }
  int n = jcat(buf, size, 0, "%s", g_cache);
  pthread_mutex_unlock(&g_lock);
  return n < size ? n : size - 1;
}
