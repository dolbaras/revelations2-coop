# Releasing

Players start the game with the "Resident Evil Revelations 2 Co-op" shortcut, which runs `coop\coop-launcher.ps1` (`tools/launcher.ps1`). Before the game starts it reads
`https://api.github.com/repos/dolbaras/revelations2-coop/releases/latest` (the repository name is a constant at the top of `tools/launcher.ps1` and in its URL check;
the owner creates the repository) and installs that release when its tag is newer than `coop\version.txt`. Publishing a release therefore updates every player who uses
the shortcut the next time they start the game, so a release must be finished and tested before it goes up. Nothing is published from this workspace without the owner.

What the launcher already out there relies on, so every later release keeps it (the same protocol as the Revelations 1, Jericho and TimeShift mods of this workspace):

- **Tag** `v<major>.<minor>.<patch>` (parsed as a .NET `[version]`), a full release: drafts and pre-releases are not "latest" and are never installed.
- **Asset** named `revelations2-coop-<version>.zip` (the first asset matching `revelations2-coop-*.zip`), attached to that release, so its URL is under
  `https://github.com/dolbaras/revelations2-coop/releases/download/`. The launcher checks the download against the `sha256:` digest GitHub gives for the asset.
- **`coop-installer.ps1` and `coop-texts.ps1` at the root of the zip** (as `tools/package.ps1` builds them), the installer taking
  `-Game <folder> -AutoClose -NoShortcut`: install into that folder with the window, close it by itself after a successful install, exit code 0 on success and
  non-zero on failure; `-NoShortcut` refreshes only the shortcuts the install record lists.
- **`files\coop\version.txt`** holding the version without the `v`.
- **`<game>\coop\install.txt`**, the install record the next installer reads: `version=`, then one line per `file=`, `backup=` and `shortcut=` (several shortcuts on one
  line are split after each `.lnk`, empty ones dropped).
- The mod's DLL carries the words `RE Revelations 2 co-op proxy` (the installer tells it from a dinput8.dll of someone else's by them): keep the log line
  (`native/proxy.c`; `tools/package.ps1` refuses a DLL without it).

A change to any of these needs a launcher that copes with both the old and the new form, shipped one release before the old form is dropped.

## Building

```
sh native/build.sh                                      # native\build\dinput8.dll (32-bit, Zig)
OUT=build-x/dinput8.dll sh native/build.sh              # the same into native\build-x\ (another agent may use native\build)
powershell -File tools\package.ps1 -Version 0.1.0       # dist\revelations2-coop-0.1.0.zip
powershell -File tools\package.ps1 -Version 0.1.0 -Exe  # + dist\revelations2-coop-setup-0.1.0.exe (workspace tools\mkinstaller; unsigned)
powershell -File tools\package.ps1 -Version 0.1.0 -Exe -Dll native\build-x\dinput8.dll -Dist <folder>   # another DLL, another output folder
```

The zip holds `Install Co-op.bat`, `coop-installer.ps1`, `coop-texts.ps1` (the 12 languages of the game, the order and codes of `native/lobby_text.c`: en fr it de es ru pl pt-br
ja ko zh-cn zh-tw; shared by the installer and the launcher), `README-COOP.txt`, `LICENSE.txt` and `files\` (`dinput8.dll`, `coop\coop-launcher.ps1`,
`coop\coop-installer.ps1`, `coop\coop-texts.ps1`, `coop\README.txt`, `coop\version.txt`, `coop\payload.txt` = the list of every payload file that the installer checks
before it touches the game). The package sets no environment variable and the shortcut's launcher sets none: `REV2_ACHBLOCK` (Steam unlocks blocked) is a console-run default only,
so a player's game passes every achievement call through to Steam (see `docs/achievements.md`).

## What the installer does

- Finds the game: its own folder, the Steam uninstall key of app 287290, every Steam library (`libraryfolders.vdf` and the `appmanifest_287290.acf` installdir), common
  folders, or the player picks it. **It checks that `rerev2.exe` is the Steam build 3658995** (SHA-256 `55ac7d2b...67f9`, the same constant as `R2_EXE_SHA256` in
  `native/r2.h`) and refuses any other exe in the player's language (the mod's gate would keep it off anyway); `-AnyExe` or `REV2_COOP_ANYEXE=1` skip the check (tests only).
- Refuses while a `rerev2.exe` runs from that folder. A folder Windows protects is installed by an elevated copy of the installer (Windows asks the player).
- Keeps a `dinput8.dll` of someone else's (an ASI loader, a graphics fix) in `<game>\coop-backup`, once; its own DLL of an older version is never kept as the original. A
  `dinput8_real.dll` the player placed (the mod's chain rule) and every other file are left alone.
- Copies the payload, writes `coop\install.txt` (`coop\version.txt` last: the launcher reads it, an update that broke off is tried again at the next start), the
  desktop and Start menu shortcuts (the game's own icon with a co-op badge, `coop\coop.ico`), and `Uninstall Co-op.bat` at the game folder's root.
- The shortcut starts the game through Steam (`steam://rungameid/287290`, found from the library's `appmanifest_*.acf`; a copy outside a Steam library starts `rerev2.exe`). Raid
  Mode is not separated by the launcher: the mod itself does nothing outside a co-op game started from the lobby (the lobby row is on the campaign page only).
- Removal (`Uninstall Co-op.bat`, or the installer again with "Remove co-op"): deletes what the record lists, puts the backed-up files back, removes the shortcuts, `coop`
  and `coop-backup`. The game's saves and `rev2_coop.ini` (the lobby's settings, `%LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS2`) are not touched.
- Texts in 12 languages, chosen by Windows' language, `-Lang` or the list in the window (Windows in Portuguese from Portugal gets Brazilian Portuguese; a language the game does not
  have gets English).

## Tests

`powershell -File tests\test_installer.ps1 [-RealGame <folder with a COPY of the real rerev2.exe>]` (everything in a temp folder, no game is started, no network): the zip's layout and
payload list; a fake exe is refused without a trace; install over a game with a foreign dinput8.dll (backup, record, two shortcuts, every other game file untouched, a
`dinput8_real.dll` and a plugin folder of the player survive); reinstall (the backup stays the foreign DLL); the launcher's update from a local feed served on 127.0.0.1 (a wrong checksum
refused, the right one installed, backup and shortcuts kept); the launcher's start line (`steam://rungameid/287290` for a Steam library folder, `rerev2.exe` otherwise; `-NoStart`);
uninstall (the game folder is byte for byte the clean one); a game without dinput8.dll; a path with spaces and an apostrophe; no shipped script sets or names a lab variable;
the feed constants; the installer's exe hash equals the mod's own gate; with `-RealGame` a COPY of the real exe passes the hash check and its icon becomes the shortcut's icon
(the test refuses a Steam library or any folder called `game`); the 12 texts (same keys, same placeholders, translated, no Revelations 1 name or build) and the language choice. The installer's
WINDOW needs a person to look at it (see "Release status"); test-only variables `REV2_COOP_UI_POS=x,y` (window at that position WITHOUT taking the focus), `REV2_COOP_UI_SHOTS=<folder>`,
`REV2_COOP_UI_LANGS=en,ru`, `REV2_COOP_UI_TAG`, `REV2_COOP_UI_AUTO=install|remove` (clicks the button, only for the folder in `REV2_COOP_UI_ONLY`; a player never sets them).

An update test against the real GitHub is done with `coop-launcher.ps1 -Feed http://127.0.0.1:<port>/latest.json -NoStart` as the test does, or `REV2_COOP_FEED` for a start
through the shortcut. New shortcuts go to `-ShortcutDir <folder>` in tests, never to the desktop.

## Versions and pre-releases

The launcher installs only `releases/latest` of the repository, and only when its tag parses as a .NET `[version]` that is HIGHER than `coop\version.txt` of the installed copy. GitHub never
reports a pre-release as "latest", so a pre-release is never installed by the shortcut: a tester installs the zip or the setup exe by hand (the installer writes the same record and
shortcut as for a full release). Scheme, fixed so that every pre-release sorts strictly below the first full release that should update it:

| What | Version / tag | Notes |
|---|---|---|
| Pre-release 1 | package version `0.1.0`, tag `v0.1.0`, GitHub "pre-release" | `R2_VERSION` `0.1.0`, `coop\version.txt` `0.1.0`; title "Pre-release 1 (testing)" |
| Pre-release 2, 3, ... | `0.1.1`, `0.1.2`, ... (tag `v0.1.N`, pre-release) | the next pre-release bumps the patch number, `R2_VERSION` and `-Version` in step; both players must run the same one |
| First full release | `0.2.0` (tag `v0.2.0`, NOT a pre-release) | higher than any `0.1.N`, so every tester who installed a pre-release is updated by the shortcut at the next start |

Rules that follow: never publish a full release with a version below `0.2.0` while pre-releases `0.1.N` exist; never mark a tag `v0.2.0` or higher as a pre-release while it should reach players;
no "RC" wording is used anywhere (title, notes, tag message, README). A pre-release's asset names are the same as a full release's (`revelations2-coop-<version>.zip`, `revelations2-coop-setup-<version>.exe`).
The network handshake checks the protocol version (`CP_PROTO_VERSION`), not the package version, but the world sync of two different packages may disagree: tell both testers to install the same package.

## Publishing checklist for a pre-release (commands for the lead; nothing here is run from the lab without the owner's word)

Tools: `gh` is the portable one at `<workspace>\tools\gh\bin\gh.exe` with `$env:GH_CONFIG_DIR=<workspace>\tools\gh\config` (PowerShell); identity: the noreply address of the owner's GitHub account, set locally in `pub\`.

1. Freeze: `git -C mod status --short` empty; the nine suites and `tests\test_installer.ps1` green on that commit; `OUT=<abs path>\dinput8.dll sh mod/native/build.sh`, then
   `powershell -File mod\tools\package.ps1 -Version 0.1.0 -Exe -Dll <that dll> -Dist work\dist-pre1`. Record the SHA-256 of the zip, the setup exe and the DLL.
2. Sanitized public tree, dry run first: `DRYRUN=1 PUB=<scratch folder> sh lab/export_public.sh` (exports the committed HEAD of `mod/`, strips the lab-only switches, puts `lab/pub-README.md` in as `README.md`,
   greps for private paths, names, account ids, e-mail and any "RC" wording, builds). Then the real sync: `sh lab/export_public.sh` (working tree in `pub\`), review `git -C pub diff --stat`.
3. Public repository: `git -C pub add -A`, `git -C pub commit -m "Revelations 2 co-op: pre-release 1 (0.1.0)"` (with the Co-Authored-By trailer), then
   `git -C pub -c credential.helper= -c "credential.helper=!'<gh.exe>' auth git-credential" push origin main`. (If the repository does not exist yet: `gh repo create dolbaras/revelations2-coop --public --source pub --remote origin` first.)
4. Pre-release (the tag is made by the command, on the pushed main):
   `gh release create v0.1.0 --repo dolbaras/revelations2-coop --target main --prerelease --title "Pre-release 1 (testing)" --notes-file notes\releases\relnotes-rev2-pre1-github.md work\dist-pre1\revelations2-coop-0.1.0.zip work\dist-pre1\revelations2-coop-setup-0.1.0.exe`
   Never `--latest`, never `--draft`.
5. Check: `gh release view v0.1.0 --repo dolbaras/revelations2-coop --json isPrerelease,assets` (`isPrerelease` true; the asset `digest` equals the local SHA-256 of the zip and of the exe), and that
   `https://api.github.com/repos/dolbaras/revelations2-coop/releases/latest` answers 404 (no full release yet: the shortcut finds nothing to install, which is intended).
6. A tester's side: download the zip or the setup exe from the release page, run `Install Co-op.bat` (or the setup exe) on BOTH PCs, start the game with the new shortcut.
7. The first full release later: `R2_VERSION` and `-Version` `0.2.0`, same commands without `--prerelease`, with `--latest`, after the owner's explicit word, and with the update test of the launcher from a copy that has pre-release 1 installed.

## Release status

First public package: **pre-release 1 (0.1.0)**, for testing by the owner and testers; the first full release is `0.2.0` (see "Versions and pre-releases").

Verified offline (no game started, no window shown):
- `tests\test_installer.ps1`: all checks pass (zip layout, payload list, refusal of another exe, install / reinstall / update through a local feed with a wrong and a right checksum, the
  launcher's Steam start line, uninstall restoring the game folder byte for byte, foreign `dinput8.dll` backup, path with spaces, the 12 texts, language choice, the window's code run
  without showing it for 12 languages x 6 folder states, no lab variable shipped, the exe hash equals the mod's gate).
- The package builds from a DLL of the mod's HEAD (`tools\package.ps1 -Dll`), zip and setup exe; the setup exe's own self test (payload equals the zip) passes.

Verified live on 2026-11-05 (window on the second monitor without focus, a throw-away copy `work\f6n\RE2 throwaway` with a COPY of the real `rerev2.exe` (hash check without `-AnyExe`) and a foreign `dinput8.dll` stand-in, shortcuts into a test folder, never the desktop):
- The installer WINDOW (start page in en / ru / ja / ko, progress page, result page `installed 0.1.0, 7 files, backup of dinput8.dll, shortcut`), the install button, and "Remove co-op": the copy was byte for byte as before afterwards (the foreign DLL back, no `coop`, `coop-backup`, shortcut or `Uninstall Co-op.bat`). Pictures `work\s6n\ev\installer_ui\`.
- The zip's `Install Co-op.bat` and `Uninstall Co-op.bat`, and the setup exe (`revelations2-coop-setup-0.1.0.exe`, started from the copy's folder): the same install / uninstall result each time.
- The shortcut the installer made: powershell -> `coop\coop-launcher.ps1`, icon `coop\coop.ico`; the launcher with `-NoStart` ran (exit 0; the GitHub repository does not exist yet, so there was nothing to update).

Still not checked live: the shortcut starting the game through Steam (deliberately not started: `steam://rungameid/287290` would start the owner's real install), a real ASI loader or the widescreen fix as the foreign `dinput8.dll` (only a stand-in file was used), the update step against a real GitHub release (none exists), the folder picker's dialog, the taskbar progress.
The wording of the known limits in `docs/release-readme.txt` (rewritten for pre-release 1 from what the lab runs showed) says plainly what is and is not verified, including that Barry's half is only verified up to stage 151 / the first corridor of stage 152.
  Real Steam unlocks were allowed by the owner for the lab accounts and one shared record (the flame achievement) was unlocked and read back on both accounts; `R2_VERSION` says `0.1.0`.
