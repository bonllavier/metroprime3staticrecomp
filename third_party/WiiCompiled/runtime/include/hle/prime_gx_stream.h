#pragma once
// Prime's GX work that touches Aurora decoder state or guest RAM at decode
// time, carried through the GX stream as host callbacks
// (aurora_stream_host_callback) so the asynchronous decoder thread applies
// them in order with the surrounding BP/draw commands. Without the
// asynchronous decoder the callbacks run immediately, on the producer.
//
// Producer side: Emit* helpers, called where the synchronous code used to
// call Aurora directly. Decoder side: PrimeGxStreamCallback.
#include <cstdint>

namespace PrimeGxStream {
// Asynchronous decoder enabled (aurora_set_async_decoder) and this thread is
// a producer: direct Aurora calls would race with the decoder.
bool Async();
// Registers the decoder-side dispatcher with Aurora. Idempotent.
void Install();

enum class Id : uint32_t {
    PeToken = 1,       // PeToken payload
    TexBind = 2,       // TexBind payload
    TlutLoad = 3,      // TlutLoad payload
    BindPalettes = 4,  // BindPalettes payload
    TexCopy = 5,       // TexCopy payload
    XfbCopy = 6,       // XfbCopy payload
};

struct PeToken {
    uint16_t token;
};
struct TexBind {
    uint64_t data;  // host pointer to the guest texture bytes (validated by the producer)
    uint32_t width, height, format;
    uint8_t slot, mipmap;
};
struct TlutLoad {
    uint64_t source;  // host pointer to the guest palette bytes
    uint32_t tmemOffset, bytes;
};
struct BindPalettes {
    uint32_t indexedMask;
    uint32_t tlutWords[8];  // BP 0x98.. / 0xB8.. per slot
    uint8_t formats[8];     // BP texture format per slot
    uint8_t vtxFmt;
};
struct TexCopy {
    uint64_t dest;  // host pointer to the guest destination
    uint32_t format;
    uint16_t left, top, width, height, dstWidth, dstHeight;
    uint8_t halfScale, clear;
};
struct XfbCopy {
    uint64_t dest;         // host pointer to the guest framebuffer
    int64_t submitNanos;   // PrimeInputLatency decoding submit time of the burst
    uint32_t guestDest, visibleLines;
    uint16_t left, top, width, height, pitch;
    uint8_t clear;
};

void EmitPeToken(uint16_t token);
void EmitTexBind(const TexBind& bind);
void EmitTlutLoad(const TlutLoad& load);
void EmitBindPalettes(const BindPalettes& bind);
void EmitTexCopy(const TexCopy& copy);
void EmitXfbCopy(const XfbCopy& copy);
}  // namespace PrimeGxStream
