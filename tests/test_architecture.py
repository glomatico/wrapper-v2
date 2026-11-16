"""Architecture/profile contracts; fixtures never contain proprietary data."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"tools"))
from architecture import normalize, load_profile, verify_elf

class ArchitectureTests(unittest.TestCase):
    def test_aliases_and_unknown_architectures(self):
        for alias in ("arm64", "aarch64", "arm64-v8a"):
            self.assertEqual(normalize(alias), "arm64-v8a")
        self.assertEqual(normalize("amd64"), "x86_64")
        with self.assertRaises(ValueError):
            normalize("riscv64")

    def test_profiles_and_machine_rejection(self):
        profiles = [load_profile(abi) for abi in ("x86_64", "arm64-v8a")]
        self.assertNotEqual(profiles[0]["apple_libraries"], profiles[1]["apple_libraries"])
        for profile in profiles:
            self.assertEqual(profile["android_api"],21)
            self.assertEqual(len(profile["apple_libraries"]),18)
            for digest in profile["apple_libraries"].values():
                self.assertRegex(digest,r"^[0-9a-f]{64}$")
            elf=bytearray(64)
            elf[:6]=b"\x7fELF\x02\x01"
            elf[18:20]=profile["elf_machine"].to_bytes(2,"little")
            verify_elf(elf,profile)
            other=next(p for p in profiles if p!=profile)
            with self.assertRaises(ValueError):
                verify_elf(elf,other)
            elf[4]=1
            with self.assertRaises(ValueError):
                verify_elf(elf,profile)

    def test_cli_build_selection(self):
        for alias, target in [("amd64","x86_64-unknown-linux-gnu"),
                              ("aarch64","aarch64-unknown-linux-gnu")]:
            result=subprocess.check_output([sys.executable,str(ROOT/"tools/architecture.py"),
                                            "--arch",alias,"--field","rust_target"],text=True)
            self.assertEqual(result.strip(),target)

if __name__=="__main__":
    unittest.main()
