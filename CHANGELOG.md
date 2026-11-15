# Changelog

All notable changes to Heatline are listed here. Versions follow [Semantic Versioning](https://semver.org/).

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
