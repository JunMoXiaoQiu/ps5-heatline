/*
 * Common FPS for PS5
 * Copyright (C) 2026 porhe911
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "common_fps/layout.hpp"
#include "common_fps/constants.hpp"

#include <algorithm>

namespace common_fps {

namespace {

constexpr float kLabelGap = 0.50f;
/* The value column never gets narrower than three digits, so names stay put. */
constexpr float kMinValueColumn = 1.80f;

bool is_right_corner(Corner corner) {
    return corner == Corner::TopRight || corner == Corner::BottomRight;
}

bool is_bottom_corner(Corner corner) {
    return corner == Corner::BottomLeft || corner == Corner::BottomRight;
}

} // namespace

float estimate_text_width(const char* text, int font_size) {
    /* The font is proportional and cannot be measured here: per-glyph guesses. */
    float units = 0.0f;
    for (; text && *text; ++text) {
        const char c = *text;
        if (c == '1')
            units += 0.36f;  /* proportional digits: a "1" is much narrower */
        else if (c >= '0' && c <= '9')
            units += 0.60f;
        else if (c >= 'A' && c <= 'Z')
            units += 0.70f;
        else if (c == '.' || c == ':' || c == ' ')
            units += 0.30f;
        else if (c == '-')
            units += 0.40f;
        else
            units += 0.60f;
    }
    return units * static_cast<float>(font_size);
}

void compute_row_positions(const OverlayFrame& frame, RowPosition* out) {
    const OverlayConfig& cfg = frame.config;
    const float font = static_cast<float>(cfg.font_size);
    const std::size_t count = std::min(frame.row_count, kMaxOverlayRows);

    float label_width = 0.0f;
    float value_width = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        label_width = std::max(
            label_width,
            estimate_text_width(frame.rows[i].label, cfg.font_size));
        value_width = std::max(
            value_width,
            estimate_text_width(frame.rows[i].value, cfg.font_size));
    }

    const bool named = label_width > 0.0f;
    const bool right = is_right_corner(cfg.corner);
    if (named)
        value_width = std::max(value_width, font * kMinValueColumn);

    const float label_column = named ? label_width + font * kLabelGap : 0.0f;
    const float left = right
        ? kLogicalWidth - cfg.margin_x - label_column - value_width
        : cfg.margin_x;
    const float value_edge = left + label_column + value_width;

    /* Fixed colon/number columns (used by the "name : value" renderer). The
     * colon sits at the start of the value column; the number starts a fixed
     * gap after it and grows rightward, so digits never shift the colon. */
    constexpr float kColonAdvance = 0.45f;  // ":" glyph + a little breathing room
    const float colon_col = left + label_column;
    const float number_col = colon_col + font * kColonAdvance;
    /* Numbers right-align to a shared edge (at least three digits wide) so
     * units line up under units; the colon stays fixed. */
    const float number_edge = number_col + value_width;

    const float step = font * kRowStep;
    const float rows_above =
        count > 0 ? static_cast<float>(count - 1) : 0.0f;
    /* margin_y counts to the digits themselves, not to the box around them. */
    const float top = is_bottom_corner(cfg.corner)
        ? kLogicalHeight - cfg.margin_y - font * kTextBottom -
              rows_above * step
        : cfg.margin_y - font * kTextTop;

    for (std::size_t i = 0; i < kMaxOverlayRows; ++i) {
        RowPosition& row = out[i];
        row.label_x = left;
        row.y = top + static_cast<float>(i) * step;
        row.value_x = left + label_column;
        row.colon_x = colon_col;
        row.number_x = number_col;
        if (i >= count)
            continue;
        row.number_x = number_edge -
            estimate_text_width(frame.rows[i].value, cfg.font_size);

        if (named && frame.rows[i].label[0] == '\0') {
            /* The bare fps number lines up with the names below it. */
            row.value_x = left;
        } else if (named || right) {
            row.value_x = value_edge - estimate_text_width(
                frame.rows[i].value, cfg.font_size);
        }
    }
}

Vec2 compute_anchor(const OverlayConfig& cfg) {
    // Approximate single-line logical bounding box.
    const float width = static_cast<float>(cfg.font_size) * 5.2f;
    const float height = static_cast<float>(cfg.font_size) * 1.45f;

    switch (cfg.corner) {
        case Corner::TopLeft:
            return {cfg.margin_x, cfg.margin_y};

        case Corner::TopRight:
            return {kLogicalWidth - width - cfg.margin_x, cfg.margin_y};

        case Corner::BottomRight:
            return {
                kLogicalWidth - width - cfg.margin_x,
                kLogicalHeight - height - cfg.margin_y
            };

        case Corner::BottomLeft:
        default:
            return {
                cfg.margin_x,
                kLogicalHeight - height - cfg.margin_y
            };
    }
}

} // namespace common_fps
