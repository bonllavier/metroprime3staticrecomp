#!/usr/bin/env python3
"""Verify that the ignored research extraction still matches pinned RM3E01_00 evidence."""
from __future__ import annotations

import hashlib
import json
import stat
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHECKPOINTS = ROOT / "recomp" / "checkpoints"
ISO = ROOT / "Metroid Prime 3 - Corruption (USA).iso"
EXTRACTION = ROOT / "recomp" / ".tools" / "disc-extraction-rm3e01"
DOL = ROOT / "recomp" / "input" / "RM3E01_00" / "main.dol"
EXPECTED_DOL_SHA256 = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"
EXPECTED_ISO_SHA256 = "3621e6522f99e8c6755353530f2d571489b805bee85c054a96ac52c7e32ee71e"
FRONTEND_CHECKPOINT = CHECKPOINTS / "20261020_frontend_rso_factory_translation_probe.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    iso_metadata = ISO.stat()
    if not (getattr(stat, "FILE_ATTRIBUTE_READONLY", 1) & iso_metadata.st_file_attributes):
        raise SystemExit("The source ISO is not marked read-only")
    if iso_metadata.st_size != 4_699_979_776:
        raise SystemExit(f"Unexpected ISO size: {iso_metadata.st_size}")

    dol_path = EXTRACTION / "sys" / "main.dol"
    fst_path = EXTRACTION / "sys" / "fst.bin"
    rso_path = EXTRACTION / "files" / "RSO" / "Rev2 Production" / "RSO_ScriptGUIScreenFrontEndUI.rso"
    fst = fst_path.read_bytes()
    if len(fst) < 12:
        raise SystemExit("Extracted FST is truncated")
    fst_entries = int.from_bytes(fst[8:12], "big")
    if fst_entries == 0 or 12 * fst_entries >= len(fst):
        raise SystemExit("Extracted FST entry table has invalid bounds")
    root_word = int.from_bytes(fst[0:4], "big")
    root_end = int.from_bytes(fst[8:12], "big")
    if (root_word >> 24) != 1 or root_end != fst_entries:
        raise SystemExit("Extracted FST root entry is malformed")

    dol_sha = sha256(dol_path)
    pinned_dol_sha = sha256(DOL)
    if dol_sha != EXPECTED_DOL_SHA256 or pinned_dol_sha != EXPECTED_DOL_SHA256:
        raise SystemExit("Extracted and pinned DOL do not both match RM3E01_00")
    if dol_path.read_bytes() != DOL.read_bytes():
        raise SystemExit("Extracted main.dol differs byte-for-byte from recomp/input")

    expected_rso_sha = json.loads(FRONTEND_CHECKPOINT.read_text(encoding="utf-8"))["rso_sha256"]
    rso_sha = sha256(rso_path)
    if rso_sha != expected_rso_sha:
        raise SystemExit("Extracted FrontEndUI RSO differs from the verified translation input")

    report = {
        "revision": "RM3E01_00",
        "iso_sha256_from_pinned_checkpoint": EXPECTED_ISO_SHA256,
        "iso_size_bytes": iso_metadata.st_size,
        "iso_read_only_attribute": True,
        "extraction_path": EXTRACTION.relative_to(ROOT).as_posix(),
        "extraction_is_ignored": True,
        "extracted_main_dol_sha256": dol_sha,
        "pinned_main_dol_sha256": pinned_dol_sha,
        "main_dol_byte_for_byte_match": True,
        "fst_size_bytes": len(fst),
        "fst_entry_count": fst_entries,
        "frontend_rso_sha256": rso_sha,
        "frontend_rso_matches_translation_checkpoint": True,
        "product_requires_extraction": False,
        "screen_presented": False,
    }
    output = CHECKPOINTS / "20261020_research_extraction_revalidation.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"accepted": True, "fst_entries": fst_entries, "frontend_rso_matches": True}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
