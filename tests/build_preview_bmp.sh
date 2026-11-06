#!/bin/sh
# Build the GDI-only preview of the lobby overlay (no game, no Direct3D device): sh tests/build_preview_bmp.sh ; tests/build/overlay_preview_bmp.exe <out dir> [w h [backdrop.bmp [lobby-stock-12lang.tsv [lang]]]]
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
mkdir -p build
"$ZIG" cc -target x86_64-windows-gnu -O1 -g -Wall -Wno-unused-function -o build/overlay_preview_bmp.exe overlay_preview_bmp.c ../native/overlay.c ../native/gametex.c ../native/lobby_text.c -lgdi32 -luser32 -lm
