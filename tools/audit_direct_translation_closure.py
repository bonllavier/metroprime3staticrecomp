#!/usr/bin/env python3
"""Audit emitted direct-call closure, source hashes, and PPC indirect sites."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD_ID = "RM3E01_00_conservative"
DEFAULT_ROOT = 0x802D5150
INDIRECT_RE = re.compile(r"\b(InvokeIndirectCpu|InvokeIndirectJump)\s*\(")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=lambda value: int(value, 0), default=DEFAULT_ROOT)
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "recomp/checkpoints/20261020_main_direct_closure_indirect_audit.json",
    )
    args = parser.parse_args()

    metadata_path = ROOT / "recomp/generated" / BUILD_ID / "base_translation_output.json"
    function_map_path = ROOT / "recomp/config/RM3E01_00/functions.txt"
    source_root = ROOT / "recomp/generated" / BUILD_ID / "functions"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    by_address = {item["entryPoint"]: item for item in metadata["functions"]}

    queue = [args.root]
    visited: set[int] = set()
    missing_metadata: list[int] = []
    while queue:
        address = queue.pop()
        if address in visited:
            continue
        visited.add(address)
        item = by_address.get(address)
        if item is None:
            missing_metadata.append(address)
            continue
        queue.extend(item["build"].get("directCallDependencies", []))
    if missing_metadata:
        raise SystemExit("Missing translation metadata for: " + ", ".join(f"0x{x:08X}" for x in missing_metadata))

    functions: list[dict[str, object]] = []
    indirect_sites: list[dict[str, object]] = []
    missing_sources: list[int] = []
    stale_hashes: list[int] = []
    edges = 0
    for address in sorted(visited):
        item = by_address[address]
        source = source_root / f"func_{address:08X}.cpp"
        if not source.is_file():
            missing_sources.append(address)
            continue
        source_hash = sha256(source)
        if source_hash != item["sha256"]:
            stale_hashes.append(address)
        text = source.read_text(encoding="utf-8")
        lines = text.splitlines()
        found: list[str] = []
        for match in INDIRECT_RE.finditer(text):
            line_number = text.count("\n", 0, match.start()) + 1
            kind = match.group(1)
            found.append(kind)
            indirect_sites.append(
                {
                    "function": f"0x{address:08X}",
                    "source": source.relative_to(ROOT).as_posix(),
                    "source_sha256": source_hash,
                    "line": line_number,
                    "kind": kind,
                    "source_line": lines[line_number - 1].strip(),
                }
            )
        dependencies = item["build"].get("directCallDependencies", [])
        edges += len(dependencies)
        functions.append(
            {
                "address": f"0x{address:08X}",
                "source_sha256": source_hash,
                "interior_to_other_translation": item["interiorToOtherTranslation"],
                "direct_call_count": len(dependencies),
                "indirect_site_count": len(found),
            }
        )

    if missing_sources or stale_hashes:
        raise SystemExit(
            f"Source gate failed: missing={len(missing_sources)}, stale_hashes={len(stale_hashes)}"
        )

    function_map = {
        int(line.split(maxsplit=1)[0], 16)
        for line in function_map_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    }
    report = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "revision": "RM3E01_00",
        "audit_tool": {
            "path": "recomp/tools/audit_direct_translation_closure.py",
            "sha256": sha256(Path(__file__).resolve()),
        },
        "dol_sha256": "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104",
        "root": f"0x{args.root:08X}",
        "root_name": "main" if args.root == DEFAULT_ROOT else None,
        "inputs": {
            "function_map_sha256": sha256(function_map_path),
            "translation_metadata_sha256": sha256(metadata_path),
        },
        "direct_call_closure": {
            "unique_functions": len(visited),
            "missing_metadata_nodes": len(missing_metadata),
            "missing_sources": len(missing_sources),
            "source_hash_mismatches": len(stale_hashes),
            "exact_function_map_entries": sum(address in function_map for address in visited),
            "addresses_absent_from_exact_function_map": [
                f"0x{address:08X}" for address in sorted(visited) if address not in function_map
            ],
            "direct_call_edges": edges,
            "interior_translation_nodes": sum(bool(x["interior_to_other_translation"]) for x in functions),
            "interior_translation_addresses": [
                x["address"] for x in functions if x["interior_to_other_translation"]
            ],
            "indirect_functions": sum(int(x["indirect_site_count"]) > 0 for x in functions),
            "indirect_call_sites": sum(x["kind"] == "InvokeIndirectCpu" for x in indirect_sites),
            "indirect_jump_sites": sum(x["kind"] == "InvokeIndirectJump" for x in indirect_sites),
            "total_indirect_sites": len(indirect_sites),
        },
        "interpretation": "Static emitted-source closure only. It is not a dynamic trace and does not prove call reachability under game state, callback target values, ABI correctness, or screen presentation.",
        "limitations": [
            "A main-loop closure includes optional and later-game behavior; it is a superset of the first-frame path.",
            "Entries marked interior are not evidence of standalone original function boundaries.",
            "Indirect dispatch sites are enumerated but not resolved by this audit.",
        ],
        "functions": functions,
        "indirect_dispatch_sites": indirect_sites,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report["direct_call_closure"], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
