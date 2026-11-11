#!/usr/bin/env python3
"""Generates a Trilogy title's work tree from the RM3E01 port, leaving RM3E01 untouched.

titles/<title>/work mirrors the workspace layout, so the existing tools and the
runtime build run on it unchanged. Their copies get this title's addresses,
identity and pins. Guest addresses come from address_map.json (make_address_map.py).
Only addresses of the Prime port are rewritten: the runtime keeps many Mario Kart
literals that are dead here, and a constant is not an address just because it looks
like one. A hook whose original instruction does not match stops the generation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_address_map import ROOT, WORKSPACE, Dol, normalize

LITERAL = re.compile(r"(0[xX]|func_|loc_)(8[01][0-9A-Fa-f]{6})(?![0-9A-Fa-f])")
RM3E01_PIN = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"
TEXT = (".cpp", ".cc", ".c", ".h", ".hpp", ".inc", ".cmake", ".txt", ".py", ".cs", ".csproj", ".json", ".S", ".rc", ".in", ".md")
# Real Prime addresses inside the generic runtime files that are not function entries.
INTERIOR_ALLOWED = {0x804E6CD4, 0x804E6D20}
# Look like addresses, are not: a DSP mail value and a range end.
NOT_ADDRESSES = {0x80544348, 0x80590000, 0x80808081}
SCRIPTS = ("prepare_prime_runtime_hooks.py", "convert_rso_to_rel_probe.py", "verify_rso_rel_probe.py",
           "discover_prime_rso_roots.py", "emit_prime_rso_registry.py", "audit_prime_rso_address_taken.py",
           "audit_prime_dol_switches.py", "verify_prime_context_save_binding.py")
TOP_SCRIPTS = ("inspect_startup_rso.py", "inspect_static_sel.py")
# RM3E01 hooks whose code is gone: Trilogy's newer WPADInit no longer polls WUD itself.
DROPPED_HOOK_LINES = ("0x804D9898:", "0x804D98A0:")
# Its only wait for status 3 is in the caller, so that loop services interrupts instead: (owner, loop).
WPAD_WAITS = {"R3ME01_MP3": (0x803C25D8, 0x803C2764), "R3ME01_FE": (0x80374B94, 0x80374D20)}
# OSExec, OSGetSavedRegion, OSSetSaveRegion's small-data pair and the only caller of OSGetSavedRegion,
# read from each DOL. The translator inlines that leaf, so the restore runs at the caller's entry.
EXEC_HOOKS = {"R3ME01_MP3": (0x804E28E4, 0x804E58D4, -0x1C70, 0x802D3958), "R3ME01_FE": (0x80490890, 0x80493880, -0x21B0, 0x8028B55C)}
# CPlayer fields that moved in rs5mp3_p.dol: text in the runtime, RM3E01 value, this DOL's value, and the same
# instruction in each DOL that proves it. A proof that stops holding stops the generation.
MOVED_FIELDS = {
    "src/hle/input/prime_mouse_look.cpp": (
        ("kPlayerOrbitSubState = {:#X}u", 0x730, 0x738, 0x80152618, 0x8014E47C),
        ("kPlayerFreeLookPitch = {:#X}u", 0x780, 0x788, 0x80152394, 0x8014E238),
        ("kPlayerPitchOverride = {:#X}u", 0x3B40, 0x3B54, 0x801525F4, 0x8014E458),
        ("kPlayerAimMode = {:#X}u", 0x3C18, 0x3C28, 0x8017A4E0, 0x801763B4),
        ("kPlayerTurnRamp = {:#X}u", 0x3C20, 0x3C30, 0x80179A68, 0x8017593C),
        ("kPlayerTurnLockTimer = {:#X}u", 0x3AD4, 0x3AE8, 0x80179B74, 0x80175A48),
        ("kPlayerControlHintFlags = {:#X}u", 0x34EC, 0x34F4, 0x8014CECC, 0x80148CE8),
        ("kPlayerControlMapper = {:#X}u", 0x35C4, 0x35D8, 0x8017A508, 0x801763DC),
        ("kPlayerFreeLookCurrentPitch = {:#X}u", 0x77C, 0x784, 0x80152398, 0x8014E23C),
        ("kPlayerFreeLookVelocity = {:#X}u", 0x3C1C, 0x3C2C, 0x801520B4, 0x8014DF58),
        ("u32({:#X}u)", 0x34EC, 0x34F4, 0x8014CECC, 0x80148CE8),  # state trace
        ("u8({:#X}u)", 0x6B4, 0x6B8, 0x8014782C, 0x80142FCC),
        ("u32({:#X}u)", 0x3E78, 0x3E88, 0x80158450, 0x80154380),
    ),
    "src/qa/prime_input_replay.cpp": (
        ("kPlayerStatePtr = {:#X}", 0x35A0, 0x35A8, 0x8017A58C, 0x80176460),
    ),
}
# rs5mp3_p.dol's sensitivity getter, replacing RM3E01's CGameState +0x142C: lwz r3,options(r13) / cmpwi r3,0 /
# beq default / lwz r3,0x24(r3) / blr / default: lwz r3,fallback(r13) / blr. The address is this DOL's, not mapped.
SENSITIVITY_GETTER = 0x800AE3AC
OVERRIDE = re.compile(r"\b(gx_stub_|PPC_NATIVE_OVERRIDE(?:_VOID)?\(\s*|GX_FATAL_STUB\(\s*|REGISTER_(?:NATIVE|TRANSLATED)_FUNCTION(?:_AS)?\(\s*0[xX]|MKW_KNOWN_TYPED_NATIVE\(\s*0[xX])([0-9A-Fa-f]{8})")


def link(target: Path, path: Path) -> None:
    if path.exists():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cmd", "/c", "mklink", "/J", str(path), str(target)], check=True, stdout=subprocess.DEVNULL)


def identity(text: str, title: str) -> str:
    return (text.replace("RM3E01_00", title)
            .replace("disc-extraction-rm3e01", "disc-extraction-" + title.lower())
            .replace("Rev2 Production", "Production"))


class Title:
    def __init__(self, title: str):
        self.title = title
        self.base = ROOT / "titles" / title
        self.work = self.base / "work"
        self.report = json.loads((self.base / "address_map.json").read_text(encoding="utf-8"))
        self.resolved = {int(k, 16): (int(v["target"], 16), v["how"]) for k, v in self.report["resolved"].items()}
        project = (self.base / "project.yml").read_text(encoding="utf-8")
        self.r13 = int(re.search(r"sda_base: (0x[0-9A-F]+)", project)[1], 16)
        self.r2 = int(re.search(r"sda2_base: (0x[0-9A-F]+)", project)[1], 16)
        self.a = Dol(ROOT / "input/RM3E01_00/main.dol", 0x806801C0, 0x806869C0)
        self.b = Dol(self.base / "extract/sys/main.dol", self.r13, self.r2)
        self.dol = (self.base / "extract/sys/main.dol").read_bytes()
        self.sha256 = hashlib.sha256(self.dol).hexdigest()
        self.entries = {int(line.split()[0], 16) for line in (self.base / "functions.txt").read_text(encoding="utf-8").splitlines()}
        self.neutralized: set[int] = set()
        self.rewritten: dict[int, int] = {}
        self.left: dict[int, set[str]] = {}

    def runtime_target(self, value: int, relative: str) -> int | None:
        """The new address when this runtime literal is a real Prime address."""
        if value in NOT_ADDRESSES or value not in self.resolved or value >= 0x8069D000:
            return None
        target, how = self.resolved[value]
        name = relative.rsplit("/", 1)[-1]
        if name.startswith("prime_") or "prime" in name:
            return target
        if value < 0x803BE000:
            return None
        if how.startswith("interior") and value not in INTERIOR_ALLOWED:
            return None
        if value >= 0x80520000 and value < 0x80575680:
            return None  # THP and task-thread leftovers of the Mario Kart profile.
        return target

    def rewrite_runtime_text(self, text: str, relative: str) -> str:
        def replace(match: re.Match) -> str:
            value = int(match[2], 16)
            target = self.runtime_target(value, relative)
            if target is None:
                if value >= 0x80004000:
                    self.left.setdefault(value, set()).add(relative)
                return match[0]
            self.rewritten[value] = target
            digits = f"{target:08X}" if match[2].upper() == match[2] else f"{target:08x}"
            return match[1] + digits
        return LITERAL.sub(replace, text)

    def rewrite_all(self, text: str) -> str:
        def replace(match: re.Match) -> str:
            value = int(match[2], 16)
            if value in NOT_ADDRESSES or value not in self.resolved:
                return match[0]
            target = self.resolved[value][0]
            digits = f"{target:08X}" if match[2].upper() == match[2] else f"{target:08x}"
            return match[1] + digits
        return LITERAL.sub(replace, text)

    def neutralize_overrides(self, text: str) -> str:
        """A dead Mario Kart override on this DOL's function entry would replace a real function."""
        def replace(match: re.Match) -> str:
            address = int(match[2], 16)
            if address not in self.entries:
                return match[0]
            self.neutralized.add(address)
            return match[1] + "A" + match[2][1:]
        return OVERRIDE.sub(replace, text)

    def exec_hooks(self, text: str) -> str:
        """Overlays of OSExec and OSGetSavedRegion's caller, in this DOL's addresses (prime_trilogy_exec.cpp)."""
        exec_entry, saved_entry, region, saved_caller = EXEC_HOOKS[self.title]
        # OSGetSavedRegion: lis r5,hi / addi r5,r5,lo / lwz r0,start(r5) / stw r0,0(r3) / lwz r0,end(r5)
        leaf = self.b.words(saved_entry, 5 * 4)
        if (self.b.words(exec_entry, 1 * 4)[0] != 0x9421FF40 or leaf[0] >> 16 != 0x3CA0 or leaf[1] >> 16 != 0x38A5
                or leaf[2] >> 16 != 0x8005 or leaf[4] != leaf[2] + 4):
            raise ValueError(f"{self.title}: OSExec or OSGetSavedRegion is not where expected")
        saved_start = ((leaf[0] & 0xFFFF) << 16) + (leaf[1] & 0x7FFF) - (leaf[1] & 0x8000) + (leaf[2] & 0xFFFF)
        first = self.resolved[0x803BBF38][0]
        anchor = f"        if address == 0x{first:08X}:\n"
        if text.count(anchor) != 1:
            raise ValueError("The first overlay branch moved")
        branch = (
            f"        if address == 0x{exec_entry:08X}:\n"
            "            source=replace_once(source,'#include \"guest_abi/guest_abi.h\"',\n"
            "                '#include \"guest_abi/guest_abi.h\"\\n'\n"
            "                'extern \"C\" bool PrimeTrilogyExec(CpuContext*, uint32_t);')\n"
            f"            anchor='extern \"C\" void func_{exec_entry:08X}(CpuContext* MKW_RESTRICT ctx)\\n{{'\n"
            f"            source=replace_once(source,anchor,anchor+'\\n    if (PrimeTrilogyExec(ctx, static_cast<uint32_t>(ctx->gpr[13]) - {-region}u)) return;')\n"
            f"        elif address == 0x{saved_caller:08X}:\n"
            f"            if 'inline leaf 0x{saved_entry:08X}' not in source:\n"
            "                raise ValueError('OSGetSavedRegion is no longer inlined in its caller')\n"
            "            source=replace_once(source,'#include \"guest_abi/guest_abi.h\"',\n"
            "                '#include \"guest_abi/guest_abi.h\"\\n'\n"
            "                'extern \"C\" void PrimeTrilogySavedRegion(uint32_t);')\n"
            f"            anchor='extern \"C\" void func_{saved_caller:08X}(CpuContext* MKW_RESTRICT ctx)\\n{{'\n"
            f"            source=replace_once(source,anchor,anchor+'\\n    PrimeTrilogySavedRegion(0x{saved_start:08X}u);')\n"
            f"        elif address == 0x{first:08X}:\n")
        text = text.replace(anchor, branch)
        marker = "        if address not in ("
        if text.count(marker) != 1:
            raise ValueError("The overlay list moved")
        return text.replace(marker, f"{marker}0x{exec_entry:08X}, 0x{saved_caller:08X}, ")

    def moved_fields(self, text: str, relative: str) -> str:
        """This DOL's CPlayer offsets and sensitivity option, each checked against both DOLs."""
        if self.title != "R3ME01_MP3":
            return text
        for pattern, old, new, old_at, new_at in MOVED_FIELDS.get(relative, ()):
            old_word, new_word = self.a.words(old_at, 4)[0], self.b.words(new_at, 4)[0]
            if old_word != (old_word & 0xFFFF0000 | old) or new_word != (old_word & 0xFFFF0000 | new):
                raise ValueError(f"{pattern}: {old_at:08X} {old_word:08x} and {new_at:08X} {new_word:08x} do not prove {old:#X} -> {new:#X}")
            before, after = pattern.format(old).replace("0X", "0x"), pattern.format(new).replace("0X", "0x")
            if before not in text:
                raise ValueError(f"{relative}: {before} is gone")
            text = text.replace(before, after)
        if relative == "src/hle/input/prime_mouse_look.cpp":
            words = self.b.words(SENSITIVITY_GETTER, 7 * 4)
            if (words[0] >> 16 != 0x806D or words[1:5] != (0x2C030000, 0x4182000C, 0x80630024, 0x4E800020)
                    or words[5] >> 16 != 0x806D or words[6] != 0x4E800020):
                raise ValueError(f"The sensitivity getter is not at {SENSITIVITY_GETTER:08X}")
            options = self.r13 + (words[0] & 0x7FFF) - (words[0] & 0x8000)
            fallback = self.r13 + (words[5] & 0x7FFF) - (words[5] & 0x8000)
            read = "!Read32(gameState + kGameStateSensitivity, sensitivity)"
            anchor = "// Angular velocity for a turn factor of 1, as "
            if read not in text or text.count(anchor) != 1:
                raise ValueError("TurnSpeed moved")
            text = text.replace(read, "!ReadSensitivity(sensitivity)")
            text = text.replace(anchor, (
                f"// The in-game sensitivity option, as 0x{SENSITIVITY_GETTER:08X} reads it.\n"
                "bool ReadSensitivity(uint32_t& value) {\n"
                "    uint32_t options = 0;\n"
                f"    if (!Read32(0x{options:08X}u, options)) return false;\n"
                f"    return options ? Read32(options + 0x24u, value) : Read32(0x{fallback:08X}u, value);\n"
                "}\n\n") + anchor)
        return text

    def copy_runtime(self) -> None:
        source = ROOT / "third_party/WiiCompiled/runtime"
        destination = self.work / "recomp/third_party/WiiCompiled/runtime"
        for path in source.rglob("*"):
            if path.is_dir():
                continue
            relative = str(path.relative_to(source)).replace("\\", "/")
            target = destination / relative
            data = path.read_bytes()
            if path.suffix in TEXT and relative.startswith(("src/", "include/")):
                text = data.decode("utf-8", errors="surrogateescape")
                text = self.rewrite_runtime_text(text, relative)
                text = self.neutralize_overrides(text)
                text = apply_runtime_identity(text, relative)
                text = self.moved_fields(text, relative)
                data = text.encode("utf-8", errors="surrogateescape")
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists() or target.read_bytes() != data:
                target.write_bytes(data)
        overlay = Path(__file__).resolve().parent / "runtime_overlay/prime_trilogy_exec.cpp"
        target = destination / "src/hle/os/prime_trilogy_exec.cpp"
        if not target.exists() or target.read_bytes() != overlay.read_bytes():
            shutil.copyfile(overlay, target)

    def expected_instructions(self, text: str) -> str:
        """Each hook's expected word becomes this DOL's word when the instruction is the same."""
        problems = []

        def replace(match: re.Match) -> str:
            old = int(match[1], 16)
            word = int(match[2], 16)
            if old not in self.resolved:
                problems.append(f"{old:08X}: no address")
                return match[0]
            new = self.resolved[old][0]
            actual = self.b.words(new, 4)
            if actual is None:
                problems.append(f"{old:08X} -> {new:08X}: outside the DOL")
                return match[0]
            if normalize(actual[0]) != normalize(word):
                problems.append(f"{old:08X} -> {new:08X}: {word:08x} became {actual[0]:08x} ({match[3].strip()[:50]})")
                return match[0]
            return f'0x{old:08X}: "{actual[0]:08x}",{match[3]}'
        out = re.sub(r'0x(8[0-9A-F]{7}): "([0-9a-f]{8})",([^\n]*)', replace, text)
        if problems:
            print(f"{self.title}: {len(problems)} hook instructions differ")
            for line in problems:
                print("  ", line)
        self.hook_problems = problems
        return out

    def copy_scripts(self) -> None:
        for name in SCRIPTS:
            text = (ROOT / "tools" / name).read_text(encoding="utf-8", errors="surrogateescape")
            if name == "prepare_prime_runtime_hooks.py":
                text = "".join(line for line in text.splitlines(True) if not line.lstrip().startswith(DROPPED_HOOK_LINES))
                text = text.replace("0x804D9850, ", "WPAD_WAIT_OWNER, ", 1).replace("'loc_804D9898'", "'WPAD_WAIT_LABEL'")
                # The filler's "lfs f0,1.0" reads the title's own small-data constant.
                word = self.b.words(self.resolved[0x803E9B48][0], 4)[0]
                displacement = (word & 0x7FFF) - (word & 0x8000)
                text = text.replace("(r2 + -13180)", f"(r2 + {displacement})")
                # The language gate's "lbz r0,flag(r13)" (0x802F13A4) reads this title's small-data flag.
                word = self.b.words(self.resolved[0x802F13A4][0], 4)[0]
                if word >> 16 != 0x880D:
                    raise ValueError(f"The language gate is not lbz r0,d(r13) here: {word:08x}")
                displacement = (word & 0x7FFF) - (word & 0x8000)
                text = text.replace("(r13 + -8896)", f"(r13 + {displacement})")
                # RM3E01's table bounds are replaced by the switch audit build_title.py runs on the repairs.
                text = text.replace("    for entry, tables in repair_tables:",
                                    "    repair_tables = tuple((entry, ()) for entry, _ in repair_tables)\n    for entry, tables in repair_tables:")
                text = text.replace("        assert len(switch_bodies) == len(tables)\n", "")
                text = text.replace("        assert actual_cases == expected_cases\n", "")
                text = self.expected_instructions(text)
                text = re.sub(r"\n\s*assert digest\(original\) == \"[0-9a-f]{64}\"", "", text)
            if name == "discover_prime_rso_roots.py":
                # A table without a recognised guard ends where the next switch table starts.
                old = "while (section, start + 4 * count) in data_code and (bound == 0 or count < bound):"
                new = ("while (section, start + 4 * count) in data_code and (bound == 0 or count < bound) "
                       "and (count == 0 or (section, start + 4 * count) not in table_starts):")
                if old not in text:
                    raise ValueError("The switch table walk moved")
                text = text.replace(old, new).replace("    switch_sources = set()\n",
                                                      "    switch_sources = set()\n    table_starts = set(switch_tables)\n", 1)
            text = self.rewrite_all(identity(text, self.title)).replace(RM3E01_PIN, self.sha256)
            if name == "prepare_prime_runtime_hooks.py":
                owner, loop = WPAD_WAITS[self.title]
                # cmpwi r3,3 / bne back to the WPADGetStatus call.
                if self.b.words(loop + 4, 8) != (0x2C030003, 0x4082FFF8):
                    raise ValueError(f"{self.title}: no WPAD wait at {loop:08X}")
                text = text.replace("WPAD_WAIT_OWNER", f"0x{owner:08X}").replace("WPAD_WAIT_LABEL", f"loc_{loop:08X}")
                text = self.exec_hooks(text)
            target = self.work / "recomp/tools" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8", errors="surrogateescape")
        # The reviewed per-module roots hold RM3E01 offsets and were always a subset
        # of the discovered roots, which that script keeps as an assertion.
        text = (ROOT / "tools/prepare_prime_rso_translation.py").read_text(encoding="utf-8", errors="surrogateescape")
        start = text.index('    if name == "MapScreenWebInterface":')
        end = text.index("    # Every relocated function entry of the module")
        text = identity(text[:start] + text[end:], self.title)
        (self.work / "recomp/tools/prepare_prime_rso_translation.py").write_text(text, encoding="utf-8", errors="surrogateescape")
        for name in TOP_SCRIPTS:
            text = (WORKSPACE / "tools" / name).read_text(encoding="utf-8", errors="surrogateescape")
            text = identity(text, self.title)
            if name == "inspect_static_sel.py":
                # The section switch moved: read its table from this DOL instead of pinning it.
                switch = self.resolved[0x80540914][0]
                start = text.index("    # Case destinations read from the switch")
                end = text.index("    return result\n", start)
                text = text[:start] + f"""    high,low=word(0x{switch:08X}),word(0x{switch + 4:08X})
    if high&0xffff0000!=0x3c600000 or low&0xffff0000!=0x38630000:
        raise ValueError('Static section switch differs')
    table=(((high&65535)<<16)+(low&0x7fff)-(low&0x8000))&0xffffffff
    result={{}}
    for index in (1,2,5,6,7,8,9,11,12):
        pc=word(table+index*4)
        high,low=word(pc),word(pc+4)
        if high&0xffff0000!=0x3c600000 or low&0xffff0000!=0x38030000:
            raise ValueError('Unexpected section base instruction pair')
        signed_low=(low&0x7fff)-(low&0x8000)
        result[index]=(((high&65535)<<16)+signed_low)&0xffffffff
""" + text[end:]
                text = re.sub(r"DOL_SHA1='[0-9a-f]{40}'", f"DOL_SHA1='{hashlib.sha1(self.dol).hexdigest()}'", text)
                selfile = (self.base / "extract/RSO/selfile.sel").read_bytes()
                text = re.sub(r"SEL_SHA1='[0-9a-f]{40}'", f"SEL_SHA1='{hashlib.sha1(selfile).hexdigest()}'", text)
            target = self.work / "tools" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8", errors="surrogateescape")
        source = ROOT / "tools/RsoRuntimeTranslator"
        for path in source.iterdir():
            if path.is_file() and path.suffix in (".cs", ".csproj"):
                text = identity(path.read_text(encoding="utf-8", errors="surrogateescape"), self.title).replace(RM3E01_PIN, self.sha256)
                target = self.work / "recomp/tools/RsoRuntimeTranslator" / path.name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8", errors="surrogateescape")

    def copy_inputs(self) -> None:
        low = self.title.lower()
        rso_dir = self.base / "extract/RSO"
        files = sorted(rso_dir.iterdir())
        destinations = [self.work / f"decomp/orig/{self.title}/files/RSO/Production",
                        self.work / f"recomp/.tools/disc-extraction-{low}/files/RSO/Production"]
        for destination in destinations:
            destination.mkdir(parents=True, exist_ok=True)
            for path in files:
                shutil.copyfile(path, destination / path.name)
        for path in (self.work / f"decomp/orig/{self.title}/sys/main.dol", self.work / f"recomp/input/{self.title}/main.dol",
                     self.work / f"recomp/.tools/disc-extraction-{low}/sys/main.dol"):
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.base / "extract/sys/main.dol", path)
        symbols = self.work / f"decomp/config/{self.title}/symbols.txt"
        symbols.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(self.base / "dtk/config/symbols.txt", symbols)
        config = self.work / f"recomp/config/{self.title}"
        config.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(self.base / "functions.txt", config / "functions.txt")
        project = (self.base / "project.yml").read_text(encoding="utf-8")
        # emit-build-shards turns on the Prime runtime profile for game_id RM3E01 only; nothing else reads it there.
        project = (project.replace("game_id: R3ME01", "game_id: RM3E01")
                   .replace("workspace_root: .", "workspace_root: ../..")
                   .replace("path: extract/sys/main.dol", f"path: input/{self.title}/main.dol")
                   .replace("path: functions.txt", f"path: config/{self.title}/functions.txt")
                   .replace("root: generated", f"root: generated/{self.title}_conservative")
                   .replace("base_manifest: generated/base_manifest.json", f"base_manifest: build/{self.title}_conservative/base_manifest.json"))
        (config / "project_full_conservative.yml").write_text(project, encoding="utf-8")
        link(self.base / "generated", self.work / f"recomp/generated/{self.title}_conservative")
        link(self.base / "generated", self.work / "recomp/third_party/WiiCompiled/generated")
        for name in ("dotnet", "portable-tools", "nod-source", "rust"):
            link(ROOT / ".tools" / name, self.work / "recomp/.tools" / name)
        for name in ("aurora-main", "translator"):
            link(ROOT / "third_party/WiiCompiled" / name, self.work / "recomp/third_party/WiiCompiled" / name)
        link(ROOT / "third_party/reshade", self.work / "recomp/third_party/reshade")
        for name in ("LICENSE", "THIRD-PARTY-NOTICES.md", "README.md"):
            shutil.copyfile(ROOT / "third_party/WiiCompiled" / name, self.work / "recomp/third_party/WiiCompiled" / name)


def apply_runtime_identity(text: str, relative: str) -> str:
    """The disc and the NAND title of Metroid Prime Trilogy."""
    if relative.endswith("hle/storage/dvd.cpp"):
        text = text.replace('std::memcmp(header.game_id, "RM3E01", 6)', 'std::memcmp(header.game_id, "R3ME01", 6)')
        text = replace_exact(text, 'constexpr const char kExpectedDiscId[] = "RM3E01";',
                             'constexpr const char kExpectedDiscId[] = "R3ME01";')
        text = text.replace("Metroid Prime 3: Corruption (USA), game ID RM3E01", "Metroid Prime Trilogy (USA), game ID R3ME01")
        text = text.replace("(USA, RM3E01:", "(USA, R3ME01:")
        text = text.replace("id=RM3E01", "id=R3ME01")
        text = text.replace("#if !defined(PRIME_TRILOGY_TITLE)", "#if 0")  # These are the Trilogy executables.
    if relative.endswith("system_bridge.cpp"):
        text = text.replace("kGameIdCode = 0x524D3345u; // RM3E", "kGameIdCode = 0x52334D45u; // R3ME")
    # The curtain over a change of executable (prime_trilogy_exec.cpp).
    if relative == "src/main.cpp":
        text = replace_exact(text, "void PrimeWaitForRestartedParent();  // hle/os/prime_host_restart.cpp\n",
                             "void PrimeWaitForRestartedParent();  // hle/os/prime_host_restart.cpp\n"
                             "void PrimeTrilogyCurtainBegin();  // hle/os/prime_trilogy_exec.cpp\n")
        text = replace_exact(text, "    PrimeWaitForRestartedParent();\n",
                             "    PrimeTrilogyCurtainBegin();\n    PrimeWaitForRestartedParent();\n")
    if relative == "src/hle/vi.cpp":
        text = replace_exact(text, "        aurora_end_frame();\n        if (beginBeforePacing) finishPresent();\n",
                             "        aurora_end_frame();\n        PrimeTrilogyFramePresented();\n        if (beginBeforePacing) finishPresent();\n")
        text = text.replace("#include", 'extern "C" void PrimeTrilogyFramePresented();\n#include', 1)
    return text


def replace_exact(text: str, old: str, new: str) -> str:
    """Replaces the one occurrence of old, whichever line endings the source file has."""
    if "\r\n" in text:
        old, new = old.replace("\n", "\r\n"), new.replace("\n", "\r\n")
    if text.count(old) != 1:
        raise ValueError(f"Expected one occurrence of: {old!r}")
    return text.replace(old, new)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("title", choices=("R3ME01_MP3", "R3ME01_FE"))
    title = Title(parser.parse_args().title)
    title.copy_inputs()
    title.copy_scripts()
    title.copy_runtime()
    (title.base / "runtime_literals.json").write_text(json.dumps({
        "rewritten": {f"0x{k:08X}": f"0x{v:08X}" for k, v in sorted(title.rewritten.items())},
        "left_as_is": {f"0x{k:08X}": sorted(v) for k, v in sorted(title.left.items())},
    }, indent=1), encoding="utf-8")
    print(f"{title.title}: runtime literals rewritten {len(title.rewritten)}, left {len(title.left)}, overrides neutralized {len(title.neutralized)}")
    # A dead literal that became the entry of a real function here would hijack it.
    entries = {int(line.split()[0], 16) for line in (title.base / "functions.txt").read_text(encoding="utf-8").splitlines()}
    collisions = sorted(k for k in title.left if k in entries)
    print("left literals that are function entries in this DOL:", [f"{k:08X}" for k in collisions])
    for k in collisions:
        print("  ", f"{k:08X}", sorted(title.left[k])[:3])


if __name__ == "__main__":
    main()
