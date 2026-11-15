// SPDX-License-Identifier: GPL-3.0-only
/* netlat.h - router / internet latency (TCP handshake time) */
#pragma once
void netlat_start(void);
int netlat_json(char *buf, int size);
void netlat_now(void);             /* refresh button: measure within 1 s */
void netlat_set(int on);           /* Settings switch, saved on the console */
