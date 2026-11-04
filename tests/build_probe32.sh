#!/bin/sh
# 32-bit offline test of native/probe.inc (the machine-code thunk of `net w probe`): sh tests/build_probe32.sh && tests/build/32/test_probe32.exe
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
mkdir -p build/32
"$ZIG" cc -target x86-windows-gnu -O0 -fno-omit-frame-pointer -g -Wall -Wno-unused-function -DWIN32 -o build/32/test_probe32.exe test_probe32.c
