# Subnautica Android Multiplayer

Experimental LAN co-op mod for **Subnautica on Android** by [zerk235](https://github.com/zerk235).

[Русский README](README.md)

This is a public preview demo. The Android installer passed a clean game installation, Unity launch and a two-phone LAN session. Version 0.3 adds explicit host/guest selection and retains verified world backups and restore through separately authorized Shizuku. This repository contains mod/installer source and tests, not the game or a prepatched game APK. See the Russian [installer guide](docs/INSTALLER.md).

## Download and install

**[Download installer 0.3.2 — APK](https://github.com/zerk235/subnautica-android-multiplayer/releases/download/v0.3.2-installer-preview/snmp-installer-preview.apk)** · [Release and SHA256](https://github.com/zerk235/subnautica-android-multiplayer/releases/tag/v0.3.2-installer-preview)

**0.3.2 fixes base demolition and repeat joining.** Two complete build/dismantle
cycles of a new standalone straight corridor passed on the two test phones.
Signing-key replacement is checked for Android clone conflicts before preparation;
the installer can re-register an existing game in the current profile.
Update the installer over the existing app, keep its data, save and exit to the
game menu, then **Prepare mod → Install prepared mod on both phones**. The native
mod changed; protocol remains 25, and both players need matching mod builds.
Do not manually uninstall the game to bypass a clone warning.
[Release notes (Russian)](docs/releases/0.3.2-installer-preview.md).
Diagnostics from 0.3.1 remain available: [report guide (Russian)](docs/DIAGNOSTICS.md).

**November 5 update:** the installer now has **“Запустить хостом” (Launch as host)**
and **“Подключиться гостем” (Join as guest)**. A non-root phone can host a previously
downloaded world. Autoload after switching roles retains the former host's original save.
When upgrading from 0.2, update SN Multiplayer Installer over the existing app,
save your game and return to its main menu, then run **Prepare mod → Install
prepared mod** on both phones. Keep the installer's data: it holds your update key.
Both players must update the mod: protocol **25** is incompatible with the previous
public protocol 23 build. [Release notes](docs/releases/0.3.0-installer-preview.md).

Requires Android 10+, ARM64 and your own installed Subnautica **1.22.83315** with
its resources. The installer runs on the phone without a PC or root and does not
contain or download the game.

**“Подготовить мод” (Prepare mod)** copies the installed game APKs into the
installer's own storage, patches, signs and verifies them. It does not install
the result or uninstall the game. For a first signing-key change, when Shizuku
access is already authorized, preparation also creates a verified world backup.
Without Shizuku, APK preparation still works but world backup is unavailable.
Private game preferences under `/data/user/...` are not included.

For a first signing-key change: connect Shizuku, create a world backup, prepare
the mod, use **“Удалить старую игру” (Remove old game)**, confirm Android's prompt,
install the prepared mod, then **“Восстановить миры” (Restore worlds)**. The remove
button checks the APKs, backup integrity and current saves before opening the
system prompt. Archives can also be exported and imported through the system picker.
See the [backup and Shizuku guide](docs/INSTALLER_BACKUPS.md). The app includes
Shizuku help and the verified GT8 workaround for the limited-ADB warning.

**Keep SN Multiplayer Installer and its app data**: they hold the prepared files
and the local key needed for future updates. Full steps are in the
[installer guide](docs/INSTALLER.md).

## Current scope

- Two Android phones on the same Wi-Fi network or hotspot.
- Explicit host/guest buttons in the installer; no root or Shizuku is needed to choose a role. The role persists between launches and changes only in the main menu.
- Host discovery, full supported saved-world transfer including `CellsCache`, and guest autoload from the main menu. No PC is needed during a join.
- Remote player rendering, head visibility, indoor lighting and Builder grip fixes.
- Live construction/deconstruction of supported base pieces attached to an existing shared base.

Tested game version: **1.22.83315**, Android **arm64-v8a**. Both phones need matching game and mod builds. Other game versions and larger sessions are unverified.

Choose **Launch as host** on one phone, load and save a world, then choose **Join as guest** on the other and leave it in the main menu. Save and return to the main menu on both phones before switching roles. For the initial demo test, build and save the initial base before connecting the guest. Standalone new bases remain experimental: two straight-corridor build/remove cycles passed in 0.3.2; final-foundation and all room variants still need live testing after this fix. Fish, complete ecosystem simulation, all held items and all animations are not synchronized. Remote flashlight on/off was tested; Seaglide placement still needs work. Internet matchmaking is not implemented.

See the Russian [play guide](docs/PLAYING.md), [compatibility matrix](docs/COMPATIBILITY.md), [build instructions](docs/BUILDING.md) and [demo roadmap](docs/ROADMAP.md).

The developer patch tool creates an **unsigned** APK from a local game APK; it does not sign or install it. Signing and split-package compatibility must be handled separately. Do not uninstall the game to resolve a signing mismatch without first backing up your saves.

The Android installer generates a local non-exportable signing key, prepares a signed package on the phone and blocks replacement when the installed game's certificate differs. It never uninstalls the game automatically. Keep the installer and its app data to retain the signing key for future updates.

No open-source license has been selected for project-owned code; see [LICENSE.md](LICENSE.md). Third-party components retain their own licenses. This unofficial project is not affiliated with the game's developers.
