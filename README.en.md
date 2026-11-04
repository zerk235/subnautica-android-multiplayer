# Subnautica Android Multiplayer

Experimental LAN co-op mod for **Subnautica on Android** by [zerk235](https://github.com/zerk235).

[Русский README](README.md)

The project is being prepared for its first public demo. The preview Android installer passed a clean game installation, Unity launch and a two-phone LAN session. It does not automatically back up worlds before a signing-key change. This repository contains mod/installer source and tests, not the game or a prepatched game APK. See the Russian [installer guide](docs/INSTALLER.md).

## Current scope

- Two Android phones on the same Wi-Fi network or hotspot.
- Host discovery, full supported saved-world transfer including `CellsCache`, and guest autoload from the main menu. No PC is needed during a join.
- Remote player rendering, head visibility, indoor lighting and Builder grip fixes.
- Live construction/deconstruction of supported base pieces attached to an existing shared base.

Tested game version: **1.22.83315**, Android **arm64-v8a**. Both phones need matching game and mod builds. Other game versions and larger sessions are unverified.

The host must save the world before the guest joins. For the initial demo test, build and save the initial base before connecting the guest. Creating the first standalone base after joining is not supported yet. Fish, complete ecosystem simulation, all held items and all animations are not synchronized. Remote flashlight behavior and Seaglide placement still need work. Internet matchmaking is not implemented.

See the Russian [play guide](docs/PLAYING.md), [compatibility matrix](docs/COMPATIBILITY.md), [build instructions](docs/BUILDING.md) and [demo roadmap](docs/ROADMAP.md).

The developer patch tool creates an **unsigned** APK from a local game APK; it does not sign or install it. Signing and split-package compatibility must be handled separately. Do not uninstall the game to resolve a signing mismatch without first backing up your saves.

The Android installer generates a local non-exportable signing key, prepares a signed package on the phone and blocks replacement when the installed game's certificate differs. It never uninstalls the game automatically. Keep the installer and its app data to retain the signing key for future updates.

No open-source license has been selected for project-owned code; see [LICENSE.md](LICENSE.md). Third-party components retain their own licenses. This unofficial project is not affiliated with the game's developers.
