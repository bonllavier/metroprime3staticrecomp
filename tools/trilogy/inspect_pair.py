#!/usr/bin/env python3
"""Lists the Trilogy functions between the paired neighbours of an RM3E01 function."""
import bisect, difflib, json, re, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_address_map import ROOT, WORKSPACE, Dol, functions, normalize
title = sys.argv[1]
base = ROOT / "titles" / title
project = (base / "project.yml").read_text(encoding="utf-8")
r13 = int(re.search(r"sda_base: (0x[0-9A-F]+)", project)[1], 16); r2 = int(re.search(r"sda2_base: (0x[0-9A-F]+)", project)[1], 16)
a = Dol(ROOT / "input/RM3E01_00/main.dol", 0x806801C0, 0x806869C0); b = Dol(base / "extract/sys/main.dol", r13, r2)
af = functions(WORKSPACE / "decomp/config/RM3E01_00/symbols.txt"); bf = functions(base / "dtk/config/symbols.txt")
pairs = {int(k, 16): int(v[0], 16) for k, v in json.load(open(base / "address_map.json"))["function_pairs"].items()}
keys = sorted(pairs); starts = [f[0] for f in bf]; astarts = [f[0] for f in af]
for arg in sys.argv[2:]:
    x = int(arg, 16); fa = af[bisect.bisect_right(astarts, x) - 1]
    i = bisect.bisect_left(keys, fa[0]); lo = pairs[keys[i-1]]; hi = pairs[keys[i]] if i < len(keys) else starts[-1]
    na = [normalize(w) for w in a.words(fa[0], fa[1])]
    print(f"A {fa[0]:08X} size {fa[1]} {fa[2][:50]}  between B {lo:08X}..{hi:08X}")
    ca = bisect.bisect_right(starts, lo); cb = bisect.bisect_left(starts, hi)
    rows = []
    for f in bf[max(ca-1,0):cb+1][:40]:
        nb = [normalize(w) for w in (b.words(f[0], f[1]) or ())]
        rows.append((difflib.SequenceMatcher(None, na, nb, autojunk=False).ratio() if nb else 0, f))
    for ratio, f in sorted(rows, key=lambda r: -r[0])[:5]:
        print(f"   B {f[0]:08X} size {f[1]} ratio {ratio:.2f} {f[2][:40]}")
