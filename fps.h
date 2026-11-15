// SPDX-License-Identifier: GPL-3.0-only
/* FPS sampling for Heatline.
 *
 * While a game runs, a background thread reads the display driver (/dev/dce,
 * ioctl 0x80308217, as in PS5 Thermal Doctor) every 4 ms. The answer holds
 * the frame counter and the console's own timestamp of the last frame, so
 * every frame's duration is exact (CPU timestamp counter), whatever the read
 * rate. Without a game the driver is not touched (it would flood the log).
 *
 *   fps       frames shown during the last second
 *   avg       frames / time since the game started (or last reset)
 *   ft_ms     mean frame time over the last second; ft_worst_ms its longest
 *   min / max lowest / highest one-second FPS of the session
 *   rt{}      CapFrameX real-time metrics over the last 20 s (see fps.c);
 *             the "more metrics" part only when fps_set_more(1)
 * Frames longer than 250 ms (pause, PS button) are not counted in the
 * session values; the 20 s window starts again after a 2 s gap.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

int  fps_start(void);                 /* escalate + start thread; 0 on success */
int  fps_json(char *buf, size_t size); /* writes "game":{...} (no trailing comma) */
void fps_reset(void);
void fps_set_more(int on);
int  fps_dce_raw(unsigned char out[0x60]);   /* one read for the raw view; 0 ok, -errno */
void fps_game(char *id, size_t idn, char *name_json_escaped, size_t namen);   /* "" = no game */
int fps_ft_json(char *buf, size_t size, unsigned long long since);            /* compute the "more metrics"; saved on the console */
