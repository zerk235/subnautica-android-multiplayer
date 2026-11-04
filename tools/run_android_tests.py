#!/usr/bin/env python3
"""Compile tests with NDK and run in /data/local/tmp, without restarting the game."""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", default=os.environ.get("ANDROID_NDK_HOME")
                        or os.environ.get("ANDROID_NDK_ROOT"))
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--test", action="append", help="Test stem; repeat, or omit for all")
    args = parser.parse_args()
    if not args.ndk:
        parser.error("Set ANDROID_NDK_HOME or pass --ndk")
    host = {"Windows": "windows-x86_64", "Linux": "linux-x86_64",
            "Darwin": "darwin-x86_64"}[platform.system()]
    suffix = ".cmd" if os.name == "nt" else ""
    compiler = (Path(args.ndk) / "toolchains/llvm/prebuilt" / host / "bin"
                / ("aarch64-linux-android29-clang++" + suffix))
    adb = shutil.which(args.adb)
    if not compiler.is_file() or not adb:
        parser.error("NDK ARM64 compiler or ADB not found")
    tests = {p.stem: p for p in sorted((ROOT / "tests").glob("test_*.cpp"))}
    requested = args.test or list(tests)
    unknown = set(requested) - tests.keys()
    if unknown:
        parser.error("Unknown tests: " + ", ".join(sorted(unknown)))
    build = ROOT / "build/android-tests"
    build.mkdir(parents=True, exist_ok=True)
    for name in requested:
        exe = build / name
        subprocess.run([str(compiler), "-std=c++17", "-static-libstdc++", "-O1",
                        "-pthread", "-I", str(ROOT), str(tests[name]), "-o", str(exe)],
                       check=True)
        remote = "/data/local/tmp/snmod_repo_" + name
        subprocess.run([adb, "-s", args.serial, "push", str(exe), remote], check=True)
        subprocess.run([adb, "-s", args.serial, "shell", "chmod", "755", remote], check=True)
        subprocess.run([adb, "-s", args.serial, "shell", remote], check=True)
        print(f"PASS {name}", flush=True)


if __name__ == "__main__":
    main()
