# Releasing

Players start the game with the "Resident Evil Revelations 2 Co-op" shortcut, which runs `coop\coop-launcher.ps1` (`tools/launcher.ps1`). Before the game starts it reads
`https://api.github.com/repos/dolbaras/revelations2-coop/releases/latest` (the repository name is a constant at the top of `tools/launcher.ps1` and in its URL check;
the owner creates the repository) and installs that release when its tag is newer than `coop\version.txt`. Publishing a release therefore updates every player who uses
the shortcut the next time they start the game, so a release must be finished and tested before it goes up. Nothing is published from this workspace without the owner.

What the launcher already out there relies on, so every later release keeps it (the same protocol as the Revelations 1, Jericho and TimeShift mods of this workspace):

- **Tag** `v<major>.<minor>.<patch>` (parsed as a .NET `[version]`), a full release: drafts and pre-releases are not "latest" and are never installed.
- **Asset** named `revelations2-coop-<version>.zip` (the first asset matching `revelations2-coop-*.zip`), attached to that release, so its URL is under
  `https://github.com/dolbaras/revelations2-coop/releases/download/`. The launcher checks the download against the `sha256:` digest GitHub gives for the asset
  (`"digest": "sha256:<64 hex>"` in the release JSON). The 0.1.x launchers install without a check when an asset has no digest; **from 0.2.0 on an asset without a
  digest is not installed at all**. GitHub gave a digest for every asset of this repository and of `jericho-coop` / `timeshift-coop` (read 2026-11-06), but not for an
  asset uploaded in January 2025 (ThirteenAG's Fusion Fix zip): always upload the release's files fresh, never re-attach an old asset.
- **`coop-installer.ps1` and `coop-texts.ps1` at the root of the zip** (as `tools/package.ps1` builds them), the installer taking
  `-Game <folder> -AutoClose -NoShortcut`: install into that folder with the window, close it by itself after a successful install, exit code 0 on success and
  non-zero on failure; `-NoShortcut` refreshes only the shortcuts the install record lists.
- **`files\coop\version.txt`** holding the version without the `v`.
- **`<game>\coop\install.txt`**, the install record the next installer reads: `version=`, then one line per `file=`, `backup=` and `shortcut=` (several shortcuts on one
  line are split after each `.lnk`, empty ones dropped).
- The mod's DLL carries the words `RE Revelations 2 co-op proxy` (the installer tells it from a dinput8.dll of someone else's by them): keep the log line
  (`native/proxy.c`; `tools/package.ps1` refuses a DLL without it).

A change to any of these needs a launcher that copes with both the old and the new form, shipped one release before the old form is dropped. The update from 0.1.x to
0.2.0 is made by the 0.1.x launcher (the one on the player's PC; the 0.1.1 one is the same text as `tools/launcher.ps1` of `2f105e5`): `tests\test_installer.ps1 -OldZip`
runs exactly that.

## What the launcher does when something goes wrong

The game always starts (through Steam for a Steam copy), nothing half-installed stays behind, and `coop\launcher-log.txt` says why. Each line but "no network" is a
case of `tests\test_installer.ps1` (section 4), run against a small HTTP server on 127.0.0.1:

| Case | Behaviour (log line) | Measured |
|---|---|---|
| GitHub's rate limit (60 unauthenticated requests an hour; 403 or 429 with `X-RateLimit-Remaining: 0`) | `update: GitHub's rate limit is used up (resets HH:mm) [HTTP 403]: no update check this time` | at once |
| No full release yet / missing file | `update: not found [HTTP 404] ...` | at once |
| No network, no name lookup | `update: no network (<status>): ...` | probe only: a lookup of a `.invalid` name failed inside the task after 740 ms |
| The feed does not answer | `update: the feed did not answer in 6 seconds` (`GetResponseAsync` + `Wait(6000)`: the request's own `Timeout` does not cover an asynchronous call and its synchronous form leaves the name lookup out (Microsoft's documentation); probe 2026-11-06: the call returns in 4-5 ms, a failed lookup or a dead address stays inside the task) | 7.1 s for the whole launcher run |
| The feed sends its headers and stops | `update: no network (Timeout): ...` (6 s read timeout; at most 10 s and 1 MB in all) | 7.1 s |
| A feed with a byte-order mark | read normally (a plain UTF-8 decode made `ConvertFrom-Json` fail: "Invalid JSON primitive") | |
| A pre-release or draft in the feed | `refused: the release is a pre-release or a draft`, nothing downloaded | |
| No `sha256:` digest on the asset | `refused: the release gives no usable checksum`, nothing downloaded | |
| Wrong checksum | message box (the window is up), version stays | |
| The download breaks off | `update: the download broke off (46929 of 93858 bytes)` (the stream just ends early: the length check catches it before the checksum does) | |
| The archive is missing (404) | `update: not found [HTTP 404] ...` | |
| This folder's `rerev2.exe` runs | `the game runs already`, nothing asked | |
| A second launcher (a double-clicked shortcut) | `another launcher runs` (mutex `Local\Rev2CoopLauncher`) | |
| The update | `downloaded <n> bytes from <scheme://host after the redirect>`, `checksum ok <sha256>`, `installer exit 0`; the temp folder is removed after a success and after every failure (the 0.1.x launcher left the downloaded zip in `%TEMP%\rev2-coop-update-*` after a wrong checksum) | |

## Building

```
sh native/build.sh                                      # native\build\dinput8.dll (32-bit, Zig)
OUT=build-x/dinput8.dll sh native/build.sh              # the same into native\build-x\ (another agent may use native\build)
powershell -File tools\package.ps1 -Version 0.1.0       # dist\revelations2-coop-0.1.0.zip
powershell -File tools\package.ps1 -Version 0.1.0 -Exe  # + dist\revelations2-coop-setup-0.1.0.exe (workspace tools\mkinstaller; unsigned)
powershell -File tools\package.ps1 -Version 0.1.0 -Exe -Dll native\build-x\dinput8.dll -Dist <folder>   # another DLL, another output folder
```

In a worktree (`work\wt-*`) set `ZIG=<workspace>/tools/zig/zig-x86_64-windows-0.16.0/zig.exe` and `ENET=<workspace>/tools/enet/enet-1.3.18` for `native/build.sh` (its defaults
are relative to `mod\`); `package.ps1` finds `<workspace>\tools\log-collector` and `tools\mkinstaller` from `mod\` and from a worktree (the first `tools\<name>` above the
repository). **`package.ps1` refuses a DLL whose `R2_VERSION` is not `-Version`** (the DLL holds the version as a string of its own right after the proxy's log line;
`-AnyDllVersion` for tests and dry runs of a coming version): `coop\version.txt` is what the launcher compares, the DLL's version is what the log and the lobby say.

**The release DLL is built from the exported public tree, not from `mod\`.** The lab tree carries lab-only research code that `lab/export_public.sh` strips from the
public source, and a DLL built from `mod\` holds it (the published 0.1.1 DLL does, and so does a 0.2.0 dry-run DLL from `mod\`, 2026-11-06): the published binary was not the
one the public source builds. `DLL=<file> ... sh lab/export_public.sh` keeps the public tree's build (the script refuses one that still holds the stripped code's strings) and
that file is what `package.ps1 -Dll` packages.

The zip holds `Install Co-op.bat`, `coop-installer.ps1`, `coop-texts.ps1` (the 12 languages of the game, the order and codes of `native/lobby_text.c`: en fr it de es ru pl pt-br
ja ko zh-cn zh-tw; shared by the installer and the launcher), `README-COOP.txt`, `LICENSE.txt` and `files\` (`dinput8.dll`, `coop\coop-launcher.ps1`,
`coop\coop-installer.ps1`, `coop\coop-texts.ps1`, `coop\README.txt`, `coop\version.txt`, `coop\collect-logs.ps1` + `coop\log-profiles\*.psd1` +
`coop\Collect logs.bat` (the bug-report collector, below), `coop\payload.txt` = the list of every payload file that the installer checks
before it touches the game). The package sets no environment variable and the shortcut's launcher sets none: `REV2_ACHBLOCK` (Steam unlocks blocked) is a console-run default only,
so a player's game passes every achievement call through to Steam (see `docs/achievements.md`).

## Bug reports: Collect logs.bat

`package.ps1` takes the bug-report collector every co-op mod of the workspace shares from `<workspace>\tools\log-collector` (`collect-logs.ps1` and one profile per
game, all of them, so that `-All` also takes the other co-op mods found on a PC; `-LogCollector <folder>` for a clone placed elsewhere) and writes
`coop\Collect logs.bat` (`collect-logs.ps1 -Mod revelations2`). A double-click puts `revelations2-coop-report-<date>-<time>.zip` on the desktop: `report.txt` (mod
version, SHA-256 of `rerev2.exe`, `dinput8.dll` and a chained `dinput8_real.dll`, install record summary, Windows and its UI language, graphics adapters and drivers,
memory, CPU, display modes), `logs\rev2_coop*.log` (the current run and the four before it), `coop\install.txt`, `install-log.txt`, `version.txt`, `launcher-log.txt`,
`settings\rev2_coop.ini` and the game's `config.ini` (`%LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS2`), `temp\rev2-coop-install.log`, `files.txt` (names and sizes
of `coop\` and of the game folder's top level). Cleaned out: user, computer and domain names, the profile, Documents and game paths, any `<drive>:\Users\<x>`, `name=` of the ini,
the Steam names the log quotes (own and the partner's: `you are <id> "<name>"`, `linked with`, `host`, `lobby member`, the session lines; other players become
`<player-2>`...) and Steam's account names, Steam IDs, public IPv4 / IPv6 addresses (the ini's `ip=` too; LAN kept), e-mails, MACs; nothing is uploaded. The
uninstaller removes it with `coop\`. Tests: `tests\test_installer.ps1` (the payload has it, it is installed and in the record, the installed script writes its zip,
uninstall removes it) and the shared `tools\log-collector\tests\test-collect.ps1`. (This text says `<drive>:` on purpose: the public export's privacy check refuses
the literal profile path of drive C.)

## What the installer does

- Finds the game: its own folder, the Steam uninstall key of app 287290, every Steam library (`libraryfolders.vdf` and the `appmanifest_287290.acf` installdir), common
  folders, or the player picks it. **It checks that `rerev2.exe` is the Steam build 3658995** (SHA-256 `55ac7d2b...67f9`, the same constant as `R2_EXE_SHA256` in
  `native/r2.h`) and refuses any other exe in the player's language (the mod's gate would keep it off anyway); `-AnyExe` or `REV2_COOP_ANYEXE=1` skip the check (tests only).
- Refuses while a `rerev2.exe` runs from that folder. A folder Windows protects is installed by an elevated copy of the installer (Windows asks the player).
- Keeps a `dinput8.dll` of someone else's (an ASI loader, a graphics fix) in `<game>\coop-backup`, once; its own DLL of an older version is never kept as the original. A
  `dinput8_real.dll` the player placed (the mod's chain rule) and every other file are left alone.
- **When that `dinput8.dll` is an Ultimate ASI Loader** (32-bit, exports `IsUltimateASILoader` and winmm's `timeGetTime`; the Fusion Fix ships v7.8.0, the loader's own
  release is v10.0.2) and the game folder has no `winmm.dll`, a copy of it is made as `winmm.dll`, and the loader's `dinput8.ini` (if any) as `winmm.ini`: a name the
  loader serves and `rerev2.exe` imports, so Windows loads it from the game folder at the start and it loads its plugins (`scripts\*.asi`) as before, next to the mod's
  `dinput8.dll`. The record lists the copies as `chain=` and `file=` lines (written before the copies are made); reinstall and update keep them, the uninstaller removes a
  copy only while it still equals its source and then puts the loader back as `dinput8.dll`. Not `dinput8_real.dll`: the loader knows itself only by its file name and
  under that name shows "This library isn't supported." and closes the game (its source: `proxy::Load` of v10.0.2, `LoadOriginalLibrary` of v7.8.0); the proxy never loads
  such a loader, nor a copy of the mod, as `dinput8_real.dll` (`native/chain_logic.h`, log line `chain: dinput8_real.dll is an Ultimate ASI Loader: not loaded ...`).
  Any other `dinput8.dll`, or a loader next to a `winmm.dll` of someone else's, stays off while co-op is installed; the result page says which of the two happened.
- **A `dinput8.dll` another mod put over the co-op one after the install** (the player installed an ASI mod later; it is not the mod's DLL by `DLL_MARK` while the
  record lists `dinput8.dll` as the mod's): an install or update (also the launcher's) keeps it in `coop-backup\dinput8.dll` as the original (it replaces an older
  one there: the file the player put there last is the one the removal puts back; copies made from the older one are removed and made again from it, the record is
  written at once) and puts the co-op DLL back; the uninstaller leaves it in place and does not put the older original over it (`overcoop`, `leftin`, `leftnote`
  in 12 languages). Before (0.2.0's first build, 0.1.1, Jericho 0.3.1, TimeShift 0.1.3): the update overwrote it without a copy, the uninstaller deleted it.
- Other mods' proxy DLLs (`d3d9.dll` of DXVK / ReShade, `dsound.dll` + ini, `version.dll`, `xinput1_3.dll`) and plugin folders are not in the payload: never touched.
- Copies the payload, writes `coop\install.txt` (`coop\version.txt` last: the launcher reads it, an update that broke off is tried again at the next start), the
  desktop and Start menu shortcuts (the game's own icon with a co-op badge, `coop\coop.ico`), and `Uninstall Co-op.bat` at the game folder's root.
- The shortcut starts the game through Steam (`steam://rungameid/287290`, found from the library's `appmanifest_*.acf`; a copy outside a Steam library starts `rerev2.exe`). Raid
  Mode is not separated by the launcher: the mod itself does nothing outside a co-op game started from the lobby (the lobby row is on the campaign page only).
- Removal (`Uninstall Co-op.bat`, or the installer again with "Remove co-op"): deletes what the record lists, puts the backed-up files back, removes the shortcuts, `coop`
  and `coop-backup`. The game's saves, `rev2_coop.ini` (the lobby's settings) and the co-op save (0.2.1: `rev2_coop_save_<hash>.dat` + `.rec`) in
  `%LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS2` are not touched: the player's progress survives a removal and a reinstall (the readme says so and how to delete them).
- Texts in 12 languages, chosen by Windows' language, `-Lang` or the list in the window (Windows in Portuguese from Portugal gets Brazilian Portuguese; a language the game does not
  have gets English).

## Tests

`powershell -File tests\test_installer.ps1 [-RealGame <folder with a COPY of the real rerev2.exe>] [-OldZip <released zip>] [-Online] [-Keep]` (everything in a temp folder, no game is
started; without `-Online` no network but 127.0.0.1). 210 checks without the options (2026-11-07), 8 more with `-OldZip`, 5 more with `-Online`:
- the package: the zip's layout and payload list, the log collector in it, the DLL version gate (another version refused, the same one taken);
- the installer: a fake exe is refused without a trace; install over a game with a foreign dinput8.dll (backup, record, two shortcuts, every other game file untouched, a
  `dinput8_real.dll` and a plugin folder of the player survive, the result says the foreign DLL is off); reinstall (the backup stays the foreign DLL); uninstall (the game
  folder is byte for byte the clean one); a game without dinput8.dll; a path with spaces and an apostrophe; the installed log collector writes its zip;
- an Ultimate ASI Loader as the foreign dinput8.dll (made-up PE files, never run): the copy as `winmm.dll` and `winmm.ini`, the record, the result text, reinstall + update
  keep the copies once, uninstall byte for byte; a `winmm.dll` the player changed is left in place; a loader next to a foreign `winmm.dll`, a 64-bit loader, a loader without
  `dinput8.ini`, a DLL that only names the loader as a string: no copy (or no ini), uninstall byte for byte; with `-OldZip` a 0.1.1 install over a loader, updated by its own
  launcher, gets the copy; the result text with the note fits its label in every language (measured, no window). The proxy's side: `tests/test_chain.c` (in `tests/build.sh`;
  `UAL_FILES=<dll>;<dll>` adds real loaders);
- (ual-rev2, 2026-11-08) other mods' proxies next to a loader (`d3d9`, `dsound` + ini, `version`, `xinput1_3`, `plugins\`) untouched by install + update, uninstall byte
  for byte; another mod's dinput8.dll put over co-op's after the install: the uninstall leaves it (and says so), an update keeps it in `coop-backup` as the original and
  makes the `winmm.dll` copy of it, the uninstall after that puts it back; the same over an older loader (the newer one wins); the removal's result with that note fits.
  The import hooks a loader writes back: `tests/ual_test.sh` (in `tests/build.sh`, see H2). 2026-11-08: 235/235 with `-OldZip` 0.1.1 `-Online`;
- the launcher (section 4): every case of the table above, the feeds made from `tests\fixtures\github-release-v0.1.1.json` (the real v0.1.1 entry of the releases API, author /
  uploader / body trimmed), the update through a redirect, backup and shortcuts kept; the start line (`steam://rungameid/287290` for a Steam library folder, `rerev2.exe` otherwise;
  `-NoStart`). The game-running case runs a stand-in `rerev2.exe` for a few seconds and is skipped while any `rerev2` runs on the PC (a lab instance must not see a second one);
  the second-launcher case takes the launcher's mutex for one run;
- `-OldZip work\dist-pre2\revelations2-coop-0.1.1.zip`: that release installed, ITS launcher updates it to a 0.2.0 package of this tree (record, collector files, this tree's
  launcher, backup kept), the uninstall of the result is byte for byte clean (PASS 2026-11-06);
- `-Online` (read only, plus ONE download of the published v0.1.1 zip, 460 KB): `releases/latest` of this repository (404 while only pre-releases exist, else the protocol's
  shape), of `jericho-coop` and `timeshift-coop` (full releases, digest on the zip, URL under `releases/download/<tag>/`); then this tree's launcher, fed a local copy of the real
  v0.1.1 entry marked as a full release, installs the real asset over a stand-in 0.1.0: 2026-11-06 `downloaded 459857 bytes from https://release-assets.githubusercontent.com`,
  `checksum ok 68635C9B...` (= GitHub's digest = the local `work\dist-pre2` zip), installed, uninstall clean;
- no shipped script sets or names a lab variable; the feed constants; the installer's exe hash equals the mod's own gate; with `-RealGame` a COPY of the real exe passes the hash
  check and its icon becomes the shortcut's icon (the test refuses a Steam library or any folder called `game`); the 12 texts (same keys, same placeholders, translated, no
  Revelations 1 name or build) and the language choice.

The installer's WINDOW needs a person to look at it (see "Release status"); test-only variables `REV2_COOP_UI_POS=x,y` (window at that position WITHOUT taking the focus),
`REV2_COOP_UI_SHOTS=<folder>`, `REV2_COOP_UI_LANGS=en,ru`, `REV2_COOP_UI_TAG`, `REV2_COOP_UI_AUTO=install|remove` (clicks the button, only for the folder in `REV2_COOP_UI_ONLY`;
a player never sets them). `coop-launcher.ps1 -Feed <url> -NoStart` (or `REV2_COOP_FEED=<url>` for a start through the shortcut) points the launcher at another feed; with a
feed of its own the launcher skips the download-URL check. New shortcuts go to `-ShortcutDir <folder>` in tests, never to the desktop.

## Update rehearsal before 0.2.0

The first full release is the first time the shortcut of a tester updates by itself, and it is the 0.1.x launcher that does it. What is proven without publishing anything
(2026-11-06): the 0.1.1 launcher updates to a 0.2.0 package of this tree from a local feed (`-OldZip`); this tree's launcher downloads a real GitHub asset over TLS through the
redirect and matches GitHub's digest (`-Online`); every failure case above. What only a real `releases/latest` shows: that GitHub answers it with the new release
(not cached, the right tag) and the launcher of a real install, started by its shortcut, takes it.

**A. With the owner's consent (a throw-away public repository; this creates a repository and a release, so it needs his one-line OK):**
1. `gh repo create dolbaras/rev2-update-rehearsal --public --add-readme` (any name; nothing else in it; `--add-readme` because a release needs a commit to tag).
2. Build the 0.2.0 package as for the release (checklist below, steps 1-4) but `-Version 9.9.9` from a DLL built with `R2_VERSION "9.9.9"` (a scratch build; not committed),
   and `gh release create v9.9.9 --repo dolbaras/rev2-update-rehearsal --latest --title "rehearsal" --notes "throw-away" <zip>`.
3. On the N100 (hardware rules of the lab: lock, own processes, teardown), in a COPY of the game folder with 0.1.1 installed from `work\dist-pre2`: `REV2_COOP_FEED=https://api.github.com/repos/dolbaras/rev2-update-rehearsal/releases/latest`
   for the shortcut's launcher (the URL check is skipped with a feed of its own, so the asset URL of the other repository is accepted), `-NoStart` first, then a start through
   the shortcut via the owner's scheduled task. Expect `installed 0.1.1, latest v9.9.9`, `checksum ok <the zip's SHA-256>`, `installer exit 0`, `coop\version.txt` 9.9.9.
4. Run the 9.9.9 launcher once more (`installed 9.9.9, latest v9.9.9`, nothing downloaded), uninstall, compare the folder with its snapshot, delete the release and the
   repository (`gh repo delete dolbaras/rev2-update-rehearsal --yes`).

**A was done on 2026-11-06** (the owner's consent; the repository existed from 23:52 to 23:56 and is deleted: `gh repo view` answers "Could not resolve"):
- Package: `release-020` at `769102d` with a scratch `R2_VERSION "9.9.9"` (not committed); every native suite of `tests/build.sh` green on it, `test_installer.ps1 -OldZip` 193 of 193; the export dry
  run (`DRYRUN=1 ... DLL=`) exit 0, its public-tree DLL `24a0ee3a...65acc8`; `package.ps1 -Version 9.9.9 -Exe -Dll <that DLL>`: 18 entries, `payload.txt` equals `files\`; the
  unzipped zip, the setup exe and the DLL grepped with the export's pattern: only the generic `Users\<name>` of the collector's comments. Zip SHA-256
  `3b14d5ea7e2612345e10a5917c9b0f1c09a727b606da06a4597bb63914b0da01` = the asset's GitHub digest; release `v9.9.9` full and `--latest`, `releases/latest` answered it at once.
- On the N100, in a test copy of the game folder (outside a Steam library, so the shortcut starts `rerev2.exe`; the real `rerev2.exe` build 3658995), 0.1.1 installed from
  `work\dist-pre2` (`-Quiet -ShortcutDir <test folder>`), `REV2_COOP_FEED` set for the shortcut's launcher, every start through the owner's scheduled task:
  - the 0.1.1 launcher with `-NoStart`: `installed 0.1.1, latest v9.9.9`, `checksum ok 3B14D5EA...0DA01`, `installer exit 0` (the 9.9.9 installer's window with `-AutoClose`;
    6 s for the whole run), `start: rerev2.exe (-NoStart)`; `coop\version.txt` 9.9.9, the record lists the collector's files, the shortcut is still the one in the record, the
    temp folder is gone (the 0.1.x launcher writes no `downloaded ... from` line: that one is this tree's launcher);
  - uninstall, 0.1.1 again, then the SHORTCUT: the same three lines, `start: rerev2.exe`; the game came up with the update's DLL (`RE Revelations 2 co-op proxy 9.9.9 loaded`,
    `gate: ... supported build 3658995`, first frames after 6 s, console `version` `rev2-coop 9.9.9 exe_ok=1`) and was closed;
  - the 9.9.9 launcher once more: `installed 9.9.9, latest v9.9.9`, nothing downloaded;
  - uninstall: the folder equals its snapshot (47241 entries by name, size and time, the top-level files by SHA-256) except (a) `dinput8.dll`: the test copy held a lab
    build of the mod, which the installer rightly takes for its own and does not keep (put back by hand afterwards), and (b) `rev2_coop.log` and an EMPTY `rev2_coop\` that
    the game start made (finding below); the shortcut is removed, the temp folder clean, the game's settings folder and the saves as before.
  The game start carried lab switches for the game only (console port, sound off, achievements blocked, rendering without focus); the launcher and the installer read none.
- **Finding (fixed for 0.2.0, branch `uninst-clean`):** every game start created `<game>\rev2_coop\` (`native/d3d.c` `d3d_install`: the default screenshot folder, made at
  load although a player never takes a screenshot) and `rev2_coop.log` (`.1` ... `.4` on later starts) next to `rerev2.exe`, and the uninstaller removed neither. Now the
  folders are made by the first screenshot (F11 / `shot`), and the uninstaller removes `rev2_coop.log` / `rev2_coop.N.log` and `rev2_coop\shots` + `rev2_coop\` when they
  are empty (a player's screenshots stay). `tests\test_installer.ps1` sections 6 and 7 check both cases (189 checks).

**B. Without consent:** `tests\test_installer.ps1 -OldZip work\dist-pre2\revelations2-coop-0.1.1.zip -Online` plus H2 (the 0.1.1 setup exe into the N100's Steam copy, a
start through the shortcut, the update from a local feed with `REV2_COOP_FEED`). The first real `releases/latest` update is then the publication itself: if it fails, the
fix can only go forward (a 0.2.1 that the testers install by hand), so tell the owner before publishing.

## Foreign dinput8.dll: the hardware check (H2)

Downloaded for it (`games\re-revelations-2\downloads\`, 2026-11-06; each zip unpacked into a new empty folder for the checks below):

| File | SHA-256 | Notes |
|---|---|---|
| `thirteenag-ultimate-asi-loader-v10.0.2-x86\Ultimate-ASI-Loader-NoPDB.zip` | `c8cb3676...61139` (= GitHub's digest) | release v10.0.2 of 2026-10-27 |
| its `dinput8.dll` | `f0e76c41b6d96270b33dd8c0e27822a5efb631c839ec9a6671ab449f2fca28a1` | PE x86, 2802920 bytes |
| `thirteenag-rerev2-fusionfix-release-2025-01-17\ResidentEvilRevelations2.FusionFix.zip` | `4a62d6b23a2b0228612dd32415c21b092baa3fd2b3b58a14248533716b145ca5` | WidescreenFixesPack tag `rerev2` (asset of 2025-01-17, no GitHub digest) |
| its `dinput8.dll` (an Ultimate ASI Loader build) | `61b4f6dd5983b975d87997ef3a2f530680e8fcba9136d4e9124dad91548b4b78` | PE x86, 1703936 bytes |
| its `scripts\ResidentEvilRevelations2.FusionFix.asi` / `.ini` | `f100dd3c...c1bdf` / `bbdb2f7e...795a2` | PE x86 / 369 bytes |

The loaders are Ultimate ASI Loader builds: the standalone one is v10.0.2, the Fusion Fix's is v7.8.0 (version resources; both export `IsUltimateASILoader`,
`DirectInput8Create` and winmm's functions). Their source (GitHub `ThirteenAG/Ultimate-ASI-Loader`, read 2026-11-07: v10.0.2 `source/loader/proxy/proxy.cpp` `Load()`,
v7.8.0 `source/dllmain.cpp` `LoadOriginalLibrary()`): the loader picks what it stands in for by its own file name; a name not in its list gets the message box "This
library isn't supported." and `ExitProcess(0)`, at the first patched kernel32 call of the exe. So under `dinput8_real.dll` it would close the game; it forwards to
`<its name>Hooked.dll` beside it or to the system DLL by full path, so there is no way back into the mod's `dinput8.dll` (no recursion). `winmm.dll` is in both lists and
`rerev2.exe` imports `WINMM.dll`: that is the name the installer uses (see "What the installer does"), and the proxy never loads such a loader as `dinput8_real.dll`.

**Live check on the N100 test copy, 2026-11-07** (one instance, the lab's rules, a snapshot of the folder before; runs `work\runs\n100-20261107-171430-relfinal-A` and
`-171915-relfinal-B`, evidence `work\relfinal\`):
- A: the Fusion Fix zip as shipped in the folder, then the 0.2.0 dry-run zip installed over it (`-Quiet`): `Original kept: dinput8.dll`, `Your ASI loader also runs as:
  winmm.dll`, record `chain=winmm.dll`. Game start: modules `<game>\WINMM.dll` (the loader), `<game>\scripts\ResidentEvilRevelations2.FusionFix.asi` and the mod's
  `<game>\DINPUT8.dll`; the mod's log `co-op proxy ... loaded`, `gate: ... supported build 3658995`, `CreateDevice -> hr=0x00000000`; the title in under 30 s; the
  co-op lobby opened (`menu: lobby opened`) and drawn with the game's own material; no access violation. The fix's own effect is visible: in the 1024x768 test window
  the title keeps its 16:9 picture between black bars, without the fix it is stretched to 4:3 (run B). Uninstall (exit 0, `Put back: dinput8.dll`): the folder equals its
  snapshot with the Fusion Fix in, 0 differences (47241 entries by name, size and time; the top level and `scripts\` by SHA-256).
- What the loader and the fix change: `iat(first DirectInput8Create): 26 of 32 patched import slots intact, 6 changed` (GetProcAddress, LoadLibraryW, Sleep,
  CreateWindowExA/W and SetWindowPos set back to the system functions). In a player's game the first three are log-only hooks and the window hooks are not installed
  (test instances only); in a lab instance the console's `window` answers "no game window known" and `quit` does nothing (the run's teardown stopped the process).
  This Fusion Fix has no split-screen module (its ini: SkipIntro, BorderlessWindowed, DisableDamageOverlay, DisableFilmGrain, DisableFade, FOVFactor 1.0,
  DisableCreateQuery, ForceLogo); nothing of it showed in the lobby.
- B, the guard, by hand: the mod's DLL as `dinput8.dll`, the Fusion Fix's loader renamed `dinput8_real.dll`: `chain: dinput8_real.dll is an Ultimate ASI Loader: not
  loaded (...)`, the game runs without the fix (no `.asi` among the modules, the system `WINMM.dll`, `32 of 32 patched import slots intact`), title and lobby as usual.
- Afterwards the test copy, its settings folder and the saves were byte for byte as before.

**Which of the mod's import hooks such a loader undoes (2026-11-08, ual-rev2; offline).** Ultimate ASI Loader v10.0.2 (`source/loader/startup/startup.cpp`
HookEntryPoints / RestoreSlots) patches the exe's kernel32 slots of about 30 functions in its DllMain, records the value it saw and writes every one back
unconditionally at the exe's first such call (not one from `.bind`, the Steam stub), before it loads its plugins; v7.8.0 did the same in run A above. The mod's DLL
imports winmm, so a loader as `winmm.dll` always runs first (in `rerev2.exe`'s own import table WINMM comes after DINPUT8; d3d9 comes before it). Of the mod's
import hooks (`r2_patch_iat`) the loader wraps three that `rerev2.exe` imports: `GetProcAddress`, `LoadLibraryW` (`dyn:` log lines) and `Sleep` (`diag:` log
lines), all log-only: they are lost, nothing a player uses. Every hook the co-op needs stays: `ExitProcess`, `CreateThread` / `WaitForSingleObject` /
`WaitForMultipleObjects` / `SleepEx` (diag), `d3d9!Direct3DCreate9`, the XInput glue (`XINPUT1_3` #2-#5), the eight `steam_api` hooks (the loader restores only
kernel32 / ole32 / vccorlib slots, and rewrites ordinal imports only of the DLL it stands in for). Jericho and TimeShift point the exe's own calls at their hook
when a slot was patched first (`iat_guard.c`); that is neither needed here nor possible: `rerev2.exe`'s `.text` is encrypted by the Steam stub while DllMain runs,
so the calls cannot be found then. `tests/ual_test.sh` (in `tests/build.sh`) reproduces run A's three without the game: a stand-in loader written from that source
as `winmm.dll`, a stand-in exe with the hooked imports (XInput by ordinal, a stand-in `steam_api.dll`), the mod's DLL with `REV2_ANYEXE=1`: 22/22 for this
tree's build and for the 0.2.0 dry-run DLL (`d2c69e28...`). A new product hook on a function the loader wraps fails there. Not covered: a loader the player names
`xinput1_3.dll` (it initialises after the mod and would rewrite the XInput ordinal slots, i.e. the pad glue; the installer never makes that name).

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
A release that changes any message layout bumps `CP_PROTO_VERSION` (`native/coop_proto.h`), so that an older package cannot join it silently.

## Publishing checklist (commands for the lead; nothing here is run from the lab without the owner's word)

Tools: `gh` is the portable one at `<workspace>\tools\gh\bin\gh.exe` with `$env:GH_CONFIG_DIR=<workspace>\tools\gh\config` (PowerShell); identity: the noreply address of the owner's GitHub account, set locally in `pub\`.
`<v>` is the version (`0.2.0` for the first full release), `<dist>` the output folder (`work\dist-020`).

1. Freeze: `git -C mod status --short` empty; `R2_VERSION` in `native/r2.h` is `<v>` and committed; `CP_PROTO_VERSION` bumped if a message layout changed since the last
   release; the nine suites and `tests\test_installer.ps1 -OldZip work\dist-pre2\revelations2-coop-0.1.1.zip -Online` green on that commit, and (0.2.1 on) the same with
   `-OldZip work\dist-020\revelations2-coop-0.2.0.zip` (the update from the published full release).
2. Sanitized public tree and the release DLL, dry run: `DRYRUN=1 PUB=<scratch folder> DLL=<dist>\dinput8.dll ZIG=<zig.exe> ENET=<enet folder> sh lab/export_public.sh` (exports the
   committed HEAD of `mod/` (`SRC=work/wt-<x>` for a worktree), strips the lab-only switches, drops `notes/` (the agents' resume files and lab notes), names the lab's
   folder of static-query outputs that some comments cite without its drive path, puts `lab/pub-README.md` in as `README.md`, greps for private paths, names, account ids,
   any e-mail address and any "RC" wording, builds, refuses a build that still holds the stripped code, keeps the DLL).
3. `powershell -File mod\tools\package.ps1 -Version <v> -Exe -Dll <dist>\dinput8.dll -Dist <dist>` (refuses the DLL if its `R2_VERSION` is not `<v>`). Record the SHA-256 of
   the zip, the setup exe and the DLL. Unzip the zip into a scratch folder and grep it with the export's pattern (only the generic `Users\<name>` of the collector's comments may match).
4. Install the zip (setup exe) on BOTH PCs for the acceptance runs; the acceptance itself is outside this file (`notes\release-0.2.0-plan.md` section 5).
5. The real sync: `sh lab/export_public.sh` (working tree in `pub\`), review `git -C pub diff --stat`; `git -C pub add -A`, `git -C pub commit -m "Revelations 2 co-op <v>"`
   (with the Co-Authored-By trailer), then `git -C pub -c credential.helper= -c "credential.helper=!'<gh.exe>' auth git-credential" push origin main`.
6. Release (the tag is made by the command, on the pushed main):
   - pre-release: `gh release create v<v> --repo dolbaras/revelations2-coop --target main --prerelease --title "Pre-release N (testing)" --notes-file <notes> <dist>\revelations2-coop-<v>.zip <dist>\revelations2-coop-setup-<v>.exe` (never `--latest`, never `--draft`);
   - full release (0.2.0 on): the same with `--latest` instead of `--prerelease`, title "Resident Evil Revelations 2 Co-op <v>", only after the acceptance and the owner's explicit word to publish.
7. Check: `gh release view v<v> --repo dolbaras/revelations2-coop --json isPrerelease,assets` (the asset `digest` equals the local SHA-256 of the zip and of the exe); for a pre-release
   `https://api.github.com/repos/dolbaras/revelations2-coop/releases/latest` answers 404; for a full release it answers the new tag and `tests\test_installer.ps1 -Online` passes its
   shape check against it. Then one launcher run on a copy with the previous version installed (`-NoStart`): `installed 0.1.1, latest v<v>`, `checksum ok`.
8. A tester's side: the setup exe or the zip from the release page on BOTH PCs (pre-release), or simply the shortcut (full release: it updates by itself at the next start).

## Release status

Public packages so far: **pre-release 1 (0.1.0)** and **pre-release 2 (0.1.1)** (GitHub pre-releases), the first full release **0.2.0** (tag `v0.2.0`, `--latest`,
published 2026-11-09 from the public commit `f15ce38`; zip `ce157026...`, setup exe `cb29cff4...`; the 0.1.1 launcher updated a copy to it through the real feed) and
**0.2.1** (tag `v0.2.1`, `--latest`, published 2026-11-10 from the public commit `8f43c08`; zip `3f230f1f...`, setup exe `053bb404...`).

0.2.1: a co-op game keeps its own save (`notes\coopsave.md` in the lab repo): the game's save calls (ISteamRemoteStorage, "DATA0.DAT") go to
`%LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS2\rev2_coop_save_<hash>.dat` while a co-op game runs, the solo save in memory comes back at the title, and the room's
Continue starts from the co-op checkpoint (the guest follows the host's record, session events 0x180). No message layout changed (new session flag bit 8, player flag
bits 32 / 64, a new event kind): `CP_PROTO_VERSION` stays 2; both PCs need 0.2.1 anyway (the package version gate).

Verified offline (no game started, no window shown):
- `tests\test_installer.ps1`: 188 of 188 (2026-11-06), 193 with `-OldZip` (the 0.1.1 launcher to 0.2.0), 193 with `-Online`.
- The package builds from a DLL of the mod's HEAD (`tools\package.ps1 -Dll`), zip and setup exe; the setup exe's own self test (payload equals the zip) passes. A 0.2.0 dry run
  (`work\dist-020dry`, DLL of the branch with `-AnyDllVersion`, 2026-11-06): 18 entries, `payload.txt` equals `files\`, no private path, name or account string in any file (the only
  matches of the export's pattern were the lab-only strings of the lab-tree DLL, see "Building"); the export dry run of the branch found the literal profile path of drive C
  in this file (the collector paragraph; now `<drive>:`).
- A second 0.2.0 dry run as the checklist says (branch `release-final`, 2026-11-07): the export dry run (exit 0; it now also drops `notes/` and refuses any e-mail address),
  the DLL built from the exported tree (`OUT=build-pub`, SHA-256 `b76e3234f18284a32eab62f9fb06fad5b8499ca219abbf0fc42fd6b219dc240a`, `R2_VERSION` still 0.1.1), the
  exported tree's `tests/build.sh` and every test of it green, its `test_installer.ps1` 210 of 210; `package.ps1 -Version 0.2.0 -Exe -AnyDllVersion` into
  `work\dist-020dry2`: zip `43e82015...b883`, setup exe `80d60d9f...93d1`, 18 entries; `test_installer.ps1 -OldZip` with that zip 218 of 218; the unzipped zip, the exe and the
  DLL hold no e-mail address and no private path or name (only the collector's generic `Users\<name>` comments).

Verified live on 2026-11-05 (window on the second monitor without focus, a throw-away copy `work\f6n\RE2 throwaway` with a COPY of the real `rerev2.exe` (hash check without `-AnyExe`) and a foreign `dinput8.dll` stand-in, shortcuts into a test folder, never the desktop):
- The installer WINDOW (start page in en / ru / ja / ko, progress page, result page `installed 0.1.0, 7 files, backup of dinput8.dll, shortcut`), the install button, and "Remove co-op": the copy was byte for byte as before afterwards (the foreign DLL back, no `coop`, `coop-backup`, shortcut or `Uninstall Co-op.bat`). Pictures `work\s6n\ev\installer_ui\`.
- The zip's `Install Co-op.bat` and `Uninstall Co-op.bat`, and the setup exe (`revelations2-coop-setup-0.1.0.exe`, started from the copy's folder): the same install / uninstall result each time.
- The shortcut the installer made: powershell -> `coop\coop-launcher.ps1`, icon `coop\coop.ico`; the launcher with `-NoStart` ran (exit 0; the GitHub repository did not exist yet, so there was nothing to update).

Verified against the real GitHub on 2026-11-06 (read only + one download): the download path of the launcher (TLS 1.2, github.com's redirect to `release-assets.githubusercontent.com`,
GitHub's digest) with the published v0.1.1 zip (`-Online`).

Verified through a real `releases/latest` on 2026-11-06 (rehearsal A, a throw-away repository, deleted): the 0.1.1 launcher of a real install on the N100, started by its
shortcut, updated to a full release (`installed 0.1.1, latest v9.9.9`, `checksum ok`, `installer exit 0`) and started the game with the new DLL; see "Update rehearsal".

Verified live on 2026-11-07 (N100 test copy, see "Foreign dinput8.dll"): the Fusion Fix of 2025-01-17 next to the mod (its loader run on as `winmm.dll` by the 0.2.0
dry-run installer, FusionFix.asi loaded, title and co-op lobby, uninstall byte for byte), and the proxy refusing that loader as `dinput8_real.dll`.

Still not checked live: the shortcut starting the game through Steam (deliberately not started: `steam://rungameid/287290` would start the owner's real install), the
standalone Ultimate ASI Loader v10.0.2 next to the mod (its source and export table were read, the chain test judges it), the folder picker's dialog, the taskbar progress.
The wording of the known limits in `docs/release-readme.txt` is the one of pre-release 2; the 0.2.0 text is written from the acceptance results (draft outside the repository).
  Real Steam unlocks were allowed by the owner for the lab accounts and one shared record (the flame achievement) was unlocked and read back on both accounts.
