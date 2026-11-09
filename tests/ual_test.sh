#!/bin/sh
# Offline check (no game): the mod's import hooks with an Ultimate ASI Loader-like winmm.dll initialised before it. The mod's DLL imports
# winmm, so a loader running as winmm.dll (the installer's copy of the player's loader, or one the player put there) always runs first; the
# loader patches the exe's kernel32 slots in its DllMain and writes the values it saw back at the exe's first call (its source, v10.0.2
# startup.cpp; the Fusion Fix's v7.8.0 did the same live, docs/releasing.md H2).
#   ual_fake.c     the loader's start-up as a stand-in, built as winmm.dll
#   ual_standin.c  a stand-in for rerev2.exe with the imports the mod hooks (+ ual_xinput.c by ordinal, ual_steam.c)
# The mod's DLL: ../native/build/dinput8.dll or MOD_DLL=<file> (REV2_ANYEXE=1: its gate is the real exe's hash).
# What has to hold: the loader writes back exactly the mod's three log-only hooks it also wraps (GetProcAddress, LoadLibraryW: "dyn:" lines;
# Sleep: "diag:" lines); every hook the co-op needs (ExitProcess log, CreateThread / WaitFor* / SleepEx, d3d9!Direct3DCreate9, the XInput
# glue #2-#5, the eight steam_api hooks) stays in place. A new product hook on a function the loader wraps would fail here.
# Usage: sh tests/ual_test.sh   (exit 0 when every check passes; ZIG may hold spaces)
set -e
cd "$(dirname "$0")"
for t in ../../../../tools ../../../../../tools; do
    [ -z "${ZIG:-}" ] && [ -f "$t/zig/zig-x86_64-windows-0.16.0/zig.exe" ] && ZIG="$t/zig/zig-x86_64-windows-0.16.0/zig.exe"
done
DLL="${MOD_DLL:-../native/build/dinput8.dll}"
[ -f "$DLL" ] || { echo "ual_test: no $DLL (sh native/build.sh, or MOD_DLL=<file>)"; exit 1; }
OUT=build/ualtest
rm -rf "$OUT"
mkdir -p "$OUT"
cc32() { "$ZIG" cc -target x86-windows-gnu -O2 "$@"; }
cc32 -shared -o "$OUT/winmm.dll" ual_fake.c ual_fake.def
cc32 -shared -o "$OUT/xinput1_3.dll" ual_xinput.c ual_xinput.def
cc32 -shared -o "$OUT/steam_api.dll" ual_steam.c
"$ZIG" dlltool -m i386 -d ual_xinput_imp.def -l "$OUT/libxinput_ord.a"
cc32 -o "$OUT/standin.exe" ual_standin.c -ld3d9 -ldinput8 -ldxguid "$OUT/libxinput_ord.a" "$OUT/ual_steam.lib" -lwinmm
fails=0; checks=0
check() { checks=$((checks + 1)); if [ "$2" = 0 ]; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
has() { grep -qi -- "$2" "$1" && echo 0 || echo 1; }
hasnt() { grep -qi -- "$2" "$1" && echo 1 || echo 0; }
# run <case> <with the fake loader 1/0>
run() {
    d="$OUT/$1"
    mkdir -p "$d"
    cp "$OUT/standin.exe" "$OUT/xinput1_3.dll" "$OUT/steam_api.dll" "$d/"
    cp "$DLL" "$d/dinput8.dll"
    [ "$2" = 1 ] && cp "$OUT/winmm.dll" "$d/winmm.dll"
    : > "$d/fake.txt"
    w=$(cd "$d" && pwd -W)
    (cd "$d" && UAL_FAKE_LOG="$w/fake.txt" REV2_LOG="$w/rev2.log" REV2_ANYEXE=1 ./standin.exe > out.txt 2>&1) || true
}
iat() { grep "iat(first DirectInput8Create)" "$1" | grep "patched import slots" | head -1; }

run plain 0
L="$OUT/plain/rev2.log"
check "no loader: the mod's hooks are in ($(grep -c 'not found in the exe' "$L") of its imports not in the stand-in)" $(has "$L" "d3d: Direct3DCreate9 import hooked")
check "no loader: every patched slot intact at the first DirectInput8Create ($(iat "$L" | sed 's/.*: //'))" $(iat "$L" | grep -q ", 0 changed" && echo 0 || echo 1)

run fake 1
L="$OUT/fake/rev2.log"
check "the fake loader patched the exe in its DllMain and wrote its slots back at the exe's first call" $( [ "$(has "$OUT/fake/fake.txt" "patched (DllMain)")$(has "$OUT/fake/fake.txt" "written back")" = 00 ] && echo 0 || echo 1)
check "with the loader first: 3 slots changed ($(iat "$L" | sed 's/.*: //'))" $(iat "$L" | grep -q ", 3 changed" && echo 0 || echo 1)
for f in GetProcAddress LoadLibraryW Sleep; do
    check "with the loader first: the log-only hook KERNEL32!$f is written back (as live, H2)" $(has "$L" "KERNEL32.dll!$f slot changed")
done
for f in "KERNEL32.dll!ExitProcess" "KERNEL32.dll!CreateThread" "KERNEL32.dll!WaitForSingleObject" "KERNEL32.dll!WaitForMultipleObjects" "KERNEL32.dll!SleepEx" \
         "d3d9.dll!Direct3DCreate9" "XINPUT1_3.dll!#2" "XINPUT1_3.dll!#3" "XINPUT1_3.dll!#4" "XINPUT1_3.dll!#5" "steam_api.dll!SteamAPI_Init" \
         "steam_api.dll!SteamAPI_RunCallbacks" "steam_api.dll!SteamAPI_RegisterCallback" "steam_api.dll!SteamInternal_CreateInterface"; do
    check "with the loader first: $f stays hooked" $(hasnt "$L" "$f slot changed")
done
check "with the loader first: the mod still gates, hooks and answers DirectInput8Create" $( [ "$(has "$L" "d3d: Direct3DCreate9 import hooked")$(has "$OUT/fake/out.txt" "DirectInput8Create hr=")" = 00 ] && echo 0 || echo 1)
echo "ual_test: $((checks - fails))/$checks passed"
[ "$fails" = 0 ]
