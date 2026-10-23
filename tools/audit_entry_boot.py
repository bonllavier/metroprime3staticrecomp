#!/usr/bin/env python3
"""Record statically visible direct calls from the RM3E01_00 DOL entry path."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from datetime import datetime, timezone


ROOT = Path(__file__).resolve().parents[2]
BUILD_ID = "RM3E01_00_conservative"
ENTRY = 0x80006320


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def read_dol_words(path: Path, address: int, count: int) -> list[str]:
    data = path.read_bytes()
    for index in range(18):
        file_offset = int.from_bytes(data[index * 4:index * 4 + 4], "big")
        guest_address = int.from_bytes(data[0x48 + index * 4:0x4C + index * 4], "big")
        size = int.from_bytes(data[0x90 + index * 4:0x94 + index * 4], "big")
        if file_offset and guest_address <= address and address + count * 4 <= guest_address + size:
            start = file_offset + address - guest_address
            return [f"0x{int.from_bytes(data[start + i * 4:start + i * 4 + 4], 'big'):08X}" for i in range(count)]
    raise ValueError(f"DOL has no section covering 0x{address:08X}..0x{address + count * 4:08X}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "recomp/checkpoints/20261020_entry_boot_audit.json")
    args = parser.parse_args()

    symbols_path = ROOT / "decomp/config/RM3E01_00/symbols.txt"
    source_path = ROOT / "recomp/generated" / BUILD_ID / "functions" / f"func_{ENTRY:08X}.cpp"
    main_source_path = ROOT / "recomp/generated" / BUILD_ID / "functions" / "func_802D5150.cpp"
    dol_path = ROOT / "recomp/input/RM3E01_00/main.dol"
    indirect_target = 0x804C0000 - 24932
    target_words = read_dol_words(dol_path, indirect_target, 2)
    symbols: dict[int, str] = {}
    symbol_re = re.compile(r"^([^\s=]+)\s*=\s*\.\w+:0x([0-9A-Fa-f]+);\s*// type:function")
    for line in symbols_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = symbol_re.match(line)
        if match:
            symbols.setdefault(int(match.group(2), 16), match.group(1))

    call_re = re.compile(r"InvokeDirectCpu<0x([0-9A-Fa-f]+)u>\(ctx\)")
    def calls_in(path: Path) -> list[dict[str, object]]:
        calls = []
        for index, match in enumerate(call_re.finditer(path.read_text(encoding="utf-8")), 1):
            address = int(match.group(1), 16)
            label = symbols.get(address, f"func_{address:08X}")
            translated = (path.parent / f"func_{address:08X}.cpp").exists()
            calls.append({
                "order_in_emitted_source": index,
                "address": f"0x{address:08X}",
                "symbol": label,
                "has_generated_translation": translated,
            })
        return calls

    calls = calls_in(source_path)
    main_calls = calls_in(main_source_path)

    doc = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "revision": "RM3E01_00",
        "entry_point": f"0x{ENTRY:08X}",
        "input_dol_sha256": sha256(dol_path),
        "entry_source": str(source_path.relative_to(ROOT)).replace("\\", "/"),
        "entry_source_sha256": sha256(source_path),
        "main_source": str(main_source_path.relative_to(ROOT)).replace("\\", "/"),
        "main_source_sha256": sha256(main_source_path),
        "symbol_map": str(symbols_path.relative_to(ROOT)).replace("\\", "/"),
        "symbol_map_sha256": sha256(symbols_path),
        "direct_calls_in_source_order": calls,
        "main_direct_calls_in_source_order": main_calls,
        "main_interpretation": "Source-order calls emitted for main at 0x802D5150. Branches can skip calls; presence of generated bodies proves translation output only, not correctness, runtime availability, or first-screen arrival.",
        "indirect_call_observed": {
            "address_expression": f"0x804C0000 - 24932 = 0x{indirect_target:08X}",
            "call_site": "0x800063C0",
            "target_address": f"0x{indirect_target:08X}",
            "target_is_registered_function_entry": indirect_target in symbols and (source_path.parent / f"func_{indirect_target:08X}.cpp").exists(),
            "symbol": symbols.get(indirect_target),
            "target_instruction_words": target_words,
            "target_instruction_interpretation": "stwu r1,-4(r1); stw r3,0(r1)",
            "note": "The target equals the registered InitMetroTRK entry. Ghidra's decompilation of __start identifies this indirect branch as InitMetroTRK; runtime branch conditions still determine whether this debug path executes.",
        },
        "interpretation": "Static source-order inventory only. It is not a dynamic trace, does not prove branch conditions or first-screen reachability, and does not validate HLE semantics or graphics output.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {args.output.relative_to(ROOT)} ({len(calls)} direct call sites)")
    for call in calls:
        status = "translated source exists" if call["has_generated_translation"] else "NO generated source"
        print(f"{call['order_in_emitted_source']:2}: {call['address']} {call['symbol']} [{status}]")
    print("main direct-call sites:")
    for call in main_calls:
        status = "translated source exists" if call["has_generated_translation"] else "NO generated source"
        print(f"{call['order_in_emitted_source']:2}: {call['address']} {call['symbol']} [{status}]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
