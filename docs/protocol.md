# Revelations 2 co-op: wire protocol, version 2 (stage 5)

Source of truth: `native/coop_proto.h` / `coop_proto.c` (encode and decode, shared by the proxy, the headless bot and the tests), session rules in
`native/coop_session.c`, transport in `native/coop_net.c`. Tests: `tests/test_proto.c` (byte vectors, fuzz), `tests/test_session.c` (sim transport,
loss / duplication / reordering), `tests/run_net_tests.py` (real UDP through a netem relay). Keep this file in step with those.

## 1. Conventions

- Little endian; floats are IEEE-754 binary32; a string is `u8 length` + that many bytes (UTF-8, at most 255, control characters are replaced by `?`
  on receipt, names are cut to 24 bytes, texts to 120).
- One packet = one transport message; the first byte is the message type. There is no framing of our own (the transport keeps message
  boundaries). Every message of this protocol is at most 512 bytes (the largest, `WELCOME` / `START` with four players, is about 250; STATE is 60).
- **Extensibility**: a decoder reads the fields it knows and ignores trailing bytes, so a newer sender may append fields to a message without
  changing the version. Changing or removing a field, or changing its meaning, needs a new `CP_PROTO_VERSION`. An unknown message type is dropped
  (and counted); a malformed one (short, bad magic, bad count) is dropped and counted in `bad`, it never ends the session.
- Two channels: **0 control** = reliable + ordered; **1 state** = unreliable + sequenced (the newest message wins; a late older one is dropped by ENet,
  and by our own sequence check on any transport that does not do it).
- Players have a `pid` 0..3 (the host is 0); characters are `0 claire, 1 moira, 2 barry, 3 natalia, 255 none`. **The character is the player slot**:
  `slot = character & 1` (Claire/Barry = slot 0 = pad 0, Moira/Natalia = slot 1 = pad 1). The session's `part` (0 = Claire/Moira half,
  1 = Barry/Natalia half) says which pair is on stage; two players cannot hold the same slot.

## 2. Topology, roles, phases

A star: the host (pid 0) is the hub, clients talk to the host only; the host relays `STATE` and `EVENT` between clients (two players: no relaying).
Phases (`enum cp_phase`): `LOBBY` (before the start, characters can change) -> `LOADING` (barrier closed: somebody is loading a level) -> `PLAYING`
(barrier open, states flow) -> `LOADING` again at every level load. The host owns the session id, the settings, the lobby table and the barrier.
Heartbeats and ENet's own pings keep the link alive; the transport (not the session) declares a peer dead (ENet: 30 s without any
acknowledgement), because the game thread may be busy in a level load for much longer than any application timeout.

## 3. Handshake and the version gate

```
client                                host
  | ---- (transport connect) ---------> |
  | -- HELLO{magic,version,min,exe,..} ->|   host: lo=max(min,PROTO_MIN) hi=min(version,PROTO_VERSION); lo>hi -> REFUSE(VERSION)
  |                                      |          exe tags both non-zero and different -> REFUSE(BUILD)
  |                                      |          mod versions both non-empty and different, neither a bot -> REFUSE(MODVER) (0.2.0)
  |                                      |          a name removed from this session -> REFUSE(REMOVED) (0.2.0)
  |                                      |          game started and (product, 0.2.0) anybody / (lab rejoin) not a known name -> REFUSE(STARTED); no free seat -> REFUSE(FULL)
  | <-- WELCOME{version,pid,session,lobby,phase,epoch,mod}                       otherwise: seat, character, then
  | <-- LOBBY_STATE (to the others)       |
  | client: WELCOME.mod != its own (an older host sends none) and neither is a bot -> LEAVE, the player sees REFUSED(MODVER)
```

- `HELLO` carries `magic 'R2CP'`, the sender's `version` and the oldest version it still speaks (`min_version`), the first 8 bytes of the game
  exe's SHA-256 (`exe_tag`; zeros = unknown, the bot) and the mod build string. The negotiated version is `min(both versions)`; today `CP_PROTO_MIN` = `CP_PROTO_VERSION` = 2 (version 1 had a 40-byte STATE: a version 1 build is refused with `REFUSE(VERSION)`).
- A host that has not received a `HELLO` 5 s after the transport connected drops the peer; a client that has no `WELCOME`/`REFUSE` after 10 s gives up.
- `REFUSE` is followed by a graceful disconnect from the host. Anything but `HELLO` before the welcome is dropped.
- **No join after the start (0.2.0 product rule, `cs_config.no_rejoin`, the glue sets it unless `REV2_REJOIN=1`)**: every `HELLO` after `START` is refused with
  `STARTED` ("joining a running game is not possible in this version"), also a known name, and no seat is taken over from a silent link (a stranger never touches a
  running game). A client with the rule that is welcomed into a running game anyway (a lab host) sends `LEAVE` at once and reports `REFUSED(STARTED)`.
- **Re-join (lab, `REV2_REJOIN=1`)**: after the start, a `HELLO` whose name equals that of a disconnected player gets the same pid and character back; the host then opens a new
  load epoch and the barrier waits for the returning player. Strangers are refused with `STARTED`. If the transport has not yet noticed that the old link is dead
  (a crash and a quick restart), a `HELLO` with the name of a seat that has sent nothing for 3 s drops the old link first (`PEER_LEAVE` b = 2), in the lobby too.
- **Package version (0.2.0)**: both PCs need the same release version of the mod (`HELLO.mod` / `WELCOME.mod` = `R2_VERSION`). The host refuses another one with
  `REFUSE(MODVER)` whose English text names both versions (what an older client shows); `REFUSE` and `WELCOME` carry the host's version as an optional tail
  (decoded as "" when missing, see 1), so a newer client shows its own text in the player's language and leaves an older host that does not check. The headless bot
  (`flags` BOT in `HELLO`, and in `WELCOME` when the host is one) and an empty version are not checked. The protocol version itself did not change for this.

## 4. Messages

Offsets are within the message, type byte = offset 0.

| Type | Name | Channel | Direction | Layout (after the type byte) |
|---|---|---|---|---|
| 0x01 | HELLO | 0 R | c -> h | `u32 magic` `u16 version` `u16 min_version` `u8[8] exe_tag` `u8 want_char` `u8 flags` `str name` `str mod` |
| 0x02 | WELCOME | 0 R | h -> c | `u16 version` `u8 pid` `u8 flags` (BOT = the host is a bot) `u32 host_ms` `u8 phase` `u8 epoch` `session` `lobby` [`str mod`: the host's mod version, 0.2.0] |
| 0x03 | REFUSE | 0 R | h -> c | `u8 code` `str text` (1 version, 2 build, 3 full, 4 started, 5 bad, 6 modver, 7 removed: 0.2.0) [`str mod`: the host's mod version, 0.2.0] |
| 0x04 | SESSION | 0 R | h -> all | `session` (the host changed the settings) |
| 0x05 | LOBBY_PICK | 0 R | c -> h | `u8 character` |
| 0x06 | LOBBY_READY | 0 R | c -> h | `u8 ready` |
| 0x07 | LOBBY_STATE | 0 R | h -> all | `lobby` (the whole table; clients ignore a revision not newer than theirs) |
| 0x08 | START | 0 R | h -> all | `session` `lobby` (characters are final) |
| 0x09 | NOTICE | 0 R | h -> c | `u8 code` `str text` (1 slot taken, 2 not ready, 3 rejected, 4 info) |
| 0x10 | LOAD_BEGIN | 0 R | any -> all | `u8 pid` `u8 epoch` `u32 area_id` |
| 0x11 | LOAD_DONE | 0 R | c -> h | `u8 pid` `u8 epoch` |
| 0x12 | GO | 0 R | h -> all | `u8 epoch` |
| 0x20 | EVENT | 0 R | any -> all | `u8 pid` `u16 kind` `u16 eseq` `u32 t_ms` `u8 epoch` `u8 len` `u8[len] body` (len <= 96) |
| 0x30 | HEARTBEAT | 1 U | any -> any | `u8 pid` `u32 t_ms` `u32 echo_t_ms` `u32 echo_dwell_ms` `u8 phase` `u8 epoch` `u16 state_seq` |
| 0x3F | LEAVE | 0 R | any -> any | `u8 reason` (0.2.0: 0 quit, 2 removed by the host, 3 the host ended co-op, 4 the end of the co-op episode; older peers send 0) |
| 0x50 | STATE | 1 U | any -> all | 60 bytes, see 4.1 |
| 0x51 | WORLD | 1 U | any -> all | stage 6, additive (no version change: an older peer drops an unknown type): `u8 pid` `u8 kind` `u16 seq` `u32 t_ms` `u8 epoch` `u16 len` `u8[len] body` (len <= 400); newer sequence only, other epochs dropped, relayed by the host. Bodies and the world events (kinds `0x120..0x12A`: SPAWN HIT DEATH PICKUP PDMG FLAG SCENE GRANT BARRIER; WORLD kinds 1 enemy records, 2 flag table digest, 3 item liveness digest): `world.md` |

Blocks: **session** = `u32 id` `u8 game_type` `u8 episode` `u8 part` `u8 difficulty` `u8 max_players` `u8 flags` `u32 area_id` `u32 seed` (16 bytes).
**lobby** = `u32 rev` `u8 count` then per player `u8 pid` `u8 character` `u8 ready` `u8 flags` `str name`; flags: 1 host, 2 connected, 4 in game, 8 loaded, 16 bot, 32 / 64 a co-op save of Claire's / Barry's half (0.2.1).

### 4.1 PLAYER_STATE (0x50), 20 Hz, 60 bytes (version 2; version 1 was the first 40)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u8 | type 0x50 | |
| 1 | u8 | pid | the sender; the host drops a state whose pid is not the sender's |
| 2 | u16 | seq | per sender, wraps; receivers keep only newer ones (`seq16_newer`) and count gaps / duplicates / old |
| 4 | u32 | t_ms | sender's monotonic milliseconds (machines' clocks differ; the receiver estimates the offset, see 6) |
| 8 | f32 x3 | pos | world position (player object `+0x40`) |
| 20 | i16 | yaw | radians * 10000, -pi..pi (from the quaternion `+0x50`: `2*atan2(qy, qw)`) |
| 22 | u16 | action | the game's action id (`+0x1A54`); `0x80000000` ("waiting") is sent as `0xFFFF`, other ids as their low 16 bits |
| 24 | i16 | hp | `+0x1A08` |
| 26 | i16 | hpmax | `+0x1A0C` |
| 28 | i16 | aim | degrees * 100 (`+0x7950`) |
| 30 | i16 x3 | aimv | aim vector * 16384 (`+0x7960..`) |
| 36 | u8 | weapon | current weapon id (stub: 0 until the equipment ids are mapped) |
| 37 | u8 | epoch | level instance counter; a receiver drops a state whose epoch is not its own |
| 38 | u16 | flags | 1 aiming, 2 firing, 4 reloading, 8 crouch, 16 melee, 32 flashlight, 64 dead, 128 down, 256 loading, 512 cutscene, 1024 **teleport** (do not interpolate to this sample), 2048 paused (the sender's pause menu is open). Set today by the glue: aiming (ctrl bit 2), dead (hp <= 0), cutscene (view mode 2/3), paused (sBioMain+0x28626), teleport; the others are reserved until the matching engine fields are found (see stage5.md) |
| 40 | u32 | pstate | the game's player state flags (`+0x37B8`), the Raid net code replays the same word on its remote players |
| 44 | u32 | ctrl | the game's player control state flags (`+0x78E8`: bit 2 = aiming) |
| 48 | u16 x3 | item | item ids in the weapon slots 1..3 (0 = empty): inventory entry `+4` of the entry `FUN_006FB270(slot type)` names in the sender's personal inventory |
| 54 | u8 | move | locomotion intensity 0..255 (`+0x792C`; the walk/run blend the engine's remote-movement helper takes) |
| 55 | u8 | - | reserved 0 |
| 56 | i16 | elev | aim elevation, degrees * 100 (`+0x7954`) |
| 58 | u16 | - | reserved 0 |

`weapon` (offset 36) now means the weapon slot in the hand (`FUN_006FB060`: 0 none, 1..3 weapon slots, 4 special). A decoder ignores trailing bytes, so later versions may append fields; any change of a field above needs a new version.

States are sent only in `PLAYING`, at most one per 45 ms (a teleport sample goes at once). 1200 bytes/s per direction before ENet's 10-byte-per-message framing.

### 4.2 Events (0x20), stubs

The envelope, relay through the host, ordering (reliable channel) and the `epoch` tag exist; the game does not raise or consume events yet except `net event` / `net say`
from the console. Planned kinds and bodies (all little endian; ids are the game's own object ids / class hashes):

| Kind | Name | Body |
|---|---|---|
| 1 | FIRE | `u8 weapon` `i16 ammo` `f32 x3 origin` `i16 x3 dir*16384` |
| 2 | HIT | `u8 target_kind` `u32 target_id` `i16 damage` `u8 weapon` `f32 x3 hit pos` (damage dealt to a named enemy; host-authoritative enemies) |
| 3 | DEATH | `u8 who_kind` `u32 who_id` `u8 killer_pid` (kill credit goes to the killer's own PC, see achievements) |
| 4 | PICKUP | `u32 item_id` `i16 count` |
| 5 | DOOR | `u32 door_id` `u8 state` |
| 6 | INTERACT | `u32 object_id` `u8 action` |
| 7 | ITEMUSE | `u8 item` |
| 8 | CHECKPOINT | `u32 id` |
| 9 | CUTSCENE | `u32 id` `u8 op` (0 start, 1 end, 2 skip vote) |
| 10 | STAT | `u16 stat_id` `i32 delta` (per-player statistics for achievements) |
| 11 | TEXT | UTF-8 text (console `net say`) |
| 0x100+ | USER | free for tests. In use: `0x120..0x12F` world events (`world.md`; `0x12C` SF: `i32 area` `u16 flag index` `u8 value`, a scenario flag was set / cleared on the sender, stage 6e, on in a session (`net w sf sync 1|2`); stage 10: index 0..511, `area` 99 = an EpisodeFlag (global flag 99000 + index); `0x12E` PROP (stage 10): `i32 a` `u16 b` `u16 c` `u8 flags` `u8 slot` `u16 seq` `f32 pos[3]` `f32 quat[4]` `i32 action` = the transform of a movable set model the sender's human pushes or carries; `0x12F` SF digest (stage 10): `i32 area` `u8 kind` `u8 words` `u32 words[16]` = every flag set on the sender at the end of its quiet window after a load; `0x129` DOOR got an optional tail for co-op set models (`f32 pos[3]` `u8 kind`, 31 bytes): an older peer reads the first 18 bytes, ignores the unknown kinds 0x12E / 0x12F and drops the SF events of area 99 as another area (no version change); `0x12D` BGL, stage 6j: `u8 count` (0..5), then `count` x (`i16 episode` `u16 stage` `u16 block`) = the sender's OWN block streamer list, sent when it changes and every 3 s while a session runs; the receiver merges it into the free slots of its list so that its streamer keeps the sender's blocks, `net w bgl mirror 0|1`), `0x130` HITDI (0.2.0 MHIT, client -> host, reliable, 96 bytes): `i32 a` `u16 b` `u16 c` `u8 slot` `u8 part` `i32 damage` `u32 flags[3]` `i16 rel[3]` (hit point minus the enemy position, 0.1 units) `f32 dir[3]` `u32 +0x110` `u16 +0x114` `u32 +0xE0` `u8 +0xE4` `u8 +0xE5` `u32 +0x264` `u16 +0xA0` `u16 +0x80` `f32 +0xAC[6]` `f32 +0xA8` `u32 +0x98` = a real blow of the sender's human on a replica (the DamageInfo fields of the game's own Raid compact pair FUN_00ADDCD0 / FUN_00ADC650); the host runs its enemy's hit handler with it (`world.md`, MHIT); an older host ignores the unknown kind, `0x131` INV (combat4, reliable, any -> other): `u8 owner slot` `u8 flags` (1 first event of a full set: the table is cleared, 2 last event: complete) `u8 seq` `u8 n` then n x (`u8 index` `u16 item id` `i32 count` `u32 +0x18`) = entries of the sender's OWN human's personal inventory (changes; a full set on request), the receiver reconciles its copy of that slot, `0x132` INVREQ (`u8 sender slot`): ask the owner for a full set (every 2 s until complete, events of another epoch are dropped), `0x133` INVADJ (`u8 owner slot` `u8 n` then n x (`u16 item id` `i32 delta` `u32 +0x18`)): the sender's inventory screen changed the owner's items (the hand-over), the owner applies the delta to its real items, `0x134` THROW (`u8 thrower slot` `u8 throw number` (bomb, 1..255; 0 from an older peer) `u16 item id` `f32 vectors[4][3]`): the sender's human launched a projectile (the vectors of the stock launch), the receiver creates the same object for the thrower's puppet; older peers ignore all four, `0x135` BOOM (bomb, reliable, thrower -> other, 16 bytes): `u8 thrower slot` `u8 throw number` `u16 item id` `f32 pos[3]` = the sender's bomb of that throw explodes there (hook on the Raid sender FUN_009F6EF0); the receiver moves its copy of that throw there and starts the copy's explosion (action 8, as the Raid receiver FUN_009E07E0 does), for the eye only: the copy's blows are dropped on the host and never MHIT on a client; an older peer ignores it, `0x150` ACH_BUMP (stage 8b, host -> client, reliable): `u8 kind` `u8 n` `u16 seq` `u32 tick`: "your human caused stat bump `kind` x`n` where only my enemies could see it" (`achievements.md`); the client replays the game's own bump and record check for its human unless its own code bumped the same kind within the dedupe window (1.5 s). `0x151..0x15F` reserved. |

Downed / revive events (stage 9, `downed-revive.md`, reliable, only while the session flag `CP_SESS_DOWNED` = bit 0 of `session.flags` is set; bits 1..2 of the same byte are the bleed-out preset): kinds `0x160` DOWN, `0x161` DEAD, `0x162` BEGIN,
`0x163` KEEPALIVE, `0x164` CANCEL, `0x165` DONE (`CP_EV_USER + 0x60..0x65`); body `u8 sender slot` `u8 target slot (255 = none)`. `STATE` flag bit 128 (`CP_SF_DOWN`, reserved since version 2) is set while the sender lies. An older peer drops the unknown kinds
and ignores the flag: no version change.

Co-op save (0.2.1, `native/coopsave_logic.h`, `lobby.md` "Continue"): `session.flags` bit 3 (`CP_SESS_CONT` = 8): the host continues the checkpoint of its co-op save.
The lobby table's player `flags` 32 (`CP_PF_SAVE0`) / 64 (`CP_PF_SAVE1`): that player's co-op save holds an Episode 1 checkpoint of Claire's / Barry's half (a client
sends them in `HELLO.flags`, the host keeps them in its table). Kind `0x180` CPREC (`CP_EV_USER + 0x80`, host -> client, reliable, sent just before `START` of a
Continue, in order): `HEAD` `u8 0` `u8 version 1` `u16 pairs` `u32 crc` `u32 difficulty` `u32 episode` `u32 +0x2C` `u32 stage` `u32 start position` `u32 scene`
(32 bytes); then `WORDS` `u8 1` `u8 n (<= 15)` `u16 first pair number` + n x (`u16 word index` `u32 value`), only the words that are not 0 of the flag block
(cGameInfo+0x123740, 0x59C4 bytes = 5745 words) followed by the 16 EpisodeFlags words; then `END` `u8 2` `u8 version` `u16 pairs` `u32 crc`. The crc (CRC-32) covers the
six fields, the flag block and the EpisodeFlags; a record with a lost, reordered or damaged event is not complete and is never applied (the guest then continues from
its own co-op checkpoint, its log says so). Older peers drop the unknown kind; both PCs need 0.2.1 anyway (the package version gate).

## 5. Lobby, start, barrier

**Lobby** (phase LOBBY): the host starts with its own `net char` (default Claire; Barry if `part` is 1); a joining client asks for its character in `HELLO` and gets
it if the slot is free, else the other slot of the pair (default: Moira). `LOBBY_PICK` is honoured when no *other connected* player holds the same slot, otherwise
the host answers `NOTICE(slot taken)` and the table does not change; a change clears the player's ready flag. `LOBBY_READY` sets the flag. The host (auto-ready) calls `start`
when everybody is ready (or forces it); it sends `START{session, lobby}`, sets epoch 1, phase LOADING. Characters and session settings can no longer change.

**Barrier** (every level load, including the first): each machine reports when its own game starts a load (`LOAD_BEGIN{pid, epoch+1, area}`; the host
adopts it and relays it) and when it is playable again (`LOAD_DONE{epoch}`). The host opens the barrier (`GO{epoch}`, phase PLAYING) when every connected player
reported loaded for the current epoch, or after 300 s (logged; stage 6i: it was 120 s, shorter than the slowest test PC's story load of ~150 s, and the host ran the opening scene alone). A machine that is playable while the barrier is still closed tells its player: after 20 s, then every 30 s, the session logs `still waiting for <name> (player N) to finish loading: X s so far` and raises the local notification `CS_N_WAIT` (a = seconds waited, b = the limit in seconds on the host, 0 on a client, pid / text = the awaited player); the game shows it as a strip across the top of the screen in the game's language (`LT_WAIT_LOAD`) until the barrier opens. Rules: a `LOAD_BEGIN` with `epoch+1` starts a new level instance for everybody (rings of remote
states are cleared); with the current epoch it just marks that player as loading; a machine whose game is not loading answers another player's `LOAD_BEGIN` by
reporting itself loaded at once. States of another epoch are dropped (counted as `stale`). While LOADING nobody sends states.
The game glue uses the barrier to flip the puppet slot to AI (type 1) for the length of a local load (a type-2 slot stalls the level load, stage 3 trap)
and back to type 2 at `GO`.

**Leave reasons (0.2.0)**: the host's `LEAVE` reaches a client as `CS_N_LOST` a = 1, b = the reason; a client's `LEAVE` with reason 4 reaches the host as `PEER_LEAVE`
b = 4. **Remove partner / End Co-Op**: `cs_kick(pid, reason)` (host): `LEAVE{2 | 3}` to that one client, a graceful disconnect of it, `PEER_LEAVE` b = 3 on the host at
once; reason 2 (in the room) puts the name on the session's removed list (`REFUSE(REMOVED)` until the session ends). A client whose join timed out without an answer
reports `REFUSED` a = `CS_REF_NOANSWER` (100, local only); a backend's own failure (Steam) keeps a = 0 and its text.

**Leave**: `LEAVE{reason}` then a graceful disconnect. Before the start a leaving player's seat is freed; after it the seat is kept (`connected` cleared) so the same
name can return. A lost host ends the session on the clients (`CS_N_LOST`); the glue turns the puppet into the game's AI partner and the game goes on single player.

## 6. Time, interpolation, liveness

- `HEARTBEAT` every 500 ms to every peer on channel 1: `t_ms` (sender clock), the last heartbeat time received from the peer and how long ago it arrived
  (`echo_*`): RTT = `now - echo_t_ms - echo_dwell_ms` (smoothed 0.8/0.2). Receive times are taken on the **network thread** when the packet arrives, not when the game
  thread gets to it.
- **Silence** (0.2.0): heartbeats and states are sent from the game thread, and the game runs its logic only while its window is active (`stage2a.md` 1), so a player in
  Alt-Tab, a stalled PC (the owner's PC stalls up to ~90 s) or a hung game sends nothing while its transport stays up (ENet on its own thread, Steam pings). The host
  watches every client, a client its host: no message of any kind for 8 s raises `CS_N_SILENT` (a = seconds, b = the limit; then every 10 s, a = 0 when heard again,
  b = how long it lasted) and a log line; after **10 minutes** the session gives up on it: the host drops that link and keeps the seat (`PEER_LEAVE` a = 1, b = 1; the player
  may join again), a client closes its session (`LOST` a = 2 "no answer from the host for 600 s"). So no wait of the session (also not a client's barrier) lasts for ever.
  A real crash ends the link through the transport much sooner. A client told by `LEAVE` that the host left reports `LOST` a = 1 "the host left the session".
- Clock offset per remote player: `off = min over time of (local_rx - sender_t_ms)` (minimum filter, creeping up 0.1 ms/s for drift; computed relative to the first
  sample so machines with days of difference in uptime work). The shown time is `local_now - off - interp_delay` (default 100 ms, `REV2_NET_INTERP`).
- `cs_remote_sample` returns the state at that time: position and aim vector linear, yaw and aim angle along the short arc, discrete fields (action, hp, weapon, flags)
  from the earlier sample. Teleport samples (flag, or a jump over 500 units) are not blended. After the newest sample it extrapolates with the last velocity for at most
  250 ms, then holds, and reports `STALE` after 1 s of silence.
- Measured on two PCs (stage 5, two game instances, injected keys, `net drift` + `tools/drift.py`, error of the shown puppet against the owner's own position at the same sender time): LAN 0.00 mean / 0.08 max units, netem 50 ms each way + 10 ms jitter (RTT 120 ms)
  0.00 mean / 0.05 max, no gap, duplicate or old state. The shown puppet is `interp_delay` + one-way latency behind the owner's live position.
- Measured (`tests/run_net_tests.py`, 20 Hz, circle path 200 units/s): clean link 0.04 mean / 0.07 max error; 5 % loss + reordering + duplicates (40 ms +- 15): 0.06 / 0.26;
  15 % bursty loss: 0.25 / up to 19; 20 % loss + 80+-40 ms + reorder + dup: 0.2-2 mean, up to 60 in an outage. Real LAN (two PCs, game instances): RTT 6-8 ms,
  7300 states in 6 minutes without a gap, duplicate or reordered packet.

## 7. Transport rules

- ENet 1.3.18, UDP, default port **28410** (`net host [port]`, `net join <ip[:port]>`; the bot uses 28411), two channels, MTU-sized messages only. The unreliable throttle is switched
  off (the state stream is 1 KB/s, thinning it out because the link "looks bad" only makes the puppets jerky). The host listens on all interfaces: **the host's firewall must
  allow inbound UDP on that port** (a client only needs outbound UDP; the installer will add a rule, stage 10).
- A transport backend is a table of functions (`cn_ops` in `coop_net.h`); `enet`, `sim` (in-process, deterministic faults, used by the unit tests) and a placeholder `steam` exist.
  Peers are small integers: the host sees its clients as 1..3, a client sees the host as 0. The backend must deliver CONNECT / DISCONNECT / RECEIVE and notice dead peers itself.
  The Steam design is in `steam-transport.md`.
- Untrusted input: every field is range-checked on decode; a client can only speak for its own pid (`STATE`, `EVENT`); a flood of unreliable messages while the game thread is
  busy is bounded (the inbox keeps the newest 256 unreliable messages).
- While this game's loop is away (a level load, a lost D3D device, a pause or a stall) the network thread keeps receiving; the unreliable messages beyond the newest 256 are
  dropped here. They are counted as `expired` (per player and epoch for STATE), never as `gap`: `gap` is loss on the way only. `net status` shows
  `... gap=N stale=N expired=N` per player and `; expired here N (M states), loop away K x, longest X s` on the transport line, and the log says once per stall of >= 3 s
  "this game's loop did not run for X s ...". (Steam run M3, 2026-11-08: the guest's device was lost for 60-75 s and `gap=1384` was all expiry; notes/steamgap.md.)

## 8. Test vectors

STATE (`pid 1, seq 0x0102, t 0x1000, pos 1 2 -1, yaw 1, action 3, hp 100/200, aim 45, aimv 0 0 1, weapon 7, epoch 2, flags 0x0101, pstate 0x11223344, ctrl 4, item 0x14D / 0 / 0x201, move 255, elev -500`):

```
50 01 0201 00100000 0000803f 00000040 000080bf 1027 0300 6400 c800 9411 000000000040 07 02 0101 44332211 04000000 4d01 0000 0102 ff00 0cfe 0000
```

HELLO (`version 2, min 2, no exe tag, want Moira, flags 0, name "Bob", mod "0.1"`):

```
01 52324350 0200 0200 0000000000000000 01 00 03426f62 03302e31
```

## 9. Mapping to the game (the glue, `coop_game.inc`)

Local character = type-0 slot of this PC, shown full screen (`vp 0 <slot>`, `view 0`, `viewguard`), driven only by this PC's keyboard / mouse / pad (nothing received from the network ever writes to it); remote character = type-2 slot moved every game pass with the game's own
remote teleport (`FUN_00710fd0`: target fields `+0x39A0..` and yaw `+0x39C4`), aim written to `+0x7950` / `+0x7960`, the action switched with `ChangeAction` (vtable `+0xB8`) for ids 2 and 3
only, the owner's health written to `+0x1A08/0C`. The puppet is held while the owner's state says cutscene, loading or dead. A state with the DEAD flag makes the receiving PC's character die too (game over for both, as in the one-PC split screen); a LOAD_BEGIN from the other PC
while this PC's character is dead confirms this PC's death screen (retry). Traps (type-2 slots stall level loads, pad spoof upkeep, the game drops a second human whose pad vanishes) are in the comments of that file; the engine's other remote-movement helpers and why they are not used: `notes/stage5.md` (private).
