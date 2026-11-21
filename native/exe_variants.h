/*
 * The one table of the rerev2.exe builds this mod knows (0.3.3). Two readers, nothing copied:
 *   - the proxy includes it (native/gate_logic.h builds its array from these lines, native/proxy.c asks it);
 *   - the installer (tools/installer.ps1) reads the very same lines with a regular expression; tools/package.ps1 puts this file into the zip
 *     as exe-variants.h next to the installer (the installer finds it in its own folder, or as ..\native\exe_variants.h in the source tree).
 * Keep one R2_VARIANT(...) per line, exactly in this form, SHA-256 in lower case:
 *   R2_VARIANT(sha256, name, kind, level, caveat)
 *   kind    "steam"      the build the mod was made on (Steam build 3658995)
 *           "unwrapped"  the same build with the Steam wrapper removed from the exe (same code at the same addresses)
 *           "other"      any other known build
 *   level   2 = the build the mod was made and fully tested on, 1 = a build the mod runs on (hooks on): its code was compared / tested as far as the notes say; no level 0: a build that must stay off is not in the table
 *   caveat  1 = the player is told: a licensed copy is recommended, stable work on other versions is not guaranteed
 * An exe that is not in the table: the hooks stay off unless the installer recorded the player's consent for exactly that SHA-256 (coop\anyexe.txt).
 * Adding a build here is a decision with evidence in notes\compat-*.md (the code compared at every address the mod touches), never a guess.
 */
R2_VARIANT("55ac7d2bd80704c6a8a1fa7bd2371726adbaa01ae0fda056198fb042404367f9", "Steam build 3658995", "steam", 2, 0)
R2_VARIANT("4c26ba8c7736db96c2c2fc82ac96e8a1a942ae11a4be75e70ec3ef1547bb4133", "Steam build 3658995, Steam wrapper removed", "unwrapped", 1, 1)
