# Resident Evil Revelations 2 Co-op

[![Latest release](https://img.shields.io/github/v/release/dolbaras/revelations2-coop?label=latest%20release)](https://github.com/dolbaras/revelations2-coop/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

Network co-op for two players in the story campaign of **Resident Evil Revelations 2** (2015, PC, Steam build 3658995): each player on their own PC with their own copy of the game and of this mod, joined through Steam friends or by IP address. The campaign's second character is played by the second human; each PC shows its own character full screen.

## Status

**Version 0.3.0** ([release notes](https://github.com/dolbaras/revelations2-coop/releases/latest)): the lobby offers Episodes 1-4 (Claire's half with Moira and Barry's half with Natalia) and the bonus episodes The Struggle (Moira and Evgeny) and Little Miss (Natalia and Dark Natalia). **Episode 1 is tested from its start to its end; the other episodes were tested stage by stage in the lab, not as complete playthroughs.** A co-op game keeps its own save (the campaign save is never opened by a co-op game) and the room can continue from the host's last co-op checkpoint of Episode 1. Earlier: pre-releases 0.1.0 and 0.1.1, full releases 0.2.0 and 0.2.1.

Checked in the game on the author's test PCs (host and guest on two PCs; Episodes 2-4 and the bonus episodes stage by stage, with test jumps). Nobody outside the lab has played 0.3.0 yet, and play between two different networks (Steam relay) has not been tried. Both players need the same version of the mod: another version is refused and told so.

## What works

✅ checked in the game on test PCs · 🟨 partly checked (the row says what is missing) · ⬜ not tested · — not offered

| Area | Status |
|---|---|
| "Co-Op Play" row of the campaign menu: host or join by Steam friend (no port forwarding) or by IP address; each player picks a character, the host picks the episode and the difficulty, both press Ready; a refused join says why | ✅ |
| Episode 1, Claire's half and Barry's half from the start to the radio: every checkpoint, the results on both PCs, game over and Retry | ✅ |
| Episodes 2-4 and the bonus episodes: the lobby start with the right characters, both players walking and seeing each other, the fights of Episode 2's village (siege waves, the Pedro boss), the hand-over between stages and halves through the game's own results screens (Episode 3: 306 -> 351 and 303 -> 351; The Struggle: 600 -> 620 -> 610 -> 630), the ends of Episodes 2 and 3, The Struggle, Little Miss and the last stage | 🟨 stage by stage, not a playthrough; see "Known limits" |
| The world run by the host's PC: enemies as replicas on the other PC, the guest's knife, handgun and bottle hits, kill credit for the player who made the kill, story flags, co-op doors, shutters, boxes and slides, enemy grabs, items, the stage change between areas | ✅ |
| Downed / revive (the host's choice, on by default): the partner helps you up with the game's Rescue key before 60 seconds run out | ✅ both ways |
| A co-op save of its own (`rev2_coop_save_<number>.dat`), Continue in the room from the host's last checkpoint | ✅ Episode 1 checkpoints only |
| The partner's arrow on the minimap | ✅ |
| Per-player Steam achievements (not blocked: what a co-op game earns is really unlocked) | ✅ |
| Join through Steam on two PCs of one network; join by IP address | ✅ |
| Play between two different networks (Steam relay) | ⬜ |
| Other mods: an Ultimate ASI Loader in `dinput8.dll` (the Fusion Fix's loader) keeps running as `winmm.dll` | ✅ tried with the Fusion Fix of January 2025 |
| Installer, uninstaller and the self-updating "Resident Evil Revelations 2 Co-op" shortcut (SHA-256 checked) | ✅ |
| Played through by people outside the lab | ⬜ |
| Survival, Invisible, Raid | — not offered (Raid is never touched) |
| Joining a running game | — not possible |

## Download and install

1. Download from [Releases](https://github.com/dolbaras/revelations2-coop/releases/latest): `revelations2-coop-setup-<version>.exe` (one file, nothing to extract) or `revelations2-coop-<version>.zip` (extract it anywhere and run `Install Co-op.bat`; the same installer). Do it on BOTH PCs.
2. The installer finds your Steam copy (or asks for the folder that holds `rerev2.exe`), keeps a `dinput8.dll` of someone else's in `coop-backup`, adds the mod and makes the "Resident Evil Revelations 2 Co-op" shortcut (desktop, Start menu).
3. Start the game with that shortcut (it starts the game through Steam): before the game starts it installs a newer release from GitHub by itself (the download is checked against the SHA-256 GitHub gives for it); offline the game starts as it is. In the game choose Campaign, then **Co-Op Play**.

The setup exe is not code-signed, so Windows SmartScreen may warn ("More info" -> "Run anyway"); the zip with `Install Co-op.bat` does the same. To remove the mod run `Uninstall Co-op.bat` in the game folder (or the installer again, "Remove co-op"): it puts the backed-up files back; your saves are not touched. Details and the limits are in `README-COOP.txt` inside the zip (source `docs/release-readme.txt`).

## Requirements

- The Steam version of Resident Evil Revelations 2, build 3658995 (the installer checks `rerev2.exe` and refuses any other one). Nothing modifies the game's files: the DLL is loaded by the exe's own import table and patches only import slots and COM vtables in memory; the Steam stub is not touched.
- Windows 10 or 11, PowerShell 5.1 (part of Windows).
- Keyboard and mouse or an XInput pad for each player.
- Direct connection by IP: the host needs inbound UDP port 28410 (Windows asks once to allow the game through its firewall). Steam friends need no port.

## How to play

- Title screen -> Campaign -> **Co-Op Play** opens the room. The host picks the episode (Episodes 1-4, The Struggle, Little Miss), the half and the difficulty; each player picks a character; both press Ready.
- Join through Steam ("Join Game" in the friends list or the host's invite) or by the host's IP address.
- **Continue** in the room (Episode 1): Yes starts at the host's last co-op checkpoint, No starts a new game of the half; the guest follows the host's checkpoint with his own items.
- Leaving: the pause menu has "Leave" while a co-op game runs, the host also "End Co-Op" (the host plays on with the game's AI partner).

### Controls

| Key / pad | What it does |
|---|---|
| Rescue key (keyboard: the game's "Rescue" key, Tab by default; pad: Y) | helps a downed partner up when you stand next to them |
| F11 (game window in front) | a screenshot of the game into `rev2_coop\shots` next to `rerev2.exe` (attach it to a report) |

## Languages

The co-op screens, the installer and the shortcut's messages are in the game's 12 languages; Japanese text in the co-op screens uses a Windows font, not the game's own.

## Known limits / not verified

- Episodes 2-4 and the bonus episodes were verified in the lab stage by stage, NOT as complete playthroughs (most stages were reached by test jumps), and no human player has played them yet: 0.3.0 is published so that players can try them. Please report what goes wrong (see "Reporting a bug").
- Not verified:
  - Episode 2, Claire + Moira: the story's own step from the village (stage 201) to the next stage (203) after the siege and the Pedro fight (the button at the rubble on the north terrace; the next stage was only entered by a test jump), and the fights and the end of Claire's half in stage 203.
  - The fights in Barry's halves of Episodes 2, 3 and 4 (their stages were entered by test jumps and walked by both players; the enemies were asleep or out of reach) and in Claire's halves of Episodes 3 and 4 (Episode 3's stages 300, 301 and 303 were walked; the end of Claire's half at 303 led on to Barry's half).
  - The final bosses of Episode 4 (stages 453 and 454; the end of the episode was shown after the stage's end was requested by a test tool).
  - The Struggle: stage 610 starts behind Ryan's gate, which did not open in the tests; the clear conditions, timers and ends of stages 610 and 620.
  - Little Miss: the stealth rules (when the enemies notice a player) on both PCs.
  - The story's own steps between the stages of Episodes 2-4.
  - Companions that the host's story removes may stay on the guest's PC where the story left them (no effect on play was seen).
  - Play between two different networks (Steam relay).
- Barry's half of Episodes 2-4 starts by itself a few seconds after the first stage of the part has loaded: the first stage's opening may flash by.
- "Continue" in the room works for Episode 1 checkpoints only; for the other episodes choose a new game of the half.
- Joining a running game is not possible: if one player's game closes or the connection is lost, start a new co-op game from the lobby.
- The end of Episode 1's halves needs both players at the radio together.
- **Steam achievements are NOT blocked:** what you earn in a co-op game is really unlocked on your Steam account.
- The installer and the setup exe are not code-signed (see above).

## Reporting a bug

Open an [issue](https://github.com/dolbaras/revelations2-coop/issues) (the "Bug report" form asks for the version, the place and what happened) with a short description (episode, place, what you did, what happened, about when) and the logs of BOTH PCs. Double-click `coop\Collect logs.bat` in the game folder right after the problem: it puts one zip on your desktop with the mod's logs, the install record and a system summary, with your user, computer and player names, Steam names and IDs, paths, e-mail, MAC and public IP addresses removed. Nothing is uploaded; open the zip to check it before you attach it. A screenshot helps.

## For developers

- `native/` — the `dinput8.dll` proxy (C, 32-bit, built with Zig: `sh native/build.sh`; the ENet 1.3.18 sources are downloaded separately): `proxy.c` (log, exe SHA-256 gate, crash log, import patching, forwarding of `DirectInput8Create`), `coop_*.c` (session, protocol, ENet and Steam transports), `lobby.c`, `coopsave.c`, `d3d.c`, `steam.c`, test helpers (`testmode.c`, `console.c`, `inject.c`, `mute.c`). Unsupported exe (another build): the proxy only forwards `DirectInput8Create`.
- `tools/` — installer, launcher, texts in 12 languages, packaging, the test console client `r2console.py`; `tests/` — offline tests (`sh tests/build.sh`, `tests/test_installer.ps1`), a headless bot (`tests/coop_bot.c`).
- `docs/` — `world.md`, `achievements.md`, `protocol.md`, `lobby.md`, `steam-transport.md`, `downed-revive.md`, `releasing.md` (the release protocol the self-update relies on), `development.md` (layout, test variables and console commands), `release-readme.txt` (the players' readme, shipped as `README-COOP.txt`).
- **Raid is never touched.** The mod is meant for the story campaign. The game's online Raid mode depends on Capcom's RE.NET service, which was unreachable during development, so Raid could not be studied online. For the author's own tests a local, default-off lab switch was used to get past the game's "service available" check on private test copies; it is **not part of this repository**, and the published mod does nothing to Raid, its servers or its traffic.

## Credits and third-party

- ENet 1.3.18 (MIT) for the network transport; notice in `docs/enet-license.txt`, shipped as `ENET-LICENSE.txt`.
- ThirteenAG's Fusion Fix (MIT) and Lyall's RERevFix (MIT) were read for patterns and structures; the hub notes of other authors (agressivepr, ANGCRU00) were used as leads and verified before use.

## License

MIT (see `LICENSE`). No game files are in this repository or in the releases; you need your own copy of the game. Resident Evil Revelations 2 is a trademark of Capcom; this project is not affiliated with Capcom.

## По-русски

Resident Evil Revelations 2 Co-op — сетевой кооператив вдвоём для сюжетной кампании Resident Evil Revelations 2 (2015, PC, Steam): у каждого игрока свой ПК и своя копия игры, подключение через друзей Steam или по IP-адресу. Скачайте `revelations2-coop-setup-<версия>.exe` или zip со страницы [выпусков](https://github.com/dolbaras/revelations2-coop/releases/latest) и запустите установщик на обоих ПК; дальше запускайте игру ярлыком «Resident Evil Revelations 2 Co-op»: он сам ставит новые версии. В игре — «Кампания», затем **Co-Op Play**. Описание каждого выпуска есть и на русском.
