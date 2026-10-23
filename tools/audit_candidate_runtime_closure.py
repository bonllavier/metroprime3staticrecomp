#!/usr/bin/env python3
"""Inventory undefined symbols after adding the eight generic PPC runtime objects.

This is an object-level inventory only. It does not perform a final link and does
not classify symbols satisfied by host libraries or by game-specific bindings.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RECOMP = ROOT / "recomp"
BASE_OBJECT_DIR = RECOMP / "build" / "RM3E01_00_conservative" / "shards"
CORE_OBJECT_DIR = RECOMP / "build" / "runtime-core"
CORE_OBJECTS = (
    "abi_bridge.o",
    "fpu_helpers.o",
    "guest_flat_memory.o",
    "memory.o",
    "ppc_helpers.o",
    "ppc_quantized.o",
    "recomp_mod_loader.o",
    "system_bridge.o",
)
OUTPUT = RECOMP / "checkpoints" / "20261020_translated_plus_core_symbol_audit.json"
LLVM_NM = RECOMP / ".tools" / "portable-tools" / "llvm-mingw" / "bin" / "llvm-nm.exe"


def run_nm(objects: list[Path], mode: str) -> set[str]:
    result = subprocess.run(
        [str(LLVM_NM), "--format=posix", mode, *(str(path) for path in objects)],
        check=True,
        capture_output=True,
        text=True,
    )
    symbols: set[str] = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        symbol, kind = fields[0], fields[1]
        if mode == "--undefined-only" and kind in {"U", "u", "w", "v"}:
            symbols.add(symbol)
        elif mode == "--defined-only" and kind not in {"U", "u", "w", "v"}:
            symbols.add(symbol)
    return symbols


def main() -> int:
    if not BASE_OBJECT_DIR.is_dir() or not CORE_OBJECT_DIR.is_dir() or not LLVM_NM.is_file():
        raise FileNotFoundError("Expected conservative translated objects and runtime-core objects")

    base_objects = sorted(BASE_OBJECT_DIR.glob("*.o"))
    core_objects = [CORE_OBJECT_DIR / name for name in CORE_OBJECTS]
    if len(base_objects) != 92 or any(not path.is_file() for path in core_objects):
        raise RuntimeError(
            f"Unexpected object set: translated={len(base_objects)}, core_present="
            f"{sum(path.is_file() for path in core_objects)}/{len(core_objects)}"
        )

    objects = base_objects + core_objects
    manifest = []
    for path in objects:
        relative = path.relative_to(RECOMP).as_posix()
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        manifest.append((relative, digest))
    object_set_hash = hashlib.sha256(
        "".join(f"{relative} {digest}\n" for relative, digest in manifest).encode("utf-8")
    ).hexdigest()

    undefined = run_nm(objects, "--undefined-only")
    defined = run_nm(objects, "--defined-only")
    unresolved = sorted(undefined - defined)

    checkpoint = {
        "created_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "game_revision": "RM3E01_00",
        "input_dol_sha256": "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104",
        "translated_object_count": len(base_objects),
        "generic_runtime_core_object_count": len(core_objects),
        "combined_object_count": len(objects),
        "combined_object_set_sha256": object_set_hash,
        "unique_undefined_symbols": len(undefined),
        "unique_defined_symbols": len(defined),
        "unresolved_after_combining_objects": len(unresolved),
        "unresolved_symbols": unresolved,
        "interpretation": (
            "Object-level inventory only; unresolved names may be provided by host system/runtime "
            "libraries, WiiCompiled glue, or title-specific bindings. This is not a final link, "
            "does not prove reachability, and does not establish runtime correctness."
        ),
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(checkpoint, indent=2) + "\n", encoding="utf-8")
    print(
        f"Wrote {OUTPUT.relative_to(ROOT)}: {len(objects)} objects, "
        f"{len(unresolved)} unresolved object symbols"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
