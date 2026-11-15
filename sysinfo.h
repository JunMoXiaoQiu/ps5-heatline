// SPDX-License-Identifier: GPL-3.0-only
/* sysinfo.h - System and Network pages (fields as PS5 Monitor) */
#pragma once
void sysinfo_start(void);
int sysinfo_json(char *buf, int size);
void sysinfo_gateway(char *out, int size);   /* "" if not known yet */
void sysinfo_refresh(void);
int  sysinfo_netctl_raw(int code, unsigned char *buf2048);   /* raw view: sceNetCtlGetInfo, guarded */   /* re-read hardware / network info within 1 s */
int sysinfo_guarded_call(int (*fn)(void *), void *ctx);   /* 0 ok, -1 crashed (caught) */
