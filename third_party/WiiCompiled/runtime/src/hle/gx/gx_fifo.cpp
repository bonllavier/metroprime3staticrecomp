#include "gx_internal.h"
#include <aurora/aurora.h>
#include "gx_stream_common.h"
#include "gx_cp_decode.h"
#include "isa/big_endian.h"
#include "runtime_log.h"
#include "recomp_mod_loader.h"
#include <cstdlib>
#include <cstdio>
#include <sstream>
#include "hle/gx_fifo_storage.h"
#include "hle/prime_raw_bp.h"
#include "hle/prime_performance.h"
#include "hle/prime_fifo_burst.h"
#include "hle/prime_tlut_memory.h"
#include "hle/gx_gpu_thread.h"
#include "hle/prime_gx_stream.h"
#include "hle/prime_input_latency.h"

// Diagnostic environment switches are fixed for the lifetime of this process.
static bool CachedWiiCompiledTracePrimeMiiFifo() {
  static const bool enabled = std::getenv("WII_COMPILED_TRACE_PRIME_MII") != nullptr;
  return enabled;
}


// Opcode constants and the stream helpers this file shares with gx_dl.cpp /
// gx_vertex.cpp; see gx_stream_common.h.
using namespace GxCmd;
using namespace GxStream;

HleGxState g_hleGxState;
#if defined(MKW_GAME_PROFILE_RM3E01_00)
extern "C" bool PrimeGpuCpReadBlocked();
extern "C" bool PrimeGpuTryCaptureCpuFifo(uint32_t value, unsigned width);
extern "C" uint32_t PrimeGpuGatherWriteBulk(const uint8_t* data, uint32_t size);
static std::vector<uint8_t> g_primeStoppedFifo;
// Not static: memory.cpp's gather-pipe store fast path checks it before appending (same test as HleFifoWrite).
bool g_primeFifoReplaying = false;
static thread_local bool g_primeFifoParsing = false;
static bool g_primeFontFifoTraceArmed = false;
#endif

extern "C" void GX_HLE_PrimeResumeFifo() {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    GxGpuThread::ResumeRetained();
    if (g_primeFifoReplaying || PrimeGpuCpReadBlocked() || g_primeStoppedFifo.empty()) return;
    std::vector<uint8_t> pending;
    pending.swap(g_primeStoppedFifo);
    g_primeFifoReplaying = true;
    try {
        size_t i = 0;
        while (i + 4 <= pending.size()) { HleFifoWrite(ReadBE32(pending.data() + i), 4); i += 4; }
        while (i < pending.size()) HleFifoWrite(pending[i++], 1);
    } catch (...) { g_primeFifoReplaying = false; throw; }
    g_primeFifoReplaying = false;
    static unsigned resumeTraceCount = 0;
    if (resumeTraceCount++ < 16)
        RT_LOG(RT_TAG_GX) << "[prime-cp-fifo-resume] bytes=" << pending.size() << std::endl;
#endif
}

namespace aurora::gx::fifo {
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);
void clear_buffer();
void abort_vertex_writer();
}
namespace aurora::gfx { void abort_frame() noexcept; }

extern "C" bool GX_HLE_PrimeFifoAtBoundary() {
    return !g_hleGxState.inBegin && g_hleGxState.fifoByteCount == 0;
}

// A GXBegin packet whose vertices span several bursts keeps Aurora inside
// GXBegin until the last one arrives; nothing else may drain Aurora meanwhile.
extern "C" bool GX_HLE_PrimeInsideDraw() {
    return g_hleGxState.inBegin;
}

extern "C" void GX_HLE_PrimeResetFifo() {
    // PI_FIFO_RESET discards work; it must not report draw completion.
    const size_t discarded = g_hleGxState.fifoByteCount;
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    g_primeStoppedFifo.clear();
    GxGpuThread::DiscardRetained();
#endif
    static unsigned resetTraceCount = 0;
    if (resetTraceCount++ < 16) {
        std::ostringstream trace;
        trace << "[prime-fifo-reset-pending] in_begin=" << g_hleGxState.inBegin
              << " vertices=" << g_hleGxState.vertsRemaining << " bytes=" << std::hex;
        for (size_t i = 0; i < discarded && i < 32; ++i)
            trace << static_cast<unsigned>(g_hleGxState.fifoBytes[g_hleGxState.fifoReadOffset + i]) << ',';
        RT_LOG(RT_TAG_GX) << trace.str() << std::endl;
    }
    g_hleGxState.fifoReadOffset = 0;
    g_hleGxState.fifoByteCount = 0;
    g_hleGxState.vertsRemaining = 0;
    g_hleGxState.inBegin = false;
    g_hleGxState.auroraBeginCalled = false;
    g_hleGxState.ResetVertex();
    aurora::gx::fifo::abort_vertex_writer();
    // The decoder thread must not be mid-frame while the producer aborts it.
    aurora_stream_sync();
    if (g_auroraFrameActive.exchange(false, std::memory_order_acq_rel))
        aurora::gfx::abort_frame();
    RT_LOG(RT_TAG_GX) << "[prime-pi-fifo-reset] discarded_bytes=" << discarded
        << " frame_aborted=1 completion_signaled=0" << std::endl;
}

extern "C" void GXCopyDispTracked(void*, GXBool, uint32_t);
extern "C" void GxPrimeNoteDisplayCopy();
extern "C" void GXBindRawTextureMemory(unsigned, const void*, unsigned, unsigned, unsigned, bool);
extern "C" void GXBindRawTexturePalette(unsigned, const void*, unsigned, unsigned, unsigned);
extern "C" uint32_t GXRawDrawTextureMask(GXVtxFmt);
extern "C" void GXAuroraPublishDirtyState(void);
void GX_HLE_PrimeCopyColorTexture(uint32_t,uint16_t,uint16_t,uint16_t,uint16_t,const PrimeRawBp::Registers::ColorCopy&,bool);
#if defined(MKW_GAME_PROFILE_RM3E01_00)
static PrimeRawBp::Registers g_primeRawBp;
// Decoder-thread state with the asynchronous decoder (gx_stream.cpp loads and
// binds it in stream order); the producer only reads it for diagnostics then.
PrimeTlutMemory g_primeTlutMemory;
#endif

void GX_HLE_PrimeBindDrawPalettes(GXVtxFmt fmt) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    PrimePerformance::Scope performance(PrimePerformance::PaletteScan);
    // No indexed source means no TLUT can be consumed. Avoid constructing
    // shader information and taking an extra worker/GPU barrier in that case.
    // submit_raw_draw still publishes dirty state and drains before drawing.
    const uint32_t indexedMask = g_primeRawBp.IndexedTextureMask();
    if (indexedMask == 0) return;
    if (PrimeGxStream::Async()) {
        // The sampled-texture mask and the TLUT bytes are decoder state: bind
        // from the stream, with the BP values this draw was issued under.
        PrimeGxStream::BindPalettes bind{};
        bind.indexedMask = indexedMask;
        bind.vtxFmt = static_cast<uint8_t>(fmt);
        for (unsigned slot = 0; slot < 8; ++slot) {
            bind.formats[slot] = static_cast<uint8_t>(g_primeRawBp.TextureFormat(slot));
            bind.tlutWords[slot] = g_primeRawBp.values[0x98 + (slot & 3u) + (slot >= 4 ? 0x20 : 0)];
        }
        // The handler derives the sampled-texture mask from the decoder's
        // vertex state: publish this draw's VCD/VAT ahead of the callback,
        // where submit_raw_draw would otherwise emit them after it.
        GXAuroraPublishDirtyState();
        PrimeGxStream::EmitBindPalettes(bind);
        return;
    }
    const uint32_t sampledMask = GXRawDrawTextureMask(fmt) & indexedMask;
    for (unsigned slot = 0; slot < 8; ++slot) {
        if ((sampledMask & (1u << slot)) == 0) continue;
        const uint32_t format = g_primeRawBp.TextureFormat(slot);
        if (format != 8 && format != 9 && format != 10) continue;
        const uint32_t word = g_primeRawBp.values[0x98 + (slot & 3u) + (slot >= 4 ? 0x20 : 0)];
        const uint32_t paletteFormat = (word >> 10) & 3u;
        if (paletteFormat > 2) throw std::runtime_error("Invalid raw TLUT color format");
        const unsigned entries = format == 8 ? 16 : format == 9 ? 256 : 16384;
        const uint32_t offset = (word & 1023u) << 9;
        const void* palette;
        try { palette = g_primeTlutMemory.Span(offset, entries * 2); }
        catch (const std::runtime_error& error) {
            std::ostringstream message;
            message << error.what() << " slot=" << slot << " format=" << format << " tmem=0x"
                << std::hex << offset << " sampled_mask=0x" << sampledMask << std::dec << " entries=" << entries;
            throw std::runtime_error(message.str());
        }
        GXBindRawTexturePalette(slot, palette, paletteFormat, entries,
                                g_primeTlutMemory.RangeGeneration(offset, entries * 2));
        static bool fontTextureDumped=false;
        if (!fontTextureDumped && !PrimeGxStream::Async() && format==8 && g_primeRawBp.TextureWidth(slot)==64 && g_primeRawBp.TextureHeight(slot)==80) {
            if(const char* prefix=std::getenv("WII_COMPILED_DUMP_FONT_TEXTURE")) {
                const uint32_t imageReg=0x94+(slot&3u)+(slot>=4 ? 0x20 : 0);
                const uint32_t address=(g_primeRawBp.values[imageReg]<<5)|0x80000000u;
                const auto dump=[&](const char* suffix,const void* data,size_t size) {
                    const std::string path=std::string(prefix)+suffix;
                    auto* file=std::fopen(path.c_str(),"wb");
                    if(!file) throw std::runtime_error("Cannot write private font diagnostic");
                    const bool ok=std::fwrite(data,1,size,file)==size;std::fclose(file);
                    if(!ok) throw std::runtime_error("Incomplete private font diagnostic");
                };
                dump(".c4.bin",Memory::GetPointer(address,2560),2560);
                dump(".tlut.bin",palette,entries*2);
                fontTextureDumped=true;
                RT_LOG(RT_TAG_GX) << "[prime-font-texture-dump] prefix=" << prefix << " address=0x" << std::hex << address << std::dec
                    << " palette_format=" << paletteFormat << " entries=" << entries << std::endl;
            }
        }

    }
#endif
}

#if defined(MKW_GAME_PROFILE_RM3E01_00)
namespace {
// Lines of an XFB the game's VI programming scans out: VTR.ACV active lines
// per field, twice that when interlaced (DCR.NIN clear). Prime copies 480
// lines but programs a shorter active area; the unscanned rows never reach a
// TV, so presenting them shows a band. 0 (show the whole copy) when VI is not
// programmed or the value does not fit the copy.
uint32_t PrimeViVisibleLines(uint32_t copyHeight) {
    const uint16_t vtr = Memory::Read16(0xCC002000u);
    const uint16_t dcr = Memory::Read16(0xCC002002u);
    const uint32_t acv = (vtr >> 4) & 0x3FFu;
    uint32_t lines = (dcr & 4u) ? acv : acv * 2u;
    if (lines > copyHeight) lines = acv;  // progressive timing counts whole frames
    if (lines == 0 || lines > copyHeight) lines = 0;
    static uint32_t tracedVtr = ~0u, tracedDcr = ~0u, tracedHeight = ~0u;
    if (vtr != tracedVtr || dcr != tracedDcr || copyHeight != tracedHeight) {
        tracedVtr = vtr;
        tracedDcr = dcr;
        tracedHeight = copyHeight;
        RT_LOG(RT_TAG_GX) << "[prime-vi-visible] vtr=0x" << std::hex << vtr << " dcr=0x" << dcr
            << " tfbl=0x" << Memory::Read16(0xCC00201Cu) << Memory::Read16(0xCC00201Eu)
            << " vto=0x" << Memory::Read16(0xCC00200Cu) << Memory::Read16(0xCC00200Eu)
            << std::dec << " acv=" << acv << " copy_height=" << copyHeight
            << " presented_lines=" << (lines ? lines : copyHeight) << std::endl;
    }
    return lines;
}
}  // namespace
#endif

void GX_HLE_ApplyRawBp(uint8_t reg, uint32_t value) {
    {
        PrimePerformance::Scope performance(PrimePerformance::BpApply);
        GXApplyBPReg(reg, value);
    }
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    auto& bp = g_primeRawBp;
    const uint32_t merged = bp.Write(reg, value);
    if (reg == 0x47) {
        // Original GXSetDrawSync emits BP48 then BP47. Prime polls PE+0x0E
        // to retire CPU-side staging allocations. GXApplyBPReg has drained
        // the preceding producer commands; Aurora owns copies of their
        // source data at that boundary. Publish the consumed token only here,
        // never advance it from the polling read or force the allocator free.
        // With the asynchronous decoder the token travels in the stream and
        // is published once that thread has decoded the preceding draws.
        if (PrimeGxStream::Async()) {
            PrimeGxStream::EmitPeToken(static_cast<uint16_t>(merged));
            return;
        }
        Memory::Write16(0xCC00100Eu, static_cast<uint16_t>(merged));
        static unsigned tokenTraceCount = 0;
        if (tokenTraceCount++ < 32)
            RT_LOG(RT_TAG_GX) << "[prime-pe-token-consumed] token="
                << static_cast<uint16_t>(merged) << std::endl;
    }
    const int slot = PrimeRawBp::Registers::TextureSlot(reg);
    if (slot >= 0) {
        PrimePerformance::Scope performance(PrimePerformance::TextureBind);
        const uint32_t format = bp.TextureFormat(slot);
        if (!(format <= 6 || format == 8 || format == 9 || format == 10 || format == 14))
            throw std::runtime_error("Prime raw texture format is not yet verified");
        const uint32_t address = (merged << 5) | 0x80000000u;
        const uint16_t width = bp.TextureWidth(slot), height = bp.TextureHeight(slot);
        const bool mips = bp.TextureMips(slot);
        const uint8_t maxLod = static_cast<uint8_t>(((bp.TextureMode1(slot) >> 8) & 255u) / 16u);
        const uint32_t bytes = GXGetTexBufferSize(width, height, static_cast<GXTexFmt>(format), mips, maxLod);
        const void* data = Memory::GetPointer(address, bytes);
        if (PrimeGxStream::Async())
            PrimeGxStream::EmitTexBind({reinterpret_cast<uintptr_t>(data), width, height, format,
                                        static_cast<uint8_t>(slot), static_cast<uint8_t>(mips)});
        else
            GXBindRawTextureMemory(slot, data, width, height, format, mips);
        if (width == 64 && height == 80 && std::getenv("WII_COMPILED_TRACE_PRIME_FONT_FIFO"))
            g_primeFontFifoTraceArmed = true;
        static unsigned textureTraceCount = 0;
        static std::array<uint64_t, 64> textureTraceKeys{};
        static unsigned textureTraceKeyCount = 0;
        const uint64_t key = (uint64_t(slot) << 48) | (uint64_t(format) << 32) | (uint64_t(width) << 16) | height;
        bool unseen = true;
        for (unsigned i = 0; i < textureTraceKeyCount; ++i) if (textureTraceKeys[i] == key) unseen = false;
        const bool traceNew = unseen && textureTraceKeyCount < textureTraceKeys.size();
        if (traceNew) textureTraceKeys[textureTraceKeyCount++] = key;
        if (textureTraceCount++ < 32 || traceNew)
            RT_LOG(RT_TAG_GX) << "[prime-raw-texture] slot=" << slot << " address=0x" << std::hex << address
                << std::dec << " width=" << width << " height=" << height << " format=" << format
                << " bytes=" << bytes << " mipmap=" << mips << std::endl;
    }
    if (reg == 0x65) {
        const uint32_t address = (bp.values[0x64] << 5) | 0x80000000u;
        const uint32_t offset = (merged & 1023u) << 9;
        const uint32_t bytes = ((merged >> 10) & 2047u) * 32u;
        const void* source = Memory::GetPointer(address, bytes);
        if (PrimeGxStream::Async()) PrimeGxStream::EmitTlutLoad({reinterpret_cast<uintptr_t>(source), offset, bytes});
        else g_primeTlutMemory.Load(offset, source, bytes);
        static unsigned tlutTraceCount = 0;
        if (tlutTraceCount++ < 16)
            RT_LOG(RT_TAG_GX) << "[prime-raw-tlut-load] source=0x" << std::hex << address
                << " tmem=0x" << offset << std::dec << " bytes=" << bytes << std::endl;
    }
    if (reg != 0x52) return;
    if (!bp.IsXfb()) {
        static unsigned textureCopies=0;
        if(textureCopies++<32) RT_LOG(RT_TAG_GX)<<"[prime-raw-texture-copy] destination=0x"<<std::hex<<bp.Destination()
            <<" trigger=0x"<<merged<<" stride_word=0x"<<bp.values[0x4D]<<std::dec
            <<" left="<<bp.Left()<<" top="<<bp.Top()<<" width="<<bp.Width()<<" height="<<bp.Height()<<std::endl;
        PrimeRawBp::Registers::ColorCopy copy{};
        try { copy=bp.DecodeColorCopy(); }
        catch(...) {
            RT_LOG(RT_TAG_GX)<<"[prime-raw-texture-copy-rejected] trigger=0x"<<std::hex<<merged
                <<" pe=0x"<<bp.values[0x43]<<" stride_word=0x"<<bp.values[0x4D]
                <<" source_xy=0x"<<bp.values[0x49]<<" source_wh=0x"<<bp.values[0x4A]<<std::dec<<std::endl;
            throw;
        }
        if(copy.halfScale) {
            static unsigned halfCopies=0;
            if(halfCopies++<16) RT_LOG(RT_TAG_GX)<<"[prime-raw-half-scale-copy] source="<<bp.Width()<<"x"<<bp.Height()
                <<" destination="<<copy.width<<"x"<<copy.height<<" bytes="<<copy.bytes<<std::endl;
        }
        GX_HLE_PrimeCopyColorTexture(bp.Destination(),bp.Left(),bp.Top(),bp.Width(),bp.Height(),copy,bp.Clear());
        if(bp.Left()+bp.Width()>640) {
            static unsigned edgeCopies=0;
            if(edgeCopies++<8) RT_LOG(RT_TAG_GX)<<"[prime-a8-right-edge-copy] left="<<bp.Left()
                <<" top="<<bp.Top()<<" width="<<bp.Width()<<" height="<<bp.Height()
                <<" bytes="<<copy.bytes<<" tail_policy=clamp-to-edge"<<std::endl;
        }
        // Record the first completed adapter submission for each format even
        // when the general copy trace budget expired in the title/menu.
        static uint64_t copyFormatsTraced = 0;
        const uint64_t formatBit = uint64_t(1) << copy.format;
        if ((copyFormatsTraced & formatBit) == 0) {
            copyFormatsTraced |= formatBit;
            RT_LOG(RT_TAG_GX) << "[prime-copy-format-submitted] format=0x" << std::hex << copy.format
                << " trigger=0x" << merged << std::dec << " width=" << copy.width
                << " height=" << copy.height << " bytes=" << copy.bytes << std::endl;
        }
        return;
    }
    // Preserve the original trigger, source rectangle, destination and clear bit.
    // Copy commands are side effects even when their BP value repeats.
    const uint32_t scaleWord = bp.values[0x4E] & 0x1FFu;
    if (scaleWord != 0x100u)
        throw std::runtime_error("Prime raw XFB copy uses a vertical scale not yet verified by this adapter");
    EnsureAuroraFrameActive();
    GxPrimeNoteDisplayCopy();  // a new frame for the volatile-array views (gx_dl.cpp)
    if (PrimeGxStream::Async()) {
        // The copy, the latency note and the XFB-ready flag all happen when the
        // decoder reaches this point of the stream.
        PrimeGxStream::EmitXfbCopy({reinterpret_cast<uintptr_t>(GuestToHostPtr(bp.Destination())),
                                    PrimeInputLatency::DecodingSubmitTime(), bp.Destination(),
                                    PrimeViVisibleLines(bp.Height()), bp.Left(), bp.Top(), bp.Width(), bp.Height(),
                                    bp.PitchPixels(), static_cast<uint8_t>(bp.Clear())});
        GXMarkFrameWork();
    } else {
    // GXCopyDisp only drains non-empty native FIFOs; DrawDone also provides
    // the worker preparation barrier for this direct raw-state path.
    GXDrawDone();
    GXSetDispCopySrc(bp.Left(), bp.Top(), bp.Width(), bp.Height());
    GXSetDispCopyDst(bp.PitchPixels(), bp.Height());
    GXCopyDispTracked(GuestToHostPtr(bp.Destination()), bp.Clear() ? GX_TRUE : GX_FALSE,
                      PrimeViVisibleLines(bp.Height()));
    GXMarkFrameWork();
    PrimeInputLatency::NoteXfbCopy();
    VI_HLE_SetXfbReady(bp.Destination());
    VI_HLE_NotePrimeXfbCopied(bp.Destination());
    }
    static unsigned traceCount = 0;
    if (traceCount++ < 24)
        RT_LOG(RT_TAG_GX) << "[prime-raw-xfb-copy] destination=0x" << std::hex << bp.Destination()
            << " trigger=0x" << merged << std::dec << " width=" << bp.Width()
            << " height=" << bp.Height() << " pitch=" << bp.PitchPixels()
            << " clear=" << bp.Clear() << std::endl;
#endif
}

void HleGxState::ResetVertex() {
    currentAttr = NextEnabledAttr(GX_VA_PNMTXIDX - 1);
    currentComp = 0;
}

GXAttr HleGxState::NextEnabledAttr(int startAttr) {
    for (int i = startAttr + 1; i < 26; ++i) {
        if (vtxDesc[i] != GX_NONE) {
            return static_cast<GXAttr>(i);
        }
    }
    return GX_VA_NULL;
}

int HleGxState::GetExpectedCompCount(GXAttr attr, const VtxAttrFmt& fmt) {
    // Attributes with no component layout (matrix indices, XF arrays, GX_VA_NBT)
    // count as one component here: the incremental parser advances one element
    // per raw stream item for them.
    return static_cast<int>(AttrCompCount(attr, fmt, 1u));
}

static void ApplyAuroraVtxStateForRawBegin(GXVtxFmt fmt) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    // CP VAT decoding already canonicalizes normals/NBT into GX_VA_NRM.
    // Publishing the unused NBT row (whose default count is GX_POS_XYZ=1)
    // overwrites that shared VAT with NBT=1: three XYZ floats become nine.
    GXSetVtxDesc(GX_VA_NBT,GX_NONE);
    PublishAuroraVtxState(fmt,AuroraVtxPublishOptions{/*includeNbt=*/false,
        /*fmtLoopFirst=*/0,/*fmtLoopLast=*/25});
#else
    // KNOWN DIVERGENCE: this is the only publish site with includeNbt=true, so it also
    // publishes GX_VA_NBT after GX_VA_NRM, which makes aurora's SETVAT fall-through clobber the
    // NRM VAT with the default. Raw-FIFO geometry renders correctly with it; do not "fix" without
    // an in-race A/B run.
    PublishAuroraVtxState(fmt, AuroraVtxPublishOptions{/*includeNbt=*/true,
                                                       /*fmtLoopFirst=*/0,
                                                       /*fmtLoopLast=*/25});
#endif
}

// There is deliberately no indexed-aurora path here. Immediate-mode indexed
// draws are parsed incrementally, and aurora's indexed-array upload has to
// precede the draw carrying the max-index bounds, which cannot be known without
// buffering the whole primitive. Indexed attributes are therefore expanded into
// the packed direct stream above (GX_INDEX8/16 -> GX_DIRECT).

static uint32_t GetDirectAttrByteSizeForFifo(GXAttr attr, const VtxAttrFmt& fmt) {
    return DirectAttrByteSize(attr, fmt, /*matrixAttrIsOneByte=*/true, /*fallbackComps=*/1u);
}

static bool TryGetRawDirectFifoVertexSize(GXVtxFmt fmt, uint32_t& vertexSize) {
    vertexSize = 0;
    if (fmt >= GX_MAX_VTXFMT) {
        return false;
    }

    for (int attr = 0; attr < 26; ++attr) {
        const GXAttrType type = g_hleGxState.vtxDesc[attr];
        if (type == GX_NONE) {
            continue;
        }
        if (type != GX_DIRECT) {
            return false;
        }

        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        const uint32_t attrBytes =
            GetDirectAttrByteSizeForFifo(gxAttr, g_hleGxState.vtxAttrFmt[fmt][attr]);
        if (attrBytes == 0) {
            return false;
        }
        vertexSize += attrBytes;
    }

    return vertexSize != 0;
}

static bool TrySubmitRawDirectFifoDraw(const uint8_t* packet, uint32_t packetBytes, GXPrimitive prim,
                                       GXVtxFmt vtxFmt, uint16_t vtxCount) {
    if (packet == nullptr || packetBytes == 0 || vtxCount == 0) {
        return false;
    }

#if defined(MKW_GAME_PROFILE_RM3E01_00)
    static unsigned fontPacketTrace = 0;
    if (g_primeFontFifoTraceArmed && vtxCount == 4 && packetBytes == 83 && fontPacketTrace++ < 256) {
        RT_LOG(RT_TAG_GX) << "[prime-font-packet-tail-before] ";
        for (unsigned i = packetBytes - 20; i < packetBytes; i += 4)
            RT_LOG(RT_TAG_GX) << std::hex << ReadBE32(packet + i) << ",";
        RT_LOG(RT_TAG_GX) << std::dec << std::endl;
    }
#endif
    EnsureAuroraFrameActive();
    if (aurora_draw_dump_active())
        RT_LOG(RT_TAG_GX) << "[dump-raw-draw] count=" << vtxCount << " fmt=" << vtxFmt << " pos_desc="
            << g_hleGxState.vtxDesc[GX_VA_POS] << " bytes=" << packetBytes << std::endl;

    g_hleGxState.currentVtxFmt = vtxFmt;
    g_hleGxState.currentPrim = prim;
    g_hleGxState.vertsRemaining = vtxCount;
    g_hleGxState.inBegin = false;
    g_hleGxState.auroraBeginCalled = false;
    g_hleGxState.ResetVertex();

    ApplyAuroraVtxStateForRawBegin(vtxFmt);
    EnsureDefaultGxAlphaCompare();

#if defined(MKW_GAME_PROFILE_RM3E01_00)
    // The original SDK flushes BP state with degenerate triangle strips. The
    // native Aurora SDK already omits that dummy primitive in __GXSendFlushPrim.
    // Preserve state publication above, but do not rasterize provably identical
    // direct vertices. Real GX cannot cover a pixel with zero-area triangles.
    const bool triangles=prim==GX_TRIANGLES || prim==GX_TRIANGLESTRIP || prim==GX_TRIANGLEFAN || prim==GX_QUADS;
    if(triangles && GxFifoStorage::AllDirectVerticesIdentical(packet+3,packetBytes-3,vtxCount,(packetBytes-3)/vtxCount)) {
        static unsigned degenerateTrace=0;
        if(degenerateTrace++<16)
            RT_LOG(RT_TAG_GX) << "[prime-raw-degenerate-draw] vertices=" << vtxCount << " bytes=" << packetBytes
                << " identical_direct_records=1" << std::endl;
        return true;
    }
#endif
    GX_HLE_PrimeBindDrawPalettes(vtxFmt);
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    static unsigned fontDrawDumpCount=0;
    if (!PrimeGxStream::Async() && fontDrawDumpCount<50 && vtxCount==4 && packetBytes==83 &&
        g_primeRawBp.TextureFormat(0)==8 && g_primeRawBp.TextureWidth(0)==64 && g_primeRawBp.TextureHeight(0)==80) {
        if(const char* prefix=std::getenv("WII_COMPILED_DUMP_FONT_TEXTURE")) {
            const uint32_t paletteWord=g_primeRawBp.values[0x98];
            const uint8_t paletteFormat=uint8_t((paletteWord>>10)&3u);
            const void* palette=g_primeTlutMemory.Span((paletteWord&1023u)<<9,32);
            const std::string path=std::string(prefix)+".draws.bin";
            auto* file=std::fopen(path.c_str(),fontDrawDumpCount==0 ? "wb" : "ab");
            if(!file) throw std::runtime_error("Cannot write private font draw inputs");
            const bool ok=std::fwrite(packet+3,1,80,file)==80 && std::fwrite(palette,1,32,file)==32 && std::fwrite(&paletteFormat,1,1,file)==1;
            std::fclose(file);
            if(!ok) throw std::runtime_error("Incomplete private font draw inputs");
            ++fontDrawDumpCount;
        }
    }
#endif
    {
        PrimePerformance::Scope performance(PrimePerformance::RawDraw);
        if (!aurora::gx::fifo::submit_raw_draw(prim, vtxFmt, packet + 3, vtxCount, packetBytes - 3u)) {
            return false;
        }
    }
    GXMarkFrameWork();
    static unsigned submittedTraceCount = 0;
    if (submittedTraceCount++ < 8)
        RT_LOG(RT_TAG_GX) << "[raw-fifo-draw-submitted] bytes=" << packetBytes
            << " vertices=" << vtxCount << std::endl;
    return true;
}

static void DecodeColorFromArray(uint32_t addr, GXCompType type, GXCompCnt cnt, GXColor& out) {
    // Gather exactly the bytes the shared decoder consumes. GX_RGBX8 is the one
    // format whose stream footprint (4 bytes) exceeds what is decoded (3), and
    // the pre-dedup code read only those 3 from the guest array - keep it that
    // way so a 3-byte tail at the end of an array cannot start faulting.
    const uint32_t byteCount =
        (type == GX_RGBX8) ? 3u : ColorByteSize(type, cnt);
    uint8_t bytes[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < byteCount && i < 4u; ++i) {
        bytes[i] = Memory::Read8(addr + i);
    }
    DecodeColorBytes(bytes, type, cnt, out);
}

void SubmitIndexedAttribute(GXAttr attr, uint32_t index) {
    const auto& fmt = g_hleGxState.vtxAttrFmt[g_hleGxState.currentVtxFmt][attr];
    const auto& arr = g_hleGxState.vtxArray[attr];
    if (arr.base == 0 || arr.stride == 0) {
        float comps[9]{};
        switch (attr) {
        case GX_VA_CLR0:
        case GX_VA_CLR1:
            if (fmt.cnt == GX_CLR_RGB) {
                GXColor3u8(0, 0, 0);
            } else {
                GXColor4u8(0, 0, 0, 0xFF);
            }
            break;
        case GX_VA_POS:
        case GX_VA_NRM:
        case GX_VA_TEX0:
        case GX_VA_TEX1:
        case GX_VA_TEX2:
        case GX_VA_TEX3:
        case GX_VA_TEX4:
        case GX_VA_TEX5:
        case GX_VA_TEX6:
        case GX_VA_TEX7:
            SubmitAttribute(attr, comps, fmt);
            break;
        default:
            break;
        }
        return;
    }
    const uint32_t baseAddr = arr.base + index * arr.stride;
    float comps[9]{};
    u32 rawComps[9]{};
    switch (attr) {
    case GX_VA_POS: {
        const int count = static_cast<int>(AttrCompCount(GX_VA_POS, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            comps[i] = ReadArrayComp(addr, fmt.type, fmt.frac);
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    case GX_VA_NRM: {
        const int count = static_cast<int>(AttrCompCount(GX_VA_NRM, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            comps[i] = ReadArrayComp(addr, fmt.type, fmt.frac);
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    case GX_VA_CLR0:
    case GX_VA_CLR1: {
        const uint32_t bytes = ColorByteSize(fmt.type, fmt.cnt);
        SubmitPackedColor(static_cast<const uint8_t*>(Memory::GetPointer(baseAddr, bytes)), bytes);
        break;
    }
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7: {
        // Every GX_VA_TEXn shares one component layout, so the constant here is
        // exact for whichever of them `attr` is.
        const int count = static_cast<int>(AttrCompCount(GX_VA_TEX0, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            comps[i] = ReadArrayComp(addr, fmt.type, fmt.frac);
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    default:
        break;
    }
}

// Mirrors the raw-integer fast paths in SubmitAttribute below: for those
// (attr, type) pairs the float component buffer is never read, so the direct
// FIFO path can skip building it.
static bool SubmitAttributeReadsFloatComps(GXAttr attr, const VtxAttrFmt& fmt) {
    switch (attr) {
    case GX_VA_POS:
    case GX_VA_NRM:
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7:
        break;
    default:
        return true;
    }
    switch (fmt.type) {
    case GX_U8:
    case GX_S8:
    case GX_U16:
    case GX_S16:
        return false;
    default:
        return true;
    }
}

void SubmitAttribute(GXAttr attr, float* comps, const VtxAttrFmt& fmt, const u32* rawComps) {
    auto r8 = [rawComps](int idx) { return static_cast<u8>(rawComps[idx]); };
    auto rs8 = [rawComps](int idx) { return static_cast<s8>(rawComps[idx]); };
    auto r16 = [rawComps](int idx) { return static_cast<u16>(rawComps[idx]); };
    auto rs16 = [rawComps](int idx) { return static_cast<s16>(rawComps[idx]); };

    switch (attr) {
    case GX_VA_POS:
        if (rawComps) {
            if (fmt.cnt == GX_POS_XY) {
                switch (fmt.type) {
                case GX_U8: GXPosition2u8(r8(0), r8(1)); return;
                case GX_S8: GXPosition2s8(rs8(0), rs8(1)); return;
                case GX_U16: GXPosition2u16(r16(0), r16(1)); return;
                case GX_S16: GXPosition2s16(rs16(0), rs16(1)); return;
                default: break;
                }
            } else {
                switch (fmt.type) {
                case GX_U8: GXPosition3u8(r8(0), r8(1), r8(2)); return;
                case GX_S8: GXPosition3s8(rs8(0), rs8(1), rs8(2)); return;
                case GX_U16: GXPosition3u16(r16(0), r16(1), r16(2)); return;
                case GX_S16: GXPosition3s16(rs16(0), rs16(1), rs16(2)); return;
                default: break;
                }
            }
        }
        if (fmt.cnt == GX_POS_XY) {
            GXPosition2f32(comps[0], comps[1]);
        } else {
            GXPosition3f32(comps[0], comps[1], comps[2]);
        }
        break;
    case GX_VA_NRM:
        if (rawComps) {
            const bool nbt = fmt.cnt == GX_NRM_NBT || fmt.cnt == GX_NRM_NBT3;
            const int groups = nbt ? 3 : 1;
            switch (fmt.type) {
            case GX_U8:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3u8(r8(i * 3), r8(i * 3 + 1), r8(i * 3 + 2));
                }
                return;
            case GX_S8:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3s8(rs8(i * 3), rs8(i * 3 + 1), rs8(i * 3 + 2));
                }
                return;
            case GX_U16:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3u16(r16(i * 3), r16(i * 3 + 1), r16(i * 3 + 2));
                }
                return;
            case GX_S16:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3s16(rs16(i * 3), rs16(i * 3 + 1), rs16(i * 3 + 2));
                }
                return;
            default: break;
            }
        }
        if (fmt.cnt == GX_NRM_NBT || fmt.cnt == GX_NRM_NBT3) {
            for (int i = 0; i < 3; ++i) {
                GXNormal3f32(comps[i * 3], comps[i * 3 + 1], comps[i * 3 + 2]);
            }
        } else {
            GXNormal3f32(comps[0], comps[1], comps[2]);
        }
        break;
    case GX_VA_CLR0:
    case GX_VA_CLR1:
        if (fmt.cnt == GX_CLR_RGB) {
            GXColor3u8(static_cast<u8>(comps[0]), static_cast<u8>(comps[1]),
                       static_cast<u8>(comps[2]));
        } else {
            GXColor4u8(static_cast<u8>(comps[0]), static_cast<u8>(comps[1]),
                       static_cast<u8>(comps[2]), static_cast<u8>(comps[3]));
        }
        break;
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7:
        if (rawComps) {
            if (fmt.cnt == GX_TEX_S) {
                switch (fmt.type) {
                case GX_U8: GXTexCoord1u8(r8(0)); return;
                case GX_S8: GXTexCoord1s8(rs8(0)); return;
                case GX_U16: GXTexCoord1u16(r16(0)); return;
                case GX_S16: GXTexCoord1s16(rs16(0)); return;
                default: break;
                }
            } else {
                switch (fmt.type) {
                case GX_U8: GXTexCoord2u8(r8(0), r8(1)); return;
                case GX_S8: GXTexCoord2s8(rs8(0), rs8(1)); return;
                case GX_U16: GXTexCoord2u16(r16(0), r16(1)); return;
                case GX_S16: GXTexCoord2s16(rs16(0), rs16(1)); return;
                default: break;
                }
            }
        }
        if (fmt.cnt == GX_TEX_S) {
            GXTexCoord1f32(comps[0]);
        } else {
            GXTexCoord2f32(comps[0], comps[1]);
        }
        break;
    default: break;
    }
}

// `val` is a raw big-endian bit pattern: the FIFO stream is type-agnostic, and
// the float entry point converts before it gets here.
static void HleFifoConsume(u32 val, uint32_t sizeBytes) {
    static const bool traceEnabled = std::getenv("WII_COMPILED_TRACE_PRIME_FIFO") != nullptr;
    static unsigned traceSequence = 0;
    bool replaying = false;
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    replaying = g_primeFifoReplaying;
#endif
    if (traceEnabled && !replaying && traceSequence < 4096) {
        RT_LOG(RT_TAG_GX) << "[prime-fifo-write] seq=" << traceSequence++
            << " width=" << sizeBytes << " value=0x" << std::hex << val << std::dec << std::endl;
    }
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    static unsigned primeFontFifoTraceCount = 0;
    if (g_primeFontFifoTraceArmed && !replaying && primeFontFifoTraceCount < 8192) {
        const auto* cpu = TryGetCpuContext();
        RT_LOG(RT_TAG_GX) << "[prime-font-fifo-write] seq=" << primeFontFifoTraceCount++
            << " width=" << sizeBytes << " value=0x" << std::hex << val
            << " active=0x" << RecompMod::CurrentTranslatedExecutionAddress()
            << " lr=0x" << (cpu ? cpu->lr : 0) << std::dec << std::endl;
    }
#endif
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    // Queued bursts were already tested against the stall when submitted.
    if (!GxGpuThread::DecodingQueued() && PrimeGpuCpReadBlocked()) {
        const uint32_t count = sizeBytes == 0 ? 4u : sizeBytes;
        if (count != 1 && count != 2 && count != 4)
            throw std::logic_error("Unsupported gather write width");
        if (g_primeStoppedFifo.size() > GxFifoStorage::MaxBufferedBytes - count)
            throw std::length_error("Stopped GP FIFO exceeded bounded storage");
        for (uint32_t i = count; i > 0; --i)
            g_primeStoppedFifo.push_back(static_cast<uint8_t>(val >> ((i - 1u) * 8u)));
        return;
    }
#endif
    const bool recordOnly = IsDisplayListActive();
    if (recordOnly) {
        WriteDisplayListData(val, sizeBytes);
        return;
    }

    auto resetFifoBuffer = [&]() {
        g_hleGxState.fifoReadOffset = 0;
        g_hleGxState.fifoByteCount = 0;
    };

    auto fifoData = [&]() -> uint8_t* {
        return g_hleGxState.fifoBytes.data() + g_hleGxState.fifoReadOffset;
    };

    auto pushBytes = [&](u32 value, uint32_t count) {
        if (count == 0) return;
        if (count > 4) count = 4;
        GxFifoStorage::EnsureAppendSpace(g_hleGxState.fifoBytes,
            g_hleGxState.fifoReadOffset, g_hleGxState.fifoByteCount, count);
        const size_t writeOffset = g_hleGxState.fifoReadOffset + g_hleGxState.fifoByteCount;
        switch (count) {
        case 4:
            BigEndian::Write32(g_hleGxState.fifoBytes.data() + writeOffset, value);
            break;
        case 2:
            BigEndian::Write16(g_hleGxState.fifoBytes.data() + writeOffset,
                               static_cast<uint16_t>(value));
            break;
        default:
            g_hleGxState.fifoBytes[writeOffset] = static_cast<uint8_t>(value & 0xFF);
            break;
        }
        g_hleGxState.fifoByteCount += count;
    };

    auto consumeBytes = [&](uint32_t count, u32& out) -> bool {
        if (count == 0 || g_hleGxState.fifoByteCount < count) return false;
        u32 value = 0;
        const uint32_t foldCount = (count > 4) ? 4 : count;
        uint8_t* data = fifoData();
        for (uint32_t i = 0; i < foldCount; ++i) {
            value = (value << 8) | data[i];
        }
        g_hleGxState.fifoReadOffset += count;
        g_hleGxState.fifoByteCount -= count;
        if (g_hleGxState.fifoByteCount == 0) {
            g_hleGxState.fifoReadOffset = 0;
        }
        out = value;
        return true;
    };

    pushBytes(val, sizeBytes == 0 ? 4 : sizeBytes);
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    // Renderer waits may pump a guest IRQ which resumes more FIFO bytes.
    // Append those bytes in order; the current consumer owns parsing until its
    // backend call returns. Recursing here invalidates both packet pointers and
    // the consumer's read/count state.
    GxFifoStorage::ConsumerScope consumerGuard(g_primeFifoParsing);
    if (!consumerGuard) return;
#endif

    // Parse raw FIFO command packets written directly to the gather pipe
    // (e.g. NW4R/G3D paths that do not call the GXBegin wrapper function).
    while (!g_hleGxState.inBegin) {
        if (g_hleGxState.fifoByteCount < 1) {
            break;
        }

        uint8_t* data = fifoData();
        const uint8_t cmd = data[0];
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;
        u32 sink = 0;

        if (cmd == GX_NOP_CMD || opcode == GX_CMD_INVL_VC_CMD) {
            if (!consumeBytes(1, sink)) break;
            continue;
        }

        if (cmd == GX_LOAD_BP_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint32_t bpWord = ReadBE32(data + 1);
            GX_HLE_ApplyRawBp(static_cast<uint8_t>(bpWord >> 24), bpWord & 0x00FFFFFFu);
            if (!consumeBytes(5, sink)) break;
            continue;
        }

        if (opcode == GX_LOAD_CP_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 6) break;
            const uint8_t reg = data[1];
            const uint32_t cpValue = ReadBE32(data + 2);
            GxCpDecode::ApplyCpRegWrite(reg, cpValue);
            if (!consumeBytes(6, sink)) break;
            continue;
        }

        if (opcode == GX_LOAD_XF_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint16_t countWords = ReadBE16(data + 1);
            const uint32_t packetBytes = 1u + 4u + (static_cast<uint32_t>(countWords) + 1u) * 4u;
            if (g_hleGxState.fifoByteCount < packetBytes) break;
#if defined(MKW_GAME_PROFILE_RM3E01_00)
            if (CachedWiiCompiledTracePrimeMiiFifo() && ReadBE16(data + 3) == 0x101A) {
                static uint32_t count = 0;
                const uint32_t sx = ReadBE32(data + 5);
                if (count++ < 16 || sx < 0x3F000000u || sx > 0x45000000u) {
                    const auto* cpu = TryGetCpuContext();
                    RT_LOG(RT_TAG_GX) << "[prime-viewport-packet] words=" << countWords + 1
                        << " active=0x" << std::hex << RecompMod::CurrentTranslatedExecutionAddress()
                        << " lr=0x" << (cpu ? cpu->lr : 0) << " sx=0x" << sx
                        << " sy=0x" << (countWords ? ReadBE32(data + 9) : 0)
                        << std::dec << std::endl;
                }
            }
            const std::vector<uint8_t> stablePacket(data, data + packetBytes);
            if (!consumeBytes(packetBytes, sink)) break;
            GXCallDisplayList(stablePacket.data(), packetBytes);
#else
            GXCallDisplayList(data, packetBytes);
            if (!consumeBytes(packetBytes, sink)) break;
#endif
            GXMarkFrameWork();
            continue;
        }

        if (opcode >= GX_LOAD_INDX_A_CMD && opcode <= GX_LOAD_INDX_D_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint32_t xfValue = ReadBE32(data + 1);
#if defined(MKW_GAME_PROFILE_RM3E01_00)
            std::array<uint8_t,5> stablePacket;
            std::memcpy(stablePacket.data(),data,stablePacket.size());
            if (!consumeBytes(5,sink)) break;
            // With the asynchronous decoder the matrix words are read now (SubmitIndexedXfImmediate).
            if (!(PrimeGxStream::Async() && SubmitIndexedXfImmediate(cmd,xfValue))) {
                ApplyIndexedXfArrayForPacket(cmd,xfValue);
                GXCallDisplayList(stablePacket.data(),stablePacket.size());
            }
#else
            ApplyIndexedXfArrayForPacket(cmd, xfValue);
            GXCallDisplayList(data, 5);
            if (!consumeBytes(5, sink)) break;
#endif
            GXMarkFrameWork();
            continue;
        }

        if (opcode == GX_CMD_CALL_DL_CMD) {
            if (g_hleGxState.fifoByteCount < 9) break;
            const uint32_t listAddr = ReadBE32(data + 1);
            const uint32_t listSize = ReadBE32(data + 5);
            if (!consumeBytes(9, sink)) break;
            if (listAddr != 0 && listSize > 0) {
                GX__CallDisplayList_80172f64(listAddr, listSize);
            }
            continue;
        }

        if (IsDrawOpcode(opcode)) {
            if (g_hleGxState.fifoByteCount < 3) break;
            const uint16_t vtxCount = ReadBE16(data + 1);
            if (vtxCount == 0) {
                if (!consumeBytes(3, sink)) break;
                continue;
            }
            const GXVtxFmt vtxFmt = static_cast<GXVtxFmt>(cmd & GX_VAT_MASK_CMD);
            const GXPrimitive prim = OpcodeToGXPrimitive(cmd);
            uint32_t rawVertexSize = 0;
            if (TryGetRawDirectFifoVertexSize(vtxFmt, rawVertexSize)) {
                const uint32_t packetBytes = 3u + static_cast<uint32_t>(vtxCount) * rawVertexSize;
                if (g_hleGxState.fifoByteCount < packetBytes) {
                    break;
                }
#if defined(MKW_GAME_PROFILE_RM3E01_00)
                // A nested producer can compact/grow fifoBytes while Aurora
                // waits. Retire the original packet and retain its exact bytes
                // independently for the entire backend call.
                const std::vector<uint8_t> stablePacket(data, data + packetBytes);
                if (!consumeBytes(packetBytes, sink)) break;
                if (!TrySubmitRawDirectFifoDraw(stablePacket.data(), packetBytes, prim, vtxFmt, vtxCount))
                    throw std::runtime_error("Prime raw GX vertex layout disagrees with its verified packet size");
                continue;
#else
                if (TrySubmitRawDirectFifoDraw(data, packetBytes, prim, vtxFmt, vtxCount)) {
                    if (!consumeBytes(packetBytes, sink)) break;
                    continue;
                }
#endif
            }
            if (!consumeBytes(3, sink)) break;

            g_hleGxState.currentVtxFmt = vtxFmt;
            g_hleGxState.currentPrim = prim;
            g_hleGxState.vertsRemaining = vtxCount;
            g_hleGxState.inBegin = true;
            g_hleGxState.auroraBeginCalled = false;
            g_hleGxState.ResetVertex();

            break;
        }

        // Unknown FIFO command byte outside a begin packet; discard it so stream parsing can recover.
        if (!consumeBytes(1, sink)) break;
    }

    if (!g_hleGxState.inBegin) {
        return;
    }

    if (!recordOnly && !g_hleGxState.auroraBeginCalled) {
        EnsureAuroraFrameActive();
        ApplyAuroraVtxStateForRawBegin(g_hleGxState.currentVtxFmt);
        EnsureDefaultGxAlphaCompare();
        GX_HLE_PrimeBindDrawPalettes(g_hleGxState.currentVtxFmt);
        GXBegin(g_hleGxState.currentPrim, g_hleGxState.currentVtxFmt, static_cast<u16>(g_hleGxState.vertsRemaining));
        g_hleGxState.auroraBeginCalled = true;
        GXMarkFrameWork();
    }

    auto finishVertexIfNeeded = [&](GXAttr prevAttr) {
        g_hleGxState.currentAttr = g_hleGxState.NextEnabledAttr(prevAttr);
        g_hleGxState.currentComp = 0;
        if (g_hleGxState.currentAttr == GX_VA_NULL) {
            g_hleGxState.ResetVertex();
            if (g_hleGxState.vertsRemaining > 0) {
                g_hleGxState.vertsRemaining--;
                if (g_hleGxState.vertsRemaining == 0) {
                    g_hleGxState.inBegin = false;
                    g_hleGxState.auroraBeginCalled = false;
                    if (!recordOnly) {
                        GXEnd();
                    }
                }
            }
        }
    };

    while (true) {
        if (!g_hleGxState.inBegin) {
            break;
        }
        GXAttr attr = g_hleGxState.currentAttr;
        if (attr == GX_VA_NULL) {
            resetFifoBuffer();
            return;
        }

        GXAttrType inputType = g_hleGxState.vtxDesc[attr];
        const VtxAttrFmt& fmt = g_hleGxState.vtxAttrFmt[g_hleGxState.currentVtxFmt][attr];

        if (IsMatrixIndexAttr(attr)) {
            if (g_hleGxState.fifoByteCount < 1) break;
            u32 raw = 0;
            if (!consumeBytes(1, raw)) break;
            if (!recordOnly) {
                GXMatrixIndex1u8(attr, static_cast<u8>(raw));
            }
            finishVertexIfNeeded(attr);
            continue;
        }

        if (inputType == GX_DIRECT) {
            if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
                const uint32_t colorSize = ColorByteSize(fmt.type, fmt.cnt);

                if (g_hleGxState.fifoByteCount < colorSize) break;

                uint8_t colorBytes[4] = {0, 0, 0, 0};
                uint8_t* data = fifoData();
                for (uint32_t i = 0; i < colorSize && i < 4; ++i) {
                    colorBytes[i] = data[i];
                }
                g_hleGxState.fifoReadOffset += colorSize;
                g_hleGxState.fifoByteCount -= colorSize;
                if (g_hleGxState.fifoByteCount == 0) {
                    g_hleGxState.fifoReadOffset = 0;
                }

                GXColor color{};
                DecodeColorBytes(colorBytes, fmt.type, fmt.cnt, color);

                if (!recordOnly) {
                    if (fmt.cnt == GX_CLR_RGB) {
                        GXColor3u8(color.r, color.g, color.b);
                    } else {
                        GXColor4u8(color.r, color.g, color.b, color.a);
                    }
                }
                finishVertexIfNeeded(attr);
                continue;
            }
            
            const int compSize = GetCompSizeBytes(fmt.type);
            if (compSize <= 0 || g_hleGxState.fifoByteCount < static_cast<size_t>(compSize)) break;
            u32 raw = 0;
            if (!consumeBytes(static_cast<uint32_t>(compSize), raw)) break;
            constexpr int kMaxComps = 9;
            if (g_hleGxState.currentComp < kMaxComps) {
                if (SubmitAttributeReadsFloatComps(attr, fmt)) {
                    g_hleGxState.compBuffer[g_hleGxState.currentComp] =
                        ConvertCompToFloat(raw, fmt.type, fmt.frac);
                }
                g_hleGxState.rawCompBuffer[g_hleGxState.currentComp] = raw;
            }
            g_hleGxState.currentComp++;
            const int expected = g_hleGxState.GetExpectedCompCount(attr, fmt);
            if (g_hleGxState.currentComp >= expected) {
                if (!recordOnly) {
                    SubmitAttribute(attr, g_hleGxState.compBuffer, fmt, g_hleGxState.rawCompBuffer);
                }
                finishVertexIfNeeded(attr);
            }
            continue;
        }

        if (inputType == GX_INDEX8 || inputType == GX_INDEX16) {
            const uint32_t idxSize = (inputType == GX_INDEX8) ? 1u : 2u;
            const uint32_t indexCount = (attr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
            if (g_hleGxState.fifoByteCount < idxSize * indexCount) break;
            u32 raw[3]{};
            for (uint32_t i = 0; i < indexCount; ++i) {
                if (!consumeBytes(idxSize, raw[i])) break;
            }
            if (!recordOnly) {
                if (attr == GX_VA_NRM && indexCount == 3u) {
                    SubmitIndexedNormalNBT3(raw, fmt);
                } else {
                    SubmitIndexedAttribute(attr, raw[0]);
                }
            }
            finishVertexIfNeeded(attr);
            continue;
        }

        g_hleGxState.currentAttr = g_hleGxState.NextEnabledAttr(attr);
        g_hleGxState.currentComp = 0;
        if (g_hleGxState.currentAttr == GX_VA_NULL) {
            g_hleGxState.ResetVertex();
        }
        break;
    }
}

// Must behave exactly like GX_HLE_FIFO_Write32/16/8 run byte by byte. The ring is not
// bulk-appended ahead of a single parse because the parser can re-enter GX HLE (nested display
// lists, XF/draw calls), which would misorder that work relative to the stream.
static void HleFifoWriteBurstChunked(const uint8_t* data, uint32_t sizeBytes) {
    uint32_t offset = 0;
    for (; offset + 4u <= sizeBytes; offset += 4u) {
        HleFifoWrite(ReadBE32(data + offset), 4);
    }
    if (offset + 2u <= sizeBytes) {
        HleFifoWrite(static_cast<u32>(ReadBE16(data + offset)), 2);
        offset += 2u;
    }
    if (offset < sizeBytes) {
        HleFifoWrite(static_cast<u32>(data[offset]), 1);
    }
}

// Applies complete register-load packets straight to the parser's own entry points, skipping the
// serialize/ring/re-parse round trip; returns bytes consumed, caller hands the rest to
// HleFifoWriteBurstChunked. Safe (per gd_fifo_hle.cpp's GdCanApplyDirect) only on a packet
// boundary with nothing buffered, outside a GXBegin packet and outside display-list recording;
// any other opcode or a truncated packet just stops the walk.
static uint32_t ApplyFifoPacketsDirect(const uint8_t* data, uint32_t sizeBytes) {
    uint32_t offset = 0;

    while (offset < sizeBytes) {
        // Re-tested per packet, not once per burst: nothing currently re-enters GX HLE mid-walk,
        // but if it ever does, breaking here just hands the remainder to the ring.
        if (IsDisplayListActive() || g_hleGxState.inBegin || g_hleGxState.fifoByteCount != 0) {
            break;
        }

        const uint8_t* packet = data + offset;
        const uint32_t avail = sizeBytes - offset;
        const uint8_t cmd = packet[0];
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;

        if (cmd == GX_NOP_CMD) {
            offset += 1u;
            continue;
        }

        if (cmd == GX_LOAD_BP_REG_CMD) {
            if (avail < 5u) break;
            const uint32_t bpWord = ReadBE32(packet + 1);
            GX_HLE_ApplyRawBp(static_cast<uint8_t>(bpWord >> 24), bpWord & 0x00FFFFFFu);
            offset += 5u;
            continue;
        }

        if (opcode == GX_LOAD_CP_REG_CMD) {
            if (avail < 6u) break;
            const uint8_t reg = packet[1];
            const uint32_t cpValue = ReadBE32(packet + 2);
            // Same function the parser calls, so the CP registers it does not
            // decode (0x30/0x40 among them) are dropped here identically.
            GxCpDecode::ApplyCpRegWrite(reg, cpValue);
            offset += 6u;
            continue;
        }

        if (opcode == GX_LOAD_XF_REG_CMD) {
            if (avail < 5u) break;
            const uint16_t countWords = ReadBE16(packet + 1);
            const uint32_t packetBytes = 1u + 4u + (static_cast<uint32_t>(countWords) + 1u) * 4u;
            if (avail < packetBytes) break;
            GXCallDisplayList(packet, packetBytes);
            GXMarkFrameWork();
            offset += packetBytes;
            continue;
        }

        break;
    }

    return offset;
}

// Display-list recording just copies bytes into the guest list buffer and advances the shadow
// cursor, so a burst is one logical append as long as it doesn't cross the list end; a burst that
// would wrap falls back to the per-write path instead. Returns false to signal that fallback.
static bool WriteDisplayListBurst(const uint8_t* data, uint32_t sizeBytes) {
    GxDisplayListState& dl = g_dlRecordState;
    if (!dl.active || dl.base == 0 || dl.size == 0 || dl.writePtr == 0) {
        // Same guard WriteDisplayListData applies; let the per-write path
        // reproduce its (silent) drop.
        return false;
    }

    const uint32_t end = dl.base + dl.size;
    if (dl.writePtr > end || (end - dl.writePtr) < sizeBytes) {
        return false;
    }

    // Guest-visible bytes are written through the ordinary store path, so
    // unaligned cursors, MMIO policy and executable-write guards all behave as
    // they do for a single store. The cursor and count advance per element so a
    // failed write in the middle leaves exactly the completed prefix recorded.
    try {
        uint32_t offset = 0;
        for (; offset + 4u <= sizeBytes; offset += 4u) {
            Memory::Write32(dl.writePtr, ReadBE32(data + offset));
            dl.writePtr += 4u;
            dl.count += 4u;
        }
        if (offset + 2u <= sizeBytes) {
            Memory::Write16(dl.writePtr, ReadBE16(data + offset));
            dl.writePtr += 2u;
            dl.count += 2u;
            offset += 2u;
        }
        if (offset < sizeBytes) {
            Memory::Write8(dl.writePtr, data[offset]);
            dl.writePtr += 1u;
            dl.count += 1u;
        }
    } catch (const Memory::AccessViolation&) {
    }
    return true;
}

void HleFifoWrite(u32 val, uint32_t sizeBytes) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    if (!g_primeFifoReplaying && PrimeGpuTryCaptureCpuFifo(val, sizeBytes ? sizeBytes : 4u)) return;
#endif
    // Uncaptured writes decode inline; keep them behind every queued burst.
    GxGpuThread::Sync gpuSync;
    HleFifoConsume(val, sizeBytes);
}

extern "C" void GX_HLE_PrimeConsumeGatherBurst(const uint8_t* data) {
    // Bypass capture for these already committed bytes only. Guest writes
    // reentered by the renderer still use the public producer and get captured.
    for (unsigned i = 0; i < 32; i += 4) HleFifoConsume(ReadBE32(data + i), 4);
}

extern "C" void GX_HLE_FIFO_WriteBurst(const uint8_t* data, uint32_t sizeBytes) {
    if (data == nullptr || sizeBytes == 0) {
        return;
    }

#if defined(MKW_GAME_PROFILE_RM3E01_00)
    // A burst may begin with vertex zeroes while CP reads are stopped. The
    // direct packet walker only sees the decoder queue, not the retained CP
    // bytes, and would discard those zeroes as NOPs before the packet resumes.
    // Route every Prime burst through the same ordered producer as scalar
    // stores. It also honors reentrant consumer ownership and DL recording.
    // Bulk-copied into the open gather burst while capture is on; the rest keeps the old path.
    const uint32_t taken = g_primeFifoReplaying ? 0u : PrimeGpuGatherWriteBulk(data, sizeBytes);
    if (taken < sizeBytes)
        PrimeFifoBurst::Write(data + taken, sizeBytes - taken, [](uint32_t value,unsigned width) { HleFifoWrite(value,width); });
    return;
#endif

    if (IsDisplayListActive() && WriteDisplayListBurst(data, sizeBytes)) {
        return;
    }

    const uint32_t applied = ApplyFifoPacketsDirect(data, sizeBytes);
    if (applied < sizeBytes) {
        HleFifoWriteBurstChunked(data + applied, sizeBytes - applied);
    }
}
