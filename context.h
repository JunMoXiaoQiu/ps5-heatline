// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
/* Append "context":{"value":"GAME"|"OS"|"unknown",...}, to a JSON object. */
int context_json(char *buf, size_t size);
