// SPDX-License-Identifier: GPL-3.0-only
/* raw.h - raw answers of the console's functions, for the "Raw data" view.
 * Only runs inside a /raw?p=<panel> request. */
#pragma once
int raw_json(const char *panel, char *buf, int size);
