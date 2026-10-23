#!/usr/bin/env python3
"""Verify the GX state helper's indirect-jump targets against the pinned Prime DOL."""
from __future__ import annotations

import hashlib
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOL = ROOT / "decomp/orig/RM3E01_00/sys/main.dol"
SOURCE = ROOT / "recomp/generated/RM3E01_00_conservative/functions/func_804D6690.cpp"
OUT = ROOT / "recomp/checkpoints/20261020_prime_gx_state_switches.json"
DOL_SHA256 = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"
TABLES = {
    "state_selector": (0x805A647C, 36),
    "attribute_selector": (0x805A6420, 23),
}


def section_offset(dol: bytes, address: int, length: int) -> tuple[int, int]:
    for index in range(18):
        file_off = struct.unpack_from(">I", dol, index * 4)[0]
        vaddr = struct.unpack_from(">I", dol, 0x48 + index * 4)[0]
        size = struct.unpack_from(">I", dol, 0x90 + index * 4)[0]
        if size and vaddr <= address and address + length <= vaddr + size:
            return file_off + address - vaddr, index
    raise SystemExit(f"DOL has no initialized section covering 0x{address:08X}")


def main() -> int:
    dol = DOL.read_bytes()
    source = SOURCE.read_text(encoding="utf-8")
    actual_hash = hashlib.sha256(dol).hexdigest()
    if actual_hash != DOL_SHA256:
        raise SystemExit(f"Unexpected DOL SHA-256: {actual_hash}")

    case_targets = {int(value, 16) for value in re.findall(r"case 0x([0-9A-Fa-f]+)u\s*:", source)}
    tables = {}
    for name, (address, count) in TABLES.items():
        file_off, section = section_offset(dol, address, count * 4)
        targets = list(struct.unpack_from(f">{count}I", dol, file_off))
        missing = sorted(set(targets) - case_targets)
        if missing:
            formatted = ", ".join(f"0x{x:08X}" for x in missing)
            raise SystemExit(f"{name}: source dispatch is missing DOL targets {formatted}")
        tables[name] = {
            "address": f"0x{address:08X}",
            "dol_section_index": section,
            "dol_file_offset": f"0x{file_off:X}",
            "entry_count": count,
            "unique_target_count": len(set(targets)),
            "all_targets_have_source_cases": True,
            "targets": [f"0x{x:08X}" for x in targets],
        }

    report = {
        "revision": "RM3E01_00",
        "dol_sha256": actual_hash,
        "source": SOURCE.relative_to(ROOT).as_posix(),
        "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        "tables": tables,
        "scope": "Checks that all literal targets in the two DOL jump tables are handled by the translated function's switch. It does not prove instruction-level equivalence or rendering correctness.",
    }
    OUT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"checkpoint": OUT.relative_to(ROOT).as_posix(), "tables": {
        name: {"entry_count": data["entry_count"], "unique_target_count": data["unique_target_count"],
               "all_targets_have_source_cases": data["all_targets_have_source_cases"]}
        for name, data in tables.items()
    }}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
