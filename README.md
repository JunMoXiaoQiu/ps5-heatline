# Heatline

**Version 0.0.1** · see [CHANGELOG.md](CHANGELOG.md)

A live hardware and game-performance monitor for a jailbroken PS5. Heatline is a payload: it runs on the console and serves a web dashboard you open in any browser on the same network, including the PS5's own browser.

It shows temperatures, fan speed, power, clocks, CPU usage, RAM and storage, FPS and frame-time metrics (with CapFrameX's formulas), the display output mode, network information and latency, and it can set the fan threshold and sync the console clock.

> Heatline only reads values the console already reports, except two settings you turn on yourself: the fan threshold and the clock sync. Use it at your own risk.

## Contents
- [Install and run](#install-and-run)
- [Pages and panels](#pages-and-panels)
- [Settings](#settings)
- [History and CSV export](#history-and-csv-export)
- [HTTP endpoints](#http-endpoints)
- [Files on the console](#files-on-the-console)
- [Known limits](#known-limits)
- [Build from source](#build-from-source)
- [Research notes](#research-notes)
- [Credits and licences](#credits-and-licences)

## Install and run
1. Start elfldr on the PS5 as usual.
2. Send the payload from a computer: `nc <PS5 IP> 9021 < heatline.elf` (or use your payload manager / autoload).
3. A notification confirms it started. Open `http://<PS5 IP>:8776/` on any device on the same network.

On the PS5 itself, Heatline installs a **Heatline tile** on the home screen (title ID HTLN00001). Selecting it opens the dashboard in the PS5 browser. The tile is reinstalled only when its icon or `param.json` changes.

**Update:** send the new `heatline.elf`; it stops the running copy and takes over port 8776.
**Stop:** open `http://<PS5 IP>:8776/quit` and click **Stop Heatline**.

**Payload Manager:** add this source in Payload Manager › Settings › Manage Sources:
`https://raw.githubusercontent.com/JunMoXiaoQiu/ps5-heatline/main/repo/payloads.json`
It points to `heatline.elf` on the [latest release](https://github.com/JunMoXiaoQiu/ps5-heatline/releases/latest), with its SHA-256 checksum.

## Pages and panels
Every panel has an **About** section: what it shows, where the value is read from, the layout of that data (what is decoded and what is not), how often it is read, the formulas, the accuracy, its CSV columns, what still needs help, and credits. Inside it, **Raw data** shows the console's original answers for that panel; it is only read while that section is open. Panels with live values have a refresh button.

### Dashboard
- **Game performance** (only while a game runs): game name and title ID, FPS now, frame time, and CapFrameX's real-time overlay metrics over 20 s (average, P1, P0.2, 1% low, 0.2% low) plus stuttering time over 5 s, with CapFrameX's own formulas and defaults. Session average / min / max / duration, stutter counts and time below a target FPS. "More metrics" (off by default, saved on the console) adds percentiles, integral lows, adaptive standard deviation and shortest / longest frame. An optional frame-time graph marks stutters. A badge shows whether a game or the home screen is in front.
- **Display output** (compact): resolution, refresh rate, HDR, VRR, colour depth, colour format, range.
- **Thermals & power**: CPU temperature, SoC channel 0, the hottest of SoC channels 1–7, CPU and GPU power and their sum, CPU and GPU voltage and current, fan speed.
- **CPU & GPU**: CPU usage (average and 16 threads, grouped by core), GPU clock, CPU clock setting.
- **Memory & Storage**: RAM (used, free, system, game) and console storage (total, used, free, reserved), M.2 and USB extended storage.
- **History**: charts and the CPU heat strip (see below).

### System
Display output (full panel with the decoding table), Heatline's own consumption (CPU, memory, traffic), hardware (model, architecture, firmware, CPU cores), extended information (uptime, boot / shutdown count, thermal status, disc drive), date and time with an internet time check and sync.

### Network
Connection (state, interface, IP, netmask, gateway, hostname), traffic (download / upload per second), DNS and link (DNS servers, MAC, MTU), latency to the router and to the internet (off by default).

### Fan threshold
Sets the temperature at which the PS5's fan driver speeds the fan up (30–90 °C / 86–194 °F, lower = sooner), with an optional separate threshold for the home screen, re-applied every 1–3600 s, and an optional notification. Off until you turn it on. Saved on the console and applied when Heatline starts. Not applied while ps5-fan-control.elf is running.

A threshold of 80 °C (176 °F) or more shows a warning and needs a second click on Save: the fan then only speeds up once the console is already that hot.

### Settings
Always the last page in the sidebar. See [Settings](#settings).

The header shows the console model, firmware, the console's date and time (in its own time zone) and the serial number (hidden except the last 4 characters; click to show). The serial number is never exported or shown in Raw data.

## Settings
| Setting | Default | Saved |
|---|---|---|
| Theme: Heatline dark / bright, static or dynamic | Heatline dark (static) | this browser |
| Temperature unit: °C / °F (display only) | °C | this browser |
| Network latency measurement | off | on the console |
| More metrics (Game performance) | off | on the console |
| Refresh rate (100 ms to 60 s) | 1 s | this browser |
| History length (1 min to 24 h) | 10 min | this browser |
| CSV separator: `,` with decimal point, or `;` with decimal comma (Excel in French) | `,` | this browser |

- The static themes have no animation or blur, so they stay smooth in the PS5 browser. The dynamic themes add hover effects and look best on a computer or phone.
- The temperature unit only changes what is shown: the console, the fan driver and History keep °C. Fan settings use the same unit.
- Data tools: Pause, Reset min/max, Export CSV.

## History and CSV export
- Charts of any value over time. Three default charts (temperatures; FPS, P1, P0.2 and frame time; fan). Add charts, add values with "+ Add value" (two units per chart, left and right axis), reorder charts by dragging the ⋮⋮ grip or with ▲ ▼, remove them with ×, or "Restore defaults". Hover a value name for its description.
- History keeps at most about 3000 points: one per refresh for short lengths, one every length / 3000 for long ones (24 h = one point every 28.8 s).
- **Export CSV** writes one row per History point: 110 columns, grouped by topic (time: `time_utc` in ISO 8601 and `time_local` as `YYYY-MM-DD hh:mm:ss` in the console's time zone, which Excel reads as a date in any language, and `console_clock_ms` from the console's monotonic clock (exact time between rows); temperatures in °C and °F side by side, fan, game, CPU, GPU, power rails, RAM, network, display). Names follow `what_detail_unit`, for example `cpu_temp_c`, `fps_p1_20s`, `rail1_power_mw`. Rails and SoC channels are numbered from 0 like the console; cores and threads from 1. Yes / no values are `true` / `false`. An empty cell means no reading at that moment (no game, a measurement off, or no new FPS summary since the previous row).
- **Settings › Data › CSV columns** lists every column in order, where the same value is on the page, and a note. Each panel's About lists its own columns.

## HTTP endpoints
All on port 8776, GET only.

**Protection.** Heatline has no login: anyone on your local network can open the dashboard. What it blocks is another web site (open on any phone or computer on that network) using Heatline behind your back:
- Other sites cannot read any answer (no `Access-Control-Allow-Origin` header), so the serial number in `/api` stays private.
- Every request that changes something (fan, clock, switches, refresh, reset, `/quit`) needs the header `X-Heatline: 1`. The dashboard sends it; another site cannot. From a script: `curl -H 'X-Heatline: 1' http://<PS5 IP>:8776/fan/apply`.
- The address must be the console's IP, `localhost`, a one-word name (`ps5`) or a `.local` name; other host names get 403 (this blocks DNS rebinding).
- Opening `/quit` in a browser shows a **Stop Heatline** button instead of stopping at once.

| Path | What it does |
|---|---|
| `/` | The dashboard |
| `/api` | All current values as JSON |
| `/raw?p=<panel>` | The console's raw answers for one panel (used by Raw data) |
| `/refresh?net` · `/refresh?lat` | Re-read network information now · measure latency now |
| `/fps/ft?since=<n>` | Frame times since a cursor (frame-time graph) |
| `/fps/more?on=0\|1` · `/fps/reset` | More metrics switch · reset the game session |
| `/lat/on?on=0\|1` | Latency switch |
| `/display/read` · `/display/log` · `/display/log/clear` | Re-read the display mode · unmapped display codes seen · clear that log |
| `/fan/set?…` · `/fan/apply` | Save the fan settings · apply them now |
| `/ntp/check` · `/ntp/sync` · `/ntp/auto?on=0\|1` | Compare with internet time · set the clock · sync at start |
| `/quit` | Stop Heatline |

## Files on the console
All in `/data/heatline/`:
- `fan_control.ini`: fan settings (same keys as ps5-fan-control).
- `lat_on`, `fps_more`, `ntp_autosync`: present = switch on.
- `display_unknown.jsonl`: every display reading with a code Heatline cannot decode yet, with the game and settings of that moment (last 100).

## Known limits
- **USB extended storage** is not detected yet (the card stays empty with a drive connected). M.2 storage has not been tested with a drive installed.
- **Display output**: decoded from one 1440p monitor. 4K, 50 / 24 / 40 Hz, YCbCr 4:4:4 / 4:2:0 and 10-bit codes are not known yet; unknown codes are shown as "unknown code N" and kept in `/display/log`.
- **SoC channels 1–7 and power rails 2–7**: read and exported, but what each one measures is not known. Rails 2–3 rarely change (frozen for 28 s in one test, unchanged for 6 minutes in a game in another); rails 4–7 read 0.
- **CPU clock** is the clock setting the console reports, not the live clock. No live source was found.
- **Power, voltage and current** are the console's own figures for chip rails, never checked with a meter, and not the console's wall power.
- **Network latency** is the time to open a TCP connection, so it reads slightly higher than a ping.
- **No SSD read / write speed** (the counters do not exist on FW 13.60) and **no SSD temperature** (no source found). Memory clocks cannot be read.
- Only the PS5's own browser, desktop browsers and phones have been used to open the page.

Readings that help: open Raw data in a panel's About, or `/display/log`, and send it with the console model, firmware and settings.

## Build from source
Requires the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) and python3.

    export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
    make

The output is always `heatline.elf`. The version number lives only in the `VERSION` file: `make` compiles it into the payload, which shows it in the start notification, in `/api` (`"version"`) and at the bottom of the sidebar.

### Release checklist
1. Change `VERSION`, run `make`, add an entry to `CHANGELOG.md`.
2. In `repo/payloads.json`, update `version`, `last_update` and `checksum` (`sha256sum heatline.elf`).
3. Once: put your developer name and links in the README credits (below), in `author` and `url` in `repo/payloads.json`, and in the `DEV` line of `web/index.tpl.html` (the page hides "by …" while the name is empty).
4. Run `make release-check`: it lists anything still to fill in and checks the version and checksum in `payloads.json`.
5. Publish the source with the ELF (for example on the same GitHub release): the GPL requires it.

The page is edited in `web/index.tpl.html`. `make` runs `web/build_page.py` (embeds the fonts and favicon) to make `index.html`, then `page_html.h`, which is compiled into `heatline.elf`.

| File | Role |
|---|---|
| `main.c` | HTTP server, `/api`, routes, power / temperature / clock reads |
| `fps.c` | Frame timing from the display driver and every game metric |
| `cpuload.c` | CPU usage per thread |
| `context.c` | Game or home screen |
| `storage.c` | Console, M.2 and USB storage |
| `sysinfo.c` | Hardware, ICC and network information (with a crash guard) |
| `display.c` | Display output mode and the unknown-codes log |
| `netlat.c` | Latency |
| `fan.c` | Fan threshold |
| `ntp.c` | Internet time check and sync |
| `selfstat.c` | Heatline's own consumption |
| `raw.c` | Raw data answers |
| `app.c` | Home-screen tile |

The developer's name and link in the credits are set in one line of `web/index.tpl.html`: `const DEV={name:'…',url:'…'}`.

## Research notes
Findings from tests on a CFI-2016, FW 13.60. The test code has been removed.

- **Safety:** a load test with 5 busy threads for 10 s crashed the console. Any load test: one thread, short bursts, yielding. Reading all SoC temperature channels 0–64 caused a kernel panic; Heatline reads 0–7 only.
- **Threads and cores:** threads 2k−1 and 2k share core k (pinned SMT contention test). `sceKernelGetCpuCoreClock` lists the 4 system cores first; Heatline reorders them.
- **CPU clock:** timed multiply loops measured 3071–3465 MHz while 3200 / 2560 MHz was reported, and the reported values never moved under load, so they are a setting.
- **Power:** `sceKernelGetSocPowerConsumption` gives 8 rails of {mW, mV, mA} and 4 extra words. Rails 0–3 satisfy P = V × I. Rail 1 follows the CPU (a single busy thread switched on and off in 2 s steps moved it by +0.84 W on 8 of 8 steps); rail 0 follows the GPU (3.2 W in the menu, ~93 W in a game). Extra word 2 = rail 2 + rail 3.
- **GPU clock:** of all `sceKernelGetSocClock` words, only word 20 (shown) and 25 (identical) move in a game: 1800–2229 MHz, correlation 0.97 with GPU power.
- **CPU usage:** a busy loop pinned to one thread read 90 % on that thread only.
- **Storage:** `sceShellCoreUtilGetFreeSizeOfUserPartition` gives the same size and free space as Settings (848.0 / 355.5 GB); a fitted `statfs` formula is the fallback.
- **Display output:** `sceVideoOutGetCurrentOutputMode_(0x7000, …)` from libSceVideoOut, found with `kernel_dynlib_dlsym`, decoded by changing one Video Output setting at a time and checking the monitor's signal info. The full decoding table is in the Display output About.

## Credits and licences
Developed by JunMoXiaoQiu ([GitHub](https://github.com/JunMoXiaoQiu/ps5-heatline)), with Claude (Anthropic) as AI co-developer.

Methods and formulas from: CapFrameX (frame-time metrics), PS5 Monitor (RAM, storage, system information, context), PS5 Thermal Doctor (temperatures, power), ps5-hwinfo and ps5-exporter (power rails), ps5-fan-control by owendswang (fan threshold, GPL-3.0, re-written), PS5 Date/Time Sync by Deckerr97 (clock sync, re-written; no code copied), AnyPS5 (display output call), shadPS4, and the ps5-payload-dev SDK and elfldr. Each panel's About credits its own sources.

Fonts: Geist and Geist Mono by Vercel, SIL Open Font License 1.1 (`web/fonts/OFL.txt`).

### Licence
Copyright (C) 2026 JunMoXiaoQiu.

Heatline is free software under the **GNU General Public License v3.0 only** (`LICENSE`, SPDX `GPL-3.0-only`). You may use, share and change it; if you share `heatline.elf` or a modified version, you must also make its source available under the same licence.

Why GPL-3.0: every payload built with the ps5-payload-sdk contains the SDK's own start-up code, which is GPL-3.0-or-later, and `fan.c` closely follows ps5-fan-control, which is GPL-3.0-only. What comes from where is listed in `THIRD_PARTY_NOTICES.md`, including the MIT notice of CapFrameX, whose metrics `fps.c` ports.
