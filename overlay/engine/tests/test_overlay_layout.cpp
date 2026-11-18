// Host tests for the overlay metric registry, layout spec and cell layout.
#include "common_fps/constants.hpp"
#include "common_fps/overlay_cells.hpp"
#include "common_fps/overlay_layout_spec.hpp"
#include "common_fps/overlay_metrics.hpp"
#include "common_fps/overlay_rows.hpp"
#include "common_fps/wire.hpp"

#include <cmath>
#include <string>
#include <cstdio>
#include <cstring>

using namespace common_fps;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static MetricValues sample() {
    OverlayReadings r;
    r.have_fps = true; r.fps = 104; r.avg = 101; r.p1 = 89; r.p02 = 86;
    return metric_values_from_readings(r);
}

static OverlayFrame frame_for(Corner corner = Corner::TopLeft) {
    OverlayFrame f;
    f.config.corner = corner;
    f.config.font_size = 24;
    f.config.margin_x = 10; f.config.margin_y = 10;
    f.config.visible = true; f.config.opacity = 100;
    return f;
}

int main() {
    // --- registry ---
    MetricId id{};
    CHECK(metric_from_token("fps", id) && id == MetricId::OVM_FPS);
    CHECK(metric_from_token("P0.2", id) && id == MetricId::OVM_P02);
    CHECK(metric_from_token("p02", id) && id == MetricId::OVM_P02);
    CHECK(!metric_from_token("nope", id));
    char buf[32];
    format_metric_value(MetricId::OVM_FPS, sample(), buf, sizeof buf);
    CHECK(std::strcmp(buf, "104") == 0);
    MetricValues none;
    format_metric_value(MetricId::OVM_AVG, none, buf, sizeof buf);
    CHECK(std::strcmp(buf, "--") == 0);

    // --- the full registry ---
    CHECK(kMetricCount >= 60);
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        const MetricInfo& a = metric_info(static_cast<MetricId>(i));
        CHECK(static_cast<std::size_t>(a.id) == i);        // table order == enum
        CHECK(a.token[0] && a.label[0] && a.widest[0]);
        CHECK(std::strlen(a.label) < kOverlayLabelSize);   // fits the wire
        CHECK(!std::strpbrk(a.token, ",; \t"));            // spec-safe
        CHECK((a.color & 0xffU) == 0xffU);                 // opaque default
        MetricId back{};
        CHECK(metric_from_token(a.token, back) && back == a.id);
        for (std::size_t j = i + 1; j < kMetricCount; ++j)
            CHECK(std::strcmp(a.token, metric_info(static_cast<MetricId>(j)).token) != 0);
    }
    {   // every metric takes a cell and formats, with and without a value
        MetricValues all;
        for (std::size_t i = 0; i < kMetricCount; ++i) { all.have[i] = true; all.value[i] = 12.34; }
        for (std::size_t i = 0; i < kMetricCount; ++i) {
            char t[32];
            format_metric_value(static_cast<MetricId>(i), all, t, sizeof t);
            CHECK(t[0] != '\0' && std::strcmp(t, "--") != 0);
        }
        std::string spec;
        for (std::size_t i = 0; i < kMaxLayoutCells; ++i) {
            if (i) spec += ';';
            spec += metric_info(static_cast<MetricId>(i)).token;
        }
        auto big = parse_layout_spec(spec);
        CHECK(big && big->cell_count == kMaxLayoutCells);
        OverlayFrame bf = frame_for();
        build_overlay_from_spec(bf, *big, all);
        CHECK(bf.row_count == kMaxLayoutCells);
    }
    {   // a column's width follows its widest metric, not the current value
        OverlayFrame a = frame_for(), b = frame_for();
        MetricValues lo, hi;
        lo.have[(int)MetricId::OVM_GPUMHZ] = hi.have[(int)MetricId::OVM_GPUMHZ] = true;
        lo.value[(int)MetricId::OVM_GPUMHZ] = 800;  hi.value[(int)MetricId::OVM_GPUMHZ] = 2230;
        auto sp = parse_layout_spec("GPUMHZ,FPS");
        build_overlay_from_spec(a, *sp, lo);
        build_overlay_from_spec(b, *sp, hi);
        CHECK(a.rows[1].label_x == b.rows[1].label_x);   // FPS cell did not move
    }

    // --- spec parsing ---
    auto v = parse_layout_spec("FPS;AVG;P1;P0.2");
    CHECK(v && v->cell_count == 4 && v->line_count == 4);
    CHECK(v->cells[3].metric == MetricId::OVM_P02 && v->cells[3].line == 3);
    auto h = parse_layout_spec("FPS,AVG,P1,P0.2");
    CHECK(h && h->cell_count == 4 && h->line_count == 1);
    auto g = parse_layout_spec(" fps , avg ; p1 ,p0.2 ");
    CHECK(g && g->line_count == 2 && g->cells[2].line == 1);
    CHECK(layout_spec_to_string(*g) == "FPS,AVG;P1,P0.2");
    auto skip = parse_layout_spec("FPS;bogus;AVG");
    CHECK(skip && skip->cell_count == 2 && skip->line_count == 2);
    CHECK(!parse_layout_spec(""));
    CHECK(!parse_layout_spec("bogus;;nothing"));
    CHECK(!parse_layout_spec(std::string(200, 'F')));
    auto many = parse_layout_spec("FPS,FPS,FPS,FPS,FPS,FPS,FPS,FPS,FPS,FPS,FPS,FPS,FPS,FPS,FPS,FPS,FPS,FPS");
    CHECK(many && many->cell_count == kMaxLayoutCells);
    CHECK(layout_spec_to_string(default_layout_spec()) == "FPS;AVG;P1;P0.2");
    CHECK(layout_spec_to_string(horizontal_layout_spec()) == "FPS,AVG,P1,P0.2");

    // --- vertical layout: one column, right-aligned numbers, fixed colon ---
    OverlayFrame vf = frame_for();
    build_overlay_from_spec(vf, default_layout_spec(), sample());
    CHECK(vf.explicit_layout && vf.row_count == 4);
    CHECK(std::strcmp(vf.rows[0].label, "FPS") == 0);
    CHECK(std::strcmp(vf.rows[3].value, "86") == 0);
    for (int i = 1; i < 4; ++i) {
        CHECK(vf.rows[i].label_x == vf.rows[0].label_x);
        CHECK(vf.rows[i].colon_x == vf.rows[0].colon_x);
        CHECK(vf.rows[i].y > vf.rows[i - 1].y);
    }
    CHECK(vf.rows[0].colon_x > vf.rows[0].label_x);
    CHECK(vf.rows[0].number_x > vf.rows[0].colon_x);
    // 104 is wider than 86, so it starts further left (shared right edge).
    CHECK(vf.rows[0].number_x < vf.rows[3].number_x);
    // Value changing digit count never moves the colon or the name.
    OverlayReadings r2; r2.have_fps = true; r2.fps = 99; r2.avg = 5; r2.p1 = 5; r2.p02 = 5;
    OverlayFrame vf2 = frame_for();
    build_overlay_from_spec(vf2, default_layout_spec(), metric_values_from_readings(r2));
    CHECK(vf2.rows[0].colon_x == vf.rows[0].colon_x);
    CHECK(vf2.rows[0].label_x == vf.rows[0].label_x);

    // --- horizontal layout: one line, cells left to right, same y ---
    OverlayFrame hf = frame_for();
    build_overlay_from_spec(hf, horizontal_layout_spec(), sample());
    CHECK(hf.row_count == 4);
    for (int i = 1; i < 4; ++i) {
        CHECK(hf.rows[i].y == hf.rows[0].y);
        CHECK(hf.rows[i].label_x > hf.rows[i - 1].number_x);
    }
    // Same y as the first row of the vertical layout.
    CHECK(hf.rows[0].y == vf.rows[0].y);
    // Horizontal cells do not move when a value changes digits.
    OverlayFrame hf2 = frame_for();
    build_overlay_from_spec(hf2, horizontal_layout_spec(), metric_values_from_readings(r2));
    for (int i = 0; i < 4; ++i) {
        CHECK(hf2.rows[i].label_x == hf.rows[i].label_x);
        CHECK(hf2.rows[i].colon_x == hf.rows[i].colon_x);
    }

    // --- 2x2 grid: columns align across lines ---
    OverlayFrame gf = frame_for();
    build_overlay_from_spec(gf, *parse_layout_spec("FPS,AVG;P1,P0.2"), sample());
    CHECK(gf.rows[0].label_x == gf.rows[2].label_x);
    CHECK(gf.rows[1].label_x == gf.rows[3].label_x);
    CHECK(gf.rows[2].y > gf.rows[0].y);

    // --- corners keep the block on screen ---
    for (Corner c : {Corner::TopRight, Corner::BottomLeft, Corner::BottomRight}) {
        OverlayFrame cf = frame_for(c);
        build_overlay_from_spec(cf, horizontal_layout_spec(), sample());
        for (std::size_t i = 0; i < cf.row_count; ++i) {
            CHECK(cf.rows[i].label_x >= 0 && cf.rows[i].number_x < kLogicalWidth);
            CHECK(cf.rows[i].y > -50 && cf.rows[i].y < kLogicalHeight);
        }
    }
    OverlayFrame tr = frame_for(Corner::TopRight);
    build_overlay_from_spec(tr, horizontal_layout_spec(), sample());
    CHECK(tr.rows[0].label_x > kLogicalWidth / 2);

    // --- position and size follow the config ---
    {
        OverlayFrame base = frame_for();
        build_overlay_from_spec(base, default_layout_spec(), sample());

        OverlayFrame moved = frame_for();
        moved.config.margin_x = 60; moved.config.margin_y = 80;
        build_overlay_from_spec(moved, default_layout_spec(), sample());
        CHECK(moved.rows[0].label_x == base.rows[0].label_x + 50);
        CHECK(moved.rows[0].y == base.rows[0].y + 70);

        OverlayFrame big = frame_for();
        big.config.font_size = 36;
        build_overlay_from_spec(big, default_layout_spec(), sample());
        // bigger text: wider columns and taller line step
        CHECK(big.rows[0].number_x - big.rows[0].label_x >
              base.rows[0].number_x - base.rows[0].label_x);
        CHECK(big.rows[1].y - big.rows[0].y > base.rows[1].y - base.rows[0].y);

        OverlayFrame dim = frame_for();
        dim.config.opacity = 50;
        build_overlay_from_spec(dim, default_layout_spec(), sample());
        CHECK((dim.rows[0].label_rgba & 0xffU) == 127);
        CHECK((dim.rows[0].value_rgba & 0xffU) == 127);
        CHECK((dim.rows[0].label_rgba & 0xffffff00U) ==
              (base.rows[0].label_rgba & 0xffffff00U));

        // bottom corner: the block's last line stays inside the margin
        OverlayFrame bot = frame_for(Corner::BottomLeft);
        bot.config.margin_y = 40;
        build_overlay_from_spec(bot, default_layout_spec(), sample());
        CHECK(bot.rows[3].y > bot.rows[0].y && bot.rows[3].y < kLogicalHeight - 40);
    }

    // --- hidden / empty ---
    OverlayFrame hid = frame_for(); hid.config.visible = false;
    build_overlay_from_spec(hid, default_layout_spec(), sample());
    CHECK(hid.row_count == 0);

    // --- wire round trip keeps positions and the flag ---
    OverlayFrame wf = frame_for(); wf.visible = true;
    build_overlay_from_spec(wf, default_layout_spec(), sample());
    WirePacket p = make_wire_packet(wf, 7);
    auto back = decode_wire_packet(p);
    CHECK(back && back->explicit_layout && back->row_count == 4);
    CHECK(back && back->rows[2].colon_x == wf.rows[2].colon_x);
    CHECK(back && back->rows[2].y == wf.rows[2].y);
    p.rows[0].y = std::nanf("");
    auto bad = decode_wire_packet(p);
    CHECK(bad && bad->rows[0].y == 0.0f);

    std::printf(failures ? "%d FAILED\n" : "all overlay layout tests passed\n", failures);
    return failures ? 1 : 0;
}
