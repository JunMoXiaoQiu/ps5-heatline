// SPDX-License-Identifier: GPL-3.0-only
/* display.h - display output mode, raw read on request (test stage) */
#pragma once
void display_read(void);
void display_start(void);          /* reads every second */
int display_json(char *buf, int size);
char *display_log_json(int *len);   /* {"entries":[...]}, malloc'd */
void display_log_clear(void);
