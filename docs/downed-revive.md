# Revelations 2 co-op: downed / revive ("agony")

Status: **designed, implemented offline; the rule itself has never run on hardware.** Two single-instance probe runs on the N100 (2026-11-06, rule off) read the engine numbers and held a forced lying state (section 8). The rule is ON by default from 0.2.0 (lead decision D2, 2026-11-08: host row "Downed / revive" = Yes; `downed=0` in the settings file turns it off); the lab checks C1-C3 passed at 8 points of both halves (noteselease-0.2.0-acceptance.md). Pure logic and its tests: `native/downed_logic.h`, `tests/test_downed.c` (410 checks);
engine glue: `native/downed.inc`; lobby row and session flag: `lobby.c`, `coop_proto.h` (`CP_SESS_DOWNED`). The engine facts behind it are in the private notes (`notes\downed-revive.md`, `notes\downed-finish.md`); the short version is section 7.

## 1. The rule

The stock campaign ends the run for both players the moment one health bar reaches 0. With the rule on (host's choice, lobby row **Downed / revive**: Yes / No):

* When a human's health would reach 0 **and at least one other human is still up**, he goes **DOWN** instead of dying: hp stays at 1, he lies on the floor (the game's own wounded animation chain), cannot act, and **bleeds out on a timer** (default 60 s; presets 30 s / 60 s / 120 s / never).
* A partner who is up and **holds the interact button next to him** for the revive time raises him: the game's own get-up motion, hp restored (at least 35 % of the maximum), the bleed timer is gone.
* **Game over** (the death screen, retry from the checkpoint, as today) when a human **dies** - the timer ran out, a void fall, a finisher the rule cannot soften - or when **nobody is up** any more (everybody DOWN). A hit on a lying human changes nothing (hp is held at 1); the timer is not shortened by hits.
* One human in the session (the partner left, a single-player test): no rule, a lethal hit is a death as before. If the partner leaves while I lie, I stand up (`get_up_on_leave`).
* The AI partner (Moira / Natalia in a one-human session) is not part of the rule in this first version.

## 2. States and transitions (per human, `downed_logic.h`)

| From | Event | To | Effects |
|---|---|---|---|
| UP | lethal hit, another human is UP | DOWN | clamp hp 1, wounded chain 0x79 -> 0x7A, timer starts, event DOWN, STATE flag `CP_SF_DOWN` |
| UP | lethal hit, nobody else is UP | DEAD | ordinary death, game over |
| UP | the engine says dead (hp 0 by another path, void, finisher, predicate) | DEAD | event DEAD, game over |
| DOWN | bleed timer 0 | DEAD | hp := 0, event DEAD, game over on both |
| DOWN | raised (hold complete) | UP | engine completion, hp >= 35 %, event DONE |
| DOWN | all other humans DOWN / DEAD | DEAD | game over |
| any | level load / retry | UP | everything reset (the events of the old epoch are dropped) |
| DOWN | partner left and I am alone | UP | get up (option) |

The timer is **frozen while somebody holds the button** on the lying human and resumes if the hold ends without success. The hold itself restarts from zero after a release (as the stock rescue does).

## 3. Who revives, and how

* **Human partner** (the supported case): hold the **rescue button** within the range (stock prompt: `g_sPlayer+0xCF4`, facing within `+0xCF8`, in sight; mod fallback 170 units) of the lying body. The rescue button is the game's own: keyboard = the rebindable **"Rescue"** key of the key config (key action 0x1E, `Rescue=` in config.ini; Tab in the owner's config, the same key as "change character"), pad = **Y / Triangle** (B / Circle for the pad button types 1..4). Two ways, `net downed mode engine|mod|both` (default both): (a) **the engine's own `pl_act_rescue`** (0x7C): the stock prompt (press) and hold with its progress gauge, time = the game's `g_sPlayer+0xCE4`; the glue only observes it (begin / keep-alive / success / cancel) and forwards it; (b) **the mod's own hold** (`dl_hold_button`: the rescue key through the engine's key query `FUN_0099E1E0`, the slot's pad button through `FUN_0040AD80` and the action-button pad word, + distance), time `DL_REVIVE_DEFAULT_MS` = 3 s, for the case that the engine's action does not start for a puppet. The first version read the action-button byte `+0x19FC`, which the engine writes only while its own 0x7C runs: always 0 for the mod's hold (run C 2026-11-06). Stock bug fixed while the rule is on: on the keyboard the engine wrote the hold of every slot into slot 0's byte, so a role-1 human (Moira / Natalia) could not hold the stock rescue (`net downed kbfix`, 16 bytes at 0x40F02B, checked). A guide (tutorial) panel blocks every action button while it is up (`g_sGUI+0x16C4` = 0; `net downed input` shows it).
* **AI partner**: the game has `cPlAIBhvCoopRescueDying` (the AI walks to the lying human and "presses" the button). Not used by the rule in this version (one-human sessions keep the old death); to be tried after the human case is proven.

## 4. Network

Every PC is the authority for **its own** human (down, dead, bleed-out) and for **its own hold** on the partner (the progress is computed where the input is). Everything about the partner is a mirror fed by reliable events, with the unreliable STATE flags as a debounced second opinion. Both PCs evaluate the game-over condition on their own mirror; "everybody down" and "somebody dead" are stable conditions, so no arbitration is needed. The one race (both take their lethal hit inside one round trip) ends in "both DOWN" on both PCs = game over on both, which is the right answer.

Events (`CP_EVENT`, reliable, ordered; kinds `CP_EV_USER + 0x60..0x65`; body = `u8 sender slot, u8 target slot (255 = none)`, the event's `epoch` must equal the receiver's epoch, otherwise it is dropped):

| Kind | Name | Sent by | Meaning / receiver's reaction |
|---|---|---|---|
| 0x60 | DOWN | the human who went down | mirror: DOWN, bleed timer predicted; puppet lies (0x7A) |
| 0x61 | DEAD | the human who died (bleed-out, void, ...) | mirror: DEAD -> game over |
| 0x62 | BEGIN | the reviver | target: be_rescued pose (0x7D), timer frozen |
| 0x63 | KEEPALIVE | the reviver, every 0.5 s while holding | target's watchdog (2.5 s) stays quiet |
| 0x64 | CANCEL | the reviver | target: back to lying (0x7A), timer runs |
| 0x65 | DONE | the reviver ("I raised target") or the human himself ("I am up", target = himself) | target: stand up (engine completion); everybody's mirror: UP |

`STATE` carries `CP_SF_DOWN` (bit 128, reserved since protocol version 2) and `CP_SF_DEAD`. A disagreement between the STATE flags and the mirror that lasts 600 ms wins (a lost or late event); DEAD is believed at once. A mirror whose bleed timer ran out +1.5 s without the DEAD event is declared dead (a link that stalls must not keep a corpse "lying" for ever). No protocol version change: the new bytes are optional (old peers ignore unknown event kinds).

Session settings: `cp_session.flags` bit 0 = rule on, bits 1..2 = bleed-out preset (0 60 s, 1 30 s, 2 120 s, 3 never). The host's room row toggles bit 0 and keeps the preset bits; the host's default is kept in the settings file (`downed=0|1`, `downed_bleed=0..3`); the console line is `net session <ep> <part> <diff> <flags>`. The client sees the row as information. Lab overrides: `REV2_DOWNED=1`, `net downed on|off|auto`.

## 5. Where it hooks into the mod

* **Damage gate** (`downed_hit`, called from the detoured player SubHP `0x715020`, `world.inc` `h_psub`, and from the PDMG replay): for MY human only. Lying: every hit is absorbed. Lethal and somebody is up: the subtraction is cut to leave hp 1, the logic goes DOWN. Lethal and nobody is up: the death goes through. (Deaths that do not subtract through this function - the enemies' grab finishers, `pl_act_die`, a void fall - are deaths, as the rule cannot save a human who is being killed by a scripted finisher; the support role's finishers already end in the engine's own wounded chain, which the glue adopts: "the engine put my character into its wounded chain".)
* **Per pass** (`downed_frame`): follows the engine for my human (dead / stood up by the engine / pulled out of the chain), ticks the timer, executes the outputs (`GO_DOWN`: hp 1 + ChangeAction 0x79; `DIE`: hp := 0; `GET_UP`: be_rescued + the engine's `FUN_0070CCC0` + hp floor; `REVIVE_PLAY` / `STOP`), observes or runs my hold, keeps the partner's puppet lying (ChangeAction 0x7A on the type-2 puppet) or standing, shows the banner strips (own bleed-out countdown, "your partner is down - hold the action button next to him").
* **STATE**: `CP_SF_DOWN` in `sample_local`, the partner's flags into the mirror in `coop_game_frame`.
* **Existing coupling** (`g_die_with`, `g_auto_retry`): unchanged; a DEAD human still ends the run for both and the retry still follows. The rule only decides that a lethal hit is not yet a death.

Console: `net downed` (status), `on|off|auto`, `bleed <s>`, `hold <ms>`, `range <units>`, `pct <n>`, `mode engine|mod|both`, `hit <n>` (the damage path of an enemy hit on my character, for tests), `engine` (the engine numbers for the live checks), `slots` (raw words of both player slots in one answer, no session needed), `input [slot]` (the rescue input of a slot as the hold reads it: engine byte, rescue key, input mode, pad button and word, guide panel, the stock prompt's target `+0x7BE8` and the `sPlayer` prompt numbers; no session needed), `kbfix on|off` (the keyboard fix above; on by itself when the rule turns on).
Test instances only:
* `probe on|off|reset` (needs `REV2_DOWNED_PROBE=1`): runs every frame from `coop_game_frame`, **with or without a net session**, for slots 0 and 1 whatever they hold (local human, AI partner, type-2 puppet): a line at **every change** of `+0x1A54` (`downed: probe act slot S ...: OLD -> NEW`, at most 12 lines a second per slot and 3000 per arming; changes into or out of 0x76..0x80 are never suppressed; the skipped count is reported), a line at every change of `+0x7BD8 / +0x7BE8 / +0x7BEC / +0x7920` and of the engine's dying flag `gm+0x8A0+slot`, and once per slot and id 0x76..0x80 the class line (DTI of `+0x1A38`, the `sPlayer` numbers, `DAT_0124AD94`).
* `pkt <state 0..3> <sender slot> <target slot>` (needs `REV2_DOWNED_PKT=1`): calls the engine's Raid rescue handler `FUN_009DF820` with a synthesized packet (section 7) after checking its code bytes, its jump table and its `ret 8` (build 3658995 only), on the game thread; prints the target's and the sender's fields before and after and the expected effect. Single player: `pkt 0 0 1` (Barry begins on the lying Natalia: 0x7D), `pkt 3 0 1` (cancel: 0x7A), `pkt 2 0 1` (success: `FUN_0070CCC0`).

## 6. Offline tests (`tests/test_downed.c`, 443 checks)

Two PCs with a latency link, STATE loss, lost events, a stalled game thread, a wrapping 32-bit clock, retry, three humans, repeated hits on a lying human, the engine-driven hold, external stand-up, edge cases of every event, a queue overflow, and a 60-seed fuzz with invariants (lying means DOWN, no lying human at hp > 1, one death call at most, a game over on one PC arrives on the other); the probe's change watch (burst, cap, chain ids, new objects, wrap) and the packet layout / code check of `net downed pkt`; the rescue input (code check of the key query / button resolver / mask table, the button ids by pad type, the hold decision with run C's case "key down, engine byte 0", the stale engine byte of run H, the keyboard fix bytes); the step-1 follow-up of my human with run C's DONE / be_rescued order (no new down from 0x7D or within 3 s after a revive). `tests/test_lobby.c` (907 checks) covers the row, the flag byte and the texts in 12 languages. `tests/run_net_tests.py` has two cases for the lab bot: `downed` (a bot goes down, the other holds, breaks the first hold off, raises it, over the wan relay) and `bot-ctl` (the bot's control file). Existing suites unchanged and green (proto 120955, session 224, steam 158, lobby 907, ach 650, glue 94, bgl 85, radio 64, pad 113, testbox 15, devreset 13).

## 7. Engine facts (summary; evidence in the private notes)

Shown on the pair (two copies on one PC, 2026-11-07, build rev2-int): the support role is raised by the main role (engine hold on the puppet ends without success, the mod hold raises; a short hold cancels), the main role lies and is raised by the support role (also with the keyboard), both down gives one game over per PC and the retry runs on both. Two faults found there and fixed: when the host's engine hold ended and its mod hold began in the same pass, the client got CANCEL and BEGIN together, be_rescued restarted and the engine cleared the rescuer word `+0x7BD8`, so the completion `FUN_0070CCC0` refused: the get-up now writes the rescuer back first, and a human the rule calls up but who still lies with hp above 1 is stood up. And the main role died at once: the "back to the chain" repair put it straight into 0x7A before 0x79 ran, so the stock dying setup (`FUN_007102A0`: gauge `+0x6F54` 150, timer `+0x6F1C` = `g_sPlayer+0x54C`) was missing, and wait_rescue's check (`FUN_006CCBD0`) kills a main-role human in a session when the gauge or the timer is used up: the down now goes through 0x79 first and the rule keeps the gauge and the timer up while it says DOWN (the rule owns the bleed-out).

### Engine facts in short

The game already has the whole chain, for the support role in the campaign and for every role in Raid: `pl_act_dying_start` (0x79) -> `pl_act_wait_rescue` (0x7A / 0x7B, lying, animation 0x8AA, no timer) -> the partner's `pl_act_rescue` (0x7C: hold the button, progress `dt` per frame until `g_sPlayer+0xCE4`, a HUD gauge) -> `pl_act_be_rescued` (0x7D) -> `FUN_0070CCC0` (hp restore, get-up motion 0x5A..0x63). A Raid net message (`FUN_009DF820`, type 9) carries begin / reject / success / cancel: `void __stdcall f(int seat, const u8 *pkt)` (`ret 8`, the incoming ECX is overwritten before use; its one caller is the dispatcher `cCommunicatorPlayer_vf6` 0x9DF220), `seat` only compared with `gm+0x904` (equal = own packet, ignored), the packet read at three places only: bits 13..24 of the dword at +0 = sender slot, byte +4 = state, byte +5 = target slot; every state refuses a type-2 target (begin, success, cancel) or a type-2 sender (reject). There is **no bleed-out timer** in the stock chain (the per-slot dying bookkeeping `FUN_00427650` counts seconds in `gm+0x8A8` but nothing reads them), so the bleed-out is ours. The stock death predicate (`FUN_00426E00`) is hp < 1, ctrl bit 1, pstate bit 14, `+0x7904` bit 28 or the `sBioMain` flag; a human at hp 1 in the chain is not dead for it.

## 8. Live checks (the hardware agent's list)

Seen so far (N100, one instance, rule off, 2026-11-06): item 2's numbers at the title and in a level: hold `+0xCE4` 2.000 s, revive hp `+0xCEC` 100, grace `+0xCF0` 3.000 s, `+0x404` 250.0, `DAT_0124AD94` = 0x40A00000 (5.0 as a float); the AI Natalia forced into hp 1 + ChangeAction(0x79) stayed at act 122 / hp 1 for 11 minutes and the engine raised its dying flag for slot 1 with a running timer (`gm+0x8A0+1` = 1). The interact keys F / E next to her did nothing in single player. The first-sight probe logged nothing there: it ran only inside a net session (fixed: it now runs every frame).

The lab runs (private scripts `work\s6n\downed\run_a.sh`, `run_b.sh`, `run_c.sh`, all on the N100 alone):
* **A** (one instance, single player): the lying AI partner, the probe of both slots, the packet sequence of `net downed pkt` (begin, does 0x7D end by itself, cancel, reject, success).
* **B** (one instance + `tests/coop_bot.exe` on the N100 over 127.0.0.1, `--follow 90 --ctl <file>`): items 4, 7, 8 (partner down + lethal hit), 9 with the bot's partner as the reviver (BEGIN / CANCEL / DONE from the bot) and the puppet as the lying one (the game's human holds).
* **C** (two copies of the game on the N100, the second with `REV2_MULTI=1`, session over 127.0.0.1): items 3, 4, 5, 6, 8 with two real game instances (the same set-up as the pair, without the main PC).
Then the pair (main PC + N100) for items 10-14 and a final pass.

Pair: main PC + N100, one session, `net downed on` on BOTH (or the host row), lab saves snapshot / restore as in the other sessions. Evidence = the `downed:` and `net: action id N first seen` log lines of both PCs.

1. **Rule off = no change.** A normal session run (walk, hit, die, retry) with the row at No: no `downed:` line in the log at all (the rule stays off), the old death coupling unchanged.
2. **Engine numbers**: `net downed engine` on both: hold time `+0xCE4` (expected 1..5 s), revive hp `+0xCEC`, grace `+0xCF0`, `+0x404`; the per-slot dying flags `gm+0x8A0 / +0x8A8`. Put them in the notes; set `DL_REVIVE_DEFAULT_MS` / `g_dl_revive_pct` from them.
3. **Down, support role** (human Moira / Natalia, slot 1): `net downed hit 99999` on her PC (then a real enemy hit): log `LETHAL hit ... absorbed: hp 1, DOWN`, `GO DOWN`; she falls and lies (action 121 -> 122 in the learning log, class names of ids 0x79..0x7D confirmed by the animation), the banner counts down; the other PC logs `event from slot 1: DOWN` and `puppet of slot 1 shown lying`, the puppet lies visibly.
4. **Down, main role** (Claire / Barry, slot 0): the same. This is the untested ground (the stock game never puts a main-role human into the chain): check the pose, the controls (can she still shoot, walk, open the inventory while lying?), the camera.
5. **Revive**: the partner walks up and holds the interact button. Expected: either `engine rescue began` (the stock gauge shows, ids 0x7C / 0x7D appear in the learning log) or, with `net downed mode mod`, `being raised by slot N` after the BEGIN event; the lying human gets the be_rescued pose; at the end `engine completion called`, `got up`, hp >= 35 %, both mirrors UP, `downed: got up` on the PC of the raised one. Interrupt halfway: back to lying, timer resumes, no stuck pose. Repeat 3 times.
6. **Which interaction works** for a type-2 puppet target (the engine's start of 0x7C may refuse a puppet): if no `engine rescue began` appears, the mod-driven hold must work (`dl_hold_button`: which button, `range`); decide `g_dl_hold_mode`.
7. **Bleed-out**: `net downed bleed 10`, down, wait: `DIE`, the death screen on both PCs (the existing coupling), retry from the checkpoint, `level load: reset` and everybody up, the rule works again in the next life.
8. **Both down**: hit one, then the other within a second: `GAME OVER decided (nobody is up)` on both, once.
9. **Enemies and a lying human**: do the stock enemies leave or keep attacking the lying human (hits are absorbed: `hit ... absorbed` lines)? Do the enemies' AI targets wedge on a lying human (stage 6g saw wedges with holds)? Do grab finishers (the Afflicted's) still kill a lying human through the stock path - then the game over comes with `my lying character died (engine)`.
10. **Networks**: 100 ms RTT and 5-10 % loss (`tests\netem.py` on the loopback relay / the pair): the revive completes, no double game over, no stuck pose (the 2.5 s watchdog), DONE of the raiser and the engine completion agree.
11. **Transitions while lying**: stage change / loader barrier, a cutscene, the pause menu, the partner leaving, the retry while lying, a save / checkpoint box (the CHP boxes of s0101) while lying.
12. **Single human + AI partner**, rule on: lethal hit = death with the log line "nobody is up"; and (optional) does the stock AI rescue a lying human in `cPlAIBhvCoopRescueDying`?
13. **HUD**: the banner strips in the game's languages (CJK font, length), the stock rescue prompt on the puppet, the `uGUI` "objective text" Natalia showed at hp 1 / act 122.
14. **Statistics / achievements**: `FUN_009815E0(0, 0x46, 1)` (a counter bumped by the rescue completion) must not unlock anything for the wrong player or twice (the per-player achievement gate is unchanged).

If 3-6 pass on the pair the default of the lobby row can become Yes (decision: only after 7-11 also pass).

## 9. Effort for the remaining live work

About 3-4 pair sessions of 40-60 minutes each (1: items 1-3 + 5 with the stock rescue; 2: items 4, 6, 7, 8; 3: items 9-11; 4: items 10, 12-14 and a long soak in a real room), plus about a day of offline fixes between them (the glue is the guess; the logic is solid). Rev1's split-screen variant is a separate, smaller job (see the Rev1 branch).

## 10. Not done / decisions for the owner

* The AI partner as a reviver (section 3) and the AI as a **target** (an AI partner who is lying while the single human lives) are the stock game's business and untouched.
* Deaths that bypass the SubHP function (grab finishers, scripted deaths, falls) stay deaths in the first version.
* The wording of the 12-language texts is ours and has not been read by native speakers.
* Default of the lobby row: No until the live checks are done.
