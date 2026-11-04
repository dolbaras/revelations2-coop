#!/bin/sh
# Build the offline tests and the headless bot (native 64-bit Windows, workspace Zig) into build/.
#   sh tests/build.sh          builds test_proto.exe test_session.exe test_steam.exe test_lobby.exe test_ach.exe test_glue.exe coop_bot.exe
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
ENET="${ENET:-../../../../tools/enet/enet-1.3.18}"
TARGET="${TARGET:-x86_64-windows-gnu}"
mkdir -p build
ENETSRC="$ENET/callbacks.c $ENET/compress.c $ENET/host.c $ENET/list.c $ENET/packet.c $ENET/peer.c $ENET/protocol.c $ENET/win32.c"
CF="-target $TARGET -O1 -g -Wall -Wno-unused-function -DWIN32"
"$ZIG" cc $CF -o build/test_proto.exe test_proto.c ../native/coop_proto.c
"$ZIG" cc $CF -I"$ENET/include" -o build/test_session.exe test_session.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c $ENETSRC -lws2_32 -lwinmm
"$ZIG" cc $CF -I"$ENET/include" -o build/test_steam.exe test_steam.c steam_mock.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c $ENETSRC -lws2_32 -lwinmm
"$ZIG" cc $CF -I"$ENET/include" -o build/coop_bot.exe coop_bot.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c $ENETSRC -lws2_32 -lwinmm
"$ZIG" cc $CF -I"$ENET/include" -o build/test_lobby.exe test_lobby.c ../native/lobby.c ../native/lobby_text.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c $ENETSRC -lws2_32 -lwinmm
"$ZIG" cc $CF -o build/test_ach.exe test_ach.c
"$ZIG" cc $CF -o build/test_glue.exe test_glue.c
