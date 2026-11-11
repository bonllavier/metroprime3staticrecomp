#!/usr/bin/env python3
"""Static entry-point discovery for a Prime 3 RSO module.

A relocatable module cannot hold an absolute code address without a
relocation, so every function that can be reached indirectly (prolog/epilog,
static constructors/destructors, exports, vtables, callbacks taken by HA/LO)
appears in the module's own relocation tables. The only relocated text
targets that are not function entries are switch-table case labels; those
tables are recognised by the code that indexes them (HA/LO to the table, then
lwzx/mtctr/bctr) and bounded by the preceding cmplwi.

Prints JSON: roots by category, the switch-table relocation sources excluded,
and any relocated text target left unclassified (reported, never guessed).
"""
import argparse
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from inspect_startup_rso import inspect_bytes  # noqa: E402

RSO_DIR = ROOT / "recomp/.tools/disc-extraction-rm3e01/files/RSO/Rev2 Production"
R_PPC_ADDR32, R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA = 1, 4, 5, 6
MTCTR_MASK, MTCTR = 0xFC1FFFFF, 0x7C0903A6
BCTR = 0x4E800420


def word(raw, offset):
    return struct.unpack_from(">I", raw, offset)[0]


def is_lwzx(insn):
    return insn >> 26 == 31 and (insn >> 1) & 0x3FF == 23


def cmplwi_bound(insn):
    # cmplwi crX, rA, UIMM: opcode 10, L=0.
    if insn >> 26 == 10 and (insn >> 21) & 1 == 0:
        return insn & 0xFFFF
    return None


def discover(name):
    raw = (RSO_DIR / f"RSO_{name}.rso").read_bytes()
    module = inspect_bytes(raw)
    sections = module["sections"]
    # The SDK layout: 1 .text, 2 .ctors, 3 .dtors, 4 .rodata, 5 .data, 6 .bss.
    # The executable flag is not set in these files; the prolog pins .text.
    text = sections[1]
    if module["entry_points"].get("prolog", {}).get("section") != 1 or not text["size"]:
        raise ValueError(f"{name}: prolog is not in section 1")
    ti, tbase, tsize = text["index"], text["file_offset"], text["size"]
    internal = module["relocations"]["internal"]

    def text_word(offset):
        return word(raw, tbase + offset)

    roots = {}

    def add(category, offset):
        if offset % 4 or not 0 <= offset < tsize:
            raise ValueError(f"{name}: {category} root {offset:#x} outside text")
        roots.setdefault(offset, set()).add(category)

    for label, entry in module["entry_points"].items():
        if entry["section"] == ti:
            add(label, entry["offset"])
    for row in module["exports"]:
        if row["section"] == ti:
            add("export", row["offset"])

    # Switch tables: `addi rD, rA, table@l` whose rD indexes an lwzx, the
    # loaded word moved to CTR and branched to (lwzx rT / mtctr rT / bctr).
    los = [r for r in internal if r["source_section"] == ti and r["type"] == R_PPC_ADDR16_LO and r["index"] != ti]
    switch_tables = {}
    dispatches = []
    for r in los:
        site = r["source_section_offset"] - 2
        addi = text_word(site)
        if addi >> 26 != 14:
            continue
        base_reg = (addi >> 21) & 31
        follow = [text_word(site + 4 * k) for k in range(1, 6) if site + 4 * k < tsize]
        index = next((i for i, w in enumerate(follow)
                      if is_lwzx(w) and base_reg in ((w >> 16) & 31, (w >> 11) & 31)), None)
        if index is None or index + 2 >= len(follow):
            continue
        loaded = (follow[index] >> 21) & 31
        if follow[index + 1] != (MTCTR | loaded << 21) or follow[index + 2] != BCTR:
            continue
        # The dispatch guard: `cmplwi cr0, rN, max` then `bgt cr0, default`
        # shortly before the table address is formed.
        bound = None
        guard = next((site - 4 * k for k in range(1, 9)
                      if site - 4 * k >= 0 and text_word(site - 4 * k) >> 16 == 0x4181), None)
        if guard is not None:
            for k in range(1, 49):
                if guard - 4 * k < 0:
                    break
                w = text_word(guard - 4 * k)
                if (w >> 23) & 7 == 0 and cmplwi_bound(w) is not None:
                    bound = cmplwi_bound(w) + 1
                    break
        key = (r["index"], r["target_offset"])
        switch_tables[key] = max(switch_tables.get(key, 0), bound or 0)
        dispatches.append((site + 4 * (index + 3), key))

    data_code = {}
    for r in internal:
        if r["index"] != ti:
            continue
        if r["source_section"] == ti and r["type"] in (R_PPC_ADDR16_HA, R_PPC_ADDR16_HI, R_PPC_ADDR16_LO):
            add("address_taken", r["target_offset"])
        elif r["source_section"] != ti and r["type"] == R_PPC_ADDR32:
            data_code[(r["source_section"], r["source_section_offset"])] = r["target_offset"]
        elif r["source_section"] == ti and r["type"] in (10, 11, 18):
            pass  # rel24/rel14 branches; the translator follows them from their callers
        else:
            raise ValueError(f"{name}: unexpected text relocation {r}")

    switch_sources = set()
    unbounded = []
    for (section, start), bound in sorted(switch_tables.items()):
        count = 0
        while (section, start + 4 * count) in data_code and (bound == 0 or count < bound):
            switch_sources.add((section, start + 4 * count))
            count += 1
        if bound == 0:
            unbounded.append({"section": section, "offset": start, "entries": count})
        elif count != bound:
            raise ValueError(f"{name}: switch table {section}:{start:#x} has {count}/{bound} relocated entries")
        switch_tables[(section, start)] = count

    for key, target in data_code.items():
        if key in switch_sources:
            continue
        section = key[0]
        category = {2: "constructor", 3: "destructor"}.get(section, "data_pointer")
        add(category, target)

    return {
        "module": name,
        "text_section": ti,
        "text_size": tsize,
        "roots": [{"text_offset": o, "categories": sorted(c)} for o, c in sorted(roots.items())],
        "switch_table_sources": [[s, o] for s, o in sorted(switch_sources)],
        "unbounded_switch_tables": unbounded,
        # bctr text offset -> case label text offsets, in table order.
        "jump_tables": [{"bctr": bctr, "table": list(key),
                         "targets": [data_code[(key[0], key[1] + 4 * i)] for i in range(switch_tables[key])]}
                        for bctr, key in sorted(dispatches)],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("modules", nargs="*", help="module names without RSO_ prefix (default: all)")
    parser.add_argument("--summary", action="store_true")
    args = parser.parse_args()
    names = args.modules or sorted(p.stem[4:] for p in RSO_DIR.glob("RSO_*.rso"))
    results = [discover(n) for n in names]
    if args.summary:
        for r in results:
            cats = {}
            for root in r["roots"]:
                for c in root["categories"]:
                    cats[c] = cats.get(c, 0) + 1
            print(f"{r['module']:40s} text={r['text_size']:7d} roots={len(r['roots']):5d} "
                  f"switch_entries={len(r['switch_table_sources']):4d} unbounded={len(r['unbounded_switch_tables'])} {cats}")
    else:
        print(json.dumps(results, indent=1))


if __name__ == "__main__":
    main()
