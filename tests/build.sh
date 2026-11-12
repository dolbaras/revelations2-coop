#!/bin/sh
# Build the offline tests and the headless bot (native 64-bit Windows, workspace Zig) into build/.
#   sh tests/build.sh          builds test_proto.exe test_session.exe test_steam.exe test_lobby.exe test_ach.exe test_glue.exe coop_bot.exe  (also test_bgl.exe, test_radio.exe, test_pad.exe)
#   ZIG / ENET may hold spaces (a workspace folder such as "D:\My Mods"): every path is quoted, the ENet sources are a positional list ("$@").
#   The defaults fit mod\tests and a worktree one level deeper (work\wt-*\tests).
set -e
cd "$(dirname "$0")"
for t in ../../../../tools ../../../../../tools; do
    [ -z "${ZIG:-}" ] && [ -f "$t/zig/zig-x86_64-windows-0.16.0/zig.exe" ] && ZIG="$t/zig/zig-x86_64-windows-0.16.0/zig.exe"
    [ -z "${ENET:-}" ] && [ -d "$t/enet/enet-1.3.18" ] && ENET="$t/enet/enet-1.3.18"
done
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
ENET="${ENET:-../../../../tools/enet/enet-1.3.18}"
TARGET="${TARGET:-x86_64-windows-gnu}"
mkdir -p build
set -- "$ENET/callbacks.c" "$ENET/compress.c" "$ENET/host.c" "$ENET/list.c" "$ENET/packet.c" "$ENET/peer.c" "$ENET/protocol.c" "$ENET/win32.c"
CF="-target $TARGET -O1 -g -Wall -Wno-unused-function -DWIN32"
"$ZIG" cc $CF -o build/test_proto.exe test_proto.c ../native/coop_proto.c
"$ZIG" cc $CF -I"$ENET/include" -o build/test_session.exe test_session.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c "$@" -lws2_32 -lwinmm
# steamgap (notes\steamgap.md, Steam run M3 2026-11-08: gap=1384 with 99 % received): the guest's game loop away while its network thread receives. The suite must
# pass, and built as the transport + session before the fix (every expired state counted as a gap, no log line) it has to fail
if ./build/test_session.exe > build/test_session.txt; then tail -1 build/test_session.txt; else grep "^FAIL" build/test_session.txt; tail -1 build/test_session.txt; echo "FAIL: test_session"; exit 1; fi
"$ZIG" cc $CF -DR2_MODEL_BEFORE_STEAMGAP -I"$ENET/include" -o build/test_session_pregap.exe test_session.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c "$@" -lws2_32 -lwinmm
if ./build/test_session_pregap.exe > build/test_session_pregap.txt; then echo "FAIL: test_session passes on the model before the steamgap fix"; exit 1; fi
"$ZIG" cc $CF -I"$ENET/include" -o build/test_steam.exe test_steam.c steam_mock.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c "$@" -lws2_32 -lwinmm
# steamgap over the Steam backend (mock universe): the same pattern through the Steam receive loop; the suite must pass and the pre-fix model must fail
if ./build/test_steam.exe > build/test_steam.txt; then tail -1 build/test_steam.txt; else grep "^FAIL" build/test_steam.txt; tail -1 build/test_steam.txt; echo "FAIL: test_steam"; exit 1; fi
"$ZIG" cc $CF -DR2_MODEL_BEFORE_STEAMGAP -I"$ENET/include" -o build/test_steam_pregap.exe test_steam.c steam_mock.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c "$@" -lws2_32 -lwinmm
if ./build/test_steam_pregap.exe > build/test_steam_pregap.txt; then echo "FAIL: test_steam passes on the model before the steamgap fix"; exit 1; fi
"$ZIG" cc $CF -I"$ENET/include" -o build/coop_bot.exe coop_bot.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c "$@" -lws2_32 -lwinmm
"$ZIG" cc $CF -I"$ENET/include" -o build/test_lobby.exe test_lobby.c ../native/lobby.c ../native/lobby_text.c ../native/coop_proto.c ../native/coop_net.c ../native/coop_steam.c ../native/coop_steam_real.c ../native/coop_session.c "$@" -lws2_32 -lwinmm
"$ZIG" cc $CF -o build/test_ach.exe test_ach.c
"$ZIG" cc $CF -o build/test_glue.exe test_glue.c
"$ZIG" cc $CF -o build/test_bgl.exe test_bgl.c
"$ZIG" cc $CF -o build/test_objsync.exe test_objsync.c
# the object sync checks (stage 10) must describe what release-020 did NOT do: built as the model of release-020 they have to fail
"$ZIG" cc $CF -DOS_MODEL_RELEASE020 -o build/test_objsync_r020.exe test_objsync.c
if ./build/test_objsync_r020.exe > build/test_objsync_r020.txt; then echo "FAIL: test_objsync passes on the release-020 model (the checks do not discriminate)"; exit 1; fi
# the echo rule of the pair run osPB (2026-11-07 07:22:56): built as the logic before it the checks have to fail
"$ZIG" cc $CF -DOS_MODEL_BEFORE_OSPB -o build/test_objsync_preospb.exe test_objsync.c
if ./build/test_objsync_preospb.exe > build/test_objsync_preospb.txt; then echo "FAIL: test_objsync passes on the model before the osPB echo rule"; exit 1; fi
# the finale helicopters of stage 454 (live allepP20): never mirrored; built as the logic before the rule the checks have to fail
"$ZIG" cc $CF -DOS_MODEL_BEFORE_HELI -o build/test_objsync_preheli.exe test_objsync.c
if ./build/test_objsync_preheli.exe > build/test_objsync_preheli.txt; then echo "FAIL: test_objsync passes on the model before the helicopter rule"; exit 1; fi
# the enemy records' window (live allepP28: the 13th and 14th enemy of stage 351 never had a record): the suite must pass, the model before the window must fail
"$ZIG" cc $CF -o build/test_wrec.exe test_wrec.c
if ./build/test_wrec.exe > build/test_wrec.txt; then tail -1 build/test_wrec.txt; else grep "^FAIL" build/test_wrec.txt; echo "FAIL: test_wrec"; exit 1; fi
"$ZIG" cc $CF -DWREC_MODEL_BEFORE_ROT -o build/test_wrec_prerot.exe test_wrec.c
if ./build/test_wrec_prerot.exe > build/test_wrec_prerot.txt; then echo "FAIL: test_wrec passes on the model before the record window"; exit 1; fi
# 0.3.1 R2: up to 128 live enemies, up to 4 batches a tick while more than 12 live (notes/fix031-plan.md); built as 0.3.0 (48 at most, one batch) the checks have to fail
"$ZIG" cc $CF -DWREC_MODEL_BEFORE_031 -o build/test_wrec_pre031.exe test_wrec.c
if ./build/test_wrec_pre031.exe > build/test_wrec_pre031.txt; then echo "FAIL: test_wrec passes on the model of 0.3.0 (one batch a tick, 48 enemies)"; exit 1; fi
# the carried box of the pair run wpB3c (2026-11-07 22:40): its relative +0x40 went out as a world position; built as the logic before the fix the checks have to fail
"$ZIG" cc $CF -DOS_MODEL_BEFORE_WPB3C -o build/test_objsync_prewpb3c.exe test_objsync.c
if ./build/test_objsync_prewpb3c.exe > build/test_objsync_prewpb3c.txt; then echo "FAIL: test_objsync passes on the model before the wpB3c carry rule"; exit 1; fi
# the put-down of the pair run wpB3d (2026-11-07 23:13 / 23:21): the box keeps its carrier as actor; built as the logic before the attach rule the checks have to fail
"$ZIG" cc $CF -DOS_MODEL_BEFORE_WPB3D -o build/test_objsync_prewpb3d.exe test_objsync.c
if ./build/test_objsync_prewpb3d.exe > build/test_objsync_prewpb3d.txt; then echo "FAIL: test_objsync passes on the model before the wpB3d put-down rule"; exit 1; fi
# 0.2.1 coopsave (native/coopsave_logic.h): a co-op game uses its own save; the solo save (Steam's DATA0.DAT) is never written by it. Built as the 0.2.0 model
# (no routing: the co-op checkpoints go into the solo save) the checks have to fail
"$ZIG" cc $CF -o build/test_coopsave.exe test_coopsave.c
if ./build/test_coopsave.exe > build/test_coopsave.txt; then tail -1 build/test_coopsave.txt; else grep "^FAIL" build/test_coopsave.txt; echo "FAIL: test_coopsave"; exit 1; fi
"$ZIG" cc $CF -DCPS_MODEL_BEFORE -o build/test_coopsave_pre.exe test_coopsave.c
if ./build/test_coopsave_pre.exe > build/test_coopsave_pre.txt; then echo "FAIL: test_coopsave passes on the 0.2.0 model (the co-op game writes the solo save there)"; exit 1; fi
# 0.3.1 C1: Continue for every episode and half of the room (Episode 1 exactly as before); built with the 0.3.0 rule (Episode 1 only) the checks have to fail
"$ZIG" cc $CF -DCPS_MODEL_BEFORE_031 -o build/test_coopsave_pre031.exe test_coopsave.c
if ./build/test_coopsave_pre031.exe > build/test_coopsave_pre031.txt; then echo "FAIL: test_coopsave passes on the 0.3.0 rule (Continue for Episode 1 only)"; exit 1; fi
"$ZIG" cc $CF -o build/test_radio.exe test_radio.c
"$ZIG" cc $CF -o build/test_warp.exe test_warp.c
# the warp back to a part start (run w1 2026-11-07 13:10: no player, black screen): built as the first version the checks have to fail
"$ZIG" cc $CF -DWL_MODEL_BEFORE_W1 -o build/test_warp_prew1.exe test_warp.c
if ./build/test_warp_prew1.exe > build/test_warp_prew1.txt; then echo "FAIL: test_warp passes on the model before the part-start fix"; exit 1; fi
# the retry coupling (the pause menu's Retry split the pair): built as rev2-int before it the checks have to fail
"$ZIG" cc $CF -DWL_MODEL_BEFORE_RC -o build/test_warp_prerc.exe test_warp.c
if ./build/test_warp_prerc.exe > build/test_warp_prerc.txt; then echo "FAIL: test_warp passes on the model before the retry coupling"; exit 1; fi
# 0.3.1 R1: the Barry-half start jump repeated until the partner answers from part 1, the partner idempotent (notes/fix031-plan.md); built as 0.3.0 the checks have to fail
"$ZIG" cc $CF -DWL_MODEL_BEFORE_031 -o build/test_warp_pre031.exe test_warp.c
if ./build/test_warp_pre031.exe > build/test_warp_pre031.txt; then echo "FAIL: test_warp passes on the model of 0.3.0 (one jump, no answer)"; exit 1; fi
# 0.2.0 MHIT: the client's real blows on enemy replicas (native/meleehit_logic.h); built as rev2-int a8a067e (the skipped replica update looked at nothing) the checks have to fail
"$ZIG" cc $CF -o build/test_meleehit.exe test_meleehit.c
"$ZIG" cc $CF -DMH_MODEL_BEFORE_FIX -o build/test_meleehit_pre.exe test_meleehit.c
if ./build/test_meleehit_pre.exe > build/test_meleehit_pre.txt; then echo "FAIL: test_meleehit passes on the model before the MHIT fix"; exit 1; fi
# the host's dead-enemy guard of pair run m1 (2026-11-08): built as the logic before it the checks have to fail
"$ZIG" cc $CF -DMH_MODEL_BEFORE_DEADGUARD -o build/test_meleehit_predead.exe test_meleehit.c
if ./build/test_meleehit_predead.exe > build/test_meleehit_predead.txt; then echo "FAIL: test_meleehit passes on the model before the dead-enemy guard"; exit 1; fi
# combat2: the host's part damage for a client's blow (pair run combat2-a: a client's head shot gave no head stagger); built as the logic before it the checks have to fail
"$ZIG" cc $CF -DMH_MODEL_BEFORE_PARTDMG -o build/test_meleehit_prepart.exe test_meleehit.c
if ./build/test_meleehit_prepart.exe > build/test_meleehit_prepart.txt; then echo "FAIL: test_meleehit passes on the model before the part damage"; exit 1; fi
# LAB aim aid `net aim` (native/aim_logic.h): the shot vector of this PC's human turned to a target; built as the lab tool before it (body turned only) the checks have to fail
"$ZIG" cc $CF -o build/test_aim.exe test_aim.c
"$ZIG" cc $CF -DAIM_MODEL_BEFORE_FIX -o build/test_aim_pre.exe test_aim.c
if ./build/test_aim_pre.exe > build/test_aim_pre.txt; then echo "FAIL: test_aim passes on the model before the aim aid"; exit 1; fi
# 0.2.0 grapple prompt (native/grapple_logic.h): the replica's alerted bit follows the host record; built as the logic before it (the record's +0x37B8 never applied) the checks have to fail
"$ZIG" cc $CF -o build/test_grapple.exe test_grapple.c
"$ZIG" cc $CF -DGP_MODEL_BEFORE_FIX -o build/test_grapple_pre.exe test_grapple.c
if ./build/test_grapple_pre.exe > build/test_grapple_pre.txt; then echo "FAIL: test_grapple passes on the model before the alert mirror"; exit 1; fi
# the notice byte of the pair run m1 (2026-11-08 04:33, +0x6F8B kept by the replica): built as 3d85f44 (alert mirror only) the checks have to fail
"$ZIG" cc $CF -DGP_MODEL_BEFORE_NOTICE -o build/test_grapple_prenotice.exe test_grapple.c
if ./build/test_grapple_prenotice.exe > build/test_grapple_prenotice.txt; then echo "FAIL: test_grapple passes on the model before the notice mirror"; exit 1; fi
# combat2 (notes\combat2.md): the puppet shows the partner's grapple ids and the host drops the puppet's own blows; built as rev2-int 5a0af8d (mask without them, no drop) the checks have to fail
"$ZIG" cc $CF -o build/test_combat2.exe test_combat2.c
"$ZIG" cc $CF -DGP_MODEL_BEFORE_PUPPET -o build/test_combat2_pre.exe test_combat2.c
if ./build/test_combat2_pre.exe > build/test_combat2_pre.txt; then echo "FAIL: test_combat2 passes on the model before the puppet fix"; exit 1; fi
# combat3 (notes\combat3.md): the puppet replays the partner's knife, throws, dodge, crouch, flashlight, reload, heal and hit reactions, and an action the puppet drops at
# once is not switched to again every pass; built as rev2-int f0c66af (the combat2 mask, the plain rule) the checks have to fail
"$ZIG" cc $CF -o build/test_combat3.exe test_combat3.c
"$ZIG" cc $CF -DPA_MODEL_BEFORE_C3 -o build/test_combat3_pre.exe test_combat3.c
if ./build/test_combat3_pre.exe > build/test_combat3_pre.txt; then echo "FAIL: test_combat3 passes on the model before the combat3 fix"; exit 1; fi
# combat4 (notes\combat4.md): the partner's inventory copy follows its owner (consumed items, rounds, herbs, the GUI hand-over, checkpoint / retry), the puppet's hand
# follows an empty hand and the light, the partner's throw is shown with its damage once; built as rev2-int 7f75163 (only PICKUP / GRANT, the hand set for slots 1..3, no throw) the checks have to fail
"$ZIG" cc $CF -o build/test_combat4.exe test_combat4.c
./build/test_combat4.exe || { echo "FAIL: test_combat4"; exit 1; }
"$ZIG" cc $CF -DIM_MODEL_BEFORE_C4 -DEQ_MODEL_BEFORE_C4 -DTH_MODEL_BEFORE_C4 -o build/test_combat4_pre.exe test_combat4.c
if ./build/test_combat4_pre.exe > build/test_combat4_pre.txt; then echo "FAIL: test_combat4 passes on the model before the combat4 fix"; exit 1; fi
# combat4 follow-up: the battery-only changes of Moira's light are throttled; built as rev2-int 3e55c72 (every change at once) the check has to fail
"$ZIG" cc $CF -DIM_MODEL_BEFORE_THROTTLE -o build/test_combat4_prethr.exe test_combat4.c
if ./build/test_combat4_prethr.exe > build/test_combat4_prethr.txt; then echo "FAIL: test_combat4 passes on the model before the battery throttle"; exit 1; fi
# flashlive CH1 (pair run flashlive-a 22:50): the real inventory screen's pass ran outside the vf5 bracket and the mirror took the bottle back; built as rev2-int 6b40a90
# (no per-frame pass watcher) the checks have to fail
"$ZIG" cc $CF -DIM_MODEL_BEFORE_FL -o build/test_combat4_prefl.exe test_combat4.c
if ./build/test_combat4_prefl.exe > build/test_combat4_prefl.txt; then echo "FAIL: test_combat4 passes on the model before the pass watcher"; exit 1; fi
# bomb (notes\bomb.md): the partner's thrown bomb has one damage path (the thrower's real explosion; the copy explodes for the eye through BOOM, its blows dropped by the
# copy table), the kill credit of a weapon's / a burn tick's blow, the MHIT log budgets; built as rev2-int b94ff2e (no BOOM, P2 by +0x19C0 only, one log budget) the checks have to fail
"$ZIG" cc $CF -o build/test_bomb.exe test_bomb.c
if ./build/test_bomb.exe > build/test_bomb.txt; then tail -1 build/test_bomb.txt; else grep "^FAIL\|case" build/test_bomb.txt; echo "FAIL: test_bomb"; exit 1; fi
"$ZIG" cc $CF -DBOMB_MODEL_BEFORE -DMH_MODEL_BEFORE_BOMB -o build/test_bomb_pre.exe test_bomb.c
if ./build/test_bomb_pre.exe > build/test_bomb_pre.txt; then echo "FAIL: test_bomb passes on the model before the bomb fix"; exit 1; fi
"$ZIG" cc $CF -o build/test_testbox.exe test_testbox.c
"$ZIG" cc $CF -o build/test_devreset.exe test_devreset.c
"$ZIG" cc $CF -o build/test_pad.exe test_pad.c
# flashlive R2-6 (pair runs l5f..l5i, combat4 c): every load swapped the seats of a slot-1 human (the game's spawn), its cPad read pad 0 = a neutral game user
# while the real pad goes to game user 1; the suite must pass, the model before the fix (rev2-int 6b40a90) must fail
if ./build/test_pad.exe > build/test_pad.txt; then tail -1 build/test_pad.txt; else grep "^FAIL" build/test_pad.txt; echo "FAIL: test_pad"; exit 1; fi
"$ZIG" cc $CF -DPAD_MODEL_BEFORE_R26 -o build/test_pad_prer26.exe test_pad.c
if ./build/test_pad_prer26.exe > build/test_pad_prer26.txt; then echo "FAIL: test_pad passes on the model before R2-6"; exit 1; fi
"$ZIG" cc $CF -o build/test_downed.exe test_downed.c
# 0.2.0 lobby-must: the in-game rules for a lost partner / host, End Co-Op and the end of Episode 1 (native/lobbymust_logic.h)
"$ZIG" cc $CF -I"$ENET/include" -o build/test_lobbymust.exe test_lobbymust.c
if ./build/test_lobbymust.exe > build/test_lobbymust.txt; then tail -1 build/test_lobbymust.txt; else grep "^FAIL" build/test_lobbymust.txt; echo "FAIL: test_lobbymust"; exit 1; fi
# 0.3.0 all-episodes: the end rule covers every episode of the room and waits for a played stage (the game's start passes a stage 9000); built as the 0.2.x rule (Episode 1 only, no latch) the checks have to fail
"$ZIG" cc $CF -DLM_MODEL_BEFORE_ALLEP -I"$ENET/include" -o build/test_lobbymust_pre.exe test_lobbymust.c
if ./build/test_lobbymust_pre.exe > build/test_lobbymust_pre.txt; then echo "FAIL: test_lobbymust passes on the 0.2.x end rule (Episode 1 only)"; exit 1; fi
# the CANCEL + BEGIN case of run C on rev2-int (2026-11-07 06:53) must describe what the logic before the fix did NOT do: built as that model the checks have to fail
"$ZIG" cc $CF -DDL_MODEL_BEFORE_RUNC0653 -o build/test_downed_pre0653.exe test_downed.c
if ./build/test_downed_pre0653.exe > build/test_downed_pre0653.txt; then echo "FAIL: test_downed passes on the model before the run C 06:53 fix (the checks do not discriminate)"; exit 1; fi
# the main-role down of run C (2026-11-07 08:22): built as the logic before that fix the checks have to fail
"$ZIG" cc $CF -DDL_MODEL_BEFORE_RUNC0822 -o build/test_downed_pre0822.exe test_downed.c
if ./build/test_downed_pre0822.exe > build/test_downed_pre0822.txt; then echo "FAIL: test_downed passes on the model before the run C 08:22 fix"; exit 1; fi
"$ZIG" cc $CF -o build/test_gametex.exe test_gametex.c ../native/gametex.c
ARC=""
for d in "${GAME_DIR:-}" ../../game ../../work/game; do
    [ -n "$d" ] && [ -f "$d/nativePCNext/arc/DX9NEXT/Msg_eng.arc" ] && ARC="$d/nativePCNext/arc/DX9NEXT/Msg_eng.arc" && break
done
if [ -n "$ARC" ]; then ./build/test_gametex.exe "$ARC" rev2; else echo "(no game folder found: only the inflate checks run; set GAME_DIR=<game folder>)"; ./build/test_gametex.exe; fi
# 0.2.0 lobby-must: the End Co-Op row is drawn by the game's own GUI font: every character of its label / help must be a glyph of that language's face
"$ZIG" cc $CF -o build/test_lm_glyphs.exe test_lm_glyphs.c ../native/gametex.c ../native/lobby_text.c
if [ -n "$ARC" ]; then ./build/test_lm_glyphs.exe "${ARC%/*}"; else ./build/test_lm_glyphs.exe; fi
# the lobby overlay's device Reset path without a device: release + render, 3 rounds, with the game's own faces when a game folder was found. Rev1's shared
# Japanese face once crashed its live game at the first Reset with a double free (0xC0000374); the heaps must stay valid, no GDI object may leak per round
"$ZIG" cc -target "$TARGET" -O1 -g -Wall -Wno-unused-function -o build/overlay_preview.exe overlay_preview.c ../native/overlay.c ../native/gametex.c ../native/lobby_text.c -ld3d9 -lgdi32 -luser32 -lm
if ./build/overlay_preview.exe --reset-cycle "${ARC%/*}" | grep "^reset cycle: 3 x release + render ok"; then :; else echo "FAIL: overlay reset cycle"; exit 1; fi
# 0.2.0 lobby-must: every in-game strip (loss, end of Episode 1, waiting, silence, downed) fits the one-line strip at 16:9, 16:10, 4:3 and 5:4 with a long name
if ./build/overlay_preview.exe --banner-fit | tail -1 | grep ", 0 too wide,"; then :; else echo "FAIL: a strip is too wide"; exit 1; fi
# 0.2.0 resilience texts (refusal reasons, the partner-silent strip) in 12 languages: formats, lengths, UTF-8
"$ZIG" cc $CF -o build/test_resil.exe test_resil.c ../native/lobby_text.c
if ./build/test_resil.exe > build/test_resil.txt; then tail -1 build/test_resil.txt; else grep "^FAIL" build/test_resil.txt; echo "FAIL: test_resil"; exit 1; fi
# lang (owner 2026-11-08): no co-op text of a non-English language falls back to English: every own column, the CJK columns of Latin-only stock entries, every English
# reason of coop_steam.c, and (with the game folder) the caption the game itself shows in each language
"$ZIG" cc $CF -o build/test_lang.exe test_lang.c ../native/lobby_text.c
if ./build/test_lang.exe "${ARC%/nativePCNext/*}" > build/test_lang.txt; then tail -2 build/test_lang.txt; else grep "^FAIL" build/test_lang.txt; tail -1 build/test_lang.txt; echo "FAIL: test_lang"; exit 1; fi
# 0.2.0 the chain rule (native/chain_logic.h): an Ultimate ASI Loader or a copy of the mod as dinput8_real.dll is never loaded, a plain wrapper is.
# Real files when present: the system's 32-bit dinput8.dll (load), the mod's own build (copy of the mod), UAL_FILES="<dll>;<dll>" (loaders, not in the repository)
"$ZIG" cc $CF -o build/test_chain.exe test_chain.c
set -- ./build/test_chain.exe
for f in /c/Windows/SysWOW64/dinput8.dll; do [ -f "$f" ] && set -- "$@" --load "$f"; done
for f in ../native/build/dinput8.dll "${OUT_DLL:-}"; do [ -n "$f" ] && [ -f "$f" ] && set -- "$@" --coop "$f"; done
OLDIFS=$IFS; IFS=';'; for f in ${UAL_FILES:-}; do [ -f "$f" ] && set -- "$@" --ual "$f"; done; IFS=$OLDIFS
if "$@" > build/test_chain.txt; then tail -2 build/test_chain.txt; else cat build/test_chain.txt; echo "FAIL: test_chain"; exit 1; fi
# (ual-rev2) an Ultimate ASI Loader-like winmm.dll initialised before the mod (the mod imports winmm: the installer's copy of a player's loader runs so):
# it may write back only the mod's three log-only kernel32 hooks it wraps itself; every hook the co-op needs stays (tests/ual_test.sh, 32-bit stand-ins).
# The mod's DLL: ../native/build/dinput8.dll or OUT_DLL
UDLL=""; for f in "${OUT_DLL:-}" ../native/build/dinput8.dll; do [ -z "$UDLL" ] && [ -n "$f" ] && [ -f "$f" ] && UDLL="$f"; done
if [ -n "$UDLL" ]; then MOD_DLL="$UDLL" ZIG="$ZIG" sh ual_test.sh > build/ual_test.txt || { cat build/ual_test.txt; echo "FAIL: ual_test"; exit 1; }; tail -1 build/ual_test.txt
else echo "(ual_test: no native/build/dinput8.dll; sh native/build.sh first, or OUT_DLL=<file>)"; fi
