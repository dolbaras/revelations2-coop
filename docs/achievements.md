# Steam achievements in co-op (Revelations 2, stage 8b)

Goal: in a co-op session every player's own Steam account earns the achievements that player really earned, as in RE5 and RE6, and nothing else. The mod never calls
`SetAchievement` itself and never writes a record that the game would not write: every unlock goes through the game's own record path on the PC of the human who did it.

## How the game awards an achievement

1. A game event bumps the profile's statistics: `FUN_00979580` adds to the **global total by kind** (`story+0x20+0x1BC`, 81 counters), and right after it `FUN_00972E90` adds the
   same event to the **per-slot** table of the acting player slot (the human ledger `FUN_00972ED0` follows only for a type-0 human). Kills come through the kill report `FUN_00986230`
   (evaluators `Result_OnEnemyKilled_A/B`), the other kinds from the enemy code (`FUN_00603EA0`, `FUN_00986560`, ...).
2. A record check (a tiny "wrapper" per achievement, `0x409160`..`0x409610`) compares the global total of the kind with the record's goal and calls
   `Ach_SetRecordState(record, 2)` (`0x408F00`), which sets the record bit in the save and queues the Steam id (`Ach_RecordToSteam` -> `Ach_QueueSteamUnlock`).
3. Once per frame the queue is posted to the game's net object, which calls `ISteamUserStats::SetAchievement`. The game never uses stats or `StoreStats`.
4. The other 24 achievements do not count anything: episode clears, survival / invisible clears and challenge medals read the local save at the end of the episode
   (`Ach_OnEpisodeClear`), two read story flags.

## What goes wrong in a mirrored session, and what the mod does

Each PC runs the whole campaign (the story machine and the engine) on its own copy; the world is synchronised (enemies, items, flags, scenes). The partner's character is a
type-2 puppet in the other player slot. Without care that breaks three things:

| Problem | What the mod does |
|---|---|
| The global total is bumped for **any** slot, so the partner's puppet (his kill replayed on the host, a replica dying by the host's blow on the client) moves this profile towards an achievement. The earlier guard rolled the tables back only after the record check had run, so at goal-1 the partner's kill still fired the record. | **The gate**: the bump pair is intercepted. When the slot is not a local human (`player+0x7920 != 0`, the game's own definition of "a human of this PC"), the global total is put back and the per-slot bump is skipped, so the check that follows sees an unchanged profile. Solo play and sessions with the gate off are untouched. |
| Causes that only the **host** can see: the host's real enemy reacts to both humans (back attack, freeing a held partner, Revenant arm, Afflicted jump attack), while the client's replicas have no AI, so the client's PC would never credit its own human. | **Forwarding** (`REV2_ACHFWD=1` / `net ach fwd 1` on the **host**; off until verified live): the host sends the bump it just undid for the client's puppet (`EV_ACH_BUMP`, 8 bytes), the client replays the game's own bump and record check for its human, **once** (a dedupe window drops it when the client's own code bumped the same kind a moment ago). A client never forwards to the host. The client's default (`auto`) follows the host; `net ach fwd 0` on a client refuses. The light stun (kind 0x30) is not forwarded: the game bumps it only for a type-0 human. |
| The game credits ASSIST_HOLD_HELP only to the slot in seat 0 (slot 0 on every PC of the mod). | **Assist call patch** (P6, installed with forwarding or `REV2_ACHASSIST=1`): the call that names the seat-0 slot answers the slot of the enemy's last attacker when he is one of the two humans; the game's own check and bump follow. |
| Records that the enemy code sets **directly** for the human it involves (`ACH_GRAP_COMBINATION`). | **Context guard**: while the enemy's target is the partner's puppet the record set is swallowed on this PC; with forwarding on the host sends it to the client (`EV_ACH_REC`), which sets it if its human is Claire. Installed for the general enemy function `0x6067B0`; the Afflicted path (`uEm1100Intel_vf62`) is guarded only with `REV2_ACHEM1100=1` until it has been entered live. |

Nothing is credited that the human did not do: the gate only removes, and the forwarded event is the game's own bump for the human that caused it. Leaving a session loses nothing
(the credit was queued to Steam when it happened); a late join starts from the PC's own save and replays no history; the gate is stateless.

## The table (all 43 mapped achievements)

PLAYER = credited only to the PC of the human who did it. SHARED = story or save state: every PC reaches it through its own copy of the campaign. RISKY = depends on state the sync
does not mirror exactly. N/A = cannot be earned in the story campaign. Index = Steam name table index, record = the game's record id, kind = the stat kind whose global total the
record compares with its goal. `ACH_ALL_ACHIEVEMENT_COMP` (index 0) is derived (record 1 when 0x76 of the records 2..0x7F are done in the local save) and follows the others.

| Index | Achievement | Record | Rule | Kind | What the game checks | In co-op |
|---|---|---|---|---|---|---|
| 1 | ACH_ALL_EP1_CLEAR | 0x02 | SHARED | - | `Ach_OnEpisodeClear` (0x409230, from the episode-clear handler 0x431680): campaign type 0 and the episode is marked cleared in the local save | Every PC runs its own episode end and writes its own save, so every account unlocks it. Needs the same campaign type on both PCs. **Credited at the end of the SECOND half** (Barry / Natalia, part 1: the check reads the part-1 clear table); the end of Claire's half writes the other table and fires nothing. Shown on the pair in stage 6h: `RECORD 0x02` + `WOULD UNLOCK` on both PCs within 10 ms (the story's own `StageClear` order fired from the console in stage 153, everything after it is the game's code) |
| 2 | ACH_SURVIVAL_ALL_EP1_CLEAR | 0x12 | SHARED | - | Same handler, campaign type 2 (Survival, `0x409250`) | As the clear: the PC reaches it by itself. The campaign type (`gm+0x5C`) must be the same on both PCs |
| 3 | ACH_INVISIBLE_ALL_EP1_CLEAR | 0x17 | SHARED | - | Same handler, campaign type 1 (Invisible, `0x409240`) | As the clear: the PC reaches it by itself. The campaign type (`gm+0x5C`) must be the same on both PCs |
| 4 | ACH_CHALLENGE_MEDAL_EP1_COMP | 0x35 | SHARED | - | All challenge medals of the episode in the local save (`0x409300` / `0x409350` after the result screen) | Derived from the PC's own per-player counters, which the gate keeps per player; no extra work |
| 5 | ACH_CAMPAINF_PT_MEDIUM | 0x3A | SHARED | - | Story points `story+0xE8C` against the goal, evaluated at the chapter end (`0x4091D0`) | Each PC adds its own points from its own profile; shared story progress, no cross-credit |
| 6 | ACH_FLAME_NOT_USED | 0x49 | SHARED | - | Area 0x66 model event (`0x4092A0` from `uBioModel::LayoutID_vf19`) and the save flag "flame used" is clear | Local event and local flag on every PC; credited to each PC whose own flag is clear |
| 7 | ACH_PROGRESSOR_KILL | 0x4F | PLAYER | 0x0F | Kill report: class kills of the Afflicted (Progressor) family, global total of kind 0x0F against the goal (`0x409400`) | The kill report runs on the PC of the killer. The partner's kill on this PC is undone by the gate (the total never moves, so the record cannot fire at goal-1) |
| 8 | ACH_HOMUNCULUS_KILL | 0x54 | PLAYER | 0x14 | Kill report: class kills of kind 0x14 (`0x4092E0`) | As PROGRESSOR_KILL |
| 9 | ACH_KNIFE_WEAPON_KILL | 0x64 | PLAYER | 0x26 | Kill report evaluator A: the killer holds the knife (weapon 0x14A/0x14B), kind 0x26 (`0x4092F0`) | As PROGRESSOR_KILL. Proven live: the client's knife kill moves only the client's total (6d), the gate leaves the host at goal-1 |
| 10 | ACH_BLOW_ATK_KILL | 0x65 | PLAYER | 0x28 | Evaluator A: the killer's action is a melee blow (0x2B/0x2D/0x2E), kind 0x28 (`0x409190`) | As PROGRESSOR_KILL. The action id is read when the kill report runs, so a high latency can miss a very short blow |
| 11 | ACH_GRAP_BLOW_KILL | 0x66 | PLAYER | 0x2A | Evaluator A: the killer's action is the grapple finisher (0x2F), kind 0x2A (`0x4092C0`) | As BLOW_ATK_KILL |
| 12 | ACH_BACK_ATK_KILL | 0x67 | PLAYER | 0x2B | Enemy code (`FUN_00606680`, called from `uEnemy_vf50`) bumps kind 0x2B for the player it involves, then `0x409170` | Only the host's real enemy sees the cause. The host undoes the bump for the client's puppet and forwards it to the client (`net ach fwd`), which replays the game's own bump and check for its human, once |
| 13 | ACH_LIGHT_ATK | 0x6A | PLAYER | 0x30 | `FUN_00986560` (called from the enemies' stunned actions) bumps kind 0x30 for the player it is given, **only if that player is a type-0 human**, then `0x409340` | The host never bumps it for the partner's puppet, so there is nothing to forward (not forwarded since P6). A client's Moira is credited only if her own PC's replica runs the stunned action with her as the player: not verified |
| 14 | ACH_BAR_WEAPON_KILL | 0x6C | PLAYER | 0x33 | Evaluator A: weapon ids 0x14D-0x14F, kind 0x33 (`0x409180`) | As PROGRESSOR_KILL |
| 15 | ACH_STONE_KILL | 0x6E | PLAYER | 0x35 | Evaluator A: weapon 0x101/0x702 (thrown stone), kind 0x35 (`0x409610`) | As PROGRESSOR_KILL |
| 16 | ACH_KNIFE_CLEAR | 0x70 | PLAYER | - | Episode 1: both parts' knife-only flags (records 0x96/0x97, `FUN_0097DA30(1|2)`) at the clear | The flag is set by the local human's own weapon use; the partner's shots on this PC do not run weapon code. Shown at the real end of Episode 1 on the pair (stage 6h): with Barry (one PC) firing his pistol and Natalia (the other PC) using no weapon, the Natalia PC logged `RECORD 0x70` + `WOULD UNLOCK`, the Barry PC did not (it logged only `RECORD 0x02`); with nobody firing both PCs logged it. The card needs both halves cleared in one save (record 0x96 of part 0 and 0x97 of part 1) |
| 17 | ACH_SHOT_TWO_KILL | 0x72 | PLAYER | - | `FUN_006E8310`: two enemies of one entry of slot 0's shot ledger (`sPlayer+0x2124`) killed by slot 0; the function returns early only for stage ids >= 1000 other than 9000 (`Area_IsEpisodeArea` 0x887910: the campaign stages 100..154 are below), so it counts in the campaign (Steam text: "in the main campaign"); slot 1 has record 0x73, which has no Steam twin | The ledger is filled by the shooter's own weapon code, so only the shooter's PC can fire it; on a client it also needs the replicas to carry the "killed by slot 0" flag after the host's DEATH: not verified |
| 18 | ACH_HOMU_WEAPON_DEST | 0x78 | PLAYER | 0x38 | Enemy intel of the Revenant (`uEm0100Intel_vf62`) bumps kind 0x38 for **the enemy's current target** (`FUN_00603EA0(0x38, 0)`), then `0x4092D0` | As BACK_ATK_KILL. The stock rule credits the human the Revenant is after, not necessarily the one who shot the arm off |
| 19 | ACH_PROG_JUMPATK_STOP | 0x79 | PLAYER | 0x39 | Enemy intel of the Afflicted (`uEm1100Intel_vf62/63`) bumps kind 0x39, then `0x4093F0` | As BACK_ATK_KILL |
| 20 | ACH_GRAP_COMBINATION | 0x7B | RISKY | - | Record 0x7B set directly by the host's enemy code (`FUN_006067B0`, `uEm1100Intel_vf62` via `FUN_00603F20`) when the enemy's player (`+0x38D0`) is Claire (character 0x16) | No stat kind to forward. While the enemy's target is the partner's puppet the record set is suppressed on this PC (context guard), so the host is never credited for the client's combo. With forwarding on (P6) the host sends the swallowed record (`EV_ACH_REC`) and a client whose human is Claire sets it through the game's own `Ach_SetRecordState`. Not run live |
| 21 | ACH_ASSIST_HOLD_HELP | 0x7D | PLAYER | 0x37 | `uEmIntel_vf22`: an enemy that holds a player gets a hit reaction; kind 0x37 is bumped for the player in **seat 0** (`GameMan_FindSlotBySeat(0)`, call 0x6018DB) if he is the enemy's last attacker (`+0x3940` = the DamageInfo's attacker), then `0x409160` | The stock join gives slot 0 (Claire / Barry) seat 0 on every PC, so Moira / Natalia could never earn it. The mod's call patch (with forwarding, P6) answers the last attacker's slot instead: the host's human is credited on the host, the client's human through the forward. Not run live |
| 22 | ACH_BOTTLE_FIVE_KILL | 0x7E | PLAYER | - | `FUN_006E8310` (as SHOT_TWO_KILL): five kills of one entry of slot 0's bottle ledger (`sPlayer+0x2DA4`); counts in the campaign | As SHOT_TWO_KILL: the thrower's PC only |
| 23 | ACH_SYS_RAID_MISSION_CLEAR | 0xB4 | N/A | - | Raid result code | Raid only; the mod is off in Raid and never touches it |
| 24 | ACH_SYS_RAID_USE_JUDGMENT | 0xB5 | N/A | - | Raid code | Raid only |
| 25 | ACH_SYS_RAID_DUNGEON_CLEAR | 0xB6 | N/A | - | Raid code | Raid only |
| 26 | ACH_ALL_EP2_CLEAR | 0x03 | SHARED | - | `Ach_OnEpisodeClear` (0x409230, from the episode-clear handler 0x431680): campaign type 0 and the episode is marked cleared in the local save | Every PC runs its own episode end and writes its own save, so every account unlocks it. Needs the same campaign type on both PCs. Not played to the end on the pair yet |
| 27 | ACH_SURVIVAL_ALL_EP2_CLEAR | 0x13 | SHARED | - | Same handler, campaign type 2 (Survival, `0x409250`) | As the clear: the PC reaches it by itself. The campaign type (`gm+0x5C`) must be the same on both PCs |
| 28 | ACH_INVISIBLE_ALL_EP2_CLEAR | 0x18 | SHARED | - | Same handler, campaign type 1 (Invisible, `0x409240`) | As the clear: the PC reaches it by itself. The campaign type (`gm+0x5C`) must be the same on both PCs |
| 29 | ACH_CHALLENGE_MEDAL_EP2_COMP | 0x36 | SHARED | - | All challenge medals of the episode in the local save (`0x409300` / `0x409350` after the result screen) | Derived from the PC's own per-player counters, which the gate keeps per player; no extra work |
| 30 | ACH_PEDRO_PAST_KILL | 0x4A | PLAYER | - | A special enemy kill of the past timeline; the setter was not found (variable record id) | Assumed to come from the kill report: per player through the gate. Unverified |
| 31 | ACH_GHOST_KILL | 0x57 | PLAYER | 0x18 | Kill report: class kills of kind 0x18 (`0x4092B0`) | As PROGRESSOR_KILL |
| 32 | ACH_ALL_EP3_CLEAR | 0x04 | SHARED | - | `Ach_OnEpisodeClear` (0x409230, from the episode-clear handler 0x431680): campaign type 0 and the episode is marked cleared in the local save | Every PC runs its own episode end and writes its own save, so every account unlocks it. Needs the same campaign type on both PCs. Not played to the end on the pair yet |
| 33 | ACH_SURVIVAL_ALL_EP3_CLEAR | 0x14 | SHARED | - | Same handler, campaign type 2 (Survival, `0x409250`) | As the clear: the PC reaches it by itself. The campaign type (`gm+0x5C`) must be the same on both PCs |
| 34 | ACH_INVISIBLE_ALL_EP3_CLEAR | 0x19 | SHARED | - | Same handler, campaign type 1 (Invisible, `0x409240`) | As the clear: the PC reaches it by itself. The campaign type (`gm+0x5C`) must be the same on both PCs |
| 35 | ACH_CHALLENGE_MEDAL_EP3_COMP | 0x37 | SHARED | - | All challenge medals of the episode in the local save (`0x409300` / `0x409350` after the result screen) | Derived from the PC's own per-player counters, which the gate keeps per player; no extra work |
| 36 | ACH_MOIRA_NEIL_KILLED | 0x4B | PLAYER | - | The Neil boss kill (episode 3); the setter was not found | Assumed to come from the kill report: per player through the gate. Unverified |
| 37 | ACH_FLOOD_GATE_ITEM_GET | 0x4C | PLAYER | - | Key item window `uGUIKeyItem_vf15` (`0x409290`): episode 3 part 1, area 0x15F, one of the key items 0x1E-0x20 | The window runs on the PC of the human who picks the item up; the other PC receives the item through the AddItem replay without the window, so only the picker's account unlocks |
| 38 | ACH_ALL_EP4_CLEAR | 0x05 | SHARED | - | `Ach_OnEpisodeClear` (0x409230, from the episode-clear handler 0x431680): campaign type 0 and the episode is marked cleared in the local save | Every PC runs its own episode end and writes its own save, so every account unlocks it. Needs the same campaign type on both PCs. Not played to the end on the pair yet |
| 39 | ACH_SURVIVAL_ALL_EP4_CLEAR | 0x15 | SHARED | - | Same handler, campaign type 2 (Survival, `0x409250`) | As the clear: the PC reaches it by itself. The campaign type (`gm+0x5C`) must be the same on both PCs |
| 40 | ACH_INVISIBLE_ALL_EP4_CLEAR | 0x1A | SHARED | - | Same handler, campaign type 1 (Invisible, `0x409240`) | As the clear: the PC reaches it by itself. The campaign type (`gm+0x5C`) must be the same on both PCs |
| 41 | ACH_CHALLENGE_MEDAL_EP4_COMP | 0x38 | SHARED | - | All challenge medals of the episode in the local save (`0x409300` / `0x409350` after the result screen) | Derived from the PC's own per-player counters, which the gate keeps per player; no extra work |
| 42 | ACH_PAST_GHOST_KILL | 0x4D | PLAYER | - | Episode 4 part 0: per-part ghost kill count (`FUN_00975AE0(0, 0x18)`) against the goal (`0x403D00`) | As PROGRESSOR_KILL (the count is bumped by the same kill report) |
| 43 | ACH_FLOOD_GATE_USED | 0x4E | SHARED | - | Episode 4 part 1: story flags 6 and 7 set, polled every frame by `sBioAchievement` (`0x404B30`) | The story flags are synced (FLAG events), so both PCs reach it and both accounts unlock |

Summary: 20 PLAYER (11 through the kill report, 5 through enemy-side code (4 of them forwarded), 2 through the shot / bottle ledger, knife-only clear, key item), 19 SHARED, 1 RISKY, 3 N/A (Raid); ACH_ALL_ACHIEVEMENT_COMP derived. Episode 1 in co-op, per achievement, host and client, with the hardware checks: `notes\ach-ep1-matrix.md` (private).

## What is proven, what is only written (two real instances, host on the main PC, client on the N100, Episode 1 Barry half, Steam writers blocked)

| Check | Result |
|---|---|
| Both PCs block the Steam writers; an unlock is logged `SetAchievement("...") BLOCKED: WOULD UNLOCK` and listed by `net ach status` | proven |
| The partner's stat bump at goal-1 (kind 0x28, both directions) is undone: the total stays at 49, no record, no unlock; the local human's bump on the same PC reaches 50 and fires the record: **exactly one `WOULD UNLOCK ACH_BLOW_ATK_KILL` per PC, each for its own human** | proven (the bump is the game's own `FUN_009815E0` plus the real record check; the cause is a console call) |
| A real kill: Barry (client) kills a crow with the knife through the HIT / DEATH path with the total at goal-1 on both PCs: the client's total reaches 100, its ledger 1, the record fires (`WOULD UNLOCK ACH_KNIFE_WEAPON_KILL`); the host stays at 99 and fires nothing | proven |
| The real kill report (`FUN_00986230`) run on the host with the client's puppet as killer: the crow's class kinds 0x1A and 0x47 are undone, global and human sums unchanged | proven |
| Forwarding: the host undoes a kind-0x2B bump for the client's puppet and sends it; the client replays the game's bump and record check once, the record fires on the client only (`WOULD UNLOCK ACH_BACK_ATK_KILL`); the client's own enemy-side bump for the host's puppet is undone and not forwarded; dedupe drops a forward that arrives within the window and when the client's own code bumped the same kind | proven with console-made causes |
| The same events with the gate off or on a PC without a partner | the game's own behaviour (unit tested; not run live) |
| Hook trampolines: stat bump, per-slot bump and record state (used constantly), the general enemy function `0x6067B0` (entered on the host with crows) | run live without a crash |
| The end of Episode 1 on the pair (stage 6h): both halves' end-of-stage order `StageClear` (`0x876960`, the last node of the radio FSMs of stages 102 / 152) fired on both PCs at the same moment, the game's own result flow (`0x431AF0` -> `Game_EpisodeClearHandler` 0x431680 -> `Ach_OnEpisodeClear` 0x409230) ran on both: part 0 writes only table B (no record), part 1 (after the game's own continuation Claire -> Barry) writes table A and `ach: RECORD 0x02 ACH_ALL_EP1_CLEAR` (SHARED) and `RECORD 0x70 ACH_KNIFE_CLEAR` (PLAYER) appear on both PCs with the matching `WOULD UNLOCK`, 0 writer calls reached Steam | proven on hardware; the order was fired from the console, the cutscene and zones that start it in the stock story (stage 102 radio, stage 152 radio) were not walked |
| A real knife kill by the CLIENT's human with the product settings (replica AI off; 0.2.0 MHIT): the blow reaches the host's enemy through the host's own hit handler, the host's DEATH names the client's slot, the client's replayed kill report sees the knife blow (F6) and credits kinds 0x26 / 0x0F on the client only; the host's own report for the puppet is undone by the gate | proven (two N100 copies, run M2 2026-11-08) |
| Aimed fire, physical attacks, stealth kills, freeing a held partner, bricks by the client | not shown: the lab input aid cannot aim the camera or make those moves; they go through the same paths (MHIT for the blow, forwarding for the enemy-side cause) and are on the acceptance list (`notes\ach-ep1-matrix.md` section 9) |
| Real enemy-side causes (a back attack, a light attack, freeing a held partner ...) on the pair, the Afflicted hook, the Survival / Invisible types, Episodes 2-4 ends | **not run**: needs real gameplay; see the table notes |

## Test mode: no real unlock from a test

The tester's Steam accounts must not receive unlocks from teleport or cheat runs. The Steam wrapper (`steam.c`) therefore has a block switch:

| Setting | Effect |
|---|---|
| `REV2_ACHBLOCK=1` | `SetAchievement`, `ClearAchievement`, `SetStat`, `UpdateAvgRateStat`, `StoreStats`, `IndicateAchievementProgress` and `ResetAllStats` are logged and **not** passed to Steam. They return success, so the game's own bookkeeping goes on as after a real unlock. Every blocked unlock is logged as `WOULD UNLOCK <name>` and listed by `net ach status`. |
| default | Blocking is **on whenever the test console is enabled** (`REV2_CONSOLE` set; every lab script sets it, a player's start never does). A normal start passes everything through. |
| `REV2_ACHBLOCK=0` | The manual switch: a console run that is allowed to reach Steam (to verify one real unlock by hand). The commands that can earn a record (`net ach bump|set|rec`) refuse to run unless the writers are blocked. |
| `REV2_PASS=1` | Pass-through mode installs no Steam hooks at all: not a test mode for achievements. |

Verifying a real unlock by hand: start the game with `REV2_ACHBLOCK=0` (with the console if wanted), play a cause that really exists in the game (no test aids: they refuse to run), and
watch `rev2_coop.log`: `ach: RECORD 0x.. achieved` followed by `ach: SetAchievement("ACH_...") -> 1` and, a moment later, the Steam toast. `net ach status` lists the unlocks that were passed
to Steam as `UNLOCKED`. The achievement is stored by the Steam client when it flushes.

## Console (`net ach ...`, game thread)

`status` (hooks, gate counters, assist patch and record-forward counters, records the game achieved, blocked/real unlocks), `totals` (global total, human ledger, goal, record state per kind), `table`, `gate 0|1`,
`fwd 0|1|auto` (the host's value decides; a client in `auto` follows), `assist 0|1` (1 installs the assist call patch), `dedupe <ms>`, `ctx 0|1`, `log <n>`, and, only while the Steam writers are blocked: `bump <kind> <slot> [n]` (the game's bump for that slot plus the
kind's record check, through the gate), `set <kind> <value>`, `rec <record>`.

## Offline tests

`tests/test_ach.c` checks the table against the game's record map, the gate decision, the dedupe window, the wire formats, this document's table and a model of two PCs; since P6 also how
each kind's reporter picks the credited player (a forwarded kind must be one the host can bump for the puppet), the assist decision on both role assignments with and without the patch,
the record forward of 0x7B, the host-decides forwarding switch and the composition of the gate with world.inc's stat guard.
