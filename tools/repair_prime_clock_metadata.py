#!/usr/bin/env python3
"""Normalize future compiler-input mtimes after a successful build following a clock rollback.

Only source/header metadata beneath recomp/ is changed. File bytes and game
resources are untouched. Invoke after the build completes, never during compilation.
"""
import hashlib
import json
import os
import re
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "recomp/build/RM3E01_00_runtime_link_probe"
NINJA = ROOT / "recomp/.tools/portable-tools/Ninja/ninja.exe"


def main():
    now = time.time_ns()
    objects = [path for target in ("mkw_runtime_common", "mkw_prime_rso")
               for path in (BUILD / "CMakeFiles" / f"{target}.dir").rglob("*.obj")]
    assert objects, "No completed runtime/module objects"
    pchs = list((BUILD / "CMakeFiles/mkw_runtime_common.dir").glob("*.pch"))
    timestamps = [path.stat().st_mtime_ns for path in objects + pchs]
    assert all(value <= now + 2_000_000_000 for value in timestamps), "Future objects must be rebuilt first"
    reference = min(timestamps) - 1_000_000_000
    # Ninja compares inputs with the recorded command snapshot, which can be
    # earlier than the completed object's filesystem mtime. Derive its Windows
    # timestamp conversion from the PCH dependency record rather than guessing
    # an epoch, then keep normalized inputs older than both forms of evidence.
    if pchs:
        pch = pchs[0]
        relative = pch.relative_to(BUILD).as_posix()
        dep = subprocess.run([str(NINJA), "-C", str(BUILD), "-t", "deps", relative],
                             check=True, capture_output=True, text=True)
        match = re.search(r"deps mtime (\d+)", dep.stdout)
        assert match, "Missing PCH dependency timestamp"
        epoch_offset = pch.stat().st_mtime_ns - int(match[1]) * 100
        # Validate the conversion against a second independently recorded file.
        # Ninja's epoch is implementation-specific; old objects may exceed a day.
        sample = objects[0]
        sample_dep = subprocess.run([str(NINJA), "-C", str(BUILD), "-t", "deps",
                                     sample.relative_to(BUILD).as_posix()],
                                    check=True, capture_output=True, text=True)
        sample_match = re.search(r"deps mtime (\d+)", sample_dep.stdout)
        assert sample_match, "Missing independent object dependency timestamp"
        sample_offset = sample.stat().st_mtime_ns - int(sample_match[1]) * 100
        assert abs(sample_offset - epoch_offset) <= 100, "Inconsistent Ninja timestamp conversion"
        wanted = {path.relative_to(BUILD).as_posix().lower() for path in objects + pchs}
        recorded = {}
        for line in (BUILD / ".ninja_log").read_text().splitlines():
            fields = line.split("\t")
            if len(fields) == 5 and fields[3].replace("\\", "/").lower() in wanted:
                recorded[fields[3].replace("\\", "/").lower()] = int(fields[2])
        if recorded:
            reference = min(reference, min(recorded.values()) * 100 + epoch_offset - 1_000_000_000)
        assert 0 < reference <= min(timestamps) - 1_000_000_000 <= now, "Unexpected Ninja timestamp conversion"
    result = subprocess.run([str(NINJA), "-C", str(BUILD), "-t", "deps"], check=True, capture_output=True, text=True)
    suffixes = {".h", ".hpp", ".hxx", ".hh", ".inc", ".inl", ".tcc", ".cpp", ".cxx", ".cc", ".c", ".s", ".def"}
    candidates = set()
    output = ROOT / "recomp/checkpoints/20261021_clock_input_metadata_repair.json"
    # An earlier repair may have used the first object, overlooking the PCH
    # generated before it. Reconcile only unchanged inputs owned by that receipt.
    if output.exists():
        previous = json.loads(output.read_text(encoding="utf-8"))
        for row in previous["compiler_inputs"]:
            path = (ROOT / row["path"]).resolve()
            if (path.is_relative_to(ROOT / "recomp") and path.suffix.lower() in suffixes and path.is_file()
                    and path.stat().st_mtime_ns == row["normalized_mtime_ns"] > reference
                    and hashlib.sha256(path.read_bytes()).hexdigest() == row["sha256"]):
                candidates.add(path)
    names = {line.strip() for line in result.stdout.splitlines() if line.startswith("    ")}
    for name in names:
        path = Path(name)
        if path.suffix.lower() not in suffixes:
            continue
        path = path.resolve()
        if not path.is_file() or path.suffix.lower() not in suffixes or not path.is_relative_to(ROOT / "recomp"):
            continue
        if path.stat().st_mtime_ns > now + 2_000_000_000:
            candidates.add(path)
    repaired = []
    for path in sorted(candidates):
        before = path.stat()
        fingerprint = hashlib.sha256(path.read_bytes()).hexdigest()
        os.utime(path, ns=(before.st_atime_ns, reference))
        assert hashlib.sha256(path.read_bytes()).hexdigest() == fingerprint, "Source bytes changed"
        repaired.append({"path": path.relative_to(ROOT).as_posix(), "sha256": fingerprint,
                         "previous_mtime_ns": before.st_mtime_ns, "normalized_mtime_ns": reference})
    report = {"revision": "RM3E01_00", "source_bytes_unchanged": True, "compiler_inputs": repaired,
              "scope": "Future compiler source/header mtimes only, after successful rebuild; no ISO/DOL/resource metadata is touched."}
    if repaired:
        output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"normalized_future_compiler_inputs": len(repaired), "source_bytes_unchanged": True}))


if __name__ == "__main__":
    main()
