#!/usr/bin/env python3
"""Replace the mod library on an already patched, rooted ARM64 game.

Backs up the current wrapper locally, stages and verifies its replacement, then
renames it atomically. Copy/hash/startup failures restore the previous wrapper.
Saves and APKs are not modified. A later gameplay crash is not automatically detected.
Save and exit the world before using --saved-and-exited with a running game.
Both multiplayer phones still require matching mod build identities.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
import uuid
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT") or "")
ADB = SDK / "platform-tools" / "adb.exe"
GAME = "com.unknownworlds.subnautica"
LOG = "/sdcard/Android/data/" + GAME + "/files/mod_log/logs/modloader.log"
BUILD_DIR = ROOT / "build" / "hotswap"
LIB_LOCAL = BUILD_DIR / "out" / "libsnmod.so"


def adb(serial, *args, timeout=180):
    return subprocess.check_output([str(ADB), "-s", serial, *map(str, args)], timeout=timeout)


def su(serial, command, timeout=180):
    return adb(serial, "exec-out", "su", "-c", command, timeout=timeout)


def installed_lib_dir(serial):
    lines = adb(serial, "shell", "pm", "path", "--user", "0", GAME).decode().splitlines()
    bases = [line.removeprefix("package:").strip() for line in lines
             if line.startswith("package:") and line.strip().endswith("/base.apk")]
    if len(bases) != 1:
        raise RuntimeError("exactly one installed game base.apk is required")
    base = PurePosixPath(bases[0])
    if (base.as_posix() != bases[0] or not bases[0].startswith("/data/app/") or
            any(p in (".", "..") for p in base.parts) or
            not base.parent.name.startswith(GAME + "-") or
            not re.fullmatch(r"[/A-Za-z0-9_=~.+-]+", bases[0])):
        raise RuntimeError("unexpected game APK path; refusing filesystem changes")
    return str(base.parent / "lib" / "arm64")


def elf_arm64(data):
    return (len(data) >= 20 and data[:6] == b"\x7fELF\x02\x01" and
            int.from_bytes(data[16:18], "little") == 3 and
            int.from_bytes(data[18:20], "little") == 183)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def remote_hash(serial, path):
    text = su(serial, "sha256sum " + shlex.quote(path)).decode().split()
    if not text or not re.fullmatch(r"[0-9a-f]{64}", text[0]):
        raise RuntimeError("cannot verify remote library hash")
    return text[0]


def game_running(serial):
    # An ADB transport failure must never be mistaken for "game is closed".
    text = adb(serial, "shell", "pidof " + GAME + " || echo SNMP_NO_GAME_PROCESS").decode().strip()
    if text == "SNMP_NO_GAME_PROCESS":
        return False
    if not text or not all(part.isdigit() and int(part) > 0 for part in text.split()):
        raise RuntimeError("cannot determine whether the game is running")
    return True


def require_saved(serial, saved_and_exited):
    if game_running(serial) and not saved_and_exited:
        raise RuntimeError("game is running: save, exit to the menu, then pass --saved-and-exited")


def swap_library(serial, library, backup_dir, *, saved_and_exited=False, restart=True, log_tail=40):
    data = Path(library).read_bytes()
    if not elf_arm64(data) or b"libmain_orig.so\0" not in data:
        raise RuntimeError("replacement must be an ARM64 SNMP wrapper library")
    if su(serial, "id -u").strip() != b"0":
        raise RuntimeError("root access is required")
    directory = installed_lib_dir(serial)
    remote, original = directory + "/libmain.so", directory + "/libmain_orig.so"
    q = shlex.quote
    su(serial, f"test -f {q(remote)} && test ! -L {q(remote)} && test -s {q(original)} && test ! -L {q(original)}")
    if not elf_arm64(su(serial, "head -c 20 " + q(original))):
        raise RuntimeError("original ARM64 bootstrap is missing or invalid; patch with the installer first")
    previous = su(serial, "cat " + q(remote))
    if not elf_arm64(previous) or b"libmain_orig.so\0" not in previous:
        raise RuntimeError("installed game is not an SNMP-patched game; use the installer first")
    before, wanted, original_sha = digest(previous), digest(data), remote_hash(serial, original)
    if remote_hash(serial, remote) != before:
        raise RuntimeError("installed library changed during inspection")
    require_saved(serial, saved_and_exited)
    # Preserve owner, mode and SELinux label on the replacement inode.
    metadata = su(serial, "stat -c '%u %g %a' " + q(remote)).decode().strip().split()
    contexts = su(serial, "ls -Zd " + q(remote)).decode().split()
    context = contexts[0] if contexts else ''
    if (len(metadata) != 3 or not all(x.isdigit() for x in metadata[:2]) or
            not re.fullmatch(r"[0-7]{3,4}", metadata[2]) or
            not re.fullmatch(r"[A-Za-z0-9_:,.-]+", context) or ":" not in context):
        raise RuntimeError("cannot preserve library permissions and SELinux context")
    activity = None
    if restart:
        resolved = adb(serial, "shell", "cmd", "package", "resolve-activity", "--brief", "--user", "0", GAME).decode().splitlines()
        activity = resolved[-1].strip() if resolved else ""
        if not activity.startswith(GAME + "/") or not re.fullmatch(r"[A-Za-z0-9_./$]+", activity):
            raise RuntimeError("game launch activity could not be resolved")
    backup_dir = Path(backup_dir)
    backup_dir.mkdir(parents=True, exist_ok=False)
    backup = backup_dir / "libmain.before.so"
    backup.write_bytes(previous)
    if digest(backup.read_bytes()) != before:
        raise RuntimeError("local rollback copy failed verification")
    token = uuid.uuid4().hex
    upload = "/data/local/tmp/snmp-hotswap-" + token + ".so"
    stage = directory + "/.snmp-pending-" + token + ".so"
    rollback = directory + "/.snmp-before-" + token + ".so"
    report = {"serial": serial, "remote": remote, "original_sha256": original_sha,
              "before_sha256": before, "new_sha256": wanted, "backup": str(backup.resolve()),
              "phase": "backup_verified", "saves_modified": False, "apks_modified": False}
    def record(phase):
        report["phase"] = phase
        (backup_dir / "transaction.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    committed = False
    rollback_failed = False
    record("backup_verified")
    try:
        adb(serial, "push", str(library), upload)
        if remote_hash(serial, upload) != wanted:
            raise RuntimeError("uploaded library failed verification")
        su(serial, f"cp {q(upload)} {q(stage)} && chown {metadata[0]}:{metadata[1]} {q(stage)} && "
           f"chmod {metadata[2]} {q(stage)} && chcon {q(context)} {q(stage)}")
        if remote_hash(serial, stage) != wanted:
            raise RuntimeError("staged library failed verification")
        require_saved(serial, saved_and_exited)
        adb(serial, "shell", "am", "force-stop", GAME)
        if (installed_lib_dir(serial) != directory or remote_hash(serial, remote) != before or
                remote_hash(serial, original) != original_sha):
            raise RuntimeError("game package or libraries changed; refusing replacement")
        su(serial, f"ln {q(remote)} {q(rollback)}")
        if remote_hash(serial, rollback) != before:
            raise RuntimeError("on-device rollback copy failed verification")
        record("committing")
        committed = True  # A transport failure may occur after the rename succeeded.
        su(serial, f"mv -f {q(stage)} {q(remote)}")
        if remote_hash(serial, remote) != wanted or remote_hash(serial, original) != original_sha:
            raise RuntimeError("installed library failed verification")
        record("installed")
        if restart:
            result = adb(serial, "shell", "am", "start", "-W", "--user", "0", "-n", activity).decode()
            if "Status: ok" not in result:
                raise RuntimeError("Android did not confirm game launch: " + result.strip())
            time.sleep(12)
            if not game_running(serial):
                raise RuntimeError("game stopped during initial launch")
            if log_tail:
                print(su(serial, f"tail -n {log_tail} {q(LOG)}").decode("utf-8", "replace"))
        record("complete")
        return report
    except BaseException:
        if committed:
            try:
                adb(serial, "shell", "am", "force-stop", GAME)
                su(serial, f"mv -f {q(rollback)} {q(remote)}")
                if remote_hash(serial, remote) != before:
                    raise RuntimeError("restored library hash mismatch")
                record("rolled_back")
            except BaseException as rollback_error:
                rollback_failed = True
                report["rollback_error"] = str(rollback_error)
                report["remote_backup"] = rollback
                record("rollback_failed")
                raise RuntimeError("automatic rollback failed; retain " + str(backup) + " and " + rollback) from rollback_error
        else:
            record("aborted_before_replacement")
        raise
    finally:
        paths = [upload, stage] + ([] if rollback_failed else [rollback])
        try:
            su(serial, "rm -f " + " ".join(q(p) for p in paths), timeout=10)
        except (OSError, subprocess.SubprocessError):
            print("Temporary cleanup was not confirmed; the verified local backup remains at " + str(backup))


def main():
    global ADB
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--adb", default=str(ADB) if ADB.is_file() else shutil.which("adb"))
    parser.add_argument("--ndk", default=os.environ.get("ANDROID_NDK_HOME") or str(SDK / "ndk" / "26.1.10909125"))
    parser.add_argument("--cmake", default=str(SDK / "cmake" / "3.22.1" / "bin" / "cmake.exe"))
    parser.add_argument("--ninja", default=str(SDK / "cmake" / "3.22.1" / "bin" / "ninja.exe"))
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-restart", action="store_true")
    parser.add_argument("--saved-and-exited", action="store_true", help="confirm the world was saved and the game is at its main menu")
    parser.add_argument("--log-tail", type=int, default=40)
    args = parser.parse_args()
    if not args.adb or not Path(args.adb).is_file():
        parser.error("ADB not found; pass --adb /path/to/adb")
    if not 0 <= args.log_tail <= 1000:
        parser.error("--log-tail must be between 0 and 1000")
    ADB = Path(args.adb)
    if not args.no_build:
        subprocess.run([sys.executable, str(ROOT / "tools" / "build.py"), "--ndk", args.ndk,
                        "--cmake", args.cmake, "--ninja", args.ninja, "--build-dir", str(BUILD_DIR)], check=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d_%H%M%S") + "_" + uuid.uuid4().hex[:8]
    backup_dir = ROOT / "build" / "hotswap-backups" / stamp
    report = swap_library(args.serial, LIB_LOCAL, backup_dir, saved_and_exited=args.saved_and_exited,
                          restart=not args.no_restart, log_tail=args.log_tail)
    print("Library verified; rollback copy: " + report["backup"])


if __name__ == "__main__":
    main()
