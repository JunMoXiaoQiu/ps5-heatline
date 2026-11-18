/*
 * Heatline in-game overlay: metric registry. See overlay_metrics.hpp.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "common_fps/overlay_metrics.hpp"
#include "common_fps/overlay_rows.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace common_fps {

namespace {

/* One row per metric, generated from overlay_metric_list.h (same order as the
 * enum, which is what lets MetricId index this table). */
constexpr MetricInfo kMetrics[kMetricCount] = {
#define COMMON_FPS_X(id, token, label, color, dec, suffix, widest) \
    {MetricId::OVM_##id, token, label, color, dec, suffix, widest},
    OVERLAY_METRIC_LIST(COMMON_FPS_X)
#undef COMMON_FPS_X
};

/* A label longer than the wire's name field would be cut off on screen. */
constexpr bool labels_fit() {
    for (const MetricInfo& m : kMetrics) {
        std::size_t n = 0;
        while (m.label[n] != '\0')
            ++n;
        if (n >= kOverlayLabelSize)
            return false;
    }
    return true;
}
static_assert(labels_fit(), "a metric label is too long for the overlay wire");

bool same_token(std::string_view a, const char* b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n)
        return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

} // namespace

const MetricInfo& metric_info(MetricId id) {
    const std::size_t i = static_cast<std::size_t>(id);
    return kMetrics[i < kMetricCount ? i : 0];
}

bool metric_from_token(std::string_view token, MetricId& out) {
    for (const MetricInfo& m : kMetrics) {
        if (same_token(token, m.token)) {
            out = m.id;
            return true;
        }
    }
    /* A few alternative spellings. */
    struct Alias { const char* name; MetricId id; };
    static const Alias kAliases[] = {
        {"P02", MetricId::OVM_P02}, {"P01", MetricId::OVM_P01},
        {"PING", MetricId::OVM_LATNET}, {"ROUTER", MetricId::OVM_LATGW},
        {"CPU", MetricId::OVM_LOAD},
    };
    for (const Alias& a : kAliases) {
        if (same_token(token, a.name)) {
            out = a.id;
            return true;
        }
    }
    return false;
}

void format_metric_value(
    MetricId id, const MetricValues& values, char* out, std::size_t size) {

    if (size == 0)
        return;
    const std::size_t i = static_cast<std::size_t>(id);
    if (i >= kMetricCount || !values.have[i]) {
        std::snprintf(out, size, "--");
        return;
    }
    const MetricInfo& info = kMetrics[i];
    std::snprintf(out, size, "%.*f%s", info.decimals, values.value[i], info.suffix);
}

MetricValues metric_values_from_readings(const OverlayReadings& r) {
    MetricValues v;
    auto set = [&v](MetricId id, bool have, double value) {
        const std::size_t i = static_cast<std::size_t>(id);
        v.have[i] = have;
        v.value[i] = value;
    };
    set(MetricId::OVM_FPS, r.have_fps, r.fps);
    set(MetricId::OVM_AVG, r.avg >= 0, r.avg);
    set(MetricId::OVM_P1,  r.p1 >= 0,  r.p1);
    set(MetricId::OVM_P02, r.p02 >= 0, r.p02);
    return v;
}

} // namespace common_fps
