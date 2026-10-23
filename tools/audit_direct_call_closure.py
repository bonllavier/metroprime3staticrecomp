#!/usr/bin/env python3
"""Measure the static direct-call closure from the DOL entry point."""

from __future__ import annotations

import hashlib
import json
import re
from collections import deque
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD_ID = "RM3E01_00_conservative"
ENTRY = 0x80006320
DIRECT = re.compile(r"InvokeDirectCpu<0x([0-9A-Fa-f]+)u>\(ctx\)")
INDIRECT = re.compile(r"InvokeIndirectCpu\(")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dol_word(address: int) -> str:
    data = (ROOT / "recomp/input/RM3E01_00/main.dol").read_bytes()
    for index in range(18):
        offset = int.from_bytes(data[index * 4:index * 4 + 4], "big")
        base = int.from_bytes(data[0x48 + index * 4:0x4C + index * 4], "big")
        size = int.from_bytes(data[0x90 + index * 4:0x94 + index * 4], "big")
        if offset and base <= address and address + 4 <= base + size:
            file_offset = offset + address - base
            return f"0x{int.from_bytes(data[file_offset:file_offset + 4], 'big'):08X}"
    raise ValueError(f"DOL has no file-backed word at 0x{address:08X}")


def main() -> int:
    generated = ROOT / "recomp/generated" / BUILD_ID / "functions"
    source_by_address: dict[int, Path] = {}
    for path in generated.glob("func_*.cpp"):
        try:
            source_by_address[int(path.stem.removeprefix("func_"), 16)] = path
        except ValueError:
            continue

    graph: dict[int, list[int]] = {}
    indirect_counts: dict[int, int] = {}
    for address, path in source_by_address.items():
        text = path.read_text(encoding="utf-8")
        graph[address] = [int(match.group(1), 16) for match in DIRECT.finditer(text)]
        indirect_counts[address] = len(INDIRECT.findall(text))

    visited: set[int] = set()
    queue = deque([ENTRY])
    while queue:
        address = queue.popleft()
        if address in visited:
            continue
        visited.add(address)
        queue.extend(target for target in graph.get(address, ()) if target not in visited)

    hle_path = ROOT / "recomp/checkpoints/20261020_upstream_hle_address_overlap.json"
    hle = json.loads(hle_path.read_text(encoding="utf-8"))
    overlap = []
    for item in hle["overlap_entries"]:
        address = item["address"]
        if address in visited:
            overlap.append({**item, "in_direct_call_closure": True})

    reachable_indirect = [f"0x{address:08X}" for address in sorted(visited) if indirect_counts.get(address, 0)]
    checkpoint = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "revision": "RM3E01_00",
        "entry_point": f"0x{ENTRY:08X}",
        "dol_sha256": digest(ROOT / "recomp/input/RM3E01_00/main.dol"),
        "generated_build_id": BUILD_ID,
        "generated_function_sources": len(source_by_address),
        "entry_reachable_direct_call_closure": len(visited),
        "reachable_missing_generated_targets": [f"0x{a:08X}" for a in sorted(visited) if a not in source_by_address],
        "reachable_functions_containing_indirect_dispatch": reachable_indirect,
        "key_indirect_dispatch_observations": [
            {
                "function": "0x80006320",
                "call_site": "0x800063C0",
                "target": f"0x{(0x804C0000 - 24932):08X}",
                "target_registered_function_entry": (0x804C0000 - 24932) in source_by_address,
                "evidence": "target computed as 0x804C0000 - 24932; it equals InitMetroTRK at 0x804B9E9C and the target bytes are verified against the DOL",
            },
            {
                "function": "0x802D4D24",
                "call_site": "0x802D4DB4",
                "vtable_address": "0x8059C788",
                "vtable_slot_address": "0x8059C790",
                "target": dol_word(0x8059C790),
                "target_registered_function_entry": dol_word(0x8059C790) in {f"0x{a:08X}" for a in source_by_address},
                "evidence": "fn_8038291C constructs the object and writes vtable 0x805A0000-14456; DOL word at slot +8 is independently read",
            },
            {
                "function": "0x803DDD48",
                "call_site": "0x803DDDDC",
                "vtable_address": "0x805992D4",
                "vtable_slot_address": "0x805992DC",
                "target": dol_word(0x805992DC),
                "target_registered_function_entry": dol_word(0x805992DC) in {f"0x{a:08X}" for a in source_by_address},
                "evidence": "fn_802D4F60 constructs the temporary owner and writes vtable 0x805A0000-27948; CToken's guarded virtual call resolves through slot +8",
            },
            {
                "function": "0x802D5150",
                "call_site": "0x802D5284",
                "vtable_address": "0x805992C8",
                "vtable_slot_address": "0x805992D0",
                "target": dol_word(0x805992D0),
                "target_registered_function_entry": dol_word(0x805992D0) in {f"0x{a:08X}" for a in source_by_address},
                "guard_assessment": "statically bypassed: main clears the local flag at stack +24 before the later test; Ghidra main pseudocode shows that same local is tested after CToken construction",
                "evidence": "fn_802D5310 writes vtable 0x805A0000-27960; vtable +8 comes from DOL data. The checked local flag is not part of the adjacent CToken object at stack +32.",
            },
        ],
        "upstream_hle_overlap_count_in_direct_closure": len(overlap),
        "upstream_hle_overlaps_in_direct_closure": overlap,
        "hle_overlap_checkpoint_sha256": digest(hle_path),
        "interpretation": "Over-approximation from direct call instructions in generated bodies. It ignores branch conditions and dynamic targets, and does not prove runtime reachability, HLE execution, function correctness, or first-screen behavior.",
    }
    out = ROOT / "recomp/checkpoints/20261020_direct_call_closure.json"
    out.write_text(json.dumps(checkpoint, indent=2) + "\n", encoding="utf-8")
    print(f"Direct-call closure: {len(visited)} entries; missing generated targets: {len(checkpoint['reachable_missing_generated_targets'])}")
    print(f"Reachable functions containing indirect dispatch: {len(reachable_indirect)}")
    print(f"Upstream HLE overlaps in this closure: {len(overlap)}")
    for item in overlap:
        print(f"  {item['address']}: {item['rm3_symbol']} <- {item['path']}:{item['line']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
