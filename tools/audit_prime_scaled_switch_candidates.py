#!/usr/bin/env python3
"""Independently locate the original eleven-entry tables of reviewed candidates.

Candidates remain private and unintegrated. This checks table identity and
source references, not native execution or complete equivalence of a function.
"""
import hashlib
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PIN = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"
ENTRIES = (0x8040D9D8, 0x8040F98C, 0x80414E20, 0x80415CB8, 0x804167CC,
           0x80419848, 0x80425B10, 0x80427A34, 0x804280AC)


def main():
    dol = (ROOT / "recomp/input/RM3E01_00/main.dol").read_bytes()
    assert hashlib.sha256(dol).hexdigest() == PIN
    sections = []
    for i in range(18):
        off, address, size = (struct.unpack_from(">I", dol, field + i * 4)[0]
                              for field in (0, 0x48, 0x90))
        if i >= 7 and size:
            assert size % 4 == 0
            words = struct.unpack_from(f">{size // 4}I", dol, off)
            sections.append((address, words))
    base = ROOT / "recomp/generated/RM3E01_00_conservative"
    metadata = json.loads((base / "base_translation_output.json").read_text())
    originals = {row["entryPoint"]: row for row in metadata["functions"]}
    private = ROOT / "recomp/.tools/dol-switch-audit"
    reports = []
    for entry in ENTRIES:
        name = f"func_{entry:08X}.cpp"
        raw = (base / "functions" / name).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == originals[entry]["sha256"]
        candidate = (private / name).read_bytes()
        facts = json.loads((private / f"func_{entry:08X}.json").read_text())
        assert hashlib.sha256(candidate).hexdigest() == facts["sha256"]
        source = candidate.decode()
        tables = []
        for switch in re.finditer(
                r"switch \(static_cast<uint32_t>\(ctr\)\) \{(.*?)default:", source, re.S):
            cases = {int(value, 16) for value in
                     re.findall(r"case 0x([0-9A-F]{8})u:", switch[1])}
            assert len(cases) == 11, (hex(entry), "unreviewed switch shape")
            matches = []
            for address, words in sections:
                index = 0
                while index < len(words):
                    if words[index] not in cases:
                        index += 1
                        continue
                    begin = index
                    while index < len(words) and words[index] in cases:
                        index += 1
                    values = words[begin:index]
                    if set(values) == cases:
                        matches.append((address + begin * 4, values))
            assert len(matches) == 1, (hex(entry), "ambiguous/missing original table", matches)
            address, values = matches[0]
            assert len(values) == 11
            # Check the candidate actually refers to this table's signed low
            # displacement and high immediate; do not accept matching data alone.
            before = source[:switch.start()]
            displacements = re.findall(r"r\d+ = \(r\d+ \+ (-?\d+)\);", before[-900:])
            low = struct.unpack(">h", struct.pack(">H", address & 0xFFFF))[0]
            assert displacements and int(displacements[-1]) == low
            high = (address + 0x8000) & 0xFFFF0000
            assert f"= 0x{high:08X}u;" in before
            assert all(value in facts["localLabelAddresses"] for value in values)
            tables.append({"address": f"0x{address:08X}", "count": len(values),
                           "unique": len(cases), "destinations": list(values),
                           "sha256": hashlib.sha256(struct.pack(">11I", *values)).hexdigest()})
        assert tables
        reports.append({"entry": f"0x{entry:08X}", "tables": tables,
                        "candidate_sha256": facts["sha256"],
                        "original_source_sha256": originals[entry]["sha256"]})
        print(f"{entry:08X}: " + ", ".join(t["address"] for t in tables))
    report = {"result": "PASS independently located original eleven-entry tables",
              "functions": reports, "dol_sha256": PIN,
              "scope": "Candidate table identity and references only; no runtime or complete equivalence claim."}
    output = ROOT / "recomp/checkpoints/20261021_prime_audio_scaled_switch_audit.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
