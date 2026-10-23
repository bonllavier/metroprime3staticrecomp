#!/usr/bin/env python3
"""Read-only audit of RM3E01 static SEL imports against generated DOL functions.

This measures symbol coverage only. It does not relocate, compile, or execute an RSO.
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from collections import Counter, deque
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import inspect_startup_rso  # noqa: E402

BUILD_ID = "RM3E01_00_conservative"
CALL = re.compile(r"InvokeDirectCpu<0x([0-9A-Fa-f]+)u>\(ctx\)")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rso_directory", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "recomp/checkpoints/20261020_static_rso_bridge_audit.json")
    args = parser.parse_args()
    if not args.rso_directory.is_dir():
        parser.error(f"not a directory: {args.rso_directory}")

    symbols: dict[str, dict] = {}
    for line in (ROOT / "decomp/config/RM3E01_00/symbols.txt").read_text(encoding="utf-8").splitlines():
        match = re.match(r"(.+?) = (\S+):0x([0-9a-fA-F]+); // (.*)", line)
        if match:
            symbols[match.group(1)] = {
                "address": int(match.group(3), 16),
                "section": match.group(2),
                "function": "type:function" in match.group(4),
            }

    recovered: dict[int, str] = {}
    for item in json.loads((ROOT / "decomp/config/RM3E01_00/recovered_leaf.json").read_text(encoding="utf-8")):
        recovered[item["address"]] = item["unit"]
    recovered.update({
        0x803057C4: "runtime/strlen.c", 0x802D50CC: "startup/region.c",
        0x8038F998: "platform/alloc_region.c", 0x804E524C: "sdk/set_region.c",
        0x804E526C: "sdk/get_saved_region.c", 0x804DF358: "sdk/get_arena_low.c",
        0x804DF340: "sdk/get_arena_high.c", 0x804DF3CC: "sdk/alloc_arena_low.c",
    })

    generated_dir = ROOT / "recomp/generated" / BUILD_ID / "functions"
    generated: dict[int, Path] = {}
    for path in generated_dir.glob("func_*.cpp"):
        match = re.fullmatch(r"func_([0-9A-Fa-f]+)", path.stem)
        if match:
            generated[int(match.group(1), 16)] = path
    graph = {
        address: [int(value, 16) for value in CALL.findall(path.read_text(encoding="utf-8"))]
        for address, path in generated.items()
    }
    reachable: set[int] = set()
    pending = deque([0x80006320])
    while pending:
        address = pending.popleft()
        if address in reachable:
            continue
        reachable.add(address)
        pending.extend(target for target in graph.get(address, ()) if target not in reachable)

    modules = []
    aggregate: Counter[str] = Counter()
    for path in sorted(args.rso_directory.glob("*.rso")):
        module = inspect_startup_rso.inspect_bytes(path.read_bytes())
        categories: Counter[str] = Counter()
        missing = []
        imports = []
        for item in module["imports"]:
            main_symbol = symbols.get(item["name"])
            if main_symbol is None:
                category = "unresolved_in_main_symbols"
                missing.append(item["name"])
                address = None
            elif not main_symbol["function"]:
                category = "main_data_symbol"
                address = main_symbol["address"]
            elif main_symbol["address"] not in generated:
                category = "function_not_translated"
                address = main_symbol["address"]
            elif main_symbol["address"] in reachable:
                category = "translated_function_in_entry_direct_closure"
                address = main_symbol["address"]
            else:
                category = "translated_function_outside_entry_direct_closure"
                address = main_symbol["address"]
            categories[category] += 1
            aggregate[category] += 1
            imports.append({
                "name": item["name"],
                "main_address": f"0x{address:08X}" if address is not None else None,
                "category": category,
                "has_matching_decomp_source": bool(main_symbol and recovered.get(main_symbol["address"])),
                "relocation_count": item["relocation_count"],
            })
        modules.append({
            "name": path.name,
            "sha256": sha256(path),
            "size": path.stat().st_size,
            "section_count": len(module["sections"]),
            "exports": [{"name": item["name"], "section": item["section"], "offset": item["offset"]}
                        for item in module["exports"]],
            "entry_points": module["entry_points"],
            "imports": len(module["imports"]),
            "internal_relocations": len(module["relocations"]["internal"]),
            "external_relocations": len(module["relocations"]["external"]),
            "import_categories": dict(sorted(categories.items())),
            "unresolved_names": missing,
            "import_details": imports,
        })

    result = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "revision": "RM3E01_00",
        "scope": "Read-only RSO metadata and name/address coverage; no relocations applied and no RSO compiled or executed.",
        "generated_function_count": len(generated),
        "entry_direct_call_closure_count": len(reachable),
        "rso_module_count": len(modules),
        "aggregate_import_categories": dict(sorted(aggregate.items())),
        "modules": modules,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "module_count": len(modules),
        "entry_direct_call_closure_count": len(reachable),
        "aggregate_import_categories": result["aggregate_import_categories"],
        "output": str(args.output),
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
