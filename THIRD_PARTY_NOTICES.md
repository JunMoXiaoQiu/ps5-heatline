# Third-party notices

Heatline is licensed under the GNU General Public License v3.0 only (see `LICENSE`).
It builds on the following work. Code that was ported or adapted keeps the notice its
licence asks for.

## Built into heatline.elf

| Component | Licence | What Heatline uses |
|---|---|---|
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) (John Törnblom and contributors) | GPL-3.0-or-later (headers in `include/freebsd`: BSD) | The start-up and runtime code (`crt`) linked into every payload, and the headers. This is why `heatline.elf` as a whole is under the GPL. |
| [SimpleFPS](https://github.com/khalifa007/SimpleFPS) by khalifa007 and [Common FPS for PS5](https://github.com/porhe911/Common-FPS-for-PS5) by porhe911 | GPL-3.0-or-later | The in-game overlay engine, vendored unmodified under `overlay/engine` (Common-FPS v1.2.1 @ `b7969fd` with SimpleFPS's `simplefps.patch`) and built as `libheatline_overlay.a`: ShellUI discovery, renderer injection and the Mono text renderer drawn over the game. Heatline adds only `overlay_api.cpp`, a static-library target, a single-line layout, and the change to source metrics from `fps.c` instead of the engine's own sampler. |
| [shsrv](https://github.com/ps5-payload-dev/shsrv) (ps5-payload-dev) | GPL-3.0-or-later | `overlay/shsrv` (`pt.c`, `elfldr.c` and headers): the ptrace helpers the overlay injector uses to load the renderer into SceShellUI. |

## Ported or adapted code

| Project | Licence | Where in Heatline |
|---|---|---|
| [ps5-fan-control](https://github.com/owendswang/ps5-fan-control) by owendswang | GPL-3.0-only | `fan.c` (fan driver call, settings-file keys, home-screen threshold rule, status notification) and the process scan in `main.c` (`stop_old_instance`). Re-written, but closely follows the original. |
| [CapFrameX](https://github.com/CXWorld/CapFrameX) by DevTechProfile | MIT (notice below) | `fps.c`: the frame-time metrics and their defaults, ported from CapFrameX's C# code. |

## Methods and research credited (no code copied)

PS5 Date/Time Sync by Deckerr97 (clock sync, re-written), PS5 Monitor, PS5 Thermal Doctor,
ps5-hwinfo, ps5-exporter (which system calls to read), AnyPS5 (the display output call),
shadPS4 (GPL-2.0-or-later; reference only) and elfldr (GPL-3.0). See each panel's About.

## Fonts

Geist and Geist Mono by Vercel, SIL Open Font License 1.1 (`web/fonts/OFL.txt`), embedded
in the dashboard page.

## CapFrameX licence

```
MIT License

Copyright (c) 2018-2026 DevTechProfile

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
