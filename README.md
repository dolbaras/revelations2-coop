# Resident Evil Revelations 2 co-op (work in progress)

Network co-op for the story campaign of Resident Evil Revelations 2 (Steam build 3658995, `rerev2.exe`, 32-bit).
Status: **developer build, not released, no installer yet.** Network co-op for the story campaign runs between two real game instances on two PCs in the author's test lab: each PC shows its own character full screen and the partner is a puppet moved from the network state. Nothing here modifies the game files
and nothing touches the Steam stub: the DLL is loaded by the exe's own import table and only patches import slots
and COM vtables in memory.

## What works (shown on two PCs, test copies)

- **Session and transport**: host / join (ENet over UDP with a direct IP; a Steam transport with a lobby for invites and Steam's relay is built and tested offline, see `docs/steam-transport.md`), a small lobby where each player picks the character they play (as in RE5/RE6), a level-load barrier (up to 300 s, with a "waiting for <name>" strip on the faster PC), 20 Hz character state, the local human plays with keyboard / mouse or a real pad, partner death and retry are coupled.
- **World sync** (stage 6, on by default in a session): the host's story creates the enemies, the other PC gets replicas; a kill is credited only to the human whose slot made it; scenario flags are mirrored; the story's co-op door opens on both PCs from either player's press; enemy grabs of a human are replayed on the partner's puppet and released with it; a human lost in the void or held by a vanished enemy is detected from the game's own game-over state.
- **Achievements for every player** (stage 8b): the game's own stat counters are gated per player, so a record belongs to the player who earned it and shared records go to both. In every test the real Steam unlocks are blocked (`WOULD UNLOCK` is only logged); the end of Episode 1 gave identical records on both PCs.
- **Episode 1**: both halves were taken to the end-of-episode screens on two PCs. In those runs the route was shortened with test-console moves and the end of the half was triggered with the story's own order from the console; walking to the in-game trigger, separated sections, bosses, the bonus episodes (The Struggle, Little Miss), Episodes 2-4, Survival / Invisible and the in-lobby achievement indicator are **not done yet**.

## Layout

- `native/` - `dinput8.dll` proxy (C, 32-bit, built with Zig: `sh native/build.sh`)
  - `proxy.c` log (rotated), exe SHA-256 gate, crash log, import patching, forwarding of `DirectInput8Create`
    (a `dinput8_real.dll` next to it, e.g. a renamed Ultimate ASI Loader, is chained instead of the system DLL)
  - `testmode.c` window placement, background mode, adapter choice, input rules for test runs
  - `d3d.c` Direct3D 9 hooks and screenshots (F11 or console `shot`)
  - `steam.c` timing of the game's Steam calls, Steam callbacks and `ISteamUserStats` logging (log only, never changes a call)
  - `diag.c` start-up diagnostics (threads, long waits, window messages), `inject.c` synthetic keyboard/mouse for tests
  - `classes.c` class-name walker (vtable[4] -> type info) and bounded object scan, `trace.c` log-only inline hooks and the split-screen calls
  - `console.c` UDP test console on 127.0.0.1, `mute.c` per-process mute
- `tools/r2console.py` test console client

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

Unsupported exe (SHA-256 other than build 3658995): the proxy only forwards `DirectInput8Create`.

## Stage 3 console commands

`L <cmd>` runs a command on the game's logic thread (everything that touches the game is queued and executed there). `pad <user> <state>` keeps the spoofed XInput pads of users 0 and 1 "connected" (the game leaves co-op about a second after a pad vanishes), `join 1` makes the game perform its own join of a second human player, `vp <n> <slot>` and `view <mode>` choose which player's viewport is shown full screen, `viewguard <slot>` keeps your own view, `replace <slot> <hash>` swaps the character in a slot, `rec` / `play` record and replay a path into a second character. Details and traps are in the notes of the project.

## Development notes

- **Raid is never touched.** The mod is meant for the story campaign. The game's online Raid mode depends on Capcom's RE.NET service, which was unreachable (HTTPS time-outs from several networks) during development, so Raid could not be studied online. For the author's own tests a local, default-off lab switch was used to get past the game's "service available" check on private test copies; it is a forced measure because the connection to the Resident Evil servers is very unstable, it is **not part of this repository**, and the published mod does nothing to Raid, its servers or its traffic.
- `net.c` is a passive log of the game's own Winsock and Steam calls for research (`REV2_NETCAP`), it never changes or sends anything.
- Credits: ThirteenAG's Fusion Fix (MIT) and Lyall's RERevFix (MIT) were read for patterns and structures; the hub notes of other authors (agressivepr, ANGCRU00) were used as leads and verified before use.

## License

MIT, see `LICENSE`. Not affiliated with Capcom. Ship no game files.

## Console (UDP 127.0.0.1, `tools/r2console.py`)

`help` lists everything. Input: `key`, `keydown`, `keyup`, `mouse`, `mbtn`, `cursor`, `wmkey`, `pad`, `padtap`, `padhold`. Engine: `classname <addr>`, `dtiname`, `objscan <filter|-> <lo> <hi> <file>` (bounded windows only),
`dump <addr> <len> <file>`, `gm`, `setcoop <0|1>`, `setmode`, `setslot`, `readmem`, `dword`. Diagnostics: `diag`, `steam`, `pads`, `dinput`, `stacks`, `shot`, `quit`.
