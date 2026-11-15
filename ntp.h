// SPDX-License-Identifier: GPL-3.0-only
/* ntp.h - date & time sync over the internet (see ntp.c) */
#pragma once
int  ntp_start(int do_set);        /* 0 = check only, 1 = set the clock */
int  ntp_autosync_get(void);
int  ntp_autosync_set(int on);
void ntp_boot(void);               /* sync at start if auto-sync is on */
int  ntp_json(char *buf, int size);
int  ntp_last_packet(unsigned char out[48], long long *at);   /* raw view; -1 = none yet */
