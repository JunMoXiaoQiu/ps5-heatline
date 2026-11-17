/*
 * Common FPS for PS5
 * Copyright (C) 2026 porhe911
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "common_fps/config.hpp"
#include "common_fps/constants.hpp"

#include <algorithm>

namespace common_fps {

namespace {

constexpr int kMinFpsThreshold = 1;
constexpr int kMaxFpsThreshold = 240;
constexpr int kMinOpacity = 10;
constexpr int kMaxOpacity = 100;
constexpr int kMaxRotateMinutes = 24 * 60;

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && is_space(value.front()))
        value.remove_prefix(1);
    while (!value.empty() && is_space(value.back()))
        value.remove_suffix(1);
    return value;
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/* Case-insensitive; '-' and ' ' in the input count as '_'. */
bool same_word(std::string_view input, std::string_view word) {
    if (input.size() != word.size())
        return false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = lower(input[i]);
        if (c == '-' || c == ' ')
            c = '_';
        if (c != word[i])
            return false;
    }
    return true;
}

/* Plain decimal such as "24", "-6" or "10.5"; anything else is rejected. */
bool parse_number(std::string_view value, double& out) {
    const bool negative = !value.empty() && value.front() == '-';
    if (negative)
        value.remove_prefix(1);
    if (value.empty() || value.size() > 9)
        return false;

    double whole = 0.0;
    double fraction = 0.0;
    double scale = 1.0;
    bool digits = false;
    bool after_point = false;

    for (const char c : value) {
        if (c == '.' && !after_point) {
            after_point = true;
        } else if (c >= '0' && c <= '9') {
            digits = true;
            if (after_point) {
                scale /= 10.0;
                fraction += (c - '0') * scale;
            } else {
                whole = whole * 10.0 + (c - '0');
            }
        } else {
            return false;
        }
    }

    if (!digits)
        return false;
    out = negative ? -(whole + fraction) : whole + fraction;
    return true;
}

bool parse_switch(std::string_view value, bool& out) {
    if (same_word(value, "1") || same_word(value, "on") ||
        same_word(value, "yes") || same_word(value, "true")) {
        out = true;
        return true;
    }
    if (same_word(value, "0") || same_word(value, "off") ||
        same_word(value, "no") || same_word(value, "false")) {
        out = false;
        return true;
    }
    return false;
}

bool parse_item(std::string_view value, OverlayItem& out) {
    if (same_word(value, "fps")) {
        out = OverlayItem::Fps;
    } else if (same_word(value, "frametime") ||
               same_word(value, "frame_time") || same_word(value, "ms")) {
        out = OverlayItem::FrameTime;
    } else if (same_word(value, "min")) {
        out = OverlayItem::Min;
    } else if (same_word(value, "max")) {
        out = OverlayItem::Max;
    } else if (same_word(value, "avg") || same_word(value, "average")) {
        out = OverlayItem::Avg;
    } else if (same_word(value, "time")) {
        out = OverlayItem::Time;
    } else {
        return false;
    }
    return true;
}

void parse_show(std::string_view value, OverlayConfig& cfg) {
    OverlayItem items[kMaxOverlayRows]{};
    std::size_t count = 0;

    while (!value.empty()) {
        const std::size_t comma = value.find(',');
        const std::string_view token = trim(value.substr(0, comma));
        value = comma == std::string_view::npos
            ? std::string_view{}
            : value.substr(comma + 1);

        OverlayItem item{};
        if (!parse_item(token, item) || count == kMaxOverlayRows)
            continue;
        if (std::find(items, items + count, item) == items + count)
            items[count++] = item;
    }

    /* Nothing usable on the line: keep what was there. */
    if (count == 0)
        return;
    std::copy(items, items + count, cfg.items);
    cfg.item_count = count;
}

void apply_setting(
    std::string_view key,
    std::string_view value,
    OverlayConfig& cfg) {

    double number = 0.0;

    if (same_word(key, "corner")) {
        if (same_word(value, "top_left"))
            cfg.corner = Corner::TopLeft;
        else if (same_word(value, "top_right"))
            cfg.corner = Corner::TopRight;
        else if (same_word(value, "bottom_left"))
            cfg.corner = Corner::BottomLeft;
        else if (same_word(value, "bottom_right"))
            cfg.corner = Corner::BottomRight;
    } else if (same_word(key, "show")) {
        parse_show(value, cfg);
    } else if (same_word(key, "labels")) {
        bool on = false;
        if (same_word(value, "auto"))
            cfg.labels = LabelMode::Auto;
        else if (parse_switch(value, on))
            cfg.labels = on ? LabelMode::On : LabelMode::Off;
    } else if (same_word(key, "color") || same_word(key, "colour")) {
        (void)parse_switch(value, cfg.color);
    } else if (same_word(key, "visible")) {
        (void)parse_switch(value, cfg.visible);
    } else if (same_word(key, "font_size")) {
        if (parse_number(value, number)) {
            cfg.font_size = std::clamp(
                static_cast<int>(number), kMinFontSize, kMaxFontSize);
        }
    } else if (same_word(key, "margin_x")) {
        if (parse_number(value, number)) {
            cfg.margin_x = std::clamp(
                static_cast<float>(number), kMinMargin, kMaxMargin);
        }
    } else if (same_word(key, "margin_y")) {
        if (parse_number(value, number)) {
            cfg.margin_y = std::clamp(
                static_cast<float>(number), kMinMargin, kMaxMargin);
        }
    } else if (same_word(key, "fps_good")) {
        if (parse_number(value, number)) {
            cfg.fps_good = std::clamp(
                static_cast<int>(number), kMinFpsThreshold, kMaxFpsThreshold);
        }
    } else if (same_word(key, "fps_ok")) {
        if (parse_number(value, number)) {
            cfg.fps_ok = std::clamp(
                static_cast<int>(number), kMinFpsThreshold, kMaxFpsThreshold);
        }
    } else if (same_word(key, "opacity")) {
        if (parse_number(value, number)) {
            cfg.opacity = std::clamp(
                static_cast<int>(number), kMinOpacity, kMaxOpacity);
        }
    } else if (same_word(key, "rotate")) {
        if (parse_number(value, number)) {
            cfg.rotate_minutes = std::clamp(
                static_cast<int>(number), 0, kMaxRotateMinutes);
        }
    }
}

} // namespace

const char* corner_name(Corner corner) {
    switch (corner) {
        case Corner::TopRight: return "top_right";
        case Corner::BottomLeft: return "bottom_left";
        case Corner::BottomRight: return "bottom_right";
        case Corner::TopLeft:
        default: return "top_left";
    }
}

const char* item_name(OverlayItem item) {
    switch (item) {
        case OverlayItem::FrameTime: return "frametime";
        case OverlayItem::Min: return "min";
        case OverlayItem::Max: return "max";
        case OverlayItem::Avg: return "avg";
        case OverlayItem::Time: return "time";
        case OverlayItem::Fps:
        default: return "fps";
    }
}

Corner rotated_corner(Corner start, unsigned steps) {
    constexpr Corner clockwise[] = {
        Corner::TopLeft,
        Corner::TopRight,
        Corner::BottomRight,
        Corner::BottomLeft,
    };
    unsigned index = 0;
    while (index < 3 && clockwise[index] != start)
        ++index;
    return clockwise[(index + steps) % 4];
}

OverlayConfig default_config() {
    return {};
}

OverlayConfig parse_config_text(std::string_view text) {
    OverlayConfig cfg = default_config();

    /* A text editor may leave a UTF-8 byte order mark in front. */
    if (text.substr(0, 3) == "\xEF\xBB\xBF")
        text.remove_prefix(3);

    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos
            ? std::string_view{}
            : text.substr(newline + 1);

        /* No value contains '#' or ';', so both always start a comment. */
        line = trim(line.substr(0, line.find_first_of("#;")));
        if (line.empty() || line.front() == '[')
            continue;

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;

        apply_setting(
            trim(line.substr(0, equals)),
            trim(line.substr(equals + 1)),
            cfg);
    }

    return cfg;
}

const char* default_config_text() {
    return
        "# SimpleFPS settings\n"
        "# Change a value and save. The overlay follows within a few seconds.\n"
        "# Delete this file to get the defaults back.\n"
        "\n"
        "# Corner: top_left, top_right, bottom_left, bottom_right\n"
        "corner=top_left\n"
        "\n"
        "# What to show, top to bottom, separated by commas:\n"
        "#   fps        frames per second\n"
        "#   frametime  milliseconds per frame\n"
        "#   min        lowest fps since the game started\n"
        "#   max        highest fps since the game started\n"
        "#   avg        average fps since the game started\n"
        "#   time       time since the game started\n"
        "# Example: show=fps,frametime,min,max,avg,time\n"
        "show=fps\n"
        "\n"
        "# Names in front of the values: auto, on, off\n"
        "# auto = on the extra lines only, the fps number stays bare\n"
        "labels=auto\n"
        "\n"
        "# color=1 paints the fps number: green from fps_good up,\n"
        "# yellow from fps_ok up, red below. color=0 keeps it white.\n"
        "color=0\n"
        "fps_good=55\n"
        "fps_ok=30\n"
        "\n"
        "# Text size, 18 to 36\n"
        "font_size=24\n"
        "\n"
        "# Distance from the screen edges to the text, -100 to 400\n"
        "margin_x=10\n"
        "margin_y=10\n"
        "\n"
        "# visible=0 hides the overlay, visible=1 shows it\n"
        "visible=1\n"
        "\n"
        "# For OLED screens. Both are off by default.\n"
        "# opacity: how strong the text is, 10 to 100. At 50 the pixels\n"
        "# under the number work half as hard.\n"
        "opacity=100\n"
        "# rotate=N moves the overlay to the next corner every N minutes,\n"
        "# clockwise from the corner above, so no spot wears alone.\n"
        "# 0 keeps it still. Saving this file starts again from that corner.\n"
        "rotate=0\n";
}

} // namespace common_fps
