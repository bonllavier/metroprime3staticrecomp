#!/usr/bin/env python3
"""Verify the bounded Prime guest-thread context-transfer probe."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKPOINT = ROOT / "recomp/checkpoints/20261020_prime_thread_context_transfer_probe.json"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    checkpoint = json.loads(CHECKPOINT.read_text(encoding="utf-8"))
    build = checkpoint["build"]
    runtime = checkpoint["runtime"]
    exe = ROOT / build["executable"]
    dol = ROOT / build["dol"]
    log_path = ROOT / runtime["log"]
    if sha256(exe) != build["sha256"]:
        raise SystemExit("Executable hash differs from checkpoint")
    if sha256(dol) != build["dol_sha256"]:
        raise SystemExit("DOL hash differs from the RM3E01_00 checkpoint")
    if sha256(log_path) != runtime["log_sha256"]:
        raise SystemExit("Runtime log hash differs from checkpoint")

    log = log_path.read_text(encoding="utf-8", errors="replace")
    wait = re.search(r"\[yield-wait\] iter=7 caller=803C41DC .*", log)
    switch = re.search(
        r"\[prime-ipc-context-switch\] running=0x0->0x8060b418 .*?"
        r"interrupted_lr=0x804e6d08 interrupted_r1=0x80699da0 "
        r"callback_lr=0x0 callback_r1=0x8069a0b0",
        log,
        re.IGNORECASE,
    )
    if wait is None or "reschedule returned iter=7" in log:
        raise SystemExit("Expected blocked wait signature is absent or changed")
    if switch is None:
        raise SystemExit("Expected Prime IPC context-switch diagnostic is absent")
    if "[fatal]" in log.lower() or "guest memory access was out of bounds" in log.lower():
        raise SystemExit("Runtime log contains a fatal guest-memory failure")

    for item in checkpoint["instrumentation"]:
        source = ROOT / item["file"]
        if sha256(source) != item["sha256"]:
            raise SystemExit(f"Instrumentation source hash differs: {item['file']}")
    interrupt_source = (ROOT / checkpoint["instrumentation"][0]["file"]).read_text(
        encoding="utf-8"
    )
    if "*current = *callback" in interrupt_source:
        raise SystemExit("Rejected callback-context copyback is present")

    iso = ROOT / "Metroid Prime 3 - Corruption (USA).iso"
    iso_stat = iso.stat()
    if iso_stat.st_size != checkpoint["invariants"]["iso_size"]:
        raise SystemExit("ISO size changed")
    if not (iso_stat.st_mode & 0o222) == 0:
        raise SystemExit("ISO is no longer read-only")

    print(json.dumps({
        "result": "PASS",
        "blocked_wait_lr": "0x803C41DC",
        "guest_thread_switch_observed": True,
        "callback_copyback_present": False,
        "fatal_error": False,
        "screen_confirmed": False,
        "checkpoint": str(CHECKPOINT.relative_to(ROOT)),
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
