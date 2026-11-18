/*
 * SimpleFPS addition to Common FPS for PS5
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "common_fps/overlay_rows.hpp"

#include <algorithm>
#include <cstdio>

namespace common_fps {

namespace {

constexpr std::uint32_t kWhite = 0xffffffffU;
constexpr std::uint32_t kValueOrange = 0xffa726ffU;
constexpr std::uint32_t kGood = 0x4cd964ffU;
constexpr std::uint32_t kOk = 0xffd60affU;
constexpr std::uint32_t kBad = 0xff453affU;

constexpr std::uint32_t kFpsName = 0xb366ffffU; // the "FPS:" purple of Common FPS
constexpr std::uint32_t kFrameTimeName = 0x33b5ffffU;
constexpr std::uint32_t kStatsName = 0x4cd964ffU;
constexpr std::uint32_t kTimeName = 0xff7eb6ffU;

/* Replaces the alpha byte of an 0xRRGGBBAA colour. */
std::uint32_t with_alpha(std::uint32_t rgba, std::uint32_t alpha) {
    return (rgba & 0xffffff00U) | (alpha & 0xffU);
}

struct ItemStyle {
    const char* name;
    std::uint32_t name_rgba;
    std::uint32_t value_rgba;
};

ItemStyle style_of(OverlayItem item) {
    switch (item) {
        case OverlayItem::FrameTime: return {"MS", kFrameTimeName, kValueOrange};
        case OverlayItem::Min: return {"MIN", kStatsName, kValueOrange};
        case OverlayItem::Max: return {"MAX", kStatsName, kValueOrange};
        case OverlayItem::Avg: return {"AVG", kStatsName, kValueOrange};
        case OverlayItem::Time: return {"TIME", kTimeName, kValueOrange};
        case OverlayItem::P1: return {"P1", kFrameTimeName, kValueOrange};
        case OverlayItem::P02: return {"P0.2", kFrameTimeName, kValueOrange};
        case OverlayItem::Fps:
        default: return {"FPS", kFpsName, kWhite};
    }
}

void write_number(char* out, bool known, int number) {
    if (known)
        std::snprintf(out, kOverlayValueSize, "%d", number);
    else
        std::snprintf(out, kOverlayValueSize, "--");
}

/* Milliseconds per frame with one decimal, in integers: 60 -> "16.7". */
void write_frame_time(char* out, bool known, int fps) {
    if (!known || fps <= 0) {
        std::snprintf(out, kOverlayValueSize, "--");
        return;
    }
    const int tenths = std::min((10000 + fps / 2) / fps, 9999);
    std::snprintf(out, kOverlayValueSize, "%d.%d", tenths / 10, tenths % 10);
}

void write_duration(char* out, std::uint64_t seconds) {
    const unsigned long long hours = seconds / 3600U;
    const unsigned minutes = static_cast<unsigned>(seconds / 60U % 60U);
    const unsigned rest = static_cast<unsigned>(seconds % 60U);
    if (hours > 0) {
        std::snprintf(
            out, kOverlayValueSize, "%llu:%02u:%02u",
            std::min(hours, 999ULL), minutes, rest);
    } else {
        std::snprintf(out, kOverlayValueSize, "%u:%02u", minutes, rest);
    }
}

} // namespace

void FpsStats::add(int fps) noexcept {
    /* A stalled second (loading screen, suspended game) is not a frame rate. */
    if (fps <= 0)
        return;
    min_ = count_ == 0 ? fps : std::min(min_, fps);
    max_ = count_ == 0 ? fps : std::max(max_, fps);
    sum_ += static_cast<std::uint64_t>(fps);
    ++count_;
}

int FpsStats::average() const noexcept {
    return count_ == 0
        ? 0
        : static_cast<int>((sum_ + count_ / 2) / count_);
}

void build_overlay_rows(OverlayFrame& frame, const OverlayReadings& readings) {
    const OverlayConfig& cfg = frame.config;
    const std::size_t count =
        cfg.visible ? std::min(cfg.item_count, kMaxOverlayRows) : 0;
    /* auto: names tell several lines apart, and the fps number needs none. */
    const bool all_named = cfg.labels == LabelMode::On;
    const bool others_named = cfg.labels == LabelMode::Auto && count > 1;
    const bool have_stats = !readings.stats.empty();
    const std::uint32_t alpha = static_cast<std::uint32_t>(
        std::clamp(cfg.opacity, 1, 100) * 255 / 100);

    frame.row_count = count;
    for (std::size_t i = 0; i < count; ++i) {
        const OverlayItem item = cfg.items[i];
        const ItemStyle style = style_of(item);
        OverlayRow& row = frame.rows[i];
        row = OverlayRow{};
        row.label_rgba = with_alpha(style.name_rgba, alpha);
        row.value_rgba = with_alpha(style.value_rgba, alpha);
        if (all_named || (others_named && item != OverlayItem::Fps))
            std::snprintf(row.label, kOverlayLabelSize, "%s", style.name);

        /* Frame-rate-like value eligible for good/ok/bad threshold colour
         * (FPS, AVG, 1% low, 0.2% low). -1 means not applicable / unknown. */
        int rate_value = -1;
        switch (item) {
            case OverlayItem::Fps:
                if (readings.have_fps) rate_value = readings.fps;
                write_number(row.value, readings.have_fps, readings.fps);
                break;
            case OverlayItem::Avg:
                if (readings.avg >= 0) rate_value = readings.avg;
                write_number(row.value, readings.avg >= 0, readings.avg);
                break;
            case OverlayItem::P1:
                if (readings.p1 >= 0) rate_value = readings.p1;
                write_number(row.value, readings.p1 >= 0, readings.p1);
                break;
            case OverlayItem::P02:
                if (readings.p02 >= 0) rate_value = readings.p02;
                write_number(row.value, readings.p02 >= 0, readings.p02);
                break;
            case OverlayItem::FrameTime:
                write_frame_time(row.value, readings.have_fps, readings.fps);
                break;
            case OverlayItem::Min:
                write_number(row.value, have_stats, readings.stats.min());
                break;
            case OverlayItem::Max:
                write_number(row.value, have_stats, readings.stats.max());
                break;
            case OverlayItem::Time:
                write_duration(row.value, readings.session_seconds);
                break;
        }
        if (cfg.color && rate_value >= 0) {
            row.value_rgba = with_alpha(
                rate_value >= cfg.fps_good ? kGood
                : rate_value >= cfg.fps_ok ? kOk
                : kBad,
                alpha);
        }
    }
}

void build_overlay_single_line(OverlayFrame& frame, const OverlayReadings& readings) {
    const OverlayConfig& cfg = frame.config;
    frame.row_count = cfg.visible ? 1 : 0;
    if (!cfg.visible)
        return;

    const std::uint32_t alpha = static_cast<std::uint32_t>(
        std::clamp(cfg.opacity, 1, 100) * 255 / 100);

    OverlayRow& row = frame.rows[0];
    row = OverlayRow{};
    row.label[0] = '\0';   // no label widget: a single value widget only

    char f[12], a[12], p1[12], p02[12];
    if (readings.have_fps) std::snprintf(f, sizeof f, "%d", readings.fps);
    else                   std::snprintf(f, sizeof f, "--");
    if (readings.avg >= 0) std::snprintf(a, sizeof a, "%d", readings.avg);
    else                   std::snprintf(a, sizeof a, "--");
    if (readings.p1 >= 0)  std::snprintf(p1, sizeof p1, "%d", readings.p1);
    else                   std::snprintf(p1, sizeof p1, "--");
    if (readings.p02 >= 0) std::snprintf(p02, sizeof p02, "%d", readings.p02);
    else                   std::snprintf(p02, sizeof p02, "--");
    std::snprintf(row.value, kOverlayValueSize,
        "FPS %s  AVG %s  P1 %s  P0.2 %s", f, a, p1, p02);

    /* Whole-line colour by the current FPS (per-metric colour needs the
     * multi-widget path, deferred until the renderer handles it safely). */
    std::uint32_t col = kWhite;
    if (cfg.color && readings.have_fps) {
        col = readings.fps >= cfg.fps_good ? kGood
            : readings.fps >= cfg.fps_ok ? kOk
            : kBad;
    }
    row.value_rgba = with_alpha(col, alpha);
    row.label_rgba = with_alpha(kWhite, alpha);
}

void build_overlay_metrics(OverlayFrame& frame, const OverlayReadings& readings) {
    const OverlayConfig& cfg = frame.config;
    if (!cfg.visible) {
        frame.row_count = 0;
        return;
    }
    const std::uint32_t alpha = static_cast<std::uint32_t>(
        std::clamp(cfg.opacity, 1, 100) * 255 / 100);

    constexpr std::uint32_t kValueColor = 0xffffffffU;  // white (number)

    /* Name colours: FPS green, AVG cyan, P1 orange, P0.2 red. Numbers white.
     * All four rows are always sent; the renderer creates one widget per
     * frame, so they appear one at a time in order. */
    struct Spec { const char* name; std::uint32_t color; int value; bool have; };
    const Spec specs[4] = {
        {"FPS",  0x4cd964ffU, readings.fps,  readings.have_fps},
        {"AVG",  0x33d6e0ffU, readings.avg,  readings.avg >= 0},
        {"P1",   0xffa733ffU, readings.p1,   readings.p1 >= 0},
        {"P0.2", 0xff5a4fffU, readings.p02,  readings.p02 >= 0},
    };
    for (std::size_t i = 0; i < 4; ++i) {
        OverlayRow& r = frame.rows[i];
        r = OverlayRow{};
        std::snprintf(r.label, kOverlayLabelSize, "%s", specs[i].name);
        r.label_rgba = with_alpha(specs[i].color, alpha);
        if (specs[i].have) std::snprintf(r.value, kOverlayValueSize, "%d", specs[i].value);
        else               std::snprintf(r.value, kOverlayValueSize, "--");
        r.value_rgba = with_alpha(kValueColor, alpha);
    }
    frame.row_count = 4;
}

} // namespace common_fps
