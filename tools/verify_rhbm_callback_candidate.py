#!/usr/bin/env python3
"""Revalidate the private translation probe for RSO_rhbm section 1 + 0xC690."""
from __future__ import annotations

import hashlib
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "recomp/.tools/rso-rel"
EXTRACTION = ROOT / "recomp/.tools/disc-extraction-rm3e01/files/RSO/Rev2 Production/RSO_rhbm.rso"
OUT = ROOT / "recomp/checkpoints/20261020_rhbm_callback_candidate_probe.json"
EXPECTED_RSO_SHA256 = "05869cc26411f6b856aedde54ef2aa89d87c72132c643d9469b89b633e80797f"
EXPECTED_REL_SHA256 = "15424c386904ed96657adb71bc252f9ebaa57a87b5a7f221d7ebd246d0fc2097"
RUNTIME_LOG = ROOT / "recomp/build/RM3E01_00_runtime_link_probe/UserData/Logs/base_1792541750_pid1620/console.log"
RUNTIME_LOG_SHA256 = "8234839a1161530428683f719a06d013a1c87e070934c67b2ea6c0d0f07a1982"
RUNTIME_EXE_SHA256 = "3a8076033b12a93056d73fe44200ebf681ca6d3f10d6f98c9261910c1d8cbb56"
EXPECTED_FUNCTIONS = {
    0x806993E0,
    0x806A36B8,
    0x806A421C,
    0x806A4224,
    0x806A4C70,
    0x806A5E88,
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    raw = EXTRACTION.read_bytes()
    if sha256(EXTRACTION) != EXPECTED_RSO_SHA256:
        raise SystemExit("RSO_rhbm extraction hash differs from the pinned probe input")

    rel_path = TOOLS / "rhbm.synthetic.rel"
    rel = rel_path.read_bytes()
    if sha256(rel_path) != EXPECTED_REL_SHA256:
        raise SystemExit("Synthetic REL hash differs; regenerate and recheck relocations")
    section_table = struct.unpack_from(">I", rel, 0x10)[0]
    section_flags, section_size = struct.unpack_from(">II", rel, section_table + 8)
    section_file_offset = section_flags & ~1
    section_offset = 0xC690
    if section_offset >= section_size:
        raise SystemExit("Observed offset 0xC690 lies outside RSO section 1")

    metadata_path = TOOLS / "rhbm_c690_metadata.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if metadata["quality"] != {"unsupportedInstructionCount": 0, "invalidSsaFunctionCount": 0}:
        raise SystemExit(f"Translation quality gate failed: {metadata['quality']}")
    functions = {item["entryPoint"]: item for item in metadata["functions"]}
    if set(address for address in functions if 0x80690000 <= address < 0x80700000) != EXPECTED_FUNCTIONS:
        raise SystemExit("The six-function RSO translation closure differs from the validated probe")

    objects: list[dict[str, str | int | bool]] = []
    sources: list[dict[str, str | int | bool]] = []
    for address in sorted(EXPECTED_FUNCTIONS):
        item = functions[address]
        if item["interiorToOtherTranslation"]:
            raise SystemExit(f"Candidate 0x{address:08X} is interior to another translated flow")
        source = TOOLS / "rhbm_probe_generated/functions" / item["relativePath"]
        obj = TOOLS / "rhbm_probe_objects" / Path(item["relativePath"]).with_suffix(".o").name
        if sha256(source) != item["sha256"]:
            raise SystemExit(f"Generated source hash mismatch at 0x{address:08X}")
        object_bytes = obj.read_bytes()
        if len(object_bytes) < 20 or struct.unpack_from("<H", object_bytes)[0] != 0x8664:
            raise SystemExit(f"Object at 0x{address:08X} is not COFF AMD64")
        sources.append({
            "address": f"0x{address:08X}",
            "path": str(source.relative_to(ROOT)),
            "sha256": sha256(source),
            "source_bytes": source.stat().st_size,
        })
        objects.append({
            "address": f"0x{address:08X}",
            "path": str(obj.relative_to(ROOT)),
            "sha256": sha256(obj),
            "object_bytes": obj.stat().st_size,
            "machine": "COFF-x86-64/AMD64",
        })

    candidate = functions[0x806A4C70]
    if candidate["build"]["directCallDependencies"] != [0x806A4224]:
        raise SystemExit("Candidate direct-call closure changed")

    log = RUNTIME_LOG.read_text(encoding="utf-8", errors="replace")
    if sha256(RUNTIME_LOG) != RUNTIME_LOG_SHA256:
        raise SystemExit("Runtime diagnostic log hash changed")
    entry_pattern = re.compile(
        r"\[rso-request-entry\] lr=([0-9A-F]{8}) sp=([0-9A-F]{8}) "
        r"thread=([0-9A-F]{8}) out=([0-9A-F]{8}) arg=([0-9A-F]{8}) "
        r"inherited_callback=([0-9A-F]{8})"
    )
    entries = entry_pattern.findall(log)
    request_states = re.findall(
        r"\[rso-request\] before-dispatch sp=([0-9A-F]{8}) req=([0-9A-F]{8}) "
        r"state=([0-9A-F]{8}) callback=([0-9A-F]{8})",
        log,
    )
    if len(entries) != 5 or [item[0] for item in entries] != ["800CC8CC"] * 4 + ["800CC91C"]:
        raise SystemExit("Observed RSO request callsites/count differ from the bounded runtime trace")
    if any(item[1] != "8069A070" or item[2] != "8060B418" or item[5] != "0101C690" for item in entries):
        raise SystemExit("The RSO request entry stack/thread/callback sample changed")
    if len(request_states) != 5 or any(item[2] != "00000000" or item[3] != "0101C690" for item in request_states):
        raise SystemExit("The five request pre-dispatch states differ from the bounded trace")
    if "[fatal]" in log.lower() or "guest memory access was out of bounds" in log.lower():
        raise SystemExit("The bounded runtime trace now contains a fatal guest error")

    report = {
        "date": "2026-10-20",
        "revision": "RM3E01_00",
        "purpose": "Check whether the observed low-address callback offset 0xC690 is code inside the private RSO_rhbm module, without integrating it into the runtime.",
        "input": {
            "path": str(EXTRACTION.relative_to(ROOT)),
            "sha256": sha256(EXTRACTION),
            "section_1_size": f"0x{section_size:X}",
            "section_1_file_offset_in_synthetic_rel": f"0x{section_file_offset:X}",
            "observed_offset_in_section_1": f"0x{section_offset:X}",
            "synthetic_probe_load_address": "0x80690000",
            "section_1_probe_address": f"0x{0x80690000 + section_file_offset:08X}",
            "candidate_probe_address": f"0x{0x80690000 + section_file_offset + section_offset:08X}",
            "synthetic_rel_sha256": sha256(rel_path),
        },
        "translation": {
            "metadata_path": str(metadata_path.relative_to(ROOT)),
            "metadata_sha256": sha256(metadata_path),
            "quality": metadata["quality"],
            "function_count": len(metadata["functions"]),
            "rso_function_count": len(EXPECTED_FUNCTIONS),
            "candidate_entry_point": "0x806A4C70",
            "candidate_source_bytes": candidate["size"],
            "candidate_interior_to_other_translation": candidate["interiorToOtherTranslation"],
            "candidate_direct_call_dependencies": ["0x806A4224"],
            "sources": sources,
            "compiled_objects": objects,
        },
        "runtime_trace": {
            "build_result": "PASS",
            "executable_path": "recomp/build/RM3E01_00_runtime_link_probe/WiiCompiled.exe",
            "executable_sha256_for_original_trace": RUNTIME_EXE_SHA256,
            "executable_artifact_available": False,
            "executable_note": "A later diagnostic rebuild replaced this binary. The preserved trace log is still hash-verified; the current executable is not attributed to this older run.",
            "log_path": str(RUNTIME_LOG.relative_to(ROOT)),
            "log_sha256": sha256(RUNTIME_LOG),
            "harness_duration_seconds": 25,
            "process_alive_at_harness_stop": True,
            "rso_request_entries": len(entries),
            "callsite_lr_sequence": [f"0x{item[0]}" for item in entries],
            "stack_pointer": "0x8069A070",
            "guest_thread": "0x8060B418",
            "preexisting_callback_value": "0x0101C690",
            "request_state_before_dispatch": "0x00000000",
            "fatal_error": False,
            "callers": [
                "0x800CC8CC: four calls from FUN_800CC804's loop",
                "0x800CC91C: fifth call from FUN_800CC804",
            ],
            "input_strings_from_RM3E01_00_DOL": [
                "RSO/Rev2 Production/RSO_rhbm.rso",
                "rhbm/homeBtn_ENG.arc",
                "rhbm/SpeakerSe.arc",
                "rhbm/HomeButtonSe.arc",
                "rhbm/homeBtnIcon.tpl",
            ],
            "interpretation": "Ghidra's FUN_800CC804 decompilation and the runtime callsite/argument trace identify Wii Home Button resources. The callback value is already present in the reused stack slot at function entry; the request remains state 0 at the sampled dispatch. This trace does not establish the value's original writer or the RSO load base.",
            "last_progress": "The probe reached USB open plus three queued IOCTLV requests; no later IPC packet appeared before the harness stopped the still-running process. No game screen was rendered.",
        },
        "conclusion": "The offset is within section 1 and translates/compiles as part of a six-function probe with no unsupported instructions or invalid SSA. This does not prove that the game loads RSO_rhbm at a section base that makes 0x0101C690 valid, that the request field is an initialized callback, or that this candidate is a semantic function boundary. Nothing was integrated into WiiCompiled.exe.",
        "invariants": {
            "matching_dol_modified": False,
            "iso_modified": False,
            "decomp_baseline_modified": False,
            "runtime_behavior_modified": False,
            "generated_runtime_diagnostic_instrumentation": True,
            "private_research_extraction_only": True,
        },
        "next": "Trace the request object's +0x28 field from the start of the stack frame through state transitions, and establish the actual RSO section load base from the loader before using any candidate callback address.",
    }
    OUT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "accepted_as_runtime_fix": False,
        "probe_functions": len(EXPECTED_FUNCTIONS),
        "unsupported_instructions": metadata["quality"]["unsupportedInstructionCount"],
        "invalid_ssa_functions": metadata["quality"]["invalidSsaFunctionCount"],
        "compiled_amd64_objects": len(objects),
        "checkpoint": str(OUT.relative_to(ROOT)),
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
