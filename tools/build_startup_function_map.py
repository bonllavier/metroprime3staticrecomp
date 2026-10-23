"""Make a restricted boundary map from a completed startup reachability walk."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

SYMBOL = re.compile(r"^(.+?) = [^:]+:0x([0-9A-Fa-f]+); // type:(?:function|label)(?: |$)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("symbols", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    names_by_address: dict[int, list[str]] = {}
    for line in args.symbols.read_text(encoding="utf-8").splitlines():
        match = SYMBOL.match(line)
        if match:
            name, raw_address = match.groups()
            names_by_address.setdefault(int(raw_address, 16), []).append(name.strip())

    metadata = json.loads(args.metadata.read_text(encoding="utf-8"))
    translated = sorted({int(item["entryPoint"]) for item in metadata["functions"]})
    missing = [address for address in translated if address not in names_by_address]

    def rank(name: str) -> tuple[int, int, int, str]:
        return (name.startswith("fn_"), name.startswith("@"), len(name), name)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        "".join(
            f"{address:08x} {min(names_by_address[address], key=rank) if address in names_by_address else f'discovered_{address:08x}'}\n"
            for address in translated
        ),
        encoding="utf-8",
        newline="\n",
    )
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
    provenance = {
        "source_symbol_map": str(args.symbols),
        "source_translation_metadata": str(args.metadata),
        "output_map_sha256": digest,
        "boundary_count": len(translated),
        "symbol_backed_boundary_count": len(translated) - len(missing),
        "translator_discovered_unverified_boundary_addresses": [f"0x{address:08X}" for address in missing],
    }
    args.output.with_name("functions_startup_provenance.json").write_text(
        json.dumps(provenance, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(provenance, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

