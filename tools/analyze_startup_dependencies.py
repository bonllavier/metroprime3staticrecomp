"""List statically identified direct call targets outside the startup translation set."""
from __future__ import annotations

import json
import re
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
METADATA = ROOT / "generated" / "RM3E01_00_startup" / "base_translation_output.json"
SYMBOLS = ROOT.parent / "decomp" / "config" / "RM3E01_00" / "symbols.txt"


def main() -> int:
    metadata = json.loads(METADATA.read_text(encoding="utf-8"))
    translated = {int(item["entryPoint"]) for item in metadata["functions"]}
    names: dict[int, list[str]] = {}
    symbol_pattern = re.compile(r"^(.+?) = [^:]+:0x([0-9A-Fa-f]+); // type:(?:function|label)(?: |$)")
    for line in SYMBOLS.read_text(encoding="utf-8").splitlines():
        match = symbol_pattern.match(line)
        if match:
            name, raw_address = match.groups()
            names.setdefault(int(raw_address, 16), []).append(name.strip())

    uses: Counter[int] = Counter()
    for item in metadata["functions"]:
        for target in item["build"].get("directCallDependencies", []):
            target = int(target)
            if target not in translated:
                uses[target] += 1
    output = {
        "game_revision": "RM3E01_00",
        "startup_translated_functions": len(translated),
        "unique_direct_call_targets_outside_translation": len(uses),
        "direct_call_edges_outside_translation": sum(uses.values()),
        "targets": [
            {
                "address": f"0x{address:08X}",
                "call_site_count": count,
                "symbols": sorted(set(names.get(address, []))),
            }
            for address, count in uses.most_common()
        ],
        "limitations": "The translator metadata lists direct-call dependencies only; indirect calls, SDK/system effects, dynamic modules, and reachability at runtime are not proven by this report.",
    }
    path = ROOT / "checkpoints" / "20261020_startup_external_calls.json"
    path.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in output.items() if key != "targets"}, indent=2))
    print(json.dumps(output["targets"][:40], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

