#!/usr/bin/env python3
"""Safe source/staging tests. No Apple binaries or account required."""
import hashlib
import importlib.util
import io
import tempfile
import unittest
import warnings
import zipfile
import sys
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("stage", ROOT / "tools/prepare_rootfs.py")
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


class StagingTests(unittest.TestCase):
    def fixture(self, directory, *, abi="x86_64", arch=None, corrupt=False, duplicate=False, manifest=b"synthetic"):
        profile = stage.load_profile(abi)
        if arch is None:
            arch = profile["elf_machine"]
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        elf[18:20] = arch.to_bytes(2, "little")
        pins = dict(profile, split_manifest_sha256=stage.digest(b"synthetic"),
                    apple_libraries={"libTest.so": stage.digest(elf)})
        path = Path(directory) / "fixture.apk"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("AndroidManifest.xml", manifest)
            archive.writestr(f"lib/{abi}/libTest.so", bytes(elf) + (b"bad" if corrupt else b""))
            if duplicate:
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", UserWarning)
                    archive.writestr(f"lib/{abi}/libTest.so", elf)
        return path, pins

    def test_verified_library(self):
        for abi in ("x86_64", "arm64-v8a"):
            with self.subTest(abi=abi), tempfile.TemporaryDirectory() as directory:
                path, pins = self.fixture(directory, abi=abi)
                self.assertEqual(len(stage.extract_apple(path,pins)["libTest.so"]), 64)

    def test_verified_apkm_bundle(self):
        for abi in ("x86_64", "arm64-v8a"):
            with self.subTest(abi=abi), tempfile.TemporaryDirectory() as directory:
                split_path, pins = self.fixture(directory, abi=abi)
                pins = dict(pins, base_manifest_sha256=stage.digest(b"synthetic base"))
                base_bytes = io.BytesIO()
                with zipfile.ZipFile(base_bytes, "w") as base:
                    base.writestr("AndroidManifest.xml", b"synthetic base")
                bundle = Path(directory) / "fixture.apkm"
                with zipfile.ZipFile(bundle, "w") as archive:
                    archive.writestr("base.apk", base_bytes.getvalue())
                    archive.write(split_path, pins["apk_split"])
                self.assertEqual(len(stage.extract_apple(bundle, pins)["libTest.so"]), 64)

    def test_standalone_apk_with_base_manifest_and_native_libraries(self):
        for abi in ("x86_64", "arm64-v8a"):
            with self.subTest(abi=abi), tempfile.TemporaryDirectory() as directory:
                path, pins = self.fixture(directory, abi=abi, manifest=b"synthetic base")
                pins = dict(pins, base_manifest_sha256=stage.digest(b"synthetic base"))
                self.assertEqual(len(stage.extract_apple(path, pins)["libTest.so"]), 64)

    def test_wrong_architecture(self):
        with tempfile.TemporaryDirectory() as directory:
            path, pins = self.fixture(directory, arch=183)
            with patch.object(stage, "PINS", pins), self.assertRaisesRegex(ValueError, "architecture"):
                stage.extract_apple(path)

    def test_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            path, pins = self.fixture(directory, corrupt=True)
            with patch.object(stage, "PINS", pins), self.assertRaisesRegex(ValueError, "unsupported Apple library"):
                stage.extract_apple(path)

    def test_manifest_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            path, pins = self.fixture(directory, manifest=b"unknown build")
            with patch.object(stage, "PINS", pins), self.assertRaisesRegex(ValueError, "APK architecture/build"):
                stage.extract_apple(path)

    def test_duplicate_library(self):
        with tempfile.TemporaryDirectory() as directory:
            path, pins = self.fixture(directory, duplicate=True)
            with patch.object(stage, "PINS", pins), self.assertRaisesRegex(ValueError, "ambiguous"):
                stage.extract_apple(path)

    def test_existing_destination_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            marker = path / "keep"
            marker.write_text("untouched")
            with self.assertRaises(ValueError):
                stage.stage(path, path, path, path, path)
            self.assertEqual(marker.read_text(), "untouched")

    def test_manifest_matches_runtime(self):
        import re
        import subprocess
        for abi in ("x86_64", "arm64-v8a"):
            with self.subTest(abi=abi), tempfile.TemporaryDirectory() as directory:
                output = Path(directory)/"worker_profile.hpp"
                subprocess.run([sys.executable,str(ROOT/"tools/generate_native_profile.py"),
                                "--arch",abi,"--output",str(output)],check=True)
                pins = dict(re.findall(r'\{"([^"]+)","([0-9a-f]{64})"\}', output.read_text()))
                self.assertEqual(pins, stage.load_profile(abi)["apple_libraries"])

    def test_capture_contract(self):
        import re
        text = (ROOT / "native/capture_offsets.hpp").read_text().split("{", 1)[1]
        offsets = list(map(int, re.findall(r"\d+", text)))
        self.assertEqual(len(offsets), 754)
        self.assertEqual(offsets, sorted(set(offsets)))
        self.assertTrue(all(0 <= v < 2048 for v in offsets))
        for offset in (0x90, 0x140, 0x788, 0x370):
            self.assertFalse(set(range(2048 - offset, 2048 - offset + 8)) & set(offsets))


if __name__ == "__main__":
    unittest.main()
