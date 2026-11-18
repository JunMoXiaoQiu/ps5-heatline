/*
 * Heatline in-game overlay: the layout currently wanted. See the header.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "overlay_layout_state.hpp"

#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>

namespace {

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
common_fps::LayoutSpec g_spec = common_fps::default_layout_spec();
common_fps::ps5::OverlayStyle g_style{};

int clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

} // namespace

namespace common_fps::ps5 {

LayoutSpec current_layout_spec() {
    pthread_mutex_lock(&g_lock);
    const LayoutSpec copy = g_spec;
    pthread_mutex_unlock(&g_lock);
    return copy;
}

OverlayStyle current_overlay_style() {
    pthread_mutex_lock(&g_lock);
    const OverlayStyle copy = g_style;
    pthread_mutex_unlock(&g_lock);
    return copy;
}

} // namespace common_fps::ps5

extern "C" int overlay_set_style(int corner, int margin_x, int margin_y,
                                 int font_size, int opacity) {
    using namespace common_fps::ps5;
    if (corner < 0 || corner > 3)
        return -1;
    OverlayStyle style;
    style.corner = static_cast<common_fps::Corner>(corner);
    style.margin_x = clamp_int(margin_x, 0, kStyleMaxMargin);
    style.margin_y = clamp_int(margin_y, 0, kStyleMaxMargin);
    style.font_size = clamp_int(font_size, kStyleMinFont, kStyleMaxFont);
    style.opacity = clamp_int(opacity, kStyleMinOpacity, kStyleMaxOpacity);
    pthread_mutex_lock(&g_lock);
    g_style = style;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

extern "C" void overlay_get_style(int out[5]) {
    const auto s = common_fps::ps5::current_overlay_style();
    out[0] = static_cast<int>(s.corner);
    out[1] = s.margin_x;
    out[2] = s.margin_y;
    out[3] = s.font_size;
    out[4] = s.opacity;
}

extern "C" int overlay_set_layout(const char* spec) {
    if (!spec)
        return -1;
    const auto parsed = common_fps::parse_layout_spec(spec);
    if (!parsed)
        return -1;
    pthread_mutex_lock(&g_lock);
    g_spec = *parsed;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

extern "C" int overlay_get_layout(char* out, unsigned long size) {
    if (!out || size == 0)
        return 0;
    pthread_mutex_lock(&g_lock);
    const std::string text = common_fps::layout_spec_to_string(g_spec);
    pthread_mutex_unlock(&g_lock);
    std::snprintf(out, size, "%s", text.c_str());
    return static_cast<int>(std::strlen(out));
}
