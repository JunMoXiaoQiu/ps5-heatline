/*
 * SimpleFPS addition to Common FPS for PS5
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#include "common_fps/types.hpp"

#include <cstdint>

namespace common_fps {

/* Lowest, highest and average FPS since the last reset. */
class FpsStats {
public:
    void reset() noexcept { *this = FpsStats{}; }
    void add(int fps) noexcept;

    bool empty() const noexcept { return count_ == 0; }
    int min() const noexcept { return min_; }
    int max() const noexcept { return max_; }
    int average() const noexcept;

private:
    std::uint64_t sum_ = 0;
    std::uint64_t count_ = 0;
    int min_ = 0;
    int max_ = 0;
};

/* All values come from Heatline's own sampler (fps.c): the in-game overlay
 * displays the same numbers as the dashboard. avg/p1/p02 are the CapFrameX
 * real-time average, 1% low and 0.2% low; -1 means "no value yet" (window
 * still filling) and draws as "--". `stats` stays for the engine's own
 * Min/Max/Time items, which Heatline's overlay does not currently show. */
struct OverlayReadings {
    bool have_fps = false;
    int fps = 0;
    int avg = -1;
    int p1 = -1;
    int p02 = -1;
    FpsStats stats{};
    std::uint64_t session_seconds = 0;
};

/* Fills frame.rows and frame.row_count from frame.config and the readings. */
void build_overlay_rows(OverlayFrame& frame, const OverlayReadings& readings);

/* Single-line overlay: one row, one value widget (no label widget), holding
 * "FPS x  AVG x  P1 x  P0.2 x" coloured by the current FPS. This matches the
 * renderer's proven single-widget shape; multi-row drawing is deferred. */
void build_overlay_single_line(OverlayFrame& frame, const OverlayReadings& readings);

/* DIAGNOSTIC: two separately-coloured value widgets ("FPS : n" green, then
 * "AVG : n" cyan ~15 s later) to test whether creating a second Mono widget is
 * safe on the console, with the two creations spaced out in time. */
void build_overlay_metrics(OverlayFrame& frame, const OverlayReadings& readings);

} // namespace common_fps
