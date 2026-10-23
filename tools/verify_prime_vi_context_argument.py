#!/usr/bin/env python3
"""Verify the RM3E01_00 VI callback-context probe and its pinned artifacts."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHECKPOINT = ROOT / "recomp/checkpoints/20261021_prime_vi_context_argument.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def main() -> int:
    data = json.loads(CHECKPOINT.read_text(encoding="utf-8"))
    assert data["revision"] == "RM3E01_00"

    dol = ROOT / data["artifacts"]["dol"]["path"]
    exe = ROOT / data["artifacts"]["executable"]["path"]
    log = ROOT / data["runtime"]["log"]
    assert sha256(dol) == data["artifacts"]["dol"]["sha256"]
    assert sha256(exe) == data["artifacts"]["executable"]["sha256"]
    assert sha256(log) == data["runtime"]["log_sha256"]

    log_text = log.read_text(encoding="utf-8", errors="replace")
    required = (
        "[prime-vi-context] n=3 prior_r4=00009002 current=8060B830 running=00000000 selected=8060B830 valid=1",
        "[prime-vi-entry] n=3 lr=804E6D08 r1=80699DA0 r3=00000018 r4=8060B830",
        "[ipc-queue-trace] req=97FC00E0 cmd=7 caller_lr=80505524 thread=8060B418 outstanding=0 head=28 tail=29",
        "[ipc-queue-return] req=97FC00E0 cmd=7 sync=1 result=0 stored=0",
    )
    for evidence in required:
        assert evidence in log_text, f"missing runtime evidence: {evidence}"
    assert "guest memory access was out of bounds" not in log_text.lower()
    assert "[fatal]" not in log_text.lower()

    iso = ROOT / data["invariants"]["iso_path"]
    assert iso.stat().st_size == data["invariants"]["iso_size"]
    if os.name == "nt":
        assert iso.stat().st_file_attributes & 1, "ISO is no longer read-only"

    print("Prime VI context probe: PASS")
    print("DOL, executable, runtime log, VI context arguments, and ISO invariant: PASS")
    print("The checkpoint explicitly records that no game screen was observed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
