#!/usr/bin/env python3
"""Compare WiiCompiled REL-loader output with RM3E01 RSO linker relocation rules."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import inspect_startup_rso  # noqa: E402
import inspect_static_sel  # noqa: E402


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & ~(boundary - 1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rso", type=Path)
    parser.add_argument("synthetic_rel", type=Path)
    parser.add_argument("relocated_image", type=Path)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=0x80690000)
    parser.add_argument("--output", type=Path, default=ROOT / "recomp/checkpoints/20261020_frontend_rso_relocation_verify.json")
    args = parser.parse_args()

    raw = args.rso.read_bytes()
    module = inspect_startup_rso.inspect_bytes(raw)
    rel = args.synthetic_rel.read_bytes()
    image = args.relocated_image.read_bytes()
    if u32(rel, 0x0C) != len(module["sections"]):
        raise ValueError("Synthetic REL section count differs from inspected RSO")
    section_count = u32(rel, 0x0C)
    section_table = u32(rel, 0x10)
    sections = []
    for index in range(section_count):
        flags, size = struct.unpack_from(">II", rel, section_table + index * 8)
        sections.append({"file_offset": flags & ~1, "size": size})
    bss_size = u32(rel, 0x20)
    bss_index = rel[0x33]
    bss_offset = align(len(rel) + 0x20, 0x20)
    if len(image) != bss_offset + bss_size:
        raise ValueError("WiiCompiled REL image size differs from its deterministic BSS placement")
    if bss_index != 0xFF:
        sections[bss_index]["file_offset"] = bss_offset

    dol = (ROOT / "decomp/orig/RM3E01_00/sys/main.dol").read_bytes()
    sel = inspect_static_sel.inspect_bytes(
        (ROOT / "decomp/orig/RM3E01_00/files/RSO/Rev2 Production/selfile.sel").read_bytes(),
        inspect_static_sel.section_bases(dol),
    )
    sel_addresses = {item["name"]: item["address"] for item in sel["exports"]}
    external_symbols = [item["name"] for item in module["imports"]]
    relocations = module["relocations"]["internal"] + module["relocations"]["external"]
    seen_fields: set[tuple[int, int, int]] = set()
    type_counts: dict[str, int] = {}
    checked = 0
    rel24_checked = 0
    for relocation in relocations:
        source_index = relocation["source_section"]
        source_offset = relocation["source_section_offset"]
        source_file_offset = sections[source_index]["file_offset"] + source_offset
        source_address = args.base + source_file_offset
        rel_type = relocation["type"]
        key = (source_index, source_offset, rel_type)
        if key in seen_fields:
            raise ValueError(f"Duplicate relocation at section {source_index}+0x{source_offset:X}")
        seen_fields.add(key)

        if relocation in module["relocations"]["internal"]:
            target_index = relocation["index"]
            target_address = args.base + sections[target_index]["file_offset"]
        else:
            target_index = relocation["index"]
            symbol = external_symbols[target_index]
            if symbol not in sel_addresses:
                raise ValueError(f"Unresolved external symbol {symbol}")
            target_address = sel_addresses[symbol]
        target = target_address + relocation["target_offset"]

        if rel_type == 1:
            expected = struct.pack(">I", target & 0xFFFFFFFF)
            width = 4
        elif rel_type == 4:
            expected = struct.pack(">H", target & 0xFFFF)
            width = 2
        elif rel_type == 6:
            expected = struct.pack(">H", ((target + 0x8000) >> 16) & 0xFFFF)
            width = 2
        elif rel_type == 10:
            original = u32(raw, relocation["source_file_offset"])
            displacement = target - source_address
            if displacement % 4 or displacement < -0x02000000 or displacement > 0x01FFFFFC:
                raise ValueError(f"REL24 target is outside PowerPC branch range at 0x{source_address:08X}")
            expected_word = (original & 0xFC000003) | (displacement & 0x03FFFFFC)
            expected = struct.pack(">I", expected_word)
            width = 4
            rel24_checked += 1
        else:
            raise ValueError(f"Unsupported relocation type {rel_type}")

        actual = image[source_file_offset:source_file_offset + width]
        if actual != expected:
            raise ValueError(
                f"Relocation mismatch type {rel_type} at 0x{source_address:08X}: "
                f"expected {expected.hex()}, got {actual.hex()}"
            )
        checked += 1
        type_counts[str(rel_type)] = type_counts.get(str(rel_type), 0) + 1

    # The original ctor section supplies the table for each individual module.
    pointer_table_offset = sections[2]["file_offset"]
    pointer_table_address = args.base + pointer_table_offset
    pointer_values = [u32(image, pointer_table_offset + i * 4) for i in range(sections[2]["size"] // 4)]
    assert pointer_values and pointer_values[-1] == 0
    text_start = args.base + sections[1]["file_offset"]
    assert all(text_start <= value < text_start + sections[1]["size"] for value in pointer_values[:-1])
    result = {
        "revision": "RM3E01_00",
        "rso_sha256": hashlib.sha256(raw).hexdigest(),
        "synthetic_rel_sha256": hashlib.sha256(rel).hexdigest(),
        "relocated_image_sha256": hashlib.sha256(image).hexdigest(),
        "base_address": f"0x{args.base:08X}",
        "relocations_checked": checked,
        "rel24_out_of_range": False,
        "rel24_checked": rel24_checked,
        "relocation_type_counts": type_counts,
        "comparison_basis": "Independent PPC relocation expressions transcribed from Ghidra decompile of RM3E01_00 RSOLink apply helper 0x805414F4; image produced by WiiCompiled Translator.Core RelFile.BuildImage.",
        "constructor_pointer_table": {
            "address": f"0x{pointer_table_address:08X}",
            "values": [f"0x{value:08X}" for value in pointer_values],
            "interpretation": "Section 2 relocated text pointers followed by a verified null terminator; not a callback execution claim.",
        },
        "scope": "Checks relocation fields only; does not prove function boundaries, callback order, host ABI, RSO runtime loading, or screen execution.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in (
        "relocations_checked", "rel24_checked", "relocation_type_counts",
        "constructor_pointer_table")}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
