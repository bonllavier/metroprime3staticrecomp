#!/usr/bin/env python3
"""Verify bounded boot evidence, including bytes read from the research disc copy."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PIN = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rel(path: Path) -> str:
    return path.resolve().relative_to(ROOT).as_posix()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--checkpoint", type=Path)
    parser.add_argument("--expect-dsp-boundary", action="store_true",
                        help="Require the explicit unsupported-DSP boundary; this is not a successful boot.")
    parser.add_argument("--expect-context-boundary", action="store_true",
                        help="Require the explicit host continuation boundary after DSP INIT IRQ.")
    parser.add_argument("--expect-texture-copy-boundary", action="store_true",
                        help="Verify the explicit unsupported raw texture-copy boundary, not a successful boot.")
    parser.add_argument("--expect-vertex-layout-boundary", action="store_true",
                        help="Verify the explicit 52/28-byte title vertex-layout mismatch, not a successful boot.")
    parser.add_argument("--expect-mmio-boundary", type=lambda value: int(value, 0),
                        help="Verify an explicitly blocked MMIO address; this is a failed runtime, not a successful boot.")
    parser.add_argument("--expect-missing-target", type=lambda value: int(value, 0))
    parser.add_argument("--expect-missing-jump", action="store_true")
    parser.add_argument("--expect-missing-module", default="rhbm", choices=("rhbm", "Tweaks", "DOL", "ScriptGUIScreenFrontEndUI", "ScriptWorldTeleporter"))
    parser.add_argument("--verify-iso", action="store_true")
    parser.add_argument("--verify-rhbm-init", action="store_true")
    parser.add_argument("--verify-tweaks-init", action="store_true")
    parser.add_argument("--verify-frontend-init", action="store_true")
    parser.add_argument("--verify-world-init", action="store_true")
    parser.add_argument("--verify-frontend-factory", action="store_true")
    parser.add_argument("--verify-world-factory", action="store_true")
    parser.add_argument("--verify-frontend-entity-callback", action="store_true")
    parser.add_argument("--verify-world-entity-callback", action="store_true")
    parser.add_argument("--verify-fifo-storage", action="store_true")
    parser.add_argument("--verify-cp-breakpoint", action="store_true")
    parser.add_argument("--verify-cp-frame-loop", action="store_true")
    parser.add_argument("--verify-xfb-submission", action="store_true")
    parser.add_argument("--verify-raw-textures", action="store_true")
    parser.add_argument("--verify-gpu-validation", action="store_true")
    parser.add_argument("--verify-efb-persistence", action="store_true")
    args = parser.parse_args()
    probe = json.loads(args.probe.read_text(encoding="utf-8-sig"))
    exe = ROOT / "recomp/build/RM3E01_00_runtime_link_probe/WiiCompiled.exe"
    log = Path(probe["log"])
    assert sha(exe) == probe["executable_sha256"], "Executable changed since probe"
    if "probe_script_sha256" in probe:
        assert sha(ROOT / "recomp/tools/probe_prime_runtime.ps1") == probe["probe_script_sha256"], "Probe script changed since run"
    assert sha(log) == probe["log_sha256"], "Log changed since probe"
    text = log.read_text(encoding="utf-8", errors="replace")
    if args.verify_gpu_validation:
        assert probe.get("gpu_validation_requested"), "Probe did not request GPU validation"
        assert "WebGPU validation and robustness enabled for runtime verification" in text
        assert "WebGPU error" not in text
    if args.verify_efb_persistence:
        assert "[prime-efb-persistence] hardware EFB survives host presentation boundaries" in text
    boundary = "Runtime error: Prime DSP microcode upload identified; a verified DSP backend is required before executing it."
    assert sum([args.expect_dsp_boundary, args.expect_context_boundary, args.expect_texture_copy_boundary, args.expect_vertex_layout_boundary, args.expect_mmio_boundary is not None, args.expect_missing_target is not None]) <= 1
    expected_failure = args.expect_dsp_boundary or args.expect_context_boundary or args.expect_texture_copy_boundary or args.expect_vertex_layout_boundary or args.expect_mmio_boundary is not None or args.expect_missing_target is not None
    if args.expect_vertex_layout_boundary:
        boundary = "Runtime error: Prime raw GX vertex layout disagrees with its verified packet size"
        assert probe["failure_logged"] and probe["process_exit_code"] == 1 and not probe["alive_at_deadline"]
        assert 'cached_stride=52 expected=208 supplied=112' in text
    if args.expect_texture_copy_boundary:
        boundary = "Runtime error: Prime raw FIFO texture-copy trigger requires a verified texture-copy adapter"
        assert probe["failure_logged"] and probe["process_exit_code"] == 1 and not probe["alive_at_deadline"]
        assert "[prime-front-gate] call=1 result=1" in text
        assert "address=0x804eb174 phase=return r3=0x0" in text
    if args.expect_missing_target is not None:
        boundary = (f"[runtime] InvokeIndirectJump: target 0x{args.expect_missing_target:x} is not a registered function."
                    if args.expect_missing_jump else
                    f"[runtime] InvokeIndirectCpu: target 0x{args.expect_missing_target:x} not translated (missing function)")
        assert probe["failure_logged"] and probe["process_exit_code"] == 1
    if args.expect_context_boundary:
        boundary = "Runtime error: Prime cross-thread OSLoadContext requires host continuation transfer"
    if args.expect_mmio_boundary is not None:
        errors = re.findall(r"^Runtime error: .*", text, re.M)
        assert len(errors) == 1 and f"at 0x{args.expect_mmio_boundary:08X} " in errors[0]
        assert "MMIO write blocked (non-GPU)" in errors[0] or "MMIO read blocked (non-GPU)" in errors[0]
        assert probe["failure_logged"] and probe["process_exit_code"] == 1
        boundary = errors[0]
    checked_text = text
    if expected_failure:
        assert text.count(boundary) == 1, "Expected exactly one explicit DSP boundary"
        checked_text = text.replace(boundary, "")
    else:
        assert not probe["failure_logged"] and probe["process_exit_code"] in (0, -1), "Unexpected runtime termination"
    assert not re.search(r"InvokeIndirectCpu: target .*not translated|InvokeIndirectJump: target .*not a registered", checked_text)
    assert not re.search(r"WebGPU error|Unhandled C\+\+ exception:|std::terminate\(\) called|Runtime error:|memory error at|(?<!no )unmapped guest touch|FATAL unmapped guest address|\[fatal\]", checked_text, re.I), "Unexpected guest failure in runtime log"
    fifo_draws = re.findall(r"\[raw-fifo-draw-submitted\] bytes=(\d+) vertices=(\d+)", text)
    if args.verify_fifo_storage:
        assert ("6915", "288") in fifo_draws, "First full vertex packet was not submitted"
        assert "Skipping indexed XF load with invalid source array" not in text
        assert not re.search(r"prime-pi-fifo-reset\] discarded_bytes=[1-9]", text)
    cp_interrupts = re.findall(r"\[prime-cp-breakpoint-isr\] handler=0x([0-9a-f]+) callback=0x([0-9a-f]+) frame_state=([0-9a-f]+)", text)
    if args.verify_cp_breakpoint:
        assert any(handler == "804d009c" and callback == "803e9820" and state == "1"
                   for handler, callback, state in cp_interrupts), "Original CP ISR/callback did not return in state 1"
    if args.verify_cp_frame_loop:
        assert args.verify_cp_breakpoint and args.verify_fifo_storage
        assert len(cp_interrupts) >= 16 and text.count("[prime-cp-fifo-resume]") >= 16
        assert "[prime-pi-fifo-reset]" not in text, "The frame loop still aborted work"
    xfb_copies = re.findall(r"\[prime-raw-xfb-copy\] destination=0x([0-9a-f]+) trigger=0x([0-9a-f]+) width=(\d+) height=(\d+) pitch=(\d+) clear=([01])", text)
    xfb_selections = re.findall(r"\[prime-vi-xfb-selected\] framebuffer=0x([0-9a-f]+) retrace=(\d+)", text)
    if args.verify_xfb_submission:
        assert ("9008c800", "4803", "640", "448", "640", "1") in xfb_copies
        assert any(address == "9008c800" for address, _ in xfb_selections)
        assert "Enabled bounded asynchronous presentation worker" in text
    raw_textures = re.findall(r"\[prime-raw-texture\] slot=(\d+) address=0x([0-9a-f]+) width=(\d+) height=(\d+) format=(\d+) bytes=(\d+) mipmap=([01])", text)
    raw_tluts = re.findall(r"\[prime-raw-tlut-load\] source=0x([0-9a-f]+) tmem=0x([0-9a-f]+) bytes=(\d+)", text)
    if args.verify_raw_textures:
        assert any(row[2:6] == ("640", "480", "14", "153600") for row in raw_textures)
        assert raw_tluts and all(0 < int(row[2]) <= 65504 and int(row[2]) % 32 == 0 for row in raw_tluts)
        assert "stopping FIFO decode at truncated draw" not in text
    for capture in probe.get("captures", []):
        assert sha(Path(capture["path"])) == capture["sha256"], "Window capture changed"
    assert "IOS IPC open /dev/di -> fd 7" in text
    assert "inquiry -> retail drive identification" in text
    assert "HCI command opcode=0C03 parameters=0" in text
    assert "HCI event=0E bytes=6" in text
    assert "HCI event=0E bytes=13" in text
    assert len(re.findall(r"pending endpoint-IN .*endpoint=81", text)) >= 2, "HCI event endpoint was not rearmed"
    assert "endpoint=82" in text
    assert "HCI command opcode=1005 parameters=0" in text
    wire_events = []
    for count, packet in re.findall(r"HCI event=0E bytes=(\d+) ipc=[0-9a-f]+ data=([0-9a-f]+)", text, re.I):
        data = bytes.fromhex(packet)
        assert len(data) == int(count) and data[0] == 0x0E and data[1] == len(data) - 2
        assert data[2] == 1 and data[5] == 0
        opcode = int.from_bytes(data[3:5], "little")
        if opcode == 0x0C03:
            assert data == bytes.fromhex("0e0401030c00")
        if opcode == 0x1005:
            assert int.from_bytes(data[6:8], "little") == 339
            assert 0 < int.from_bytes(data[9:11], "little") <= 10
        wire_events.append({"opcode": f"0x{opcode:04X}", "packet": packet})
    assert wire_events, "No wire-level HCI event evidence"
    reads = []
    iso_reads = re.findall(r"\[prime-iso-read\] offset=0x([0-9a-f]+) length=0x([0-9a-f]+) buffer=0x([0-9a-f]+) fnv1a64=0x([0-9a-f]+) hashes_validated=1", text)
    if args.verify_iso:
        assert "[prime-iso-open] id=RM3E01 disc=0 revision=0 readonly=1 validate_hashes=1" in text
        pinned_probe = json.loads((ROOT / "recomp/checkpoints/20261020_worlds_alignment_iso_proof.json").read_text())
        for filename, fingerprint in pinned_probe["source_hashes"].items():
            assert sha(ROOT / filename) == fingerprint, "ISO range verifier/toolchain changed"
    extraction = ROOT / "recomp/.tools/disc-extraction-rm3e01/files"
    probe_environment = dict(os.environ)
    probe_environment["PATH"] = str(ROOT / "recomp/.tools/portable-tools/llvm-mingw/bin") + os.pathsep + probe_environment.get("PATH", "")
    for match in re.finditer(r"\[prime-partition-read\] path=(.*?) offset=0x([0-9a-f]+) length=0x([0-9a-f]+) buffer=0x([0-9a-f]+) fnv1a64=0x([0-9a-f]+)", text, re.I):
        game_path, offset, size, destination, expected = match.groups()
        path = (extraction / game_path.lstrip("/")).resolve()
        path.relative_to(extraction.resolve())
        offset, size = int(offset, 16), int(size, 16)
        with path.open("rb") as stream:
            stream.seek(offset)
            file_data = stream.read(size)
        partition_offset = None
        if args.verify_iso:
            assert len(iso_reads) > len(reads), "Missing direct ISO read evidence"
            raw_offset, raw_size, raw_buffer, raw_hash = iso_reads[len(reads)]
            assert (int(raw_size, 16), int(raw_buffer, 16), int(raw_hash, 16)) == (size, int(destination, 16), int(expected, 16))
            partition_offset = int(raw_offset, 16)
            private_output = ROOT / f"recomp/.tools/partition_range_checks/{partition_offset:016x}_{size:08x}.bin"
            completed = subprocess.run([str(ROOT / "recomp/.tools/nod_partition_range_probe.exe"),
                str(ROOT / "Metroid Prime 3 - Corruption (USA).iso"), hex(partition_offset), hex(size),
                str(private_output)], check=True, capture_output=True, text=True, env=probe_environment)
            assert "hashes_validated=true" in completed.stdout
            data = private_output.read_bytes()
            assert len(data) == size and data[:len(file_data)] == file_data
            # Native DVD alignment may include real partition bytes after EOF.
            assert size - len(file_data) <= 31, "Unexpected read crossing an FST file extent"
            readback = f"[prime-partition-readback] buffer=0x{int(destination, 16):x} length=0x{size:x} fnv1a64=0x{int(expected,16):x}"
            assert readback in text, "Guest RAM DMA readback was not verified"
        else:
            data = file_data
            assert len(data) == size, "Short research disc read"
        fingerprint = 14695981039346656037
        for byte in data:
            fingerprint = ((fingerprint ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        assert fingerprint == int(expected, 16), "Runtime bytes differ from research file"
        reads.append({"path": game_path, "offset": offset, "size": size,
                      "partition_offset": partition_offset, "original_eof_alignment_bytes": size - len(file_data),
                      "guest_dma_readback_verified": args.verify_iso,
                      "destination": "0x" + destination, "read_sha256": hashlib.sha256(data).hexdigest(),
                      "file_sha256": sha(path), "fnv1a64": f"{fingerprint:016x}"})
    assert reads, "No verified resource read"
    if args.verify_iso:
        assert len(iso_reads) == len(reads)
        readbacks = re.findall(r"\[prime-partition-readback\] buffer=0x([0-9a-f]+) length=0x([0-9a-f]+) fnv1a64=0x([0-9a-f]+)", text)
        assert len(readbacks) == len(reads)
        for row, (buffer, length, fingerprint) in zip(reads, readbacks):
            assert (int(row["destination"], 16), row["size"], int(row["fnv1a64"],16)) == tuple(int(value,16) for value in (buffer,length,fingerprint))
    rso_boundary = None
    nand_cache = None
    if args.verify_rhbm_init:
        cached = ROOT / "recomp/build/RM3E01_00_runtime_link_probe/UserData/NAND/tmp/7F826CCE.pak"
        original = extraction / "MiscData.pak"
        assert cached.read_bytes() == original.read_bytes(), "NAND cache differs from original MiscData.pak"
        nand_cache = {"path": rel(cached), "size": cached.stat().st_size, "sha256": sha(cached),
                      "original_path": rel(original), "exact_original_bytes": True}
        # A valid early MiscData cache alone missed a truncated InGameNAND
        # cache. Verify every currently present, identified Prime cache too.
        identified = {
            "7F826CCE.pak": "MiscData.pak", "26226D27.pak": "InGameNAND.pak",
            "7F7B52B0.pak": "GuiNAND.pak", "7B30C91B.pak": "SamusGun.pak",
            "C013FDAF.pak": "SamGunFx.pak", "F9897189.pak": "Worlds.pak",
        }
        nand_cache["other_present_caches"] = []
        for cache_name, original_name in identified.items():
            candidate = cached.parent / cache_name
            if not candidate.exists() or candidate == cached:
                continue
            source = extraction / original_name
            assert candidate.read_bytes() == source.read_bytes(), f"NAND cache differs from original {original_name}"
            nand_cache["other_present_caches"].append({
                "path": rel(candidate), "original_path": rel(source),
                "size": candidate.stat().st_size, "sha256": sha(candidate),
                "exact_original_bytes": True,
            })
    if args.expect_missing_target is not None or args.verify_rhbm_init or args.verify_tweaks_init or args.verify_frontend_init or args.verify_world_init:
        sys.path.insert(0, str(ROOT / "tools"))
        from inspect_startup_rso import inspect_bytes
        module_path = extraction / "RSO/Rev2 Production/RSO_rhbm.rso"
        module = inspect_bytes(module_path.read_bytes())
        prolog = module["entry_points"]["prolog"]
        section = module["sections"][prolog["section"]]
        module_reads = [row for row in reads if row["path"].endswith("/RSO_rhbm.rso") and row["offset"] == 0]
        assert len(module_reads) == 1 and module_reads[0]["size"] >= module["size"]
        loaded = (int(module_reads[0]["destination"], 16) & 0x3FFFFFFF) | 0x80000000
        expected_target = loaded + section["file_offset"] + prolog["offset"]
        rhbm_missing = args.expect_missing_target if args.expect_missing_module == "rhbm" else None
        missing_offset = rhbm_missing - loaded - section["file_offset"] if rhbm_missing is not None else None
        assert missing_offset is None or 0 <= missing_offset < section["size"], "Missing target is outside rhbm code"
        export_names = [row["name"] for row in module["exports"]
                        if row["section"] == prolog["section"] and row["offset"] == missing_offset]
        bound = args.verify_rhbm_init or expected_target != rhbm_missing
        execution = []
        if bound:
            assert f"[prime-rso-bind] module=rhbm base=0x{loaded:x} prolog=0x{expected_target:x}" in text
            assert "relocations=4238" in text and "section_hashes_verified=1" in text
            manifest_rso = json.loads((ROOT / "recomp/.tools/rso-runtime/rhbm/module.json").read_text())
            nominal_text = manifest_rso["sections"][prolog["section"]]["nominal"]
            entries = re.findall(r"\[prime-rso-enter\] target=0x([0-9a-f]+) nominal=0x([0-9a-f]+) sp=0x([0-9a-f]+) lr=0x([0-9a-f]+)", text)
            returns = re.findall(r"\[prime-rso-return\] target=0x([0-9a-f]+) sp=0x([0-9a-f]+) lr=0x([0-9a-f]+) r3=0x([0-9a-f]+) dispatched=1", text)
            required = manifest_rso["constructor_entries"] + [nominal_text + prolog["offset"]]
            if args.verify_rhbm_init:
                required += [nominal_text + row["offset"] for row in module["exports"] if row["name"] in ("HBMSize", "HBMInit")]
            for nominal in required:
                target = loaded + section["file_offset"] + nominal - nominal_text
                matching = [row for row in entries if int(row[0], 16) == target and int(row[1], 16) == nominal]
                assert len(matching) == 1, "Expected one traced constructor/prolog entry"
                entry = matching[0]
                returned = [row for row in returns if row[:3] == (entry[0], entry[2], entry[3])]
                assert len(returned) == 1, "Constructor/prolog did not restore SP/LR and return"
                execution.append({"nominal": hex(nominal), "runtime": hex(target), "sp": entry[2], "lr": entry[3]})
        else:
            assert "[runtime] Caller LR = 0x8019f8cc" in text
        rso_boundary = {"module": "RSO_rhbm.rso", "role": "Wii HOME infrastructure, not Metroid title screen",
            "original_sha256": module["sha256"], "cached_load_base": hex(loaded),
            "prolog_section": prolog["section"], "prolog_section_offset": prolog["offset"],
            "section_file_offset": section["file_offset"], "prolog_target": hex(expected_target),
            "translation_registered": bound, "missing_section_offset": hex(missing_offset) if missing_offset is not None else None,
            "missing_export_names": export_names, "verified_constructor_and_prolog_returns": execution}
        if args.expect_missing_module == "Tweaks" or args.verify_tweaks_init:
            pending_name = "RSO_Tweaks.rso"
            pending = inspect_bytes((extraction / "RSO/Rev2 Production" / pending_name).read_bytes())
            pending_reads = [row for row in reads if row["path"].endswith("/" + pending_name) and row["offset"] == 0]
            assert len(pending_reads) == 1 and pending_reads[0]["size"] >= pending["size"]
            pending_base = (int(pending_reads[0]["destination"], 16) & 0x3FFFFFFF) | 0x80000000
            entry = pending["entry_points"]["prolog"]
            pending_target = pending_base + pending["sections"][entry["section"]]["file_offset"] + entry["offset"]
            pending_missing_offset = (args.expect_missing_target - pending_base - pending["sections"][entry["section"]]["file_offset"]
                                      if args.expect_missing_module == "Tweaks" else None)
            assert pending_missing_offset is None or 0 <= pending_missing_offset < pending["sections"][entry["section"]]["size"]
            prolog_returned = args.verify_tweaks_init or pending_target != args.expect_missing_target
            pending_execution = []
            if prolog_returned:
                assert f"[prime-rso-bind] module=Tweaks base=0x{pending_base:x} prolog=0x{pending_target:x}" in text
                assert "relocations=5273" in text and "section_hashes_verified=1" in text
                meta = json.loads((ROOT / "recomp/.tools/rso-runtime/Tweaks/module.json").read_text())
                nominal_text = meta["sections"][entry["section"]]["nominal"]
                for nominal in meta["constructor_entries"] + [nominal_text + entry["offset"]]:
                    runtime = pending_target + nominal - nominal_text - entry["offset"]
                    matching = [row for row in entries if int(row[0], 16) == runtime and int(row[1], 16) == nominal]
                    assert len(matching) == 1
                    traced = matching[0]
                    assert len([row for row in returns if row[:3] == (traced[0], traced[2], traced[3])]) == 1
                    pending_execution.append({"nominal": hex(nominal), "runtime": hex(runtime), "sp": traced[2], "lr": traced[3]})
            else:
                assert "[runtime] Caller LR = 0x8019f8cc" in text
            rso_boundary["next_untranslated_module" if args.expect_missing_module == "Tweaks" else "tweaks_execution"] = {"name": pending_name, "sha256": pending["sha256"],
                "load_base": hex(pending_base), "prolog": hex(pending_target), "prolog_returned": prolog_returned,
                "missing_text_offset": hex(pending_missing_offset) if pending_missing_offset is not None else None,
                "verified_constructor_and_prolog_returns": pending_execution}
        if args.expect_missing_module == "DOL":
            original = (ROOT / "decomp/orig/RM3E01_00/sys/main.dol").read_bytes()
            word = lambda offset: int.from_bytes(original[offset:offset + 4], "big")
            regions = [(word(i * 4), word(0x48 + i * 4), word(0x90 + i * 4)) for i in range(7)]
            matches = [(offset, base, size) for offset, base, size in regions if base <= args.expect_missing_target < base + size]
            assert len(matches) == 1, "Missing DOL jump is outside original executable sections"
            offset, base, _ = matches[0]
            raw_instruction = original[offset + args.expect_missing_target - base:offset + args.expect_missing_target - base + 4]
            rso_boundary["next_untranslated_dol_jump"] = {"target": hex(args.expect_missing_target), "original_instruction": raw_instruction.hex()}
        if args.expect_missing_module == "ScriptGUIScreenFrontEndUI" or args.verify_frontend_init:
            frontend_name = "RSO_ScriptGUIScreenFrontEndUI.rso"
            frontend = inspect_bytes((extraction / "RSO/Rev2 Production" / frontend_name).read_bytes())
            module_reads = [row for row in reads if row["path"].endswith("/" + frontend_name) and row["offset"] == 0]
            assert len(module_reads) == 1 and module_reads[0]["size"] >= frontend["size"]
            frontend_base = (int(module_reads[0]["destination"], 16) & 0x3FFFFFFF) | 0x80000000
            prolog = frontend["entry_points"]["prolog"]
            frontend_text = frontend_base + frontend["sections"][prolog["section"]]["file_offset"]
            frontend_prolog = frontend_text + prolog["offset"]
            missing_offset = (args.expect_missing_target - frontend_text
                              if args.expect_missing_module == "ScriptGUIScreenFrontEndUI" else None)
            assert missing_offset is None or 0 <= missing_offset < frontend["sections"][prolog["section"]]["size"]
            returned = args.verify_frontend_init or args.expect_missing_target != frontend_prolog
            frontend_execution = []
            if returned:
                assert f"[prime-rso-bind] module=ScriptGUIScreenFrontEndUI base=0x{frontend_base:x} prolog=0x{frontend_prolog:x}" in text
                assert "relocations=3421" in text and "section_hashes_verified=1" in text
                meta = json.loads((ROOT / "recomp/.tools/rso-runtime/ScriptGUIScreenFrontEndUI/module.json").read_text())
                nominal_text = meta["sections"][prolog["section"]]["nominal"]
                required = meta["constructor_entries"] + [nominal_text + prolog["offset"]]
                if args.verify_frontend_factory:
                    required.append(nominal_text + 0x1675C)
                if args.verify_frontend_entity_callback:
                    required.append(nominal_text + 0x156DC)
                for nominal in required:
                    target = frontend_text + nominal - nominal_text
                    matching = [row for row in entries if int(row[0], 16) == target and int(row[1], 16) == nominal]
                    assert len(matching) == 1
                    traced = matching[0]
                    assert len([row for row in returns if row[:3] == (traced[0], traced[2], traced[3])]) == 1
                    result = [row for row in returns if row[:3] == (traced[0], traced[2], traced[3])][0]
                    frontend_execution.append({"nominal": hex(nominal), "runtime": hex(target), "sp": traced[2], "lr": traced[3], "return_r3": result[3]})
            else:
                assert "[runtime] Caller LR = 0x8019f8cc" in text
            rso_boundary["frontend_execution"] = {"module": frontend_name, "sha256": frontend["sha256"],
                "load_base": hex(frontend_base), "prolog": hex(frontend_prolog), "prolog_returned": returned,
                "missing_text_offset": hex(missing_offset) if missing_offset is not None else None,
                "verified_constructor_and_prolog_returns": frontend_execution}
        if args.expect_missing_module == "ScriptWorldTeleporter" or args.verify_world_init:
            name = "RSO_ScriptWorldTeleporter.rso"
            pending = inspect_bytes((extraction / "RSO/Rev2 Production" / name).read_bytes())
            loaded_reads = [row for row in reads if row["path"].endswith("/" + name) and row["offset"] == 0]
            assert len(loaded_reads) == 1 and loaded_reads[0]["size"] >= pending["size"]
            load_base = (int(loaded_reads[0]["destination"], 16) & 0x3FFFFFFF) | 0x80000000
            entry = pending["entry_points"]["prolog"]
            target = load_base + pending["sections"][entry["section"]]["file_offset"] + entry["offset"]
            world_missing = (args.expect_missing_target - load_base - pending["sections"][entry["section"]]["file_offset"]
                             if args.expect_missing_module == "ScriptWorldTeleporter" else None)
            assert world_missing is None or 0 <= world_missing < pending["sections"][entry["section"]]["size"]
            world_returned = args.verify_world_init or target != args.expect_missing_target
            world_execution = []
            if world_returned:
                assert f"[prime-rso-bind] module=ScriptWorldTeleporter base=0x{load_base:x} prolog=0x{target:x}" in text
                assert "relocations=668" in text and "section_hashes_verified=1" in text
                meta = json.loads((ROOT / "recomp/.tools/rso-runtime/ScriptWorldTeleporter/module.json").read_text())
                nominal_text = meta["sections"][entry["section"]]["nominal"]
                required_world = meta["constructor_entries"] + [nominal_text + entry["offset"]]
                if args.verify_world_factory:
                    required_world.append(nominal_text + 0x2988)
                if args.verify_world_entity_callback:
                    required_world.append(nominal_text + 0xB0C)
                for nominal in required_world:
                    runtime = target + nominal - nominal_text - entry["offset"]
                    matching = [row for row in entries if int(row[0], 16) == runtime and int(row[1], 16) == nominal]
                    assert len(matching) == 1
                    traced = matching[0]
                    assert len([row for row in returns if row[:3] == (traced[0], traced[2], traced[3])]) == 1
                    world_execution.append({"nominal": hex(nominal), "runtime": hex(runtime), "sp": traced[2], "lr": traced[3]})
            else:
                assert target == args.expect_missing_target and "[runtime] Caller LR = 0x8019f8cc" in text
            assert "[prime-rso-retire] module=Tweaks reason=original_epilog_return active_frames=0" in text
            rso_boundary["next_untranslated_module" if args.expect_missing_module == "ScriptWorldTeleporter" else "world_execution"] = {
                "name": name, "sha256": pending["sha256"], "load_base": hex(load_base), "prolog": hex(target),
                "missing_text_offset": hex(world_missing) if world_missing is not None else None,
                "prolog_returned": world_returned, "verified_constructor_and_prolog_returns": world_execution}
    raw_dol = ROOT / "decomp/orig/RM3E01_00/sys/main.dol"
    assert sha(raw_dol) == PIN
    dsp_upload = None
    if expected_failure:
        match = re.search(r"\[prime-dsp-ucode\] source=0x([0-9a-f]+) length=0x([0-9a-f]+) imem=0x([0-9a-f]+) dmem=0x([0-9a-f]+) entry=0x([0-9a-f]+) hash=0x([0-9a-f]+)", text)
        assert match, "Missing DSP upload evidence"
        address, length, imem, dmem, entry, expected_hash = [int(value, 16) for value in match.groups()]
        dol_bytes = raw_dol.read_bytes()
        # DOL header: 7 text + 11 data sections, all big endian.
        word = lambda offset: int.from_bytes(dol_bytes[offset:offset + 4], "big")
        regions = [(word(i * 4), word(0x48 + i * 4), word(0x90 + i * 4)) for i in range(18)]
        matches = [(offset, base, size) for offset, base, size in regions
                   if base <= address and address + length <= base + size]
        assert len(matches) == 1, "DSP range is not wholly backed by one original DOL section"
        offset, base, _ = matches[0]
        payload = dol_bytes[offset + address - base:offset + address - base + length]
        fingerprint = 0
        for byte in payload:
            fingerprint ^= byte
            fingerprint = ((fingerprint << 3) | (fingerprint >> 29)) & 0xFFFFFFFF
        assert fingerprint == expected_hash == 0x347112BA
        assert (address, length, imem, dmem, entry) == (0x805A1C00, 0x2000, 0, 0, 0x10)
        dsp_upload = {"source": hex(address), "length": length, "imem": imem, "dmem_length": dmem,
                      "entry": entry, "hash_ector": f"{fingerprint:08x}",
                      "original_dol_payload_sha256": hashlib.sha256(payload).hexdigest(),
                      "backend_started": not args.expect_dsp_boundary,
                      "command_list_execution_verified": False}
        if not args.expect_dsp_boundary:
            assert "[prime-dsp-ax-start] crc=0x347112ba raw_mailbox=1 mkw_task_globals=0" in text
            assert "[prime-dsp-ax-reply] value=0xdcd10000" in text
            assert "[prime-dsp-irq] handler=0x804c5514" in text
            if args.expect_context_boundary:
                assert "[prime-context-transfer-required] target=0x8098da28 pc=0x8042fec4" in text
    built_dol = ROOT / "decomp/build/RM3E01_00/main.dol"
    assert hashlib.sha1(built_dol.read_bytes()).hexdigest() == "3e81f9b09dfb1e7c02e22b9c6c48a4a1390224b2"
    iso = ROOT / "Metroid Prime 3 - Corruption (USA).iso"
    iso_state = iso.stat()
    assert iso_state.st_size == 4699979776
    assert iso_state.st_file_attributes & stat.FILE_ATTRIBUTE_READONLY
    manifest = ROOT / "recomp/generated/RM3E01_00_conservative/runtime_hooks/manifest.json"
    overlays = json.loads(manifest.read_text(encoding="utf-8"))
    for item in overlays["overlays"]:
        filename = "func_" + item["address"][2:] + ".cpp"
        base = ROOT / "recomp/generated/RM3E01_00_conservative"
        assert sha(base / "functions" / filename) == item["raw_sha256"]
        assert sha(base / "runtime_hooks/functions" / filename) == item["overlay_sha256"]
    reference_manifest = ROOT / "recomp/checkpoints/20261020_prime_runtime_reference_provenance.json"
    references = json.loads(reference_manifest.read_text(encoding="utf-8"))
    for reference in references["files"]:
        assert sha(ROOT / reference["file"]) == reference["sha256"]
    sources = [
        "recomp/tools/prepare_prime_runtime_hooks.py", "recomp/tools/build_prime_runtime.ps1",
        "recomp/tools/probe_prime_runtime.ps1", "recomp/tools/verify_prime_runtime_boot_probe.py",
        "recomp/third_party/WiiCompiled/runtime/src/memory.cpp",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_ai_registers.h",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_dsp_boot_registers.h",
        "recomp/tools/prime_ai_registers_tests.cpp",
        "recomp/tools/prime_dsp_boot_registers_tests.cpp",
        "recomp/tools/prime_host_continuations_tests.cpp",
        "recomp/tools/prime_rso_relocations_tests.cpp",
        "recomp/tools/prepare_prime_rso_translation.py",
        "recomp/tools/emit_prime_rso_registry.py",
        "recomp/tools/RsoRuntimeTranslator/Program.cs",
        "recomp/tools/RsoRuntimeTranslator/RelocatedInstructions.cs",
        "recomp/tools/RsoRuntimeTranslator.Tests/RelocationControlFlowTests.cs",
        "recomp/third_party/WiiCompiled/translator/src/Translator.Core/CodeGen/CxxLinearCodeGenerator.EmitInstruction.cs",
        "recomp/tools/RsoRuntimeTranslator/DolFunctionRepair.cs",
        "recomp/third_party/WiiCompiled/translator/src/Translator.Core/Disassembly/JumpTableDetector.cs",
        "recomp/third_party/WiiCompiled/runtime/src/prime_rso_runtime.cpp",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_rso_runtime.h",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_rso_relocations.h",
        "recomp/third_party/WiiCompiled/runtime/include/guest_abi/dispatch.h",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_host_continuations.h",
        "recomp/third_party/WiiCompiled/runtime/src/hle/os/os_context.cpp",
        "recomp/third_party/WiiCompiled/runtime/src/main.cpp",
        "recomp/third_party/WiiCompiled/runtime/src/hle/audio/ax_dsp.h",
        "recomp/third_party/WiiCompiled/runtime/src/hle/audio/ax_mix.cpp",
        "recomp/third_party/WiiCompiled/runtime/src/hle/audio/ax_internal.h",
        "recomp/third_party/WiiCompiled/runtime/include/runtime_config.h",
        "recomp/third_party/WiiCompiled/runtime/cmake/PublicProducts.cmake",
        "recomp/third_party/WiiCompiled/runtime/src/hle/gx/gx_fifo.cpp",
        "recomp/third_party/WiiCompiled/runtime/src/hle/gx/gx_internal.h",
        "recomp/third_party/WiiCompiled/runtime/include/hle/gx_fifo_storage.h",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_fifo_burst.h",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_keyboard_mouse.h",
        "recomp/third_party/WiiCompiled/runtime/src/hle/input/kpad.cpp",
        "recomp/third_party/WiiCompiled/runtime/include/aurora_events.h",
        "recomp/tools/verify_prime_keyboard_input.py",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_video_trace.h",
        "recomp/tools/verify_prime_video_trace.py",
        "recomp/tools/verify_prime_movie_activation.py",
        "recomp/tools/verify_prime_thp_pixels.py",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_video_capture.h",
        "recomp/tools/verify_prime_rso_context_import.py",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_cp_fifo.h",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_cpu_fifo_capture.h",
        "recomp/third_party/WiiCompiled/runtime/tests/prime_cpu_fifo_capture_tests.cpp",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_mii_trace.h",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_nand_trace.h",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_resource_trace.h",
        "recomp/tools/prepare_prime_mii_profile.py",
        "recomp/tools/test_prime_mii_profile.py",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_raw_bp.h",
        "recomp/third_party/WiiCompiled/runtime/tests/prime_raw_bp_tests.cpp",
        "recomp/third_party/WiiCompiled/runtime/tests/prime_raw_texture_snapshot_tests.cpp",
        "recomp/third_party/WiiCompiled/aurora-main/lib/gx/raw_texture_snapshot_cache.hpp",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_locked_cache_dma.h",
        "recomp/third_party/WiiCompiled/runtime/src/ppc_helpers.cpp",
        "recomp/third_party/WiiCompiled/runtime/src/hle/gx/gx_copy.cpp",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_tlut_memory.h",
        "recomp/third_party/WiiCompiled/runtime/include/hle/prime_xfb_ready.h",
        "recomp/third_party/WiiCompiled/runtime/src/hle/gx/gx_dl.cpp",
        "recomp/third_party/WiiCompiled/runtime/src/hle/gx/gx_stream_common.h",
        "recomp/third_party/WiiCompiled/aurora-main/lib/dolphin/gx/GXTexture.cpp",
        "recomp/third_party/WiiCompiled/runtime/src/settings_overlay.cpp",
        "recomp/third_party/WiiCompiled/runtime/src/hle/os/os_alarm.cpp",
        "recomp/third_party/WiiCompiled/aurora-main/lib/dolphin/gx/GXFrameBuffer.cpp",
        "recomp/third_party/WiiCompiled/aurora-main/lib/gx/command_processor.cpp",
        "recomp/third_party/WiiCompiled/aurora-main/lib/aurora.cpp",
        "recomp/third_party/WiiCompiled/aurora-main/include/aurora/aurora.h",
        "recomp/third_party/WiiCompiled/aurora-main/lib/gfx/common.cpp",
        "recomp/third_party/WiiCompiled/aurora-main/lib/gfx/common.hpp",
        "recomp/third_party/WiiCompiled/aurora-main/lib/gx/pipeline.cpp",
        "recomp/third_party/WiiCompiled/aurora-main/lib/gx/gx.cpp",
        "recomp/tools/verify_prime_font_raster.py",
        "recomp/tools/verify_prime_gpu_font_input.py",
        "recomp/third_party/WiiCompiled/aurora-main/lib/gx/shader.cpp",
        "recomp/third_party/WiiCompiled/aurora-main/lib/gx/shader_info.cpp",
        "recomp/third_party/WiiCompiled/aurora-main/lib/webgpu/gpu.cpp",
        "recomp/third_party/WiiCompiled/runtime/tests/gx_fifo_storage_tests.cpp",
        "recomp/third_party/WiiCompiled/aurora-main/lib/dolphin/gx/GXVert.cpp",
        "recomp/third_party/WiiCompiled/aurora-main/lib/gx/fifo.hpp",
        "recomp/tools/nod_partition_range_probe.cpp",
        "recomp/tools/test_prime_runtime_contracts.ps1",
        "recomp/third_party/WiiCompiled/runtime/tests/runtime_config_tests.cpp",
        "recomp/third_party/WiiCompiled/runtime/src/guest_flat_memory.cpp",
        "recomp/third_party/WiiCompiled/runtime/src/hle/storage/nand_isfs.cpp",
        "recomp/third_party/WiiCompiled/runtime/include/hle/nand_directory_usage.h",
        "recomp/third_party/WiiCompiled/runtime/tests/nand_directory_usage_tests.cpp",
        "recomp/third_party/WiiCompiled/runtime/src/hle/storage/dvd.cpp",
        "recomp/third_party/WiiCompiled/runtime/src/hle/os/os_interrupt.cpp",
        "recomp/third_party/WiiCompiled/runtime/src/hle/os/os_internal.h",
        "recomp/third_party/WiiCompiled/runtime/src/hle/vi.cpp",
        "recomp/third_party/WiiCompiled/translator/src/Translator.Core/Build/TranslatedBuildShardEmitter.cs",
        "recomp/third_party/WiiCompiled/translator/src/Translator.Core/RuntimeNativeIndex.cs",
        "recomp/third_party/WiiCompiled/translator/src/Translator.Cli/Program.cs",
    ]
    report = {"revision": "RM3E01_00", "game_screen_verified": False,
        "outcome": "expected_vertex_layout_boundary" if args.expect_vertex_layout_boundary else "expected_texture_copy_boundary" if args.expect_texture_copy_boundary else "expected_missing_rso_translation" if args.expect_missing_target is not None else ("expected_mmio_boundary" if args.expect_mmio_boundary is not None else (
            "expected_context_transfer_boundary" if args.expect_context_boundary else (
            "expected_unsupported_dsp_boundary" if args.expect_dsp_boundary else "bounded_boot_progress"))),
        "missing_guest_target": hex(args.expect_missing_target) if args.expect_missing_target is not None else None,
        "rso_boundary": rso_boundary,
        "verified_nand_cache": nand_cache,
        "iso_partition_reads_verified": args.verify_iso,
        "nod_provenance": {"commit": "6a54b418e541ce5eeb364399116afd6e47b817bb",
            "library_sha256": sha(ROOT / "recomp/.tools/nod-source/target/x86_64-pc-windows-gnu/release/libnod.a"),
            "header_sha256": sha(ROOT / "recomp/.tools/nod-source/nod-ffi/include/nod.h")},
        "blocked_mmio": hex(args.expect_mmio_boundary) if args.expect_mmio_boundary is not None else None,
        "host_thread_starts": re.findall(r"\[prime-host-thread-start\] context=0x([0-9a-f]+) entry=0x([0-9a-f]+) exit=0x([0-9a-f]+) sp=0x([0-9a-f]+)", text),
        "host_thread_switches": re.findall(r"\[prime-host-thread-switch\] from=0x([0-9a-f]+) to=0x([0-9a-f]+)", text),
        "dsp_upload": dsp_upload,
        "build": {"exe_sha256": sha(exe), "dol_sha256": PIN},
        "runtime": {**probe, "log": rel(log), "probe": rel(args.probe)},
        "verified_resource_reads": reads, "runtime_overlay_manifest": {"path": rel(manifest), "sha256": sha(manifest)},
        "hci_commands_observed": re.findall(r"HCI command opcode=([0-9A-F]+) parameters=(\d+)", text),
        "verified_hci_wire_events": wire_events,
        "decrementer_delivered": "[prime-decrementer]" in text,
        "unsupported_oh1_transfer_observed": "OH1 ioctlv unsupported" in text,
        "verified_raw_texture_memory_bindings": args.verify_raw_textures,
        "verified_gpu_validation_enabled": args.verify_gpu_validation,
        "verified_efb_persistence_policy_enabled": args.verify_efb_persistence,
        "raw_textures_observed": raw_textures,
        "raw_tluts_observed": raw_tluts,
        "verified_xfb_submission": args.verify_xfb_submission,
        "xfb_copies_observed": xfb_copies,
        "xfb_selections_observed": xfb_selections,
        "verified_fifo_storage": args.verify_fifo_storage,
        "raw_fifo_draws_observed": fifo_draws,
        "verified_cp_breakpoint": args.verify_cp_breakpoint,
        "verified_cp_frame_loop_without_reset": args.verify_cp_frame_loop,
        "cp_interrupts_observed": cp_interrupts,
        "source_hashes": {path: sha(ROOT / path) for path in sources},
        "invariants": {"matching_dol_exact": True, "iso_size": iso_state.st_size, "iso_read_only": True},
        "reference_provenance": {"path": rel(reference_manifest), "sha256": sha(reference_manifest),
                                 "commit": references["commit"]},
        "limitations": ["No game screen is verified.", "ISO data reads are connected, but FST publication/startup still requires the private research extraction.",
            "DSP task execution, AI/DSP interrupt dispatch and audio output remain unverified.",
            "Controller has no paired remotes or ACL connections; HCI support is incomplete.",
            "Host continuation transport is experimental; thread cancellation/reuse, general preemption, complete FPU contexts and graphics equivalence remain unverified.",
            "Runtime test-suite results are separate evidence; this verifier covers the recorded boot path only."]}
    if args.checkpoint:
        assert sha(exe) == probe["executable_sha256"], "Executable changed during evidence verification"
        args.checkpoint.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"result": "PASS evidence verification", "outcome": report["outcome"], "resource_reads": len(reads),
        "hci_commands": report["hci_commands_observed"], "game_screen_verified": False}, indent=2))


if __name__ == "__main__":
    main()
