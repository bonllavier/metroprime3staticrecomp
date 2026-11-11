// gx_copy.cpp - Framebuffer Copy Operations
#include <aurora/aurora.h>
#include "gx_internal.h"

#include "settings_overlay.h"
#include "hle/prime_raw_bp.h"
#include "hle/prime_performance.h"
#include "hle/gx_gpu_thread.h"
#include "hle/prime_gx_stream.h"

#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>
#include <set>
#include <tuple>

#include <xxhash.h>

namespace {
// Copy destinations stay GPU-only until an explicit/lazy readback. Track the
// guest ranges that own those results so a later data-cache flush over a reused
// allocation can retire the stale GPU texture before it is considered by a
// subsequent GXLoadTexObj. There is at most one live range per destination;
// rewriting the destination replaces its previous extent.
//
// A flush only proves the CPU wrote *somewhere* in its range: RM3E01 flushes
// large blocks that contain its character-shadow copy buffers, and dcbf
// writes back dirty lines only, so the GPU result in RAM survives on hardware.
// Each destination keeps per-4 KiB digests of the guest bytes under it at copy
// time; a flush retires the copy only when a page it touches changed (the CPU
// wrote there). Pages keep a 1 MiB screen copy cheap under many small flushes.
constexpr uint32_t kCopyDigestPage = 4096;
struct EfbCopyDestination {
    uint32_t size = 0;
    std::vector<uint64_t> pageDigests;
};
std::mutex g_efbCopyDestinationsMutex;
std::map<uint32_t, EfbCopyDestination> g_efbCopyDestinations;
uint32_t g_largestEfbCopyDestination = 0;

uint64_t GuestBytesDigest(uint32_t addr, uint32_t size) {
    const void* bytes = GuestToHostPtr(addr, size);
    return bytes ? static_cast<uint64_t>(XXH3_64bits(bytes, size)) : 0;
}

// Digest of page `page` of a copy at canonical address `start`.
uint64_t CopyPageDigest(uint32_t start, uint32_t size, uint32_t page) {
    const uint32_t offset = page * kCopyDigestPage;
    return GuestBytesDigest(0x80000000u | (start + offset), std::min(kCopyDigestPage, size - offset));
}

bool CopyBytesChanged(uint32_t start, const EfbCopyDestination& copy, uint64_t dirtyStart, uint64_t dirtyEnd) {
    const uint64_t from = std::max<uint64_t>(dirtyStart, start) - start;
    const uint64_t to = std::min<uint64_t>(dirtyEnd, uint64_t(start) + copy.size) - start;
    for (uint32_t page = uint32_t(from / kCopyDigestPage); page <= uint32_t((to - 1) / kCopyDigestPage); ++page)
        if (page >= copy.pageDigests.size() || CopyPageDigest(start, copy.size, page) != copy.pageDigests[page])
            return true;
    return false;
}

void RememberEfbCopyDestination(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }
    const uint32_t start = CanonicalizeGxMainRamAddress(addr);
    EfbCopyDestination copy{size, {}};
    copy.pageDigests.resize((size + kCopyDigestPage - 1) / kCopyDigestPage);
    for (uint32_t page = 0; page < copy.pageDigests.size(); ++page)
        copy.pageDigests[page] = CopyPageDigest(start, size, page);
    std::lock_guard<std::mutex> guard(g_efbCopyDestinationsMutex);
    g_efbCopyDestinations[start] = std::move(copy);
    // A conservative monotonic maximum lets invalidation jump directly to
    // the only map interval that could overlap instead of walking every copy
    // destination on every DCStoreRange.
    g_largestEfbCopyDestination = std::max(g_largestEfbCopyDestination, size);
}

} // namespace

// Prime's original SDK emits these operations through BP, rather than the
// Mario Kart native overrides. Keep the same GPU destination lifetime rules.
void GX_HLE_PrimeCopyColorTexture(uint32_t destination,uint16_t left,uint16_t top,
    uint16_t width,uint16_t height,const PrimeRawBp::Registers::ColorCopy& copy,bool clear) {
    PrimePerformance::Scope performance(PrimePerformance::ColorCopy);
    if (PrimePerformance::Enabled()) {
        // Geometry only: bounded distinct copy shapes, no guest pixels/dumps.
        // Helps distinguish four EFB regions from a later full-screen pass.
        using Shape=std::tuple<uint16_t,uint16_t,uint16_t,uint16_t,uint16_t,uint16_t,uint32_t,bool,bool>;
        static std::set<Shape> shapes;
        const Shape shape{left,top,width,height,copy.width,copy.height,copy.format,copy.halfScale,clear};
        if (shapes.size()<128 && shapes.insert(shape).second) {
            RT_LOG(RT_TAG_GX)<<"[prime-screen-copy-shape] source="<<left<<","<<top<<","<<width<<","<<height
                <<" destination="<<copy.width<<"x"<<copy.height<<" format="<<copy.format
                <<" half="<<copy.halfScale<<" clear="<<clear<<std::endl;
        }
    }
    if (aurora_draw_dump_active())
        RT_LOG(RT_TAG_GX)<<"[dump-copy] format="<<copy.format<<" source="<<left<<","<<top<<","<<width<<","<<height
            <<" destination=0x"<<std::hex<<destination<<std::dec<<std::endl;
    Memory::GetPointer(destination,copy.bytes);
    EnsureAuroraFrameActive();
    if (PrimeGxStream::Async()) {
        // Copy parameters travel with the command; the decoder thread resolves
        // it after every earlier draw, as GXDrawDone ordered it here.
        PrimeGxStream::EmitTexCopy({reinterpret_cast<uintptr_t>(GuestToHostPtr(destination)), copy.format, left, top,
                                    width, height, copy.width, copy.height, static_cast<uint8_t>(copy.halfScale),
                                    static_cast<uint8_t>(clear)});
    } else {
        GXDrawDone();
        GXSetTexCopySrc(left,top,width,height);
        GXSetTexCopyDst(copy.width,copy.height,static_cast<GXTexFmt>(copy.format),copy.halfScale ? GX_TRUE : GX_FALSE);
        GXCopyTex(GuestToHostPtr(destination),clear ? GX_TRUE : GX_FALSE);
    }
    RememberEfbCopyDestination(destination,copy.bytes);
    GXMarkFrameWork();
}

void InvalidateEfbCopyDestinationsForRange(uint32_t addr, uint32_t size) {
    // With the GX consumer thread, retire copies in stream order relative to
    // the bursts already queued, exactly where the synchronous decoder would.
    if (size == 0 || GxGpuThread::QueueInvalidate(addr, size)) {
        return;
    }
    InvalidateEfbCopyDestinationsForRangeNow(addr, size);
}

void InvalidateEfbCopyDestinationsForRangeNow(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }

    const uint64_t dirtyStart = CanonicalizeGxMainRamAddress(addr);
    const uint64_t dirtyEnd = dirtyStart + size;
    std::vector<uint32_t> retired;
    {
        std::lock_guard<std::mutex> guard(g_efbCopyDestinationsMutex);
        const uint64_t earliestCandidate =
            dirtyStart > g_largestEfbCopyDestination ? dirtyStart - g_largestEfbCopyDestination : 0;
        for (auto it = g_efbCopyDestinations.lower_bound(static_cast<uint32_t>(earliestCandidate));
             it != g_efbCopyDestinations.end() && static_cast<uint64_t>(it->first) < dirtyEnd;) {
            const uint64_t copyStart = it->first;
            const uint64_t copyEnd = copyStart + it->second.size;
            if (dirtyEnd <= copyStart || dirtyStart >= copyEnd) {
                ++it;
                continue;
            }
            // Flushed but untouched: the GPU copy is still what RAM holds.
            if (!CopyBytesChanged(it->first, it->second, dirtyStart, dirtyEnd)) {
                ++it;
                continue;
            }
            retired.push_back(it->first);
            it = g_efbCopyDestinations.erase(it);
        }
    }

    // Preserve FIFO ordering: the destroy command is emitted before any later
    // texture load that can consume the freshly flushed RAM bytes.
    for (const uint32_t copyAddr : retired) {
        GXDestroyCopyTex(GuestToHostPtr(copyAddr));
    }
}

// ============================================================================
// Display Copy Source/Destination
// ============================================================================

extern "C" void GX__SetDispCopySrc_8016f438(uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    GXSetDispCopySrc((u16)l, (u16)t, (u16)w, (u16)h);
}
PPC_NATIVE_OVERRIDE_VOID(8016f438, GX__SetDispCopySrc_8016f438, (uint32_t l, uint32_t t, uint32_t w, uint32_t h), (l, t, w, h));

extern "C" void GX__SetDispCopyDst_8016f4b8(uint32_t w, uint32_t h) { GXSetDispCopyDst((u16)w, (u16)h); }
PPC_NATIVE_OVERRIDE_VOID(8016f4b8, GX__SetDispCopyDst_8016f4b8, (uint32_t w, uint32_t h), (w, h));

// ============================================================================
// Texture Copy Source/Destination
// ============================================================================

extern "C" void GX__SetTexCopySrc_8016f478(uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    GXSetTexCopySrc((u16)l, (u16)t, (u16)w, (u16)h);
    g_texCopyState.srcLeft=(u16)l; g_texCopyState.srcTop=(u16)t;
    g_texCopyState.srcWidth=(u16)w; g_texCopyState.srcHeight=(u16)h;
}
PPC_NATIVE_OVERRIDE_VOID(8016f478, GX__SetTexCopySrc_8016f478, (uint32_t l, uint32_t t, uint32_t w, uint32_t h), (l, t, w, h));

extern "C" void GX__SetTexCopyDst_8016f4dc(uint32_t w, uint32_t h, uint32_t f, uint32_t m) {
    GXSetTexCopyDst((u16)w, (u16)h, (GXTexFmt)f, (GXBool)m);
    g_texCopyState.dstWidth=(u16)w; g_texCopyState.dstHeight=(u16)h;
    g_texCopyState.dstFormat=f; g_texCopyState.dstMipmap=m;
}
PPC_NATIVE_OVERRIDE_VOID(8016f4dc, GX__SetTexCopyDst_8016f4dc, (uint32_t w, uint32_t h, uint32_t f, uint32_t m), (w, h, f, m));

extern "C" void GX__SetCopyFilter_8016fa40(uint32_t aa, uint32_t spa, uint32_t vf, uint32_t vfa) {
    uint8_t sp[12][2]={}, vfb[7]={};
    if(spa) std::memcpy(sp, GuestToHostPtr(spa, 24), 24);
    if(vfa) std::memcpy(vfb, GuestToHostPtr(vfa, 7), 7);
    GXSetCopyFilter((GXBool)aa, sp, (GXBool)vf, vfb);
}
PPC_NATIVE_OVERRIDE_VOID(8016fa40, GX__SetCopyFilter_8016fa40, (uint32_t aa, uint32_t spa, uint32_t vf, uint32_t vfa), (aa, spa, vf, vfa));

extern "C" void GX__SetDispCopyGamma_8016fc24(uint32_t g) { GXSetDispCopyGamma((GXGamma)g); }
PPC_NATIVE_OVERRIDE_VOID(8016fc24, GX__SetDispCopyGamma_8016fc24, (uint32_t g), (g));

// ============================================================================
// Copy Execution
// ============================================================================

extern "C" void GxPrimeNoteDisplayCopy();
extern "C" void GX__CopyDisp_8016fc38(uint32_t da, uint32_t c) {
    EnsureAuroraFrameActive();
    // GX copies are FIFO-ordered on hardware. Drain submitted draws before
    // resolving the EFB so high-level copies see the same contents.
    GXDrawDone();
    GXCopyDisp(GuestToHostPtr(da), (GXBool)c);
    // No second GXDrawDone here: the frame-worker wait below is for the DONE
    // phase, which strictly subsumes the drain this call would perform.
    ++g_gxFrameCount;
    GxPrimeNoteDisplayCopy();
    VI_HLE_SetXfbReady(da);
    // Present immediately so post-copy draws don't leak into this frame. Join at the DONE phase
    // (not the cheaper SEALED phase GXDrawDone waits for) because ImGui's draw lists, owned by
    // Aurora's render worker, replay during encode; aurora_end_frame would join here anyway.
    aurora_wait_for_frame_worker();
    settings_overlay::Draw();
    // Seal, pace to the VI retrace boundary (Aurora renders the sealed frame
    // during the wait), and pre-warm the next frame.
    VI_HLE_PresentFrame(/*presentedXfb=*/true, /*paceToRetrace=*/true);
}

PPC_NATIVE_OVERRIDE_VOID(8016fc38, GX__CopyDisp_8016fc38, (uint32_t da, uint32_t c), (da, c));


extern "C" void GX__CopyTex_8016fd74(uint32_t da, uint32_t c) {
    EnsureAuroraFrameActive();
    // Match GX FIFO ordering: texture copies observe all prior draws.
    GXDrawDone();
    const uint16_t rawSrcLeft = g_texCopyState.srcLeft;
    const uint16_t rawSrcTop = g_texCopyState.srcTop;
    const uint16_t rawSrcWidth = g_texCopyState.srcWidth;
    const uint16_t rawSrcHeight = g_texCopyState.srcHeight;

    // Keep the source in guest EFB coordinates. Aurora maps it to the scaled
    // EFB exactly once, matching Dolphin's ConvertEFBRectangle path.
    GXSetTexCopySrc(rawSrcLeft, rawSrcTop, rawSrcWidth, rawSrcHeight);
    // EFB copies stay GPU-only except probe-sized ones (e.g. the 4x4 lens-flare depth probe),
    // which Aurora reads back asynchronously via efb_ram::schedule and land in guest RAM a frame
    // later. RISK: copies above the probe threshold, or on the offscreen list, are not
    // auto-downloaded, so guest reads see stale RAM; call aurora_flush_efb_copies_to_ram if a
    // copy needs reading back.
    GXCopyTex(GuestToHostPtr(da), (GXBool)c);
    RememberEfbCopyDestination(
        da, GXGetTexBufferSize(g_texCopyState.dstWidth, g_texCopyState.dstHeight,
                               g_texCopyState.dstFormat, GX_FALSE, 0));
    GXSetTexCopySrc(rawSrcLeft, rawSrcTop, rawSrcWidth, rawSrcHeight);
}
PPC_NATIVE_OVERRIDE_VOID(8016fd74, GX__CopyTex_8016fd74, (uint32_t da, uint32_t c), (da, c));
