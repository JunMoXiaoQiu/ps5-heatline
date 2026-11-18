/*
 * Common FPS for PS5
 * Copyright (C) 2026 porhe911
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "common_fps/types.hpp"

#include <cstdint>
#include <optional>

namespace common_fps {

inline constexpr std::uint32_t kWireMagic = 0x53465043; // "CFPS"
inline constexpr std::uint32_t kWireShutdownMagic = 0x58465043; // "CFPX"
inline constexpr std::uint16_t kWireVersion = 3; // 3: rows carry their own positions (explicit_layout)
inline constexpr std::uint16_t kDefaultIpcPort = 39028;

#pragma pack(push, 1)
struct WirePacket {
    std::uint32_t magic = kWireMagic;
    std::uint16_t version = kWireVersion;
    std::uint16_t size = sizeof(WirePacket);

    std::uint64_t sequence = 0;

    std::int32_t fps = 0;
    std::uint8_t loading = 1;
    std::uint8_t corner = 0; // TopLeft
    std::uint16_t font_size = 24;

    float margin_x = 10.0f;
    float margin_y = 10.0f;

    std::uint8_t row_count = 0;
    std::uint8_t explicit_layout = 0; // 1: rows carry label_x/colon_x/number_x/y
    std::uint8_t reserved[2]{};
    OverlayRow rows[kMaxOverlayRows]{};
};
#pragma pack(pop)

static_assert(sizeof(OverlayRow) ==
    kOverlayLabelSize + kOverlayValueSize + 2 * sizeof(std::uint32_t) +
    4 * sizeof(float));
static_assert(sizeof(WirePacket) == 36 + sizeof(OverlayRow) * kMaxOverlayRows);

WirePacket make_wire_packet(
    const OverlayFrame& frame,
    std::uint64_t sequence);

/* Reserved controller-to-ShellUI quiesce packet; v1.2.0 does not send it. */
WirePacket make_shutdown_wire_packet(std::uint64_t sequence);

[[nodiscard]] bool
is_shutdown_wire_packet(const WirePacket& packet) noexcept;

std::optional<OverlayFrame>
decode_wire_packet(const WirePacket& packet);

} // namespace common_fps
