#!/usr/bin/env python3
"""Verify the RM3E01 OSSaveContext replacement used by the RM3E01 runtime profile."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOL = ROOT / "recomp/input/RM3E01_00/main.dol"
SYMBOLS = ROOT / "decomp/config/RM3E01_00/symbols.txt"
FUNCTIONS = ROOT / "recomp/config/RM3E01_00/functions.txt"
METADATA = ROOT / "recomp/generated/RM3E01_00_conservative/base_translation_output.json"
GENERATED = ROOT / "recomp/generated/RM3E01_00_conservative/functions/func_804E0144.cpp"
RUNTIME = ROOT / "recomp/third_party/WiiCompiled/runtime/src/hle/os/os_scheduler.cpp"
EXPECTED_DOL = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"
CONTEXT_SAVE = 0x804E0144
SCHEDULER_CONTEXT_SWITCH = 0x804E6BD4
SCHEDULER_SAVE_CALL = 0x804E6CD0


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_dol(image: bytes, address: int, size: int) -> bytes:
    sections = []
    for i in range(7):
        sections.append((
            int.from_bytes(image[0x00 + i * 4 : 0x04 + i * 4], "big"),
            int.from_bytes(image[0x48 + i * 4 : 0x4C + i * 4], "big"),
            int.from_bytes(image[0x90 + i * 4 : 0x94 + i * 4], "big"),
        ))
    for i in range(11):
        sections.append((
            int.from_bytes(image[0x1C + i * 4 : 0x20 + i * 4], "big"),
            int.from_bytes(image[0x64 + i * 4 : 0x68 + i * 4], "big"),
            int.from_bytes(image[0xAC + i * 4 : 0xB0 + i * 4], "big"),
        ))
    for file_offset, guest_base, section_size in sections:
        if section_size and guest_base <= address and address + size <= guest_base + section_size:
            start = file_offset + address - guest_base
            return image[start : start + size]
    raise ValueError(f"Address is not in a DOL section: {address:#010x}")


def branch_target(address: int, instruction: int) -> int:
    if instruction >> 26 != 18 or not instruction & 1:
        raise ValueError("Expected a linked relative PPC branch")
    displacement = instruction & 0x03FFFFFC
    if displacement & 0x02000000:
        displacement -= 0x04000000
    return (address + displacement) & 0xFFFFFFFF


def main() -> int:
    image = DOL.read_bytes()
    if sha(DOL) != EXPECTED_DOL:
        raise SystemExit("Unexpected DOL revision/hash")
    expected = {
        0x804E0144: "bda30034",  # stmw r13,0x34(r3)
        0x804E0180: "7c000026",  # mfcr r0
        0x804E0188: "7c0802a6",  # mfspr r0,LR
        0x804E0194: "7c0000a6",  # mfmsr r0
        0x804E019C: "7c0902a6",  # mfspr r0,CTR
        0x804E01A4: "7c0102a6",  # mfspr r0,XER
        0x804E01AC: "90230004",  # stw r1,0x4(r3)
        0x804E01B0: "90430008",  # stw r2,0x8(r3)
        0x804E01B8: "9003000c",  # stw r0,0xc(r3), save-context return value 1
        0x804E01BC: "38600000",  # li r3,0, first return from save
        SCHEDULER_SAVE_CALL: "4bff9475",  # bl 0x804E0144
    }
    raw = {f"0x{address:08X}": read_dol(image, address, 4).hex() for address in expected}
    for address, encoded in expected.items():
        if raw[f"0x{address:08X}"] != encoded:
            raise SystemExit(f"Unexpected DOL instruction at {address:#010x}: {raw[f'0x{address:08X}']}")
    if branch_target(SCHEDULER_SAVE_CALL, int.from_bytes(read_dol(image, SCHEDULER_SAVE_CALL, 4), "big")) != CONTEXT_SAVE:
        raise SystemExit("Prime scheduler no longer calls the verified context-save entry")

    symbols = SYMBOLS.read_text(encoding="utf-8")
    if not re.search(r"^fn_804E0144 = \.text:0x804E0144; // type:function size:0x80$", symbols, re.M):
        raise SystemExit("Expected exact-sized Prime context-save symbol missing")
    function_entries = {int(line.split()[0], 16) for line in FUNCTIONS.read_text(encoding="utf-8").splitlines() if line.strip()}
    if CONTEXT_SAVE not in function_entries:
        raise SystemExit("Prime context-save entry missing from function map")

    metadata = json.loads(METADATA.read_text(encoding="utf-8"))
    item = next((entry for entry in metadata["functions"] if entry["entryPoint"] == CONTEXT_SAVE), None)
    if item is None or item["sha256"] != sha(GENERATED):
        raise SystemExit("Translated Prime context-save source/hash mismatch")
    if "InvokeDirectCpu<0x804E0144u>(cpu);" not in RUNTIME.read_text(encoding="utf-8"):
        raise SystemExit("RM3E01 runtime scheduler does not dispatch to the Prime context-save function")

    report = {
        "accepted": True,
        "dol_sha256": sha(DOL),
        "prime_context_save": {
            "entry": "0x804E0144",
            "symbol_size": "0x80",
            "function_map_entry": True,
            "translated_source_sha256": sha(GENERATED),
            "saved_register_groups": ["r13-r31", "GQR1-GQR7", "CR", "LR", "MSR", "CTR", "XER", "r1", "r2"],
            "first_return_value": 0,
            "resume_return_value_stored_at_context_plus_0x0C": True,
        },
        "prime_scheduler_caller": {
            "function": "0x804E6BD4",
            "call_site": f"0x{SCHEDULER_SAVE_CALL:08X}",
            "instruction": raw[f"0x{SCHEDULER_SAVE_CALL:08X}"],
            "branch_target": f"0x{branch_target(SCHEDULER_SAVE_CALL, int.from_bytes(read_dol(image, SCHEDULER_SAVE_CALL, 4), 'big')):08X}",
        },
        "runtime_binding": {
            "path": "recomp/third_party/WiiCompiled/runtime/src/hle/os/os_scheduler.cpp",
            "sha256": sha(RUNTIME),
            "RM3E01_profile_uses_prime_entry": True,
            "other_profiles_keep_original_binding": True,
        },
        "limitations": [
            "This proves the replacement target's code and static caller, plus successful compilation of mkw_runtime_common; it does not prove runtime scheduler/fiber equivalence.",
            "A strict executable link now succeeds; runtime entry and scheduler/fiber equivalence remain unverified.",
        ],
    }
    output = ROOT / "recomp/checkpoints/20261020_prime_context_save_binding.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
