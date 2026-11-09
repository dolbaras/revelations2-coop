# Co-op menu and lobby (Resident Evil Revelations 2 co-op mod)

How a player starts a co-op game from the game's own menus, and how that is built. Game build: Steam 3658995 (`rerev2.exe` SHA-256 `55ac7d2b...67f9`); on any other exe the mod does nothing but forward DirectInput.

## What the player sees

1. Title screen > **Campaign** > (main campaign) the campaign page now has a row **Co-Op Play** after "Episode Select". The caption is the game's own string, so it is translated in all 12 game languages.
2. Selecting it opens the **co-op screen** (drawn by the mod over the finished frame, dark panel, same language as the game):
   - **Find a Partner** (host): *Friend* = a Steam lobby (friends only; the friend sees "Join game" on your Steam profile, or `Invite Friends` in the room opens Steam's invite dialog) or *System Link* = direct connection by IP (UDP port 28410, shows your LAN address).
   - **Join a Partner**: *Friend* = list of friends who host a co-op game right now (and public lobbies of the mod); *System Link* = type the host's address (`192.168.1.5` or `192.168.1.5:28410`, Ctrl+V pastes).
   - A friend's **Join game** click (or a Steam invitation) while you sit in the menus opens this screen by itself, already in the room.
3. **The room**: the four characters (Claire, Moira, Barry, Natalia). Each player picks who to play; the slot the partner has taken is grey. Claire/Moira are the first half of an episode, Barry/Natalia the second half: the host's pick decides the half, and the partner's character follows to the same slot of that half. The host also sets **Difficulty** (Casual / Normal / Hard / Very Hard) with Left/Right; **Episode** is fixed to 1 in 0.2.0 (the room says "Co-op play covers Episode 1 in this version"; a guest does not get ready for, and does not start, a session of another episode). The partner presses **Ready**; the host presses **Start Game** when everybody is ready. Both PCs then start the episode through the game's normal "new game" path and the mod's session layer takes over (see `protocol.md`).
   - Status lines (0.2.0): the guest sees "Connecting to <address>...", then "Awaiting reply from <address>...", in the room "Choose your character, then mark yourself ready." and after Ready "Waiting for <host> to start the game..."; the host sees "Waiting for a partner to join...", "Waiting for <partner> to get ready...", "<partner> is ready. You can start the game.", and when the partner leaves the room the game's own "<name> has left the session." / "<name> has disconnected.".
   - The System Link host reads under the player table how a friend reaches him over the internet (public address, UDP port forwarded to this PC, the game allowed in Windows Firewall).
   - **Remove partner** (host, before the start) asks the game's own "Do you want to kick <name>?" (cursor on No); the removed player reads the game's "You have been removed from the session by the host." and his name is refused until the host's session ends.
   - Refusals name their reason in the player's language after the game's "Failed to join session.": session full, game already started (joining a running game is not possible in this version), no answer from the host (check the address and that the host is in the room), another mod / protocol version, another game version (the game's own words), removed by the host.
4. **Leave** (or Esc) in the room asks the game's own "Quit session?" question when a partner is present; the note under it says that leaving ends the session for both. A guest whose host closes the room reads the game's "Session has been concluded.".
5. In the game, the **pause menu** has a **Leave** row while a net session runs (the game's local split-screen rows are hidden then, they would break the session). It asks "Quit session?" with the game's popup; confirming is the game's normal quit to the title, and the mod ends the session when the title screen is back. The host also has **End Co-Op** (0.2.0): the game's "End the session you created?" question; Yes tells the partner and the host plays on alone with the game's AI partner.
6. **When a player is gone after the start (0.2.0)**: the host reads a strip "<name> has left the game. The game continues with the AI partner." (or "Connection to <name> was lost. ..."); the partner's character becomes the game's AI partner at once, and in plain play about 1.5 s later the game is single player again through its own leave of the split screen, so the stock character swap (Tab) works; the session is closed (nobody joins a running game in this version). The guest whose host left, ended co-op or was lost reads "The host has ended the session. Returning to the title screen..." (or "Connection to the host was lost. ..."), and the game goes back to the title by itself as soon as it is in plain play (the same call the pause menu's Quit makes; a cutscene is waited out for at most 60 s).
7. **The end of Episode 1 (0.2.0)**: when either PC reaches the episode's result area after Barry's half, its session ends; the game's own results, records, achievements and rewards run on each PC, and the game's episode-end menu offers no "next episode" while this is so: both players leave it with the game's own return to the title. While that menu is on screen a strip says why ("Episode 1 is complete. Co-op play ends here in this version: the session is closed."); the area load and the ending scene stay clean. A PC that gets the partner's end while it still plays on gets the strip after 3 s of plain play.

Keys: Up/Down choose, Left/Right change a value, Enter / Space / F confirm, Esc / Backspace go back; in the address field type, Backspace edits, Ctrl+V pastes, Enter connects. Pad: D-pad or left stick, A or Start confirm, B back.

**Save slot warning**: the co-op game uses the normal campaign save slot of the Steam account; its checkpoints overwrite the current campaign save, exactly as "New Game" would (the room says so).

## Settings file

`%LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS2\rev2_coop.ini` (the game's own config folder; never the Steam install). Written by the mod whenever you change something, `key=value` lines:

| Key | Meaning |
|---|---|
| `name` | player name shown to the partner (default: the Steam persona name, asked once when the screen opens; without Steam "Player 1" when hosting / "Player 2" when joining, shown as the game's word for "Player"; never the Windows user or computer name) |
| `character` | `claire` / `moira` / `barry` / `natalia`: wanted when hosting or joining |
| `ip`, `port` | last direct address, host port (default 28410) |
| `transport` | `steam` or `direct`: the way used last |
| `episode`, `difficulty` | host defaults |
| `enabled` | `0` removes the Co-Op row and all hooks |
| `autoopen` | `1` (default): a Steam invitation opens the screen; test runs with the console switch this off |
| `hide_menu`, `input_gate` | `hide_menu=0` (default; `1` calls the common menu's hide toggle, which on the tested build leaves the stock list invisible afterwards), `input_gate=1` (default): set the pad system's "ignore pad" byte while the screen is open |
| `solo` | `1`: the host may start without a partner (development) |

`REV2_COOP_INI=<file>` uses another file; `REV2_MENU=0` disables the feature for one run. Lab only: `REV2_EPISODES=all` (or `menu set episodes=all`) gives the host's Episode row 1-4 back; `REV2_REJOIN=1` lets a known player rejoin a running game (the stage-4 rule).

## How it is built (for modders)

Everything is native hooks plus the game's own page tables; no game file is replaced.

| Part | What |
|---|---|
| Row | The campaign page (page 2) of the common-menu page tables (`.rdata` 0x13947C8, row id 0x14 at 0x1393DDC) is a dormant "online campaign" row: its label (table 3 message 90) is an empty message and `cDemoCampaignMenu_Build` hides it with one `SetItemState(0x14, 2)` call (0x44687D). The mod writes the label of message 120 ("Co-Op Play") and the help text of message 126, and redirects that one call to a stub that shows the row for the main campaign. |
| Screen | Selecting row 0x14 already sets the menu id 0x18, the game's dormant `cDemoSessionSelectMenu` (an 8 byte object whose Update only pokes the Raid session manager). Its vtable slots [5] Update and [0] destructor are replaced: Update runs the lobby model and the input, the destructor tells the lobby that the screen is gone. The game's own list stays where it is under the opaque overlay; when the screen closes the selected id of the common menu is reset (0x68) and `CommonMenu_Resume` re-arms it, so a confirm the hidden list saw cannot reach the campaign menu. |
| Drawing | `overlay.c`. Default backend: the screen is composed on the CPU with GDI (the fonts of the system, per language: Segoe UI, Yu Gothic UI, Malgun Gothic, Microsoft YaHei UI, Microsoft JhengHei UI) into a bitmap that is copied into the back buffer with `UpdateSurface` + `StretchRect` from the Present hook: no `BeginScene`, no sprite, no render state is touched, nothing is drawn while the screen is closed. The composition is repeated only when the scene changes. The 1920x1080 virtual layout is scaled to the back buffer and centred. (A D3DX sprite/font backend exists, `menu ov 2`; on the first hardware run it left the game's render thread stalled after the first frame, so it is not used.) |
| Model | `lobby.c`: screens, rules, session calls through the console grammar of the glue (`host`, `join`, `char`, `ready`, `start`, `session`, `stop`, `steamhost`, `steamjoin`, `steamlobbies`, `steaminvite`, `name`). No Windows or game code; `tests/test_lobby.c` drives two lobbies against each other on the in-process transport (791 checks). |
| Texts | `lobby_text.c`: the game's strings are read through `Msg_GetString` (message tables 0, 3, 6, 14, 51, 52, 58) in the language the game runs in; the mod's own strings (about 25) are translated into the other 11 languages. A missing table falls back to English. |
| Start | the stock new-game sequence without its menus: `GameMan_SetGameType(0)`, `Story_SetEpisode`, `Story_SetDifficulty`, `story[0x15C4A8] = part`, `Story_SetContinueFlag(0)`, `story[0x15C485] = 0`, `Menu_GoToEpisodeStart` (menu id 0xE..0x15). The session layer's glue (`coop_game.inc`) then sets the two characters up as in stage 4/5. |
| Pause | `cPauseTopProcess_SetRowVisibility` (0x8B9A60) is wrapped: while a net game runs rows 0x36 / 0x37 (split screen) are hidden and row 0x3B ("Leave", label from the game's cockpit table) is shown. `cPauseTopProcess_OnSelect` (0x8B85D0) is wrapped: for row 0x3B the popup text of the stock quit question is swapped to message 116 "Quit session?" by wrapping `Popup_SetMessage` (0x436CE0) for that one call. The stock quit path does the rest. |
| Leaving | `menu_frame` ends the session when a running net game and the title menus are on screen together for 1.5 s. |
| End Co-Op (0.2.0) | Row 0x3D of the pause page (a Raid-only "Give Up" row, 0x1394388) gets our label / help ids while the host of a running net game has a partner (`h_pause_vis`), the stock row otherwise. `Msg_GetString` (0x954B40) is wrapped for two private ids of table 6 (0x2100 / 0x2101: `LT_END_COOP` / `LT_END_COOP_HELP` in the game's language; every character is a glyph of that language's game face, `tests/test_lm_glyphs.c`). The row runs the stock quit question with the popup text swapped to m40 #95 "End the session you created?"; the pause result setter `FUN_009564F0` is wrapped so that a confirmed quit of this row becomes "resume" (1) and the glue runs End Co-Op (`net endcoop`: `cs_kick(partner, END_COOP)`, the solo step). |
| Losses (0.2.0) | `lobbymust.inc` + `lobbymust_logic.h` (tests `tests/test_lobbymust.c`): the strips, the host's solo step (`Coop_ApplyJoinLeave` leave, 0x4287B0, after the main slot became this PC's human when the host plays slot 1; `sPlayer_RequestSwapCharacter` 0x6E9FC0 refuses every swap while gm+0x8F0 = 1, so the swap needs it), the guest's quit (`FUN_00A91D50`, what sBioMain runs for the pause result 3: in the campaign aGame state 0xE -> 0xD -> `Game_ResetToTitle`). |
| End of Episode 1 (0.2.0) | The episode-end menu (`FUN_00431190`, aGame state 2 / +0x620 9 / +0x624 7) offers items 2 (skills), 0 (next episode) and 3 (title); the call of the unlock check at 0x431279 is redirected: while the co-op end of Episode 1 is up it answers 0, so item 0 is left out and the stock "title" item (`Game_ResetToTitle`) is the way out. The EP1 strip is refreshed while this menu is up (`lobbymust_logic.h` `end_menu`: area 9000, aGame state 2, +0x624 = 8); the ending scene runs in aGame state 8 with no sDemo cutscene, so no strip is shown before the menu (live runs 5-7). |

Console (test builds, UDP 28200): `menu status | view | open | close | ev <up|down|left|right|ok|back|bs|paste|click n> | text <string> | start <ep> <part> <diff> | set k=v | gate 0|1 | hidecm 0|1 | enabled 0|1 | pad | text0 <table> <id> | lang <0-11|off> | banner <kind> [who] [ms]`; `net lm | endcoop | solo | quittitle | kick [pid] [reason]` (0.2.0). `menu view` prints the current screen as text (rows, selection, players, status), which is how the screens are tested without screenshots.

Tests: `sh tests/build.sh && tests/build/test_lobby.exe`; `tests/overlay_preview.c` renders the overlay into BMP files without the game (a Direct3D 9 device); its
`--reset-cycle` mode (run by `tests/build.sh`, no device) renders the lobby with the game's own faces in 6 face classes, releases everything as before a device Reset and
renders again, 3 rounds: the heaps must stay valid and the GDI object count must not grow (Rev1's shared Japanese face was once freed twice: heap corruption 0xC0000374).
`tests/test_resil.c`: the refusal reasons and the partner-silent strip (0.2.0) in 12 languages.

**0.2.0 messages.** A refusal because of another co-op package shows its reason in the player's language (`lobby_refusal_text`, `menu.c` drain_notes: protocol
`LT_REF_PROTO`, another mod version `LT_REF_MODVER` with both versions, an older host `LT_REF_MODVER_OLD`); other codes keep the session's English text. While the
partner's game sends nothing (`CS_N_SILENT`: Alt-Tab, a stall, a blocking load) and no lobby or other strip is on the screen, a strip says "No answer from <name> for N s"
(`LT_PARTNER_SILENT`) until it is heard again, gone, or 15 s after the last note.
