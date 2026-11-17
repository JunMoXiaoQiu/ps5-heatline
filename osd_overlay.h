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
