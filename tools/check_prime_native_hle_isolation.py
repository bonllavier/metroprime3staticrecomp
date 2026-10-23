#!/usr/bin/env python3
"""Verify the RM3E01_00 native-HLE isolation probe and archive its runtime result."""
from __future__ import annotations

import hashlib
import json
import re
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROFILE = "RM3E01_00"
EXPECTED_DOL_SHA256 = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"
BUILD = ROOT / "recomp/build/RM3E01_00_runtime_link_probe"
RUN = BUILD / "UserData/Logs/base_1792530393_pid9196"
AUDIT = ROOT / "recomp/checkpoints/20261020_hle_registration_function_range_audit.json"
JUMP_TABLE = ROOT / "recomp/checkpoints/20261020_prime_crash_jump_table.json"
OUT = ROOT / "recomp/checkpoints/20261020_prime_native_hle_isolation_probe.json"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    dol = ROOT / "decomp/orig/RM3E01_00/sys/main.dol"
    exe = BUILD / "WiiCompiled.exe"
    crash = RUN / "crash_missing_jump_target.txt"
    console = RUN / "console.log"
    audit = json.loads(AUDIT.read_text(encoding="utf-8"))
    jump_table = json.loads(JUMP_TABLE.read_text(encoding="utf-8"))
    if sha(dol) != EXPECTED_DOL_SHA256:
        raise SystemExit("The source DOL is not the pinned RM3E01_00 image")
    if not exe.is_file() or not crash.is_file() or not console.is_file():
        raise SystemExit("The executable or this run's crash artifacts are missing")
    summary = audit["summary"]
    if summary["classification_counts"] != {"inside_sized_rm3_function": 566}:
        raise SystemExit(f"Unexpected Prime HLE registration audit: {summary}")
    if summary["registered_addresses_in_main_direct_closure"] != 0:
        raise SystemExit("The audited fixed HLE set changed; review before using this checkpoint")
    table = jump_table["table"]
    if (table["virtual_address"] != "0x805A4480" or table["index"] != 14 or
            table["expected_target"] != "0x804C9770" or
            table["crash_snapshot_target"] != "0x00000000" or table["matches"]):
        raise SystemExit(f"The Prime jump-table crash evidence changed: {table}")

    bridge = ROOT / "recomp/third_party/WiiCompiled/runtime/include/abi_bridge.h"
    bridge_text = bridge.read_text(encoding="utf-8")
    native_gate_count = len(re.findall(
        r"#if defined\(MKW_GAME_PROFILE_RM3E01_00\)[\s\S]{0,350}?if \(kind == FunctionKind::Native\) return;",
        bridge_text,
    ))
    if native_gate_count != 2:
        raise SystemExit(f"Expected both ABI trampoline specializations to gate Native entries; got {native_gate_count}")

    crash_text = crash.read_text(encoding="utf-8")
    console_text = console.read_text(encoding="utf-8")
    required = [
        "guest jump target: 0x0",
        "lr=0x804C9480<fn_804C9174+0x30C>",
        "r3=0x00000000",
        "ctr=0x00000000",
        "r26=0x0000000E",
        "r27=0x00000020",
    ]
    if any(token not in crash_text for token in required):
        raise SystemExit("Crash artifact does not match the recorded null callback path")
    trace = console_text.split("[runtime] host stack trace", 1)[-1]
    if "OS__GetTime_HLE" in trace or "OS____GetSystemTime_801aad7c" in trace:
        raise SystemExit("The filtered Mario Kart time HLE still appears on the runtime stack")

    sources = [
        ROOT / "recomp/third_party/WiiCompiled/runtime/include/abi_bridge.h",
        ROOT / "recomp/third_party/WiiCompiled/runtime/src/memory.cpp",
        ROOT / "recomp/tools/audit_hle_registration_interior_targets.py",
        ROOT / "recomp/tools/verify_prime_crash_jump_table.py",
        AUDIT,
        JUMP_TABLE,
        exe,
        console,
        crash,
    ]
    report = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "revision": PROFILE,
        "dol_sha256": EXPECTED_DOL_SHA256,
        "executable_sha256": sha(exe),
        "build": {
            "directory": BUILD.relative_to(ROOT).as_posix(),
            "command": "ninja -C recomp/build/RM3E01_00_runtime_link_probe WiiCompiled.exe",
            "result": "pass",
        },
        "native_hle_isolation": {
            "profile_guard": "MKW_GAME_PROFILE_RM3E01_00",
            "rejected_kind": "FunctionKind::Native",
            "constructor_specializations_checked": native_gate_count,
            "fixed_registration_audit": summary,
            "audit_checkpoint": AUDIT.relative_to(ROOT).as_posix(),
            "meaning": "Inherited native HLE registrations are not installed in this profile; title-specific replacements remain to be implemented and verified.",
        },
        "runtime_probe": {
            "historical_log_only": True,
            "association_with_current_build": "Not established: current build verification and the archived runtime log are separate evidence.",
            "run_directory": RUN.relative_to(ROOT).as_posix(),
            "runtime_started": True,
            "window_and_d3d12_initialized": "[runtime] graphics backend: d3d12" in console_text,
            "screen_presented": False,
            "fatal_reason": "missing_jump_target",
            "guest_target": "0x00000000",
            "lr": "0x804C9480",
            "active_path": ["0x80006320", "0x804DE05C", "0x804CA2BC", "0x804C9174"],
            "next_guest_operation": "The PPC state-machine dispatch at 0x804C949C reads table 0x805A4480 + event_type*4. The event_type 0x0E entry should dispatch to 0x804C9770 per the DOL, but the crash MEM1 snapshot contains zero at 0x805A44B8, producing CTR=0. The exact initialization/overwrite point is unknown.",
            "jump_table_probe": {
                "checkpoint": JUMP_TABLE.relative_to(ROOT).as_posix(),
                "dol_entry_target": table["expected_target"],
                "crash_snapshot_entry": table["crash_snapshot_target"],
                "entry_address": table["entry_address"],
            },
            "evidence": {
                "console_log": (RUN / "console.log").relative_to(ROOT).as_posix(),
                "crash_artifact": crash.relative_to(ROOT).as_posix(),
            },
        },
        "source_hashes": {path.relative_to(ROOT).as_posix(): sha(path) for path in sources},
        "limitations": [
            "No game screen was rendered.",
            "The crash-time jump-table entry is zero although the pinned DOL contains a valid target; its initialization/overwrite point is not yet traced.",
            "The current DVD runtime still depends on a private research extraction.",
            "The inherited native HLE services have been disabled for RM3E01_00 pending title-specific implementations.",
        ],
    }
    OUT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"checkpoint": OUT.relative_to(ROOT).as_posix(), "screen_presented": False,
                      "audit_count": summary["registration_sites"], "executable_sha256": report["executable_sha256"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
