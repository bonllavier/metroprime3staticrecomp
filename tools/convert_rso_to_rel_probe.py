#!/usr/bin/env python3
"""Build a private synthetic REL image to reuse WiiCompiled's tested REL loader.

This is an analysis bridge only: it does not modify the RSO, ISO, DOL or translator.
It requires every RSO import to resolve through the pinned RM3E01_00 selfile.sel.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import inspect_startup_rso  # noqa: E402
import inspect_static_sel  # noqa: E402

RSO_TO_REL = {1: 1, 4: 4, 6: 6, 10: 10}
REL_SECT = 202
REL_STOP = 203
REL_NONE = 201
REL_HEADER_SIZE = 0x4C


def put_u16(buf: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">H", buf, offset, value)


def put_u32(buf: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">I", buf, offset, value & 0xFFFFFFFF)


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & ~(boundary - 1)


def emit_reloc_stream(records: list[dict], module_id: int, import_addresses: dict[int, int]) -> bytes:
    out = bytearray()
    by_source: dict[int, list[dict]] = {}
    for record in records:
        by_source.setdefault(record["source_section"], []).append(record)
    for source_section in sorted(by_source):
        out += struct.pack(">HBBI", 0, REL_SECT, source_section, 0)
        previous = 0
        for record in sorted(by_source[source_section], key=lambda item: item["source_section_offset"]):
            source_offset = record["source_section_offset"]
            if source_offset < previous:
                raise ValueError("Relocations are not monotonically ordered")
            gap = source_offset - previous
            while gap > 0xFFFF:
                out += struct.pack(">HBBI", 0xFFFF, REL_NONE, 0, 0)
                previous += 0xFFFF
                gap -= 0xFFFF
            reloc_type = RSO_TO_REL.get(record["type"])
            if reloc_type is None:
                raise ValueError(f"Unsupported RSO relocation type {record['type']}")
            if module_id == 0:
                target_section = 0
                target = import_addresses[record["index"]]
            else:
                target_section = record["index"]
                target = record["target_offset"]
            out += struct.pack(">HBBI", gap, reloc_type, target_section, target)
            previous = source_offset
    out += struct.pack(">HBBI", 0, REL_STOP, 0, 0)
    return bytes(out)


def create_probe_rso(rso_path: Path, output_path: Path) -> dict:
    raw = rso_path.read_bytes()
    module = inspect_startup_rso.inspect_bytes(raw)
    dol = (ROOT / "decomp/orig/RM3E01_00/sys/main.dol").read_bytes()
    bases = inspect_static_sel.section_bases(dol)
    sel_path = ROOT / "decomp/orig/RM3E01_00/files/RSO/Rev2 Production/selfile.sel"
    sel_raw = sel_path.read_bytes()
    if hashlib.sha1(sel_raw).hexdigest() != inspect_static_sel.SEL_SHA1:
        raise ValueError("Pinned RM3E01_00 selfile.sel hash differs")
    sel = inspect_static_sel.inspect_bytes(sel_raw, bases)
    sel_addresses = {item["name"]: item["address"] for item in sel["exports"]}
    symbols: dict[str, int] = {}
    for line in (ROOT / "decomp/config/RM3E01_00/symbols.txt").read_text(encoding="utf-8").splitlines():
        match = re.match(r"(.+?) = \S+:0x([0-9a-fA-F]+);", line)
        if match:
            symbols[match.group(1)] = int(match.group(2), 16)

    import_addresses: dict[int, int] = {}
    for index, item in enumerate(module["imports"]):
        name = item["name"]
        if name not in sel_addresses:
            raise ValueError(f"RSO import absent from pinned selfile.sel: {name}")
        address = sel_addresses[name]
        if symbols.get(name) != address:
            raise ValueError(f"selfile.sel and named DOL symbol disagree for {name}")
        import_addresses[index] = address

    internal_stream = emit_reloc_stream(module["relocations"]["internal"], 1, import_addresses)
    external_stream = emit_reloc_stream(module["relocations"]["external"], 0, import_addresses)
    streams = [(1, internal_stream), (0, external_stream)]
    streams = [(module_id, content) for module_id, content in streams if content]

    section_count = len(module["sections"])
    section_table_offset = REL_HEADER_SIZE
    import_table_offset = section_table_offset + section_count * 8
    import_table_size = len(streams) * 8
    cursor = import_table_offset + import_table_size
    import_entries = []
    for module_id, content in streams:
        import_entries.append((module_id, cursor))
        cursor += len(content)
    cursor = align(cursor, 0x20)

    section_layout = []
    bss_indices = []
    section_blobs: list[tuple[int, bytes]] = []
    for section in module["sections"]:
        if section["bss"]:
            bss_indices.append(section["index"])
            section_layout.append((0, section["size"], section["executable_flag"]))
        elif section["file_offset"]:
            cursor = align(cursor, 0x20)
            content = raw[section["file_offset"]:section["file_offset"] + section["size"]]
            if len(content) != section["size"]:
                raise ValueError("RSO section content is truncated")
            section_blobs.append((cursor, content))
            section_layout.append((cursor, section["size"], section["executable_flag"]))
            cursor += len(content)
        else:
            section_layout.append((0, section["size"], section["executable_flag"]))

    if len(bss_indices) > 1:
        raise ValueError("REL probe currently supports at most one RSO BSS section")
    bss_index = bss_indices[0] if bss_indices else 0xFF
    if sum(size for offset, size, _ in section_layout if offset == 0 and size) != module["bss_size"]:
        raise ValueError("RSO BSS section sizes do not match header")

    out = bytearray(cursor)
    put_u32(out, 0x00, 1)  # synthetic module id; internal relocations use any nonzero id
    put_u32(out, 0x0C, section_count)
    put_u32(out, 0x10, section_table_offset)
    put_u32(out, 0x1C, 1)
    put_u32(out, 0x20, module["bss_size"])
    put_u32(out, 0x28, import_table_offset)
    put_u32(out, 0x2C, import_table_size)
    for field, label in ((0x30, "prolog"), (0x31, "epilog"), (0x32, "unresolved")):
        entry = module["entry_points"].get(label)
        out[field] = entry["section"] if entry else 0
    for field, label in ((0x34, "prolog"), (0x38, "epilog"), (0x3C, "unresolved")):
        entry = module["entry_points"].get(label)
        put_u32(out, field, entry["offset"] if entry else 0)
    out[0x33] = bss_index
    put_u32(out, 0x40, 0x20)
    put_u32(out, 0x44, 0x20)

    for index, (file_offset, size, executable) in enumerate(section_layout):
        flags = file_offset | int(executable)
        struct.pack_into(">II", out, section_table_offset + index * 8, flags, size)
    for index, (module_id, relocation_offset) in enumerate(import_entries):
        struct.pack_into(">II", out, import_table_offset + index * 8, module_id, relocation_offset)
    for (module_id, content), (_, relocation_offset) in zip(streams, import_entries):
        out[relocation_offset:relocation_offset + len(content)] = content
    for file_offset, content in section_blobs:
        out[file_offset:file_offset + len(content)] = content

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(out)
    relocation_type_counts: dict[str, int] = {}
    for group in module["relocations"].values():
        for record in group:
            key = str(record["type"])
            relocation_type_counts[key] = relocation_type_counts.get(key, 0) + 1
    return {
        "revision": "RM3E01_00",
        "module": rso_path.name,
        "rso_sha256": hashlib.sha256(raw).hexdigest(),
        "synthetic_rel_sha256": hashlib.sha256(out).hexdigest(),
        "synthetic_rel_size": len(out),
        "module_load_address_for_probe": "0x80690000",
        "sections": [{"index": index, "file_offset": f"0x{offset:X}", "size": size,
                      "executable_flag": executable}
                     for index, (offset, size, executable) in enumerate(section_layout)],
        "imports_resolved_by_pinned_selfile": len(import_addresses),
        "internal_relocations": len(module["relocations"]["internal"]),
        "external_relocations": len(module["relocations"]["external"]),
        "relocation_type_counts": relocation_type_counts,
        "entry_points": module["entry_points"],
        "bss_size": module["bss_size"],
        "synthetic_rel_path": str(output_path),
        "limitations": [
            "Private translation probe only; the RSO was not modified.",
            "Uses WiiCompiled RelFile as the loader for the synthetic image; no runtime loading was added.",
            "Chosen guest load address is a probe placement and is not asserted as the game's actual RSO address.",
            "Import names/addresses were checked against the pinned static SEL and named DOL symbols.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rso", type=Path)
    parser.add_argument("output_rel", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    report = create_probe_rso(args.rso, args.output_rel)
    report_path = args.report or args.output_rel.with_suffix(".conversion.json")
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in (
        "module", "rso_sha256", "synthetic_rel_sha256", "synthetic_rel_size",
        "imports_resolved_by_pinned_selfile", "internal_relocations", "external_relocations",
        "relocation_type_counts", "entry_points")}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
