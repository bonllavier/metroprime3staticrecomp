"""Check whether guest addresses referenced by runtime HLE exist as RM3E01 starts."""
from __future__ import annotations

import argparse
import hashlib
import json
from bisect import bisect_left
from pathlib import Path


TARGETS = {
    "func_801D9E94": "network_config.cpp",
    "func_801D8D30": "network_config.cpp",
    "func_8012B830": "audio/ax_effects.cpp",
    "func_801A0620": "os/os_alarm.cpp",
    "func_801AADE0": "os/os_alarm.cpp",
    "func_8055531C": "os/os_init.cpp",
    "func_801A1ED8": "os/os_scheduler.cpp",
    "func_801A961C": "os/os_init.cpp",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("function_map", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    symbols: list[tuple[int, str]] = []
    for line in args.function_map.read_text(encoding="utf-8").splitlines():
        fields = line.split(maxsplit=1)
        if len(fields) == 2:
            symbols.append((int(fields[0], 16), fields[1]))
    symbols.sort()
    addresses = [item[0] for item in symbols]

    results = []
    for name, source in TARGETS.items():
        address = int(name.removeprefix("func_"), 16)
        pos = bisect_left(addresses, address)
        results.append({
            "symbol": name,
            "address": f"0x{address:08X}",
            "runtime_source": source,
            "exact_rm3e01_function_start": pos < len(symbols) and symbols[pos][0] == address,
            "nearest_lower_map_entry": (
                {"address": f"0x{symbols[pos - 1][0]:08X}", "name": symbols[pos - 1][1]}
                if pos else None
            ),
            "nearest_upper_map_entry": (
                {"address": f"0x{symbols[pos][0]:08X}", "name": symbols[pos][1]}
                if pos < len(symbols) else None
            ),
        })

    report = {
        "revision": "RM3E01_00",
        "dol_sha256": "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104",
        "function_map": str(args.function_map),
        "function_map_sha256": hashlib.sha256(args.function_map.read_bytes()).hexdigest(),
        "function_map_entry_count": len(symbols),
        "interpretation": "An address absent from this map is not proven to be a callable RM3E01 function entry; neighboring symbols are context only.",
        "targets": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"targets": len(results), "exact_starts": sum(x["exact_rm3e01_function_start"] for x in results), "output": str(args.output)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
