#include "gx_internal.h"
#include "gx_stream_common.h"
#include "gx_cp_decode.h"
#include "isa/big_endian.h"
#include "runtime_log.h"
#include "recomp_mod_loader.h"
#include "hle/prime_dl_vertex_expansion.h"
#include "hle/prime_deferred_draw.h"
#include "hle/prime_gx_stream.h"
extern "C" void GXAuroraPublishDirtyState(void);
#include "hle/gx_fifo_storage.h"
#include <aurora/aurora.h>

#include <cstdlib>
#include <sstream>
#include <unordered_map>

// The display-list scan cache validates a cached scan against the live guest
// bytes with a 64-bit XXH3 digest (see GxDisplayListScanCache::CanReuse).
#include <xxhash.h>
#include "hle/prime_dl_vertex_cache.h"

// Diagnostic environment switches are fixed for the lifetime of this process.
static bool CachedWiiCompiledTracePrimeMii() {
  static const bool enabled = std::getenv("WII_COMPILED_TRACE_PRIME_MII") != nullptr;
  return enabled;
}


namespace aurora::gx::fifo {
bool in_display_list();
void write_data_bulk(const void* data,uint32_t length);
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);
}

namespace {

// Opcode constants and the stream helpers this file shares with gx_fifo.cpp /
// gx_vertex.cpp; see gx_stream_common.h.
using namespace GxCmd;
using namespace GxStream;
using GxCpDecode::ApplyCpRegWrite;
using GxCpDecode::SameVtxAttrFmt;

// Small display lists dominate the in-race call count. Cache them as well, but
// cap both individual entries and aggregate copied command bytes so malformed
// guest input cannot turn this optimization into unbounded host allocation.
constexpr uint32_t kDlScanCacheMaxEntryBytes = 64u * 1024u;
constexpr size_t kDlScanCacheMaxEntries = 8192;
constexpr size_t kDlScanCacheMaxStoredBytes = 8u * 1024u * 1024u;

// Display-list write tracking (audit F6a): re-digesting every list every call is the
// costliest step of GX__CallDisplayList, and wasted on BRRES shape lists that are written
// once at load and never touched again.
constexpr uint64_t kDlWriteGenerationUntracked = GxGuestWrite::kUntracked;

// Byte footprint of one vertex of the currently configured layout. Indexed attributes are
// sized before the matrix-attribute check here (unlike BuildScanVertexLayout), but a
// GX_INDEX16 matrix attribute is impossible on real hardware so both are unreachable.
static uint32_t CalcDLVertexSize(GXVtxFmt vtxfmt) {
    uint32_t size = 0;
    for (int attr = 0; attr < 26; ++attr) {
        const auto type = g_hleGxState.vtxDesc[attr];
        if (type == GX_NONE) continue;
        const auto& fmt = g_hleGxState.vtxAttrFmt[vtxfmt][attr];
        if (type == GX_INDEX8) size += (attr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
        else if (type == GX_INDEX16) size += ((attr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u) * 2u;
        else if (type == GX_DIRECT) {
            size += DirectAttrByteSize(static_cast<GXAttr>(attr), fmt, /*matrixAttrIsOneByte=*/true,
                                       /*fallbackComps=*/0u);
        }
    }
    return size;
}

static void ApplyAuroraVtxDesc();
static void ApplyAuroraVtxAttrFmtForDisplayList(GXVtxFmt fmt, bool mixed);
static void SyncAppliedVtxStateFromHleReal();
static void SubmitLytDrawPacket(const uint8_t* packet, uint32_t packetBytes);

static inline uint8_t ScaleLytAlpha(uint8_t alpha, uint32_t modulate) {
    const int32_t product = static_cast<int32_t>(alpha) * static_cast<int32_t>(modulate & 0xFFu);
    const int32_t magic = static_cast<int32_t>(0x80808081u);
    int32_t quotient = static_cast<int32_t>((static_cast<int64_t>(magic) * product) >> 32);
    quotient = static_cast<int32_t>(quotient + product) >> 7;
    quotient += (static_cast<uint32_t>(quotient) >> 31);
    return static_cast<uint8_t>(quotient);
}

static inline uint32_t ReadLytColor(uint32_t colorAddr, int index) {
    return Memory::Read32(colorAddr + static_cast<uint32_t>(index * 4));
}

static inline uint32_t ReadModulatedLytColor(uint32_t colorAddr, int index, uint32_t alpha) {
    const uint32_t addr = colorAddr + static_cast<uint32_t>(index * 4);
    const uint8_t r = Memory::Read8(addr);
    const uint8_t g = Memory::Read8(addr + 1);
    const uint8_t b = Memory::Read8(addr + 2);
    uint8_t a = Memory::Read8(addr + 3);
    if ((alpha & 0xFFu) != 0xFFu) {
        a = ScaleLytAlpha(a, alpha);
    }
    return (static_cast<uint32_t>(r) << 24) |
           (static_cast<uint32_t>(g) << 16) |
           (static_cast<uint32_t>(b) << 8) |
           static_cast<uint32_t>(a);
}

static inline void AppendLytTexCoords(uint8_t* packet, uint32_t& pos, uint32_t texCoordAddr,
                                      int texCoordCount, uint32_t cornerOffset) {
    if (texCoordCount <= 0) {
        return;
    }
    for (int i = 0; i < texCoordCount; ++i) {
        const uint32_t coordAddr = texCoordAddr + static_cast<uint32_t>(i * 32) + cornerOffset;
        BigEndian::AppendFloat32(packet, pos, Memory::ReadFloat32(coordAddr));
        BigEndian::AppendFloat32(packet, pos, Memory::ReadFloat32(coordAddr + 4));
    }
}

static inline void AppendLytQuadVertex(uint8_t* packet, uint32_t& pos, float x, float y,
                                       uint32_t texCoordAddr, int texCoordCount,
                                       uint32_t cornerOffset, const uint32_t* colors, int colorIndex) {
    BigEndian::AppendFloat32(packet, pos, x);
    BigEndian::AppendFloat32(packet, pos, y);
    if (colors) {
        BigEndian::Append32(packet, pos, colors[colorIndex]);
    }
    AppendLytTexCoords(packet, pos, texCoordAddr, texCoordCount, cornerOffset);
}

// The four corners of an nw4r::lyt quad, in the order GX_QUADS expects, with
// each corner's texcoord byte offset and colour index. The winding order is
// load-bearing and both emit paths below share this one copy of it.
static inline void AppendLytQuadVertices(uint8_t* packet, uint32_t& pos, float x0, float y0,
                                         float x1, float y1, uint32_t texCoordAddr,
                                         int texCoordCount, const uint32_t* colors) {
    AppendLytQuadVertex(packet, pos, x0, y0, texCoordAddr, texCoordCount, 0, colors, 0);
    AppendLytQuadVertex(packet, pos, x1, y0, texCoordAddr, texCoordCount, 8, colors, 1);
    AppendLytQuadVertex(packet, pos, x1, y1, texCoordAddr, texCoordCount, 24, colors, 3);
    AppendLytQuadVertex(packet, pos, x0, y1, texCoordAddr, texCoordCount, 16, colors, 2);
}

static bool CanSubmitLytDrawDirect(int texCoordCount, const uint32_t* colors) {
    if (texCoordCount < 0 || texCoordCount > 8) {
        return false;
    }
    const auto& posFmt = g_hleGxState.vtxAttrFmt[GX_VTXFMT0][GX_VA_POS];
    if (g_hleGxState.vtxDesc[GX_VA_POS] != GX_DIRECT ||
        posFmt.cnt != GX_POS_XY ||
        posFmt.type != GX_F32) {
        return false;
    }

    const bool hasColors = colors != nullptr;
    const auto& clrFmt = g_hleGxState.vtxAttrFmt[GX_VTXFMT0][GX_VA_CLR0];
    if (hasColors) {
        if (g_hleGxState.vtxDesc[GX_VA_CLR0] != GX_DIRECT ||
            clrFmt.cnt != GX_CLR_RGBA ||
            clrFmt.type != GX_RGBA8) {
            return false;
        }
    } else if (g_hleGxState.vtxDesc[GX_VA_CLR0] != GX_NONE) {
        return false;
    }

    if (g_hleGxState.vtxDesc[GX_VA_NRM] != GX_NONE ||
        g_hleGxState.vtxDesc[GX_VA_CLR1] != GX_NONE) {
        return false;
    }

    for (int attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7MTXIDX; ++attr) {
        if (g_hleGxState.vtxDesc[attr] != GX_NONE) {
            return false;
        }
    }

    for (int i = 0; i < 8; ++i) {
        const GXAttr attr = static_cast<GXAttr>(GX_VA_TEX0 + i);
        const auto& fmt = g_hleGxState.vtxAttrFmt[GX_VTXFMT0][attr];
        if (i < texCoordCount) {
            if (g_hleGxState.vtxDesc[attr] != GX_DIRECT ||
                fmt.cnt != GX_TEX_ST ||
                fmt.type != GX_F32) {
                return false;
            }
        } else if (g_hleGxState.vtxDesc[attr] != GX_NONE) {
            return false;
        }
    }

    return true;
}

static bool SubmitLytDrawDirect(float x0, float y0, float x1, float y1, int texCoordCount,
                                uint32_t texCoordAddr, const uint32_t* colors) {
    if (!CanSubmitLytDrawDirect(texCoordCount, colors)) {
        return false;
    }


    EnsureAuroraFrameActive();

    ApplyAuroraVtxDesc();

    ApplyAuroraVtxAttrFmtForDisplayList(GX_VTXFMT0, false);

    EnsureDefaultGxAlphaCompare();


    std::array<uint8_t, 4u * (8u + 4u + 8u * 8u)> vertices{};
    uint32_t pos = 0;
    AppendLytQuadVertices(vertices.data(), pos, x0, y0, x1, y1, texCoordAddr, texCoordCount, colors);

    if (!aurora::gx::fifo::submit_raw_draw(GX_QUADS, GX_VTXFMT0, vertices.data(), 4, pos)) {
        return false;
    }
    GXMarkFrameWork();

    SyncAppliedVtxStateFromHleReal();
    return true;
}

static inline void EmitLytDrawQuad(uint32_t posAddr, uint32_t sizeAddr, int texCoordCount,
                                   uint32_t texCoordAddr, const uint32_t* colors) {
    const float x0 = Memory::ReadFloat32(posAddr);
    const float y0 = Memory::ReadFloat32(posAddr + 4);
    // The casts pin the arithmetic to PPC single precision regardless of the
    // host's FLT_EVAL_METHOD.
    const float x1 = static_cast<float>(x0 + Memory::ReadFloat32(sizeAddr));
    const float y1 = static_cast<float>(y0 - Memory::ReadFloat32(sizeAddr + 4));

    if (SubmitLytDrawDirect(x0, y0, x1, y1, texCoordCount, texCoordAddr, colors)) {
        return;
    }

    // GX has exactly 8 texture coordinates, so nw4r::lyt cannot ask for more.
    // The fixed packet buffer below is sized for that maximum; bail rather than
    // overrun it if the guest ever hands us something else.
    if (texCoordCount > 8) {
        return;
    }

    std::array<uint8_t, 3u + 4u * (8u + 4u + 8u * 8u)> packet{};
    uint32_t pos = 0;
    packet[pos++] = GX_DRAW_QUADS_CMD | GX_VTXFMT0;
    BigEndian::Append16(packet.data(), pos, 4);
    AppendLytQuadVertices(packet.data(), pos, x0, y0, x1, y1, texCoordAddr, texCoordCount, colors);
    SubmitLytDrawPacket(packet.data(), pos);
}

static uint32_t SubmitDLVertex(const uint8_t* ptr, GXVtxFmt vtxfmt, const GXAttrType* sourceVtxDesc) {
    uint32_t offset = 0;
    for (int attr = 0; attr < 26; ++attr) {
        const auto type = sourceVtxDesc[attr]; if (type == GX_NONE) continue;
        const auto& fmt = g_hleGxState.vtxAttrFmt[vtxfmt][attr];
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        if (type == GX_INDEX8) {
            const uint32_t indexCount = (gxAttr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
            if (gxAttr == GX_VA_NRM && indexCount == 3u) {
                const uint32_t indices[3] = {ptr[offset], ptr[offset + 1], ptr[offset + 2]};
                SubmitIndexedNormalNBT3(indices, fmt);
            } else {
                SubmitIndexedAttribute(gxAttr, ptr[offset]);
            }
            offset += indexCount;
        }
        else if (type == GX_INDEX16) {
            const uint32_t indexCount = (gxAttr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
            if (gxAttr == GX_VA_NRM && indexCount == 3u) {
                const uint32_t indices[3] = {
                    ReadBE16(ptr + offset),
                    ReadBE16(ptr + offset + 2),
                    ReadBE16(ptr + offset + 4),
                };
                SubmitIndexedNormalNBT3(indices, fmt);
            } else {
                SubmitIndexedAttribute(gxAttr, ReadBE16(ptr + offset));
            }
            offset += indexCount * 2u;
        }
        else if (type == GX_DIRECT) {
            float comps[9]{};
            u32 rawComps[9]{};
            switch (attr) {
            case GX_VA_PNMTXIDX: {
                const u8 idx = ptr[offset];
                offset += 1;
                GXMatrixIndex1u8(gxAttr, idx);
                break;
            }
            case GX_VA_TEX0MTXIDX: case GX_VA_TEX1MTXIDX: case GX_VA_TEX2MTXIDX:
            case GX_VA_TEX3MTXIDX: case GX_VA_TEX4MTXIDX: case GX_VA_TEX5MTXIDX:
            case GX_VA_TEX6MTXIDX: case GX_VA_TEX7MTXIDX:
                GXMatrixIndex1u8(gxAttr, ptr[offset]);
                offset += 1;
                break;
            case GX_VA_POS: {
                int count = static_cast<int>(AttrCompCount(GX_VA_POS, fmt, 0u)), compSize = GetCompSizeBytes(fmt.type);
                for (int i = 0; i < count; ++i) {
                    rawComps[i] = ReadBECompRaw(ptr + offset, fmt.type);
                    comps[i] = ConvertCompToFloat(rawComps[i], fmt.type, fmt.frac);
                    offset += compSize;
                }
                SubmitAttribute(gxAttr, comps, fmt, rawComps);
                break;
            }
            case GX_VA_NRM: {
                int count = static_cast<int>(AttrCompCount(GX_VA_NRM, fmt, 0u)), compSize = GetCompSizeBytes(fmt.type);
                for (int i = 0; i < count; ++i) {
                    rawComps[i] = ReadBECompRaw(ptr + offset, fmt.type);
                    comps[i] = ConvertCompToFloat(rawComps[i], fmt.type, fmt.frac);
                    offset += compSize;
                }
                SubmitAttribute(gxAttr, comps, fmt, rawComps);
                break;
            }
            case GX_VA_CLR0: case GX_VA_CLR1: {
                const uint32_t bytes = ColorByteSize(fmt.type, fmt.cnt);
                SubmitPackedColor(ptr + offset, bytes);
                offset += bytes;
                break;
            }
            case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3: case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7: {
                // All eight GX_VA_TEXn share one component layout.
                int count = static_cast<int>(AttrCompCount(GX_VA_TEX0, fmt, 0u)), compSize = GetCompSizeBytes(fmt.type);
                for (int i = 0; i < count; ++i) {
                    rawComps[i] = ReadBECompRaw(ptr + offset, fmt.type);
                    comps[i] = ConvertCompToFloat(rawComps[i], fmt.type, fmt.frac);
                    offset += compSize;
                }
                SubmitAttribute(gxAttr, comps, fmt, rawComps);
                break;
            }
            default: break;
            }
        }
    }
    return offset;
}

static void ApplyAuroraVtxStateForDlBegin(GXVtxFmt fmt) {
    // Only this publish site latches the format into the HLE state; the FIFO and
    // immediate-mode sites have already done it by the time they publish.
    g_hleGxState.currentVtxFmt = fmt;

    PublishAuroraVtxState(fmt, AuroraVtxPublishOptions{/*includeNbt=*/false,
                                                       /*fmtLoopFirst=*/0,
                                                       /*fmtLoopLast=*/25});
}


struct HleGxVertexStateSnapshot {
    GXAttrType vtxDesc[26];
    VtxAttrFmt vtxAttrFmt[8][26];
    HleGxState::VtxArray vtxArray[26];
    GXVtxFmt currentVtxFmt;
    uint64_t vtxLayoutHash;
    bool vtxLayoutHashDirty;
    // Bit per vtxAttrFmt row carried in this snapshot.
    uint32_t vtxAttrFmtRows;
    bool hasVtxDesc;
    bool hasVtxArray;
    bool hasCurrentVtxFmt;
};

constexpr uint32_t kAllVtxAttrFmtRows = 0xFFu;

static HleGxVertexStateSnapshot CaptureGxVertexState() {
    HleGxVertexStateSnapshot snapshot;
    std::memcpy(snapshot.vtxDesc, g_hleGxState.vtxDesc, sizeof(snapshot.vtxDesc));
    std::memcpy(snapshot.vtxAttrFmt, g_hleGxState.vtxAttrFmt, sizeof(snapshot.vtxAttrFmt));
    std::memcpy(snapshot.vtxArray, g_hleGxState.vtxArray, sizeof(snapshot.vtxArray));
    snapshot.currentVtxFmt = g_hleGxState.currentVtxFmt;
    snapshot.vtxLayoutHash = g_hleGxState.vtxLayoutHash;
    snapshot.vtxLayoutHashDirty = g_hleGxState.vtxLayoutHashDirty;
    snapshot.vtxAttrFmtRows = kAllVtxAttrFmtRows;
    snapshot.hasVtxDesc = true;
    snapshot.hasVtxArray = true;
    snapshot.hasCurrentVtxFmt = true;
    return snapshot;
}

static void RestoreGxVertexState(const HleGxVertexStateSnapshot& snapshot) {
    if (snapshot.hasVtxDesc) {
        std::memcpy(g_hleGxState.vtxDesc, snapshot.vtxDesc, sizeof(snapshot.vtxDesc));
    }
    if (snapshot.vtxAttrFmtRows == kAllVtxAttrFmtRows) {
        std::memcpy(g_hleGxState.vtxAttrFmt, snapshot.vtxAttrFmt, sizeof(snapshot.vtxAttrFmt));
    } else {
        for (uint32_t fmt = 0; fmt < 8; ++fmt) {
            if ((snapshot.vtxAttrFmtRows & (1u << fmt)) == 0) {
                continue;
            }
            std::memcpy(g_hleGxState.vtxAttrFmt[fmt], snapshot.vtxAttrFmt[fmt],
                        sizeof(snapshot.vtxAttrFmt[fmt]));
        }
    }
    if (snapshot.hasVtxArray) {
        std::memcpy(g_hleGxState.vtxArray, snapshot.vtxArray, sizeof(snapshot.vtxArray));
    }
    if (snapshot.hasCurrentVtxFmt) {
        g_hleGxState.currentVtxFmt = snapshot.currentVtxFmt;
    }
    g_hleGxState.vtxLayoutHash = snapshot.vtxLayoutHash;
    g_hleGxState.vtxLayoutHashDirty = snapshot.vtxLayoutHashDirty;
    // The wholesale desc/attr-fmt overwrite above bypasses the per-field change
    // checks that normally bump this, so mirrors must be told to resync.
    ++g_hleGxState.vtxStateGeneration;
}

struct ScanAttrStep {
    GXAttr attr = GX_VA_NULL;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool indexed = false;
    bool index16 = false;
};

struct ScanLayoutCacheEntry {
    // Only the indexed steps are consumed (ScanDLVerticesMaxIndices); the scan
    // has no use for the direct ones beyond their contribution to vertexSize.
    std::array<ScanAttrStep, 26> indexedSteps{};
    uint32_t indexedStepCount = 0;
    uint32_t vertexSize = 0;
    uint32_t version = 0;
    uint32_t generation = 0;
    bool valid = false;
};


struct DlCpWrite {
    uint8_t reg = 0;
    uint32_t value = 0;
};


static HleGxVertexStateSnapshot CaptureGxVertexStateForCpWrites(const std::vector<DlCpWrite>& writes) {
    HleGxVertexStateSnapshot snapshot;
    snapshot.vtxAttrFmtRows = 0;
    snapshot.hasVtxDesc = false;
    snapshot.hasVtxArray = false;
    snapshot.hasCurrentVtxFmt = false;
    for (const auto& write : writes) {
        const uint8_t reg = write.reg;
        if (reg == 0x50 || reg == 0x60) {
            snapshot.hasVtxDesc = true;
        } else if (reg >= 0x70 && reg <= 0x77) {
            snapshot.vtxAttrFmtRows |= 1u << (reg - 0x70);
        } else if (reg >= 0x80 && reg <= 0x87) {
            snapshot.vtxAttrFmtRows |= 1u << (reg - 0x80);
        } else if (reg >= 0x90 && reg <= 0x97) {
            snapshot.vtxAttrFmtRows |= 1u << (reg - 0x90);
        } else if (reg >= 0xA0 && reg <= 0xBF) {
            snapshot.hasVtxArray = true;
        }
    }
    if (snapshot.hasVtxDesc) {
        std::memcpy(snapshot.vtxDesc, g_hleGxState.vtxDesc, sizeof(snapshot.vtxDesc));
    }
    for (uint32_t fmt = 0; fmt < 8; ++fmt) {
        if ((snapshot.vtxAttrFmtRows & (1u << fmt)) == 0) {
            continue;
        }
        std::memcpy(snapshot.vtxAttrFmt[fmt], g_hleGxState.vtxAttrFmt[fmt],
                    sizeof(snapshot.vtxAttrFmt[fmt]));
    }
    if (snapshot.hasVtxArray) {
        std::memcpy(snapshot.vtxArray, g_hleGxState.vtxArray, sizeof(snapshot.vtxArray));
    }
    snapshot.vtxLayoutHash = g_hleGxState.vtxLayoutHash;
    snapshot.vtxLayoutHashDirty = g_hleGxState.vtxLayoutHashDirty;
    return snapshot;
}

struct DlScanCacheEntry {
    uint32_t listAddr = 0;
    uint32_t nbytes = 0;
    uint64_t layoutHash = 0;
    std::array<uint32_t, GX_VA_MAX_ATTR> maxIdx{};
    std::array<bool, GX_VA_MAX_ATTR> sawIdx{};
    std::array<uint32_t, GX_VA_MAX_ATTR> maxXfIdx{};
    std::array<uint32_t, GX_VA_MAX_ATTR> maxXfBytes{};
    std::array<bool, GX_VA_MAX_ATTR> sawXfIdx{};
    GXVtxFmt dlVtxFmt = GX_MAX_VTXFMT;
    bool dlVtxFmtMixed = false;
    bool dlHasNestedDl = false;
    bool dlHasArrayStateWrites = false;
    bool dlHasCpWrites = false;
    bool scanOk = false;
    // Flatten results for lists that need one. `flattenVerbatim` means the
    // flattened stream is byte-identical to the source list, so aurora can be
    // handed the guest bytes directly and no copy has to be stored.
    bool needsFlatten = false;
    bool flattenOk = false;
    bool flattenVerbatim = false;
};

struct DlScanCacheRecord {
    DlScanCacheEntry result{};
    // Answer of DisplayListMayContainDraw for these bytes
    bool mayContainDraw = false;
    uint64_t contentDigest = 0;
    uint64_t writeGeneration = kDlWriteGenerationUntracked;
    std::vector<DlCpWrite> cpWrites{};
    std::vector<uint8_t> flattened{};
};

struct DlScanCacheState {
    std::unordered_map<uint64_t, DlScanCacheRecord> entries{};
    size_t storedCommandBytes = 0;
};

static uint64_t HashScanLayoutState() {
    if (!g_hleGxState.vtxLayoutHashDirty) {
        return g_hleGxState.vtxLayoutHash;
    }
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](uint32_t value) {
        hash ^= static_cast<uint64_t>(value);
        hash *= 1099511628211ull;
    };
    for (int attr = 0; attr < 26; ++attr) {
        mix(static_cast<uint32_t>(g_hleGxState.vtxDesc[attr]));
    }
    for (int fmt = 0; fmt < 8; ++fmt) {
        for (int attr = 0; attr < 26; ++attr) {
            const auto& f = g_hleGxState.vtxAttrFmt[fmt][attr];
            mix(static_cast<uint32_t>(f.cnt));
            mix(static_cast<uint32_t>(f.type));
            mix(static_cast<uint32_t>(f.frac));
        }
    }
    g_hleGxState.vtxLayoutHash = hash;
    g_hleGxState.vtxLayoutHashDirty = false;
    return g_hleGxState.vtxLayoutHash;
}

static uint64_t DlScanCacheKey(uint32_t listAddr, uint32_t nbytes, uint64_t layoutHash) {
    return GxDisplayListScanCache::MakeIdentityKey(listAddr, nbytes, layoutHash);
}

static DlScanCacheState& DlScanCache() {
    static thread_local DlScanCacheState s_cache;
    return s_cache;
}

static uint64_t DlContentDigest(const uint8_t* list, uint32_t nbytes) {
    return static_cast<uint64_t>(XXH3_64bits(list, static_cast<size_t>(nbytes)));
}


struct DlScanCacheProbe {
    const DlScanCacheRecord* record = nullptr;
    uint64_t contentDigest = 0;
    bool digestValid = false;
    uint64_t writeGeneration = kDlWriteGenerationUntracked;
};

static DlScanCacheProbe ProbeDlScanCache(const uint8_t* list, uint32_t listAddr, uint32_t nbytes,
                                         uint64_t layoutHash) {
    DlScanCacheProbe probe{};
    probe.writeGeneration = GxGuestWrite::GenerationForRange(listAddr, nbytes);

    auto& s_cache = DlScanCache();
    const uint64_t key = DlScanCacheKey(listAddr, nbytes, layoutHash);
    const auto it = s_cache.entries.find(key);
    if (it != s_cache.entries.end()) {
        auto& record = it->second;
        const auto& entry = record.result;
        const bool identityMatches = entry.listAddr == CanonicalizeGxMainRamAddress(listAddr) &&
                                     entry.nbytes == nbytes && entry.layoutHash == layoutHash;
        if (identityMatches && probe.writeGeneration != kDlWriteGenerationUntracked &&
            record.writeGeneration == probe.writeGeneration) {
            // Nothing has flushed a write over these bytes since the digest was
            // taken, so the stored digest still describes them. Skip XXH3.
            probe.record = &record;
            probe.contentDigest = record.contentDigest;
            probe.digestValid = true;
        } else {
            probe.contentDigest = DlContentDigest(list, nbytes);
            probe.digestValid = true;
            if (identityMatches &&
                GxDisplayListScanCache::CanReuse(entry.listAddr, entry.nbytes, entry.layoutHash,
                                                 record.contentDigest, listAddr, nbytes, layoutHash,
                                                 probe.contentDigest)) {
                // False alarm: the bump did not actually change this list
                record.writeGeneration = probe.writeGeneration;
                probe.record = &record;
            }
        }
    }
    return probe;
}

static size_t DlScanCacheRecordBytes(const DlScanCacheRecord& record) {
    return record.flattened.size() + record.cpWrites.size() * sizeof(DlCpWrite);
}

static void StoreDlScanCache(uint32_t listAddr, uint32_t nbytes, uint64_t layoutHash,
                             uint64_t contentDigest, uint64_t writeGeneration,
                             bool mayContainDraw, const DlScanCacheEntry& entry,
                             std::vector<DlCpWrite>&& cpWrites, const uint8_t* flattened,
                             uint32_t flattenedBytes) {
    auto& s_cache = DlScanCache();
    const uint64_t key = DlScanCacheKey(listAddr, nbytes, layoutHash);
    auto old = s_cache.entries.find(key);
    bool replacing = old != s_cache.entries.end();
    size_t replacedBytes = replacing ? DlScanCacheRecordBytes(old->second) : 0;
    const size_t incomingBytes = static_cast<size_t>(flattenedBytes) +
                                 cpWrites.size() * sizeof(DlCpWrite);
    if ((!replacing && s_cache.entries.size() >= kDlScanCacheMaxEntries) ||
        s_cache.storedCommandBytes - replacedBytes + incomingBytes > kDlScanCacheMaxStoredBytes) {
        s_cache.entries.clear();
        s_cache.storedCommandBytes = 0;
        replacing = false;
        replacedBytes = 0;
    }
    DlScanCacheRecord record{};
    record.result = entry;
    record.mayContainDraw = mayContainDraw;
    record.result.listAddr = CanonicalizeGxMainRamAddress(listAddr);
    record.result.nbytes = nbytes;
    record.result.layoutHash = layoutHash;
    record.contentDigest = contentDigest;
    // Sampled before the digest was taken, so a write racing the digest can only
    // make the next call re-digest, never make it trust a stale entry.
    record.writeGeneration = writeGeneration;
    record.cpWrites = std::move(cpWrites);
    if (flattened != nullptr && flattenedBytes != 0) {
        record.flattened.assign(flattened, flattened + flattenedBytes);
    }
    auto [it, inserted] = s_cache.entries.insert_or_assign(key, std::move(record));
    (void)inserted;
    s_cache.storedCommandBytes += DlScanCacheRecordBytes(it->second);
    if (replacing) {
        s_cache.storedCommandBytes -= replacedBytes;
    }
}

static void BuildScanVertexLayout(GXVtxFmt vtxfmt, std::array<ScanAttrStep, 26>& indexedSteps,
                                  uint32_t& indexedStepCount, uint32_t& vertexSize) {
    indexedStepCount = 0;
    vertexSize = 0;
    for (int attr = 0; attr < 26; ++attr) {
        const auto type = g_hleGxState.vtxDesc[attr];
        if (type == GX_NONE) {
            continue;
        }
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        ScanAttrStep step{};
        step.attr = gxAttr;
        step.offset = vertexSize;
        if (IsMatrixIndexAttr(gxAttr)) {
            step.size = 1;
        } else if (type == GX_INDEX8) {
            step.size = (gxAttr == GX_VA_NRM) ? NormalIndexCount(g_hleGxState.vtxAttrFmt[vtxfmt][attr]) : 1u;
            step.indexed = true;
        } else if (type == GX_INDEX16) {
            step.size = ((gxAttr == GX_VA_NRM) ? NormalIndexCount(g_hleGxState.vtxAttrFmt[vtxfmt][attr]) : 1u) * 2u;
            step.indexed = true;
            step.index16 = true;
        } else if (type == GX_DIRECT) {
            // Matrix attributes are handled above, so this reports 0 for them.
            step.size = DirectAttrByteSize(gxAttr, g_hleGxState.vtxAttrFmt[vtxfmt][attr],
                                           /*matrixAttrIsOneByte=*/false, /*fallbackComps=*/0u);
        }
        if (step.size == 0) {
            continue;
        }
        if (step.indexed) {
            indexedSteps[indexedStepCount++] = step;
        }
        // Every step contributes to the vertex stride, indexed or not: the
        // offsets the indexed steps carry are relative to the whole vertex.
        vertexSize += step.size;
    }
}

static const ScanLayoutCacheEntry& GetScanVertexLayout(GXVtxFmt vtxfmt,
                                                       std::array<ScanLayoutCacheEntry, 8>& cache,
                                                       uint32_t version,
                                                       uint32_t generation) {
    auto& entry = cache[static_cast<uint32_t>(vtxfmt)];
    if (entry.valid && entry.version == version && entry.generation == generation) {
        return entry;
    }
    BuildScanVertexLayout(vtxfmt, entry.indexedSteps, entry.indexedStepCount, entry.vertexSize);
    entry.version = version;
    entry.generation = generation;
    entry.valid = true;
    return entry;
}

static void ScanDLVerticesMaxIndices(const uint8_t* ptr, uint16_t vtxCount,
                                     const std::array<ScanAttrStep, 26>& steps, uint32_t stepCount,
                                     uint32_t vertexSize,
                                     std::array<uint32_t, GX_VA_MAX_ATTR>& maxIdx,
                                     std::array<bool, GX_VA_MAX_ATTR>& sawIdx) {
    if (stepCount == 0) {
        return;
    }
    const uint8_t* vertex = ptr;
    for (uint16_t v = 0; v < vtxCount; ++v, vertex += vertexSize) {
        for (uint32_t i = 0; i < stepCount; ++i) {
            const auto& step = steps[i];
            const int attr = static_cast<int>(step.attr);
            const uint32_t indexSize = step.index16 ? 2u : 1u;
            const uint32_t indexCount = std::max(1u, step.size / indexSize);
            for (uint32_t indexIdx = 0; indexIdx < indexCount; ++indexIdx) {
                const uint8_t* indexPtr = vertex + step.offset + indexIdx * indexSize;
                const uint32_t index = step.index16 ? ReadBE16(indexPtr) : *indexPtr;
                if (!sawIdx[attr] || index > maxIdx[attr]) {
                    maxIdx[attr] = index;
                    sawIdx[attr] = true;
                }
            }
        }
    }
}


static void AppendBytes(std::vector<uint8_t>& out, const uint8_t* data, uint32_t count) {
    out.insert(out.end(), data, data + count);
}

template <class Visitor>
static bool WalkDisplayList(const uint8_t* data, uint32_t nbytes, Visitor& visitor, int depth) {
    if (depth > Visitor::kMaxDepth) {
        return false;
    }
    uint32_t pos = 0;
    while (pos < nbytes) {
        const uint8_t* const cmdPtr = data + pos;
        const uint8_t cmd = data[pos++];
        if (cmd == GX_NOP_CMD) {
            if (!visitor.OnNop(cmd)) return false;
            continue;
        }
        if (cmd == GX_LOAD_BP_REG_CMD) {
            if (pos + 4u > nbytes) return false;
            if (!visitor.OnBpReg(cmdPtr, ReadBE32(data + pos))) return false;
            pos += 4u;
            continue;
        }
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;
        if (opcode == GX_LOAD_CP_REG_CMD) {
            if (pos + 5u > nbytes) return false;
            if (!visitor.OnCpReg(cmdPtr, data[pos], ReadBE32(data + pos + 1))) return false;
            pos += 5u;
            continue;
        }
        if (opcode == GX_LOAD_XF_REG_CMD) {
            // Overflow-safe spelling of "pos + 4 <= nbytes && pos + payload <= nbytes".
            if (pos > nbytes || nbytes - pos < 4u) return false;
            const uint16_t count = ReadBE16(data + pos);
            const uint32_t payloadBytes = 4u + (static_cast<uint32_t>(count) + 1u) * 4u;
            if (payloadBytes > nbytes - pos) return false;
            if (!visitor.OnXfReg(cmdPtr, payloadBytes)) return false;
            pos += payloadBytes;
            continue;
        }
        if (opcode >= GX_LOAD_INDX_A_CMD && opcode <= GX_LOAD_INDX_D_CMD) {
            if (pos + 4u > nbytes) return false;
            if (!visitor.OnIndexedXf(cmdPtr, cmd, ReadBE32(data + pos))) return false;
            pos += 4u;
            continue;
        }
        if (opcode == GX_CMD_CALL_DL_CMD) {
            if (pos + 8u > nbytes) return false;
            const uint32_t addr = ReadBE32(data + pos);
            const uint32_t size = ReadBE32(data + pos + 4);
            pos += 8u;
            if (!visitor.OnCallDisplayList(addr, size, depth)) return false;
            continue;
        }
        if (opcode == GX_CMD_INVL_VC_CMD) {
            if (!visitor.OnInvalidateVertexCache(cmd)) return false;
            continue;
        }
        if constexpr (Visitor::kHandlesDraw) {
            if (IsDrawOpcode(opcode)) {
                const GXVtxFmt vtxfmt = static_cast<GXVtxFmt>(cmd & GX_VAT_MASK_CMD);
                // Resolved before the count bounds check: the index scan latches
                // the list's vertex format here, and for the other two this is a
                // pure function of the current layout.
                const uint32_t vertexSize = visitor.DrawVertexSize(vtxfmt);
                if (pos + 2u > nbytes) return false;
                const uint16_t vtxCount = ReadBE16(data + pos);
                pos += 2u;
                uint32_t payloadBytes = static_cast<uint32_t>(vtxCount) * vertexSize;
                if (pos + payloadBytes > nbytes) return false;
                if (!visitor.OnDraw(cmdPtr, opcode, vtxfmt, vtxCount, data + pos, payloadBytes)) {
                    return false;
                }
                pos += payloadBytes;
                continue;
            }
        }
        if (!visitor.OnUnknownCommand(cmd)) return false;
    }
    return true;
}

// Does this list carry anything that can produce geometry? Anything the walker
// cannot model counts as "yes", which is why every hook that would have to look
// at a payload just fails the walk instead.
struct DlMayContainDrawVisitor {
    static constexpr int kMaxDepth = 8;
    static constexpr bool kHandlesDraw = false;

    bool OnNop(uint8_t) { return true; }
    bool OnBpReg(const uint8_t*, uint32_t) { return true; }
    // An array base/stride write means the list is setting up indexed geometry.
    bool OnCpReg(const uint8_t*, uint8_t reg, uint32_t) { return !(reg >= 0xA0 && reg <= 0xBF); }
    bool OnXfReg(const uint8_t*, uint32_t) { return true; }
    bool OnIndexedXf(const uint8_t*, uint8_t, uint32_t) { return true; }
    bool OnInvalidateVertexCache(uint8_t) { return true; }
    // Draw opcodes reach this too (kHandlesDraw is false): a draw opcode, or a
    // command byte this walker does not model, both mean "assume it draws".
    bool OnUnknownCommand(uint8_t) { return false; }
    bool OnCallDisplayList(uint32_t addr, uint32_t size, int depth) {
        if (addr == 0 || size == 0) return true;
        const uint8_t* nested = static_cast<const uint8_t*>(GuestToHostPtr(addr, size));
        if (!nested) return false;
        DlMayContainDrawVisitor child;
        return WalkDisplayList(nested, size, child, depth + 1);
    }
};

static bool DisplayListMayContainDraw(const uint8_t* data, uint32_t nbytes) {
    DlMayContainDrawVisitor visitor;
    return !WalkDisplayList(data, nbytes, visitor, 0);
}

#if defined(MKW_GAME_PROFILE_RM3E01_00)
// True when the array bytes this draw's indices read may still be changing: some 64 KiB granule
// under them got a write notification (DC flush/store, DMA) within the last second, or the arrays
// cannot be tracked (uncached mirror, multi-group attributes). Such draws are not deferred to the
// asynchronous decoder (see TrySubmitPrimePackedDl). GX thread only.
// Whether the game published (DC flush/store, DMA) any of [addr, addr + bytes) within the last
// second, or the range is not tracked at all (uncached mirror, outside MEM1/MEM2). GX thread only.
static bool RangeWrittenRecently(uint32_t addr,uint32_t bytes) {
    struct Seen {
        uint32_t generation=0;
        int64_t changedMs=-1000000;
    };
    static thread_local std::vector<Seen> seen(GxGuestWrite::kGranuleCount);
    constexpr int64_t kQuietMs=1000;
    // Writes through the uncached mirror send no notification at all.
    if(addr>=0xC0000000u)return true;
    const uint64_t first=CanonicalizeGxMainRamAddress(addr);
    const uint64_t end=first+bytes;
    if(bytes==0 || end>GxGuestWrite::kTrackedSpan)return true;
    const int64_t nowMs=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    bool recent=false;
    for(uint64_t granule=first>>GxGuestWrite::kGranuleShift;granule<=((end-1)>>GxGuestWrite::kGranuleShift);++granule) {
        const uint32_t generation=GxGuestWrite::g_generations[granule].load(std::memory_order_relaxed);
        auto& entry=seen[granule];
        if(generation!=entry.generation) {
            entry.generation=generation;
            entry.changedMs=nowMs;
        }
        if(nowMs-entry.changedMs<kQuietMs)recent=true;
    }
    return recent;
}

static bool ArraysWrittenRecently(const uint8_t* source,uint32_t count,
                                  const std::vector<PrimeDlVertices::Attribute>& attributes) {
    static thread_local std::vector<PrimeDlVertexCache::Span> spans;
    if(!PrimeDlVertexCache::ReadSpans(source,count,attributes,spans))return true;
    bool recent=false;
    for(const auto& span:spans)
        if(RangeWrittenRecently(span.first,span.bytes))recent=true;
    if(recent) {
        static unsigned traces=0;
        if(traces++<4)RT_LOG(RT_TAG_GX)<<"[prime-packed-indexed-dl] kept on the GX thread: arrays written recently vertices="
                                       <<count<<std::endl;
    }
    return recent;
}

// Arrays the game rewrites without notification while the GX thread may still be reading them for
// the previous frame (Bryyo lava ripples: the game keeps one ripple mesh, rewrites it every frame and
// lets the GPU run up to a frame behind through its FIFO breakpoint). On the Wii the GPU is done long
// before; here a patch read before the rewrite and its neighbour read during it disagree on the
// vertices they share: black wedges between patches. The decoder finds such arrays (a snapshot that no
// longer matches memory) and lists them here; the GX thread then reads each byte of them only once
// per frame (FrameArrayView), so every patch sharing a vertex sees the same value and the surface
// stays closed even when the read overlaps the game's rewrite.
namespace {
std::array<std::atomic<uint32_t>,16> g_volatileArrays{};
uint32_t g_gxFrame=0;  // GX thread: display copies interpreted
}

static bool IsVolatileArray(uint32_t base) {
    for(const auto& slot:g_volatileArrays)
        if(slot.load(std::memory_order_relaxed)==base)return true;
    return false;
}

static void MarkVolatileArray(uint32_t base) {
    // The mesh moves when a room is loaded again: the oldest address gives way to the new one.
    static std::atomic<uint32_t> nextSlot{0};
    if(base==0 || IsVolatileArray(base))return;
    const uint32_t slot=nextSlot.fetch_add(1,std::memory_order_relaxed)%g_volatileArrays.size();
    g_volatileArrays[slot].store(base,std::memory_order_relaxed);
    RT_LOG(RT_TAG_GX)<<"[prime-volatile-array] base=0x"<<std::hex<<base<<std::dec
                     <<" rewritten under the GX thread: read once per frame from now on"<<std::endl;
}

extern "C" void GxPrimeNoteDisplayCopy() { ++g_gxFrame; }

namespace {
struct FrameArrayView {
    uint32_t base=0,stride=0,frame=~0u;
    std::vector<uint8_t> bytes;  // [0, size) of the array, each byte read once this frame
};
std::array<FrameArrayView,8> s_frameViews;
}

// GX thread. Null when no attribute reads a volatile array; otherwise `attributes` with those arrays
// pointing into this frame's views (extended now with the bytes this draw reads beyond them).
static const std::vector<PrimeDlVertices::Attribute>* FrameCoherentAttributes(
    const uint8_t* source,uint32_t count,const std::vector<PrimeDlVertices::Attribute>& attributes) {
    constexpr size_t kMaxViewBytes=1u<<20;
    bool any=false;
    for(const auto& a:attributes)
        if(a.indexBytes && IsVolatileArray(a.arrayBase)) {any=true;break;}
    if(!any)return nullptr;
    static thread_local std::vector<PrimeDlVertexCache::Span> spans;
    static thread_local std::vector<PrimeDlVertices::Attribute> viewAttributes;
    if(!PrimeDlVertexCache::ReadSpans(source,count,attributes,spans))return nullptr;
    viewAttributes=attributes;
    size_t next=0;
    for(auto& a:viewAttributes) {
        if(!a.indexBytes)continue;
        const auto& span=spans[next++];
        if(!IsVolatileArray(a.arrayBase) || !a.arrayData || span.first<a.arrayBase)continue;
        const size_t end=size_t(span.first-a.arrayBase)+span.bytes;
        if(end>a.arrayBytes || end>kMaxViewBytes)continue;
        FrameArrayView* view=nullptr;
        for(auto& candidate:s_frameViews)
            if(candidate.base==a.arrayBase && candidate.stride==a.arrayStride) {view=&candidate;break;}
        if(!view) {
            view=&s_frameViews[0];
            for(auto& candidate:s_frameViews)
                if(candidate.frame!=g_gxFrame) {view=&candidate;break;}
            *view={a.arrayBase,a.arrayStride,~0u,{}};
        }
        if(view->frame!=g_gxFrame) {
            view->frame=g_gxFrame;
            view->bytes.clear();
        }
        if(end>view->bytes.size()) {
            const size_t from=view->bytes.size();
            view->bytes.resize(end);
            std::memcpy(view->bytes.data()+from,a.arrayData+from,end-from);
        }
        a.arrayData=view->bytes.data();
        a.arrayBytes=view->bytes.size();
    }
    return &viewAttributes;
}

// Snapshots for a deferred draw (PrimeDeferredDraw::Snapshot): the array bytes its indices read,
// copied now, when the GX thread interprets the draw, while the decoder would read them later. The game
// rewrites some arrays without any notification (Bryyo lava ripples: the patches it drew were expanded
// with heights the game had already replaced, on the decoder only: seams, flashes and jumping
// geometry, 2026-11-01). Up to 16 KiB of array bytes (the patches read a few hundred); large static meshes
// keep reading the array. Null when nothing is copied.
static const std::vector<PrimeDeferredDraw::Snapshot>* SmallArraySnapshots(
    const uint8_t* source,uint32_t count,const std::vector<PrimeDlVertices::Attribute>& attributes) {
    constexpr uint32_t kMaxSnapshotBytes=16*1024;
    static thread_local std::vector<PrimeDlVertexCache::Span> spans;
    static thread_local std::vector<PrimeDeferredDraw::Snapshot> snapshots;
    if(!PrimeDlVertexCache::ReadSpans(source,count,attributes,spans) || spans.empty())return nullptr;
    uint64_t total=0;
    for(const auto& span:spans)total+=span.bytes;
    if(total>kMaxSnapshotBytes)return nullptr;
    snapshots.assign(attributes.size(),{});
    size_t next=0;
    for(size_t i=0;i<attributes.size();++i) {
        const auto& a=attributes[i];
        if(!a.indexBytes)continue;
        if(next>=spans.size() || !a.arrayData)return nullptr;
        const auto& span=spans[next++];
        const uint64_t skip=uint64_t(span.first)-a.arrayBase;
        if(span.first<a.arrayBase || skip+span.bytes>a.arrayBytes)return nullptr;
        snapshots[i]={static_cast<uint32_t>(skip),span.bytes};
    }
    return &snapshots;
}

// Decoder: how often the live array no longer matched a snapshot (what the draw would have read).
static void NoteSnapshotChanges(const std::vector<PrimeDeferredDraw::SnapshotUse>& uses) {
    static std::atomic<uint64_t> draws{0},changed{0};
    static std::atomic<uint32_t> sampleBase{0};
    if(uses.empty())return;
    draws.fetch_add(1,std::memory_order_relaxed);
    bool any=false;
    for(const auto& use:uses) {
        if(std::memcmp(use.live,use.copy,use.size)==0)continue;
        if(!any)changed.fetch_add(1,std::memory_order_relaxed);
        any=true;
        sampleBase.store(use.arrayBase,std::memory_order_relaxed);
    }
    // All of the draw's arrays: once it stops being deferred, this check never sees its other arrays
    // (the mesh's positions when its normals changed first).
    if(any)for(const auto& use:uses)MarkVolatileArray(use.arrayBase);
#if !defined(MKW_PRIME_QA_TOOLS)
    return;
#endif
    static std::atomic<int64_t> nextLogMs{0};
    const int64_t nowMs=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t due=nextLogMs.load(std::memory_order_relaxed);
    if(nowMs<due || !nextLogMs.compare_exchange_strong(due,nowMs+5000))return;
    const uint64_t d=draws.exchange(0),c=changed.exchange(0);
    if(c==0)return;
    RT_LOG(RT_TAG_GX)<<"[prime-deferred-snapshot] draws="<<d<<" game_had_changed_the_array="<<c
                     <<" sample_array=0x"<<std::hex<<sampleBase.load()<<std::dec<<std::endl;
}

#if defined(MKW_PRIME_QA_TOOLS)
// Decoder, defer_dl_verify: the expansion here against the hash of the producer's own.
static void VerifyDeferredExpansion(const PrimeDeferredDraw::View& draw,
                                    const std::vector<PrimeDlVertices::Attribute>& attributes,
                                    const std::vector<PrimeDeferredDraw::SnapshotUse>& snapshots,
                                    const std::vector<uint8_t>& vertices) {
    static std::atomic<uint64_t> checked{0},mismatched{0};
    static std::atomic<int> details{0};
    checked.fetch_add(1,std::memory_order_relaxed);
    const uint64_t mine=static_cast<uint64_t>(XXH3_64bits(vertices.data(),vertices.size()))|1u;
    if(mine!=draw.verifyHash) {
        mismatched.fetch_add(1,std::memory_order_relaxed);
        if(details.fetch_add(1,std::memory_order_relaxed)<24) {
            std::ostringstream line;
            line<<"[prime-defer-verify] MISMATCH cmd=0x"<<std::hex<<unsigned(draw.command)<<std::dec<<" count="<<draw.count
                <<" bytes="<<vertices.size()<<" snapshots="<<snapshots.size()<<" attrs:";
            for(const auto& a:attributes)
                line<<" ["<<a.bytes<<"b idx"<<a.indexBytes<<" base=0x"<<std::hex<<a.arrayBase<<std::dec
                    <<" stride="<<a.arrayStride<<" mapped="<<a.arrayBytes<<"]";
            RT_LOG(RT_TAG_GX)<<line.str()<<std::endl;
        }
    }
    static std::atomic<int64_t> nextLogMs{0};
    const int64_t nowMs=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t due=nextLogMs.load(std::memory_order_relaxed);
    if(nowMs<due || !nextLogMs.compare_exchange_strong(due,nowMs+5000))return;
    RT_LOG(RT_TAG_GX)<<"[prime-defer-verify] checked="<<checked.exchange(0)<<" mismatched="<<mismatched.exchange(0)<<std::endl;
}

#endif

// Expansion of a packed draw (caller's thread: the GX thread, or the
// asynchronous decoder for a deferred draw), through the cross-frame cache of
// prime_dl_vertex_cache.h. Each thread keeps its own cache.
static const std::vector<uint8_t>* ExpandPackedDraw(const uint8_t* source,uint32_t sourceBytes,uint32_t count,
                                                    GXVtxFmt fmt,const std::vector<PrimeDlVertices::Attribute>& attributes,
                                                    std::vector<uint8_t>& expanded,bool allowCache=true) {
    const auto read=[](uint32_t address,size_t bytes,const uint8_t*& span) {
        if(auto* pointer=MemoryInline::GetPointerFast(address,bytes)) {span=pointer;return true;}
        if(!Memory::Contains(address,bytes))return false;
        span=static_cast<const uint8_t*>(Memory::GetPointer(address,bytes));return true;
    };
    bool expandedParallel=false;
    // Cross-frame reuse of this draw's expansion; see prime_dl_vertex_cache.h for the proofs.
    // WII_COMPILED_DISABLE_DL_VERTEX_CACHE=1 restores expanding every draw (A/B comparison).
    static const bool cacheEnabled=std::getenv("WII_COMPILED_DISABLE_DL_VERTEX_CACHE")==nullptr;
    static thread_local PrimeDlVertexCache::Cache vertexCache(64u<<20);
    const auto generation=[](uint32_t first,uint32_t bytes) {
        // Uncached mirrors (0xC0000000+) can be written without any flush notification.
        return first>=0xC0000000u ? PrimeDlVertexCache::kUntracked : GxGuestWrite::GenerationForRange(first,bytes);
    };
    const std::vector<uint8_t>* vertices=nullptr;
    PrimeDlVertexCache::Key cacheKey{};
    uint64_t sourceDigest=0;
    // Only large draws: RM3E01 sends hundreds of thousands of 4-16 vertex draws per second,
    // whose expansion is cheaper than the key, digest and lookup (measured: the lookup took
    // 22% of the GX thread with every draw cached).
    constexpr uint32_t kMinCachedVertices=48;
    const bool cacheThisDraw=cacheEnabled && allowCache && count>=kMinCachedVertices;
    if(cacheThisDraw) {
        cacheKey={source,sourceBytes,count,static_cast<uint32_t>(fmt),PrimeDlVertexCache::LayoutSignature(attributes)};
        sourceDigest=static_cast<uint64_t>(XXH3_64bits(source,sourceBytes));
        vertices=vertexCache.Find(cacheKey,sourceDigest,generation);
    }
    if(!vertices) {
        static thread_local std::vector<PrimeDlVertexCache::Span> spans;
        // Generations are sampled before the bytes are read: a racing write
        // costs a later re-expansion, never a stale reuse.
        const bool tracked=cacheThisDraw && PrimeDlVertexCache::ReadSpans(source,count,attributes,spans);
        const uint64_t arrayGeneration=tracked ? PrimeDlVertexCache::Cache::ArrayGeneration(spans,generation)
                                               : PrimeDlVertexCache::kUntracked;
        if(!PrimeDlVertices::Expand(source,sourceBytes,count,attributes,expanded,read,
                                    GxFifoStorage::MaxBufferedBytes,&expandedParallel))return nullptr;
        if(tracked)vertexCache.Store(cacheKey,sourceDigest,arrayGeneration,spans,expanded);
        vertices=&expanded;
    }
#if defined(MKW_PRIME_QA_TOOLS)
    if(cacheThisDraw && (vertexCache.Lookups()&0x3FFFu)==0) {
        RT_LOG(RT_TAG_GX)<<"[prime-dl-vertex-cache] lookups="<<vertexCache.Lookups()<<" hits="<<vertexCache.Hits()
            <<" stale="<<vertexCache.Stale()<<" entries="<<vertexCache.Entries()<<" bytes="<<vertexCache.Bytes()<<std::endl;
    }
#endif
    static unsigned parallelTraces=0;
    if(expandedParallel && parallelTraces++<8)RT_LOG(RT_TAG_GX)
        <<"[prime-packed-indexed-dl] parallel vertex expansion vertices="<<count
        <<" bytes="<<expanded.size()<<" workers=2"<<std::endl;
    return vertices;
}

// Decoder thread (aurora_set_deferred_draw_expander): a deferred packed draw
// back to GX wire form. The source bytes travel in the payload; the arrays are
// read now, which the guest's sync points (PE token, draw done) already cover:
// Aurora reads guest textures and lists here too (hilo_decodificador_aurora.md).
extern "C" const uint8_t* PrimeExpandDeferredDraw(const uint8_t* payload,uint32_t size,uint32_t* wireBytes) {
    static thread_local std::vector<PrimeDlVertices::Attribute> attributes;
    static thread_local std::vector<uint8_t> expanded,wire;
    static thread_local std::vector<PrimeDeferredDraw::SnapshotUse> snapshots;
    PrimeDeferredDraw::View draw{};
    if(!PrimeDeferredDraw::Decode(payload,size,draw,attributes,&snapshots)) {
        static unsigned traces=0;
        if(traces++<8)RT_LOG(RT_TAG_GX)<<"[prime-packed-indexed-dl] invalid deferred draw payload bytes="<<size<<std::endl;
        return nullptr;
    }
    NoteSnapshotChanges(snapshots);
    // No vertex cache here. Its proof is the arrays' write generation, and the game rewrites some
    // arrays without any notification; the load balancing sends a draw here only on some frames, so
    // this thread's entry for it could be several frames old while the generation still matched:
    // lava ripple patches drawn with old heights beside current ones (black wedges between patches,
    // only with this stage on, 2026-11-01). The decoder has the spare time to expand every draw.
    const auto* vertices=ExpandPackedDraw(draw.source,draw.sourceBytes,draw.count,
                                          static_cast<GXVtxFmt>(draw.command&7u),attributes,expanded,false);
    if(!vertices)return nullptr;  // unreachable: the producer proved ExpandCannotFail
#if defined(MKW_PRIME_QA_TOOLS)
    if(draw.verifyHash!=0)VerifyDeferredExpansion(draw,attributes,snapshots,*vertices);
#endif
    wire.resize(3+vertices->size());
    wire[0]=draw.command;
    wire[1]=static_cast<uint8_t>(draw.count>>8);
    wire[2]=static_cast<uint8_t>(draw.count);
    std::memcpy(wire.data()+3,vertices->data(),vertices->size());
    *wireBytes=static_cast<uint32_t>(wire.size());
    return wire.data();
}

// Interpreter fallback
static bool TrySubmitPrimePackedDl(uint8_t opcode,GXVtxFmt fmt,uint16_t count,
                                   const uint8_t* source,uint32_t sourceBytes,bool publishLayout,
                                   bool bindPalettes) {
    if(!count || fmt>=GX_MAX_VTXFMT || aurora::gx::fifo::in_display_list())return false;
    static thread_local std::vector<PrimeDlVertices::Attribute> attributes;
    static thread_local std::vector<uint8_t> expanded;
    // The caller's publication proof is local to this visitor and invalidated
    // by every non-draw command/nested list. Consecutive same-format draws
    // therefore share the layout and mapped-span proof, but always copy fresh
    // source bytes. The first draw of every visitor rebuilds this scratch plan.
    // The plan (sizes, index widths, mapped array spans) depends only on the VAT/VCD
    // layout of `fmt` (vtxStateGeneration) and on the array bases/strides, which
    // GXSetArray and CP 0xA0/0xB0 writes change without bumping the generation.
    // Reuse it across draws whose publication BP/XF commands invalidated.
    static thread_local uint32_t planFormat=~0u, planGeneration=~0u;
    static thread_local std::array<std::pair<uint32_t,uint32_t>,26> planArrays{};
    bool planReusable=publishLayout && planFormat==static_cast<uint32_t>(fmt) &&
                      planGeneration==g_hleGxState.vtxStateGeneration;
    for(unsigned attr=0;planReusable && attr<26;++attr) {
        const auto type=g_hleGxState.vtxDesc[attr];
        if(type==GX_INDEX8 || type==GX_INDEX16) {
            const auto& array=g_hleGxState.vtxArray[attr];
            planReusable=planArrays[attr]==std::pair<uint32_t,uint32_t>{array.base,array.stride};
        }
    }
    if(publishLayout && !planReusable) {
      planFormat=~0u;
      attributes.clear();
      for(unsigned attr=0;attr<26;++attr) {
        const auto type=g_hleGxState.vtxDesc[attr];
        if(type==GX_NONE)continue;
        const auto gxAttr=static_cast<GXAttr>(attr);
        const auto& format=g_hleGxState.vtxAttrFmt[fmt][attr];
        if(attr>GX_VA_TEX7 || (IsMatrixIndexAttr(gxAttr) && type!=GX_DIRECT))return false;
        const uint32_t bytes=DirectAttrByteSize(gxAttr,format,true,0u);
        if(!bytes)return false;
        const uint32_t indexBytes=type==GX_INDEX8 ? 1 : type==GX_INDEX16 ? 2 : 0;
        if(type!=GX_DIRECT && !indexBytes)return false;
        const auto& array=g_hleGxState.vtxArray[attr];
        attributes.push_back({bytes,indexBytes,indexBytes && gxAttr==GX_VA_NRM ? NormalIndexCount(format) : 1u,
                              array.base,array.stride});
        auto& attribute=attributes.back();
        if(indexBytes && array.base && array.stride && attribute.groups && bytes%attribute.groups==0) {
            const uint64_t lastIndex=indexBytes==1 ? 255u : 65535u;
            const uint64_t spanBytes=lastIndex*array.stride+bytes/attribute.groups;
            // A successful RAM mapping covers every possible encoded index.
            // Otherwise Expand retains its per-index validated fallback.
            if(spanBytes<=SIZE_MAX && uint64_t(array.base)+spanBytes<=uint64_t(UINT32_MAX)+1u) {
                attribute.arrayData=MemoryInline::GetPointerFast(array.base,static_cast<size_t>(spanBytes));
                // The inline page table deliberately rejects spans crossing
                // its 1MiB page boundary. A RAM region may still cover them;
                // validate that once here, rather than per vertex.
                if(!attribute.arrayData && Memory::Contains(array.base,static_cast<size_t>(spanBytes)))
                    attribute.arrayData=Memory::GetPointer(array.base,static_cast<size_t>(spanBytes));
                if(attribute.arrayData)attribute.arrayBytes=static_cast<size_t>(spanBytes);
            }
        }
      }
    }
    if(publishLayout && !planReusable) {
      planFormat=static_cast<uint32_t>(fmt);
      planGeneration=g_hleGxState.vtxStateGeneration;
      for(unsigned attr=0;attr<26;++attr)
        planArrays[attr]={g_hleGxState.vtxArray[attr].base,g_hleGxState.vtxArray[attr].stride};
    }
    // Draw dump companion: where this draw reads its positions from.
#if defined(MKW_PRIME_QA_TOOLS)
    if(aurora_draw_dump_active()) {
#else
    if(false) {
#endif
        uint32_t offset=0,index=0;
        for(unsigned attr=0;attr<26;++attr) {
            if(g_hleGxState.vtxDesc[attr]==GX_NONE)continue;
            const auto& attribute=attributes[index++];
            if(attr==GX_VA_POS) {
                const auto& format=g_hleGxState.vtxAttrFmt[fmt][attr];
                const uint32_t stride=sourceBytes/count;
                std::ostringstream line;
                line<<"[dump-dl-draw] count="<<count<<" fmt="<<fmt<<" pos_desc="<<g_hleGxState.vtxDesc[attr]
                    <<" type="<<format.type<<" cnt="<<format.cnt<<" frac="<<unsigned(format.frac)
                    <<" base=0x"<<std::hex<<attribute.arrayBase<<std::dec<<" stride="<<attribute.arrayStride
                    <<" vtx_stride="<<stride<<" idx=";
                for(uint32_t v=0;v<count && v<8;++v) {
                    const uint8_t* at=source+v*stride+offset;
                    line<<(attribute.indexBytes==2 ? uint32_t((at[0]<<8)|at[1]) : attribute.indexBytes==1 ? uint32_t(at[0]) : 0u)<<",";
                }
                RT_LOG(RT_TAG_GX)<<line.str()<<std::endl;
                break;
            }
            offset+=attribute.indexBytes ? attribute.indexBytes*attribute.groups : attribute.bytes;
        }
    }
    // Load balancing: the decoder expands a draw only while it keeps up (at most
    // the chunk in progress pending). Deferring every draw made the decoder the
    // bottleneck (80 % busy in the Norion and Dane cinematics, 2026-10-30); only
    // with nothing pending at all, it took almost nothing (10 %) because the
    // writer hands off a chunk every 16 KiB. A backlog past one chunk sends the
    // expansion back to the GX thread, so neither thread waits on the other.
    // Off by default ([prime] defer_dl_expansion): with it on, the lava ripple patches of Bryyo show
    // seams (unsolved, hilo_decodificador_aurora.md). WII_COMPILED_PRIME_DEFER_DL_EXPANSION=1 turns it on.
    static const bool deferEnabled=[]{const char* v=std::getenv("WII_COMPILED_PRIME_DEFER_DL_EXPANSION");return v && v[0]=='1';}();
    // The decoder reads the arrays later than this thread would. Arrays the game rewrites all the time
    // (fluid ripples: the lava of Bryyo) could then be read after the CPU had started on the next
    // frame's values, and since the load balancing above defers a draw only on some frames, the lava
    // switched between two states and its surface jumped (user, 2026-11-01; gone with the decoder
    // off). A draw whose array memory was written within the last second, or is not tracked at all,
    // is expanded here, at the moment the game submits it.
    // Arrays the game rewrites under this thread are read through this frame's view (FrameCoherentAttributes):
    // such a draw is expanded here, from the view, without the vertex cache.
    const auto* coherent=deferEnabled ? FrameCoherentAttributes(source,count,attributes) : nullptr;
    const bool deferred=!coherent && deferEnabled && PrimeGxStream::Async() && aurora_stream_decoder_backlog_hint()<=1 &&
        PrimeDlVertices::ExpandCannotFail(sourceBytes,count,attributes,GxFifoStorage::MaxBufferedBytes) &&
        !ArraysWrittenRecently(source,count,attributes);
    const std::vector<uint8_t>* vertices=nullptr;
    if(!deferred) {
        vertices=coherent ? ExpandPackedDraw(source,sourceBytes,count,fmt,*coherent,expanded,false)
                          : ExpandPackedDraw(source,sourceBytes,count,fmt,attributes,expanded);
        if(!vertices)return false;
    }
    struct RestoreFormat {
        GXVtxFmt previous;
        ~RestoreFormat(){g_hleGxState.currentVtxFmt=previous;}
    } restore{g_hleGxState.currentVtxFmt};
    g_hleGxState.currentVtxFmt=fmt;
    if(publishLayout) ApplyAuroraVtxStateForDlBegin(fmt);
    if(publishLayout || bindPalettes) GX_HLE_PrimeBindDrawPalettes(fmt);
    if(deferred) {
        // The asynchronous decoder expands this draw (PrimeExpandDeferredDraw):
        // only the plan and the encoded indices travel, and the GX thread, the
        // one that saturates in heavy scenes, skips the expansion. Publish what
        // GXBegin would flush first: the draw is decoded against that state.
        GXAuroraPublishDirtyState();
        static thread_local std::vector<uint8_t> payload;
        const auto* snapshots=SmallArraySnapshots(source,count,attributes);
        // WII_COMPILED_PRIME_DEFER_VERIFY=1 ([prime] defer_dl_verify): this thread also expands the draw
        // and the decoder compares its own result with that hash (diagnostics, costs the saving).
        uint64_t verifyHash=0;
#if defined(MKW_PRIME_QA_TOOLS)
        static const bool verify=[]{const char* v=std::getenv("WII_COMPILED_PRIME_DEFER_VERIFY");return v && v[0]=='1';}();
        if(verify) {
            static thread_local std::vector<uint8_t> check;
            if(const auto* own=ExpandPackedDraw(source,sourceBytes,count,fmt,attributes,check,false))
                verifyHash=static_cast<uint64_t>(XXH3_64bits(own->data(),own->size()))|1u;
        }
#endif
        PrimeDeferredDraw::Encode(OpcodeToGXPrimitive(opcode)|fmt,count,source,sourceBytes,attributes,payload,
                                  snapshots,verifyHash);
        if(aurora_stream_deferred_draw(payload.data(),static_cast<uint32_t>(payload.size()))) {
            GXMarkFrameWork();
            static unsigned deferredTraces=0;
            if(deferredTraces++<4)RT_LOG(RT_TAG_GX)<<"[prime-packed-indexed-dl] deferred to the decoder vertices="<<count
                <<" source_bytes="<<sourceBytes<<" payload_bytes="<<payload.size()<<std::endl;
            return true;
        }
        vertices=ExpandPackedDraw(source,sourceBytes,count,fmt,attributes,expanded);
        if(!vertices)return false;
    }
    // Consecutive draws in this visitor cannot reload TLUT or change BP,
    // XF, CP, or VAT state: every such command invalidates publication.
    // Keep the existing palette bindings instead of draining the producer
    // FIFO merely to rediscover the same sampled mask between primitives.
    // Vertex/source bytes still get copied above on every draw.
    // Retain the producer FIFO's batching. Direct submit drains and resolves
    // textures per tiny primitive, defeating the native decoder's draw batches.
    GXBegin(OpcodeToGXPrimitive(opcode),fmt,count);
    aurora::gx::fifo::write_data_bulk(vertices->data(),static_cast<uint32_t>(vertices->size()));
    GXEnd();
    GXMarkFrameWork();
    static unsigned traces=0;
    if(traces++<8)RT_LOG(RT_TAG_GX)<<"[prime-packed-indexed-dl] vertices="<<count
        <<" source_bytes="<<sourceBytes<<" expanded_bytes="<<vertices->size()<<" batching=producer_fifo"<<std::endl;
    return true;
}
#endif
struct DlInterpretVisitor {
    static constexpr int kMaxDepth = 8;
    static constexpr bool kHandlesDraw = true;
    PrimeDlVertices::PublicationTracker publication;

    bool OnNop(uint8_t) { return true; }  // padding: no state
    bool OnInvalidateVertexCache(uint8_t) { publication.Invalidate();return true; }
    bool OnUnknownCommand(uint8_t) { publication.Invalidate();return true; }
    bool OnCpReg(const uint8_t*, uint8_t reg, uint32_t value) { publication.Invalidate();ApplyCpRegWrite(reg, value); return true; }
    bool OnBpReg(const uint8_t*, uint32_t bpWord) {
        switch(PrimeDlVertices::BpEffectOf(static_cast<uint8_t>(bpWord >> 24))) {
        case PrimeDlVertices::BpEffect::All: publication.Invalidate(); break;
        case PrimeDlVertices::BpEffect::Palettes: publication.InvalidatePalettes(); break;
        case PrimeDlVertices::BpEffect::None: break;
        }
        GX_HLE_ApplyRawBp(static_cast<uint8_t>(bpWord >> 24), bpWord & 0x00FFFFFFu);
        return true;
    }
    bool OnXfReg(const uint8_t* cmdPtr, uint32_t payloadBytes) {
        publication.Invalidate();
        GXCallDisplayList(cmdPtr, 1u + payloadBytes);
        GXMarkFrameWork();
        return true;
    }
    bool OnIndexedXf(const uint8_t* cmdPtr, uint8_t cmd, uint32_t value) {
        // A matrix load (position/normal/texture/light): neither the vertex
        // layout nor the palettes change.
        // With the asynchronous decoder the matrix words are read now (SubmitIndexedXfImmediate).
        if (!(PrimeGxStream::Async() && SubmitIndexedXfImmediate(cmd, value))) {
            ApplyIndexedXfArrayForPacket(cmd, value);
            GXCallDisplayList(cmdPtr, 5);
        }
        GXMarkFrameWork();
        return true;
    }
    bool OnCallDisplayList(uint32_t addr, uint32_t size, int depth) {
        publication.Invalidate();
        if (addr == 0 || size == 0) return true;
        const uint8_t* nested = static_cast<const uint8_t*>(GuestToHostPtr(addr, size));
        if (!nested) return true;
        DlInterpretVisitor child;
        WalkDisplayList(nested, size, child, depth + 1);
        return true;
    }
    uint32_t DrawVertexSize(GXVtxFmt vtxfmt) { return CalcDLVertexSize(vtxfmt); }
    bool OnDraw(const uint8_t*, uint8_t opcode, GXVtxFmt vtxfmt, uint16_t vtxCount,
                const uint8_t* vertices, uint32_t& payloadBytes) {
        EnsureAuroraFrameActive();
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        if(TrySubmitPrimePackedDl(opcode,vtxfmt,vtxCount,vertices,payloadBytes,publication.NeedsPublish(vtxfmt),
                                  publication.NeedsPalettes(vtxfmt))) {
            publication.Completed(vtxfmt);return true;
        }
#endif
        publication.Invalidate();
        const HleGxVertexStateSnapshot drawGuestState = CaptureGxVertexState();
        ApplyAuroraVtxStateForDlBegin(vtxfmt);
        GX_HLE_PrimeBindDrawPalettes(vtxfmt);
        GXBegin(OpcodeToGXPrimitive(opcode), vtxfmt, vtxCount);
        GXMarkFrameWork();
        // The per-vertex decode is what advances the cursor here, exactly as
        // before the walk was shared: report what it actually consumed rather
        // than the vtxCount * stride product the walker precomputed.
        uint32_t consumed = 0;
        for (uint16_t v = 0; v < vtxCount; ++v) {
            consumed += SubmitDLVertex(vertices + consumed, vtxfmt, drawGuestState.vtxDesc);
        }
        GXEnd();
        RestoreGxVertexState(drawGuestState);
        payloadBytes = consumed;
        return true;
    }
};

static void ParseDisplayList(const uint8_t* data, uint32_t nbytes) {
    DlInterpretVisitor visitor;
    WalkDisplayList(data, nbytes, visitor, 0);
}

struct DlIndexScanVisitor {
    static constexpr int kMaxDepth = 4;
    static constexpr bool kHandlesDraw = true;

    static std::array<ScanLayoutCacheEntry, 8>& LayoutCache() {
        static thread_local std::array<ScanLayoutCacheEntry, 8> cache;
        return cache;
    }
    static uint32_t NextLayoutGeneration() {
        static thread_local uint32_t counter = 0;
        return ++counter;
    }

    std::array<uint32_t, GX_VA_MAX_ATTR>& maxIdx;
    std::array<bool, GX_VA_MAX_ATTR>& sawIdx;
    std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfIdx;
    std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfBytes;
    std::array<bool, GX_VA_MAX_ATTR>& sawXfIdx;
    GXVtxFmt* outFmt;
    bool* outFmtMixed;
    bool* outHasNestedDl;
    bool* outHasArrayStateWrites;
    bool* outHasCpWrites;
    std::vector<DlCpWrite>* outCpWrites;
    // Every walk - including each nested one - gets its own layout generation, so
    // a cached layout can only be reused inside the walk that built it.
    uint32_t layoutVersion = 1;
    uint32_t layoutGeneration = NextLayoutGeneration();
    const ScanLayoutCacheEntry* pendingLayout = nullptr;

    bool OnNop(uint8_t) { return true; }
    bool OnBpReg(const uint8_t*, uint32_t) { return true; }
    bool OnXfReg(const uint8_t*, uint32_t) { return true; }
    bool OnInvalidateVertexCache(uint8_t) { return true; }
    bool OnUnknownCommand(uint8_t) { return false; }
    bool OnCpReg(const uint8_t*, uint8_t reg, uint32_t value) {
        if (outHasCpWrites) *outHasCpWrites = true;
        if (reg >= 0xA0 && reg <= 0xBF && outHasArrayStateWrites) *outHasArrayStateWrites = true;
        if (outCpWrites != nullptr) outCpWrites->push_back(DlCpWrite{reg, value});
        ApplyCpRegWrite(reg, value);
        if (reg == 0x50 || reg == 0x60 || (reg >= 0x70 && reg <= 0x97)) ++layoutVersion;
        return true;
    }
    bool OnIndexedXf(const uint8_t*, uint8_t cmd, uint32_t value) {
        const int attr =
            GX_POS_MTX_ARRAY + (((cmd & GX_OPCODE_MASK_CMD) - GX_LOAD_INDX_A_CMD) / 0x08);
        const uint32_t index = value >> 16;
        const uint32_t bytes = (((value >> 12) & 0x0fu) + 1u) * 4u;
        if (attr >= 0 && attr < GX_VA_MAX_ATTR) {
            if (!sawXfIdx[attr] || index > maxXfIdx[attr]) maxXfIdx[attr] = index;
            if (bytes > maxXfBytes[attr]) maxXfBytes[attr] = bytes;
            sawXfIdx[attr] = true;
        }
        return true;
    }
    bool OnCallDisplayList(uint32_t addr, uint32_t size, int depth) {
        if (addr == 0 || size == 0) return true;
        if (outHasNestedDl) *outHasNestedDl = true;
        try {
            const uint8_t* nested = static_cast<const uint8_t*>(GuestToHostPtr(addr, size));
            if (nested) {
                DlIndexScanVisitor child{maxIdx,         sawIdx,         maxXfIdx,
                                         maxXfBytes,     sawXfIdx,       outFmt,
                                         outFmtMixed,    outHasNestedDl, outHasArrayStateWrites,
                                         outHasCpWrites, outCpWrites};
                if (!WalkDisplayList(nested, size, child, depth + 1)) return false;
            }
            ++layoutVersion;
        } catch (...) {
            return false;
        }
        return true;
    }
    uint32_t DrawVertexSize(GXVtxFmt vtxfmt) {
        if (outFmt) {
            if (*outFmt == GX_MAX_VTXFMT) *outFmt = vtxfmt;
            else if (*outFmt != vtxfmt && outFmtMixed) *outFmtMixed = true;
        }
        pendingLayout = &GetScanVertexLayout(vtxfmt, LayoutCache(), layoutVersion, layoutGeneration);
        return pendingLayout->vertexSize;
    }
    bool OnDraw(const uint8_t*, uint8_t, GXVtxFmt, uint16_t vtxCount, const uint8_t* vertices,
                uint32_t&) {
        ScanDLVerticesMaxIndices(vertices, vtxCount, pendingLayout->indexedSteps,
                                 pendingLayout->indexedStepCount, pendingLayout->vertexSize, maxIdx,
                                 sawIdx);
        return true;
    }
};

static bool ScanDisplayListMaxIndices(const uint8_t* data, uint32_t nbytes,
                                      std::array<uint32_t, GX_VA_MAX_ATTR>& maxIdx,
                                      std::array<bool, GX_VA_MAX_ATTR>& sawIdx,
                                      std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfIdx,
                                      std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfBytes,
                                      std::array<bool, GX_VA_MAX_ATTR>& sawXfIdx,
                                      GXVtxFmt* outFmt, bool* outFmtMixed, bool* outHasNestedDl,
                                      bool* outHasArrayStateWrites, bool* outHasCpWrites,
                                      std::vector<DlCpWrite>* outCpWrites) {
    DlIndexScanVisitor visitor{maxIdx,         sawIdx,         maxXfIdx,
                               maxXfBytes,     sawXfIdx,       outFmt,
                               outFmtMixed,    outHasNestedDl, outHasArrayStateWrites,
                               outHasCpWrites, outCpWrites};
    return WalkDisplayList(data, nbytes, visitor, 0);
}

// Inlines nested lists into one flat command stream aurora can consume directly.
// `outVerbatim` stays true only while every consumed command is appended
// unchanged, i.e. the flattened stream is a byte-for-byte copy of the source
// list. Callers use that to hand aurora the guest bytes directly.
struct DlFlattenVisitor {
    static constexpr int kMaxDepth = 8;
    static constexpr bool kHandlesDraw = true;

    std::vector<uint8_t>& out;
    bool skipArrayState;
    bool* outVerbatim;

    bool OnNop(uint8_t cmd) { out.push_back(cmd); return true; }
    bool OnInvalidateVertexCache(uint8_t cmd) { out.push_back(cmd); return true; }
    bool OnUnknownCommand(uint8_t) { return false; }
    bool OnBpReg(const uint8_t* cmdPtr, uint32_t) { AppendBytes(out, cmdPtr, 5); return true; }
    bool OnIndexedXf(const uint8_t* cmdPtr, uint8_t, uint32_t) { AppendBytes(out, cmdPtr, 5); return true; }
    bool OnXfReg(const uint8_t* cmdPtr, uint32_t payloadBytes) {
        AppendBytes(out, cmdPtr, 1u + payloadBytes);
        return true;
    }
    // Command byte + the 2-byte vertex count + the vertex payload.
    bool OnDraw(const uint8_t* cmdPtr, uint8_t, GXVtxFmt, uint16_t, const uint8_t*,
                uint32_t& payloadBytes) {
        AppendBytes(out, cmdPtr, 3u + payloadBytes);
        return true;
    }
    uint32_t DrawVertexSize(GXVtxFmt vtxfmt) { return CalcDLVertexSize(vtxfmt); }
    bool OnCpReg(const uint8_t* cmdPtr, uint8_t reg, uint32_t value) {
        const bool arrayBaseOrStride = (reg >= 0xA0 && reg <= 0xBF);
        if (!skipArrayState || !arrayBaseOrStride) {
            AppendBytes(out, cmdPtr, 6);
        } else if (outVerbatim != nullptr) {
            *outVerbatim = false;
        }
        ApplyCpRegWrite(reg, value);
        return true;
    }
    bool OnCallDisplayList(uint32_t addr, uint32_t size, int depth) {
        if (outVerbatim != nullptr) *outVerbatim = false;
        if (addr == 0 || size == 0) return true;
        const uint8_t* nested = static_cast<const uint8_t*>(GuestToHostPtr(addr, size));
        if (!nested) return true;
        DlFlattenVisitor child{out, skipArrayState, outVerbatim};
        return WalkDisplayList(nested, size, child, depth + 1);
    }
};

static bool FlattenDisplayListForAurora(const uint8_t* data, uint32_t nbytes,
                                        std::vector<uint8_t>& out, bool skipArrayState,
                                        bool* outVerbatim = nullptr) {
    DlFlattenVisitor visitor{out, skipArrayState, outVerbatim};
    return WalkDisplayList(data, nbytes, visitor, 0);
}

static std::vector<uint8_t>& FlattenDisplayListScratch() {
    static thread_local std::vector<uint8_t> s_scratch;
    return s_scratch;
}

static bool s_appliedVtxDescValid = false;
static std::array<GXAttrType, 26> s_appliedVtxDesc{};
static std::array<GXAttrType, 26> s_appliedSourceVtxDesc{};
static bool s_appliedVtxAttrFmtValid = false;
static VtxAttrFmt s_appliedVtxAttrFmt[8][26]{};

struct AppliedArrayState {
    const void* data = nullptr;
    uint32_t size = 0;
    uint8_t stride = 0;
    bool valid = false;
};

static std::array<AppliedArrayState, GX_VA_MAX_ATTR> s_appliedArrays{};

// True only while the applied-state mirror is a verbatim copy of HLE vertex state at
// generation s_appliedVtxStateGeneration; any HLE-side mutation bumps that generation, so the
// memoized republish below can never observe a stale match.
static bool s_appliedVtxStateMatchesHle = false;
static uint32_t s_appliedVtxStateGeneration = 0;

static void InvalidateAppliedAuroraState() {
    s_appliedVtxDescValid = false;
    s_appliedVtxAttrFmtValid = false;
    s_appliedVtxStateMatchesHle = false;
    for (auto& array : s_appliedArrays) {
        array.valid = false;
    }
}

static void SyncAppliedVtxStateFromHleReal() {
    // skip the republish only when the mirror is already an exact copy of
    // HLE state
    if (s_appliedVtxStateMatchesHle && s_appliedVtxDescValid && s_appliedVtxAttrFmtValid &&
        s_appliedVtxStateGeneration == g_hleGxState.vtxStateGeneration) {
        return;
    }
    // Both mirrors are dense arrays of the same trivially copyable element types
    // as their HLE counterparts, so publish them with three memcpys instead of
    // 234 individual element assignments.
    static_assert(sizeof(s_appliedVtxDesc) == sizeof(g_hleGxState.vtxDesc));
    static_assert(sizeof(s_appliedSourceVtxDesc) == sizeof(g_hleGxState.vtxDesc));
    static_assert(sizeof(s_appliedVtxAttrFmt) == sizeof(g_hleGxState.vtxAttrFmt));
    std::memcpy(s_appliedVtxDesc.data(), g_hleGxState.vtxDesc, sizeof(g_hleGxState.vtxDesc));
    std::memcpy(s_appliedSourceVtxDesc.data(), g_hleGxState.vtxDesc, sizeof(g_hleGxState.vtxDesc));
    s_appliedVtxDescValid = true;

    std::memcpy(s_appliedVtxAttrFmt, g_hleGxState.vtxAttrFmt, sizeof(g_hleGxState.vtxAttrFmt));
    s_appliedVtxAttrFmtValid = true;

    s_appliedVtxStateMatchesHle = true;
    s_appliedVtxStateGeneration = g_hleGxState.vtxStateGeneration;
}

// Arrays sent by value (GXSetArrayCopy) and what the decoder holds for each attribute. Survives
// InvalidateAppliedAuroraState (run around every display list): a copy is resent only when the
// game publishes that memory again or a draw reads past what was copied, so a rewritten array
// travels about once per frame.
struct CopiedArrayState {
    uint32_t guestAddr = 0;
    uint32_t size = 0;
    uint64_t digest = 0;
    uint8_t stride = 0;
    bool valid = false;
};
static std::array<CopiedArrayState, GX_VA_MAX_ATTR> s_copiedArrays{};
static uint64_t s_arrayCopies = 0, s_arrayCopyBytes = 0, s_arrayCopyChecks = 0;

extern "C" void GxForgetCopiedArray(uint32_t attr) {
    if (attr < s_copiedArrays.size()) s_copiedArrays[attr].valid = false;
}

static void ApplyAuroraArrayIfChanged(GXAttr attr, const void* data, uint32_t size, uint8_t stride,
                                      uint32_t guestAddr = 0) {
    auto& applied = s_appliedArrays[attr];
    // With the asynchronous decoder a pointer is read when the decoder gets to the draw; by then the
    // game may have written the next frame's values (lava ripples: seams, black wedges and jumping
    // geometry, only with the decoder on, 2026-11-01). The game writes those arrays without any DC
    // flush notification, so small arrays are compared by content: the decoder gets a copy whenever
    // the bytes differ from the copy it holds. Large arrays (static meshes) keep the pointer, unless
    // the game published them within the last second.
    constexpr uint32_t kContentCopyBytes = 16 * 1024;
    if (guestAddr != 0 && PrimeGxStream::Async() &&
        (size <= kContentCopyBytes || RangeWrittenRecently(guestAddr, size))) {
        auto& copied = s_copiedArrays[attr];
        // Display lists read different index ranges of one array: compare and send the largest range
        // seen (still mapped: the same base was valid for it), so one copy serves them all.
        const bool sameArray = copied.valid && copied.guestAddr == guestAddr && copied.stride == stride;
        const uint32_t copySize = sameArray && copied.size > size && copied.size <= kContentCopyBytes ? copied.size : size;
        const uint64_t digest = static_cast<uint64_t>(XXH3_64bits(data, copySize));
        ++s_arrayCopyChecks;
        if (sameArray && copied.size == copySize && copied.digest == digest) {
            applied.valid = false;  // the decoder holds the copy, not this pointer
            return;
        }
        const bool contentChanged = sameArray && copied.size == copySize;
        if (GXSetArrayCopy(attr, data, copySize, stride)) {
            copied = {guestAddr, copySize, digest, stride, true};
            applied.valid = false;
            static uint64_t changes = 0;
            static uint32_t sampleBase = 0;
            if (contentChanged) {
                ++changes;
                sampleBase = guestAddr;
            }
            ++s_arrayCopies;
            s_arrayCopyBytes += copySize;
#if defined(MKW_PRIME_QA_TOOLS)
            static auto nextLog = std::chrono::steady_clock::now();
            if (std::chrono::steady_clock::now() >= nextLog) {
                nextLog = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                RT_LOG(RT_TAG_GX) << "[prime-array-copy] checks=" << s_arrayCopyChecks << " copies=" << s_arrayCopies
                                  << " bytes=" << s_arrayCopyBytes << " same_array_new_content=" << changes
                                  << " sample=0x" << std::hex << sampleBase << std::dec << std::endl;
                s_arrayCopies = s_arrayCopyBytes = s_arrayCopyChecks = changes = 0;
            }
#endif
            return;
        }
    }
    s_copiedArrays[attr].valid = false;
    if (applied.valid && applied.data == data && applied.size == size && applied.stride == stride) {
        return;
    }
    GXSetArray(attr, data, size, stride);
    applied.data = data;
    applied.size = size;
    applied.stride = stride;
    applied.valid = true;
}

// Aurora's live vertex descriptor for one attribute, read straight back out of
// its CP shadow registers (GXGetVtxDesc is the exact inverse of GXSetVtxDesc,
// GX_VA_NRM included). This is the ground truth the applied-state mirror is
// checked against below; it is a switch plus a shift/mask, no state change and
// no FIFO traffic.
static bool AuroraVtxDescMatchesLive(GXAttr attr, GXAttrType expected) {
    GXAttrType live = GX_NONE;
    GXGetVtxDesc(attr, &live);
    return live == expected;
}


static void ApplyAuroraVtxDesc() {
    const bool hadValidState = s_appliedVtxDescValid;
    if (!hadValidState) {
        s_appliedVtxStateMatchesHle = false;
    }
    for (int attr = 0; attr < 26; ++attr) {
        if (attr == GX_VA_NBT) continue;
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        const GXAttrType type = g_hleGxState.vtxDesc[attr];
        bool needsPublish = !hadValidState || s_appliedVtxDesc[attr] != type;
        // GXGetVtxDesc only decodes GX_VA_PNMTXIDX..GX_VA_TEX7; the XF array
        // pseudo-attributes above GX_VA_TEX7 have no CP descriptor bits, so
        // probing them would always report GX_NONE and republish a no-op.
        if (!needsPublish && !IsMatrixIndexAttr(gxAttr) && gxAttr <= GX_VA_TEX7 &&
            (type == GX_INDEX8 || type == GX_INDEX16)) {
            needsPublish = !AuroraVtxDescMatchesLive(gxAttr, type);
        }
        if (needsPublish) {
            GXSetVtxDesc(gxAttr, type);
            s_appliedVtxDesc[attr] = type;
        }
        if (!hadValidState || s_appliedSourceVtxDesc[attr] != type) {
            GXSetSourceVtxDesc(gxAttr, type);
            s_appliedSourceVtxDesc[attr] = type;
        }
    }
    s_appliedVtxDescValid = true;
}

// Size of one element of the guest array an indexed attribute reads from. An
// NBT3 normal is indexed per 3-component group, not per 9-component triple.
static uint32_t GetIndexedAttrElementSizeBytes(GXAttr attr, const VtxAttrFmt& fmt) {
    if (attr == GX_VA_NRM && fmt.cnt == GX_NRM_NBT3) {
        return 3u * static_cast<uint32_t>(GetCompSizeBytes(fmt.type));
    }
    return DirectAttrByteSize(attr, fmt, /*matrixAttrIsOneByte=*/false, /*fallbackComps=*/0u);
}

static void ApplyAuroraVtxAttrFmt(GXVtxFmt fmt) {
    const bool hadValidState = s_appliedVtxAttrFmtValid;
    // Rows other than `fmt` keep their contents and every write below copies
    // g_hleGxState, so an already-matching mirror still matches. Only the rebuild
    // from an invalidated mirror leaves the untouched rows divergent.
    if (!hadValidState) {
        s_appliedVtxStateMatchesHle = false;
    }
    for (int attr = 0; attr < 26; ++attr) {
        if (attr == GX_VA_NBT) continue;
        if (IsMatrixIndexAttr(static_cast<GXAttr>(attr))) continue;
        const auto& f = g_hleGxState.vtxAttrFmt[fmt][attr];
        if (hadValidState && SameVtxAttrFmt(s_appliedVtxAttrFmt[fmt][attr], f)) {
            continue;
        }
        GXSetVtxAttrFmt(fmt, static_cast<GXAttr>(attr), f.cnt, f.type, f.frac);
        s_appliedVtxAttrFmt[fmt][attr] = f;
    }
    s_appliedVtxAttrFmtValid = true;
}

static void ApplyAuroraVtxAttrFmtAll() {
    for (int fmt = 0; fmt < 8; ++fmt) {
        ApplyAuroraVtxAttrFmt(static_cast<GXVtxFmt>(fmt));
    }
}

static void ApplyAuroraVtxAttrFmtForDisplayList(GXVtxFmt fmt, bool mixed) {
    if (!mixed && fmt < GX_MAX_VTXFMT) {
        ApplyAuroraVtxAttrFmt(fmt);
        return;
    }
    ApplyAuroraVtxAttrFmtAll();
}

static void SubmitLytDrawPacket(const uint8_t* packet, uint32_t packetBytes) {
    if (packet == nullptr || packetBytes == 0) {
        return;
    }
    EnsureAuroraFrameActive();
    ApplyAuroraVtxDesc();
    ApplyAuroraVtxAttrFmtForDisplayList(GX_VTXFMT0, false);
    EnsureDefaultGxAlphaCompare();

    GXCallDisplayList(packet, packetBytes);
    GXMarkFrameWork();
    SyncAppliedVtxStateFromHleReal();
}

static bool ApplyAuroraArraysForDisplayList(const std::array<uint32_t, GX_VA_MAX_ATTR>& maxIdx,
                                            const std::array<bool, GX_VA_MAX_ATTR>& sawIdx,
                                            GXVtxFmt vtxfmt,
                                            bool vtxfmtMixed) {
    static std::array<std::array<std::vector<uint8_t>, 2>, GX_VA_MAX_ATTR> s_packedArrays;
    static std::array<uint32_t, GX_VA_MAX_ATTR> s_packedArrayCursor{};
    bool ok = true;
    if (vtxfmt >= GX_MAX_VTXFMT) {
        vtxfmt = g_hleGxState.currentVtxFmt;
    }
    for (int attr = 0; attr < 26; ++attr) {
        const GXAttrType type = g_hleGxState.vtxDesc[attr]; if (type != GX_INDEX8 && type != GX_INDEX16) continue;
        if (!sawIdx[attr]) continue;
        const auto& arr = g_hleGxState.vtxArray[attr];
        if (arr.base == 0 || arr.stride == 0) { ok = false; continue; }
        const uint32_t count = maxIdx[attr] + 1u;
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        const auto& fmt = g_hleGxState.vtxAttrFmt[vtxfmt][attr];
        const uint32_t elemSize = GetIndexedAttrElementSizeBytes(gxAttr, fmt);
        if (elemSize == 0) {
            ok = false;
            continue;
        }
        const uint32_t span = (count > 0) ? (static_cast<uint32_t>((count - 1u) * arr.stride) + elemSize) : 0u;
        const bool needsPack = arr.stride > 0xffu;
        if (!Memory::Contains(arr.base, span)) {
            ok = false;
            continue;
        }
        const uint8_t* hostPtr = static_cast<const uint8_t*>(GuestToHostPtr(arr.base, span));
        if (!hostPtr) {
            ok = false;
            continue;
        }
        // Aurora's storage-fetch shaders consume guest big-endian bytes directly
        // and honor the array stride. Keep the backing pointer stable so per-frame
        // storage uploads can be cached instead of repacking every display-list call.
        if (!needsPack) {
            ApplyAuroraArrayIfChanged(gxAttr, hostPtr, span, static_cast<uint8_t>(arr.stride), arr.base);
            continue;
        }
        auto& packed = s_packedArrays[attr][++s_packedArrayCursor[attr] & 1u];
        if (!PackIndexedAttrData(hostPtr, arr.stride, count, elemSize, packed)) {
            ok = false;
            continue;
        }
        // The repacked copy lives in a two-slot buffer: with the asynchronous decoder it travels by
        // value, or a third display list could rewrite it before the decoder reads it.
        if (PrimeGxStream::Async() && GXSetArrayCopy(gxAttr, packed.data(), static_cast<uint32_t>(packed.size()),
                                                     static_cast<uint8_t>(elemSize))) {
            s_appliedArrays[gxAttr].valid = false;
            GxForgetCopiedArray(gxAttr);
            continue;
        }
        ApplyAuroraArrayIfChanged(gxAttr, packed.data(), static_cast<uint32_t>(packed.size()), static_cast<uint8_t>(elemSize));
    }
    return ok;
}

static bool ApplyAuroraIndexedXFArraysForDisplayList(const std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfIdx,
                                                     const std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfBytes,
                                                     const std::array<bool, GX_VA_MAX_ATTR>& sawXfIdx) {
    bool ok = true;
    for (int attr = GX_POS_MTX_ARRAY; attr <= GX_LIGHT_ARRAY; ++attr) {
        if (!sawXfIdx[attr]) {
            continue;
        }
        const auto& arr = g_hleGxState.vtxArray[attr];
        const uint32_t bytes = maxXfBytes[attr];
        if (arr.base == 0 || arr.stride == 0 || bytes == 0) {
            ok = false;
            continue;
        }
        const uint32_t span = maxXfIdx[attr] * arr.stride + bytes;
        if (!Memory::Contains(arr.base, span)) {
            ok = false;
            continue;
        }
        const uint8_t* hostPtr = static_cast<const uint8_t*>(GuestToHostPtr(arr.base, span));
        if (!hostPtr) {
            ok = false;
            continue;
        }
        ApplyAuroraArrayIfChanged(static_cast<GXAttr>(attr), hostPtr, span, static_cast<uint8_t>(arr.stride), arr.base);
    }
    return ok;
}

} // namespace

extern "C" void GxNotifyDisplayListMemoryWrite(uint32_t addr, uint32_t size) {
    GxGuestWrite::NotifyWrite(addr, size);
}

extern "C" void GX__CallDisplayList_80172f64(uint32_t listAddr, uint32_t nbytes) {
    if (nbytes == 0 || listAddr == 0) return;
    try {
        const uint8_t* list = static_cast<const uint8_t*>(GuestToHostPtr(listAddr, nbytes));
        if (!list) return;
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        if (CachedWiiCompiledTracePrimeMii()) {
            static std::unordered_map<uint64_t, bool> traced;
            const uint64_t key = (uint64_t(listAddr) << 32) | nbytes;
            if (traced.size() < 512 && traced.emplace(key, true).second) {
                const auto* cpu = TryGetCpuContext();
                RT_LOG(RT_TAG_GX) << "[prime-mii-display-list] address=0x" << std::hex
                    << listAddr << " bytes=0x" << nbytes
                    << " active=0x" << RecompMod::CurrentTranslatedExecutionAddress()
                    << " lr=0x" << (cpu ? cpu->lr : 0) << std::dec << std::endl;
            }
        }
        // Reconcile every original list command with the raw MMIO state.
        // The upstream fast-path mirrors assume its SDK HLE wrappers own GX
        // state, whereas Prime executes its original SDK register writes.
        EnsureAuroraFrameActive();
        GXMarkFrameWork();
        InvalidateAppliedAuroraState();
        ParseDisplayList(list, nbytes);
        InvalidateAppliedAuroraState();
        return;
#endif
        const bool allowScanCache = nbytes <= kDlScanCacheMaxEntryBytes;
        const uint64_t scanLayoutHash = allowScanCache ? HashScanLayoutState() : 0;
        // The digest is computed lazily inside the probe: an entry whose covering
        // write-tracking granules have not moved since it was stored keeps its
        // recorded digest and skips the XXH3 pass over the whole list entirely.
        const DlScanCacheProbe probe =
            allowScanCache ? ProbeDlScanCache(list, listAddr, nbytes, scanLayoutHash)
                           : DlScanCacheProbe{};
        const DlScanCacheRecord* cached = probe.record;

        // The scan a cached record came from already walked the list for this,
        // so only a miss pays for DisplayListMayContainDraw.
        const bool mayContainDraw =
            (cached != nullptr) ? cached->mayContainDraw : DisplayListMayContainDraw(list, nbytes);
        if (!mayContainDraw) {
            EnsureAuroraFrameActive();
            GXMarkFrameWork();
            GXCallDisplayList(list, nbytes);
            // The republish is required even for register-only lists (verified by
            // experiment: dropping it corrupts in-race geometry decode). It is
            // instead memoized inside SyncAppliedVtxStateFromHleReal, so a run of
            // register-only lists between draws pays for at most one mirror copy.
            SyncAppliedVtxStateFromHleReal();
            return;
        }
        std::array<uint32_t, GX_VA_MAX_ATTR> maxIdx{}; std::array<bool, GX_VA_MAX_ATTR> sawIdx{};
        std::array<uint32_t, GX_VA_MAX_ATTR> maxXfIdx{}; std::array<uint32_t, GX_VA_MAX_ATTR> maxXfBytes{};
        std::array<bool, GX_VA_MAX_ATTR> sawXfIdx{};
        GXVtxFmt dlVtxFmt = GX_MAX_VTXFMT; bool dlVtxFmtMixed = false;
        bool dlHasNestedDl = false;
        bool dlHasArrayStateWrites = false;
        bool dlHasCpWrites = false;

        HleGxVertexStateSnapshot stateBeforeScan;
        bool haveStateBeforeScan = false;

        bool scanOk = false;
        bool needsFlatten = false;
        bool flattenOk = false;
        const uint8_t* auroraStream = nullptr;
        uint32_t auroraStreamBytes = 0;
        if (cached != nullptr) {
            const auto& entry = cached->result;
            scanOk = entry.scanOk;
            maxIdx = entry.maxIdx;
            sawIdx = entry.sawIdx;
            maxXfIdx = entry.maxXfIdx;
            maxXfBytes = entry.maxXfBytes;
            sawXfIdx = entry.sawXfIdx;
            dlVtxFmt = entry.dlVtxFmt;
            dlVtxFmtMixed = entry.dlVtxFmtMixed;
            dlHasNestedDl = entry.dlHasNestedDl;
            dlHasArrayStateWrites = entry.dlHasArrayStateWrites;
            dlHasCpWrites = entry.dlHasCpWrites;
            needsFlatten = entry.needsFlatten;
            flattenOk = entry.flattenOk;
            // A scan/flatten only mutates HLE vertex state through the CP writes
            // embedded in the list. Replaying the recorded writes in stream order
            // lands g_hleGxState exactly where the uncached path would have left it.
            if (dlHasCpWrites) {
                stateBeforeScan = CaptureGxVertexStateForCpWrites(cached->cpWrites);
                haveStateBeforeScan = true;
            }
            for (const auto& write : cached->cpWrites) {
                ApplyCpRegWrite(write.reg, write.value);
            }
            if (needsFlatten && flattenOk) {
                if (entry.flattenVerbatim) {
                    auroraStream = list;
                    auroraStreamBytes = nbytes;
                } else {
                    auroraStream = cached->flattened.data();
                    auroraStreamBytes = static_cast<uint32_t>(cached->flattened.size());
                }
            }
        } else {
            std::vector<DlCpWrite> cpWrites{};
            // The scan discovers the list's CP writes as it walks, so this path
            // cannot narrow the snapshot the way the cached one does.
            stateBeforeScan = CaptureGxVertexState();
            haveStateBeforeScan = true;
            scanOk = ScanDisplayListMaxIndices(list, nbytes, maxIdx, sawIdx, maxXfIdx, maxXfBytes, sawXfIdx,
                                               &dlVtxFmt, &dlVtxFmtMixed, &dlHasNestedDl,
                                               &dlHasArrayStateWrites, &dlHasCpWrites, &cpWrites);
            needsFlatten = dlHasNestedDl || dlHasArrayStateWrites || dlHasCpWrites;
            bool flattenVerbatim = true;
            auto& flattened = FlattenDisplayListScratch();
            if (needsFlatten) {
                RestoreGxVertexState(stateBeforeScan);
                flattened.clear();
                flattenOk = FlattenDisplayListForAurora(list, nbytes, flattened, dlHasArrayStateWrites,
                                                        &flattenVerbatim);
                if (flattenOk) {
                    if (flattenVerbatim) {
                        auroraStream = list;
                        auroraStreamBytes = nbytes;
                    } else {
                        auroraStream = flattened.data();
                        auroraStreamBytes = static_cast<uint32_t>(flattened.size());
                    }
                }
            }
            // Nested lists stay uncached: the digest only covers the outer list, so a
            // rewritten callee would go undetected. Everything else is cacheable since the
            // scan/flatten are pure functions of the list bytes and the layout-hash cache key.
            if (allowScanCache && scanOk && !dlHasNestedDl && (!needsFlatten || flattenOk)) {
                DlScanCacheEntry entry{};
                entry.listAddr = CanonicalizeGxMainRamAddress(listAddr);
                entry.nbytes = nbytes;
                entry.layoutHash = scanLayoutHash;
                entry.maxIdx = maxIdx;
                entry.sawIdx = sawIdx;
                entry.maxXfIdx = maxXfIdx;
                entry.maxXfBytes = maxXfBytes;
                entry.sawXfIdx = sawXfIdx;
                entry.dlVtxFmt = dlVtxFmt;
                entry.dlVtxFmtMixed = dlVtxFmtMixed;
                entry.dlHasNestedDl = dlHasNestedDl;
                entry.dlHasArrayStateWrites = dlHasArrayStateWrites;
                entry.dlHasCpWrites = dlHasCpWrites;
                entry.scanOk = scanOk;
                entry.needsFlatten = needsFlatten;
                entry.flattenOk = flattenOk;
                entry.flattenVerbatim = flattenVerbatim;
                const bool storeFlattened = needsFlatten && flattenOk && !flattenVerbatim;

                const uint64_t contentDigest =
                    probe.digestValid ? probe.contentDigest : DlContentDigest(list, nbytes);
                StoreDlScanCache(listAddr, nbytes, scanLayoutHash, contentDigest,
                                 probe.writeGeneration, mayContainDraw, entry, std::move(cpWrites),
                                 storeFlattened ? flattened.data() : nullptr,
                                 storeFlattened ? static_cast<uint32_t>(flattened.size()) : 0u);
            }
        }

        if (needsFlatten) {
            bool flattenApplyOk = flattenOk && scanOk && auroraStream != nullptr && auroraStreamBytes != 0;
            if (flattenApplyOk) {
                ApplyAuroraVtxDesc();
                ApplyAuroraVtxAttrFmtForDisplayList(dlVtxFmt, dlVtxFmtMixed);
                const bool flattenArraysOk =
                    ApplyAuroraArraysForDisplayList(maxIdx, sawIdx, dlVtxFmt, dlVtxFmtMixed);
                const bool flattenXfOk =
                    ApplyAuroraIndexedXFArraysForDisplayList(maxXfIdx, maxXfBytes, sawXfIdx);
                flattenApplyOk = flattenArraysOk && flattenXfOk;
            }
            if (flattenApplyOk) {
                EnsureAuroraFrameActive();
                GXMarkFrameWork();
                GXCallDisplayList(auroraStream, auroraStreamBytes);
                // No flip back to GX_DIRECT here
                SyncAppliedVtxStateFromHleReal();
                return;
            }

        } else {
            bool applyOk = scanOk;
            bool arraysOk = false;
            bool xfOk = false;
            if (scanOk) {
                ApplyAuroraVtxDesc();
                ApplyAuroraVtxAttrFmtForDisplayList(dlVtxFmt, dlVtxFmtMixed);
                arraysOk = ApplyAuroraArraysForDisplayList(maxIdx, sawIdx, dlVtxFmt, dlVtxFmtMixed);
                xfOk = ApplyAuroraIndexedXFArraysForDisplayList(maxXfIdx, maxXfBytes, sawXfIdx);
                applyOk = arraysOk && xfOk;
            }
            if (applyOk) {
                EnsureAuroraFrameActive();
                GXMarkFrameWork();
                GXCallDisplayList(list, nbytes);

                SyncAppliedVtxStateFromHleReal();
                return;
            }

        }

        if (haveStateBeforeScan) {
            RestoreGxVertexState(stateBeforeScan);
        }
        EnsureAuroraFrameActive();
        GXMarkFrameWork();
        InvalidateAppliedAuroraState();
        ParseDisplayList(list, nbytes);
        InvalidateAppliedAuroraState();
    } catch (...) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        throw;
#endif
    }
}

PPC_NATIVE_OVERRIDE_VOID(80172F64, GX__CallDisplayList_80172f64, (uint32_t listAddr, uint32_t nbytes), (listAddr, nbytes));

extern "C" void nw4r__lyt__detail__DrawQuad_800847c0(CpuContext* ctx) {
    uint32_t colors[4];
    const uint32_t colorAddr = ctx->gpr[7];
    const uint32_t* colorPtr = nullptr;
    if (colorAddr != 0) {
        colors[0] = ReadLytColor(colorAddr, 0);
        colors[1] = ReadLytColor(colorAddr, 1);
        colors[2] = ReadLytColor(colorAddr, 2);
        colors[3] = ReadLytColor(colorAddr, 3);
        colorPtr = colors;
    }

    EmitLytDrawQuad(ctx->gpr[3], ctx->gpr[4], static_cast<int32_t>(ctx->gpr[5]), ctx->gpr[6], colorPtr);
}

REGISTER_NATIVE_FUNCTION(0x800847C0, nw4r__lyt__detail__DrawQuad_800847c0);

extern "C" void nw4r__lyt__detail__DrawQuad_80084d20(CpuContext* ctx) {
    uint32_t colors[4];
    const uint32_t colorAddr = ctx->gpr[7];
    const uint32_t* colorPtr = nullptr;
    if (colorAddr != 0) {
        const uint32_t alpha = ctx->gpr[8];
        colors[0] = ReadModulatedLytColor(colorAddr, 0, alpha);
        colors[1] = ReadModulatedLytColor(colorAddr, 1, alpha);
        colors[2] = ReadModulatedLytColor(colorAddr, 2, alpha);
        colors[3] = ReadModulatedLytColor(colorAddr, 3, alpha);
        colorPtr = colors;
    }

    EmitLytDrawQuad(ctx->gpr[3], ctx->gpr[4], static_cast<int32_t>(ctx->gpr[5]), ctx->gpr[6], colorPtr);
}

REGISTER_NATIVE_FUNCTION(0x80084D20, nw4r__lyt__detail__DrawQuad_80084d20);
