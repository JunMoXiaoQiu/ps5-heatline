// SPDX-License-Identifier: GPL-3.0-only
/* netlat.h - router / internet latency (TCP handshake time) */
#pragma once
void netlat_start(void);
int netlat_json(char *buf, int size);
int  netlat_overlay(double v[4]);  /* router last, router jitter, internet last, internet jitter (ms, -1 = none); 0 = switch off */
void netlat_now(void);             /* refresh button: measure within 1 s */
void netlat_set(int on);           /* Settings switch, saved on the console */
