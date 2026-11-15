// SPDX-License-Identifier: GPL-3.0-only
/* Home-screen tile for Heatline.
 *
 * Installs a small "browser launcher" app (title HTLN00001) with the Heatline
 * icon. Selecting the tile opens http://127.0.0.1:8776/ in the PS5 browser.
 * Same method as PS5 Payload Manager (src/app_installer.c), itself based on
 * ftpsrv by John Tornblom: param.json + icon0.png in /user/app/<TITLE>/sce_sys,
 * registered with sceAppInstUtil. Only reinstalls when the files differ.
 */
#pragma once

int app_install_if_needed(void);   /* 0 = ok or already up to date, -1 = error */
