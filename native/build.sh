#!/bin/sh
# Build the 32-bit dinput8.dll proxy for Revelations 2 with the workspace Zig (no Visual Studio needed).
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
ENET="${ENET:-../../../../tools/enet/enet-1.3.18}"
OUT="${OUT:-build/dinput8.dll}"   # OUT=build-steam/dinput8.dll builds somewhere else (a second agent may be using build/)
mkdir -p "$(dirname "$OUT")"
"$ZIG" cc -target x86-windows-gnu -O2 -fno-omit-frame-pointer -shared -o "$OUT" \
  -DWIN32 -I"$ENET/include" proxy.c testmode.c d3d.c steam.c diag.c classes.c trace.c inject.c console.c mute.c net.c coop3.c dinput8.def \
  menu.c lobby.c lobby_text.c overlay.c \
  coop_proto.c coop_net.c coop_steam.c coop_steam_real.c coop_session.c \
  "$ENET/callbacks.c" "$ENET/compress.c" "$ENET/host.c" "$ENET/list.c" "$ENET/packet.c" "$ENET/peer.c" "$ENET/protocol.c" "$ENET/win32.c" \
  -luser32 -lgdi32 -lkernel32 -lws2_32 -liphlpapi -lole32 -luuid -lwinmm
