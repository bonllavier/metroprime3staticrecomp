// gx_stream.cpp - Prime GX work carried through the GX stream (see prime_gx_stream.h)
#include "hle/prime_gx_stream.h"
extern "C" const uint8_t* PrimeExpandDeferredDraw(const uint8_t* payload, uint32_t size, uint32_t* wireBytes);

#include "gx_internal.h"
#include "hle/prime_input_latency.h"
#include "hle/prime_tlut_memory.h"
#include "memory.h"
#include "runtime_log.h"

#include <aurora/aurora.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXFrameBuffer.h>

#include <cstring>
#include <sstream>
#include <stdexcept>

extern "C" void GXBindRawTextureMemory(unsigned, const void*, unsigned, unsigned, unsigned, bool);
extern "C" void GXBindRawTexturePalette(unsigned, const void*, unsigned, unsigned, unsigned);
extern "C" uint32_t GXRawDrawTextureMask(GXVtxFmt fmt);
extern "C" void GXCopyTexWithParams(void*, GXBool, u16, u16, u16, u16, u16, u16, u32, GXBool);
extern "C" void GXCopyDispTrackedWithParams(void*, GXBool, u32, u16, u16, u16, u16, u16, u16);
void VI_HLE_SetXfbReady(uint32_t xfbAddr);
void VI_HLE_NotePrimeXfbCopied(uint32_t xfbAddr);

#if defined(MKW_GAME_PROFILE_RM3E01_00)
extern PrimeTlutMemory g_primeTlutMemory;
#endif

namespace PrimeGxStream {
namespace {
template <class T>
T Read(const uint8_t* data, uint32_t size) {
    if (size != sizeof(T)) throw std::runtime_error("Prime GX stream callback payload size mismatch");
    T value;
    std::memcpy(&value, data, sizeof(T));
    return value;
}

template <class T>
void Emit(Id id, const T& payload) {
    aurora_stream_host_callback(static_cast<uint32_t>(id), &payload, sizeof(T));
}

// Decoder side (or the producer when the asynchronous decoder is off): the
// same work the synchronous path did at this point of the stream.
void Dispatch(uint32_t id, const uint8_t* data, uint32_t size) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    switch (static_cast<Id>(id)) {
    case Id::PeToken: {
        const auto token = Read<PeToken>(data, size);
        // Every earlier draw has been decoded: Aurora owns copies of their
        // guest source data, so the CPU may reuse its staging now.
        Memory::Write16(0xCC00100Eu, token.token);
        static unsigned traces = 0;
        if (traces++ < 32) RT_LOG(RT_TAG_GX) << "[prime-pe-token-consumed] token=" << token.token << std::endl;
        return;
    }
    case Id::TexBind: {
        const auto bind = Read<TexBind>(data, size);
        GXBindRawTextureMemory(bind.slot, reinterpret_cast<const void*>(static_cast<uintptr_t>(bind.data)),
                               bind.width, bind.height, bind.format, bind.mipmap != 0);
        return;
    }
    case Id::TlutLoad: {
        const auto load = Read<TlutLoad>(data, size);
        g_primeTlutMemory.Load(load.tmemOffset, reinterpret_cast<const void*>(static_cast<uintptr_t>(load.source)),
                               load.bytes);
        return;
    }
    case Id::BindPalettes: {
        const auto bind = Read<BindPalettes>(data, size);
        const uint32_t sampledMask = GXRawDrawTextureMask(static_cast<GXVtxFmt>(bind.vtxFmt)) & bind.indexedMask;
        for (unsigned slot = 0; slot < 8; ++slot) {
            if ((sampledMask & (1u << slot)) == 0) continue;
            const uint32_t format = bind.formats[slot];
            if (format != 8 && format != 9 && format != 10) continue;
            const uint32_t paletteFormat = (bind.tlutWords[slot] >> 10) & 3u;
            if (paletteFormat > 2) throw std::runtime_error("Invalid raw TLUT color format");
            const unsigned entries = format == 8 ? 16 : format == 9 ? 256 : 16384;
            const uint32_t offset = (bind.tlutWords[slot] & 1023u) << 9;
            const void* palette;
            try { palette = g_primeTlutMemory.Span(offset, entries * 2); }
            catch (const std::runtime_error& error) {
                std::ostringstream message;
                message << error.what() << " slot=" << slot << " format=" << format << " tmem=0x" << std::hex
                        << offset << " sampled_mask=0x" << sampledMask << std::dec << " entries=" << entries;
                throw std::runtime_error(message.str());
            }
            GXBindRawTexturePalette(slot, palette, paletteFormat, entries,
                                    g_primeTlutMemory.RangeGeneration(offset, entries * 2));
        }
        return;
    }
    case Id::TexCopy: {
        const auto copy = Read<TexCopy>(data, size);
        GXCopyTexWithParams(reinterpret_cast<void*>(static_cast<uintptr_t>(copy.dest)), copy.clear ? GX_TRUE : GX_FALSE,
                            copy.left, copy.top, copy.width, copy.height, copy.dstWidth, copy.dstHeight, copy.format,
                            copy.halfScale ? GX_TRUE : GX_FALSE);
        return;
    }
    case Id::XfbCopy: {
        const auto copy = Read<XfbCopy>(data, size);
        GXCopyDispTrackedWithParams(reinterpret_cast<void*>(static_cast<uintptr_t>(copy.dest)),
                                    copy.clear ? GX_TRUE : GX_FALSE, copy.visibleLines, copy.left, copy.top,
                                    copy.width, copy.height, copy.pitch, copy.height);
        PrimeInputLatency::SetDecodingSubmitTime(copy.submitNanos);
        PrimeInputLatency::NoteXfbCopy();
        PrimeInputLatency::SetDecodingSubmitTime(0);
        // The copy exists now: VI may present this framebuffer.
        VI_HLE_SetXfbReady(copy.guestDest);
        VI_HLE_NotePrimeXfbCopied(copy.guestDest);
        return;
    }
    }
#endif
    (void)data;
    (void)size;
    throw std::runtime_error("Unknown Prime GX stream callback id " + std::to_string(id));
}
}  // namespace

bool Async() { return aurora_async_decoder_enabled() && !aurora_on_decoder_thread(); }

void Install() {
    aurora_set_stream_callback(&Dispatch);
    // Packed display-list draws expanded on the decoder thread (gx_dl.cpp).
    aurora_set_deferred_draw_expander(&PrimeExpandDeferredDraw);
}

void EmitPeToken(uint16_t token) {
    Emit(Id::PeToken, PeToken{token});
    // The game polls this token from the CPU: do not let it sit in a half-filled chunk.
    aurora_stream_flush();
}
void EmitTexBind(const TexBind& bind) { Emit(Id::TexBind, bind); }
void EmitTlutLoad(const TlutLoad& load) { Emit(Id::TlutLoad, load); }
void EmitBindPalettes(const BindPalettes& bind) { Emit(Id::BindPalettes, bind); }
void EmitTexCopy(const TexCopy& copy) { Emit(Id::TexCopy, copy); }
void EmitXfbCopy(const XfbCopy& copy) {
    Emit(Id::XfbCopy, copy);
    // A finished frame: start the decoder on it at once.
    aurora_stream_flush();
}
}  // namespace PrimeGxStream
