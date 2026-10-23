#!/usr/bin/env python3
"""Compare the Prime DVD state-machine jump-table bytes with the crash MEM1 dump."""
from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOL = ROOT / "decomp/orig/RM3E01_00/sys/main.dol"
RUN = ROOT / "recomp/build/RM3E01_00_runtime_link_probe/UserData/Logs/base_1792529414_pid3552"
MEM1 = RUN / "mem1.bin"
CONSOLE = RUN / "console.log"
OUT = ROOT / "recomp/checkpoints/20261020_prime_crash_jump_table.json"
DOL_SHA256 = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"
TABLE = 0x805A4480
INDEX = 0x0E
EXPECTED_TARGET = 0x804C9770
ENTRY = TABLE + INDEX * 4
BSS_INFO = 0x800066A0


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dol_file_offset(dol: bytes, address: int, length: int) -> tuple[int, int]:
    for index in range(18):
        file_offset = struct.unpack_from(">I", dol, index * 4)[0]
        vaddr = struct.unpack_from(">I", dol, 0x48 + index * 4)[0]
        size = struct.unpack_from(">I", dol, 0x90 + index * 4)[0]
        if size and vaddr <= address and address + length <= vaddr + size:
            return file_offset + address - vaddr, index
    raise SystemExit(f"DOL has no initialized section covering 0x{address:08X}")


def main() -> int:
    dol = DOL.read_bytes()
    mem1 = MEM1.read_bytes()
    console = CONSOLE.read_text(encoding="utf-8", errors="replace")
    if sha(DOL) != DOL_SHA256:
        raise SystemExit("DOL hash does not match pinned RM3E01_00")
    if len(mem1) != 0x01800000:
        raise SystemExit(f"Unexpected MEM1 snapshot size: 0x{len(mem1):X}")
    file_offset, section_index = dol_file_offset(dol, ENTRY, 4)
    expected = struct.unpack_from(">I", dol, file_offset)[0]
    actual = struct.unpack_from(">I", mem1, ENTRY - 0x80000000)[0]
    if expected != EXPECTED_TARGET:
        raise SystemExit(f"Unexpected DOL jump-table target: 0x{expected:08X}")
    initial_probe = "after embedded data load: checked=0x804c9770 flat=0x804c9770"
    bss_probe = "jump-table after __init_data = 0x804c9770"
    if initial_probe not in console or bss_probe not in console:
        raise SystemExit("The runtime log does not prove the table is valid after loading and __init_data")
    bss_ranges = []
    for offset in range(BSS_INFO - 0x80000000, BSS_INFO - 0x80000000 + 0x40, 8):
        start, size = struct.unpack_from(">II", mem1, offset)
        if size == 0:
            break
        bss_ranges.append({"start": f"0x{start:08X}", "size": f"0x{size:X}",
                           "end_exclusive": f"0x{start + size:08X}"})
    if any(int(item["start"], 16) <= ENTRY < int(item["end_exclusive"], 16) for item in bss_ranges):
        raise SystemExit("The recorded jump-table entry falls inside a DOL BSS clear range")

    report = {
        "revision": "RM3E01_00",
        "dol_sha256": DOL_SHA256,
        "crash_mem1_sha256": sha(MEM1),
        "crash_mem1": MEM1.relative_to(ROOT).as_posix(),
        "console_log_sha256": sha(CONSOLE),
        "table_value_after_embedded_data_load": "0x804C9770 (checked Memory and flat guest views)",
        "table_value_after_dol_init_data_bss_clear": "0x804C9770",
        "bss_init_ranges": bss_ranges,
        "table": {
            "virtual_address": f"0x{TABLE:08X}",
            "index": INDEX,
            "entry_address": f"0x{ENTRY:08X}",
            "dol_section_index": section_index,
            "dol_file_offset": f"0x{file_offset:X}",
            "expected_target": f"0x{expected:08X}",
            "crash_snapshot_target": f"0x{actual:08X}",
            "matches": expected == actual,
        },
        "interpretation": "The table is valid in both guest memory views after embedded section loading and still valid after the DOL's __init_data BSS clear. Its entry is zero in the later crash snapshot, and the recorded BSS ranges do not cover it. The entry is lost later during startup; the exact OSInit writer or mapping failure remains unknown.",
    }
    OUT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"checkpoint": OUT.relative_to(ROOT).as_posix(), **report["table"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
