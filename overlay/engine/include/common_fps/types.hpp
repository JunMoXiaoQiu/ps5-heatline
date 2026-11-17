/*
 * Common FPS for PS5
 * Copyright (C) 2026 porhe911
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace common_fps {

using ProcessId = int;

enum class Corner {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

struct Vec2 {
    float x{};
    float y{};
};

/* One line of the overlay, top to bottom in the order of "show=". */
enum class OverlayItem : std::uint8_t {
    Fps,
    FrameTime,
    Min,
    Max,
    Avg,
    Time,
    P1,   // 1% low  (1st-percentile FPS over the session)
    P02,  // 0.2% low (0.2-percentile FPS over the session)
};

enum class LabelMode : std::uint8_t {
    Auto, // names when more than one line is shown, never on the fps line
    On,
    Off,
};

inline constexpr std::size_t kMaxOverlayRows = 8;
inline constexpr std::size_t kOverlayLabelSize = 8;
/* Wide enough for the whole single-line overlay ("FPS x  AVG x  P1 x  P0.2 x")
 * in one value widget. The renderer's TextWidget cache and the wire packet both
 * size off this, and the Mono text set is length-based, so growing it is safe. */
inline constexpr std::size_t kOverlayValueSize = 64;

struct OverlayConfig {
    Corner corner = Corner::TopLeft;
    int font_size = 24;
    float margin_x = 10.0f;
    float margin_y = 10.0f;

    bool visible = true;
    OverlayItem items[kMaxOverlayRows] = {OverlayItem::Fps};
    std::size_t item_count = 1;
    LabelMode labels = LabelMode::Auto;
    bool color = false;
    int fps_good = 55;
    int fps_ok = 30;
    int opacity = 100;      // of the text, in percent
    int rotate_minutes = 0; // move to the next corner this often; 0 stays put
};

/* Text and 0xRRGGBBAA colours, ready to draw. An empty label draws nothing. */
struct OverlayRow {
    char label[kOverlayLabelSize]{};
    char value[kOverlayValueSize]{};
    std::uint32_t label_rgba = 0xffffffffU;
    std::uint32_t value_rgba = 0xffffffffU;
};

struct OverlayFrame {
    bool visible = false;
    bool loading = true;
    int fps = 0;
    OverlayConfig config{};
    Vec2 anchor{};
    std::size_t row_count = 0;
    OverlayRow rows[kMaxOverlayRows]{};
};

} // namespace common_fps
