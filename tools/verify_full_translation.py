"""Verify generated functions against the translator manifest and complete function map."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPECTED_SHA1 = "3e81f9b09dfb1e7c02e22b9c6c48a4a1390224b2"
EXPECTED_SHA256 = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"
TRANSLATOR_COMMIT = "4c166a7eb1ddfc79b4c832e422e6bb00ec06fb4f"


def digest(path: Path, algorithm: str = "sha256") -> str:
    h = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-id", default="RM3E01_00_conservative")
    args = parser.parse_args()
    if not args.build_id.replace("_", "").isalnum():
        raise SystemExit("--build-id must contain only letters, digits, and underscores")

    generated = ROOT / "generated" / args.build_id
    input_dol = ROOT / "input" / "RM3E01_00" / "main.dol"
    map_path = ROOT / "config" / "RM3E01_00" / "functions.txt"
    metadata = json.loads((generated / "base_translation_output.json").read_text(encoding="utf-8"))
    map_bytes = map_path.read_bytes()
    mapped = {
        int(line.split()[0], 16)
        for line in map_bytes.decode("utf-8").splitlines()
        if line.strip()
    }
    entries = metadata["functions"]
    by_address = {int(item["entryPoint"]): item for item in entries}
    missing = sorted(mapped - set(by_address))
    duplicates = len(entries) - len(by_address)
    failures: list[dict[str, str]] = []
    source_bytes = 0
    for item in entries:
        source = generated / "functions" / item["relativePath"]
        if not source.is_file():
            failures.append({"path": item["relativePath"], "error": "missing"})
            continue
        raw = source.read_bytes()
        source_bytes += len(raw)
        if len(raw) != item["size"] or hashlib.sha256(raw).hexdigest() != item["sha256"]:
            failures.append({"path": item["relativePath"], "error": "size/hash mismatch"})

    input_sha1 = digest(input_dol, "sha1")
    input_sha256 = digest(input_dol)
    unsupported = metadata["quality"]["unsupportedInstructionCount"]
    invalid_ssa = metadata["quality"]["invalidSsaFunctionCount"]
    checkpoint = {
        "game": "Metroid Prime 3: Corruption",
        "revision": "RM3E01_00",
        "input_sha1": input_sha1,
        "input_sha256": input_sha256,
        "translator_commit": TRANSLATOR_COMMIT,
        "project": "config/RM3E01_00/project_full_conservative.yml" if args.build_id == "RM3E01_00_conservative" else args.build_id,
        "function_map_sha256": hashlib.sha256(map_bytes).hexdigest(),
        "function_map_entries": len(mapped),
        "translated_entries": len(entries),
        "unique_translated_entry_points": len(by_address),
        "function_map_entries_missing_from_translation": [f"0x{address:08X}" for address in missing],
        "duplicate_translation_entries": duplicates,
        "source_bytes_verified": source_bytes,
        "source_verification_failures": failures,
        "unsupported_instruction_count": unsupported,
        "invalid_ssa_function_count": invalid_ssa,
        "interior_to_other_translation_count": sum(bool(item.get("interiorToOtherTranslation")) for item in entries),
        "state_free_abi_enabled": args.build_id != "RM3E01_00_conservative",
        "accepted": (
            input_sha1 == EXPECTED_SHA1
            and input_sha256 == EXPECTED_SHA256
            and not missing
            and duplicates == 0
            and not failures
            and unsupported == 0
            and invalid_ssa == 0
        ),
    }
    checkpoint_name = "20261020_full_conservative_translation.json" if args.build_id == "RM3E01_00_conservative" else f"20261020_{args.build_id}_translation.json"
    output = ROOT / "checkpoints" / checkpoint_name
    output.write_text(json.dumps(checkpoint, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(checkpoint, indent=2))
    return 0 if checkpoint["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
