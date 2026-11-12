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
        0x804F6A2C: "9421ff70",  # WPADGetInfoAsync: full-battery answer (wiimote_battery_full)
        0x804D9E20: "9421ffe0",  # 56-byte raw WPAD history reader, void
        0x804D9138: "9421ff40",  # 132-byte processed input sample reader
        0x8017A4B4: "9421ffc0",  # CPlayer pointer turn factor (mouse look post-hook)
        0x801799A8: "9421fde0",  # CPlayer ground/air movement (mouse look post-hook: yaw rate)
        0x801522E0: "9421ff30",  # CPlayer free-look pitch (mouse look post-hook)
        0x80151F64: "9421ff50",  # CPlayer free-look easing toward the target (mouse look post-hook)
        0x8036BC98: "9421fff0",  # smoothing filter step (raw pointer post-hook)
        0x8007F904: "9421f8b0",  # first-person camera think (mouse look: exact look direction)
        # Modern lighting (hle/prime_engine_lights.h, docs/iluminacion_motor.md).
        0x801ACAB8: "9421fe60",  # the frame's dynamic light lists (post-hook: send the engine's lights)
        0x8000C918: "9421ffb0",  # CActorLights::ActivateLights (character light fading, restored after)
        # First-person FOV (hle/prime_fov.h, after PrimeHack's view modifier).
        0x8009CF90: "9421ffe0",  # CGameCamera set FOV: three callers skipped while the FOV option is on
        0x800DD87C: "4bfbf715",  # bl 0x8009CF90 (skipped)
        0x800DCEB0: "4bfc00e1",  # bl 0x8009CF90 (skipped)
        0x8017D40C: "4bf1fb85",  # bl 0x8009CF90 (skipped)
        0x8007F2A4: "9421ffa0",  # camera FOV easing
        0x8007F504: "4bfffba9",  # bl 0x8007F0AC: easing's store of the current FOV (inlined leaf)
        0x802B2668: "9421ffc0",  # camera manager map set
        0x802B26C0: "90060014",  # stw r0,0x14(r6): the active camera id changes
        0x80316A1C: "c003000c",  # box-against-plane culling test
        # Ultrawide Hor+ (hle/prime_widescreen.h, GitHub issue #7).
        0x800BE034: "9421ffb0",  # viewport aspect from width/height (post-hook: widened)
        0x800BF0C4: "9421fff0",  # viewport aspect update (entry: remember the viewport)
        0x803EB060: "9421ff70",  # projection state to GX (before the frustum/ortho matrix)
        0x803E91D4: "9421ffa0",  # CGraphics::SetPerspective (entry: GUI camera aspect widened)
        # Game language (prime_host_options.cpp): the choice between English and the console language.
        0x802F1378: "9421fff0",  # language index prologue
        0x802F13A4: "880ddd40",  # lbz r0,-0x22C0(r13): PAL/EuRGB60 video flag that opens the console language
        # Motion prompts (hle/prime_motion_text.cpp): CStringTable::GetString hands its UTF-8 text to the char view.
        0x803CDB38: "9421ffe0",  # GetString prologue
        # Photo mode (hle/prime_photo_mode.h): the per-frame game update, called once by the main loop.
        0x802EBAF4: "9421ffa0",  # game update prologue
        0x802EDFCC: "4bffdb29",  # bl 0x802EBAF4 (main loop)
        0x802EDFD0: "2c030000",  # cmpwi r3,0: a zero result takes the loop's stop path
        # End-of-scene filler (called by 0x803E9F78 right before the display copy): 96 identical triangles.
        0x803E9B28: "38a00120",  # li r5,288: the vertex count
        0x803E9B48: "c002cc84",  # lfs f0,1.0: the coordinates the overlay turns to zero

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
        # Quit to the title screen (CMain reset, 0x802ED6C4): the DVD reset
        # callback sets a stack flag the loop at 0x802EDC00 spins on, then
        # OSRestart (or OSReturnToMenu) reboots the console.
        0x802EDC00: "80010008",  # lwz r0,8(r1): the flag
        0x804E57C4: "9421fff0",  # OSRestart prologue
        0x804E5850: "9421ffd0",  # OSReturnToMenu prologue
        # Data-cache range ops (cmplwi r4,0 prologue): dcbf+sc, dcbst+sc, dcbf,
        # dcbz. Translated dcb* are no-ops; the overlay notifies the GX caches of
        # the range the CPU wrote. DCInvalidateRange (0x804DF8C8, dcbi) is not
        # hooked: it follows a DMA, including the GPU's own EFB copy, so treating
        # it as a CPU write destroyed the character shadow copy (floor flicker);
        # DVD and locked-cache DMA notify on their own. Its overlay only waits for
        # an AX mix still writing that range (the movie mixer reads AX output).
        0x804DF8C8: "28040000",  # DCInvalidateRange
        0x804DF8F4: "28040000",  # DCFlushRange
        0x804DF924: "28040000",  # DCStoreRange
        0x804DF954: "28040000",  # DCFlushRangeNoSync
        0x804DF980: "28040000",  # DCZeroRange
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
        if address not in (0x803BBF38, 0x8054A6D8, 0x805013D4, 0x80259474, 0x80259978, 0x802598B0, 0x802599FC, 0x804EE4A8, 0x804EB174, 0x80353F70, 0x803540A8, 0x804F66B8, 0x804F6A2C, 0x804D9138, 0x804D9E20, 0x804E6BD4, 0x804E01C4, 0x804D2644, 0x8050D4A0, 0x804D9850, 0x804E6E14,
                           0x802ED6C4, 0x804E57C4, 0x804E5850,
                           0x804DF8C8, 0x804DF8F4, 0x804DF924, 0x804DF954, 0x804DF980, 0x8017A4B4, 0x801799A8, 0x801522E0, 0x80151F64, 0x8036BC98, 0x8007F904,
                           0x8009CF90, 0x8007F2A4, 0x802B2668, 0x80316A1C, 0x802EBAF4, 0x803E9A34, 0x802F1378, 0x803CDB38,
                           0x801ACAB8, 0x8000C918, 0x800BE034, 0x800BF0C4, 0x803EB060, 0x803E91D4):
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
                # Ultrawide: pre-rendered movies are presented at 16:9 while frames are decoded.
                source=replace_once(source,'#include "hle/prime_video_capture.h"',
                    '#include "hle/prime_video_capture.h"\nvoid PrimeWidescreenNoteVideoFrame();')
                source=replace_once(source,anchor,anchor+'\n    PrimeWidescreenNoteVideoFrame();')
                source=replace_once(source,'    PrimeTraceVideo(ctx,0x804EB174u,true);',
                    '    frameCapture.Finish(ctx);\n    PrimeTraceVideo(ctx,0x804EB174u,true);')
        elif address in (0x804DF8F4,0x804DF924,0x804DF954,0x804DF980):
            # Notify on every return, after DCZeroRange has zeroed the blocks:
            # GX texture/TLUT/display-list caches trust write generations.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" void PrimeNotifyGuestDcRange(uint32_t address, uint32_t length);')
            anchor=f'extern "C" void func_{address:08X}(CpuContext* MKW_RESTRICT ctx)\n{{'
            source=replace_once(source,anchor,anchor+'\n'
                '    struct PrimeDcRangeNotify {\n'
                '        uint32_t address, length;\n'
                '        ~PrimeDcRangeNotify() { PrimeNotifyGuestDcRange(address, length); }\n'
                '    } primeDcRangeNotify{static_cast<uint32_t>(ctx->gpr[3]), static_cast<uint32_t>(ctx->gpr[4])};')
        elif address == 0x802ED6C4:
            # The loop has no call, so no call-boundary poll ever delivers the
            # IPC reply whose DVD callback ends it; on the console that interrupt
            # preempts the spin. Service interrupts on every pass, like the
            # Bluetooth/WPAD init loops below.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" void OS_HLE_ServicePrimeIdle(CpuContext*, uint32_t, uint32_t);')
            source=replace_once(source,'loc_802EDC00:\n{',
                'loc_802EDC00:\n{\n    OS_HLE_ServicePrimeIdle(ctx, r1, ctx->gpr[13]);')
        elif address in (0x804E57C4,0x804E5850):
            # The console reboots the disc (OSRestart) or goes back to the Wii
            # Menu (OSReturnToMenu); prime_host_restart.cpp relaunches or closes
            # the program instead. Neither returns when it acts.
            helper={0x804E57C4:'PrimeHostOsRestart',0x804E5850:'PrimeHostOsReturnToMenu'}[address]
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'+f'extern "C" bool {helper}(CpuContext*);')
            anchor=f'extern "C" void func_{address:08X}(CpuContext* MKW_RESTRICT ctx)\n{{'
            source=replace_once(source,anchor,anchor+f'\n    if ({helper}(ctx)) return;')
        elif address == 0x804DF8C8:
            # DCInvalidateRange is where the CPU starts reading what a DMA or the
            # DSP wrote. The movie player's AI callback (0x80354D34) invalidates
            # and reads the AX output buffer that __AXOutNewFrame just handed to
            # the DSP; the DSP finishes it during the sound frame work in between,
            # the mix worker may not. Wait for that mix only; GX is not notified.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" void PrimeBeforeGuestDcInvalidate(uint32_t address, uint32_t length);')
            anchor=f'extern "C" void func_{address:08X}(CpuContext* MKW_RESTRICT ctx)\n{{'
            source=replace_once(source,anchor,anchor+'\n'
                '    PrimeBeforeGuestDcInvalidate(static_cast<uint32_t>(ctx->gpr[3]), static_cast<uint32_t>(ctx->gpr[4]));')
        elif address in (0x804F66B8,0x804F6A2C,0x804D9138,0x804D9E20):
            helper={0x804F66B8:'PrimeInputTryProbe',0x804F6A2C:'PrimeInputTryGetInfoAsync',0x804D9138:'PrimeInputTryReadKpad',0x804D9E20:'PrimeInputTryReadUnifiedWpad'}[address]
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'+f'extern "C" bool {helper}(CpuContext*);')
            anchor=f'extern "C" void func_{address:08X}(CpuContext* MKW_RESTRICT ctx)\n{{'
            source=replace_once(source,anchor,anchor+f'\n    if ({helper}(ctx)) return;')
        elif address == 0x803CDB38:
            # A Wii Remote/Nunchuk motion prompt is swapped for one naming the bound key or pad input
            # right before the borrowed char view (0x803FC164) is built from it; the game then converts
            # and owns that text as usual (hle/prime_motion_text.cpp).
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\nextern "C" uint32_t PrimeMotionTextOverride(uint32_t);')
            source=replace_once(source,'    InvokeDirectCpu<0x803FC164u>(ctx);\n',
                '    ctx->gpr[4] = PrimeMotionTextOverride(static_cast<uint32_t>(ctx->gpr[4]));\n'
                '    InvokeDirectCpu<0x803FC164u>(ctx);\n')
        elif address == 0x802F1378:
            # Language: with a non-English choice the NTSC gate reads as PAL, so the game
            # uses its own console-language path (index from SCGetLanguage, Dutch -> English).
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\nextern "C" bool PrimeGameLanguageUnlocked();')
            source=replace_once(source,'    r0 = MemoryInline::FlatRead8((r13 + -8896));',
                '    r0 = PrimeGameLanguageUnlocked() ? 1u : MemoryInline::FlatRead8((r13 + -8896));')
        elif address in (0x8017A4B4,0x801799A8,0x801522E0,0x80151F64):
            # Mouse look (hle/prime_mouse_look.h): after the original body ran, the
            # helper replaces the turn factor in f1 / the yaw rate / the pitch in
            # CPlayer +0x780 / the eased current pitch in +0x77C.
            helper={0x8017A4B4:'PrimeMouseLookAfterTurn',0x801799A8:'PrimeMouseLookAfterMove',0x801522E0:'PrimeMouseLookAfterPitch',0x80151F64:'PrimeMouseLookAfterFreeLookEase'}[address]
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'+f'extern "C" void {helper}(CpuContext*, uint32_t, uint32_t, uint32_t);')
            anchor=f'extern "C" void func_{address:08X}(CpuContext* MKW_RESTRICT ctx)\n{{'
            source=replace_once(source,anchor,anchor+'\n'
                '    struct PrimeMouseLookAfter {\n'
                '        CpuContext* ctx; uint32_t player, input, manager;\n'
                f'        ~PrimeMouseLookAfter() {{ {helper}(ctx, player, input, manager); }}\n'
                '    } primeMouseLookAfter{ctx, static_cast<uint32_t>(ctx->gpr[3]), static_cast<uint32_t>(ctx->gpr[4]), static_cast<uint32_t>(ctx->gpr[5])};')
        elif address == 0x8007F904:
            # Mouse look: the first-person camera turns toward the free-look
            # direction with a maximum angle per frame (dt * CTweakPlayer +0x3BC
            # deg/s; the quaternion LookAt of the pitch leg). While the mouse
            # drives the look, that limit is lifted for this call only so the
            # camera and gun show the exact pitch, as yaw already does
            # (its LookAt limit is 360 degrees). Arguments: r3 camera, r4 manager.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" bool PrimeMouseLookCameraBegin(CpuContext*, uint32_t, uint32_t, uint32_t*);\n'
                'extern "C" void PrimeMouseLookCameraEnd(uint32_t);')
            anchor='extern "C" void func_8007F904(CpuContext* MKW_RESTRICT ctx)\n{'
            source=replace_once(source,anchor,anchor+'\n'
                '    struct PrimeMouseLookCamera {\n'
                '        uint32_t saved = 0; bool patched = false;\n'
                '        ~PrimeMouseLookCamera() { if (patched) PrimeMouseLookCameraEnd(saved); }\n'
                '    } primeMouseLookCamera;\n'
                '    primeMouseLookCamera.patched = PrimeMouseLookCameraBegin(ctx, static_cast<uint32_t>(ctx->gpr[3]), static_cast<uint32_t>(ctx->gpr[4]), &primeMouseLookCamera.saved);')
        elif address == 0x801ACAB8:
            # Modern lighting: once the frame's dynamic light lists are built (this +0x5D8 holds the
            # world's), the runtime sends them and the loaded areas' static lights to Aurora.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" void PrimeEngineLightsAfterWorldLights(uint32_t);')
            anchor='extern "C" void func_801ACAB8(CpuContext* MKW_RESTRICT ctx)\n{'
            source=replace_once(source,anchor,anchor+'\n'
                '    struct PrimeEngineLightsAfter {\n'
                '        uint32_t self;\n'
                '        ~PrimeEngineLightsAfter() { PrimeEngineLightsAfterWorldLights(self); }\n'
                '    } primeEngineLightsAfter{static_cast<uint32_t>(ctx->gpr[3])};')
        elif address == 0x8000C918:
            # Modern lighting: CActorLights::ActivateLights (r3 = the block) loads a character's lights
            # into GX. Begin fades the area lights that entered or left the engine's choice (writing
            # the block), End restores the block exactly after the original body.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" uint32_t PrimeActorLightsBegin(uint32_t);\n'
                'extern "C" void PrimeActorLightsEnd(uint32_t);')
            anchor='extern "C" void func_8000C918(CpuContext* MKW_RESTRICT ctx)\n{'
            source=replace_once(source,anchor,anchor+'\n'
                '    struct PrimeActorLightsCall {\n'
                '        uint32_t token;\n'
                '        ~PrimeActorLightsCall() { PrimeActorLightsEnd(token); }\n'
                '    } primeActorLightsCall{PrimeActorLightsBegin(static_cast<uint32_t>(ctx->gpr[3]))};')
        elif address == 0x8009CF90:
            # FOV option: PrimeHack nops three calls to this setter so the game
            # does not put its own FOV back; skip the body when called from them
            # (ctx->lr is the caller's return address at entry).
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" bool PrimeFovSkipCameraSetFov(uint32_t);')
            anchor='extern "C" void func_8009CF90(CpuContext* MKW_RESTRICT ctx)\n{'
            source=replace_once(source,anchor,anchor+'\n    if (PrimeFovSkipCameraSetFov(static_cast<uint32_t>(ctx->lr))) return;')
        elif address == 0x8007F2A4:
            # FOV option: the easing step's store of the current FOV (0x8007F504,
            # leaf 0x8007F0AC inlined) is skipped, as PrimeHack's nop does.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" bool PrimeFovSkipEasedFov();')
            source=replace_once(source,
                '    f1.d = static_cast<double>(PpcForceSingleValueInline(f0.d + f1.d));\n'
                '    // inline leaf 0x8007F0AC (2 guest instruction(s))\n'
                '    MemoryInline::FlatWriteFloat32((r3 + 24), f1.d);',
                '    f1.d = static_cast<double>(PpcForceSingleValueInline(f0.d + f1.d));\n'
                '    // inline leaf 0x8007F0AC (2 guest instruction(s))\n'
                '    if (!PrimeFovSkipEasedFov()) MemoryInline::FlatWriteFloat32((r3 + 24), f1.d);')
        elif address == 0x802B2668:
            # FOV option: the active camera changed (0x802B26C0); give the new
            # camera the FOV at once, as PrimeHack's vmcall there does.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" void PrimeFovOnCameraChange(uint32_t);')
            source=replace_once(source,
                '    MemoryInline::FlatWrite32((r6 + 20), r0);\n    goto loc_802B2708;',
                '    MemoryInline::FlatWrite32((r6 + 20), r0);\n    PrimeFovOnCameraChange(r0);\n    goto loc_802B2708;')
        elif address == 0x80316A1C:
            # FOV option: above 94 degrees (or on request) the culling test
            # passes everything, as PrimeHack's li r3,1; blr does.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" bool PrimeFovCullingBypassed();')
            anchor='extern "C" void func_80316A1C(CpuContext* MKW_RESTRICT ctx)\n{'
            source=replace_once(source,anchor,anchor+'\n    if (PrimeFovCullingBypassed()) { ctx->gpr[3] = 1; return; }')
        elif address == 0x800BE034:
            # Ultrawide: the viewport aspect the game computed for (w, h) =
            # (r3, r4) is widened for the full-screen viewport, so the frustum
            # planes (culling) and the projection matrix built from it agree.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" double PrimeWidescreenViewportAspect(double, uint32_t, uint32_t, uint32_t, uint32_t);')
            source=replace_once(source,'    uint32_t r4 = ctx->gpr[4];\n',
                '    uint32_t r4 = ctx->gpr[4];\n    const uint32_t primeUwWidth = r3, primeUwHeight = r4;\n')
            source=replace_once(source,
                '    ctx->fpr[0] = f0;\n    ctx->fpr[1] = f1;\n    ctx->fpr[29] = f29;',
                '    f1.d = PrimeWidescreenViewportAspect(f1.d, primeUwWidth, primeUwHeight, r0, r1);\n'
                '    ctx->fpr[0] = f0;\n    ctx->fpr[1] = f1;\n    ctx->fpr[29] = f29;')
        elif address == 0x800BF0C4:
            # Ultrawide: remember the viewports so a change of shape can mark them for update.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" void PrimeWidescreenNoteViewport(uint32_t);')
            anchor='extern "C" void func_800BF0C4(CpuContext* MKW_RESTRICT ctx)\n{'
            source=replace_once(source,anchor,anchor+'\n    PrimeWidescreenNoteViewport(static_cast<uint32_t>(ctx->gpr[3]));')
        elif address == 0x803E91D4:
            # Ultrawide: the GUI cameras (helmet HUD, menus; 0x80433BD8 and its
            # widescreen variant 0x80433CF8) pass their own aspect in f1. Widened
            # here, the game's projection state holds it, so the copies the game
            # saves and restores (0x80045DB8, 0x80048250) stay widened too.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" double PrimeWidescreenGuiAspect(double, uint32_t, uint32_t);')
            anchor='extern "C" void func_803E91D4(CpuContext* MKW_RESTRICT ctx)\n{'
            source=replace_once(source,anchor,anchor+'\n    ctx->fpr[1].d = PrimeWidescreenGuiAspect(ctx->fpr[1].d, static_cast<uint32_t>(ctx->lr), static_cast<uint32_t>(ctx->gpr[1]));')
        elif address == 0x803EB060:
            # Ultrawide: the frustum (f3 left, f4 right; f1 top, f2 bottom) handed
            # to C_MTXFrustum / C_MTXOrtho; the game's own state is left as is.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" void PrimeWidescreenFlush(uint32_t, uint32_t, double*, double*, double, double);')
            source=replace_once(source,
                '    ctx->fpr[6] = f6;\n    InvokeDirectCpu<0x80506538u>(ctx);',
                '    ctx->fpr[6] = f6;\n'
                '    PrimeWidescreenFlush(1u, r1, &ctx->fpr[3].d, &ctx->fpr[4].d, ctx->fpr[1].d, ctx->fpr[2].d);\n'
                '    InvokeDirectCpu<0x80506538u>(ctx);')
            source=replace_once(source,
                '    ctx->fpr[6] = f6;\n    InvokeDirectCpu<0x805066C0u>(ctx);',
                '    ctx->fpr[6] = f6;\n'
                '    PrimeWidescreenFlush(0u, r1, &ctx->fpr[3].d, &ctx->fpr[4].d, ctx->fpr[1].d, ctx->fpr[2].d);\n'
                '    InvokeDirectCpu<0x805066C0u>(ctx);')
        elif address == 0x802EBAF4:
            # Photo mode: skip the game update while frozen; the main loop still draws. r3 = 1 is
            # the update's "continue" result (0 sets the stop flag at +0x12C).
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" bool PrimePhotoModeSkipUpdate();')
            anchor='extern "C" void func_802EBAF4(CpuContext* MKW_RESTRICT ctx)\n{'
            source=replace_once(source,anchor,anchor+'\n    if (PrimePhotoModeSkipUpdate()) { ctx->gpr[3] = 1; return; }')
        elif address == 0x803E9A34:
            # End-of-scene filler: 96 triangles of half an EFB pixel at the screen centre, with
            # whatever TEV state the last draw left. Above 1x they rasterize into a dark dot
            # (recomp/docs/copias_efb_y_cache.md). Every 1.0 becomes 0.0: same GX state and vertex
            # stream, no area at any resolution.
            assert digest(original) == "5ec10200d7b28ee21b1fe540b3b1b037f7036f03b950e25f3e3749155fd3f21b"
            one='f0.d = MemoryInline::ReadResolvedFloat32(guest_range_0, 4u, (r2 + -13180));'
            start,end=source.index('loc_803E9B40:'),source.index('loc_803E9F64:')
            if source[start:end].count(one)!=40 or source.count('(r2 + -13180)')!=40:
                raise ValueError("Unexpected filler loop at 803E9B40")
            source=source[:start]+source[start:end].replace(one,'f0.d = 0.0;')+source[end:]
        elif address == 0x8036BC98:
            # Raw pointer (prime_pointer_filter.cpp): the filter's input (f1 at
            # entry) replaces its output for the game's two pointer filters.
            source=replace_once(source,'#include "guest_abi/guest_abi.h"',
                '#include "guest_abi/guest_abi.h"\n'
                'extern "C" void PrimePointerFilterAfter(CpuContext*, uint32_t, double);')
            anchor='extern "C" void func_8036BC98(CpuContext* MKW_RESTRICT ctx)\n{'
            source=replace_once(source,anchor,anchor+'\n'
                '    struct PrimePointerFilterGuard {\n'
                '        CpuContext* ctx; uint32_t self; double input;\n'
                '        ~PrimePointerFilterGuard() { PrimePointerFilterAfter(ctx, self, input); }\n'
                '    } primePointerFilterGuard{ctx, static_cast<uint32_t>(ctx->gpr[3]), ctx->fpr[1].d};')
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
    # Switches found by audit_prime_dol_switches.py: every guard-bounded entry
    # of the original table must now be a case of the retranslated body.
    audited = json.loads((ROOT / "recomp/config/RM3E01_00/dol_jump_tables.json").read_text())
    assert not {int(f, 16) for f in audited["repair_functions"]} & {entry for entry, _ in repair_tables}
    for function in audited["repair_functions"]:
        entry = int(function, 16)
        filename = f"func_{entry:08X}.cpp"
        repaired = json.loads((repair_dir / f"func_{entry:08X}.json").read_text())
        data = (repair_dir / repaired["relativePath"]).read_bytes()
        assert digest(data) == repaired["sha256"] and repaired["entryPoint"] == entry
        actual_cases = {int(value, 16) for value in re.findall(r"case 0x([0-9A-F]{8})u:", data.decode())}
        tables = [table for table in audited["tables"] if table["function"] == function]
        for table in tables:
            entries = [int.from_bytes(read_dol(dol, int(table["table"], 16) + 4 * i, 4), "big")
                       for i in range(len(table["targets"]))]
            assert entries == [int(target, 16) for target in table["targets"]]
            assert set(entries) <= actual_cases, (function, sorted(set(entries) - actual_cases))
        index = next(i for i, row in enumerate(metadata["functions"]) if row["entryPoint"] == entry)
        original = (BASE / "functions" / filename).read_bytes()
        accepted = audited.get("accepted_regenerated_bases", {}).get(function, {}).get("sha256")
        assert digest(original) in (metadata["functions"][index]["sha256"], accepted), f"{function} already has a runtime overlay"
        repaired["relativePath"] = f"runtime_hooks/functions/{filename}"
        metadata["functions"][index] = repaired
        target = out / "functions" / filename
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
        changes.append({"address": f"0x{entry:08X}", "raw_sha256": digest(original), "overlay_sha256": digest(data),
                        "kind": "DOL retranslated with guard-bounded switch tables (audit_prime_dol_switches.py)",
                        "dispatches": [table["bctr"] for table in tables]})
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
