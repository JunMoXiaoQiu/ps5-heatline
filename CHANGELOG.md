# Changelog

All notable changes to Heatline are listed here. Versions follow [Semantic Versioning](https://semver.org/).

## [0.1.0] - 2026-11-16

### Added
- In-game overlay: draws performance metrics over the running game itself, on its own **In-game overlay** page in the dashboard (its own nav entry). Working on FW 13.60. Live status in `/api` under `"overlay"`.
- Overlay metrics: **FPS**, **AVG**, **P1** (1% low) and **P0.2** (0.2% low), drawn on a **single line** (`FPS x  AVG x  P1 x  P0.2 x`) in one text element — the renderer's proven single-widget shape — colour-coded green/yellow/red by the current FPS. (Multi-row drawing created several Mono widgets at once and hard-crashed the console; deferred until the renderer handles it safely.) These come from Heatline's own sampler (`fps_overlay_metrics` in `fps.c`) — the CapFrameX real-time avg / 1% low / 0.2% low the dashboard already computes from exact per-frame times — so the overlay and dashboard share one source. The vendored engine no longer samples or computes stats for the overlay.
- The overlay is the **SimpleFPS / Common-FPS** engine, vendored under `overlay/` and built as a static library (`overlay/libheatline_overlay.a`) that links into `heatline.elf`. It injects a Mono renderer into the home-screen process (SceShellUI) — whose layer the console composites over the game — and draws there. `osd.c` starts it from the toggle through `osd_overlay.h` (`overlay_start`/`overlay_stop`); the engine does its own ShellUI discovery, game-gating and injection (ptrace + kernel R/W, in Heatline's own process — no etaHEN or separate process needed).
  - `overlay/engine` is Common-FPS v1.2.1 @ `b7969fd` + SimpleFPS's `simplefps.patch`, plus `src/ps5/overlay_api.cpp` (renames the engine entry and exposes `overlay_start`/`overlay_stop`), `src/ps5/shellui_attach_probe.hpp` (re-created minimal attach/detach probe for diagnostics), and a `heatline_overlay` static-library target. `overlay/shsrv` holds the four shsrv files the injector needs. `overlay/build_overlay.sh` builds the library; `make` runs it automatically.

### Changed vs. upstream SimpleFPS
- **No game ptrace-attach** (`HEATLINE_NO_GAME_ATTACH`): the engine's VideoOut sampler attaches the running game to read frame data, which stops/resumes it each poll and destabilises frametimes. Heatline disables that path and uses only the DCE sampler (`/dev/dce`, the same source the dashboard reads) — no game stutter. This was also the root cause of the early home-screen hang: the game-attach ran concurrently with the renderer injection; with it gone, injection from Heatline's own process is stable.
- **Session-only safety**: the overlay is always OFF after Heatline (re)starts (`osd_start` clears the saved flag) and runs only after an explicit toggle, so a reload/boot can never auto-trigger the injection. A restart removes the overlay (SimpleFPS parity: no clean off switch).
- Build-time diagnostic modes remain available through the engine's `COMMON_FPS_TEST25/26` defines (off in release builds). Temporary diagnostic endpoints added during bring-up (e.g. `/overlay/log`) have been removed — the web interface exposes only pages reachable from the nav.

> ⚠️ Unlike the rest of Heatline (read-only, plus the fan and clock settings), the overlay **injects code into the home-screen process**. A fault there crashes the home screen, not the game. It is opt-in and session-only for that reason.

### Licence
- The vendored overlay engine is GPL-3.0-or-later (Common-FPS / SimpleFPS); shsrv is used under its licence. Both are compatible with Heatline's GPL-3.0-only. See `overlay/engine/LICENSE` and `THIRD_PARTY_NOTICES.md`.

## [0.0.1] - 2026-11-15
First versioned release.

### Added
- `VERSION` file as the single source of the version number, compiled into `heatline.elf` by the Makefile.
- The version is shown in the start notification ("Heatline v0.0.1 on port 8776"), in `/api` as `"version"`, and at the bottom of the sidebar.
- Warning and second click on Save for a fan threshold of 80 °C (176 °F) or more.
- `LICENSE` (GPL-3.0-only), SPDX headers in the sources and `THIRD_PARTY_NOTICES.md`.
- `make release-check` lists what is still to fill in before publishing.

### Security
- Other web sites can no longer read Heatline's answers (no `Access-Control-Allow-Origin` header), so the serial number in `/api` stays private.
- Requests that change something (fan, clock, switches, refresh, reset, quit) need the `X-Heatline: 1` header, which the dashboard sends and other sites cannot. Requests addressed to an unexpected host name are refused (DNS rebinding).
- Opening `/quit` in a browser shows a Stop button instead of stopping at once.
- Requests that arrive in several pieces are read whole.

### Changed
- The developer credit stays empty until it is set; the page hides "by …" meanwhile.
