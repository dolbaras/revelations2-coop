Resident Evil Revelations 2 Co-op  -  version 0.2.0
===================================================

Network co-op for Episode 1 of the story campaign of Resident Evil Revelations 2 (PC, Steam build 3658995): two players on two PCs, each PC with its own copy of
the game and of this mod.

What it does
- The campaign's second character (Claire and Moira in the first half of Episode 1, Barry and Natalia in the second half) is played by a second human over
  the network. Each PC shows its own character full screen; the other player's character is moved from the network.
- A lobby, opened by the new "Co-Op Play" row of the campaign menu (title screen, Campaign): host or join by Steam friend (no port forwarding) or by
  IP address, each player picks a character, the host picks the difficulty, both press Ready. The room tells the guest what it waits for, and a refused join
  says why (session full, game already started, no answer from the host, another co-op version, another game version, removed by the host). The host can
  remove a player from the room. The pause menu has "Leave" while a co-op game runs, and the host also has "End Co-Op" (the host plays on with the game's AI
  partner).
- The story is kept in step between the two PCs: enemies (the host's PC runs their AI; the guest's blows and shots reach them through the host), doors and
  co-op shutters, boxes that are carried or pushed, slides and boosts, the story flags, item pickups, level loads (both PCs wait for
  each other), deaths and retries, the stage changes, and each player's own Steam achievements.
- Downed / revive (the host's choice in the room, on by default): a player whose health runs out lies down instead of dying and bleeds out after 60 seconds
  unless the partner holds the game's Rescue key next to him (keyboard: the "Rescue" key of the game's key settings, Tab by default; pad: Y). When both
  players are down, or one dies outright (a fall), it is game over for both.
- At the end of Episode 1 each PC shows the game's own results, records and rewards, the co-op session ends, and the game's episode-end menu offers no next
  episode: both players go back to the title with its title row.
- When a player leaves or the connection is lost, the host reads it on screen and plays on with the game's AI partner; a guest whose host is gone reads it
  and goes back to the title by itself. A partner who does not answer for 8 seconds (game in the background, a long load, a stall) is named on screen.
- Raid Mode and the game's online services are not touched; the mod does nothing outside a co-op game started from the lobby.

Install
1. Run "Install Co-op.bat" (or the setup .exe). The window finds your Steam copy; otherwise choose the folder that contains rerev2.exe. Do it on BOTH PCs.
2. The installer copies dinput8.dll and a coop folder into the game folder, keeps a dinput8.dll of someone else's in coop-backup, and makes the
   "Resident Evil Revelations 2 Co-op" shortcut (desktop, Start menu). Start the game with that shortcut (it starts it through Steam): it also installs a newer
   version of the mod from GitHub first, if there is one (no internet: the game starts as it is). A new version is installed only when its download matches the
   checksum GitHub gives for it.
3. In the game choose Campaign, then "Co-Op Play". Both players need the same version of the mod: a player with another version is refused and told so.
If you had a pre-release (0.1.0 or 0.1.1): the shortcut updates it to this version by itself at the next start.

Remove: run "Uninstall Co-op.bat" in the game folder (or the installer again and choose "Remove co-op"). It deletes what it installed (also the mod's log
files next to rerev2.exe), puts the backed-up files back and removes the shortcuts. Your saves are not touched; the settings file stays in
%LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS2 (rev2_coop.ini, a few lines); screenshots you took with the mod stay in rev2_coop\shots.

Needs: the Steam version of Resident Evil Revelations 2, build 3658995 (the installer checks rerev2.exe and refuses any other one), Windows 10 or 11,
PowerShell 5.1 (part of Windows). Direct connection by IP: the host needs inbound UDP port 28410 (Windows asks once to allow the game through its firewall).

Limits
- Episode 1 only: Episodes 2 to 4, the bonus episodes, Survival, Invisible and Raid are not offered by the lobby.
- A co-op game always starts at the beginning of the half the host picks (Claire's half or Barry's half); joining a running game is not possible. If one
  player's game closes or the connection is lost, start a new co-op game of that half from the lobby.
- The end of a half: both players must stand at the radio together (there is no timeout while one of them stays away).
- Saves: a co-op game uses the normal campaign save slot of your Steam account, and its checkpoints overwrite that slot exactly as "New Game" would. BACK UP YOUR SAVE FIRST
  if it matters. After a game over, Retry takes both players back to the same checkpoint.
- Achievements: Steam achievements are NOT blocked in your game. What you earn in a co-op game is really unlocked on your Steam account (and cannot be taken back by this mod);
  each PC credits only what its own player did, through the game's own record path.
- Japanese text in the co-op screens uses a Windows font, not the game's own.
- Other mods that use dinput8.dll (ASI loaders, the Fusion Fix): the installer keeps your file in coop-backup and puts the mod's dinput8.dll in its place.
  If your file is an Ultimate ASI Loader (the loader of the Fusion Fix and of most ASI mods), the installer also puts a copy of it in the game folder as
  winmm.dll, so the plugins in your scripts folder keep working (tried with the Fusion Fix of January 2025: the fix and the co-op lobby both ran). Any other
  dinput8.dll is off until you remove the co-op mod, which puts your files back as they were. Do not rename an ASI loader to dinput8_real.dll: the mod does not
  load a loader under that name. A dinput8.dll another mod puts over the co-op one later is kept too: the next install or update of the co-op mod keeps it as
  your original, removing the co-op mod leaves it in place. Other DLLs such mods use (d3d9.dll, dsound.dll, version.dll, xinput1_3.dll) are not touched.
- The installer and the setup .exe are not code-signed: Windows SmartScreen may warn. The zip works the same way.
- Keyboard and mouse or an XInput pad for the local player; the game's own split-screen rows are hidden during a co-op game.

Report a problem
Please send (a) a short description: which episode and place, what you did, what happened, roughly what time; (b) from BOTH PCs the zip that coop\Collect logs.bat in the
game folder puts on your desktop (double-click it right after the problem, before you start the game again; the game keeps only the last five starts' logs). It holds
rev2_coop.log (next to rerev2.exe; if the game crashed, it holds the crash lines) and rev2_coop.1.log ... rev2_coop.4.log (the starts before), rev2_coop.ini and the
game's config.ini from %LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS2 (your lobby and display settings), the install record and logs, the mod's version, hashes of
rerev2.exe and of the mod's DLL, Windows, graphics card and driver, and the names (not the contents) of the files in the mod's folder. It removes your Windows user name,
computer name, folder paths, player names, Steam names and IDs, e-mail and MAC addresses and public IP addresses, and uploads nothing; open the zip to check it.
A screenshot of the problem helps. Send it to whoever gave you this package, or open an issue at https://github.com/dolbaras/revelations2-coop/issues

Licence: MIT (LICENSE.txt). Not affiliated with Capcom. No game files are shipped; the mod changes no game file other than adding dinput8.dll.
