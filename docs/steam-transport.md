# Steam transport for Revelations 2 co-op

Status: **built and shown on a real Steam session between the two test PCs** (2026-11-03): `native/coop_steam.c` backend + commands, `native/coop_steam_real.c` real platform, hooks in `steam.c`.
Offline tests: a mock Steam universe (`tests/test_steam.c`, 158 checks) and a fake game-side Steam with the game's real C++ calling conventions (`tests/test_steam_real32.c`, 97 checks, two layout variants).
Hardware: N100 (account Boris6) hosts, main PC (Alisa77) joins; lobby, join by SteamID, invite, "Join game" request, both pipes, the state stream of the campaign and the load barrier all work (section "Hardware results").
Not yet shown: a click in the real friends list / overlay (a `steam://joinlobby/...` URL stood in), PCs on different networks (both test PCs share a LAN), `InitRelayNetworkAccess` / `GetRelayNetworkStatus` (they killed the game once and are off by default).
The direct-IP backend (`enet`) is unchanged and remains the fallback.

## Goal

A player hosts, a friend clicks "Join game" (or the host invites the friend, or the friend types the host's SteamID): no IP address, no port forwarding, NAT handled by Valve's
relay network. The session layer above (`coop_session.c`) is untouched: it sees the same `cn_ops` as for ENet (channel 0 reliable ordered, channel 1 unreliable, small integer peers,
CONNECT / DISCONNECT / RECEIVE events, RTT).

## Facts about the game's own Steam use (observed, build 3658995)

- The game runs under its **real app id 287290** with its own 2015-era `steam_api.dll` (interfaces `SteamClient017`, `SteamUser019`, `SteamFriends015`, `SteamMatchMaking009`,
  `SteamNetworking005`, `STEAMHTTP002`, ...). Its `CSteamAPIContext` (returned by `SteamInternal_ContextInit`) holds: [0] ISteamClient, [1] User, [2] Friends, [3] Utils, [4] Matchmaking,
  [5] UserStats, [6] Apps, [7] MatchmakingServers, [8] Networking (the code reads `ctx+0x20`). We use exactly these pointers, so the version of every interface is the game's own.
- **`SteamNetworking005` has the channel argument** (the old note "no channel" was wrong): `SendP2PPacket(id, data, len, type, channel)`, `IsP2PPacketAvailable(&size, channel)`,
  `ReadP2PPacket(dest, size, &len, &id, channel)`, `CloseP2PChannelWithUser(id, channel)`. The game polls **two channels only**: **905 (0x389, "SteamQosReq/Ans" ping loop)**, code
  `0xBF6338` (`IsP2PPacketAvailable`) / `0xBF6389` (`ReadP2PPacket`) / `0xBF6408` (`SendP2PPacket`) / `0xBF37E1` and `0xBF6536` (`CloseP2PChannelWithUser`), and **0** (game data), code
  `0xC0B998` / `0xC0B9E9`. The stage 2b run saw exactly these two polling call sites (`exe+0x7F633A`, `exe+0x80B99A`) and nothing else. Both loops exist only once the Raid menu is entered
  (its session object registers 505 / 506 / 1202 / 1203 / 143 / 304 then, not at start-up; the boot-time registrations are 704, 331, 1005, 1101-1103, 304, 505 and **333**, from a friends-handling object).
- Raid online is dead (RE.NET servers gone), so none of this runs in the campaign. The Raid lobby uses the keys `OwnerId, Name, IsBehindNAT, SlotPublic*, SlotPrivate*, SearchKeyNum,
  SearchKey0..6, BinarySize, BinaryData` and searches with `SearchKey0 == 1`, `SearchKey2 == 31`, `SlotPublicOpen >= 1`.
- The game's callback 333 (`GameLobbyJoinRequested_t`) handler stores the lobby id and calls `RequestLobbyData` on it (handler `0xB696B0`); its 505 handler continues from there.

## Design as built

### Two layers of Steam access

| Layer | What | Why |
|---|---|---|
| lobby, friends, rich presence, call results | the game's own ISteamMatchmaking009 / ISteamFriends015 / ISteamUser019 / ISteamUtils through the context pointers (MSVC thiscall vtable calls, `coop_steam_real.c`) | no second Steam API, same interface versions as the game |
| message pipe between the lobby members | **`msg`**: `ISteamNetworkingMessages002` (`ISteamClient::GetISteamGenericInterface`, Valve's relay network, its own queues, our channel number 21042; the game never calls that interface), reliable / unreliable per message; **`p2p`** fallback: the game's `SteamNetworking005` on our own channel 21042 (never 905 or 0), `CloseP2PChannelWithUser` for our channel only | the Messages interface is current Steam client code reached through an old `ISteamClient017` pointer: shown to work on the test PCs (see Hardware results); if it is missing or its vtable is unusable the backend takes `p2p` by itself. `net steampipe msg|p2p`, `REV2_STEAM_PIPE` choose |

The generic interfaces are fetched at the first real use (host / join / search), not while the game idles.

### Why the game cannot see our traffic

- `msg`: other interface, other queues; no callback of the game is involved. `p2p`: our packets are on channel 21042 and the game reads 905 and 0 only (static proof above; the fake
  test traps any call of ours on 0 or 905). Our packets are never read by the game, and we never read its packets.
- Callbacks: we register our own callback objects with the game's `SteamAPI_RegisterCallback` (the DLL calls every registered object). The game's handlers keep running for everything,
  except callbacks that concern **our** lobby: `steam.c` asks `cst_swallow_game_callback` before running a game handler for 333 (join request / invite), 505 and 506 (lobby data / member
  changes): ours -> not delivered to the game (else its handler would `RequestLobbyData` on our lobby and feed its Raid state machine). A lobby counts as ours when it is the lobby this
  process hosts / joined, or carries our tag, or is unknown with no Raid key visible. A lobby with Raid keys, or any other id, reaches the game unchanged. `REV2_STEAM_SWALLOW=0` turns the
  filter off. Nothing else of the game's Steam calls is altered.

### Lobby and rendezvous (our own keys, never a Raid key)

| Key | Value |
|---|---|
| `r2c` | `R2COOP1` (the private tag; the last digit is this backend's wire version) |
| `r2c_host` | host SteamID64 (decimal) |
| `r2c_name` | host persona name (ASCII) |
| `r2c_max` | player limit (2-4) |
| `r2c_phase` | `lobby` / `game` (informational) |
| `r2c_build` | `rev2-coop` |

Lobby type: friends only by default (joinable by Steam friends and invitees, **not listed in searches**), `public` (listed, found by `r2c`/`r2c_host` string filters), `private` (invite only).
Host rich presence: `connect = +connect_lobby <lobby id>` (the friends list shows "Join game"), `status`, `steam_player_group` = lobby id, `steam_player_group_size`. Cleared when the host leaves.

- **Join targets** (`net steamjoin ...` / `cs_join(..., "steam", target)`): `lobby:<id>` or a bare lobby id (the type bits of a SteamID64 tell lobby from user), `+connect_lobby <id>`,
  `steam:<steamid64>` or a bare user SteamID64 (the host). A user id is resolved by the friend's game info (`GetFriendGamePlayed`: app 287290 and a lobby), else by a lobby search with
  `r2c = R2COOP1` and `r2c_host = <id>` (public lobbies), for 9 s. The tag is checked after the join (a lobby without it is left again).
- **Who the host talks to** (`net steamallow`): friends (default), any lobby member, or an allow list only. Others in the lobby are ignored (logged once) and never get a peer id.
  A pipe session request is accepted only from a lobby member the policy allows.
- **Searches** (`net steamlobbies`): public lobbies with our tag + friends who are in the game (with their lobby). Raid's searches cannot see our lobby (none of their keys), ours cannot see Raid's.

### Link protocol on the pipe (all messages start with a type byte)

| Byte | Message | Notes |
|---|---|---|
| `H` / `W` | hello / welcome: `version(1) lobby id(u64) is_host flags` | both sides send `H` (reliable, every 500 ms) until they heard the other; the answer to `H` is `W`; the first valid `H`/`W` from an expected member links the peer (CONNECT). The lobby id must match. |
| `D` | data: `channel byte` + the session's packet | reliable / unreliable as the caller asked; unlinked senders are dropped |
| `P` / `Q` | ping / pong with the sender's millisecond clock | every second: RTT and keep-alive |
| `B` | bye | graceful leave; the peer is parked (not re-greeted) until it says `H` again |

Down: `B`, the member gone from the lobby for 3 s (host) / the lobby owner changed or gone for 3 s (client), the pipe session state `failed` for 5 s, nothing heard for 30 s. The session layer's join
deadline is 30 s for `steam` (10 s for ENet). Peers on the host are numbered 1..n in lobby order; a returning member keeps its slot while the old record exists.

### Threading

`service()` runs on the transport thread (every millisecond or two) like the ENet backend and is the only place that talks to the pipe; a global lock serialises it with the console commands and with
`cst_game_tick()`, which `steam.c` calls on the game thread right before `SteamAPI_RunCallbacks` (it registers our callback objects once, moves events, starts an invited join; it never blocks: `TryEnterCriticalSection`).
Call results (lobby created / entered / list) are polled with `ISteamUtils::IsAPICallCompleted` / `GetAPICallResult`, so they work while the game thread is busy loading.
Broadcast callbacks (Join game, invites, pipe session requests) need the game thread's `SteamAPI_RunCallbacks`: during a long level load they wait.

### Console

```
net steam                       state: ready?, your SteamID, pipe, relay network status, lobby, pending join request, last error
net steamhost [friends|public|private] [2-4]     create the lobby and host (default friends only, 2 players); prints your SteamID and the lobby id
net steamjoin <steamid64 | lobby id | lobby:<id> | pending>      join the host of that SteamID / that lobby / the last "Join game" request
net steamlobbies                public lobbies with our tag, friends in the game and their lobbies
net steaminvite [steamid | name part]   without argument: Steam's invite dialog (overlay); with one: InviteUserToLobby
net steamallow any|friends|list|<steamid64>      who the host links with
net steampipe [msg|p2p]         show / choose the message pipe (before hosting or joining)
net steamtrace [0|1]           log the pipe messages (hello / welcome / ping / bye, the first 20 data messages)
net host 0 steam / net join <target> steam      the generic forms
```

Environment: `REV2_STEAM=0` (no listening, no hooks of ours act), `REV2_STEAM_AUTOJOIN=0` (a Join game request only sets `pending`), `REV2_STEAM_SWALLOW=0`, `REV2_STEAM_PIPE=msg|p2p`, `REV2_STEAM_TRACE=1` (same as `net steamtrace 1`: log every pipe message but the data ones), `REV2_STEAM_RELAYINIT=1` / `REV2_STEAM_DIAG=1` (the two relay-network calls that crashed once, see Hardware results).
"Join game" while the game runs: Steam sends callback 337 (`GameRichPresenceJoinRequested_t`, connect string) or 333 (lobby); the request starts `join lobby:<id> steam` on the game thread unless this
process already hosts or joined something. When the game is not running, Steam starts it with `+connect_lobby <id>` on the command line; that lobby becomes the pending request at the first tick.

## What is tested where

| Test | What it proves | What it cannot |
|---|---|---|
| `tests/test_steam.c` + `steam_mock.c` | backend logic: tag / keys, presence, join by id / host id (friend game info, public search), policy, full / private / foreign / Raid-like lobbies, session requests that must be accepted, bye / owner leave / killed process / left the lobby / broken link / silent link, re-join, the whole session over the backend under 15 % loss and 25 % loss + dup + reorder, join requests and auto-join, the callback filter, all commands | that Steam behaves like the mock |
| `tests/test_steam_real32.c` + `steam_fake32.c` | `coop_steam_real.c` against fake interfaces with the game's calling conventions (this ecx, callee pops, hidden CSteamID result, 64-bit call handles in edx:eax), slot numbers, struct offsets (sender identity at +12 or +16, release function after 3 x 8 + 4 bytes), call results, callback objects in both vtable orders, both pipes, our channel only, bye / lobby leave / presence clear | that the Steam client agrees with the SDK layouts the code assumes (the hardware session below showed it does for everything used except the two relay-network calls) |

## Hardware results (2026-11-03, N100 host Boris6 `76561198000000162`, main PC client Alisa77 `76561199000000001`, same LAN, game at the title screen / in Episode 1)

| Question | Result |
|---|---|
| `GetISteamGenericInterface("SteamNetworkingMessages002" / "SteamNetworkingUtils004")` through the old `ISteamClient017` | **works**: returns small adapter objects (8 bytes) whose vtable thunks forward slot i to slot i of the real implementation (`mov ecx,[ecx+4]; jmp [vtbl+4i]`); the Messages vtable has exactly 6 entries (the public layout), Utils004 follows the public header (slot 7 takes a float: `CheckPingDataUpToDate`). The code logs and checks the vtable before using it |
| Create lobby (friends only, 2 players) | 1.7 s from the console command to the lobby id; lobby data and rich presence accepted |
| Join by the host's SteamID | `GetFriendGamePlayed` of the host gave app 287290 **and** his lobby id at once (the `connect` presence works); JoinLobby 0.32 s; link on the message pipe 1.9 s later: **2.2-2.3 s from command to linked**, 3.4 s counted by a console poll once a second; re-join after `net stop` the same |
| Pipe `msg` (NetworkingMessages002) | works; sender identity at offset 12 on both PCs (probe), messages released through `m_pfnRelease`; ping 5-8 ms |
| Pipe `p2p` (SteamNetworking005, our channel 21042) | works; 4.0 s from command to linked (3.7 s after entering the lobby); ping 7-8 ms |
| Session over the backend | HELLO / WELCOME / lobby / `auto` ready / START / barrier: all as with ENet; two campaign instances (Claire host on the N100, Moira client on the main PC) walked for a minute: **host received 1004 states, client 1380, gap 0, dup 0, old 0**, rtt 6-8 ms; `net simload` from either side moved the barrier epoch 1 -> 2 -> 3 and play continued; `net say` events arrive both ways |
| Leave / re-join | `net stop` on the client: bye received, host's peer dropped at once, lobby member count back to 1, parked peer forgotten 3.4 s later when Steam showed him leaving the lobby; client re-join works |
| Invite | `InviteUserToLobby(host's lobby, friend)` returns true (by SteamID and by name part) |
| "Join game" request | `steam://joinlobby/287290/<lobby>/<host>` (what the friends list uses for a lobby) run on the main PC while the game ran: Steam delivered **callback 333** to the game; our filter kept it from the game's handler ("callback 333 about our lobby kept"), our callback object received it, the auto-join started `join lobby:<id> steam` and linked in 2.1 s. 337 (connect string) is handled by the same code but was not seen |
| `net steamlobbies` | lists friends in the game with their lobby (`(ours)` by tag) and public lobbies with our tag (none) |
| `net steaminvite` without argument | `ActivateGameOverlayInviteDialog` returns; whether the overlay window opens was not looked at |
| `InitRelayNetworkAccess` (Utils004 slot 1) / `GetRelayNetworkStatus` (slot 2) | the first two tries ended in an access violation (return address on the stack) right after `ensure_pipe`; one of the two calls is wrong for this client (the thunks look right: slot 2 pops one argument). Both are now opt-in (`REV2_STEAM_RELAYINIT=1`, `REV2_STEAM_DIAG=1`) and not used; the relay network starts by itself on first use (the link above was made without them) |
| Saves | no save was written by the runs (main PC `DATA0.DAT` unchanged from 17:46; only Steam's `remotecache.vdf` and the usual `localconfig.vdf` / `gamerecording.pb` changed) |

## Open

1. Two PCs on different networks (relay vs direct path); ICE/relay behaviour under real loss.
2. A click on "Join Game" in the real friends list and the overlay invite dialog (the URL stands in for the click; 337 is untested).
3. Why `InitRelayNetworkAccess` / `GetRelayNetworkStatus` crash (log lines before and after each with the env switches; or a disassembly of the inner functions behind the adapter thunks in `steamclient.dll`).
4. A level load longer than a few seconds with real callbacks pending (accepts and Join requests wait for the game thread).
5. How Steam treats `AutoRestartBrokenSession` after our own bye + lobby leave (no ill effect seen).
