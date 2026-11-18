// SPDX-License-Identifier: GPL-3.0-only
/* In-game overlay for Heatline.
 *
 * Draws the current FPS over the running game itself, using
 * the vendored SimpleFPS / Common-FPS engine (libheatline_overlay.a, driven
 * through osd_overlay.h): it injects a Mono renderer into the home-screen
 * process (SceShellUI) and draws there, so it appears above the game. This is
 * the ONLY part of Heatline that writes to another process; everything else
 * only reads. See osd.c for the lifecycle.
 *
 * Off by default, saved on the console (/data/heatline/fps_overlay), toggled
 * from Settings on the dashboard. The engine shows the number only while a
 * game has been in front for a few seconds.
 *
 * SAFETY: the engine writes into ShellUI; a fault there crashes the home
 * screen, not the game. A restart always removes the overlay (no clean off
 * switch, like SimpleFPS); the saved flag keeps it from starting next launch.
 */
#pragma once

#include <stddef.h>

int  osd_start(void);                 /* start the thread and apply the saved flag; 0 on success */
void osd_set(int on);                 /* turn the overlay on/off and save the flag */
int  osd_get(void);                   /* 1 if the saved flag is on */
int  osd_set_style(int corner, int mx, int my, int font, int opacity); /* position/size; saved; 0 ok, -1 invalid */
int  osd_set_layout(const char *spec); /* choose the overlay's metrics/arrangement and save it; 0 ok, -1 invalid */
int  osd_json(char *buf, size_t size); /* append "overlay":{...}, to a JSON object (no leading comma) */
int  osd_log_dump(char *buf, size_t size); /* read-only: overlay engine /data logs (temporary diagnostic) */
