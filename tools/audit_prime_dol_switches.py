#!/usr/bin/env python3
"""Audit DOL switch dispatches against the translated case labels.

For each MWCC jump-table dispatch in main.dol (lis/addi table, lwzx, mtctr,
bctr) the table length comes from its guard (`cmplwi cr0, rN, max` then
`bgt`). Every table entry must be a local label of the translated function
holding the bctr, or the case is unreachable at runtime ("Missing indirect
jump target"). The DOL has no relocations, so this is an audit that lists
candidates for retranslation, not a proof.
"""
import argparse
import re
import bisect
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOL = ROOT / "recomp/input/RM3E01_00/main.dol"
BASE = ROOT / "recomp/generated/RM3E01_00_conservative/runtime_hooks/base_translation_output.json"
REPAIRS = ROOT / "recomp/.tools/dol-runtime-repairs"
# Repairs reviewed before this audit (prepare_prime_runtime_hooks.py).
ORIGINAL_REPAIRS = (0x800E01F0, 0x80420444, 0x8019804C, 0x8042B530, 0x80423190, 0x80423EF8, 0x8040D9D8,
                    0x8040F98C, 0x80414E20, 0x80415CB8, 0x804167CC, 0x80419848, 0x80425B10, 0x80427A34, 0x804280AC)


def load_dol():
    raw = DOL.read_bytes()
    offsets = struct.unpack_from(">18I", raw, 0)
    addresses = struct.unpack_from(">18I", raw, 0x48)
    sizes = struct.unpack_from(">18I", raw, 0x90)
    sections = [(addresses[i], offsets[i], sizes[i], i < 7) for i in range(18) if sizes[i]]
    return raw, sections


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output")
    parser.add_argument("--original-repairs-only", action="store_true",
                        help="audit against the base plus only the repairs reviewed before this audit")
    parser.add_argument("--emit-overrides", help="write dol_jump_tables.json for the functions with missing cases")
    args = parser.parse_args()
    raw, sections = load_dol()

    def read(address):
        for start, offset, size, _ in sections:
            if start <= address < start + size:
                return struct.unpack_from(">I", raw, offset + address - start)[0]
        return None

    # Case labels as emitted in the C++ that is compiled (metadata labels can
    # be stale for bodies that were regenerated in place).
    def cases(path):
        return {int(v, 16) for v in re.findall(r"case 0x([0-9A-F]{8})u:", path.read_text())}
    functions = {}
    for f in json.loads(BASE.read_text())["functions"]:
        functions[f["entryPoint"]] = cases(BASE.parent.parent / f["relativePath"])
    repaired = set(ORIGINAL_REPAIRS) if args.original_repairs_only else None
    for path in REPAIRS.glob("func_*.json"):
        row = json.loads(path.read_text())
        if repaired is None or row["entryPoint"] in repaired:
            functions[row["entryPoint"]] = cases(REPAIRS / row["relativePath"])
    entries = sorted(functions)

    findings, checked = [], 0
    for start, offset, size, is_text in sections:
        if not is_text:
            continue
        for k in range(size // 4):
            address = start + 4 * k
            if read(address) != 0x4E800420:
                continue
            mtctr, lwzx = read(address - 4), read(address - 8)
            if mtctr is None or (mtctr & 0xFC1FFFFF) != 0x7C0903A6:
                continue
            target_reg = (mtctr >> 21) & 31
            if lwzx >> 26 != 31 or (lwzx >> 1) & 0x3FF != 23 or (lwzx >> 21) & 31 != target_reg:
                continue
            regs = {(lwzx >> 16) & 31, (lwzx >> 11) & 31}
            # Table base: addi rD,rA,lo with rD among the lwzx operands, then lis rA,hi.
            table, addi_at = None, None
            for back in range(3, 8):
                w = read(address - 4 * back)
                if w is not None and w >> 26 == 14 and (w >> 21) & 31 in regs:
                    addi_at, base_reg, low = address - 4 * back, (w >> 16) & 31, w & 0xFFFF
                    break
            if addi_at is None:
                continue
            for back in range(1, 200):
                w = read(addi_at - 4 * back)
                if w is not None and w >> 26 == 15 and (w >> 21) & 31 == base_reg and (w >> 16) & 31 == 0:
                    table = ((w & 0xFFFF) << 16) + (low - 0x10000 if low & 0x8000 else low)
                    table &= 0xFFFFFFFF
                    break
                if w is not None and (w >> 21) & 31 == base_reg and w >> 26 not in (36, 37, 38, 44, 45, 52, 53, 54, 55, 10, 11):
                    break  # base register redefined by something other than a store/compare
            guard = next((addi_at - 4 * b for b in range(1, 9) if (read(addi_at - 4 * b) or 0) >> 16 == 0x4181), None)
            bound = None
            if guard is not None:
                for b in range(1, 49):
                    w = read(guard - 4 * b)
                    if w is not None and w >> 26 == 10 and (w >> 21) & 1 == 0 and (w >> 23) & 7 == 0:
                        bound = (w & 0xFFFF) + 1
                        break
            if table is None or bound is None or bound > 512:
                continue
            checked += 1
            targets = [read(table + 4 * i) for i in range(bound)]
            owner = entries[bisect.bisect_right(entries, address) - 1]
            labels = functions[owner]
            missing = sorted({t for t in targets if t is not None and t not in labels and t != owner})
            if missing:
                findings.append({"bctr": f"{address:08X}", "function": f"{owner:08X}", "table": f"{table:08X}",
                                 "cases": bound, "missing": [f"{t:08X}" for t in missing],
                                 "targets": [f"{t:08X}" for t in targets]})
    result = {"checked_dispatches": checked, "functions_with_missing_cases": len({f["function"] for f in findings}),
              "findings": findings}
    text = json.dumps(result, indent=1)
    if args.emit_overrides:
        if not args.original_repairs_only:
            raise SystemExit("--emit-overrides needs --original-repairs-only (repaired bodies hide their findings)")
        path = Path(args.emit_overrides)
        tables = {f["bctr"]: {"bctr": f["bctr"], "function": f["function"], "table": f["table"], "targets": f["targets"]}
                  for f in findings}
        path.write_text(json.dumps({
            "description": "MWCC switch tables in main.dol whose translated case labels were incomplete; bound from "
                           "the cmplwi/bgt guard, entries read from the DOL. Produced by audit_prime_dol_switches.py.",
            "repair_functions": sorted({t["function"] for t in tables.values()}),
            "tables": [tables[key] for key in sorted(tables)]},
            indent=1) + "\n")
    if args.output:
        Path(args.output).write_text(text + "\n")
    print(json.dumps({k: result[k] for k in ("checked_dispatches", "functions_with_missing_cases")}))
    for f in findings:
        print(f["function"], f["bctr"], f"{len(f['missing'])}/{f['cases']} missing")


if __name__ == "__main__":
    main()
