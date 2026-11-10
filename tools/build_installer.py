#!/usr/bin/env python3
"""Build the on-device installer with Android SDK tools; no Gradle downloads."""
import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile
import xml.etree.ElementTree as ET

from build import source_build_id
from fetch_installer_dependencies import jars as shizuku_jars

ROOT = Path(__file__).resolve().parents[1]
ANDROID = "{http://schemas.android.com/apk/res/android}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT"))
    parser.add_argument("--java-home", type=Path, default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--build-tools", default="35.0.0")
    parser.add_argument("--platform", default="android-29")
    parser.add_argument("--library", type=Path, default=ROOT / "build/android/out/libsnmod.so")
    parser.add_argument("--test", action="store_true", help="Register local instrumentation; never distribute this build")
    parser.add_argument("--keystore", type=Path, help="Optional private signing key, never included in output")
    parser.add_argument("--alias", default="snmp-installer")
    args = parser.parse_args()
    if not args.sdk or not args.java_home:
        parser.error("Pass --sdk and --java-home, or set ANDROID_HOME and JAVA_HOME")
    sdk, java = args.sdk.resolve(), args.java_home.resolve()
    shizuku = shizuku_jars()
    bt = sdk / "build-tools" / args.build_tools
    android = sdk / "platforms" / args.platform / "android.jar"
    suffix = ".exe" if os.name == "nt" else ""
    javac, java_bin = java / "bin" / ("javac" + suffix), java / "bin" / ("java" + suffix)
    apksigner = bt / "lib/apksigner.jar"
    for file in (javac, java_bin, android, apksigner, bt / ("aapt2" + suffix), bt / "lib/d8.jar"):
        if not file.is_file(): parser.error(f"Missing tool: {file}")
    library = args.library.resolve()
    identity = source_build_id()
    if not library.is_file() or library.parent.parent.joinpath("snmod_build_id.txt").read_text().strip() != identity:
        parser.error("Build the current native library with tools/build.py first")
    out = ROOT / "build" / ("installer-test" if args.test else "installer")
    out.mkdir(parents=True, exist_ok=True)
    for name in ("classes", "dex", "assets"):
        folder = out / name
        if folder.exists(): shutil.rmtree(folder)
        folder.mkdir()
    manifest = ET.parse(ROOT / "installer/AndroidManifest.xml")
    ET.register_namespace("android", "http://schemas.android.com/apk/res/android")
    if args.test:
        manifest.getroot().find("application").set(ANDROID + "debuggable", "true")
        queries = manifest.getroot().find("queries")
        ET.SubElement(queries, "package", {ANDROID + "name": "io.github.zerk235.snmp.fixture"})
        ET.SubElement(queries, "provider", {ANDROID + "authorities": "io.github.zerk235.snmp.fixture.state"})
        ET.SubElement(manifest.getroot(), "instrumentation", {
            ANDROID + "name": "io.github.zerk235.snmp.installer.InstallerTests",
            ANDROID + "targetPackage": "io.github.zerk235.snmp.installer"})
        ET.SubElement(manifest.getroot(), "instrumentation", {
            ANDROID + "name": "io.github.zerk235.snmp.installer.SaveTests",
            ANDROID + "targetPackage": "io.github.zerk235.snmp.installer"})
        ET.SubElement(manifest.getroot(), "instrumentation", {
            ANDROID + "name": "io.github.zerk235.snmp.installer.DiagnosticsTests",
            ANDROID + "targetPackage": "io.github.zerk235.snmp.installer"})
    manifest.write(out / "AndroidManifest.xml", encoding="utf-8", xml_declaration=True)
    shutil.copyfile(library, out / "assets/libsnmod.so")
    (out / "assets/mod.json").write_text(json.dumps({"buildId": identity, "sha256": hashlib.sha256(library.read_bytes()).hexdigest(), "protocol": 29}), encoding="utf-8")
    for name in ("LICENSE.md", "THIRD_PARTY_NOTICES.md"):
        shutil.copyfile(ROOT / name, out / "assets" / name)
    # The APK contains the actual license, not just a link to the dependency.
    shutil.copyfile(ROOT / "modloader/libs/shadowhook/LICENSE", out / "assets/ShadowHook-LICENSE.txt")
    third_party = ROOT / "modloader/libs/shadowhook/shadowhook/src/main/cpp/third_party"
    notices = []
    for path in sorted(third_party.rglob("*")):
        if path.is_file() and path.name == "LICENSE":
            notices.append(path.relative_to(third_party).as_posix() + "\n" + path.read_text(encoding="utf-8"))
    for name in ("queue.h", "tree.h"):
        text = (third_party / "bsd" / name).read_text(encoding="utf-8")
        notices.append("bsd/" + name + "\n" + text[:text.index("*/")+2])
    (out / "assets/ShadowHook-third-party-LICENSES.txt").write_text("\n\n".join(notices), encoding="utf-8")
    shutil.copyfile(bt / "NOTICE.txt", out / "assets/Android-SDK-NOTICE.txt")
    shutil.copyfile(ROOT / "installer/licenses/Shizuku-API-LICENSE.txt", out / "assets/Shizuku-API-LICENSE.txt")
    if args.test:
        fixture_dir = out / "fixture"
        fixture_dir.mkdir(exist_ok=True)
        fixture_assets = fixture_dir / "assets"
        fixture_assets.mkdir(exist_ok=True)
        (fixture_assets / "probe.txt").write_text("Synthetic split payload; no game data.\n", encoding="utf-8")
        fixture_classes = fixture_dir / "classes"
        fixture_classes.mkdir(exist_ok=True)
        subprocess.run([str(javac), "-encoding", "UTF-8", "-source", "8", "-target", "8", "-classpath", str(android),
                        "-d", str(fixture_classes), *map(str, sorted((ROOT / "installer/tests/fixture").glob("*.java")))], check=True)
        fixture_jar = fixture_dir / "fixture.jar"
        with zipfile.ZipFile(fixture_jar, "w") as z:
            for path in fixture_classes.rglob("*.class"): z.write(path, path.relative_to(fixture_classes).as_posix())
        fixture_dex = fixture_dir / "dex"
        fixture_dex.mkdir(exist_ok=True)
        subprocess.run([str(java_bin), "-cp", str(bt / "lib/d8.jar"), "com.android.tools.r8.D8", "--min-api", "29",
                        "--lib", str(android), "--output", str(fixture_dex), str(fixture_jar)], check=True)
        for name in ("base", "split"):
            split = ' split="demo_assets"' if name == "split" else ""
            xml = ('<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
                   'package="io.github.zerk235.snmp.fixture" android:versionCode="1" android:versionName="fixture"'
                   + split + '><uses-sdk android:minSdkVersion="29" android:targetSdkVersion="29"/>'
                   + ('<application android:hasCode="false"/>' if name == "split" else
                      '<application android:label="SNMP test fixture"><provider android:name="io.github.zerk235.snmp.fixture.FixtureProvider" '
                      'android:authorities="io.github.zerk235.snmp.fixture.state" android:exported="true"/>'
                      '<activity android:name="io.github.zerk235.snmp.fixture.FixtureActivity" android:exported="true"/></application>')
                   + '</manifest>')
            path = fixture_dir / (name + ".xml")
            path.write_text(xml, encoding="utf-8")
            subprocess.run([str(bt / ("aapt2" + suffix)), "link", "-I", str(android), "--manifest", str(path),
                            "-A", str(fixture_assets), "-o", str(out / "assets" / ("fixture-" + name + ".apk"))], check=True)
            if name == "base":
                with zipfile.ZipFile(out / "assets/fixture-base.apk", "a") as z:
                    z.write(fixture_dex / "classes.dex", "classes.dex")
    dep = out / "apksig.jar"
    with zipfile.ZipFile(apksigner) as src, zipfile.ZipFile(dep, "w") as dst:
        for info in src.infolist():
            if info.filename.startswith("com/android/apksig/") and info.filename.endswith(".class"):
                dst.writestr(info, src.read(info))
    sources = sorted((ROOT / "installer/src").rglob("*.java"))
    if args.test: sources += sorted((ROOT / "installer/tests").glob("*.java"))
    classpath = os.pathsep.join(map(str,(android,dep,*shizuku)))
    subprocess.run([str(javac), "-encoding", "UTF-8", "-source", "8", "-target", "8",
                    "-classpath", classpath, "-d", str(out / "classes"), *map(str, sources)], check=True)
    classes = out / "app.jar"
    with zipfile.ZipFile(classes, "w") as z:
        for file in (out / "classes").rglob("*.class"):
            z.write(file, file.relative_to(out / "classes").as_posix())
    subprocess.run([str(java_bin), "-cp", str(bt / "lib/d8.jar"), "com.android.tools.r8.D8",
                    "--min-api", "29", "--lib", str(android), "--output", str(out / "dex"),
                    str(classes), str(dep), *map(str,shizuku)], check=True)
    unsigned = out / "installer.unsigned.apk"
    subprocess.run([str(bt / ("aapt2" + suffix)), "link", "-I", str(android), "--manifest", str(out / "AndroidManifest.xml"),
                    "-A", str(out / "assets"), "-o", str(unsigned)], check=True)
    with zipfile.ZipFile(unsigned, "a") as z:
        for file in (out / "dex").glob("*.dex"): z.write(file, file.name)
    aligned = out / "installer.aligned.apk"
    subprocess.run([str(bt / ("zipalign" + suffix)), "-f", "-P", "16", "4", str(unsigned), str(aligned)], check=True)
    artifact = aligned
    if args.keystore:
        password = getpass.getpass("Installer signing keystore password: ")
        if not password: parser.error("Empty password")
        env = os.environ.copy(); env["SNMP_INSTALLER_SIGN_PASS"] = password
        artifact = out / ("snmp-installer-test.apk" if args.test else "snmp-installer-preview.apk")
        try:
            subprocess.run([str(java_bin), "-jar", str(apksigner), "sign", "--ks", str(args.keystore.resolve()),
                            "--ks-key-alias", args.alias, "--ks-pass", "env:SNMP_INSTALLER_SIGN_PASS",
                            "--key-pass", "env:SNMP_INSTALLER_SIGN_PASS", "--min-sdk-version", "29",
                            "--v1-signing-enabled", "false", "--v2-signing-enabled", "true", "--v3-signing-enabled", "true",
                            "--out", str(artifact), str(aligned)], env=env, check=True)
            subprocess.run([str(java_bin), "-jar", str(apksigner), "verify", "--min-sdk-version", "29", str(artifact)], check=True)
        finally:
            env.pop("SNMP_INSTALLER_SIGN_PASS", None); password = ""
    print(f"Artifact: {artifact}\nNative build: {identity}\nSHA256: {hashlib.sha256(artifact.read_bytes()).hexdigest()}")
    print("TEST BUILD: not for release" if args.test else ("Signed preview" if args.keystore else "Unsigned; sign before installation"))


if __name__ == "__main__":
    main()
