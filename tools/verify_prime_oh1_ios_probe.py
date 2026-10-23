#!/usr/bin/env python3
"""Verify the immutable artifacts and invariants of the Prime OH1 IOS probe."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHECKPOINT = ROOT / "recomp/checkpoints/20261020_prime_oh1_ios_probe.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    checkpoint = json.loads(CHECKPOINT.read_text(encoding="utf-8"))
    build = checkpoint["build"]
    runtime = checkpoint["runtime"]
    invariants = checkpoint["invariants"]

    for key, relative in (("executable", build["executable"]), ("runtime log", runtime["log"])):
        path = ROOT / relative
        if not path.is_file():
            raise SystemExit(f"missing {key}: {path}")
    if sha256(ROOT / build["executable"]) != build["sha256"]:
        raise SystemExit("executable SHA-256 mismatch")
    if sha256(ROOT / runtime["log"]) != runtime["log_sha256"]:
        raise SystemExit("runtime log SHA-256 mismatch")
    if sha256(ROOT / build["dol"]) != build["dol_sha256"]:
        raise SystemExit("RM3E01_00 DOL SHA-256 mismatch")

    log = (ROOT / runtime["log"]).read_text(encoding="utf-8", errors="replace")
    required_log_evidence = (
        "[yield-wait] reschedule returned iter=10",
        "virtual OH1 endpoint '/dev/usb/oh1/57e/305' -> fd 131073",
        "OH1 ioctlv unsupported fd=131073 cmd=2 in=2 out=1",
        "OH1 vector 0 addr=17FC1E80 size=1 bytes=81",
        "OH1 vector 1 addr=17FC1EC0 size=2 bytes=02 5C",
        "OH1 vector 2 addr=102A0900 size=604",
    )
    for evidence in required_log_evidence:
        if evidence not in log:
            raise SystemExit(f"runtime log missing evidence: {evidence}")
    if "Fatal Error" in log or "guest memory access was out of bounds" in log:
        raise SystemExit("runtime log contains a fatal guest error")

    iso = ROOT / invariants["iso_path"]
    if not iso.is_file() or iso.stat().st_size != invariants["iso_size"]:
        raise SystemExit("ISO missing or size changed")
    if not (iso.stat().st_file_attributes & 0x1):
        raise SystemExit("ISO is no longer marked read-only")
    if invariants["matching_dol_modified"] or invariants["baseline_decomp_modified"]:
        raise SystemExit("checkpoint claims a baseline mutation")
    if invariants["guest_hci_state_forced"] or invariants["screen_claimed"]:
        raise SystemExit("checkpoint claims a forced state or unverified screen")

    print("PASS: OH1 probe executable, log, DOL, and ISO invariants verified")
    print("PASS: log confirms completed retrace waits and the captured OH1 command-2 vectors")
    print("LIMIT: HCI ioctlv remains unsupported; no game screen is confirmed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
