/*
 * Common FPS for PS5
 * Copyright (C) 2026 porhe911
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#include "common_fps/types.hpp"

namespace common_fps {

Vec2 compute_anchor(const OverlayConfig& cfg);

struct RowPosition {
    float label_x{};
    float value_x{};
    float y{};
};

/* Estimated width of one line of text, in logical pixels. */
float estimate_text_width(const char* text, int font_size);

/*
 * Where each row of the frame goes; out holds kMaxOverlayRows entries.
 * Names form a column on the left and their values end on a shared right
 * edge; a line without a name among them starts where the names start.
 * With no names at all, values start at the anchor in the left corners and
 * end at the margin in the right corners.
 */
void compute_row_positions(const OverlayFrame& frame, RowPosition* out);

} // namespace common_fps
