"""Inventory undefined symbols after combining generated Windows COFF objects."""
from __future__ import annotations

import hashlib
import argparse
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NM = ROOT / ".tools" / "portable-tools" / "llvm-mingw" / "bin" / "llvm-nm.exe"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-id", default="RM3E01_00_startup")
    build_id = parser.parse_args().build_id
    if not build_id.replace("_", "").isalnum():
        raise SystemExit("--build-id must contain only letters, digits, and underscores")
    objects_dir = ROOT / "build" / build_id / "shards"
    objects = sorted(objects_dir.glob("*.o"))
    if not objects:
        raise SystemExit(f"No object files under {objects_dir}")
    result = subprocess.run([str(NM), *map(str, objects)], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(result.stderr)

    undefined: set[str] = set()
    defined: set[str] = set()
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        kind, name = parts[-2:]
        if kind == "U":
            undefined.add(name)
        elif len(kind) == 1 and kind != "?":
            defined.add(name)

    unresolved = sorted(undefined - defined)
    object_set_hash = hashlib.sha256(
        "\n".join(f"{path.name}:{hashlib.sha256(path.read_bytes()).hexdigest()}" for path in objects).encode()
    ).hexdigest()
    checkpoint = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "revision": "RM3E01_00",
        "input_sha256": "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104",
        "object_count": len(objects),
        "object_set_sha256": object_set_hash,
        "unique_undefined_symbols": len(undefined),
        "symbols_defined_in_object_set": len(defined),
        "unresolved_symbols_after_combining_objects": len(unresolved),
        "unresolved_symbols": unresolved,
        "interpretation": (
            "Object-level link inventory only. It lists required runtime/compiler symbols; "
            "it does not include indirect runtime requirements or prove which calls execute before the first screen."
        ),
    }
    output = ROOT / "checkpoints" / f"{datetime.now(timezone.utc):%Y%m%d}_{build_id}_object_undefined_symbols.json"
    output.write_text(json.dumps(checkpoint, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in checkpoint.items() if key != "unresolved_symbols"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
