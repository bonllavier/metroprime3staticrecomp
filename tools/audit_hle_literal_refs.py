"""Scan RM3E01_00 DOL sections for aligned words equal to active HLE addresses."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from collections import Counter, defaultdict
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dol", type=Path)
    parser.add_argument("active_hle_audit", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    blob = args.dol.read_bytes()
    audit = json.loads(args.active_hle_audit.read_text(encoding="utf-8-sig"))
    if sha256(args.dol) != "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104":
        raise SystemExit("DOL hash does not match pinned RM3E01_00")
    addresses = {int(value, 16) for value in audit["registered_addresses"]}

    text_offsets = struct.unpack_from(">7I", blob, 0x00)
    data_offsets = struct.unpack_from(">11I", blob, 0x1C)
    text_bases = struct.unpack_from(">7I", blob, 0x48)
    data_bases = struct.unpack_from(">11I", blob, 0x64)
    text_sizes = struct.unpack_from(">7I", blob, 0x90)
    data_sizes = struct.unpack_from(">11I", blob, 0xAC)

    hits: dict[int, list[dict[str, object]]] = defaultdict(list)
    section_counts: Counter[str] = Counter()
    for kind, offsets, bases, sizes in (
        ("text", text_offsets, text_bases, text_sizes),
        ("data", data_offsets, data_bases, data_sizes),
    ):
        for index, (file_offset, guest_base, size) in enumerate(zip(offsets, bases, sizes)):
            if size == 0:
                continue
            section_counts[kind] += 1
            if guest_base & 3:
                continue
            end = file_offset + size - 3
            for pos in range(file_offset, end, 4):
                value = struct.unpack_from(">I", blob, pos)[0]
                if value in addresses:
                    hits[value].append({
                        "section_kind": kind,
                        "section_index": index,
                        "dol_file_offset": f"0x{pos:X}",
                        "guest_location": f"0x{guest_base + pos - file_offset:08X}",
                    })

    hit_list = [
        {"address": f"0x{address:08X}", "occurrences": occurrences}
        for address, occurrences in sorted(hits.items())
    ]
    report = {
        "revision": "RM3E01_00",
        "dol_sha256": sha256(args.dol),
        "active_hle_audit": str(args.active_hle_audit),
        "active_hle_audit_sha256": sha256(args.active_hle_audit),
        "active_hle_addresses": len(addresses),
        "scan_scope": "Aligned big-endian 32-bit words in all loaded DOL text and data sections.",
        "loaded_section_counts": dict(section_counts),
        "addresses_with_literal_word_hits": len(hit_list),
        "occurrence_count": sum(len(item["occurrences"]) for item in hit_list),
        "hit_occurrences_by_section_kind": dict(Counter(
            occurrence["section_kind"]
            for item in hit_list
            for occurrence in item["occurrences"]
        )),
        "hits": hit_list,
        "interpretation": "No aligned data-section word would directly identify an HLE address as a stored function pointer. Text-section matches may be instruction encodings or immediates and need disassembly; this scan does not detect split immediates, computed pointers, REL/RSO references, or dynamic callbacks.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "active_hle_addresses": len(addresses),
        "addresses_with_literal_word_hits": len(hit_list),
        "occurrence_count": report["occurrence_count"],
        "hit_occurrences_by_section_kind": report["hit_occurrences_by_section_kind"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
