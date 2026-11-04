# Third-party components

## ShadowHook

Vendored native sources: `modloader/libs/shadowhook/shadowhook/src/main/cpp`.
The top-level MIT license and upstream README are preserved under
`modloader/libs/shadowhook/`.

Upstream: https://github.com/bytedance/android-inline-hook

ShadowHook also contains bundled third-party sources (including xdl, LSS and BSD
helpers). Their license files and source notices are retained in the vendored
tree. Those licenses apply to their respective files independently of this
project's licensing status.

This export preserves the dependency files used by the working mod. It does not
claim a verified upstream release tag for that snapshot.

## Android apksig

The Android installer uses the `com.android.apksig` library classes from Android
SDK Build Tools 35.0.0, under the Apache License 2.0 and applicable notices.
The build script extracts these classes locally; no SDK JAR is checked into Git.
SDK notices and the native dependency licenses are included in the built APK's
assets. Upstream: https://android.googlesource.com/platform/tools/apksig/

The Android runtime compatibility of this library is tested on actual devices;
upstream primarily describes it as a tool for use outside Android devices.

## Shizuku API

Android installer backup/restore uses `dev.rikka.shizuku` API, provider, shared
and AIDL modules, version 13.1.5, under the MIT License. The complete notice is
preserved in `installer/licenses/Shizuku-API-LICENSE.txt` and packaged in the APK.
Pinned Maven Central artifact hashes are in `tools/fetch_installer_dependencies.py`.
The Shizuku app itself is not bundled; users install and authorize it separately.
Upstream: https://github.com/RikkaApps/Shizuku-API
