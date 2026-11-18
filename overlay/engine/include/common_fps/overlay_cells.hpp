/*
 * Heatline in-game overlay: cell layout.
 *
 * Turns a LayoutSpec plus the current metric values into finished rows: the
 * text, the colours and the position of every name, colon and number. All
 * layout decisions live here (on the host-testable side); the ShellUI renderer
 * only places what it is given.
 *
 * Each cell is   NAME : NUMBER   with the colon at a fixed distance from the
 * name column and the number right-aligned in a column wide enough for three
 * digits, so a changing value never moves anything. Cells in the same position
 * on different lines share a column, which keeps a vertical stack aligned.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "common_fps/overlay_layout_spec.hpp"
#include "common_fps/overlay_metrics.hpp"
#include "common_fps/types.hpp"

namespace common_fps {

/*
 * Fills frame.rows, frame.row_count and frame.explicit_layout from the spec and
 * the values, using frame.config for corner, font size, margins and opacity.
 * Rows come out in spec order (line by line, left to right).
 */
void build_overlay_from_spec(
    OverlayFrame& frame, const LayoutSpec& spec, const MetricValues& values);

} // namespace common_fps
