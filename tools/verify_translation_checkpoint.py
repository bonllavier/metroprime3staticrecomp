"""Verify translated source against the translator's output manifest and pinned DOL."""
from __future__ import annotations

import hashlib
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "third_party" / "WiiCompiled"
INPUT = ROOT / "input" / "RM3E01_00" / "main.dol"
OUTPUT = ROOT / "generated" / "RM3E01_00_startup"
EXPECTED_SHA1 = "3e81f9b09dfb1e7c02e22b9c6c48a4a1390224b2"
EXPECTED_SHA256 = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"


def digest(path: Path, algorithm: str = "sha256") -> str:
    h = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def main() -> int:
    metadata_path = OUTPUT / "base_translation_output.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    function_map = ROOT / "config" / "RM3E01_00" / "functions_startup.txt"
    map_addresses = {
        int(line.split(maxsplit=1)[0], 16)
        for line in function_map.read_text(encoding="utf-8").splitlines()
        if line.strip()
    }

    functions = metadata["functions"]
    errors: list[str] = []
    if digest(INPUT, "sha1") != EXPECTED_SHA1:
        errors.append("Input DOL SHA-1 differs from RM3E01_00 baseline")
    if digest(INPUT) != EXPECTED_SHA256:
        errors.append("Input DOL SHA-256 differs from RM3E01_00 baseline")
    if metadata["quality"]["unsupportedInstructionCount"] != 0:
        errors.append("Translator reported unsupported instructions")
    if metadata["quality"]["invalidSsaFunctionCount"] != 0:
        errors.append("Translator reported invalid SSA functions")
    if len({entry["entryPoint"] for entry in functions}) != len(functions):
        errors.append("Duplicate translated entry points")

    total_source_bytes = 0
    files: list[dict[str, object]] = []
    for entry in functions:
        source = OUTPUT / "functions" / entry["relativePath"]
        if not source.is_file():
            errors.append(f"Missing emitted source: {entry['relativePath']}")
            continue
        actual_hash = digest(source)
        actual_size = source.stat().st_size
        if actual_hash != entry["sha256"] or actual_size != entry["size"]:
            errors.append(f"Source differs from metadata: {entry['relativePath']}")
        if entry["entryPoint"] not in map_addresses:
            errors.append(f"Translated entry missing from startup map: 0x{entry['entryPoint']:08X}")
        total_source_bytes += actual_size
        files.append({
            "entry_point": f"0x{entry['entryPoint']:08X}",
            "path": entry["relativePath"],
            "size": actual_size,
            "sha256": actual_hash,
            "interior_to_other_translation": entry["interiorToOtherTranslation"],
        })

    interior = [item["entry_point"] for item in files if item["interior_to_other_translation"]]
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=UPSTREAM, text=True).strip()
    quality = metadata["quality"]
    checkpoint = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "game": "Metroid Prime 3: Corruption",
        "revision": "RM3E01_00",
        "input": {
            "path": "input/RM3E01_00/main.dol",
            "sha1": digest(INPUT, "sha1"),
            "sha256": digest(INPUT),
        },
        "translator": {
            "repository": "https://github.com/patchzyy/Wiicompiled",
            "commit": commit,
            "working_tree_clean": not bool(subprocess.check_output(
                ["git", "status", "--porcelain"], cwd=UPSTREAM, text=True
            ).strip()),
        },
        "entry_point": "0x80006320",
        "sda_base": "0x806801C0",
        "sda2_base": "0x806869C0",
        "function_map": {
            "path": "config/RM3E01_00/functions_startup.txt",
            "sha256": digest(function_map),
            "boundary_count": len(map_addresses),
            "translator_discovered_unverified": json.loads(
                function_map.with_name("functions_startup_provenance.json").read_text(encoding="utf-8")
            )["translator_discovered_unverified_boundary_addresses"],
        },
        "translation": {
            "command": "translate-recursive 0x80006320 --project project_startup.yml --threads 8",
            "allow_unsupported_instructions": False,
            "translated_function_count": len(functions),
            "unsupported_instruction_count": quality["unsupportedInstructionCount"],
            "invalid_ssa_function_count": quality["invalidSsaFunctionCount"],
            "interior_to_other_translation_count": len(interior),
            "interior_to_other_translation_addresses": interior,
            "generated_source_bytes": total_source_bytes,
            "sources": files,
        },
        "runtime_build": "not performed",
        "game_execution": "not performed",
        "errors": errors,
    }
    checkpoint_path = ROOT / "checkpoints" / "20261020_startup_translation.json"
    checkpoint_path.parent.mkdir(parents=True, exist_ok=True)
    checkpoint_path.write_text(json.dumps(checkpoint, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in checkpoint.items() if key != "translation"}, indent=2))
    print(json.dumps({key: value for key, value in checkpoint["translation"].items() if key != "sources"}, indent=2))
    if errors:
        raise SystemExit(1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

