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

/* Current one-second FPS for the on-screen overlay (osd.c). Returns 1 and
 * writes the rounded FPS to *fps while a game is running and a measurement
 * exists; returns 0 otherwise (no game yet, or no first measurement). Cheap:
 * just reads the last computed value under the lock, triggers no driver read. */
int  fps_now_value(int *fps);

/* Overlay metrics from Heatline's own sampler: current FPS plus the CapFrameX
 * real-time average, 1% low (P1) and 0.2% low (P0.2). Returns 1 while a game
 * runs; a metric still filling its window is written as -1. The in-game
 * overlay draws these so it shares one source with the dashboard. */
int  fps_overlay_metrics(int *fps, int *avg, int *p1, int *p02);

/* Everything the in-game overlay can show about the running game. A field is
 * negative when it has no value yet (a window still filling, or the "more
 * metrics" switch off). */
typedef struct {
  double fps, avg, p1, p02, low1, low02;          /* real-time, 20 s window */
  double ft, ft_worst;                            /* frame time, ms, last second */
  double savg, smin, smax;                        /* session average / lowest / highest */
  double stut_pct, stut_n;                        /* stuttering time %, stutters in 5 s */
  double p5, p50, p95, p99, p01, low01;           /* "more metrics" */
  double il1, il02, il01, astd, ftmin, ftmax;
} fps_overlay_t;
int  fps_overlay_all(fps_overlay_t *o);           /* 1 = a game is running, 0 = none (o untouched) */
int  fps_dce_raw(unsigned char out[0x60]);   /* one read for the raw view; 0 ok, -errno */
void fps_game(char *id, size_t idn, char *name_json_escaped, size_t namen);   /* "" = no game */
int fps_ft_json(char *buf, size_t size, unsigned long long since);            /* compute the "more metrics"; saved on the console */
