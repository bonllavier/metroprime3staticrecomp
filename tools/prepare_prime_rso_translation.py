#!/usr/bin/env python3
"""Prepare a private, runtime-relocatable translation manifest for rhbm's real prolog/ctors."""
import hashlib
import argparse
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from inspect_startup_rso import inspect_bytes
sys.path.insert(0, str(Path(__file__).resolve().parent))
from discover_prime_rso_roots import discover


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    table = json.loads((ROOT / "recomp/config/RM3E01_00/rso_modules.json").read_text())["modules"]
    rows = {row["name"]: row for row in table}
    parser.add_argument("--module", choices=tuple(rows), default="rhbm")
    name = parser.parse_args().module
    module_id = rows[name]["id"]
    pins = {name: (rows[name]["rso_sha256"], rows[name]["rel_sha256"])}
    directory = ROOT / f"recomp/.tools/rso-runtime/{name}"
    directory.mkdir(parents=True, exist_ok=True)
    original_path = ROOT / f"recomp/.tools/disc-extraction-rm3e01/files/RSO/Rev2 Production/RSO_{name}.rso"
    raw = original_path.read_bytes()
    module = inspect_bytes(raw)
    assert module["sha256"] == pins[name][0]
    rel_path = ROOT / f"recomp/.tools/rso-rel/{name}.synthetic.rel"
    rel = rel_path.read_bytes()
    assert hashlib.sha256(rel).hexdigest() == pins[name][1]
    table = struct.unpack_from(">I", rel, 0x10)[0]
    base = 0x80690000  # Nominal code identities, never a required runtime placement.
    sections = []
    for section in module["sections"]:
        offset, size = struct.unpack_from(">II", rel, table + section["index"] * 8)
        offset &= ~1
        if section["bss"]:
            offset = (len(rel) + 0x3F) & ~0x1F
        sections.append({**section, "nominal": base + offset})
    sites = []
    boundaries = set()
    constructors = []
    for group in ("internal", "external"):
        for row in module["relocations"][group]:
            if row["source_section"] == 1:
                offset = row["source_section_offset"]
                if row["type"] in (4, 6):
                    assert offset % 4 == 2
                    offset -= 2
                else:
                    assert offset % 4 == 0
                sites.append({"pc": sections[1]["nominal"] + offset, "type": row["type"], "external": group == "external"})
            if group == "internal" and row["index"] == 1:
                address = sections[1]["nominal"] + row["target_offset"]
                if row["type"] in (1, 10):
                    boundaries.add(address)
                if row["source_section"] == 2 and row["type"] == 1:
                    constructors.append(address)
    for row in module["exports"]:
        if row["section"] == 1:
            boundaries.add(sections[1]["nominal"] + row["offset"])
    roots = [sections[entry["section"]]["nominal"] + entry["offset"]
             for entry in module["entry_points"].values()]
    roots += constructors
    if name == "MapScreenWebInterface":
        # The epilog walks the original section-3 destructor pointer array.
        dtors=[r for r in module["relocations"]["internal"] if r["source_section"]==3]
        assert sorted((r["source_section_offset"],r["type"],r["index"],r["target_offset"]) for r in dtors)==[
            (0,1,1,0x1D6A0),(4,1,1,0x1D6A0)]
        assert struct.unpack_from(">I",raw,sections[1]["file_offset"]+0x1D6A0)[0]==0x9421FFF0
        roots += [sections[1]["nominal"]+r["target_offset"] for r in dtors]
    if name == "MapScreenWebInterface":
        # Startup publishes this factory through the imported setter. DOL
        # 0x80166940 later tail-calls it; it is not an interior switch label.
        factory=[r for r in module["relocations"]["internal"] if r["source_section"]==1
            and r["index"]==1 and r["target_offset"]==0xDC]
        assert sorted((r["source_section_offset"],r["type"]) for r in factory)==[(0x22,6),(0x32,4)]
        assert struct.unpack_from(">I",raw,sections[1]["file_offset"]+0xDC)[0]==0x9421FFE0
        roots.append(sections[1]["nominal"]+0xDC)
        # Callback installed from the map interface; later invoked by the
        # original DOL dispatcher 0x803D7EB0. Verify its paired HA/LO refs.
        callback=[r for r in module["relocations"]["internal"] if r["source_section"]==1
            and r["index"]==1 and r["target_offset"]==0x73A4]
        assert sorted((r["source_section_offset"],r["type"]) for r in callback)==[(0x735E,6),(0x7366,4)]
        assert struct.unpack_from(">I",raw,sections[1]["file_offset"]+0x73A4)[0]==0x9421FFE0
        roots.append(sections[1]["nominal"]+0x73A4)
        # Static-array destructor callback registered by startup and invoked
        # indirectly during the section-3 cleanup above.
        cleanup=[r for r in module["relocations"]["internal"] if r["source_section"]==1
            and r["index"]==1 and r["target_offset"]==0x1D40C]
        assert sorted((r["source_section_offset"],r["type"]) for r in cleanup)==[(119542,6),(119550,4)]
        assert struct.unpack_from(">I",raw,sections[1]["file_offset"]+0x1D40C)[0]==0x3C600000
        roots.append(sections[1]["nominal"]+0x1D40C)
        # Audit the remaining address-taken callbacks together: temporary
        # callable objects and the array element destructor used on map exit.
        audit=json.loads((ROOT/"recomp/config/RM3E01_00/map_remaining_callbacks.json").read_text())
        assert audit["module_sha256"]==module["sha256"]
        for item in audit["callbacks"]:
            target=item["text_offset"]
            references=sorted([r["source_section_offset"],r["type"]] for r in module["relocations"]["internal"]
                if r["source_section"]==1 and r["index"]==1 and r["target_offset"]==target and r["type"] in (4,6))
            assert references==item["references"]
            assert struct.unpack_from(">I",raw,sections[1]["file_offset"]+target)[0]==item["entry_word"]
            roots.append(sections[1]["nominal"]+target)
    if name == "rhbm":
        # Original HOME lifecycle API. Scene transition pid10244 calls
        # HBMShutdown (text+0xEF60) via the loader's stored export pointer.
        # Translate the coherent eight-export interface, not only boot/init.
        expected_exports={"HBMSize":60776,"HBMInit":60784,"HBMOpen":60896,
                          "HBMClose":61204,"HBMShutdown":61280,"HBMUpdate":61360,
                          "HBMStartBlackOut":61440,"HBMDraw":61504}
        exports={r["name"]:r["offset"] for r in module["exports"] if r["section"]==1 and not r["name"].startswith('_')}
        assert exports==expected_exports
        roots += [sections[1]["nominal"]+offset for offset in expected_exports.values()]
    if name == "ScriptContextActionButtonPressing":
        factory=[r for r in module["relocations"]["internal"] if r["source_section"]==1
                 and r["index"]==1 and r["target_offset"]==5184]
        assert len(factory)==2 and {(r["source_section_offset"],r["type"]) for r in factory}=={(2,6),(10,4)}
        roots.append(sections[1]["nominal"]+5184)
        expected={36:3552,48:4452,64:5572,68:5560,80:5700,84:5688,96:5916,108:2676,112:300}
        pointers=[r for r in module["relocations"]["internal"] if r["source_section"]==5 and r["index"]==1]
        assert len(pointers)==len(expected) and all(r["type"]==1 for r in pointers)
        assert {r["source_section_offset"]:r["target_offset"] for r in pointers}==expected
        roots += [sections[1]["nominal"]+r["target_offset"] for r in pointers]
    if name == "ScriptContextSensitiveAction":
        factory=[r for r in module["relocations"]["internal"] if r["source_section"]==1
                 and r["index"]==1 and r["target_offset"]==664]
        assert len(factory)==2 and {(r["source_section_offset"],r["type"]) for r in factory}=={(2,6),(10,4)}
        roots.append(sections[1]["nominal"]+664)
        expected={8:8544,20:8544,32:436,44:3156,48:1552,60:8772,80:8756,
                  124:8748,128:8744,132:8740,136:8716,140:8704,144:8696,160:8792,164:8780}
        pointers=[r for r in module["relocations"]["internal"] if r["source_section"]==5 and r["index"]==1]
        assert len(pointers)==len(expected) and all(r["type"]==1 for r in pointers)
        assert {r["source_section_offset"]:r["target_offset"] for r in pointers}==expected
        roots += [sections[1]["nominal"]+r["target_offset"] for r in pointers]
    if name == "ScriptMultiModelActor":
        factory=[r for r in module["relocations"]["internal"] if r["source_section"]==1
                 and r["index"]==1 and r["target_offset"]==6376]
        assert len(factory)==2 and {(r["source_section_offset"],r["type"]) for r in factory}=={(2,6),(10,4)}
        roots.append(sections[1]["nominal"]+6376)
        # Original relocated data code pointers. The first ten are switch
        # labels, not callable functions: retain them in their owning body.
        expected={0:9724,4:9564,8:9404,12:9244,16:9084,20:8924,24:8764,28:8604,32:8444,36:8284,
                  48:11504,60:11316,76:10544,96:10528,124:10556,140:10520,144:10516,148:10512,
                  152:10488,156:10476,160:10468,168:11208,176:10984,184:10760,200:10552,
                  212:1224,224:1532,228:2488,240:10544,260:10528,280:2028,284:2024,
                  304:10520,308:10516,312:10512,316:10488,320:10476,324:10468,364:10552,372:2244,376:2268}
        pointers=[r for r in module["relocations"]["internal"] if r["source_section"]==5 and r["index"]==1]
        assert len(pointers)==len(expected) and all(r["type"]==1 for r in pointers)
        assert {r["source_section_offset"]:r["target_offset"] for r in pointers}==expected
        text=sections[1]["file_offset"]
        assert struct.unpack_from(">8I",raw,text+0x203C)==(
            0x28000009,0x418105BC,0x3C600000,0x5400103A,
            0x38630000,0x7C63002E,0x7C6903A6,0x4E800420)
        switch_refs=[r for r in module["relocations"]["internal"] if r["source_section"]==1
                     and r["source_section_offset"] in (0x2046,0x204E)]
        assert {(r["source_section_offset"],r["type"],r["index"],r["target_offset"]) for r in switch_refs}=={
            (0x2046,6,5,0),(0x204E,4,5,0)} and len(switch_refs)==2
        roots += [sections[1]["nominal"]+r["target_offset"] for r in pointers if r["source_section_offset"]>=40]
    if name == "ScriptGrapplePoint":
        # Runtime missing target 0x80F8E634 in pid11212 is text+0x1DBC.
        # Original text+0x1AA8/0x1AB0 forms this callback address and stores
        # it in a live stack record later dispatched by DOL 0x801AEE3C.
        callback=[r for r in module["relocations"]["internal"] if r["source_section"]==1
                  and r["index"]==1 and r["target_offset"]==0x1DBC]
        assert len(callback)==2 and {(r["source_section_offset"],r["type"]) for r in callback}=={
            (0x1AAA,6),(0x1AB2,4)}
        assert struct.unpack_from(">4I",raw,sections[1]["file_offset"]+0x1DBC)==(
            0x9421FFE0,0x7C0802A6,0x90010024,0x93E1001C)
        roots.append(sections[1]["nominal"]+0x1DBC)
        # Original startup takes this code address through a checked HA/LO pair.
        factory=[r for r in module["relocations"]["internal"]
                 if r["source_section"]==1 and r["index"]==1 and r["target_offset"]==0x2B98]
        assert {(r["source_section_offset"],r["type"]) for r in factory}=={(34,6),(62,4)} and len(factory)==2
        roots.append(sections[1]["nominal"]+0x2B98)
        # All 23 local code pointers in original data, including member-pointer
        # records and methods. Do not infer one common vtable/class layout.
        pointers=[r for r in module["relocations"]["internal"] if r["source_section"]==5 and r["index"]==1]
        expected={8:8556,20:9896,32:9448,44:9524,56:9748,68:9660,80:3124,92:10856,
                  136:10528,152:12492,156:12480,168:1980,180:4444,184:6452,196:12464,
                  216:12448,236:2208,260:12472,264:8192,268:8416,272:12424,276:12412,280:12404}
        assert len(pointers)==len(expected) and all(r["type"]==1 for r in pointers)
        assert {r["source_section_offset"]:r["target_offset"] for r in pointers}==expected
        roots += [sections[1]["nominal"]+r["target_offset"] for r in pointers]
    if name == "ScriptContextSensitiveActivator":
        # Original startup publishes text+0x210 through the imported setter.
        factory = [r for r in module["relocations"]["internal"] if r["source_section"]==1
            and r["index"]==1 and r["target_offset"]==0x210]
        assert len(factory)==2 and {(r["source_section_offset"],r["type"]) for r in factory}=={(14,6),(30,4)}
        roots.append(sections[1]["nominal"]+0x210)
        # Factory constructor text+0x40 publishes the complete observed table
        # data+0x18: two zero headers, 30 methods, then non-pointer string bytes.
        refs=[r for r in module["relocations"]["internal"] if r["source_section"]==1
            and 0x40<=r["source_section_offset"]<0x19C and r["index"]==5]
        assert len(refs)==2 and {(r["source_section_offset"],r["type"],r["target_offset"]) for r in refs}=={
            (0x12A,6,0x18),(0x132,4,0x18)}
        assert struct.unpack_from(">II",raw,sections[5]["file_offset"]+0x18)==(0,0)
        methods=[r for group in ("internal","external") for r in module["relocations"][group]
            if r["source_section"]==5 and 0x20<=r["source_section_offset"]<0x98]
        assert len(methods)==30 and all(r["type"]==1 for r in methods)
        assert {r["source_section_offset"] for r in methods}==set(range(0x20,0x98,4))
        local=[r for r in module["relocations"]["internal"] if r in methods]
        assert len(local)==11 and all(r["index"]==1 for r in local)
        assert {r["target_offset"] for r in local}=={0x19C,0x5E4,0x420,0xD50,0xD40,0xD38,0xD34,0xD30,0xD18,0xD0C,0xD04}
        roots += [sections[1]["nominal"]+r["target_offset"] for r in local]
    if name == "ScriptAchievement":
        # Startup text+0 installs this factory via SetSAchievement_FuncPtrs.
        # DOL 0x801DEC1C later tail-calls its first word; it is a real function
        # entry, not an interior switch label despite the generic error dialog.
        factory = [row for row in module["relocations"]["internal"]
            if row["source_section"] == 1 and row["index"] == 1
            and row["target_offset"] == 0xE44]
        assert {(row["source_section_offset"],row["type"]) for row in factory} == {(2,6),(10,4)}
        assert len(factory) == 2
        roots.append(sections[1]["nominal"] + 0xE44)
        # The factory's constructor (text+0x24, HA/LO at +0x72/+0x76)
        # publishes data+0x0C as the object's virtual table. The original
        # DOL dispatch at 0x8027E3C8 reads +0x18 and reaches text+0x2FC.
        # Recover the complete observed seven-method table, keeping its
        # four original imported methods and adding its three local bodies.
        table_refs = [row for row in module["relocations"]["internal"]
            if row["source_section"] == 1 and row["index"] == 5
            and row["target_offset"] == 0xC]
        assert {(row["source_section_offset"],row["type"]) for row in table_refs} == {(0x72,6),(0x76,4)}
        assert len(table_refs) == 2
        assert struct.unpack_from(">II",raw,sections[5]["file_offset"]+0xC) == (0,0)
        methods = [row for group in ("internal","external") for row in module["relocations"][group]
            if row["source_section"] == 5 and 0x14 <= row["source_section_offset"] < 0x30]
        assert len(methods) == 7 and all(row["type"] == 1 for row in methods)
        assert {row["source_section_offset"] for row in methods} == set(range(0x14,0x30,4))
        local_methods = [row for row in module["relocations"]["internal"]
            if row["source_section"] == 5 and 0x14 <= row["source_section_offset"] < 0x30]
        assert {(row["source_section_offset"],row["index"],row["target_offset"]) for row in local_methods} == {
            (0x14,1,0x23C),(0x20,1,0x6EC),(0x24,1,0x2FC)}
        roots += [sections[1]["nominal"]+row["target_offset"] for row in local_methods]
    if name == "Tweaks":
        # The reviewed startup helper at text+0x174 writes three text
        # pointers into its table before the imported tail call. The DOL
        # subsequently calls its first two entries (PIDs 10564 and 15548).
        callbacks = [row for row in module["relocations"]["internal"]
                     if row["source_section"] == 1 and 0x174 <= row["source_section_offset"] < 0x1A4
                     and row["index"] == 1 and row["type"] in (4, 6)]
        assert len(callbacks) == 6
        assert {row["target_offset"] for row in callbacks} == {0x1BA14, 0x1BB94, 0x1BFB8}
        roots += [sections[1]["nominal"] + offset for offset in sorted({row["target_offset"] for row in callbacks})]
    if name == "ScriptGUIScreenFrontEndUI":
        # Live GSCR dispatch 0x8021D30C reads +4 of the table published by
        # text+0x133B4. Its HA/LO pair names the local factory at text+0x1675C.
        # The first entry is a separate DOL pointer, not the executed factory.
        factory = [row for row in module["relocations"]["internal"]
                   if row["source_section"] == 1 and 0x133B4 <= row["source_section_offset"] < 0x133D4
                   and row["index"] == 1 and row["target_offset"] == 0x1675C]
        assert len(factory) == 2 and {row["type"] for row in factory} == {4, 6}
        roots.append(sections[1]["nominal"] + 0x1675C)
        # The live object constructed by that factory uses the observed table
        # at data+0x548. Its 40 contiguous method pointers have 15 local entries;
        # the failing CStateManager call reads slot +0x18 (text+0x156DC).
        table_rows = [row for group in ("internal", "external") for row in module["relocations"][group]
                      if row["source_section"] == 5 and 0x550 <= row["source_section_offset"] < 0x5F0]
        assert len(table_rows) == 40 and all(row["type"] == 1 for row in table_rows)
        assert {row["source_section_offset"] for row in table_rows} == set(range(0x550, 0x5F0, 4))
        data_offset = sections[5]["file_offset"]
        assert struct.unpack_from(">II", raw, data_offset + 0x548) == (0, 0)
        assert struct.unpack_from(">II", raw, data_offset + 0x5F0) == (0, 0)
        local_methods = [row for row in module["relocations"]["internal"]
                         if row["source_section"] == 5 and 0x550 <= row["source_section_offset"] < 0x5F0
                         and row["type"] == 1 and row["index"] == 1]
        assert len(local_methods) == 15 and any(row["target_offset"] == 0x156DC for row in local_methods)
        roots += [sections[1]["nominal"] + row["target_offset"] for row in local_methods]
        # A real A input reaches a second object through DOL 0x800E01F0,
        # virtual slot +0x0C. Its table starts at data+0x240 with two null
        # header words, then 38 method pointers. The failed local method is
        # data+0x24C -> text+0x2AF0 (nominal 0x80699750).
        interface_rows=[row for group in ("internal","external") for row in module["relocations"][group]
            if row["source_section"]==5 and 0x248<=row["source_section_offset"]<0x2E0]
        assert len(interface_rows)==38 and all(row["type"]==1 for row in interface_rows)
        assert {row["source_section_offset"] for row in interface_rows}==set(range(0x248,0x2E0,4))
        assert struct.unpack_from(">II",raw,data_offset+0x240)==(0,0)
        assert struct.unpack_from(">II",raw,data_offset+0x2E0)==(0,0)
        local_interface=[row for row in module["relocations"]["internal"]
            if row["source_section"]==5 and 0x248<=row["source_section_offset"]<0x2E0
            and row["type"]==1 and row["index"]==1]
        assert len(local_interface)==22
        assert any(row["source_section_offset"]==0x24C and row["target_offset"]==0x2AF0 for row in local_interface)
        roots += [sections[1]["nominal"]+row["target_offset"] for row in local_interface]
        # Popup listener invocation observed at DOL 0x80199C9C. Seven
        # reviewed HA/LO pairs install the same local wrapper text+0x3888.
        wrapper_refs=[row for row in module["relocations"]["internal"]
            if row["source_section"]==1 and row["index"]==1 and row["target_offset"]==0x3888]
        assert len(wrapper_refs)==14 and {row["type"] for row in wrapper_refs}=={4,6}
        high=sorted(row["source_section_offset"] for row in wrapper_refs if row["type"]==6)
        low=sorted(row["source_section_offset"] for row in wrapper_refs if row["type"]==4)
        assert len(high)==7 and low==[value+8 for value in high]
        roots.append(sections[1]["nominal"]+0x3888)
        # The payload is the original three-word PPC member pointer. All
        # twenty records have this_delta=0 and direct-call sentinel -1.
        # Their code pointers are separate roots, not class-completeness claims.
        member_refs=[row for row in module["relocations"]["internal"]
            if row["source_section"]==5 and 0xF8<=row["source_section_offset"]<0x1E0]
        assert len(member_refs)==20 and all(row["type"]==1 and row["index"]==1 for row in member_refs)
        assert {row["source_section_offset"] for row in member_refs}==set(range(0xF8,0x1E0,12))
        for offset in range(0xF0,0x1E0,12):
            assert struct.unpack_from(">II",raw,data_offset+offset)==(0,0xFFFFFFFF)
        roots += [sections[1]["nominal"]+row["target_offset"] for row in member_refs]
        # The original popup listener walks a second callback list. Its
        # next missing target (live 0x80B14164) is text+0x15BE4, installed
        # by these two reviewed HA/LO pairs. Both feed the pointer into
        # the original listener allocation; no callback signature is guessed.
        listener_refs=[row for row in module["relocations"]["internal"]
            if row["source_section"]==1 and row["index"]==1
            and row["target_offset"]==0x15BE4]
        assert len(listener_refs)==4
        assert {(row["source_section_offset"],row["type"]) for row in listener_refs}=={
            (0x15976,6),(0x1597E,4),(0x15A96,6),(0x15A9E,4)}
        roots.append(sections[1]["nominal"]+0x15BE4)
        # A second signature wrapper is installed by the immediately
        # preceding helper. A real A press reaches it after the small-table
        # callbacks; live placement may differ between runs.
        listener_variant=[row for row in module["relocations"]["internal"]
            if row["source_section"]==1 and row["index"]==1
            and row["target_offset"]==0x15C58]
        assert {(row["source_section_offset"],row["type"]) for row in listener_variant}=={
            (0x15856,6),(0x1585E,4)} and len(listener_variant)==2
        roots.append(sections[1]["nominal"]+0x15C58)
        # SaveGameInterface invokes the original wrapper stored by the
        # startup helper, with a three-word member payload from data+0xF0.
        save_refs=[row for row in module["relocations"]["internal"]
            if row["source_section"]==1 and row["index"]==1
            and row["target_offset"]==0x27E0]
        assert {(row["source_section_offset"],row["type"]) for row in save_refs}=={
            (0x236E,6),(0x2376,4)} and len(save_refs)==2
        roots.append(sections[1]["nominal"]+0x27E0)
        # The original popup choices call a shared listener after the Mii
        # and WC24 paths become reachable. Four constructors install it.
        popup_listener=[row for row in module["relocations"]["internal"]
            if row["source_section"]==1 and row["index"]==1
            and row["target_offset"]==0x9F2C]
        assert {(row["source_section_offset"],row["type"]) for row in popup_listener}=={
            (0x993A,6),(0x9942,4),(0xB8AA,6),(0xB8B2,4),
            (0x11986,6),(0x1198E,4),(0x11BF2,6),(0x11BFA,4)} and len(popup_listener)==8
        roots.append(sections[1]["nominal"]+0x9F2C)
        # Two more installations use the same verified member-listener wrapper
        # prefix as text+0x27E0. Include the coherent wrapper family rather than
        # registering interior addresses when each menu state reaches one.
        for callback, pairs in (
            (0x7434, ((0x72C6,0x72CE),(0x7ACE,0x7AD6),(0x818E,0x8196),
                      (0x8B02,0x8B0A),(0x8D76,0x8D7E),(0x8E92,0x8E9A),(0xA40A,0xA412))),
            (0xEC64, ((0xEB5E,0xEB66),)),
        ):
            references=[row for row in module["relocations"]["internal"]
                if row["source_section"]==1 and row["index"]==1
                and row["target_offset"]==callback]
            assert {(row["source_section_offset"],row["type"]) for row in references}=={
                (position,kind) for high,low in pairs for position,kind in ((high,6),(low,4))}
            assert len(references)==len(pairs)*2
            text_offset=module["sections"][1]["file_offset"]
            assert raw[text_offset+callback:text_offset+callback+36]==raw[text_offset+0x27E0:text_offset+0x27E0+36]
            roots.append(sections[1]["nominal"]+callback)
        # Correct ES title-directory and FS usage handling enables the
        # original early script callback at text+0x13C4C. Its list node
        # carries a member pointer copied from one of two static records.
        early_listener=[row for row in module["relocations"]["internal"]
            if row["source_section"]==1 and row["index"]==1
            and row["target_offset"]==0x13C4C]
        assert {(row["source_section_offset"],row["type"]) for row in early_listener}=={
            (0x1388A,6),(0x13892,4),(0x13A26,6),(0x13A2E,4)} and len(early_listener)==4
        roots.append(sections[1]["nominal"]+0x13C4C)
        early_members=[row for row in module["relocations"]["internal"]
            if row["source_section"]==5 and row["source_section_offset"] in (0x4B8,0x4C4)]
        assert len(early_members)==2 and all(row["type"]==1 and row["index"]==1
            and row["target_offset"]==0x13EA0 for row in early_members)
        for offset in (0x4B0,0x4BC):
            assert struct.unpack_from(">II",raw,data_offset+offset)==(0,0xFFFFFFFF)
        roots.append(sections[1]["nominal"]+0x13EA0)
        # The observed listener payload is (0, -1, text+0x140F8).
        # Audit its contiguous three-record group and translate the actual
        # destinations passed to the original PPC member-call dispatcher.
        listener_members=[row for row in module["relocations"]["internal"]
            if row["source_section"]==5 and 0x4F8<=row["source_section_offset"]<=0x510]
        assert len(listener_members)==3
        assert {(row["source_section_offset"],row["target_offset"]) for row in listener_members}=={
            (0x4F8,0x154C8),(0x504,0x1419C),(0x510,0x140F8)}
        assert all(row["type"]==1 and row["index"]==1 for row in listener_members)
        for offset in (0x4F0,0x4FC,0x508):
            assert struct.unpack_from(">II",raw,data_offset+offset)==(0,0xFFFFFFFF)
        roots += [sections[1]["nominal"]+row["target_offset"] for row in listener_members]
        # The listener progresses to a DOL virtual dispatch of the object
        # whose table is data+0x1E0 (observed slot +0x0C -> text+0x129CC).
        # Six adjacent tables each have two null header words and two
        # method slots: eleven local destinations and one original null.
        small_tables=[row for group in ("internal","external") for row in module["relocations"][group]
            if row["source_section"]==5 and 0x1E0<=row["source_section_offset"]<0x240]
        assert len(small_tables)==11 and all(row["type"]==1 for row in small_tables)
        assert {row["source_section_offset"] for row in small_tables}=={
            offset+slot for offset in range(0x1E0,0x240,16) for slot in (8,12)}-{0x21C}
        assert struct.unpack_from(">I",raw,data_offset+0x21C)[0]==0
        for offset in range(0x1E0,0x240,16):
            assert struct.unpack_from(">II",raw,data_offset+offset)==(0,0)
        local_small_tables=[row for row in module["relocations"]["internal"]
            if row["source_section"]==5 and 0x1E0<=row["source_section_offset"]<0x240]
        assert len(local_small_tables)==11 and all(row["index"]==1 for row in local_small_tables)
        assert {row["target_offset"] for row in local_small_tables}=={
            0x129D8,0x129CC,0x12A58,0x12A4C,0x12AD8,0x12ACC,
            0x41B0,0x12B58,0x12B4C,0x12BD8,0x12BCC}
        roots += [sections[1]["nominal"]+row["target_offset"] for row in local_small_tables]
        # The movie destructor reaches three adjacent cleanup-listener tables.
        # Each has two null header words, an owning destructor and a clear leaf.
        cleanup_tables=[row for group in ("internal","external")
            for row in module["relocations"][group]
            if row["source_section"]==5 and 0x518<=row["source_section_offset"]<0x548]
        assert len(cleanup_tables)==6 and all(row["type"]==1 and row["index"]==1 for row in cleanup_tables)
        assert {(row["source_section_offset"],row["target_offset"]) for row in cleanup_tables}=={
            (0x520,0x162A0),(0x524,0x16294),(0x530,0x16320),
            (0x534,0x16314),(0x540,0x163A0),(0x544,0x16394)}
        for table in (0x518,0x528,0x538):
            assert struct.unpack_from(">II",raw,data_offset+table)==(0,0)
        roots += [sections[1]["nominal"]+row["target_offset"] for row in cleanup_tables]
    if name == "ScriptWorldTeleporter":
        # The observed dispatcher 0x80267758 reads slot zero. The startup
        # helper at text+0 publishes that local factory via the HA/LO pair.
        factory = [row for row in module["relocations"]["internal"]
                   if row["source_section"] == 1 and row["source_section_offset"] in (6, 14)
                   and row["index"] == 1 and row["target_offset"] == 0x2988]
        assert len(factory) == 2 and {row["type"] for row in factory} == {4, 6}
        roots.append(sections[1]["nominal"] + 0x2988)
        # This object's observed table is data+0x4C: two zero prefix words
        # and seven relocated methods, followed by non-pointer data. It is a
        # different span from the front-end table; do not copy that layout.
        table_rows = [row for group in ("internal", "external") for row in module["relocations"][group]
                      if row["source_section"] == 5 and 0x54 <= row["source_section_offset"] < 0x70]
        assert len(table_rows) == 7 and all(row["type"] == 1 for row in table_rows)
        assert {row["source_section_offset"] for row in table_rows} == set(range(0x54, 0x70, 4))
        assert struct.unpack_from(">II", raw, sections[5]["file_offset"] + 0x4C) == (0, 0)
        local_methods = [row for row in module["relocations"]["internal"]
                         if row["source_section"] == 5 and 0x54 <= row["source_section_offset"] < 0x70
                         and row["type"] == 1 and row["index"] == 1]
        assert len(local_methods) == 2 and {row["target_offset"] for row in local_methods} == {0x9EC, 0xB0C}
        roots += [sections[1]["nominal"] + row["target_offset"] for row in local_methods]
    if name in ("Friendly", "GunTurret", "MapScreenWebInterface"):
        # First playable scene: native loader calls Friendly's actual prolog.
        # These original relocation-backed tables contain virtual/member code
        # entries. Friendly's two eight-way switch tables are excluded: their
        # destinations are labels in a body, never callable function roots.
        spec = json.loads((ROOT / "recomp/config/RM3E01_00/rso_scene_indirect_roots.json").read_text())[name]
        rows = [[r["source_section_offset"], r["target_offset"]]
                for r in module["relocations"]["internal"]
                if r["source_section"] == 5 and r["type"] == 1 and r["index"] == 1]
        assert sorted(rows) == spec["all_data_code_relocations"]
        excluded = set(spec["switch_table_sources"])
        if name == "Friendly":
            assert excluded == set(range(0x428, 0x468, 4))
            for offset in (0x514C, 0x52B8):
                assert struct.unpack_from(">I", raw, sections[1]["file_offset"] + offset)[0] == 0x28000007
            for offset in (0x5160, 0x52CC):
                assert struct.unpack_from(">3I", raw, sections[1]["file_offset"] + offset) == (0x7C63002E, 0x7C6903A6, 0x4E800420)
            switch_refs = sorted([r["source_section_offset"], r["type"], r["target_offset"]]
                for r in module["relocations"]["internal"] if r["source_section"] == 1
                and r["index"] == 5 and r["target_offset"] in (0x428, 0x448))
            assert switch_refs == [[0x5156,6,0x448],[0x515E,4,0x448],[0x52C2,6,0x428],[0x52CA,4,0x428]]
        elif name == "MapScreenWebInterface":
            assert excluded == set(range(0x124,0x1AC,4))
            assert struct.unpack_from(">5I",raw,sections[1]["file_offset"]+0x5318) == (
                0x5460103A,0x38840000,0x7C84002E,0x7C8903A6,0x4E800420)
            refs=sorted((r["source_section_offset"],r["type"]) for r in module["relocations"]["internal"]
                if r["source_section"]==1 and r["index"]==5 and r["target_offset"]==0x124)
            assert refs == [(0x5316,6),(0x531E,4)]
        else:
            assert not excluded
        for source, target in rows:
            if source not in excluded:
                roots.append(sections[1]["nominal"] + target)
    # Explicit, reviewed address-taken code roots. Never promote arbitrary
    # data words or switch labels to callable functions. The original module
    # hash is pinned above; every listed relocation and entry word is checked.
    callbacks=json.loads((ROOT/"recomp/config/RM3E01_00/rso_address_taken_roots.json").read_text())["modules"].get(name,[])
    for callback in callbacks:
        offset=callback["text_offset"]
        assert offset%4==0 and 0<=offset<=sections[1]["size"]-16
        references=sorted([[r["source_section_offset"],r["type"]] for r in module["relocations"]["internal"]
            if r["source_section"]==1 and r["index"]==1 and r["target_offset"]==offset and r["type"] in (4,6)])
        assert references==callback["references"] and {r[1] for r in references}=={4,6}
        assert list(struct.unpack_from(">4I",raw,sections[1]["file_offset"]+offset))==callback["first_words"]
        roots.append(sections[1]["nominal"]+offset)
    # Every relocated function entry of the module (see discover_prime_rso_roots):
    # the reviewed roots above are a subset, kept as assertions.
    discovered = discover(name)
    discovered_roots = {sections[1]["nominal"] + row["text_offset"] for row in discovered["roots"]}
    assert set(roots) <= discovered_roots, sorted(hex(r) for r in set(roots) - discovered_roots)
    roots += sorted(discovered_roots - set(roots))
    boundaries.update(roots)
    base_metadata = json.loads((ROOT / "recomp/generated/RM3E01_00_conservative/base_translation_output.json").read_text())
    boundaries.update(row["entryPoint"] for row in base_metadata["functions"])
    manifest = {"dol": str(ROOT / "recomp/input/RM3E01_00/main.dol"), "rel": str(rel_path),
                "base": base, "moduleId": module_id, "textStart": sections[1]["nominal"],
                "textEnd": sections[1]["nominal"] + sections[1]["size"],
                "roots": sorted(set(roots)), "boundaries": sorted(boundaries), "sites": sites,
                "jumpTables": [{"bctr": sections[1]["nominal"] + table["bctr"],
                                "targets": [sections[1]["nominal"] + t for t in table["targets"]]}
                               for table in discovered["jump_tables"]]}
    (directory / "input.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (directory / "module.json").write_text(json.dumps({"module": name, "module_id": module_id,
        "original_sha256": module["sha256"], "sections": sections, "constructor_entries": constructors,
        "original_imports": module["imports"], "relocations": module["relocations"],
        "entry_points": module["entry_points"], "nominal_image_base": base,
        "provenance": "Nominal section layout from the already verified synthetic REL; runtime uses SDK section addresses."}, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"manifest": str(directory / "input.json"), "roots": len(set(roots)), "relocation_sites": len(sites)}, indent=2))


if __name__ == "__main__":
    main()
