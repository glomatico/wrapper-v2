"""The supported runtime profiles. Accept Linux and Android architecture names."""
import json
import platform
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
ALIASES = {"x86_64": "x86_64", "amd64": "x86_64",
           "aarch64": "arm64-v8a", "arm64": "arm64-v8a", "arm64-v8a": "arm64-v8a"}

def normalize(value=None):
    value = value or platform.machine()
    if value not in ALIASES:
        raise ValueError("unsupported architecture: " + value)
    return ALIASES[value]

def load_profile(architecture=None):
    arch = normalize(architecture)
    profile = json.loads((ROOT / "profiles" / ("apple-music-1109-" + arch + ".json")).read_text())
    if profile["architecture"] != arch:
        raise ValueError("profile architecture mismatch")
    return profile

def verify_elf(data, profile):
    if len(data) < 20 or data[:6] != b"\x7fELF\x02\x01" or int.from_bytes(data[18:20], "little") != profile["elf_machine"]:
        raise ValueError("ELF architecture does not match " + profile["architecture"])

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch")
    parser.add_argument("--field")
    args = parser.parse_args()
    try:
        value = load_profile(args.arch)
        print(value[args.field] if args.field else json.dumps(value))
    except (ValueError, KeyError) as error:
        parser.exit(1, str(error) + "\n")
