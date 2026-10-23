#!/usr/bin/env python3
"""Build a reproducible RM3E01 runtime overlay; never edit the raw translation."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

from verify_prime_context_save_binding import read_dol

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / "recomp/generated/RM3E01_00_conservative"
PIN = "6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def replace_once(source: str, old: str, new: str) -> str:
    if source.count(old) != 1:
        raise ValueError(f"Runtime hook anchor must occur exactly once: {old!r}")
    return source.replace(old, new, 1)


def main() -> None:
    dol = (ROOT / "recomp/input/RM3E01_00/main.dol").read_bytes()
    if digest(dol) != PIN:
        raise ValueError("Refusing a different game revision")
    expected = {
        0x803BBF38: "9421ff70",  # Read-only resource stream diagnostics
        0x8054A6D8: "9421ff90",  # Original RFL official-profile validation, observation only
        0x805013D4: "9421ffe0",  # IOS usage request, observation only
        0x80259474: "9421ffe0",  # Script movie message handler, observation only
        0x80259978: "9421fff0",  # Original play request
        0x802598B0: "9421fff0",  # Original preload request
        0x802599FC: "80830190",  # Original pause request
        0x804EE4A8: "9421fff0",  # Original THP initializer, optional observation only
        0x804EB174: "9421ffe0",  # Original video decoder
        0x80353F70: "9421efb0",  # Original movie decode dispatcher
        0x803540A8: "9421ffa0",  # Original movie update path

        0x804F66B8: "9421fff0",  # WPADProbe, independently audited Prime region
        0x804D9E20: "9421ffe0",  # 56-byte raw WPAD history reader, void
        0x804D9138: "9421ff40",  # 132-byte processed input sample reader

        0x804E6CD0: "4bff9475",  # SelectThread: bl OSSaveContext
        0x804E6CD4: "2c030000",  # Restored SRR0: cmpwi r3,0
        0x804E6CD8: "4182000c",  # Zero continues selection; resumed path returns.
        0x804E6D1C: "4bffcfb5",  # bl OSEnableInterrupts
        0x804E6D20: "800de338",  # idle: lwz r0,runQueueBits(r13)
        0x804E6D28: "4182fff8",  # beq idle
        0x804E0298: "4c000064",  # OSLoadContext: rfi
        0x804D2644: "9421fff0",  # GXDrawDone prologue
        0x8050D558: "48003065",  # Bluetooth init polls its own state.
        0x8050D560: "4182fff0",  # while not ready
        0x804D9898: "4801bb11",  # WPADInit polls WUD status
        0x804D98A0: "4082fff8",  # until status == 3
    }
    for address, encoded in expected.items():
        if read_dol(dol, address, 4).hex() != encoded:
            raise ValueError(f"Unexpected original instruction at {address:08X}")
    metadata = json.loads((BASE / "base_translation_output.json").read_text(encoding="utf-8"))
    metadata["sourceBundlePath"] = None
    out = BASE / "runtime_hooks"
    (out / "functions").mkdir(parents=True, exist_ok=True)
    changes = []
    for item in metadata["functions"]:
        address = item["entryPoint"]
        raw_relative_path = item["relativePath"]
        item["relativePath"] = "functions/" + raw_relative_path
        if address not in (0x803BBF38, 0x8054A6D8, 0x805013D4, 0x80259474, 0x80259978, 0x802598B0, 0x802599FC, 0x804EE4A8, 0x804EB174, 0x80353F70, 0x803540A8, 0x804F66B8, 0x804D9138, 0x804D9E20, 0x804E6BD4, 0x804E01C4, 0x804D2644, 0x8050D4A0, 0x804D9850, 0x804E6E14):
            continue
        original = (BASE / "functions" / raw_relative_path).read_bytes()
        if digest(original) != item["sha256"]:
            raise ValueError(f"Raw translation hash mismatch at {address:08X}")
        source = original.decode("utf-8").replace("\r\n", "\n")
        if address == 0x803BBF38:
            assert digest(original) == "c2ad19826a2db3c04855930700c410015c86e65c6b1d11b8cd8b5c9e9a404cbd"
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n#include "hle/prime_resource_trace.h"')
            anchor='extern "C" void func_803BBF38(CpuContext* MKW_RESTRICT ctx)\n{'
            source=replace_once(source,anchor,anchor+'\n    PrimeTraceResourceStream(0,ctx->gpr[3],0,0,0,0);')
            anchor='    InvokeDirectCpu<0x8037A1A4u>(ctx);'
            source=replace_once(source,anchor,
                '    PrimeTraceResourceStream(1,r30,r3,r4,r5,MemoryInline::FlatRead32(r6));\n'+anchor+
                '\n    PrimeTraceResourceStream(2,r30,ctx->gpr[3],ctx->gpr[4],ctx->gpr[5],MemoryInline::FlatRead32(ctx->gpr[6]));')
        elif address == 0x8054A6D8:
            assert digest(original) == "076c760f86a8f8ec4d8b92d91a04961997b46a7c8ec1fe485d079f5812ce4435"
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n#include "hle/prime_mii_trace.h"')
            anchor='extern "C" void func_8054A6D8(CpuContext* MKW_RESTRICT ctx)\n{'
            source=replace_once(source,anchor,anchor+'\n    const uint32_t prime_mii_index = ctx->gpr[3];')
            source=replace_once(source,'    return;',
                '    PrimeTraceMiiLookup(prime_mii_index,ctx->gpr[3]);\n    return;')
        elif address == 0x805013D4:
            assert digest(original) == "ccce60fbd481bc9be609c918fcfe106ab9ecf754da6930f40caafc3ccd893c35"
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n#include "hle/prime_nand_trace.h"')
            anchor='extern "C" void func_805013D4(CpuContext* MKW_RESTRICT ctx)\n{'
            source=replace_once(source,anchor,anchor+'\n    PrimeTraceNandUsage(0,ctx->gpr[3],0,64);')
            source=replace_once(source,'    InvokeDirectCpu<0x80004000u>(ctx);',
                '    PrimeTraceNandUsage(1,r4,r3,r5);\n    InvokeDirectCpu<0x80004000u>(ctx);\n    PrimeTraceNandUsage(2,r30,r29,r31+1);')
        elif address in (0x80259474,0x80259978,0x802598B0,0x802599FC):
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n#include "hle/prime_video_trace.h"')
            anchor=f'extern "C" void func_{address:08X}(CpuContext* MKW_RESTRICT ctx)\n{{'
            source=replace_once(source,anchor,anchor+f'\n    PrimeTraceMovieControl(ctx,0x{address:08X}u);')
        elif address in (0x804EE4A8,0x804EB174,0x80353F70,0x803540A8):
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n#include "hle/prime_video_trace.h"')
            anchor=f'extern "C" void func_{address:08X}(CpuContext* MKW_RESTRICT ctx)\n{{'
            source=replace_once(source,anchor,anchor+f'\n    PrimeTraceVideo(ctx,0x{address:08X}u,false);')
            source=replace_once(source,'    return;',f'    PrimeTraceVideo(ctx,0x{address:08X}u,true);\n    return;')
            if address==0x804EB174:
                source=replace_once(source,'#include "hle/prime_video_trace.h"',
                    '#include "hle/prime_video_trace.h"\n#include "hle/prime_video_capture.h"')
                source=replace_once(source,anchor,anchor+'\n    const PrimeVideoFrameCapture frameCapture(ctx);')
                source=replace_once(source,'    PrimeTraceVideo(ctx,0x804EB174u,true);',
                    '    frameCapture.Finish(ctx);\n    PrimeTraceVideo(ctx,0x804EB174u,true);')
        elif address in (0x804F66B8,0x804D9138,0x804D9E20):
            helper={0x804F66B8:'PrimeInputTryProbe',0x804D9138:'PrimeInputTryReadKpad',0x804D9E20:'PrimeInputTryReadUnifiedWpad'}[address]
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'+f'extern "C" bool {helper}(CpuContext*);')
            anchor=f'extern "C" void func_{address:08X}(CpuContext* MKW_RESTRICT ctx)\n{{'
            source=replace_once(source,anchor,anchor+f'\n    if ({helper}(ctx)) return;')
        elif address == 0x804E6BD4:
            source = replace_once(source, '#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" bool OS_HLE_ShouldDeferPrimeScheduler();\n'
                'extern "C" void OS_HLE_ServicePrimeIdle(CpuContext*, uint32_t, uint32_t);')
            source = replace_once(source,
                'extern "C" void func_804E6BD4(CpuContext* MKW_RESTRICT ctx)\n{',
                'extern "C" void func_804E6BD4(CpuContext* MKW_RESTRICT ctx)\n{\n'
                '    if (OS_HLE_ShouldDeferPrimeScheduler()) return;')
            source = replace_once(source, 'loc_804E6D20:\n{',
                'loc_804E6D20:\n{\n    OS_HLE_ServicePrimeIdle(ctx, r1, r13);')
            source = replace_once(source,
                '    InvokeDirectCpu<0x804E0144u>(ctx);\n    r0 = ctx->gpr[0];\n    r3 = ctx->gpr[3];\n',
                '    InvokeDirectCpu<0x804E0144u>(ctx);\n    r0 = ctx->gpr[0];\n    r3 = ctx->gpr[3];\n'
                'prime_saved_context_resume:\n')
            source = replace_once(source,
                '    r3 = r30;\n}\n\nloc_804E6DE4:',
                '    goto prime_saved_context_resume; // Original SRR0 = 0x804E6CD4.\n}\n\nloc_804E6DE4:')
        elif address == 0x804E01C4:
            source = replace_once(source, '#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" void OS_HLE_TracePrimeContextLoad(CpuContext*);\n'
                'extern "C" void OS_HLE_PrimeTransferContext(CpuContext*);\n'
                'extern "C" bool OS_HLE_ShouldDeferPrimeScheduler();')
            source = replace_once(source,
                'extern "C" void func_804E01C4(CpuContext* MKW_RESTRICT ctx)\n{',
                'extern "C" void func_804E01C4(CpuContext* MKW_RESTRICT ctx)\n{\n'
                '    if (OS_HLE_ShouldDeferPrimeScheduler()) return;\n'
                '    OS_HLE_PrimeTransferContext(ctx);\n'
                '    OS_HLE_TracePrimeContextLoad(ctx);')
            # The host continuation transport returns on the source stack only.
            # Restore MSR before SelectThread resumes its OSSaveContext branch.
            source = replace_once(source, '    ctx->xer = xer;\n    return;',
                '    ctx->xer = xer;\n    ctx->msr = ctx->srr1; // rfi MSR restoration\n    return;')
        elif address == 0x804E6E14:
            # OSYieldThread must remain an interruptible scheduling boundary even
            # when the original scheduler immediately selects the same thread.
            source = replace_once(source, '#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" void VI_HLE_PollRetraceGuestSpin(CpuContext*);')
            source = replace_once(source,
                'extern "C" void func_804E6E14(CpuContext* MKW_RESTRICT ctx)\n{',
                'extern "C" void func_804E6E14(CpuContext* MKW_RESTRICT ctx)\n{\n'
                '    VI_HLE_PollRetraceGuestSpin(ctx);')
        elif address == 0x804D2644:
            # Platform seam: Aurora consumes the FIFO and completes its own
            # blocking draw-done operation instead of waiting on a Wii PE IRQ.
            marker = source[source.index('// RECOMP_GUEST_ABI'):]
            source = ('#include <cstdint>\n#include "guest_abi/guest_abi.h"\n'
                'extern "C" void GX_HLE_PrimeDrawDone(CpuContext*);\n'
                'extern "C" void func_804D2644(CpuContext* MKW_RESTRICT ctx)\n'
                '{\n    GX_HLE_PrimeDrawDone(ctx);\n}\n\n' + marker)
            item["build"]["directCallDependencies"] = []
            item["build"]["estimatedIrInstructions"] = 16
        else:
            source = replace_once(source, '#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" void OS_HLE_ServicePrimeIdle(CpuContext*, uint32_t, uint32_t);')
            label = 'loc_8050D558' if address == 0x8050D4A0 else 'loc_804D9898'
            source = replace_once(source, label + ':\n{',
                label + ':\n{\n    OS_HLE_ServicePrimeIdle(ctx, r1, ctx->gpr[13]);')
        data = source.encode("utf-8")
        filename = f"func_{address:08X}.cpp"
        target = out / "functions" / filename
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
        changes.append({"address": f"0x{address:08X}", "raw_sha256": digest(original),
                        "overlay_sha256": digest(data)})
        item["relativePath"] = f"runtime_hooks/functions/{filename}"
        item["sha256"] = digest(data)
        item["size"] = len(data)
    # A decoder repair retranslates the original body; it is not an HLE stub.
    repair_dir = ROOT / "recomp/.tools/dol-runtime-repairs"
    repair_tables = (
        (0x800E01F0, ((0x80591910,12,9),)),
        (0x80420444, ((0x8059F1D0,11,11),)),
        (0x8019804C, ((0x80594610,13,6),)),
        (0x8042B530, tuple((table,11,11) for table in
            (0x8059F930,0x8059F904,0x8059F8D8,0x8059F8AC,0x8059F880))),
        (0x80423190, ((0x8059F34C,11,11),(0x8059F324,10,9),(0x8059F2F8,11,11))),
        (0x80423EF8, ((0x8059F3CC,11,11),(0x8059F3A4,10,9),(0x8059F378,11,11))),
        (0x8040D9D8, ((0x8059EB5C,11,11),)),
        (0x8040F98C, ((0x8059EC8C,11,11),)),
        (0x80414E20, ((0x8059EDE4,11,11),(0x8059EDB8,11,11))),
        (0x80415CB8, tuple((table,11,11) for table in
            (0x8059EE94,0x8059EE68,0x8059EE3C,0x8059EE10))),
        (0x804167CC, ((0x8059EED0,11,11),)),
        (0x80419848, ((0x8059F004,11,11),(0x8059EFD8,11,11))),
        (0x80425B10, ((0x8059F5C0,11,11),(0x8059F594,11,11),(0x8059F568,11,11))),
        (0x80427A34, ((0x8059F69C,11,11),)),
        (0x804280AC, ((0x8059F6F4,11,11),)),
    )
    for entry, tables in repair_tables:
        filename = f"func_{entry:08X}.cpp"
        repaired = json.loads((repair_dir / f"func_{entry:08X}.json").read_text())
        data = (repair_dir / repaired["relativePath"]).read_bytes()
        assert digest(data) == repaired["sha256"] and repaired["entryPoint"] == entry
        table_reports = []
        expected_cases = set()
        switch_bodies = re.findall(r"switch \(static_cast<uint32_t>\(ctr\)\) \{(.*?)default:",data.decode(),re.S)
        assert len(switch_bodies) == len(tables)
        for (table,count,unique), body in zip(tables,switch_bodies):
            table_values = [int.from_bytes(read_dol(dol, table + index * 4, 4), "big") for index in range(count)]
            cases = set(table_values)
            assert len(cases) == unique
            assert {int(value,16) for value in re.findall(r"case 0x([0-9A-F]{8})u:",body)} == cases
            expected_cases.update(cases)
            table_reports.append({"original_table_address":f"0x{table:08X}","original_table_entries":table_values,
                                  "original_table_cases":sorted(cases)})
        actual_cases = {int(value, 16) for value in re.findall(r"case 0x([0-9A-F]{8})u:", data.decode())}
        assert actual_cases == expected_cases
        index = next(i for i, row in enumerate(metadata["functions"]) if row["entryPoint"] == entry)
        original = (BASE / "functions" / filename).read_bytes()
        assert digest(original) == metadata["functions"][index]["sha256"]
        repaired["relativePath"] = f"runtime_hooks/functions/{filename}"
        metadata["functions"][index] = repaired
        target = out / "functions" / filename
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
        changes.append({"address": f"0x{entry:08X}", "raw_sha256": digest(original), "overlay_sha256": digest(data),
                        "kind": "DOL retranslated after scaled switch-selector analysis repair",
                        **(table_reports[0] if len(table_reports)==1 else {"original_tables":table_reports})})
    metadata_text = json.dumps(metadata, indent=2) + "\n"
    metadata_path = out / "base_translation_output.json"
    if not metadata_path.exists() or metadata_path.read_text(encoding="utf-8") != metadata_text:
        metadata_path.write_text(metadata_text, encoding="utf-8")
    report = {"revision": "RM3E01_00", "dol_sha256": PIN, "overlays": changes,
              "limitation": "Experimental Windows host continuations; thread cancellation/reuse, general preemption and complete FPU context semantics remain unverified."}
    (out / "manifest.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
