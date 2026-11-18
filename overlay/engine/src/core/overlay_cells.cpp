/*
 * Heatline in-game overlay: cell layout. See overlay_cells.hpp.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "common_fps/overlay_cells.hpp"
#include "common_fps/constants.hpp"
#include "common_fps/layout.hpp"
#include "common_fps/overlay_rows.hpp"

#include <algorithm>
#include <cstdio>

namespace common_fps {

namespace {

/* In font sizes. */
constexpr float kNameToColon = 0.50f;    // name column edge to the colon
constexpr float kColonToNumber = 0.45f;  // colon to the number column
constexpr float kCellGap = 1.00f;        // between two cells on one line

constexpr std::size_t kMaxColumns = kMaxLayoutCells;

std::uint32_t with_alpha(std::uint32_t rgba, std::uint32_t alpha) {
    return (rgba & 0xffffff00U) | (alpha & 0xffU);
}

} // namespace

void build_overlay_from_spec(
    OverlayFrame& frame, const LayoutSpec& spec, const MetricValues& values) {

    const OverlayConfig& cfg = frame.config;
    frame.explicit_layout = true;
    if (!cfg.visible || spec.cell_count == 0) {
        frame.row_count = 0;
        return;
    }

    const float font = static_cast<float>(cfg.font_size);
    const std::uint32_t alpha = static_cast<std::uint32_t>(
        std::clamp(cfg.opacity, 1, 100) * 255 / 100);

    /* Text and colours first; the widths below depend on the text. */
    const std::size_t count = std::min(spec.cell_count, kMaxOverlayRows);
    std::size_t column_of[kMaxOverlayRows]{};
    std::size_t column_count = 0;
    {
        std::size_t col = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (i > 0 && spec.cells[i].line != spec.cells[i - 1].line)
                col = 0;
            column_of[i] = col++;
            column_count = std::max(column_count, col);
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        const MetricInfo& info = metric_info(spec.cells[i].metric);
        OverlayRow& row = frame.rows[i];
        row = OverlayRow{};
        std::snprintf(row.label, kOverlayLabelSize, "%s", info.label);
        format_metric_value(
            spec.cells[i].metric, values, row.value, kOverlayValueSize);
        row.label_rgba = with_alpha(info.color, alpha);
        row.value_rgba = with_alpha(0xffffffffU, alpha);  // numbers stay white
    }
    frame.row_count = count;

    /* Column widths: the widest name and the widest number in each column. */
    float name_w[kMaxColumns]{};
    float number_w[kMaxColumns]{};
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t c = column_of[i];
        name_w[c] = std::max(
            name_w[c], estimate_text_width(frame.rows[i].label, cfg.font_size));
        /* Each metric reserves the width of its own typical widest value, so
         * the column does not resize as the number changes. */
        number_w[c] = std::max(
            number_w[c],
            std::max(
                estimate_text_width(
                    metric_info(spec.cells[i].metric).widest, cfg.font_size),
                estimate_text_width(frame.rows[i].value, cfg.font_size)));
    }

    /* Left edge of each column, and the width of the widest line. */
    float column_x[kMaxColumns]{};
    float block_w = 0.0f;
    {
        float x = 0.0f;
        for (std::size_t c = 0; c < column_count; ++c) {
            column_x[c] = x;
            x += name_w[c] + font * (kNameToColon + kColonToNumber) +
                 number_w[c] + (c + 1 < column_count ? font * kCellGap : 0.0f);
        }
        block_w = x;
    }

    const std::size_t lines = spec.line_count > 0 ? spec.line_count : 1;
    const float step = font * kRowStep;

    const bool right = cfg.corner == Corner::TopRight ||
                       cfg.corner == Corner::BottomRight;
    const bool bottom = cfg.corner == Corner::BottomLeft ||
                        cfg.corner == Corner::BottomRight;
    const float left = right
        ? kLogicalWidth - cfg.margin_x - block_w
        : cfg.margin_x;
    /* margin_y counts to the digits themselves, not to the box around them. */
    const float top = bottom
        ? kLogicalHeight - cfg.margin_y - font * kTextBottom -
              static_cast<float>(lines - 1) * step
        : cfg.margin_y - font * kTextTop;

    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t c = column_of[i];
        OverlayRow& row = frame.rows[i];
        const float cell_x = left + column_x[c];
        const float colon = cell_x + name_w[c] + font * kNameToColon;
        const float number_edge =
            colon + font * kColonToNumber + number_w[c];
        row.label_x = cell_x;
        row.colon_x = colon;
        row.number_x =
            number_edge - estimate_text_width(row.value, cfg.font_size);
        row.y = top + static_cast<float>(spec.cells[i].line) * step;
    }
}

} // namespace common_fps
