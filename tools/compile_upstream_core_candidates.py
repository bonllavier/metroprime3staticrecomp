"""Compile generic WiiCompiled CPU/memory runtime candidates without linking HLE/product code."""
from __future__ import annotations

import hashlib
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "third_party" / "WiiCompiled" / "runtime"
COMPILER = ROOT / ".tools" / "portable-tools" / "llvm-mingw" / "bin" / "x86_64-w64-mingw32-clang++.exe"
SOURCES = [
    "abi_bridge.cpp",
    "fpu_helpers.cpp",
    "guest_flat_memory.cpp",
    "memory.cpp",
    "ppc_helpers.cpp",
    "ppc_quantized.cpp",
    "recomp_mod_loader.cpp",
    "system_bridge.cpp",
]
FLAGS = [
    "-std=c++20", "-O2", "-march=x86-64-v3", "-fno-fast-math",
    "-ffp-contract=off", "-fno-slp-vectorize", "-w", "-DTARGET_PC",
]


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    output_dir = ROOT / "build" / "runtime-core"
    output_dir.mkdir(parents=True, exist_ok=True)
    include_dirs = [
        RUNTIME / "include",
        RUNTIME / "src",
        RUNTIME / "third_party" / "toml11",
        ROOT / "third_party" / "WiiCompiled",
        ROOT / "third_party" / "WiiCompiled" / "aurora-main" / "include",
    ]
    results = []
    for name in SOURCES:
        source = RUNTIME / "src" / name
        output = output_dir / f"{source.stem}.o"
        command = [str(COMPILER), *FLAGS]
        for include in include_dirs:
            command.extend(["-I", str(include)])
        command.extend(["-c", str(source), "-o", str(output)])
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise SystemExit(f"compile failed: {name}\n{result.stderr[-8000:]}")
        results.append({
            "source": str(source.relative_to(ROOT)).replace("\\", "/"),
            "source_sha256": digest(source),
            "object": str(output.relative_to(ROOT)).replace("\\", "/"),
            "object_size": output.stat().st_size,
            "object_sha256": digest(output),
        })
    version = subprocess.check_output([str(COMPILER), "--version"], text=True).splitlines()[0]
    checkpoint = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "game_revision": "RM3E01_00",
        "upstream_commit": "4c166a7eb1ddfc79b4c832e422e6bb00ec06fb4f",
        "purpose": "Compile-only candidates for generic CPU/memory/dispatch services; no title HLE, product, Aurora, or link.",
        "compiler": version,
        "target": "x86_64-w64-windows-gnu COFF; x86-64-v3",
        "flags": FLAGS,
        "object_count": len(results),
        "objects": results,
        "limitations": "Compile success is not semantic validation for Prime 3 and does not resolve all translated-image link dependencies.",
    }
    path = ROOT / "checkpoints" / "20261020_upstream_core_candidate_compile.json"
    path.write_text(json.dumps(checkpoint, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in checkpoint.items() if key != "objects"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
