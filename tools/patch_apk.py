#!/usr/bin/env python3
"""Create an unsigned local APK; never signs, installs or overwrites an input."""
import argparse
from pathlib import Path
import zipfile

MAIN = "lib/arm64-v8a/libmain.so"
ORIGINAL = "lib/arm64-v8a/libmain_orig.so"


def require_arm64_library(data):
    # ELF64, little endian, ET_DYN, EM_AARCH64.
    if (len(data) < 64 or data[:6] != b"\x7fELF\x02\x01"
            or int.from_bytes(data[16:18], "little") != 3
            or int.from_bytes(data[18:20], "little") != 183):
        raise ValueError("Expected an Android arm64 ELF shared library")


def patch(base, library, output):
    base, library, output = (Path(p).resolve() for p in (base, library, output))
    if output in (base, library) or output.exists():
        raise ValueError("Output must be a new file, separate from the inputs")
    mod = library.read_bytes()
    require_arm64_library(mod)
    with zipfile.ZipFile(base) as src:
        names = src.namelist()
        if len(names) != len(set(names)):
            raise ValueError("APK has duplicate entries")
        if MAIN not in names:
            raise ValueError("APK does not contain an arm64 libmain.so")
        original = src.read(ORIGINAL if ORIGINAL in names else MAIN)
        require_arm64_library(original)
        if b"libmain_orig" in original:
            raise ValueError("Cannot recover the original Unity bootstrap; use an original APK")
        output.parent.mkdir(parents=True, exist_ok=True)
        created = False
        try:
            # Exclusive create prevents accidental overwrites even after the initial check.
            with output.open("xb") as target:
                created = True
                with zipfile.ZipFile(target, "w") as dst:
                    for item in src.infolist():
                        if item.filename.startswith("META-INF/") or item.filename in (MAIN, ORIGINAL):
                            continue
                        dst.writestr(item, src.read(item.filename))
                    for name, data in ((MAIN, mod), (ORIGINAL, original)):
                        info = zipfile.ZipInfo(name)
                        info.compress_type = zipfile.ZIP_STORED
                        dst.writestr(info, data)
        except BaseException:
            if created:
                output.unlink(missing_ok=True)
            raise
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        output = patch(args.base, args.library, args.output)
    except (OSError, ValueError, zipfile.BadZipFile) as exc:
        parser.exit(1, f"Patch failed: {exc}\n")
    print(f"Unsigned APK: {output}\nAlignment, signing and installation are separate steps.")


if __name__ == "__main__":
    main()
