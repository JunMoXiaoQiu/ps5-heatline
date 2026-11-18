/*
 * Heatline in-game overlay: metric registry.
 *
 * Every metric the overlay can show is one row of the table in
 * overlay_metrics.cpp: its token (used in layout specs and the web UI), its
 * on-screen name, its default colour and how its number is written. Adding a
 * metric to the overlay is: add one line to overlay_metric_list.h and fill its
 * value in Heatline's overlay_collect_metrics. The layout code never changes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "common_fps/overlay_metric_list.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace common_fps {

struct OverlayReadings;

/* One enumerator per metric in overlay_metric_list.h, named OVM_<ID>. */
enum class MetricId : std::uint8_t {
#define COMMON_FPS_X(id, token, label, color, dec, suffix, widest) OVM_##id,
    OVERLAY_METRIC_LIST(COMMON_FPS_X)
#undef COMMON_FPS_X
    Count, // keep last
};

inline constexpr std::size_t kMetricCount =
    static_cast<std::size_t>(MetricId::Count);

struct MetricInfo {
    MetricId id;
    const char* token;     // what a layout spec calls it, e.g. "P0.2"
    const char* label;     // what the overlay draws as its name
    std::uint32_t color;   // default name colour, 0xRRGGBBAA
    int decimals;          // digits after the point
    const char* suffix;    // appended to the number, "" for none
    const char* widest;    // a typical widest value; reserves the column width
};

const MetricInfo& metric_info(MetricId id);

/* Case-insensitive; also accepts a few aliases ("P02" for "P0.2"). */
bool metric_from_token(std::string_view token, MetricId& out);

/* The current reading of every metric. A metric without a value yet (a window
 * still filling, a sensor not read) has have == false and draws as "--". */
struct MetricValues {
    bool have[kMetricCount]{};
    double value[kMetricCount]{};
};

/* Writes the number text for one metric ("104", "62.5C", or "--"). */
void format_metric_value(
    MetricId id, const MetricValues& values, char* out, std::size_t size);

/* The FPS-engine readings (FPS, AVG, P1, P0.2) as metric values. */
MetricValues metric_values_from_readings(const OverlayReadings& readings);

/* Heatline's collector, which fills `value`/`have` (kMetricCount entries each)
 * for every metric it can read right now. Provided by the Heatline build; null
 * in a standalone engine build, where only the FPS-engine readings exist. */
using MetricCollector = int (*)(double* value, unsigned char* have, int count);

} // namespace common_fps
