// SPDX-License-Identifier: GPL-3.0-only
/* selfstat.h - Heatline's own CPU, memory and network use (see selfstat.c) */
#pragma once
#include <stdint.h>
void selfstat_tick(void);              /* once per second */
void selfstat_count(uint64_t bytes);   /* one HTTP response sent */
int  selfstat_json(char *buf, int size);
