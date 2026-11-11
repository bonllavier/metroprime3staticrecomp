#!/usr/bin/env python3
"""Writes each Trilogy DOL's function map and translator project from its dtk analysis."""
import hashlib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TITLES = {
    "R3ME01_MP3": ("Metroid Prime 3 from Metroid Prime Trilogy USA", 0x800060A4, 0x80672260, 0x80678B00,
                   "e86908b9311e71cc0eb635b105c416d8f8d83607"),
    "R3ME01_FE": ("Metroid Prime Trilogy USA menu", 0x800060A4, 0x80622220, 0x80628580,
                  "d55b890c4e7c2cc6f2e120ec4ef988c186780308"),
}
SYMBOL = re.compile(r"(\S.*?) = \.(?:text|init):0x([0-9A-Fa-f]{8}); // type:function size:0x([0-9A-Fa-f]+)")


def main() -> None:
    for title, (name, entry, sda, sda2, sha1) in TITLES.items():
        base = ROOT / "titles" / title
        dol = (base / "extract/sys/main.dol").read_bytes()
        if hashlib.sha1(dol).hexdigest() != sha1:
            raise ValueError(f"{title}: unexpected DOL")
        if int.from_bytes(dol[0xE0:0xE4], "big") != entry:
            raise ValueError(f"{title}: unexpected entry point")
        names: dict[int, str] = {}
        for line in (base / "dtk/config/symbols.txt").read_text(encoding="utf-8").splitlines():
            match = SYMBOL.match(line)
            if match:
                names.setdefault(int(match[2], 16), match[1])
        (base / "functions.txt").write_text(
            "".join(f"{address:08X} {names[address]}\n" for address in sorted(names)), encoding="utf-8")
        (base / "project.yml").write_text(f"""schema_version: 1
workspace_root: .

project:
  id: {title.lower().replace('_', '-')}
  display_name: {name}
  game_id: R3ME01
  region: E

memory:
  base: 0x80000000
  size: 0x00700000
  sda_base: 0x{sda:08X}
  sda2_base: 0x{sda2:08X}

inputs:
  dol:
    path: extract/sys/main.dol
    sha256: {hashlib.sha256(dol).hexdigest()}

translation:
  entry_points:
    - 0x{entry:08X}
  function_map:
    path: functions.txt
  allow_unsupported_instructions: false
  enable_state_free_abi: false

runtime:
  native_abi_directories: []

output:
  root: generated
  functions: functions
  runtime_config: RuntimeConfig.h
  data_initializer: data_sections_init.cpp
  base_manifest: generated/base_manifest.json
""", encoding="utf-8")
        print(title, len(names), "functions")


if __name__ == "__main__":
    main()
