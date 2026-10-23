"""Compare fixed-address native registrations in WiiCompiled HLE with RM3E01_00."""
from __future__ import annotations

import json
import re
import argparse
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAP = ROOT / "config" / "RM3E01_00" / "functions.txt"
HLE = ROOT / "third_party" / "WiiCompiled" / "runtime" / "src" / "hle"
REGISTRATION = re.compile(
    r"\b(?:PPC_NATIVE_OVERRIDE(?:_VOID)?|REGISTER_NATIVE_FUNCTION(?:_AS)?)"
    r"\s*\(\s*(?:0x)?([0-9A-Fa-f]{8})\b"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", choices=("all", "RM3E01_00"), default="all")
    parser.add_argument("--output-name", default=None)
    args = parser.parse_args()
    profile_enabled = args.profile == "RM3E01_00"
    rm3_addresses = {
        int(line.split()[0], 16)
        for line in MAP.read_text(encoding="utf-8").splitlines()
        if line.strip()
    }
    rm3_names = {
        int(line.split()[0], 16): line.split(maxsplit=1)[1]
        for line in MAP.read_text(encoding="utf-8").splitlines()
        if line.strip() and len(line.split(maxsplit=1)) > 1
    }
    entries: list[dict[str, object]] = []
    for source in sorted(HLE.rglob("*.cpp")):
        active = True
        stack: list[tuple[bool, bool | None]] = []
        for line_number, line in enumerate(source.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("#if"):
                parent = active
                condition_profile = "MKW_GAME_PROFILE_RM3E01_00" in stripped
                if condition_profile and "!defined" in stripped:
                    branch = not profile_enabled
                    stack.append((parent, branch))
                    active = parent and branch
                elif condition_profile and "defined" in stripped:
                    branch = profile_enabled
                    stack.append((parent, branch))
                    active = parent and branch
                else:
                    stack.append((parent, None))
                    active = parent
                continue
            if stripped.startswith("#else") and stack:
                parent, branch = stack[-1]
                active = parent and (not branch if branch is not None else True)
                stack[-1] = (parent, None if branch is None else not branch)
                continue
            if stripped.startswith("#endif") and stack:
                parent, _ = stack.pop()
                active = parent
                continue
            if args.profile != "all" and not active:
                continue
            for match in REGISTRATION.finditer(line):
                entries.append({
                    "address": int(match.group(1), 16),
                    "rm3_symbol": rm3_names.get(int(match.group(1), 16)),
                    "path": source.relative_to(ROOT).as_posix(),
                    "line": line_number,
                })
    addresses = sorted({int(entry["address"]) for entry in entries})
    overlap = sorted(set(addresses) & rm3_addresses)
    checkpoint = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "revision": "RM3E01_00",
        "compile_profile": args.profile,
        "upstream": "WiiCompiled runtime/src/hle, commit 4c166a7eb1ddfc79b4c832e422e6bb00ec06fb4f",
        "registration_macro_occurrences": len(entries),
        "unique_registered_addresses": len(addresses),
        "registered_addresses": [f"0x{address:08X}" for address in addresses],
        "addresses_present_in_rm3_function_map": len(overlap),
        "overlap_addresses": [f"0x{address:08X}" for address in overlap],
        "overlap_entries": [entry for entry in entries if int(entry["address"]) in set(overlap)],
        "interpretation": (
            "Address overlap is a signal for per-title audit only; matching addresses do not prove matching "
            "function semantics or ABI. This scan does not resolve indirect/dynamic registrations."
        ),
    }
    output = ROOT / "checkpoints" / (args.output_name or ("20261020_upstream_hle_address_overlap.json" if args.profile == "all" else "20261020_prime_profile_hle_overlap.json"))
    output.write_text(json.dumps(checkpoint, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in checkpoint.items() if key != "overlap_entries"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
