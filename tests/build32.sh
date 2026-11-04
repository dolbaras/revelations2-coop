#!/bin/sh
# Build the 32-bit test of the REAL Steam platform (coop_steam_real.c) against the fake game-side Steam: build/32/test_steam_real32.exe + build/32/steam_api.dll
#   sh tests/build32.sh && (cd tests/build/32 && ./test_steam_real32.exe [idoff=16] [slots=1])
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
ENET="${ENET:-../../../../tools/enet/enet-1.3.18}"
mkdir -p build/32
ENETSRC="$ENET/callbacks.c $ENET/compress.c $ENET/host.c $ENET/list.c $ENET/packet.c $ENET/peer.c $ENET/protocol.c $ENET/win32.c"
CF="-target x86-windows-gnu -O1 -g -Wall -Wno-unused-function -DWIN32"
"$ZIG" cc $CF -shared -o build/32/steam_api.dll steam_fake32.c
"$ZIG" cc $CF -I"$ENET/include" -o build/32/test_steam_real32.exe test_steam_real32.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c $ENETSRC -lws2_32 -lwinmm
