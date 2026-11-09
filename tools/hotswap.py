#!/usr/bin/env python3
"""Fast iteration loop for the native mod on the rooted phone.

Installing a patched APK takes minutes and needs the on-device installer for the
root phone (its game is signed with a key that cannot be exported). For iterating on
the mod itself none of that is necessary: the game loads libmain.so from its own
extracted lib directory, and on the rooted phone that directory is writable by root.

So: build, copy the fresh library over the installed one, restart the game, pull the
new log lines. Nothing is resigned, nothing is reinstalled, and the split APK is never
touched.

Usage:
    py -3 tools/hotswap.py --serial 4f7aa996
    py -3 tools/hotswap.py --serial 4f7aa996 --no-build --log-tail 200
"""
import argparse
import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT") or "")
ADB = SDK / "platform-tools" / "adb.exe"
GAME = "com.unknownworlds.subnautica"
LOG = "/sdcard/Android/data/" + GAME + "/files/mod_log/logs/modloader.log"
BUILD_DIR = ROOT / "build" / "hotswap"
LIB_LOCAL = BUILD_DIR / "out" / "libsnmod.so"


def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def adb(serial, *args, timeout=180):
    return subprocess.check_output([str(ADB), "-s", serial, *map(str, args)], timeout=timeout)


def su(serial, command, timeout=180):
    return adb(serial, "exec-out", "su", "-c", command, timeout=timeout)


def installed_lib_dir(serial):
    """Directory that holds the extracted arm64 libraries of the installed game."""
    paths = adb(serial, "shell", "pm", "path", "--user", "0", GAME).decode().splitlines()
    base = next((line[len("package:"):].strip() for line in paths
                 if line.startswith("package:") and line.strip().endswith("/base.apk")), None)
    if not base:
        raise RuntimeError("game base.apk not found on the device")
    return base[: -len("/base.apk")] + "/lib/arm64"


def build(ndk, cmake, ninja):
    run([sys.executable, str(ROOT / "tools" / "build.py"), "--ndk", ndk, "--cmake", cmake,
         "--ninja", ninja, "--build-dir", str(BUILD_DIR)])
    if not LIB_LOCAL.is_file():
        raise RuntimeError("build produced no library")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--ndk", default=os.environ.get("ANDROID_NDK_HOME")
                        or str(SDK / "ndk" / "26.1.10909125"))
    parser.add_argument("--cmake", default=str(SDK / "cmake" / "3.22.1" / "bin" / "cmake.exe"))
    parser.add_argument("--ninja", default=str(SDK / "cmake" / "3.22.1" / "bin" / "ninja.exe"))
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-restart", action="store_true")
    parser.add_argument("--log-tail", type=int, default=40)
    args = parser.parse_args()

    if not ADB.is_file():
        parser.error(f"adb not found at {ADB}")
    serial = args.serial

    if not args.no_build:
        started = time.time()
        build(args.ndk, args.cmake, args.ninja)
        print(f"build: {time.time() - started:.1f}s", flush=True)

    directory = installed_lib_dir(serial)
    remote = directory + "/libmain.so"
    size = LIB_LOCAL.stat().st_size

    adb(serial, "shell", "am", "force-stop", GAME)
    adb(serial, "push", str(LIB_LOCAL), "/data/local/tmp/libsnmod.so")
    # Keep the original bootstrap next to it so the game can still be restored from the
    # installer; only libmain.so (the wrapper this mod replaces) is overwritten.
    su(serial, f"cp /data/local/tmp/libsnmod.so {remote} && chmod 755 {remote} && "
               f"rm -f /data/local/tmp/libsnmod.so")
    remote_hash = su(serial, f"sha256sum {remote}").decode().split()[0]
    local_hash = hashlib.sha256(LIB_LOCAL.read_bytes()).hexdigest()
    if remote_hash != local_hash:
        raise RuntimeError(f"library hash mismatch remote={remote_hash} local={local_hash}")
    print(f"installed {remote} sha256={remote_hash[:16]}… ({size} bytes)", flush=True)

    if args.no_restart:
        return
    adb(serial, "shell", "am", "start", "-n", GAME + "/com.unity3d.player.UnityPlayerActivity")
    print("game started; loading the world takes the usual time", flush=True)
    time.sleep(12)
    tail = su(serial, f"tail -n {args.log_tail} {LOG}").decode("utf-8", "replace")
    print(tail)


if __name__ == "__main__":
    main()
