// SPDX-License-Identifier: GPL-3.0-only
/* fan.h - Fan control page (features of ps5-fan-control, see fan.c) */
#pragma once
void fan_start(void);
const char *fan_set(const char *query);   /* NULL = saved and applied, else an error text */
void fan_apply_now(void);
int fan_json(char *buf, int size);
int fan_last_write(unsigned char out[10], long long *at);   /* raw view; -1 = nothing written yet */
