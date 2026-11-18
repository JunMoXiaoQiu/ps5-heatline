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

/*
 * Where the digits sit inside a text box, in font sizes from its top. The font
 * cannot be measured from here: estimated from a photo of the overlay. Shared
 * by every layout routine so they all place text the same way.
 */
inline constexpr float kTextTop = 0.42f;
inline constexpr float kTextBottom = 1.14f;
/* Step between two lines of text. */
inline constexpr float kRowStep = 1.30f;

struct RowPosition {
    float label_x{};
    float value_x{};
    float y{};
    /* Fixed columns for the "name : value" layout: the colon sits at a fixed
     * x and the number starts at a fixed x just after it, so adding a digit
     * grows the number rightward without shifting anything to its left. */
    float colon_x{};
    float number_x{};
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
