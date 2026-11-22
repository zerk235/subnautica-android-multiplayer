# Subnautica Android Multiplayer

Experimental LAN co-op mod for **Subnautica on Android** by [zerk235](https://github.com/zerk235).

**[Download APK for Android · 0.3.13-installer-fix-test](https://github.com/zerk235/subnautica-android-multiplayer/releases/download/v0.3.13-installer-fix-test/snmp-installer-fix-test.apk)** · [Release notes and SHA‑256](https://github.com/zerk235/subnautica-android-multiplayer/releases/tag/v0.3.13-installer-fix-test)

**Pre-release for testing.** This build includes an in-game lobby and fixes for Shizuku access, world restoration and diagnostics.

[Русский README](README.md) · [Installation guide (Russian)](docs/INSTALLER.md) · [How to join (Russian)](docs/PLAYING.md)

## Download and install

1. Download the APK above and install SN Multiplayer Installer. If the signing certificate matches, update the app in place to keep your local signing key and backups.
2. Save your world and return to the game menu. In the installer, select **“Подготовить игру” (Prepare game)** and then **“Установить подготовленный мод” (Install prepared mod)**.
3. Repeat on both phones. Joining requires matching mod builds on protocol **45**.

[Previous demo on protocol 37](https://github.com/zerk235/subnautica-android-multiplayer/releases/tag/v0.3.13-installer-preview)

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
- In-game lobby with room selection and ready status. Either phone can host without root.
- Host discovery, full supported saved-world transfer including `CellsCache`, and guest autoload from the main menu. No PC is needed during a join.
- Remote player rendering, head visibility, indoor lighting and Builder grip fixes.
- Live construction/deconstruction of supported base pieces attached to an existing shared base.

Tested game version: **1.22.83315**, Android **arm64-v8a**. Both phones need matching game and mod builds. Other game versions and larger sessions are unverified.

Connect both phones to the same Wi-Fi network. Open **“Совместная игра” (Multiplayer)** in the game menu. The host creates a room and the guest selects it. When both players are ready, the host opens and saves a world. The guest waits in the lobby for the transfer and automatic load.

Tested base pieces and floor Lockers are listed in the compatibility guide. Sync for selected fish and Stalkers is experimental. Full ecosystem, inventory, vehicle and animation sync remain limited.

See the Russian [play guide](docs/PLAYING.md), [compatibility matrix](docs/COMPATIBILITY.md), [build instructions](docs/BUILDING.md) and [demo roadmap](docs/ROADMAP.md).

The developer patch tool creates an **unsigned** APK from a local game APK; it does not sign or install it. Signing and split-package compatibility must be handled separately. Do not uninstall the game to resolve a signing mismatch without first backing up your saves.

The Android installer generates a local non-exportable signing key, prepares a signed package on the phone and blocks replacement when the installed game's certificate differs. It never uninstalls the game automatically. Keep the installer and its app data to retain the signing key for future updates.

No open-source license has been selected for project-owned code; see [LICENSE.md](LICENSE.md). Third-party components retain their own licenses. This unofficial project is not affiliated with the game's developers.
