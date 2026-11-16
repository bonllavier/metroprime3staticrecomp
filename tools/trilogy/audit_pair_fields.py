#!/usr/bin/env python3
"""Compares the translated code of RM3E01 functions with their Trilogy counterparts (read-only audit).

The translated C++ (generated/<title>_conservative/functions) is the disassembly: one statement per guest
instruction. Addresses, labels and small-data displacements are normalised away; what is left differing is
real: a structure field that moved (a memory displacement), or changed code.

  audit_pair_fields.py R3ME01_MP3 0x8017A4B4 0x801522E0      diff of those functions (by any address inside)
  audit_pair_fields.py R3ME01_MP3 --census                   every moved displacement over all paired functions
  audit_pair_fields.py R3ME01_MP3 --port                     diff summary of every function the port refers to

A port offset that appears on the old side of the census for the structure it belongs to must be listed in
MOVED_FIELDS (make_title_tree.py). See recomp/docs/trilogy_r3me01.md.
"""
from __future__ import annotations

import bisect
import collections
import difflib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ADDRESS = re.compile(r"0x8[0-9A-Fa-f]{7}u?")
LABEL = re.compile(r"\b(loc|func)_8[0-9A-Fa-f]{7}\b")
SMALL = re.compile(r"\((r13|r2) \+ -?\d+\)")
MEMORY = re.compile(r"\((r\d+) \+ (-?\d+)\)")
SELF_ADD = re.compile(r"^\s*(r\d+) = \(\1 \+ -?\d+\);$")


class Title:
    def __init__(self, title: str):
        self.title = title
        self.base = ROOT / "titles" / title
        report = json.loads((self.base / "address_map.json").read_text(encoding="utf-8"))
        self.pairs = {int(k, 16): (int(v[0], 16), v[1], v[2]) for k, v in report["function_pairs"].items()}
        self.resolved = report["resolved"]
        self.starts = sorted(self.pairs)
        self.a_dir = ROOT / "generated/RM3E01_00_conservative/functions"
        self.b_dir = self.base / f"work/recomp/generated/{title}_conservative/functions"

    def owner(self, address: int) -> int | None:
        index = bisect.bisect_right(self.starts, address) - 1
        return self.starts[index] if index >= 0 else None

    @staticmethod
    def body(path: Path) -> list[str] | None:
        if not path.exists():
            return None
        text = path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")
        start = text.find("\nloc_")
        lines = [line.rstrip() for line in text[start:].split("\n")]
        # Context shuffling around calls is the translator's, not the game's.
        return [line for line in lines if line.strip() and "ctx->" not in line and "RECOMP_GUEST_ABI" not in line]

    @staticmethod
    def normal(line: str) -> str:
        line = SMALL.sub(lambda m: f"({m[1]} + SDA)", line)
        line = LABEL.sub(lambda m: m[1] + "_X", line)
        line = ADDRESS.sub("ADDR", line)
        if SELF_ADD.match(line):  # the low half of a lis/addi address
            line = re.sub(r"-?\d+\);$", "LO);", line)
        return line

    def diff(self, a_entry: int):
        """(a lines, b lines, opcodes) or None when a side is missing."""
        if a_entry not in self.pairs:
            return None
        a = self.body(self.a_dir / f"func_{a_entry:08X}.cpp")
        b = self.body(self.b_dir / f"func_{self.pairs[a_entry][0]:08X}.cpp")
        if a is None or b is None:
            return None
        na, nb = [self.normal(x) for x in a], [self.normal(x) for x in b]
        return a, b, na, nb, difflib.SequenceMatcher(None, na, nb, autojunk=False).get_opcodes()

    def moved(self, a_entry: int):
        """Displacements that changed in otherwise identical statements, and the count of other changes."""
        result = self.diff(a_entry)
        if result is None:
            return None
        a, b, na, nb, opcodes = result
        fields, other = collections.Counter(), 0
        for tag, i1, i2, j1, j2 in opcodes:
            if tag == "equal":
                continue
            if tag == "replace" and i2 - i1 == j2 - j1:
                for x, y in zip(na[i1:i2], nb[j1:j2]):
                    mx, my = MEMORY.findall(x), MEMORY.findall(y)
                    if MEMORY.sub("(M)", x) == MEMORY.sub("(M)", y) and len(mx) == len(my):
                        for (rx, dx), (ry, dy) in zip(mx, my):
                            if rx == ry and dx != dy:
                                fields[(int(dx), int(dy))] += 1
                    else:
                        other += 1
            else:
                other += max(i2 - i1, j2 - j1)
        return fields, other


def show(title: Title, address: int) -> None:
    entry = title.owner(address)
    result = title.diff(entry) if entry is not None else None
    if result is None:
        print(f"{address:08X}: no pair or no translated source")
        return
    a, b, na, nb, opcodes = result
    target, how, ratio = title.pairs[entry]
    print(f"== {entry:08X} -> {target:08X} ({how} {ratio}) lines {len(a)} / {len(b)}")
    for tag, i1, i2, j1, j2 in opcodes:
        if tag == "equal":
            continue
        for line in a[i1:i2]:
            print("  -", line.strip())
        for line in b[j1:j2]:
            print("  +", line.strip())
        print("  ..")


def port_addresses(title: Title) -> dict[int, set[str]]:
    used: dict[int, set[str]] = collections.defaultdict(set)
    for key, row in title.resolved.items():
        files = {name.rsplit("/", 1)[-1] for name in row["used_in"]}
        if row["how"].startswith("data"):
            continue
        entry = title.owner(int(key, 16))
        if entry is not None:
            used[entry] |= files
    return used


def main() -> None:
    title = Title(sys.argv[1])
    arguments = sys.argv[2:]
    if arguments == ["--census"]:
        census, where = collections.Counter(), collections.defaultdict(set)
        for entry in title.starts:
            result = title.moved(entry)
            if not result:
                continue
            for pair, count in result[0].items():
                census[pair] += count
                where[pair].add(entry)
        for (old, new), count in sorted(census.items()):
            sample = " ".join(f"{e:08X}" for e in sorted(where[(old, new)])[:6])
            print(f"{old:#7x} -> {new:#7x}  x{count:<5} in {len(where[(old, new)]):<4} functions: {sample}")
    elif arguments == ["--port"]:
        for entry, files in sorted(port_addresses(title).items()):
            result = title.moved(entry)
            target, how, ratio = title.pairs[entry]
            if result is None:
                print(f"{entry:08X} -> {target:08X} {how} {ratio}: no translated source  [{', '.join(sorted(files))}]")
                continue
            fields, other = result
            moved = " ".join(f"{o:#x}->{n:#x}" for (o, n), _ in sorted(fields.items()))
            state = "identical" if not fields and not other else f"fields[{moved}] other={other}"
            print(f"{entry:08X} -> {target:08X} {how} {ratio}: {state}  [{', '.join(sorted(files))}]")
    else:
        for argument in arguments:
            show(title, int(argument, 16))


if __name__ == "__main__":
    main()
