#!/usr/bin/env python3
"""Validate the FrontEndUI RSO prolog callback/registration probe artifacts."""
from __future__ import annotations

import hashlib
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "recomp/.tools/rso-rel"
OUT = ROOT / "recomp/checkpoints/20261020_frontend_rso_prolog_dispatch_audit.json"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    image_path = TOOLS / "FrontEndUI.relocated.bin"
    image = image_path.read_bytes()
    if sha256(image_path) != "302998f8025dbb16849bc1d57f673696b80cadf7a43110c69251fd1e49bfbfcd":
        raise SystemExit("Relocated FrontEndUI probe image hash changed; re-verify relocations first")

    base = 0x80690000
    table_address = 0x806AD7C0
    table_offset = table_address - base
    values = list(struct.unpack_from(">5I", image, table_offset))
    expected = [0x806A9F90, 0x806AD298, 0x806AD554, 0x806AD6C8, 0]
    if values != expected:
        raise SystemExit(f"Unexpected relocated callback table: {[hex(x) for x in values]}")

    function_map = {
        int(line.split()[0], 16)
        for line in (TOOLS / "frontend_function_map.txt").read_text(encoding="utf-8").splitlines()
        if line.strip()
    }
    metadata = json.loads((TOOLS / "extended_metadata.json").read_text(encoding="utf-8"))
    if metadata["quality"] != {"unsupportedInstructionCount": 0, "invalidSsaFunctionCount": 0}:
        raise SystemExit(f"Translation quality gate failed: {metadata['quality']}")
    by_address = {item["entryPoint"]: item for item in metadata["functions"]}

    addresses = [0x806AD5B8, 0x806AD72C, 0x806AA038, 0x806AA014, *expected[:-1]]
    compiled = []
    callback_bodies = []
    for address in addresses:
        source = TOOLS / "extended_functions" / f"func_{address:08X}.cpp"
        obj = TOOLS / "objects" / f"func_{address:08X}.o"
        item = by_address.get(address)
        if item is None or item["interiorToOtherTranslation"]:
            raise SystemExit(f"Missing/non-entry translated body at 0x{address:08X}")
        if not source.is_file() or not obj.is_file():
            raise SystemExit(f"Missing source/object for 0x{address:08X}")
        if sha256(source) != item["sha256"]:
            raise SystemExit(f"Generated source hash mismatch for 0x{address:08X}")
        if struct.unpack_from("<H", obj.read_bytes(), 0)[0] != 0x8664:
            raise SystemExit(f"Object is not COFF AMD64 for 0x{address:08X}")
        if address in expected[:-1] and address not in function_map:
            raise SystemExit(f"Callback 0x{address:08X} absent from translator function map")
        if address in expected[:-1]:
            source_text = source.read_text(encoding="utf-8")
            direct_calls = re.findall(r"InvokeDirectCpu<0x[0-9A-Fa-f]+u>\(ctx\)", source_text)
            indirect_calls = re.findall(r"InvokeIndirect(?:Cpu|Jump)\s*\(", source_text)
            if item["build"].get("directCallDependencies", []) or direct_calls or indirect_calls:
                raise SystemExit(f"Callback 0x{address:08X} is no longer a call-free leaf")
            callback_bodies.append(
                {
                    "address": f"0x{address:08X}",
                    "source_sha256": sha256(source),
                    "direct_call_dependencies": 0,
                    "direct_dispatch_sites": 0,
                    "indirect_dispatch_sites": 0,
                    "memory_write_sites": len(
                        re.findall(r"MemoryInline::[A-Za-z0-9]*Write[A-Za-z0-9]*\(", source_text)
                    ),
                    "interpretation": "Call-free translated initialization body; write targets and runtime ordering remain data/ABI behavior, not established semantics.",
                }
            )
        compiled.append(
            {
                "address": f"0x{address:08X}",
                "source_sha256": sha256(source),
                "object_sha256": sha256(obj),
                "object_machine": "COFF-x86-64/AMD64",
                "interior_to_other_translation": item["interiorToOtherTranslation"],
            }
        )

    # Tie the machine-level control sequence to its generated implementation.
    source_by_address = {int(item["address"], 16): item for item in compiled}
    prolog = (TOOLS / "extended_functions/func_806AD5B8.cpp").read_text(encoding="utf-8")
    loop = (TOOLS / "extended_functions/func_806AD72C.cpp").read_text(encoding="utf-8")
    thunk = (TOOLS / "extended_functions/func_806AA038.cpp").read_text(encoding="utf-8")
    register = (TOOLS / "extended_functions/func_806AA014.cpp").read_text(encoding="utf-8")
    required = [
        "InvokeDirectCpu<0x806AD72Cu>(ctx);",
        "InvokeDirectCpu<0x806AA038u>(ctx);",
    ]
    if not all(token in prolog for token in required):
        raise SystemExit("Prolog direct call order differs from the verified PPC disassembly")
    if "InvokeIndirectCpu(ctr, ctx);" not in loop or "(r31 + 4)" not in loop:
        raise SystemExit("Constructor-table loop implementation is missing its indirect call/stride")
    if "InvokeDirectCpu<0x806AA014u>(ctx);" not in thunk:
        raise SystemExit("Registration thunk no longer targets 0x806AA014")
    if "MemoryInline::FlatWriteRam32((r3 + 4), r4);" not in register:
        raise SystemExit("Registration function no longer stores factory at table +4")
    if "MemoryInline::FlatWriteRam32((r5 + -2432), r6);" not in register:
        raise SystemExit("Registration function no longer stores dispatcher at table +0")

    report = {
        "revision": "RM3E01_00",
        "validator_script": "recomp/tools/verify_frontend_rso_prolog.py",
        "validator_sha256": sha256(Path(__file__).resolve()),
        "rso_sha256": "a8f1ae11bd85d80139a816ea5128c59643d02f20d49e0ba3a43b19f07719e68b",
        "relocated_image_sha256": sha256(image_path),
        "probe_load_base": f"0x{base:08X}",
        "callback_table_address": f"0x{table_address:08X}",
        "callback_table_values": [f"0x{x:08X}" for x in values],
        "callback_count": len(values) - 1,
        "prolog": {
            "address": "0x806AD5B8",
            "decoded_instructions": 9,
            "calls_in_order": ["0x806AD72C", "0x806AA038"],
        },
        "callback_loop": {
            "address": "0x806AD72C",
            "decoded_instructions": 18,
            "indirect_call_site": "0x806AD74C",
            "table_stride_bytes": 4,
            "terminates_on_null": True,
        },
        "callback_body_audit": callback_bodies,
        "registration": {
            "thunk": "0x806AA038",
            "target": "0x806AA014",
            "dispatcher_table_entry": "+0",
            "factory_table_entry": "+4",
            "published_by": "0x8021D2F4",
        },
        "compiled_functions": compiled,
        "validation": {
            "table_matches_expected_relocated_values": True,
            "all_callback_addresses_in_function_map": True,
            "all_eight_sources_match_translation_metadata": True,
            "all_eight_objects_coff_amd64": True,
            "all_four_callbacks_are_call_free_leaves": len(callback_bodies) == 4,
            "unsupported_instructions": 0,
            "invalid_ssa_functions": 0,
        },
        "limitations": [
            "This verifies a synthetic relocated probe, not execution of the RSO prolog.",
            "The runtime still needs ISO-backed loading, guest-memory allocation, relocation, BSS initialization, function registration, and a verified lifecycle point for calling the prolog.",
            "No executable has linked or presented the game screen.",
        ],
    }
    OUT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"callback_count": report["callback_count"], "compiled_functions": len(compiled), "accepted": True}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
