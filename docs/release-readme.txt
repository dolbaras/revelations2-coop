Resident Evil Revelations 2 Co-op - version @VERSION@
=====================================================

Network co-op for the story campaign of Resident Evil Revelations 2 (PC, Steam build 3658995): Episodes 1 to 4 (both halves of each) and the bonus episodes
"The Struggle" and "Little Miss", for two players on two PCs, each PC with its own copy of the game and of this mod. Episode 1 is tested from start to end;
the other episodes were tested stage by stage in the lab, not as complete playthroughs: see "Limits".

What it does
- The campaign's second character (Claire and Moira in the first half of an episode, Barry and Natalia in the second half; Moira and Evgeny in The Struggle,
  Natalia and Dark Natalia in Little Miss) is played by a second human over the network. Each PC shows its own character full screen; the other player's character is moved from the network.
- A lobby, opened by the new "Co-Op Play" row of the campaign menu (title screen, Campaign): host or join by Steam friend (no port forwarding) or by
  IP address, each player picks a character, the host picks the episode and the difficulty, both press Ready. The room tells the guest what it waits for, and a refused join
  says why (session full, game already started, no answer from the host, another co-op version, another game version, removed by the host). The host can
  remove a player from the room. The pause menu has "Leave" while a co-op game runs, and the host also has "End Co-Op" (the host plays on with the game's AI
  partner).
- The story is kept in step between the two PCs: enemies (the host's PC runs their AI; the guest's blows and shots reach them through the host), doors and
  co-op shutters, boxes that are carried or pushed, slides and boosts, the story flags, item pickups, level loads (both PCs wait for
  each other), deaths and retries, the stage changes, and each player's own Steam achievements.
- Downed / revive (the host's choice in the room, on by default): a player whose health runs out lies down instead of dying and bleeds out after 60 seconds
  unless the partner holds the game's Rescue key next to him (keyboard: the "Rescue" key of the game's key settings, Tab by default; pad: Y). When both
  players are down, or one dies outright (a fall), it is game over for both.
- At the end of an episode (or of a half that ends the game's story part, The Struggle and Little Miss) each PC shows the game's own results, records and
  rewards, the co-op session ends, and the game's end menu offers no next episode: both players go back to the title with its title row. In the last stage of
  Episode 4 the credits come first (about ten minutes on each PC).
- When a player leaves or the connection is lost, the host reads it on screen and plays on with the game's AI partner; a guest whose host is gone reads it
  and goes back to the title by itself. A partner who does not answer for 8 seconds (game in the background, a long load, a stall) is named on screen.
- A co-op game keeps its own save (new in 0.2.1): your campaign save is never changed by a co-op game. In the room the host can choose "Continue" to go on
  from the last checkpoint of the co-op save; the guest then follows the host's checkpoint. See "Saves" below.
- Raid Mode and the game's online services are not touched; the mod does nothing outside a co-op game started from the lobby.

Install
1. Run "Install Co-op.bat" (or the setup .exe). The window finds your Steam copy; otherwise choose the folder that contains rerev2.exe. Do it on BOTH PCs.
2. The installer copies dinput8.dll and a coop folder into the game folder, keeps a dinput8.dll of someone else's in coop-backup, and makes the
   "Resident Evil Revelations 2 Co-op" shortcut (desktop, Start menu). Start the game with that shortcut (it starts it through Steam): it also installs a newer
   version of the mod from GitHub first, if there is one (no internet: the game starts as it is). A new version is installed only when its download matches the
   checksum GitHub gives for it.
3. In the game choose Campaign, then "Co-Op Play". Both players need the same version of the mod: a player with another version is refused and told so.
If you had an earlier version (0.1.0, 0.1.1, 0.2.0 or 0.2.1): the shortcut updates it to this version by itself at the next start. Both PCs need the same version.

Remove: run "Uninstall Co-op.bat" in the game folder (or the installer again and choose "Remove co-op"). It deletes what it installed (also the mod's log
files next to rerev2.exe), puts the backed-up files back and removes the shortcuts. Your saves are not touched: the game's own save stays where Steam keeps
it, and the co-op save and the settings file stay in %LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS2 (rev2_coop_save_<number>.dat and .rec, rev2_coop.ini;
delete them by hand if you want them gone); screenshots you took with the mod stay in rev2_coop\shots.

Needs: the Steam version of Resident Evil Revelations 2, build 3658995 (the installer checks rerev2.exe and refuses any other one), Windows 10 or 11,
PowerShell 5.1 (part of Windows). Direct connection by IP: the host needs inbound UDP port 28410 (Windows asks once to allow the game through its firewall).

Limits
- Verified in the lab stage by stage, NOT as a complete playthrough. Episode 1 is tested from start to end. Episodes 2-4 and the two bonus episodes: the lobby start with
  the right characters, walking of both players, the fights of Episode 2's village (siege waves and the Pedro boss) and of The Struggle's second chapter, the hand-over
  of Episode 3 and of The Struggle between their stages, and the end of Episodes 2 and 3, The Struggle, Little Miss and the game's last stage were shown on two PCs; most
  stages were reached by test jumps. Nobody outside the author has played them yet. If something goes wrong, please report it with the logs (see "Report a problem").
- NOT verified:
  * Episode 2, Claire and Moira: the story's own step from the village (stage 201) to the next stage (203) after the siege and the Pedro fight (the button at the rubble
    on the north terrace; the next stage was only entered by a test jump), and the fights and the end of Claire's half in stage 203.
  * The fights in Barry's halves of Episodes 2, 3 and 4 (their stages were entered by test jumps and walked by both players; the enemies were asleep or out of reach)
    and in Claire's halves of Episodes 3 and 4 (Episode 3's stages 300, 301 and 303 were walked, and the end of Claire's half at 303 led on to Barry's half).
  * The final bosses of Episode 4 (stages 453 and 454; the end of the episode was shown after the stage's end was requested by a test tool).
  * The Struggle: stage 610 starts behind Ryan's gate, which did not open in the tests; the clear conditions, timers and ends of stages 610 and 620.
  * Little Miss: the stealth rules (when the enemies notice a player) on both PCs.
  * The story's own steps between the stages of Episodes 2-4.
  * Companions that the host's story removes may stay on the guest's PC where the story left them (no effect on play was seen).
  * Play between two different networks (Steam relay).
- Barry's half of Episodes 2-4 starts by itself a few seconds after the first stage of the part has loaded (the game does not start it from the menu): the first stage's
  opening may flash by.
- "Continue" in the room works for Episode 1 checkpoints only; for the other episodes choose a new game of the half.
- Survival, Invisible and Raid are not offered by the lobby. Episodes 1 to 4 and the two bonus episodes (The Struggle, Little Miss) are.
- A co-op game starts at the beginning of the half the host picks (Claire's half or Barry's half), or with Continue at the last checkpoint of the co-op save;
  joining a running game is not possible. If one player's game closes or the connection is lost, start a new co-op game from the lobby (Continue takes you
  back to the last checkpoint).
- The end of Episode 1's halves: both players must stand at the radio together (there is no timeout while one of them stays away).
- Saves: a co-op game has its own save, on each PC in %LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS2\rev2_coop_save_<number>.dat (one per Steam account; the
  .rec file next to it is the checkpoint the room shows). Your campaign save (the one Steam keeps and Steam Cloud carries) is not opened by a co-op game and
  is exactly as before when you leave co-op; the co-op save is not in Steam Cloud, so it stays on that PC. The first co-op game starts its save from a copy of
  your campaign save's settings and unlocks; after that the two saves are separate. "Continue" in the room starts at the last checkpoint of the host's co-op
  save; both players need a co-op save of that half (they played it together before), and the guest follows the host's checkpoint. To start the co-op story
  over, choose Continue "No" (a new game of the half), or delete the two files. After a game over, Retry takes both players back to the same checkpoint.
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
A screenshot of the problem helps. Open an issue at https://github.com/dolbaras/revelations2-coop/issues and attach the zips and the screenshot.

License: MIT (LICENSE.txt). ENet 1.3.18 (MIT, ENET-LICENSE.txt). Resident Evil Revelations 2 is a trademark of Capcom; this project is not affiliated with Capcom. No game files are shipped; the mod changes no game file other than adding dinput8.dll.
