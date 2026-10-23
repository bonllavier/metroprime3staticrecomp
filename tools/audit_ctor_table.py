"""Verify the RM3E01_00 DOL constructor table against the function map."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ctors_blob", type=Path)
    parser.add_argument("function_map", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    blob = args.ctors_blob.read_bytes()
    if len(blob) % 4:
        raise SystemExit(".ctors blob length is not a multiple of four")
    words = struct.unpack(f">{len(blob) // 4}I", blob)
    try:
        terminator = words.index(0)
    except ValueError as exc:
        raise SystemExit(".ctors table has no null terminator") from exc
    entries = list(words[:terminator])
    starts = {
        int(line.split(maxsplit=1)[0], 16)
        for line in args.function_map.read_text(encoding="utf-8").splitlines()
        if line.strip()
    }
    missing = sorted(set(entries) - starts)
    report = {
        "revision": "RM3E01_00",
        "dol_sha256": "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104",
        "ctors_address": "0x80575680",
        "ctors_blob_bytes": len(blob),
        "ctors_blob_sha256": sha256(args.ctors_blob),
        "function_map_sha256": sha256(args.function_map),
        "table_word_count": len(words),
        "null_terminator_index": terminator,
        "nonzero_entries_before_terminator": len(entries),
        "unique_entries": len(set(entries)),
        "duplicate_entry_count": len(entries) - len(set(entries)),
        "entries_exactly_present_in_function_map": len(entries) - len(missing),
        "entries_missing_from_function_map": [f"0x{x:08X}" for x in missing],
        "first_entry": f"0x{entries[0]:08X}" if entries else None,
        "accepted": not missing and len(entries) == len(set(entries)),
        "interpretation": "The DOL guest __init_cpp iterates this table until the first null entry; table membership does not prove that every constructor body is behaviorally recovered.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("nonzero_entries_before_terminator", "unique_entries", "entries_exactly_present_in_function_map", "accepted")}, indent=2))
    return 0 if report["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
