#!/bin/sh
# Build the 32-bit dinput8.dll proxy for Revelations 2 with the workspace Zig (no Visual Studio needed).
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
mkdir -p build
"$ZIG" cc -target x86-windows-gnu -O2 -fno-omit-frame-pointer -shared -o build/dinput8.dll \
  -DWIN32 proxy.c testmode.c d3d.c steam.c diag.c classes.c trace.c inject.c console.c mute.c net.c coop3.c dinput8.def \
  -luser32 -lkernel32 -lws2_32 -liphlpapi -lole32 -luuid
