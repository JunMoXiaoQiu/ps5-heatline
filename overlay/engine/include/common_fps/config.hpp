/*
 * Common FPS for PS5
 * Copyright (C) 2026 porhe911
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#include "common_fps/types.hpp"
#include <string_view>

namespace common_fps {

OverlayConfig default_config();

/*
 * Reads "key=value" lines. Unknown keys and bad values keep the default, so a
 * half-edited file never produces anything worse than the default overlay.
 */
OverlayConfig parse_config_text(std::string_view text);

/* The settings file written on first run, with every key and its choices. */
const char* default_config_text();

const char* corner_name(Corner corner);
const char* item_name(OverlayItem item);

/* The corner reached after "steps" moves clockwise from "start". */
Corner rotated_corner(Corner start, unsigned steps);

} // namespace common_fps
