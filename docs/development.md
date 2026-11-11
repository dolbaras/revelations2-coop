# Development: layout, test variables, test console

This is the developer material that the README carried up to 0.3.0 (moved here unchanged when the README took the common layout of the author's
co-op mods). Nothing here is needed to play: a player's game sets none of these variables, and the console does not exist without `REV2_CONSOLE`.

## Layout

- `native/` - `dinput8.dll` proxy (C, 32-bit, built with Zig: `sh native/build.sh`; the ENet 1.3.18 sources are downloaded separately)
  - `proxy.c` log (rotated), exe SHA-256 gate, crash log, import patching, forwarding of `DirectInput8Create`
    (a `dinput8_real.dll` next to it, e.g. a renamed Ultimate ASI Loader, is chained instead of the system DLL)
  - `testmode.c` window placement, background mode, adapter choice, input rules for test runs
  - `d3d.c` Direct3D 9 hooks and screenshots (F11 or console `shot`)
  - `steam.c` timing of the game's Steam calls, Steam callbacks and `ISteamUserStats` logging (log only, never changes a call)
  - `coopsave.c` the co-op save (0.2.1): while a co-op game runs, the game's `DATA0.DAT` calls on `ISteamRemoteStorage` go to a co-op save in `%LOCALAPPDATA%`, so the campaign save is never opened by a co-op game
  - `diag.c` start-up diagnostics (threads, long waits, window messages), `inject.c` synthetic keyboard/mouse for tests
  - `classes.c` class-name walker (vtable[4] -> type info) and bounded object scan, `trace.c` log-only inline hooks and the split-screen calls
  - `console.c` UDP test console on 127.0.0.1, `mute.c` per-process mute
  - `net.c` a passive log of the game's own Winsock and Steam calls for research (`REV2_NETCAP`); it never changes or sends anything
- `tools/r2console.py` test console client
- `tests/` offline tests (`sh tests/build.sh`, `tests/test_installer.ps1`) and the headless bot `tests/coop_bot.c`

Unsupported exe (SHA-256 other than build 3658995): the proxy only forwards `DirectInput8Create`.

## Test variables (all off in a player's game)

| Variable | Effect |
|---|---|
| `REV2_LOG=<file>` | log file (default `rev2_coop.log` next to the exe, rotated) |
| `REV2_CONSOLE=<port>` | UDP console on 127.0.0.1 (`ping`, `version`, `readmem`, `shot`, `pad`, ... `help`) |
| `REV2_WINPOS=x,y` `REV2_WINSIZE=w,h` | window client area position / size |
| `REV2_ADAPTER=window` or `<n>` | Direct3D adapter of the window's monitor / adapter n |
| `REV2_BACKGROUND=1` | windowed, `WS_EX_NOACTIVATE` (never takes focus; the game is told it is active and in front), no keyboard/mouse/pad input unless the window is in front, no mouse clip |
| `REV2_PADS=1` | with background mode: console pads reach the game; the game is told it is the foreground window |
| `REV2_MUTE=1` | mutes this process's audio sessions |
| `REV2_SHOTDIR=<dir>` `REV2_SHOTS=s1,s2` | screenshot folder, timed screenshots |
| `REV2_WINLOG=1` | log window creation |
| `REV2_PASS=1` | pass-through: only window placement, the Present counter and the console (no Steam/pad/dinput/diag hooks) |
| `REV2_TRACE=1` | install the log-only inline hooks (game-mode functions) at the first Present |
| `REV2_NOACH=1` | do not log `ISteamUserStats` calls |
| `REV2_ANYEXE=1` | install hooks on an exe that is not the supported build (lab only) |

## Test console commands

`L <cmd>` runs a command on the game's logic thread (everything that touches the game is queued and executed there). `pad <user> <state>` keeps the spoofed XInput pads of users 0 and 1 "connected" (the game leaves co-op about a second after a pad vanishes), `join 1` makes the game perform its own join of a second human player, `vp <n> <slot>` and `view <mode>` choose which player's viewport is shown full screen, `viewguard <slot>` keeps your own view, `replace <slot> <hash>` swaps the character in a slot, `rec` / `play` record and replay a path into a second character.

Test aids of the co-op layer (lab runs only; they need `REV2_CONSOLE`): `warp <stage> <start position> [half] [keep]` (a stage jump through the game's own restart copy, sent to both PCs), `net w give <item> <count>` (an item for the local human), `net w sf set <flag> <0|1>` (a story flag on the host), `net ach fight ...` / `net ach walk ...` / `net ach follow ...` / `net ach god ...` (scripted fighting, walking, following the partner and kept health for the local human; `help` and each command's usage line give the arguments).

## Console (UDP 127.0.0.1, `tools/r2console.py`)

`help` lists everything. Input: `key`, `keydown`, `keyup`, `mouse`, `mbtn`, `cursor`, `wmkey`, `pad`, `padtap`, `padhold`. Engine: `classname <addr>`, `dtiname`, `objscan <filter|-> <lo> <hi> <file>` (bounded windows only),
`dump <addr> <len> <file>`, `gm`, `setcoop <0|1>`, `setmode`, `setslot`, `readmem`, `dword`. Diagnostics: `diag`, `steam`, `pads`, `dinput`, `stacks`, `shot`, `quit`.
