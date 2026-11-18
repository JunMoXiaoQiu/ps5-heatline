/*
 * Common FPS for PS5
 * Copyright (C) 2026 porhe911
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "common_fps/wire.hpp"
#include "common_fps/constants.hpp"
#include "common_fps/layout.hpp"

#include <algorithm>

namespace common_fps {

/* The packet is never trusted: an odd value becomes the default. */
static float decode_margin(float value) {
    if (value != value)
        return 0.0f;
    return std::clamp(value, kMinMargin, kMaxMargin);
}

/* A position from the packet: NaN becomes 0, the rest is clamped to the screen
 * (with a little room, since the text box starts above the digits). */
static float decode_position(float value, float limit) {
    if (value != value)
        return 0.0f;
    return std::clamp(value, -limit, limit);
}

static std::uint8_t encode_corner(Corner corner) {
    switch (corner) {
        case Corner::TopLeft: return 0;
        case Corner::TopRight: return 1;
        case Corner::BottomLeft: return 2;
        case Corner::BottomRight: return 3;
    }
    return 2;
}

static Corner decode_corner(std::uint8_t corner) {
    switch (corner) {
        case 0: return Corner::TopLeft;
        case 1: return Corner::TopRight;
        case 3: return Corner::BottomRight;
        case 2:
        default: return Corner::BottomLeft;
    }
}

WirePacket make_wire_packet(
    const OverlayFrame& frame,
    std::uint64_t sequence) {

    WirePacket packet;
    packet.sequence = sequence;
    packet.fps = frame.fps;
    packet.loading = frame.loading ? 1 : 0;
    packet.corner = encode_corner(frame.config.corner);
    packet.font_size = static_cast<std::uint16_t>(
        std::clamp(frame.config.font_size, kMinFontSize, kMaxFontSize));
    packet.margin_x = frame.config.margin_x;
    packet.margin_y = frame.config.margin_y;

    const std::size_t rows = frame.visible
        ? std::min(frame.row_count, kMaxOverlayRows)
        : 0;
    packet.row_count = static_cast<std::uint8_t>(rows);
    packet.explicit_layout = frame.explicit_layout ? 1 : 0;
    for (std::size_t i = 0; i < rows; ++i)
        packet.rows[i] = frame.rows[i];
    return packet;
}

WirePacket make_shutdown_wire_packet(std::uint64_t sequence) {
    WirePacket packet{};
    packet.magic = kWireShutdownMagic;
    packet.sequence = sequence;
    packet.loading = 1;
    return packet;
}

bool is_shutdown_wire_packet(const WirePacket& packet) noexcept {
    return packet.magic == kWireShutdownMagic &&
        packet.version == kWireVersion &&
        packet.size == sizeof(WirePacket);
}

std::optional<OverlayFrame>
decode_wire_packet(const WirePacket& packet) {
    if (packet.magic != kWireMagic ||
        packet.version != kWireVersion ||
        packet.size != sizeof(WirePacket)) {
        return std::nullopt;
    }

    OverlayFrame frame;
    frame.visible = true;
    frame.loading = packet.loading != 0;
    frame.fps = packet.fps;
    frame.config.corner = decode_corner(packet.corner);
    frame.config.font_size = std::clamp(
        static_cast<int>(packet.font_size),
        kMinFontSize,
        kMaxFontSize);
    frame.config.margin_x = decode_margin(packet.margin_x);
    frame.config.margin_y = decode_margin(packet.margin_y);
    frame.anchor = compute_anchor(frame.config);

    frame.row_count = std::min<std::size_t>(packet.row_count, kMaxOverlayRows);
    frame.explicit_layout = packet.explicit_layout != 0;
    for (std::size_t i = 0; i < frame.row_count; ++i) {
        frame.rows[i] = packet.rows[i];
        /* The text is handed to Mono as a C string: never trust the sender. */
        frame.rows[i].label[kOverlayLabelSize - 1] = '\0';
        frame.rows[i].value[kOverlayValueSize - 1] = '\0';
        /* Positions are never trusted either: a bad float becomes 0 and the
         * rest stays on the logical screen. */
        OverlayRow& row = frame.rows[i];
        row.label_x = decode_position(row.label_x, kLogicalWidth);
        row.colon_x = decode_position(row.colon_x, kLogicalWidth);
        row.number_x = decode_position(row.number_x, kLogicalWidth);
        row.y = decode_position(row.y, kLogicalHeight);
    }
    return frame;
}

} // namespace common_fps
