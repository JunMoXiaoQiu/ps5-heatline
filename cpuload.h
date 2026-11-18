// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define CPULOAD_MAX 16   /* logical CPUs (8 cores x 2 threads) */

/* Start the 1 s sampler thread (after the privilege escalation). */
void cpuload_start(void);
/* Append "cpu_usage":{...}, or "cpu_usage":null, to a JSON object. */
int  cpuload_json(char *buf, size_t size);
/* Copy the last usage per logical CPU (-1 = unknown) into out[CPULOAD_MAX]. */
void cpuload_snapshot(int out[CPULOAD_MAX]);
int  cpuload_avg(void);               /* overall load, 0..100, -1 = not known */
int  cpuload_raw_json(char *out, int size);   /* raw view, on request only */
