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
out = tpl.replace("__FONTS__", fonts).replace("__FAVICON__", fav)
assert "__FONTS__" not in out and "__FAVICON__" not in out
open(sys.argv[1] if len(sys.argv) > 1 else "index.html", "w", encoding="utf-8").write(out)
