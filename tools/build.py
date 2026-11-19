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
    digest.update(b"snmod-public-demo:geometry-capture+apply:lobby:arm64:api29:v2\0")
    inputs = [root / "CMakeLists.txt"]
    inputs += [p for folder in ("src", "libs") for p in (root / folder).rglob("*")
               if p.is_file()]
    for path in sorted(inputs, key=lambda p: p.relative_to(root).as_posix()):
        digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        data = path.read_bytes().replace(b"\r\n", b"\n")
        digest.update(len(data).to_bytes(8, "little"))
        digest.update(data)
    return digest.hexdigest()


def build_lobby(sdk, java_home, build):
    suffix = ".exe" if os.name == "nt" else ""
    javac = java_home / "bin" / ("javac" + suffix)
    java = java_home / "bin" / ("java" + suffix)
    android = sdk / "platforms/android-29/android.jar"
    d8 = sdk / "build-tools/35.0.0/lib/d8.jar"
    for path in (javac, java, android, d8):
        if not path.is_file():
            raise FileNotFoundError(f"Missing Android lobby build dependency: {path}")
    output = build / "lobby"
    classes = output / "classes"
    classes.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(javac), "-encoding", "UTF-8", "--release", "8", "-classpath", str(android),
                    "-d", str(classes), str(SOURCE / "src/LobbyPanel.java")], check=True)
    subprocess.run([str(java), "-cp", str(d8), "com.android.tools.r8.D8",
                    "--min-api", "29", "--lib", str(android), "--output", str(output),
                    *map(str, sorted(classes.rglob("*.class")))], check=True)
    dex = (output / "classes.dex").read_bytes()
    rows = [", ".join(f"0x{byte:02x}" for byte in dex[i:i + 16]) for i in range(0, len(dex), 16)]
    header = "#pragma once\nconstexpr unsigned char snmp_lobby_dex[] = {\n    " + ",\n    ".join(rows) + ",\n};\n"
    generated = output / "lobby_dex.h"
    if not generated.is_file() or generated.read_text(encoding="ascii") != header:
        generated.write_text(header, encoding="ascii", newline="\n")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", default=os.environ.get("ANDROID_NDK_HOME")
                        or os.environ.get("ANDROID_NDK_ROOT"))
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--sdk", type=Path, default=os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT"))
    parser.add_argument("--java-home", type=Path, default=os.environ.get("JAVA_HOME"))
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
    if not args.sdk or not args.java_home:
        parser.error("Set ANDROID_HOME and JAVA_HOME (JDK 17), or pass --sdk and --java-home")
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
    lobby = build_lobby(args.sdk.resolve(), args.java_home.resolve(), build)
    subprocess.run([
        cmake, "-S", str(SOURCE), "-B", str(build), "-G", "Ninja",
        f"-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}",
        "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-29",
        "-DCMAKE_BUILD_TYPE=Release", f"-DSNMOD_GIT_HASH={identity}",
        f"-DSNMOD_LOBBY_INCLUDE={lobby.as_posix()}",
        "-DCMAKE_CXX_FLAGS=-DSNMOD_BASE_GEOMETRY_CAPTURE -DSNMOD_BASE_GEOMETRY_APPLY",
    ], check=True)
    subprocess.run([cmake, "--build", str(build), "--target", "snmod",
                    "--parallel", str(args.jobs)], check=True)
    library = build / "out/libsnmod.so"
    print(f"Build ID: {identity}\nLibrary: {library}")
    print(f"SHA256: {hashlib.sha256(library.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
