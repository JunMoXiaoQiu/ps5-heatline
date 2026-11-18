#!/usr/bin/env python3
"""Builds index.html from web/index.tpl.html: embeds the Geist fonts and the favicon
so the page works with no internet access (the PS5 serves it on its own)."""
import base64, os, sys
here = os.path.dirname(os.path.abspath(__file__))
tpl = open(os.path.join(here, "index.tpl.html"), encoding="utf-8").read()
def face(family, fname):
    b = base64.b64encode(open(os.path.join(here, "fonts", fname), "rb").read()).decode()
    return ('@font-face{font-family:"%s";src:url(data:font/woff2;base64,%s) format("woff2");'
            'font-weight:100 900;font-style:normal;font-display:swap}' % (family, b))
fonts = face("Geist", "Geist-Variable-subset.woff2") + "\n" + face("Geist Mono", "GeistMono-Variable-subset.woff2")
fav = open(os.path.join(here, "favicon.b64")).read().strip()
# The in-game overlay's metric picker is generated from the same list the overlay
# engine is built from, so the page can never offer a metric the engine lacks.
import html, re
mlist = os.path.join(here, "..", "overlay", "engine", "include", "common_fps", "overlay_metric_list.h")
groups, cur = [], None
for line in open(mlist, encoding="utf-8").read().split("\n"):
    g = re.search(r"/\* ---- (.*?) ---- \*/", line)
    if g:
        cur = [g.group(1), []]; groups.append(cur); continue
    m = re.search(r'X\((\w+),\s*"([^"]+)",\s*"([^"]+)",\s*0x([0-9a-fA-F]{6})', line)
    if m and cur is not None:
        cur[1].append((m.group(2), m.group(3), m.group(4)))
assert groups and sum(len(g[1]) for g in groups) >= 40, "metric list not parsed"
chips = []
for name, items in groups:
    chips.append('<div class="mgroup"><span class="mname">%s</span><span class="mchips">' % html.escape(name))
    for token, label, rgb in items:
        chips.append('<button type="button" class="mchip" data-token="%s" title="add %s to the layout" '
                     'style="--mc:#%s">%s</button>' % (html.escape(token), html.escape(token), rgb, html.escape(label)))
    chips.append('</span></div>')
metrics_html = "".join(chips)
types_h = open(os.path.join(here, "..", "overlay", "engine", "include", "common_fps", "types.hpp"), encoding="utf-8").read()
state_h = open(os.path.join(here, "..", "overlay", "engine", "src", "ps5", "overlay_layout_state.hpp"), encoding="utf-8").read()
lim = {k: re.search(r"%s\s*=\s*(\d+)" % k, state_h).group(1)
       for k in ("kStyleMinFont", "kStyleMaxFont", "kStyleMaxMargin", "kStyleMinOpacity", "kStyleMaxOpacity")}
max_cells = re.search(r"kMaxOverlayRows\s*=\s*(\d+)", types_h).group(1)
out = (tpl.replace("__FONTS__", fonts).replace("__FAVICON__", fav)
          .replace("__OVERLAY_METRICS__", metrics_html).replace("__OVERLAY_MAX__", max_cells)
          .replace("__FONT_MIN__", lim["kStyleMinFont"]).replace("__FONT_MAX__", lim["kStyleMaxFont"])
          .replace("__MARGIN_MAX__", lim["kStyleMaxMargin"])
          .replace("__OPAC_MIN__", lim["kStyleMinOpacity"]).replace("__OPAC_MAX__", lim["kStyleMaxOpacity"]))
assert "__FONTS__" not in out and "__FAVICON__" not in out and "__OVERLAY_METRICS__" not in out and "__OVERLAY_MAX__" not in out and "__FONT_MIN__" not in out and "__MARGIN_MAX__" not in out and "__OPAC_MIN__" not in out
open(sys.argv[1] if len(sys.argv) > 1 else "index.html", "w", encoding="utf-8").write(out)
