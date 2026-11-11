#!/usr/bin/env python3
"""Emit a private runtime descriptor; original game resources remain private."""
import hashlib
import argparse
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import inspect_static_sel


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    table = {row["name"]: row for row in json.loads(
        (ROOT / "recomp/config/RM3E01_00/rso_modules.json").read_text())["modules"]}
    parser.add_argument("--module", choices=tuple(table), default="rhbm")
    name = parser.parse_args().module
    directory = ROOT / f"recomp/.tools/rso-runtime/{name}"
    module = json.loads((directory / "module.json").read_text())
    translation = json.loads((directory / "functions/translation.json").read_text())
    raw = (ROOT / f"recomp/.tools/disc-extraction-rm3e01/files/RSO/Rev2 Production/RSO_{name}.rso").read_bytes()
    assert hashlib.sha256(raw).hexdigest() == module["original_sha256"]
    dol = (ROOT / "decomp/orig/RM3E01_00/sys/main.dol").read_bytes()
    sel = inspect_static_sel.inspect_bytes((ROOT / "decomp/orig/RM3E01_00/files/RSO/Rev2 Production/selfile.sel").read_bytes(), inspect_static_sel.section_bases(dol))
    exports = {item["name"]: item["address"] for item in sel["exports"]}
    masked = [bytearray(raw[s["file_offset"]:s["file_offset"] + s["size"]]) if s["file_offset"] else bytearray(s["size"]) for s in module["sections"]]
    relocation_lines = []
    for group in ("internal", "external"):
        for row in module["relocations"][group]:
            section, offset, typ = row["source_section"], row["source_section_offset"], row["type"]
            data = masked[section]
            if typ == 1: data[offset:offset + 4] = bytes(4)
            elif typ in (4, 6): data[offset:offset + 2] = bytes(2)
            elif typ == 10:
                value = struct.unpack_from(">I", data, offset)[0] & 0xFC000003
                struct.pack_into(">I", data, offset, value)
            else: raise ValueError(f"Unreviewed relocation {typ}")
            imported = exports[module["original_imports"][row["index"]]["name"]] if group == "external" else 0
            relocation_lines.append(f'    {{{section}u, {offset}u, {typ}u, {row["index"] if group == "internal" else 0}u, {row["target_offset"]}u, 0x{imported:08X}u, {str(group == "external").lower()}}},')
    code = ['#include "hle/prime_rso_runtime.h"', '']
    sources, seen = [], set()
    for row in translation["functions"]:
        source = directory / "functions" / row["path"]
        assert hashlib.sha256(source.read_bytes()).hexdigest() == row["sha256"]
        assert not seen.intersection(row["instructionAddresses"]), "Overlapping translated module bodies"
        seen.update(row["instructionAddresses"])
        code.append(f'extern "C" void {row["name"]}(CpuContext*);')
        sources.append(source)
    code += ['', 'namespace {', 'const PrimeRsoSection sections[] = {']
    for section, data in zip(module["sections"], masked):
        code.append(f'    {{0x{section["nominal"]:08X}u, {section["size"]}u, "{hashlib.sha256(data).hexdigest()}"}},')
    code += ['};', 'const PrimeRsoRelocation relocations[] = {', *relocation_lines, '};', 'const PrimeRsoFunction functions[] = {']
    for row in translation["functions"]:
        code.append(f'    {{0x{row["entry"]:08X}u, {row["name"]}}},')
    prolog = module["entry_points"]["prolog"]
    symbol = table[name]["symbol"]
    code += ['};', '}', f'extern const PrimeRsoModule {symbol} = {{',
             f'    {module["module_id"]}u, {len(raw)}u, {prolog["section"]}u, {prolog["offset"]}u, {module["sections"][prolog["section"]]["file_offset"]}u, "{name}", "{module["original_sha256"]}",',
             '    sections, sizeof(sections)/sizeof(sections[0]),',
             '    relocations, sizeof(relocations)/sizeof(relocations[0]),',
             '    functions, sizeof(functions)/sizeof(functions[0])', '};', '']
    registry = directory / "registry.cpp"
    content = '\n'.join(code)
    if not registry.exists() or registry.read_text(encoding="utf-8") != content:
        registry.write_text(content, encoding="utf-8")
    sources.append(registry)
    cmake = directory / "sources.cmake"
    content = 'set(MKW_PRIME_RSO_GENERATED_SOURCES\n' + ''.join(f'    "{source.as_posix()}"\n' for source in sources) + ')\n'
    if not cmake.exists() or cmake.read_text(encoding="utf-8") != content:
        cmake.write_text(content, encoding="utf-8")
    print(json.dumps({"functions": len(sources) - 1, "relocations": len(relocation_lines), "source_file": str(registry)}, indent=2))


if __name__ == "__main__": main()
