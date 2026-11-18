/*
 * Heatline in-game overlay: layout spec. See overlay_layout_spec.hpp.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "common_fps/overlay_layout_spec.hpp"

namespace common_fps {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

} // namespace

std::optional<LayoutSpec> parse_layout_spec(std::string_view text) {
    if (text.size() > kMaxLayoutSpecLength)
        return std::nullopt;

    LayoutSpec spec;
    std::size_t line = 0;       // index of the line being filled
    bool line_has_cell = false; // an empty line is dropped, not kept blank

    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find_first_of(";,", pos);
        if (end == std::string_view::npos)
            end = text.size();
        const std::string_view token = trim(text.substr(pos, end - pos));

        MetricId id{};
        if (!token.empty() && metric_from_token(token, id) &&
            spec.cell_count < kMaxLayoutCells) {
            spec.cells[spec.cell_count].metric = id;
            spec.cells[spec.cell_count].line = static_cast<std::uint8_t>(line);
            ++spec.cell_count;
            line_has_cell = true;
        }

        if (end < text.size() && text[end] == ';' && line_has_cell) {
            ++line;
            line_has_cell = false;
        }
        pos = end + 1;
    }

    if (spec.cell_count == 0)
        return std::nullopt;
    spec.line_count = static_cast<std::size_t>(
        spec.cells[spec.cell_count - 1].line) + 1;
    return spec;
}

std::string layout_spec_to_string(const LayoutSpec& spec) {
    std::string out;
    for (std::size_t i = 0; i < spec.cell_count; ++i) {
        if (i > 0)
            out += spec.cells[i].line != spec.cells[i - 1].line ? ';' : ',';
        out += metric_info(spec.cells[i].metric).token;
    }
    return out;
}

LayoutSpec default_layout_spec() {
    return *parse_layout_spec("FPS;AVG;P1;P0.2");
}

LayoutSpec horizontal_layout_spec() {
    return *parse_layout_spec("FPS,AVG,P1,P0.2");
}

} // namespace common_fps
