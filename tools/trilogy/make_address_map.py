#!/usr/bin/env python3
"""Maps every RM3E01 guest address the port uses onto one Trilogy DOL.

Functions are paired by their normalized code (relocated fields masked), then
by layout, then by similarity inside the window between paired neighbours.
Interior addresses come from aligning the two bodies. Data addresses come from
the instructions that reference them in paired code. A result that cannot be
shown stays listed under "unresolved": nothing here guesses.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import difflib
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKSPACE = ROOT.parent
SYMBOL = re.compile(r"(\S.*?) = \.(?:text|init):0x([0-9A-Fa-f]{8}); // type:function size:0x([0-9A-Fa-f]+)")
LITERAL = re.compile(r"(?:0[xX]|func_|loc_)(8[01][0-9A-Fa-f]{6})(?![0-9A-Fa-f])")
SOURCES = ("third_party/WiiCompiled/runtime/src", "third_party/WiiCompiled/runtime/include",
           "tools/prepare_prime_runtime_hooks.py", "tools/prepare_prime_rso_translation.py",
           "config/RM3E01_00/dol_jump_tables.json", "patches")


class Dol:
    def __init__(self, path: Path, r13: int, r2: int):
        self.raw = path.read_bytes()
        offsets = struct.unpack_from(">18I", self.raw, 0)
        addresses = struct.unpack_from(">18I", self.raw, 0x48)
        sizes = struct.unpack_from(">18I", self.raw, 0x90)
        self.sections = [(i, addresses[i], sizes[i], offsets[i]) for i in range(18) if sizes[i]]
        self.bss = struct.unpack_from(">2I", self.raw, 0xD8)
        self.r13, self.r2 = r13, r2

    def words(self, address: int, size: int) -> tuple[int, ...] | None:
        for _, start, length, offset in self.sections:
            if start <= address and address + size <= start + length:
                return struct.unpack_from(f">{size // 4}I", self.raw, offset + address - start)
        return None

    def read(self, address: int, size: int) -> bytes | None:
        for _, start, length, offset in self.sections:
            if start <= address and address + size <= start + length:
                return self.raw[offset + address - start:offset + address - start + size]
        return None

    def is_text(self, address: int) -> bool:
        return any(i < 7 and start <= address < start + length for i, start, length, _ in self.sections)


def normalize(word: int) -> int:
    op = word >> 26
    if op == 18:
        return word & 0xFC000003
    if op == 16:
        return word & 0xFFFF0003
    if op in (14, 15, 24, 25) or 32 <= op <= 55 or op in (56, 57, 60, 61):
        return word & 0xFFFF0000
    return word


def functions(path: Path) -> list[tuple[int, int, str]]:
    found: dict[int, tuple[int, int, str]] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = SYMBOL.match(line)
        if match:
            address = int(match[2], 16)
            found.setdefault(address, (address, int(match[3], 16) & ~3, match[1]))
    return [found[address] for address in sorted(found)]


def pair_functions(a: Dol, b: Dol, a_funcs, b_funcs):
    a_norm = {f[0]: tuple(normalize(w) for w in (a.words(f[0], f[1]) or ())) for f in a_funcs}
    b_norm = {f[0]: tuple(normalize(w) for w in (b.words(f[0], f[1]) or ())) for f in b_funcs}
    by_body = collections.defaultdict(list)
    for address, body in b_norm.items():
        if body:
            by_body[body].append(address)
    pairs: dict[int, tuple[int, str, float]] = {}
    # Whole-body matches long enough to mean something anchor the layout.
    for address, size, _ in a_funcs:
        candidates = by_body.get(a_norm[address], [])
        if len(candidates) == 1 and size >= 16:
            pairs[address] = (candidates[0], "unique", 1.0)
    keys = sorted(pairs)
    b_addresses = sorted(b_norm)
    used = {value[0] for value in pairs.values()}
    # Repeated bodies: the candidate that keeps the neighbours' layout.
    for address, size, _ in a_funcs:
        if address in pairs:
            continue
        candidates = by_body.get(a_norm[address], [])
        if not candidates:
            continue
        i = bisect.bisect_left(keys, address)
        predicted = []
        if i > 0:
            predicted.append(address + pairs[keys[i - 1]][0] - keys[i - 1])
        if i < len(keys):
            predicted.append(address + pairs[keys[i]][0] - keys[i])
        best = min(candidates, key=lambda c: min(abs(c - p) for p in predicted) if predicted else 0)
        if predicted and min(abs(best - p) for p in predicted) <= 0x4000:
            pairs[address] = (best, "layout", 1.0)
            used.add(best)
    # Changed bodies: best similar function between the paired neighbours.
    keys = sorted(pairs)
    for address, size, _ in a_funcs:
        if address in pairs or not a_norm[address]:
            continue
        i = bisect.bisect_left(keys, address)
        low = pairs[keys[i - 1]][0] if i > 0 else b_addresses[0]
        high = pairs[keys[i]][0] if i < len(keys) else b_addresses[-1]
        if not low < high:
            continue
        window = b_addresses[bisect.bisect_right(b_addresses, low):bisect.bisect_left(b_addresses, high)]
        window = [c for c in window if c not in used][:64]
        best, best_ratio = None, 0.0
        for candidate in window:
            body = b_norm[candidate]
            if not body:
                continue
            matcher = difflib.SequenceMatcher(None, a_norm[address], body, autojunk=False)
            if matcher.real_quick_ratio() <= best_ratio or matcher.quick_ratio() <= best_ratio:
                continue
            ratio = matcher.ratio()
            if ratio > best_ratio:
                best, best_ratio = candidate, ratio
        if best is not None and best_ratio >= 0.5:
            pairs[address] = (best, "similar", best_ratio)
            used.add(best)
    return pairs, a_norm, b_norm


class Mapper:
    def __init__(self, a, b, a_funcs, b_funcs, pairs, a_norm, b_norm):
        self.a, self.b, self.pairs, self.a_norm, self.b_norm = a, b, pairs, a_norm, b_norm
        self.a_starts = [f[0] for f in a_funcs]
        self.a_funcs = a_funcs
        self.b_sizes = {f[0]: f[1] for f in b_funcs}
        self.alignment: dict[int, dict[int, int]] = {}

    def owner(self, address: int):
        i = bisect.bisect_right(self.a_starts, address) - 1
        if i < 0:
            return None
        start, size, name = self.a_funcs[i]
        return (start, size, name) if address < start + size else None

    def align(self, start: int) -> dict[int, int]:
        if start not in self.alignment:
            target = self.pairs[start][0]
            table: dict[int, int] = {}
            matcher = difflib.SequenceMatcher(None, self.a_norm[start], self.b_norm[target], autojunk=False)
            for block in matcher.get_matching_blocks():
                for k in range(block.size):
                    table[start + (block.a + k) * 4] = target + (block.b + k) * 4
            self.alignment[start] = table
        return self.alignment[start]

    def code(self, address: int):
        owner = self.owner(address)
        if owner is None:
            return None, "not inside a known function", None
        start, _, name = owner
        if start not in self.pairs:
            return None, f"{name} has no counterpart", name
        target, method, ratio = self.pairs[start]
        if address == start:
            return target, f"entry/{method}/{ratio:.2f}", name
        mapped = self.align(start).get(address)
        if mapped is None:
            return None, f"interior of changed part of {name}", name
        return mapped, f"interior/{method}/{ratio:.2f}", name


def branch_target(pc: int, word: int) -> int | None:
    """Target of a direct b/bl, not of a local conditional branch."""
    if word >> 26 != 18:
        return None
    offset = word & 0x3FFFFFC
    return (pc + (offset - 0x4000000 if offset & 0x2000000 else offset)) & 0xFFFFFFFF


def pair_by_calls(a, b, mapper, pairs, a_funcs, b_funcs) -> None:
    """Aligned calls in paired bodies name the same function, whatever their layout."""
    a_entries = {f[0] for f in a_funcs}
    b_entries = {f[0] for f in b_funcs}
    for _ in range(4):
        taken = {value[0] for value in pairs.values()}
        votes: dict[int, collections.Counter] = collections.defaultdict(collections.Counter)
        for start in list(pairs):
            for pc_a, pc_b in mapper.align(start).items():
                word_a, word_b = a.words(pc_a, 4)[0], b.words(pc_b, 4)[0]
                target_a, target_b = branch_target(pc_a, word_a), branch_target(pc_b, word_b)
                if target_a in a_entries and target_a not in pairs and target_b in b_entries and target_b not in taken:
                    votes[target_a][target_b] += 1
        added = 0
        for target_a, counter in votes.items():
            target_b, count = counter.most_common(1)[0]
            if count * 2 > sum(counter.values()) and target_b not in taken:
                pairs[target_a] = (target_b, "calls", count / sum(counter.values()))
                taken.add(target_b)
                added += 1
        if not added:
            break


def absolute_references(dol: Dol):
    """Yields (pc, target) for lis+addi/ori/load-store and for SDA accesses."""
    for i, start, length, offset in dol.sections:
        if i >= 7:
            continue
        words = struct.unpack_from(f">{length // 4}I", dol.raw, offset)
        high: dict[int, int] = {}
        for k, word in enumerate(words):
            pc = start + k * 4
            op, rd, ra, imm = word >> 26, word >> 21 & 31, word >> 16 & 31, word & 0xFFFF
            signed = imm - 0x10000 if imm & 0x8000 else imm
            if word == 0x4E800020:
                high.clear()
                continue
            if op == 15 and ra == 0:
                high[rd] = imm << 16
                continue
            if ra == 13 and (op == 14 or 32 <= op <= 55):
                yield pc, (dol.r13 + signed) & 0xFFFFFFFF
            elif ra == 2 and (op == 14 or 32 <= op <= 55):
                yield pc, (dol.r2 + signed) & 0xFFFFFFFF
            elif ra in high and (op == 14 or 32 <= op <= 55):
                yield pc, (high[ra] + signed) & 0xFFFFFFFF
            elif op == 24 and rd in high:
                yield pc, high[rd] | imm


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("title", choices=("R3ME01_MP3", "R3ME01_FE"))
    args = parser.parse_args()
    base = ROOT / "titles" / args.title
    project = (base / "project.yml").read_text(encoding="utf-8")
    r13 = int(re.search(r"sda_base: (0x[0-9A-F]+)", project)[1], 16)
    r2 = int(re.search(r"sda2_base: (0x[0-9A-F]+)", project)[1], 16)
    a = Dol(ROOT / "input/RM3E01_00/main.dol", 0x806801C0, 0x806869C0)
    b = Dol(base / "extract/sys/main.dol", r13, r2)
    a_funcs = functions(WORKSPACE / "decomp/config/RM3E01_00/symbols.txt")
    b_funcs = functions(base / "dtk/config/symbols.txt")
    pairs, a_norm, b_norm = pair_functions(a, b, a_funcs, b_funcs)
    mapper = Mapper(a, b, a_funcs, b_funcs, pairs, a_norm, b_norm)
    pair_by_calls(a, b, mapper, pairs, a_funcs, b_funcs)
    methods = collections.Counter(value[1] for value in pairs.values())
    print(args.title, "functions paired", len(pairs), "of", len(a_funcs), dict(methods))

    used: dict[int, set[str]] = collections.defaultdict(set)
    for source in SOURCES:
        path = ROOT / source
        for item in ([path] if path.is_file() else path.rglob("*")):
            if item.suffix in (".cpp", ".h", ".hpp", ".inc", ".py", ".json", ".patch"):
                for literal in LITERAL.findall(item.read_text(encoding="utf-8", errors="replace")):
                    value = int(literal, 16)
                    if value >= 0x80004000:
                        used[value].add(str(item.relative_to(ROOT)).replace("\\", "/"))

    # RSOStaticLocateObject's section switch, which inspect_static_sel.py reads.
    used[0x80540914].add("tools/inspect_static_sel.py")

    b_refs = dict(absolute_references(b))
    a_refs = collections.defaultdict(list)
    for pc, target in absolute_references(a):
        a_refs[target].append(pc)

    def map_data(address: int):
        votes = collections.Counter()
        for pc in a_refs.get(address, ()):
            mapped, _, _ = mapper.code(pc)
            if mapped is not None and mapped in b_refs:
                votes[b_refs[mapped]] += 1
        if votes:
            target, count = votes.most_common(1)[0]
            return target, f"data/refs {count}/{sum(votes.values())}"
        # An address inside an object: the nearest referenced neighbour below it.
        for distance in range(4, 0x800, 4):
            for neighbour in (address - distance, address + distance):
                if neighbour in a_refs:
                    mapped = map_data_direct(neighbour)
                    if mapped is not None:
                        return mapped + address - neighbour, f"data/neighbour {address - neighbour:+#x}"
        return None, "no reference in paired code"

    def map_data_direct(address: int):
        votes = collections.Counter()
        for pc in a_refs.get(address, ()):
            mapped, _, _ = mapper.code(pc)
            if mapped is not None and mapped in b_refs:
                votes[b_refs[mapped]] += 1
        return votes.most_common(1)[0][0] if votes else None

    resolved, unresolved = {}, {}
    for address in sorted(used):
        if a.is_text(address):
            mapped, how, name = mapper.code(address)
        else:
            mapped, how = map_data(address)
            name = "data"
        row = {"how": how, "owner": name, "used_in": sorted(used[address])[:4]}
        if mapped is None:
            unresolved[f"0x{address:08X}"] = row
        else:
            row["target"] = f"0x{mapped:08X}"
            # Initialized data must hold the same bytes; relocated pointers are skipped.
            if not a.is_text(address) and how.startswith("data"):
                x, y = a.read(address, 4), b.read(mapped, 4)
                if x is not None and y is not None and x != y and not (x[0] == 0x80 and y[0] == 0x80):
                    row["data_differs"] = f"{x.hex()} != {y.hex()}"
            resolved[f"0x{address:08X}"] = row

    report = {"title": args.title, "function_pairs": {f"0x{k:08X}": [f"0x{v[0]:08X}", v[1], round(v[2], 3)] for k, v in pairs.items()},
              "resolved": resolved, "unresolved": unresolved}
    (base / "address_map.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    print("literal addresses", len(used), "resolved", len(resolved), "unresolved", len(unresolved),
          "data differing", sum(1 for r in resolved.values() if "data_differs" in r))
    for key, row in list(unresolved.items())[:120]:
        print(" ", key, row["how"][:60], row["used_in"][0][-40:])


if __name__ == "__main__":
    main()
