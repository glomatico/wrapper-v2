"""Reject wrong-machine executables before packaging."""
import argparse
from pathlib import Path
from architecture import load_profile, verify_elf

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch")
    parser.add_argument("files", type=Path, nargs="+")
    args = parser.parse_args()
    profile = load_profile(args.arch)
    for path in args.files:
        with path.open("rb") as source:
            try:
                verify_elf(source.read(64), profile)
            except ValueError as error:
                parser.exit(1, f"{path}: {error}\n")
    print("Verified ELF architecture: " + profile["architecture"])

if __name__ == "__main__":
    main()
