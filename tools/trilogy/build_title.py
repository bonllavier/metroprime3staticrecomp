#!/usr/bin/env python3
"""Runs the RM3E01 pipeline (build_prime_runtime.ps1) on a Trilogy title's work tree."""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_IDS = {row["name"]: row for row in json.loads((ROOT / "config/RM3E01_00/rso_modules.json").read_text())["modules"]}
STEPS = ("rso", "repairs", "hooks", "shards", "configure")
# ORIGINAL_REPAIRS of audit_prime_dol_switches.py, by their RM3E01 address.
REVIEWED_REPAIRS = (0x800E01F0, 0x80420444, 0x8019804C, 0x8042B530, 0x80423190, 0x80423EF8, 0x8040D9D8,
                    0x8040F98C, 0x80414E20, 0x80415CB8, 0x804167CC, 0x80419848, 0x80425B10, 0x80427A34, 0x804280AC)


def run(work: Path, *command: str) -> None:
    done = subprocess.run(command, cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, errors="replace")
    if done.returncode:
        print(done.stdout[-3000:])
        raise SystemExit(f"FAILED: {' '.join(command)}")


def modules_json(base: Path) -> list[dict]:
    """Same module ids as RM3E01 where the module exists: the runtime refers to some by id."""
    names = sorted(path.stem[4:] for path in (base / "extract/RSO").glob("RSO_*.rso"))
    rows, free = [], max(row["id"] for row in MODULE_IDS.values()) + 1
    for name in names:
        known = MODULE_IDS.get(name)
        if known:
            module_id, symbol = known["id"], known["symbol"]
        else:
            module_id, symbol = free, f"g_prime{name}Module"
            free += 1
        rows.append({"name": name, "id": module_id, "symbol": symbol,
                     "rso_sha256": hashlib.sha256((base / f"extract/RSO/RSO_{name}.rso").read_bytes()).hexdigest()})
    return sorted(rows, key=lambda row: row["id"])


def configure(title: str, work: Path) -> None:
    """The RM3E01 build's configuration, reusing the sources its dependencies already fetched."""
    tools = ROOT / ".tools/portable-tools"
    mingw = tools / "llvm-mingw/bin"
    build = ROOT / "build/RM3E01_00_runtime_link_probe"
    dependencies = {}
    for folder in (build / "_deps", ROOT / "build/aurora-rm3-dawn-20261016-probe/_deps"):
        for source in folder.glob("*-src"):
            dependencies.setdefault(source.name[:-4].upper(), source)
    command = ["cmake", "-S", "recomp/third_party/WiiCompiled/runtime", "-B", f"recomp/build/{title}_runtime", "-G", "Ninja",
               f"-DCMAKE_MAKE_PROGRAM={tools / 'Ninja/ninja.exe'}", "-DCMAKE_BUILD_TYPE=Release",
               f"-DCMAKE_C_COMPILER={mingw / 'clang.exe'}", f"-DCMAKE_CXX_COMPILER={mingw / 'clang++.exe'}",
               f"-DCMAKE_ASM_COMPILER={mingw / 'clang.exe'}", f"-DCMAKE_LINKER={mingw / 'ld.lld.exe'}",
               f"-DCMAKE_RC_COMPILER={mingw / 'windres.exe'}",
               f"-DMKW_PRIME_NOD_SOURCE_DIR={ROOT / '.tools/nod-source'}",
               f"-DMKW_PRIME_RSO_DIR={work / 'recomp/.tools/rso-runtime/rhbm'}",
               f"-DMKW_CPPWINRT_INCLUDE_DIR={build / 'dependencies/cppwinrt'}",
               "-DMKW_GAME_PROFILE_RM3E01_00=ON", "-DMKW_PRIME_QA_TOOLS=ON", "-DCMAKE_SUPPRESS_REGENERATION=ON",
               "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG", "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG",
               "-DAURORA_DAWN_PROVIDER=package", "-DAURORA_DAWN_VERSION=v20260603.191052", "-DAURORA_DAWN_LINKAGE=shared",
               "-DAURORA_SDL3_PROVIDER=package", "-DAURORA_SDL3_LINKAGE=shared", "-DAURORA_SDL3_VERSION=3.4.4", "-DAURORA_SDL3_LIBUSB=OFF",
               "-DAURORA_NOD_PROVIDER=package", "-DAURORA_NOD_LINKAGE=static", "-DAURORA_NOD_VERSION=v2.0.0-alpha.8",
               "-DAURORA_ENABLE_DVD=OFF", "-DAURORA_ENABLE_CARD=OFF", "-DAURORA_ENABLE_GX=ON", "-DAURORA_CACHE_USE_ZSTD=ON"]
    command += [f"-DFETCHCONTENT_SOURCE_DIR_{name}={path}" for name, path in dependencies.items()]
    run(work, *[part.replace(chr(92), "/") if part.startswith("-D") else part for part in command])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("title", choices=("R3ME01_MP3", "R3ME01_FE"))
    parser.add_argument("--from-step", default="rso", choices=STEPS)
    args = parser.parse_args()
    title = args.title
    base = ROOT / "titles" / title
    work = base / "work"
    dotnet = str(work / "recomp/.tools/dotnet/dotnet.exe")
    rso_tool = "recomp/tools/RsoRuntimeTranslator/bin/Release/net8.0/RsoRuntimeTranslator.dll"
    python = [sys.executable, "-B"]
    start = STEPS.index(args.from_step)
    config = work / f"recomp/config/{title}"
    extract = f"recomp/.tools/disc-extraction-{title.lower()}/files/RSO/Production"
    if start <= 0:
        run(work, dotnet, "build", "recomp/tools/RsoRuntimeTranslator/RsoRuntimeTranslator.csproj", "-c", "Release", "--nologo")
        run(work, dotnet, rso_tool, "--self-test-relocations")
        rows = modules_json(base)
        skipped: dict[str, str] = {}
        (work / "recomp/.tools/rso-rel").mkdir(parents=True, exist_ok=True)
        for row in rows:
            name = row["name"]
            rel = f"recomp/.tools/rso-rel/{name}.synthetic.rel"
            # A module importing what this DOL does not export could never link on the console.
            done = subprocess.run([*python, "recomp/tools/convert_rso_to_rel_probe.py", f"{extract}/RSO_{name}.rso", rel],
                                  cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, errors="replace")
            if done.returncode:
                if "RSO import absent" not in done.stdout:
                    raise SystemExit(done.stdout[-3000:])
                skipped[name] = done.stdout.strip().splitlines()[-1]
                continue
            row["rel_sha256"] = hashlib.sha256((work / rel).read_bytes()).hexdigest()
        rows = [row for row in rows if row["name"] not in skipped]
        print(f"modules not linkable on this DOL: {skipped}", flush=True)
        (base / "rso_unlinkable.json").write_text(json.dumps(skipped, indent=1), encoding="utf-8")
        (config / "rso_modules.json").write_text(json.dumps(
            {"description": f"{title} RSO modules, roots discovered automatically.", "modules": rows}, indent=1), encoding="utf-8")
        for index, row in enumerate(rows):
            name = row["name"]
            run(work, *python, "recomp/tools/prepare_prime_rso_translation.py", "--module", name)
            run(work, dotnet, rso_tool, f"recomp/.tools/rso-runtime/{name}/input.json", f"recomp/.tools/rso-runtime/{name}/functions")
            run(work, *python, "recomp/tools/verify_rso_rel_probe.py", f"{extract}/RSO_{name}.rso",
                f"recomp/.tools/rso-rel/{name}.synthetic.rel", f"recomp/.tools/rso-runtime/{name}/functions/relocated_module.bin",
                "--output", f"recomp/.tools/rso-runtime/{name}/nominal_relocations.json")
            run(work, *python, "recomp/tools/emit_prime_rso_registry.py", "--module", name)
            print(f"[{index + 1}/{len(rows)}] {name}", flush=True)
        specs = "\n".join(f'    "{row["name"]},{row["symbol"]}"' for row in rows)
        (work / "recomp/.tools/rso-runtime/modules.cmake").write_text(
            f"set(MKW_PRIME_RSO_MODULE_SPECS\n{specs})\n", encoding="utf-8", newline="")
        print("rso done", flush=True)
    generated = work / f"recomp/generated/{title}_conservative"
    if start <= 1:
        # The audit reads the same layout the hook overlay later writes.
        metadata = json.loads((generated / "base_translation_output.json").read_text(encoding="utf-8"))
        for item in metadata["functions"]:
            item["relativePath"] = "functions/" + item["relativePath"]
        (generated / "runtime_hooks").mkdir(exist_ok=True)
        hooks_metadata = generated / "runtime_hooks/base_translation_output.json"
        hooks_metadata.write_text(json.dumps(metadata), encoding="utf-8")
        map_rows = json.loads((base / "address_map.json").read_text(encoding="utf-8"))["function_pairs"]
        original = [map_rows[f"0x{a:08X}"][0][2:] for a in REVIEWED_REPAIRS]
        overrides = config / "dol_jump_tables.json"
        overrides.unlink(missing_ok=True)
        (work / "recomp/.tools/dol-runtime-repairs").mkdir(parents=True, exist_ok=True)
        for entry in original:
            run(work, dotnet, rso_tool, "--dol-repair", ".", entry, "recomp/.tools/dol-runtime-repairs")
        run(work, *python, "recomp/tools/audit_prime_dol_switches.py", "--original-repairs-only", "--emit-overrides", str(overrides.relative_to(work)))
        audited = json.loads(overrides.read_text(encoding="utf-8"))
        for entry in original + audited["repair_functions"]:
            run(work, dotnet, rso_tool, "--dol-repair", ".", entry, "recomp/.tools/dol-runtime-repairs")
        hooks_metadata.unlink()
        print("repairs done:", len(original), "reviewed,", len(audited["repair_functions"]), "audited", flush=True)
    if start <= 2:
        run(work, *python, "recomp/tools/prepare_prime_runtime_hooks.py")
        print("hooks done", flush=True)
        report = subprocess.run([*python, "recomp/tools/audit_prime_dol_switches.py"], cwd=work, text=True, stdout=subprocess.PIPE)
        print("switch audit after repairs:", report.stdout.strip()[:300], flush=True)
        if '"functions_with_missing_cases": 0' not in report.stdout:
            raise SystemExit("switch cases still missing")
    if start <= 3:
        run(work, dotnet, "recomp/third_party/WiiCompiled/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll", "emit-build-shards",
            "--project", f"recomp/config/{title}/project_full_conservative.yml",
            "--base-metadata", f"recomp/generated/{title}_conservative/runtime_hooks/base_translation_output.json",
            "--base-functions-dir", f"recomp/generated/{title}_conservative",
            "--native-source-dir", "recomp/third_party/WiiCompiled/runtime/src",
            "--out", f"recomp/generated/{title}_conservative/build_shards")
        print("shards done", flush=True)
    if start <= 4:
        configure(title, work)
        print("configured", flush=True)


if __name__ == "__main__":
    main()
