Resident Evil Revelations 2 Co-op  -  PRE-RELEASE 1 (version 0.1.0)
===================================================================

THIS IS A PRE-RELEASE FOR TESTING, NOT A FINISHED MOD. It was tested only by its author, on two PCs, mostly with shortened routes. Please play it, expect
problems, and tell us what went wrong (see "Report a problem" below). The shortcut's self-update never installs a pre-release by itself: you installed this one by hand.

Network co-op for the story campaign of Resident Evil Revelations 2 (PC, Steam build 3658995): two players on two PCs, each PC with its own copy of the
game and of this mod. This first version is for EPISODE 1 only.

What it does
- The campaign's second character (Claire and Moira in the first half of an episode, Barry and Natalia in the second half) is played by a second human over
  the network. Each PC shows its own character full screen; the other player's character is moved from the network.
- A lobby, opened by the new "Co-Op Play" row of the campaign menu (title screen, Campaign): host or join by Steam friend (no port forwarding) or by
  IP address, each player picks a character, the host picks the episode and the difficulty, both press Ready. The pause menu has "Leave" while a co-op
  game runs.
- The story is kept in step between the two PCs: enemies (the host's PC runs their AI), doors, scenes, level loads (both PCs wait for each other), deaths
  and retries, the stage change between areas, and each player's own Steam achievements.
- Raid Mode and the game's online services are not touched; the mod does nothing outside a co-op game started from the lobby.

Install
1. Run "Install Co-op.bat" (or the setup .exe). The window finds your Steam copy; otherwise choose the folder that contains rerev2.exe. Do it on BOTH PCs.
2. The installer copies dinput8.dll and a coop folder into the game folder, keeps a dinput8.dll of someone else's in coop-backup, and makes the
   "Resident Evil Revelations 2 Co-op" shortcut (desktop, Start menu). Start the game with that shortcut (it starts it through Steam): it also installs a newer
   version of the mod from GitHub first, if there is one (no internet: the game starts as it is).
3. In the game choose Campaign, then "Co-Op Play". Both players need the same version of the mod.

Remove: run "Uninstall Co-op.bat" in the game folder (or the installer again and choose "Remove co-op"). It deletes what it installed, puts the backed-up
files back and removes the shortcuts. Your saves are not touched; the settings file stays in %LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS2
(rev2_coop.ini, a few lines).

Needs: the Steam version of Resident Evil Revelations 2, build 3658995 (the installer checks rerev2.exe and refuses any other one), Windows 10 or 11,
PowerShell 5.1 (part of Windows). Direct connection by IP: the host needs inbound UDP port 28410 (Windows asks once to allow the game through its firewall).

Limits (honest)
- WHAT HAS BEEN SEEN WORKING (two PCs, author's lab, shortened routes): Claire's half of Episode 1 from its start to its end (Claire and Moira, host and client), the stage
  changes, the end of the half, the game's own continuation into Barry's half, retries after a death (from the first two checkpoints), the lobby (host / join by Steam friend or
  by IP, character choice, Ready), the "Leave" row, and Steam achievements: one shared achievement (the flame achievement of the first half) was unlocked on both authors'
  Steam accounts and read back from Steam. Windows and menus were driven with a virtual gamepad on one PC (menus, walking, aiming, firing).
- WHAT HAS NOT BEEN CHECKED, expect trouble there:
  - Barry's half of Episode 1 beyond the north door of its stage 151 (the prison puzzles with two humans, the forest, the radio and the end of the half). The two-human steps of
    the prison (Natalia through a small hole, bricks, crates) were never played through for real by two people.
  - Real kills by both players (Natalia has no weapon in Barry's half), per-player kill records, boss fights, parts of the game where the two characters are far apart, and a
    full playthrough of anything without shortcuts.
  - Episodes 2 to 4 (the lobby offers them, they are untested and may not work at all); the bonus episodes, Survival, Invisible and Raid are not supported.
  - A real keyboard and mouse or a physical gamepad in the lobby (only a virtual pad and a test driver were used). The shortcut starting the game through Steam was never run
    live (the author's tests started the game another way), and the installer was only run on a throw-away copy of the game folder.
- The end of a half: the story's own radio zone does not react in a co-op game (it is an inert zone of the game's script). The mod's host side ends the half when every
  human stands in the radio zone for a moment; a player who stays away keeps the half from ending (there is no timeout). This was seen working in Claire's half; the zone of
  Barry's half is only tested offline.
- Known problems: trigger zones on the floor (some puzzles with two zones) may not react to the partner's character; after a death the game puts you back at the last
  checkpoint of your own PC (a checkpoint is written when a player walks into a checkpoint area, so both players should pass the same ones). In Barry's half the stage
  change at the end of stage 151 needed help from test tools in the lab: it may not happen by itself.
- Saves: a co-op game uses the normal campaign save slot of your Steam account, and its checkpoints overwrite that slot exactly as "New Game" would. BACK UP YOUR SAVE FIRST
  if it matters.
- Achievements: Steam achievements are NOT blocked in your game. What you earn in a co-op game is really unlocked on your Steam account (and cannot be taken back by this mod);
  each PC credits only what its own player did, through the game's own record path. The per-player records for kills have only been checked in logs with unlocks switched off,
  so a kill or knife achievement may be missing or credited to the wrong person.
- Other mods that use dinput8.dll (ASI loaders, widescreen fixes): the installer keeps your file in coop-backup and puts the mod's dinput8.dll in its place;
  they stop working until you remove the co-op mod. The mod forwards to a dinput8_real.dll next to it if you rename a loader yourself; that is untested.
- The installer and the setup .exe are not code-signed: Windows SmartScreen may warn. The zip works the same way.
- Keyboard and mouse or an XInput pad for the local player; the game's own split-screen rows are hidden during a co-op game.

Report a problem
Please send (a) a short description: which episode and place, what you did, what happened, roughly what time; (b) from BOTH PCs the file rev2_coop.log in the game folder
(next to rerev2.exe) and rev2_coop.1.log (the previous start's log), copied right after the problem, before you start the game again; if the game crashed, they hold the
crash lines. They contain your Steam name and the game folder's path, nothing else personal; (c) the file rev2_coop.ini from %LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS2.
A screenshot of the problem helps. Send it to whoever gave you this package, or open an issue at https://github.com/dolbaras/revelations2-coop/issues

Licence: MIT (LICENSE.txt). Not affiliated with Capcom. No game files are shipped; the mod changes no game file other than adding dinput8.dll.
