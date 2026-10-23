"""Compile all startup translation shards to Windows COFF objects, without linking a runtime."""
from __future__ import annotations

import hashlib
import argparse
import json
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INPUT = ROOT / "input" / "RM3E01_00" / "main.dol"
EXPECTED_SHA1 = "3e81f9b09dfb1e7c02e22b9c6c48a4a1390224b2"
EXPECTED_SHA256 = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"
COMPILER = ROOT / ".tools" / "portable-tools" / "llvm-mingw" / "bin" / "x86_64-w64-mingw32-clang++.exe"


def digest(path: Path, algorithm: str = "sha256") -> str:
    h = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def compile_one(source: Path, kind: str, generated: Path, objects: Path) -> dict[str, object]:
    output = objects / (source.stem + ".o")
    command = [str(COMPILER)]
    if kind == "data_blob_assembly":
        command.extend(["-c", "-x", "assembler-with-cpp"])
    else:
        command.extend([
            "-std=c++20", "-O2", "-march=x86-64-v3", "-fno-fast-math",
            "-ffp-contract=off", "-fno-slp-vectorize", "-w",
            "-I", str(ROOT / "third_party" / "WiiCompiled" / "runtime" / "include"),
            "-I", str(generated), "-c",
        ])
    command.extend([str(source), "-o", str(output)])
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        return {
            "source": source.name,
            "kind": kind,
            "return_code": result.returncode,
            "stdout": result.stdout[-4000:],
            "stderr": result.stderr[-8000:],
        }
    return {
        "source": source.name,
        "kind": kind,
        "source_sha256": digest(source),
        "object": str(output.relative_to(ROOT)).replace("\\", "/"),
        "object_size": output.stat().st_size,
        "object_sha256": digest(output),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-id", default="RM3E01_00_startup")
    build_id = parser.parse_args().build_id
    if not build_id.replace("_", "").isalnum():
        raise SystemExit("--build-id must contain only letters, digits, and underscores")
    generated = ROOT / "generated" / build_id
    shards = generated / "build_shards" / "base_common"
    objects = ROOT / "build" / build_id / "shards"
    if digest(INPUT, "sha1") != EXPECTED_SHA1 or digest(INPUT) != EXPECTED_SHA256:
        raise SystemExit("RM3E01_00 input DOL hash mismatch")
    metadata = json.loads((generated / "base_translation_output.json").read_text(encoding="utf-8"))
    if metadata["quality"]["unsupportedInstructionCount"] != 0:
        raise SystemExit("Translation metadata reports unsupported instructions")
    sources = [(source, "translated_shard") for source in sorted(shards.glob("*.cpp"))]
    for directory in ("base_dispatch", "base_registration"):
        sources.extend(
            (source, "generated_registration")
            for source in sorted((generated / "build_shards" / directory).glob("*.cpp"))
        )
    initializer = generated / "data_sections_init.cpp"
    if initializer.is_file():
        sources.append((initializer, "data_initializer"))
    symbol_table = generated / "guest_symbol_table.cpp"
    if symbol_table.is_file():
        sources.append((symbol_table, "guest_symbol_table"))
    blobs = generated / "data_sections_init_blobs.S"
    if blobs.is_file():
        sources.append((blobs, "data_blob_assembly"))
    if not sources:
        raise SystemExit(f"No generated shards under {shards}")
    objects.mkdir(parents=True, exist_ok=True)
    version = subprocess.check_output([str(COMPILER), "--version"], text=True).splitlines()[0]
    flags = ["-std=c++20", "-O2", "-march=x86-64-v3", "-fno-fast-math", "-ffp-contract=off", "-fno-slp-vectorize", "-w"]
    checkpoint_path = ROOT / "checkpoints" / f"{datetime.now(timezone.utc):%Y%m%d}_{build_id}_shard_compile.json"
    cached: dict[str, dict[str, object]] = {}
    if checkpoint_path.is_file():
        old = json.loads(checkpoint_path.read_text(encoding="utf-8"))
        if old.get("compiler") == version and old.get("flags") == flags and old.get("input_sha256") == EXPECTED_SHA256:
            cached = {str(item["source"]): item for item in old.get("objects", [])}
    results: list[dict[str, object]] = []
    pending_sources: list[tuple[Path, str]] = []
    for source, kind in sources:
        previous = cached.get(source.name)
        if previous and previous.get("source_sha256") == digest(source):
            previous_object = ROOT / str(previous["object"])
            if previous_object.is_file() and digest(previous_object) == previous.get("object_sha256"):
                results.append({**previous, "kind": kind, "reused": True})
                continue
        pending_sources.append((source, kind))
    with ThreadPoolExecutor(max_workers=4) as executor:
        futures = [executor.submit(compile_one, source, kind, generated, objects) for source, kind in pending_sources]
        for future in as_completed(futures):
            item = future.result()
            results.append(item)
            if "return_code" in item:
                for pending in futures:
                    pending.cancel()
                break
    results.sort(key=lambda item: str(item["source"]))
    failures = [item for item in results if "return_code" in item]
    checkpoint = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "game_revision": "RM3E01_00",
        "input_sha1": digest(INPUT, "sha1"),
        "input_sha256": digest(INPUT),
        "compiler": version,
        "target": "x86_64-w64-windows-gnu COFF; x86-64-v3",
        "flags": flags,
        "translated_shard_count": sum(kind == "translated_shard" for _, kind in sources),
        "generated_registration_source_count": sum(kind == "generated_registration" for _, kind in sources),
        "data_initializer_included": any(kind == "data_initializer" for _, kind in sources),
        "compiled_object_count": len(results) - len(failures),
        "newly_compiled_object_count": len(results) - len(failures) - sum(bool(item.get("reused")) for item in results),
        "reused_object_count": sum(bool(item.get("reused")) for item in results),
        "compiled_object_bytes": sum(int(item.get("object_size", 0)) for item in results),
        "linked_runtime": False,
        "failures": failures,
        "objects": results,
    }
    checkpoint_path.parent.mkdir(parents=True, exist_ok=True)
    checkpoint_path.write_text(json.dumps(checkpoint, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in checkpoint.items() if key != "objects"}, indent=2))
    return 1 if failures or len(results) != len(sources) else 0


if __name__ == "__main__":
    raise SystemExit(main())

