#!/usr/bin/env python3
"""Verify the WiiCompiled PPCMtdec address against pinned RM3E01_00 bytes/symbols."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOL = ROOT / "recomp/input/RM3E01_00/main.dol"
SYMBOLS = ROOT / "decomp/config/RM3E01_00/symbols.txt"
FUNCTIONS = ROOT / "recomp/config/RM3E01_00/functions.txt"
SOURCE = ROOT / "recomp/third_party/WiiCompiled/runtime/src/hle/os/os_init.cpp"
EXPECTED_DOL_SHA256 = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"
TARGET = 0x8012E594
FUNCTION_START = 0x8012E3EC
FUNCTION_SIZE = 0x250
EXPECTED_INSTRUCTION = bytes.fromhex("40800070")


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_dol_address(image: bytes, address: int, size: int) -> bytes:
    text_offsets = [int.from_bytes(image[i : i + 4], "big") for i in range(0x00, 0x1C, 4)]
    data_offsets = [int.from_bytes(image[i : i + 4], "big") for i in range(0x1C, 0x48, 4)]
    text_addrs = [int.from_bytes(image[i : i + 4], "big") for i in range(0x48, 0x64, 4)]
    data_addrs = [int.from_bytes(image[i : i + 4], "big") for i in range(0x64, 0x90, 4)]
    text_sizes = [int.from_bytes(image[i : i + 4], "big") for i in range(0x90, 0xAC, 4)]
    data_sizes = [int.from_bytes(image[i : i + 4], "big") for i in range(0xAC, 0xD8, 4)]
    sections = list(zip(text_offsets, text_addrs, text_sizes)) + list(
        zip(data_offsets, data_addrs, data_sizes)
    )
    for offset, base, section_size in sections:
        if section_size and base <= address and address + size <= base + section_size:
            file_offset = offset + address - base
            return image[file_offset : file_offset + size]
    raise ValueError(f"Address is not backed by a DOL section: {address:#010x}")


def main() -> int:
    image = DOL.read_bytes()
    dol_sha = sha(DOL)
    if dol_sha != EXPECTED_DOL_SHA256:
        raise SystemExit("Pinned RM3E01_00 DOL SHA-256 mismatch")

    raw = read_dol_address(image, TARGET, 4)
    if raw != EXPECTED_INSTRUCTION:
        raise SystemExit(f"Unexpected instruction at PPCMtdec binding address: {raw.hex()}")

    symbols_text = SYMBOLS.read_text(encoding="utf-8")
    symbol_match = re.search(
        r"^PursuePlayer__10CPatternedFR13CStateManager9EStateMsgff = \.text:0x([0-9A-Fa-f]+);\s*// type:function size:0x([0-9A-Fa-f]+)",
        symbols_text,
        re.MULTILINE,
    )
    if not symbol_match:
        raise SystemExit("Expected sized Prime function symbol is missing")
    start, size = (int(value, 16) for value in symbol_match.groups())
    if (start, size) != (FUNCTION_START, FUNCTION_SIZE) or not start <= TARGET < start + size:
        raise SystemExit("Prime function boundary evidence changed")

    function_starts = {
        int(line.split()[0], 16)
        for line in FUNCTIONS.read_text(encoding="utf-8").splitlines()
        if line.strip()
    }
    if TARGET in function_starts:
        raise SystemExit("PPCMtdec target unexpectedly became an exact Prime function entry")

    source = SOURCE.read_text(encoding="utf-8")
    registration = re.search(
        r"PPC_NATIVE_OVERRIDE_VOID\(8012E594,\s*PPCMtdec_8012e594,\s*\(\),\s*\(\)\s*\);",
        source,
    )
    if not registration:
        raise SystemExit("Expected WiiCompiled PPCMtdec fixed-address registration changed")
    if "We don't simulate decrementer exceptions" not in source:
        raise SystemExit("Expected inherited runtime limitation statement changed")

    report = {
        "accepted": True,
        "dol_sha256": dol_sha,
        "target": f"0x{TARGET:08X}",
        "instruction": {"bytes": raw.hex(), "mnemonic": "bge 0x8012E604", "verified_from_dol": True},
        "prime_containing_function": {
            "name": "PursuePlayer__10CPatternedFR13CStateManager9EStateMsgff",
            "start": f"0x{start:08X}",
            "end_exclusive": f"0x{start + size:08X}",
            "offset": f"+0x{TARGET - start:X}",
            "exact_function_entry": False,
        },
        "wiicompiled_registration": {
            "source": "recomp/third_party/WiiCompiled/runtime/src/hle/os/os_init.cpp",
            "sha256": sha(SOURCE),
            "registered_address": f"0x{TARGET:08X}",
            "registration_found": True,
            "decrementer_exceptions_simulated": False,
        },
        "ghidra_observation": {
            "program_sha256": EXPECTED_DOL_SHA256,
            "xrefs_to_target": 0,
            "dynamic_reachability_proven": False,
            "note": "Ghidra disassembly identifies the instruction and containing function; no incoming references were returned.",
        },
        "interpretation": "This is an interior-address binding collision, not a verified Prime decrementer entry. It does not prove runtime invocation or interception of sequential execution.",
    }
    output = ROOT / "recomp/checkpoints/20261020_decrementer_binding_collision.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
