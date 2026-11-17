#!/usr/bin/env bash
# Builds overlay/libheatline_overlay.a: the vendored SimpleFPS / Common-FPS
# on-TV overlay engine (GPL-3.0-or-later) as a static library that heatline.elf
# links. Needs PS5_PAYLOAD_SDK set and prospero-cmake on the SDK.
#
# Source provenance (GPL): overlay/engine is Common-FPS v1.2.1 @ b7969fd with
# SimpleFPS's simplefps.patch applied, plus overlay_api.cpp and a heatline_overlay
# target (both under ps5/). overlay/shsrv holds pt.c/pt.h/elfldr.c/elfldr.h from
# ps5-payload-dev/shsrv. etaHEN is NOT needed: the library uses no etaHEN header.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK to the PS5 payload SDK folder}"
export PS5_PAYLOAD_SDK
build="$here/build"

"$PS5_PAYLOAD_SDK/bin/prospero-cmake" -S "$here/engine/ps5" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCOMMON_FPS_SHSRV_SOURCE="$here/shsrv" > /dev/null
cmake --build "$build" --target heatline_overlay -j"$(nproc)" > /dev/null
cp "$build/libheatline_overlay.a" "$here/libheatline_overlay.a"
echo "built $here/libheatline_overlay.a"
