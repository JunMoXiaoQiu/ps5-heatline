// SPDX-License-Identifier: GPL-3.0-only
/* C API of the vendored in-game overlay engine (libheatline_overlay.a).
 *
 * This is the SimpleFPS / Common-FPS engine (GPL-3.0-or-later) built as a
 * static library: it finds SceShellUI, injects a Mono renderer, and draws the
 * FPS over the running game. osd.c drives it from the dashboard toggle.
 *
 * overlay_start(): start the engine's worker (idempotent; starts once). The
 *   worker injects the renderer only after a game has been in front for a few
 *   seconds, exactly as SimpleFPS does.
 * overlay_stop(): best effort only — the engine worker never returns (SimpleFPS
 *   has no off switch; a console restart removes the overlay). Heatline still
 *   persists the off state so the overlay does not start on the next launch.
 */
#pragma once

int  overlay_start(void);
void overlay_stop(void);

/* Which metrics the overlay shows and how they are arranged, as a text such as
 * "FPS;AVG;P1;P0.2" (one per line) or "FPS,AVG,P1,P0.2" (one line): ';' starts
 * a new line, ',' a new cell on the same line. Safe to call at any time and
 * from any thread; the engine picks it up within a second.
 * overlay_set_layout(): 0 on success, -1 (nothing changed) if the text holds no
 *   known metric.
 * overlay_get_layout(): writes the current layout's canonical text, returns its
 *   length. */
int  overlay_set_layout(const char *spec);
int  overlay_get_layout(char *out, unsigned long size);

/* Where and how big the overlay is. corner: 0 top-left, 1 top-right,
 * 2 bottom-left, 3 bottom-right; margins in logical pixels; font size and
 * opacity (%) are clamped to the engine's limits. overlay_set_style() returns 0,
 * or -1 for a bad corner (nothing changes); overlay_get_style() writes
 * corner, margin_x, margin_y, font_size, opacity to out[0..4]. */
int  overlay_set_style(int corner, int margin_x, int margin_y, int font_size, int opacity);
void overlay_get_style(int out[5]);

/* Heatline's side of the metric list: fills v[OVM_*] / have[OVM_*] (see
 * overlay/engine/include/common_fps/overlay_metric_list.h) with every metric
 * readable right now. Called by the overlay engine about once a second.
 * Returns 0, or -1 if count is not this build's OVM_COUNT. */
int  overlay_collect_metrics(double *v, unsigned char *have, int count);
