#!/usr/bin/env python3
"""Build the native Android demo. Never signs, installs or modifies a game APK."""
import argparse
import hashlib
import os
import re
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "modloader"


def session_protocol(root=SOURCE):
    """Protocol for the geometry-capture demo configured by this builder."""
    header = (root / "src/net.h").read_text(encoding="utf-8")
    found = re.search(r"#if defined\(SNMOD_BASE_GEOMETRY_CAPTURE\)\s+"
                      r"constexpr uint16_t kSessionProtocolVersion = (\d+);", header)
    if not found or not 1 <= int(found.group(1)) <= 65535:
        raise ValueError("Cannot identify the demo session protocol in net.h")
    return int(found.group(1))


def source_build_id(root=SOURCE):
    """Hash tracked native inputs; normalize text so Windows/Linux agree."""
    digest = hashlib.sha1()
    digest.update(b"snmod-public-demo:geometry-capture+apply:arm64:api29:v1\0")
    inputs = [root / "CMakeLists.txt"]
    inputs += [p for folder in ("src", "libs") for p in (root / folder).rglob("*")
               if p.is_file()]
    for path in sorted(inputs, key=lambda p: p.relative_to(root).as_posix()):
        digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        data = path.read_bytes().replace(b"\r\n", b"\n")
        digest.update(len(data).to_bytes(8, "little"))
        digest.update(data)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", default=os.environ.get("ANDROID_NDK_HOME")
                        or os.environ.get("ANDROID_NDK_ROOT"))
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "android")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--print-build-id", action="store_true")
    args = parser.parse_args()
    identity = source_build_id()
    if args.print_build_id:
        print(identity)
        return
    if not args.ndk:
        parser.error("Set ANDROID_NDK_HOME or pass --ndk (tested: NDK 26.1.10909125)")
    toolchain = Path(args.ndk).resolve() / "build/cmake/android.toolchain.cmake"
    if not toolchain.is_file():
        parser.error(f"Android toolchain not found: {toolchain}")
    cmake, ninja = shutil.which(args.cmake), shutil.which(args.ninja)
    if not cmake or not ninja:
        parser.error("CMake and Ninja must be on PATH or supplied with --cmake / --ninja")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    build = args.build_dir.resolve()
    if build == SOURCE or SOURCE in build.parents:
        parser.error("Use a build directory outside modloader/ to keep source inputs clean")
    subprocess.run([
        cmake, "-S", str(SOURCE), "-B", str(build), "-G", "Ninja",
        f"-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}",
        "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-29",
        "-DCMAKE_BUILD_TYPE=Release", f"-DSNMOD_GIT_HASH={identity}",
        "-DCMAKE_CXX_FLAGS=-DSNMOD_BASE_GEOMETRY_CAPTURE -DSNMOD_BASE_GEOMETRY_APPLY",
    ], check=True)
    subprocess.run([cmake, "--build", str(build), "--target", "snmod",
                    "--parallel", str(args.jobs)], check=True)
    library = build / "out/libsnmod.so"
    print(f"Build ID: {identity}\nLibrary: {library}")
    print(f"SHA256: {hashlib.sha256(library.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
