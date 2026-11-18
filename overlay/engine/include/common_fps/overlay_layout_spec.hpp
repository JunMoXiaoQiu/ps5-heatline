/*
 * Heatline in-game overlay: layout spec.
 *
 * A spec says which metrics the overlay shows and how they are arranged, as a
 * short text that the web page, the saved settings and the engine all share:
 *
 *   ';' separates lines (top to bottom), ',' separates cells on a line
 *   (left to right).
 *
 *   "FPS;AVG;P1;P0.2"       one metric per line (vertical)
 *   "FPS,AVG,P1,P0.2"       everything on one line (horizontal)
 *   "FPS,AVG;P1,P0.2"       two lines of two
 *
 * Metric names are the tokens in overlay_metrics.cpp. Unknown names are
 * skipped, so an old saved spec keeps working after a metric is renamed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "common_fps/overlay_metrics.hpp"
#include "common_fps/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace common_fps {

/* Total cells are bounded by the overlay's widget budget (kMaxOverlayRows). */
inline constexpr std::size_t kMaxLayoutCells = kMaxOverlayRows;
inline constexpr std::size_t kMaxLayoutSpecLength = 96;

struct LayoutCell {
    MetricId metric = MetricId::OVM_FPS;
    std::uint8_t line = 0;  // 0-based line this cell sits on
};

struct LayoutSpec {
    LayoutCell cells[kMaxLayoutCells]{};
    std::size_t cell_count = 0;
    std::size_t line_count = 0;
};

/* nullopt when the text holds no usable metric at all. */
std::optional<LayoutSpec> parse_layout_spec(std::string_view text);

/* The canonical text of a spec; parse_layout_spec(to_string(s)) == s. */
std::string layout_spec_to_string(const LayoutSpec& spec);

/* Ready-made arrangements of the four FPS metrics. */
LayoutSpec default_layout_spec();     // vertical: one metric per line
LayoutSpec horizontal_layout_spec();  // one line

} // namespace common_fps
