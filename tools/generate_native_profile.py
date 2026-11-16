"""Generate build-local native metadata from the selected source profile."""
import argparse
import json
from pathlib import Path
from architecture import load_profile
p = argparse.ArgumentParser()
p.add_argument("--arch", required=True)
p.add_argument("--output", type=Path, required=True)
a = p.parse_args()
profile = load_profile(a.arch)
lines = ["// Generated from profiles/; do not edit.", "#pragma once", "#include <utility>", "namespace worker {"]
for key in ("profile", "architecture", "linux_architecture"):
    lines.append("inline constexpr const char* " + key + " = " + json.dumps(profile[key]) + ";")
lines.append("inline constexpr const char* context_profile = " + (json.dumps(profile["context_profile"]) if profile["context_profile"] else "nullptr") + ";")
lines.append("inline constexpr bool context_supported = " + str(profile["context_supported"]).lower() + ";")
lines.append("inline constexpr std::pair<const char*,const char*> apple_libraries[]={")
for name, digest in profile["apple_libraries"].items():
    lines.append("    {" + json.dumps(name) + "," + json.dumps(digest) + "},")
lines += ["};", "}"]
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text("\n".join(lines) + "\n")
