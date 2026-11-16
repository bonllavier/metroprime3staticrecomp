#!/usr/bin/env python3
"""Compares the small-data constants (r2, sdata2: floats and doubles) that paired functions read, by value.

audit_pair_fields.py normalises (r2 + d) away, so a constant whose value changed in Trilogy (a gesture threshold,
a speed) would not show there. This reads, for each paired function, every (r2 + d) the translated code loads, in
order, and compares the bytes behind them in the two DOLs. Read-only.

  audit_small_data.py R3ME01_MP3 --port          functions the port refers to (as audit_pair_fields.py --port)
  audit_small_data.py R3ME01_MP3 --all           every paired function
  audit_small_data.py R3ME01_MP3 0x803C11D8 ...  those functions
"""
from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from audit_pair_fields import Title, port_addresses  # noqa: E402
from make_address_map import ROOT, Dol  # noqa: E402

LOAD = re.compile(r"Read(?:Resolved)?(Float32|Float64|32|16|8)\([^;]*?\(r2 \+ (-?\d+)\)")


def loads(path: Path) -> list[tuple[str, int]]:
    if not path.exists():
        return []
    text = path.read_text(encoding="utf-8", errors="replace")
    return [(kind, int(d)) for kind, d in LOAD.findall(text)]


def value(dol: Dol, address: int, kind: str):
    size = {"Float32": 4, "Float64": 8, "32": 4, "16": 2, "8": 1}[kind]
    raw = dol.read(address, size)
    if raw is None:
        return None
    if kind == "Float32":
        return struct.unpack(">f", raw)[0]
    if kind == "Float64":
        return struct.unpack(">d", raw)[0]
    return int.from_bytes(raw, "big")


def main() -> None:
    title = Title(sys.argv[1])
    project = (title.base / "project.yml").read_text(encoding="utf-8")
    r2 = int(re.search(r"sda2_base: (0x[0-9A-F]+)", project)[1], 16)
    r13 = int(re.search(r"sda_base: (0x[0-9A-F]+)", project)[1], 16)
    a = Dol(ROOT / "input/RM3E01_00/main.dol", 0x806801C0, 0x806869C0)
    b = Dol(title.base / "extract/sys/main.dol", r13, r2)
    arguments = sys.argv[2:]
    if arguments == ["--port"]:
        entries = sorted(port_addresses(title))
    elif arguments == ["--all"]:
        entries = title.starts
    else:
        entries = sorted({title.owner(int(x, 16)) for x in arguments})
    checked = differing = 0
    for entry in entries:
        if entry not in title.pairs:
            continue
        target = title.pairs[entry][0]
        la = loads(title.a_dir / f"func_{entry:08X}.cpp")
        lb = loads(title.b_dir / f"func_{target:08X}.cpp")
        if not la:
            continue
        checked += 1
        if len(la) != len(lb):
            print(f"{entry:08X} -> {target:08X}: {len(la)} vs {len(lb)} small-data loads (compare by hand)")
            differing += 1
            continue
        changes = []
        for (ka, da), (kb, db) in zip(la, lb):
            va, vb = value(a, 0x806869C0 + da, ka), value(b, r2 + db, kb)
            if ka != kb or va != vb:
                changes.append(f"{ka} {va!r} -> {kb} {vb!r}")
        if changes:
            differing += 1
            print(f"{entry:08X} -> {target:08X}: " + "; ".join(changes))
    print(f"{checked} functions with small-data constants checked, {differing} differ")


if __name__ == "__main__":
    main()
