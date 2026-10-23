#!/usr/bin/env python3
"""Verify the bounded RM3E01_00 IPC caller trace and its source LR mapping."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKPOINT = ROOT / "recomp/checkpoints/20261020_prime_ipc_queue_caller_trace.json"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    checkpoint = json.loads(CHECKPOINT.read_text(encoding="utf-8"))
    runtime = checkpoint["runtime"]
    log_path = ROOT / runtime["log"]
    exe_path = ROOT / checkpoint["build"]["executable"]
    dol_path = ROOT / "recomp/.tools/disc-extraction-rm3e01/sys/main.dol"
    if sha256(log_path) != runtime["log_sha256"]:
        raise SystemExit("Runtime log hash differs from the checkpoint")
    if sha256(exe_path) != checkpoint["build"]["sha256"]:
        raise SystemExit("Runtime executable hash differs from the checkpoint")
    if sha256(dol_path) != checkpoint["build"]["dol_sha256"]:
        raise SystemExit("RM3E01_00 DOL hash differs from the pinned revision")

    shard = ROOT / checkpoint["instrumentation"]["file"]
    source_0441c = ROOT / "recomp/generated/RM3E01_00_conservative/functions/func_8050441C.cpp"
    if "caller_lr=%08X" not in shard.read_text(encoding="utf-8"):
        raise SystemExit("Corrected caller_lr diagnostic is absent from the generated shard")
    if "MemoryInline::FlatRead32(r1 + 36u)" not in shard.read_text(encoding="utf-8"):
        raise SystemExit("Caller LR is not read from the saved LR slot")
    if "MemoryInline::FlatWriteRam32((r1 + 36), r0)" not in source_0441c.read_text(encoding="utf-8"):
        raise SystemExit("func_8050441C source no longer supports the LR offset")

    log = log_path.read_text(encoding="utf-8", errors="replace")
    pattern = re.compile(
        r"\[ipc-queue-trace\] req=([0-9A-F]{8}) cmd=(\d+) caller_lr=([0-9A-F]{8}) "
        r"thread=([0-9A-F]{8}) outstanding=(-?\d+) head=(\d+) tail=(\d+) get=(\d+) put=(\d+)"
    )
    parsed = [tuple(int(value, 16) if i in (0, 2, 3) else int(value)
                    for i, value in enumerate(match)) for match in pattern.findall(log)]
    expected = [
        (6, 0x805050F8, 0x8060B418, 1, 27, 28),
        (7, 0x80505524, 0x8060B418, 0, 28, 29),
        (1, 0x80504878, 0x8060B418, 0, 28, 30),
        (7, 0x80505448, 0x8060B418, 0, 28, 31),
        (7, 0x80505448, 0x8060B418, 0, 28, 32),
        (7, 0x80505448, 0x8060B418, 0, 28, 33),
    ]
    # Compare the final six queue entries, ignoring request pointer and ring get/put fields.
    actual = [(item[1], item[2], item[3], item[4], item[5], item[6]) for item in parsed[-6:]]
    if actual != expected:
        raise SystemExit(f"Final IPC queue sequence differs: {actual!r}")
    for lr, file_name in (("805050F8", "func_80504FE0.cpp"),
                          ("80505524", "func_80505468.cpp"),
                          ("80504878", "func_80504780.cpp"),
                          ("80505448", "func_80505384.cpp")):
        source = ROOT / "recomp/generated/RM3E01_00_conservative/functions" / file_name
        if f"0x{lr}u" not in source.read_text(encoding="utf-8"):
            raise SystemExit(f"Saved caller LR 0x{lr} lacks a matching source return site")
    if "[fatal]" in log.lower() or "guest memory access was out of bounds" in log.lower():
        raise SystemExit("Runtime log contains a fatal guest-memory failure")

    print(json.dumps({
        "result": "PASS",
        "verified_final_queue_entries": len(expected),
        "verified_lr_source_sites": 4,
        "fatal_error": False,
        "screen_observed": False,
        "checkpoint": str(CHECKPOINT.relative_to(ROOT)),
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
