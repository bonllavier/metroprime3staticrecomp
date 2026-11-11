#!/usr/bin/env python3
"""Lists runtime overrides whose fixed address is a function entry of a Trilogy DOL.

In RM3E01 these Mario Kart addresses fall inside other functions and do nothing.
Where one lands on an entry here, it would replace a real function of the title.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MACRO = re.compile(r"\b(PPC_NATIVE_OVERRIDE(?:_VOID)?|REGISTER_(?:NATIVE|TRANSLATED)_FUNCTION(?:_AS)?|MKW_KNOWN_TYPED_NATIVE)\(\s*(?:0[xX])?([0-9A-Fa-f]{8})")
DEFINED = re.compile(r'extern "C"\s+[\w:\*\s]+\bfunc_(8[0-9A-Fa-f]{7})\s*\([^;]*\{')


def main() -> None:
    title = sys.argv[1]
    entries = {int(line.split()[0], 16) for line in (ROOT / "titles" / title / "functions.txt").read_text(encoding="utf-8").splitlines()}
    runtime = ROOT / "titles" / title / "work/recomp/third_party/WiiCompiled/runtime"
    total, hits = 0, []
    for path in list((runtime / "src").rglob("*")) + list((runtime / "include").rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp", ".inc"):
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for pattern in (MACRO, DEFINED):
                for match in pattern.finditer(line):
                    address = int(match.groups()[-1], 16)
                    total += 1
                    if address in entries:
                        hits.append((f"{address:08X}", f"{path.relative_to(runtime)}:{number}", line.strip()[:80]))
    print(title, "fixed overrides", total, "on function entries", len(hits))
    for hit in hits:
        print(" ", *hit)


if __name__ == "__main__":
    main()
