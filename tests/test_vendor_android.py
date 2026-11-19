"""Verify the checked-in, official Google Android system runtime files."""
import hashlib
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "vendor" / "android-system"


class VendoredAndroidTests(unittest.TestCase):
    def test_bundled_files_match_pinned_manifest(self):
        for arch in ("x86_64", "arm64-v8a"):
            with self.subTest(arch=arch):
                profile = json.loads(
                    (ROOT / "profiles" / f"apple-music-1109-{arch}.json").read_text()
                )
                directory = VENDOR / arch
                expected = profile["android_system"]
                actual = {
                    str(file.relative_to(directory))
                    for file in directory.rglob("*")
                    if file.is_file() and file.name != "NOTICE.txt"
                }
                self.assertEqual(actual, set(expected))
                for rel, checksum in expected.items():
                    file = directory / rel
                    self.assertFalse(file.is_symlink(), rel)
                    self.assertEqual(hashlib.sha256(file.read_bytes()).hexdigest(), checksum, rel)
                notice = directory / "NOTICE.txt"
                self.assertGreater(notice.stat().st_size, 100_000)
                self.assertIn(b"Notices for files contained", notice.read_bytes()[:150])


if __name__ == "__main__":
    unittest.main()
