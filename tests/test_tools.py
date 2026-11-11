"""Synthetic APK fixtures only; no game bytes or signing credentials."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


patcher = load("patch_apk")
builder = load("build")


def elf(payload):
    header = bytearray(64)
    header[:6] = b"\x7fELF\x02\x01"
    header[16:20] = b"\x03\x00\xb7\x00"
    return bytes(header) + payload


class ToolsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.base = self.root / "original.apk"
        self.lib = self.root / "libsnmod.so"
        self.out = self.root / "patched.apk"
        self.original = elf(b"original-bootstrap")
        self.mod = elf(b"mod-libmain_orig.so")
        self.lib.write_bytes(self.mod)
        self.apk()

    def apk(self, original=None, recovered=None):
        with zipfile.ZipFile(self.base, "w") as z:
            z.writestr(patcher.MAIN, original if original is not None else self.original)
            z.writestr("AndroidManifest.xml", b"synthetic-manifest")
            z.writestr("assets/sample", b"synthetic-assets")
            z.writestr("META-INF/CERT.RSA", b"obsolete-signature")
            if recovered is not None:
                z.writestr(patcher.ORIGINAL, recovered)

    def test_patch_preserves_input_and_payload_and_removes_signature(self):
        before = self.base.read_bytes()
        patcher.patch(self.base, self.lib, self.out)
        self.assertEqual(self.base.read_bytes(), before)
        with zipfile.ZipFile(self.out) as z:
            self.assertEqual(z.read(patcher.MAIN), self.mod)
            self.assertEqual(z.read(patcher.ORIGINAL), self.original)
            self.assertEqual(z.read("assets/sample"), b"synthetic-assets")
            self.assertNotIn("META-INF/CERT.RSA", z.namelist())
            self.assertEqual(z.getinfo(patcher.MAIN).compress_type, zipfile.ZIP_STORED)

    def test_repatch_preserves_true_original(self):
        self.apk(self.mod, self.original)
        patcher.patch(self.base, self.lib, self.out)
        with zipfile.ZipFile(self.out) as z:
            self.assertEqual(z.read(patcher.ORIGINAL), self.original)

    def test_broken_repatch_is_rejected(self):
        self.apk(self.mod)
        with self.assertRaises(ValueError):
            patcher.patch(self.base, self.lib, self.out)
        self.assertFalse(self.out.exists())

    def test_no_existing_output_or_input_overwrite(self):
        self.out.write_bytes(b"existing")
        for target in (self.base, self.lib, self.out):
            with self.assertRaises(ValueError):
                patcher.patch(self.base, self.lib, target)
        self.assertEqual(self.out.read_bytes(), b"existing")

    def test_wrong_architecture_is_rejected(self):
        wrong = bytearray(self.mod)
        wrong[18:20] = b"\x3e\x00"
        self.lib.write_bytes(wrong)
        with self.assertRaises(ValueError):
            patcher.patch(self.base, self.lib, self.out)
        self.assertFalse(self.out.exists())

    def test_build_identity_changes_with_sources_not_outputs_or_line_endings(self):
        native = self.root / "modloader"
        (native / "src").mkdir(parents=True)
        (native / "libs").mkdir()
        (native / "CMakeLists.txt").write_bytes(b"project(test)\n")
        source = native / "src/a.cpp"
        source.write_bytes(b"one\n")
        before = builder.source_build_id(native)
        source.write_bytes(b"one\r\n")
        self.assertEqual(builder.source_build_id(native), before)
        (native / "build").mkdir()
        (native / "build/output").write_bytes(b"generated")
        self.assertEqual(builder.source_build_id(native), before)
        source.write_bytes(b"two\n")
        self.assertNotEqual(builder.source_build_id(native), before)

    def test_installer_protocol_follows_native_demo_branch(self):
        native = self.root / "modloader"
        (native / "src").mkdir(parents=True)
        header = native / "src/net.h"
        for protocol in (29, 31, 65535):
            header.write_text("#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)\n"
                              f"constexpr uint16_t kSessionProtocolVersion = {protocol};\n"
                              "#else\nconstexpr uint16_t kSessionProtocolVersion = 10;\n#endif\n")
            self.assertEqual(builder.session_protocol(native), protocol)
        for invalid in ("", "constexpr uint16_t kSessionProtocolVersion = 10;",
                        "#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)\nconstexpr uint16_t kSessionProtocolVersion = 0;",
                        "#if defined(SNMOD_BASE_GEOMETRY_CAPTURE)\nconstexpr uint16_t kSessionProtocolVersion = 65536;"):
            header.write_text(invalid)
            with self.assertRaises(ValueError):
                builder.session_protocol(native)


if __name__ == "__main__":
    unittest.main()
