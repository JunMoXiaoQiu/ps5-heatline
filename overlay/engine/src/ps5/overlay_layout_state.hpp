/*
 * Heatline in-game overlay: the layout currently wanted.
 *
 * Heatline's web page sets it (through overlay_set_layout, a C function); the
 * engine worker reads it every tick. Both run in one process on different
 * threads, so it sits behind a mutex. An invalid spec is refused and the
 * previous layout stays in force.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "common_fps/overlay_layout_spec.hpp"

namespace common_fps::ps5 {

/* The layout to draw now. Defaults to the vertical FPS/AVG/P1/P0.2 stack. */
LayoutSpec current_layout_spec();

/* Where and how big the overlay is. Set from Heatline's web page, read by the
 * engine every tick; always within the limits below. */
inline constexpr int kStyleMinFont = 16, kStyleMaxFont = 40;
inline constexpr int kStyleMaxMargin = 400;
inline constexpr int kStyleMinOpacity = 20, kStyleMaxOpacity = 100;

struct OverlayStyle {
    Corner corner = Corner::TopLeft;
    int margin_x = 10;
    int margin_y = 10;
    int font_size = 24;
    int opacity = 100;
};
OverlayStyle current_overlay_style();

} // namespace common_fps::ps5

extern "C" {
/* Parses and applies a spec. 0 on success; -1 and no change if it holds no
 * usable metric. */
int overlay_set_layout(const char* spec);
/* Writes the canonical text of the current layout; returns its length. */
int overlay_get_layout(char* out, unsigned long size);

/* Sets the overlay's position and size. corner: 0 top-left, 1 top-right,
 * 2 bottom-left, 3 bottom-right. Every value is clamped to its limit; returns
 * 0, or -1 if the corner is not 0..3 (nothing changes). */
int overlay_set_style(int corner, int margin_x, int margin_y,
                      int font_size, int opacity);
/* out[5] = corner, margin_x, margin_y, font_size, opacity. */
void overlay_get_style(int out[5]);
}
