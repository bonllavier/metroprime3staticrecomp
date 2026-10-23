#!/usr/bin/env python3
"""Compare an original RSO with the original SDK's relocated module in a private RAM dump."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import inspect_startup_rso
import inspect_static_sel


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def word(data: bytes, offset: int = 0) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def audit(rso: Path, dump: Path, base: int) -> dict:
    original = rso.read_bytes()
    module = inspect_startup_rso.inspect_bytes(original)
    dol = (ROOT / "decomp/orig/RM3E01_00/sys/main.dol").read_bytes()
    assert sha(dol) == "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"
    sel_raw = (ROOT / "decomp/orig/RM3E01_00/files/RSO/Rev2 Production/selfile.sel").read_bytes()
    assert hashlib.sha1(sel_raw).hexdigest() == inspect_static_sel.SEL_SHA1
    exports = inspect_static_sel.inspect_bytes(sel_raw, inspect_static_sel.section_bases(dol))
    symbols = {item["name"]: item["address"] for item in exports["exports"]}
    with dump.open("rb") as stream:
        def read(address: int, size: int) -> bytes:
            assert dump.stat().st_size in (0x1800000, 0x8000000), "Expected a complete MEM1 or MEM2 dump"
            physical_offset = (address & 0x3FFFFFFF) - (0 if dump.stat().st_size == 0x1800000 else 0x10000000)
            assert physical_offset >= 0 and physical_offset + size <= dump.stat().st_size
            stream.seek(physical_offset)
            data = stream.read(size)
            assert len(data) == size
            return data

        live = read(base, len(original))
        table = word(live, 0xC)
        assert word(live, 8) == len(module["sections"])
        sections = []
        for section in module["sections"]:
            address, size = struct.unpack(">II", read(table + section["index"] * 8, 8))
            assert size == section["size"]
            if section["file_offset"] and size:
                assert address == base + section["file_offset"]
            sections.append({**section, "runtime_address": address})

        thunks: dict[int, dict] = {}
        def thunk_target(address: int) -> int | None:
            if not (0x80000000 <= address < 0x81800000 or 0x90000000 <= address < 0x98000000):
                return None
            values = struct.unpack(">6I", read(address, 24))
            if (values[0] != 0x91810004 or values[1] & 0xFFFF0000 != 0x3D800000 or
                values[2] & 0xFFFF0000 != 0x398C0000 or values[3:] != (0x7D8903A6, 0x81810004, 0x4E800420)):
                return None
            target = ((values[1] & 0xFFFF) << 16) + struct.unpack(">h", struct.pack(">H", values[2] & 0xFFFF))[0]
            target &= 0xFFFFFFFF
            thunks[address] = {"address": hex(address), "target": hex(target),
                               "instruction_sha256": sha(read(address, 24))}
            return target

        expected_sections = {section["index"]: bytearray(original[section["file_offset"]:section["file_offset"] + section["size"]])
                             for section in sections if section["file_offset"] and section["size"]}
        checked, mismatches = [], []
        for group in ("internal", "external"):
            for relocation in module["relocations"][group]:
                section = sections[relocation["source_section"]]
                offset = relocation["source_section_offset"]
                source = section["runtime_address"] + offset
                typ = relocation["type"]
                if group == "internal":
                    target = sections[relocation["index"]]["runtime_address"] + relocation["target_offset"]
                    symbol = None
                else:
                    symbol = module["imports"][relocation["index"]]["name"]
                    target = symbols[symbol] + relocation["target_offset"]
                via_thunk = False
                if typ == 1:
                    actual = word(read(source, 4))
                    via_thunk = group == "external" and actual != target and thunk_target(actual) == target
                    good = actual == target or via_thunk
                    patch = struct.pack(">I", actual if good else target)
                elif typ in (4, 6):
                    expected = target & 0xFFFF if typ == 4 else ((target + 0x8000) >> 16) & 0xFFFF
                    actual = struct.unpack(">H", read(source, 2))[0]
                    good = actual == expected
                    patch = struct.pack(">H", expected)
                elif typ == 10:
                    actual = word(read(source, 4))
                    displacement = actual & 0x03FFFFFC
                    if displacement & 0x02000000:
                        displacement -= 0x04000000
                    destination = (source + displacement) & 0xFFFFFFFF
                    via_thunk = group == "external" and destination != target and thunk_target(destination) == target
                    original_instruction = word(original, relocation["source_file_offset"])
                    good = (actual & 0xFC000003) == (original_instruction & 0xFC000003) and (destination == target or via_thunk)
                    patch = struct.pack(">I", actual if good else ((original_instruction & 0xFC000003) | ((target - source) & 0x03FFFFFC)))
                else:
                    raise ValueError(f"Unsupported relocation type {typ}")
                expected_sections[section["index"]][offset:offset + len(patch)] = patch
                row = {"source": hex(source), "section": section["index"], "offset": offset,
                       "type": typ, "group": group, "target": hex(target), "symbol": symbol,
                       "via_verified_thunk": via_thunk, "matches": good}
                checked.append(row)
                if not good:
                    mismatches.append(row)

        contents = []
        for index, expected in expected_sections.items():
            section = sections[index]
            actual = read(section["runtime_address"], section["size"])
            differences = [offset for offset, pair in enumerate(zip(expected, actual)) if pair[0] != pair[1]]
            contents.append({"index": index, "size": len(actual), "expected_sha256": sha(expected),
                             "runtime_sha256": sha(actual), "different_bytes": len(differences),
                             "first_differences": differences[:12]})
        bss = []
        for section in sections:
            if section["bss"] and section["size"]:
                data = read(section["runtime_address"], section["size"])
                bss.append({"index": section["index"], "address": hex(section["runtime_address"]),
                            "size": len(data), "all_zero_before_prolog": not any(data), "sha256": sha(data)})
        prolog = module["entry_points"]["prolog"]
        prolog_address = sections[prolog["section"]]["runtime_address"] + prolog["offset"]
        ctor_section = sections[2]
        ctors = list(struct.unpack(f">{ctor_section['size'] // 4}I", read(ctor_section["runtime_address"], ctor_section["size"])))

    return {"revision": "RM3E01_00", "module": rso.name, "original_sha256": sha(original),
            "runtime_base": hex(base), "prolog_address": hex(prolog_address),
            "dump_sha256": sha(dump.read_bytes()), "dump_path": str(dump),
            "sections": sections, "relocations_checked": len(checked), "mismatches": mismatches,
            "verified_import_thunks": list(thunks.values()), "section_contents": contents,
            "bss": bss, "constructor_entries": [hex(value) for value in ctors if value],
            "source": "Original SDK linker output in a private runtime RAM dump; not host execution of these PPC functions.",
            "all_relocations_match": not mismatches,
            "all_section_contents_match": all(row["different_bytes"] == 0 for row in contents),
            "game_screen_verified": False}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rso", type=Path)
    parser.add_argument("mem2_dump", type=Path)
    parser.add_argument("--base", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = audit(args.rso, args.mem2_dump, args.base)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("module", "prolog_address", "relocations_checked", "all_relocations_match", "all_section_contents_match", "constructor_entries")}, indent=2))


if __name__ == "__main__":
    main()
