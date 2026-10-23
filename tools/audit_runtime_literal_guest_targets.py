#!/usr/bin/env python3
"""Inventory literal guest targets invoked by the WiiCompiled runtime against RM3E01_00."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARGET_RE = re.compile(
    r"\b(InvokeIndirectCpu|InvokeIndirectJump)\s*\(\s*0x([0-9A-Fa-f]{1,8})[uU]?"
)
SYMBOL_RE = re.compile(r"^(.+?) = \S+:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "recomp/checkpoints/20261020_runtime_literal_guest_target_audit.json",
    )
    args = parser.parse_args()

    runtime_root = ROOT / "recomp/third_party/WiiCompiled/runtime/src"
    function_map_path = ROOT / "recomp/config/RM3E01_00/functions.txt"
    symbols_path = ROOT / "decomp/config/RM3E01_00/symbols.txt"
    dol_path = ROOT / "recomp/input/RM3E01_00/main.dol"
    dol_sha = sha256(dol_path)
    if dol_sha != "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104":
        raise SystemExit("DOL hash does not match pinned RM3E01_00")

    exact_functions = {
        int(line.split(maxsplit=1)[0], 16)
        for line in function_map_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    }
    function_ranges: list[tuple[int, int, str]] = []
    for line in symbols_path.read_text(encoding="utf-8").splitlines():
        match = SYMBOL_RE.match(line)
        if match:
            start = int(match.group(2), 16)
            size = int(match.group(3), 16)
            if size:
                function_ranges.append((start, start + size, match.group(1)))
    function_ranges.sort()

    sites: list[dict[str, object]] = []
    source_paths = sorted(runtime_root.rglob("*.cpp"))
    for source in source_paths:
        text = source.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        for match in TARGET_RE.finditer(text):
            address = int(match.group(2), 16)
            line_number = text.count("\n", 0, match.start()) + 1
            container = next(
                ((start, end, name) for start, end, name in function_ranges if start <= address < end),
                None,
            )
            if address in exact_functions:
                classification = "exact_rm3e01_function_entry"
            elif container:
                classification = "inside_rm3e01_function_body"
            else:
                classification = "no_exact_or_sized_symbol_evidence"
            sites.append(
                {
                    "target": f"0x{address:08X}",
                    "kind": match.group(1),
                    "source": source.relative_to(ROOT).as_posix(),
                    "source_sha256": sha256(source),
                    "line": line_number,
                    "source_line": lines[line_number - 1].strip(),
                    "classification": classification,
                    "containing_symbol": (
                        {
                            "name": container[2],
                            "start": f"0x{container[0]:08X}",
                            "end_exclusive": f"0x{container[1]:08X}",
                            "offset": f"+0x{address - container[0]:X}",
                        }
                        if container and address != container[0]
                        else None
                    ),
                }
            )

    by_target: dict[str, list[dict[str, object]]] = {}
    for site in sites:
        by_target.setdefault(str(site["target"]), []).append(site)
    target_summary = []
    for target, target_sites in sorted(by_target.items()):
        classifications = Counter(str(site["classification"]) for site in target_sites)
        target_summary.append(
            {
                "target": target,
                "site_count": len(target_sites),
                "classifications": dict(sorted(classifications.items())),
                "runtime_sources": sorted({str(site["source"]) for site in target_sites}),
            }
        )

    classification_counts = Counter(str(site["classification"]) for site in sites)
    report = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "revision": "RM3E01_00",
        "dol_sha256": dol_sha,
        "inputs": {
            "function_map_sha256": sha256(function_map_path),
            "symbols_sha256": sha256(symbols_path),
            "runtime_root": runtime_root.relative_to(ROOT).as_posix(),
        },
        "scope": "Literal constant targets passed as the first argument to InvokeIndirectCpu/InvokeIndirectJump in runtime C++ sources.",
        "summary": {
            "runtime_cpp_files_scanned": len(source_paths),
            "literal_call_sites": len(sites),
            "unique_targets": len(by_target),
            "site_classifications": dict(sorted(classification_counts.items())),
            "targets_with_inside_function_body_sites": sum(
                any(site["classification"] == "inside_rm3e01_function_body" for site in values)
                for values in by_target.values()
            ),
        },
        "interpretation": "A target inside another Prime 3 function is not established as a callable entry and must not be dispatched as one without machine-level/control-flow evidence. This is an address-collision inventory, not proof that a particular path executes.",
        "limitations": [
            "Runtime indirect targets computed in registers or memory are not covered.",
            "This scan does not model preprocessor guards, runtime reachability, or native HLE registration paths.",
            "Symbols without explicit size cannot classify an interior address; absence from the map is not proof of invalid code.",
        ],
        "targets": target_summary,
        "sites": sites,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report["summary"], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
