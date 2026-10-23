"""Build a WiiCompiled address/name map from this title's dtk symbol file."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections import defaultdict
from pathlib import Path

SYMBOL = re.compile(r"^(.+?) = [^:]+:0x([0-9A-Fa-f]+); // type:function(?: |$)")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("symbols", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    by_address: dict[int, list[str]] = defaultdict(list)
    for line in args.symbols.read_text(encoding="utf-8").splitlines():
        match = SYMBOL.match(line)
        if match:
            name, address = match.groups()
            by_address[int(address, 16)].append(name.strip())
    if not by_address:
        raise SystemExit("No `type:function` entries found in symbol file")

    def rank(name: str) -> tuple[int, int, str]:
        # Preserve meaningful names for HLE binding; use generated address names last.
        return (name.startswith("fn_"), name.startswith("@"), len(name), name)

    entries = [(address, min(set(names), key=rank)) for address, names in by_address.items()]
    entries.sort()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        "".join(f"{address:08x} {name}\n" for address, name in entries),
        encoding="utf-8",
        newline="\n",
    )
    aliases = {
        f"0x{address:08X}": sorted(set(names))
        for address, names in by_address.items()
        if len(set(names)) > 1
    }
    provenance = {
        "source_symbols": str(args.symbols),
        "source_symbols_sha256": sha256(args.symbols),
        "output_map": str(args.output),
        "output_map_sha256": sha256(args.output),
        "unique_function_addresses": len(entries),
        "duplicate_address_groups": len(aliases),
        "duplicate_address_aliases": aliases,
        "selection_rule": "One name per unique address; prefer non-generated and non-local-looking names for native registration. This is boundary metadata, not recovered code.",
    }
    provenance_path = args.output.with_name("function_map_provenance.json")
    provenance_path.write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in provenance.items() if key != "duplicate_address_aliases"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

