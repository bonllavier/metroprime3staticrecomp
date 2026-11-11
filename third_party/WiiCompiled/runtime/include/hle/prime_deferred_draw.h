#pragma once
// Payload of a deferred packed display-list draw (aurora_stream_deferred_draw):
// the GX thread sends the attribute plan and the encoded indices of a draw and
// the asynchronous decoder thread expands it (PrimeExpandDeferredDraw in
// gx_dl.cpp). Both ends are this process, so host byte order and the array
// host pointers (proved mapped for the whole index domain by the producer,
// PrimeDlVertices::ExpandCannotFail) are kept as they are.
//
// Layout: Header, Header::attributeCount Packed attributes, then the source bytes.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "hle/prime_dl_vertex_expansion.h"

namespace PrimeDeferredDraw {

struct Header {
    uint32_t magic = 0;
    uint8_t command = 0;  // GX draw command: primitive | vertex format
    uint8_t reserved = 0;
    uint16_t count = 0;
    uint32_t sourceBytes = 0;
    uint32_t attributeCount = 0;
    uint32_t snapshotBytes = 0;  // array bytes copied after the source (Snapshot)
    uint32_t pad = 0;
    uint64_t verifyHash = 0;  // diagnostics: hash of the producer's own expansion (0: none)
};
struct Packed {
    uint32_t bytes, indexBytes, groups, arrayBase, arrayStride, reserved;
    uint64_t arrayData, arrayBytes;
    // Snapshot of the array bytes the indices read, copied when the draw was written:
    // [arrayData + snapshotSkip, + snapshotSize) lives at payload + snapshotOffset (~0u: none).
    uint32_t snapshotOffset, snapshotSkip, snapshotSize, pad;
};
inline constexpr uint32_t kMagic = 0x50444457u;  // "PDDW"
inline constexpr uint32_t kMaxAttributes = 26;

// Bytes of one attribute's array to copy into the payload: skip from its array start, size.
struct Snapshot {
    uint32_t skip = 0, size = 0;  // size 0: read the array itself
};

// `snapshots` (optional) is parallel to `attributes`.
inline void Encode(uint8_t command, uint16_t count, const uint8_t* source, uint32_t sourceBytes,
                   const std::vector<PrimeDlVertices::Attribute>& attributes, std::vector<uint8_t>& out,
                   const std::vector<Snapshot>* snapshots = nullptr,
                   uint64_t verifyHash = 0) {
    uint32_t snapshotBytes = 0;
    if (snapshots)
        for (const auto& snap : *snapshots) snapshotBytes += snap.size;
    Header header{kMagic, command, 0, count, sourceBytes, static_cast<uint32_t>(attributes.size()), snapshotBytes, 0,
                  verifyHash};
    const size_t sourceAt = sizeof(Header) + attributes.size() * sizeof(Packed);
    out.resize(sourceAt + sourceBytes + snapshotBytes);
    std::memcpy(out.data(), &header, sizeof(header));
    uint8_t* at = out.data() + sizeof(Header);
    uint32_t snapshotAt = static_cast<uint32_t>(sourceAt + sourceBytes);
    for (size_t i = 0; i < attributes.size(); ++i) {
        const auto& a = attributes[i];
        Packed packed{a.bytes, a.indexBytes, a.groups, a.arrayBase, a.arrayStride, 0,
                      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(a.arrayData)),
                      static_cast<uint64_t>(a.arrayBytes), ~0u, 0, 0, 0};
        if (snapshots && i < snapshots->size() && (*snapshots)[i].size != 0) {
            const auto& snap = (*snapshots)[i];
            packed.snapshotOffset = snapshotAt;
            packed.snapshotSkip = snap.skip;
            packed.snapshotSize = snap.size;
            std::memcpy(out.data() + snapshotAt, a.arrayData + snap.skip, snap.size);
            snapshotAt += snap.size;
        }
        std::memcpy(at, &packed, sizeof(packed));
        at += sizeof(packed);
    }
    if (sourceBytes) std::memcpy(at, source, sourceBytes);
}

struct View {
    uint8_t command = 0;
    uint16_t count = 0;
    const uint8_t* source = nullptr;  // inside the payload
    uint32_t sourceBytes = 0;
    uint64_t verifyHash = 0;
};

// A snapshot next to the live array bytes it was copied from (diagnostics: did the game change them?).
struct SnapshotUse {
    const uint8_t* live = nullptr;
    const uint8_t* copy = nullptr;
    uint32_t size = 0;
    uint32_t arrayBase = 0;
};

// False for a payload this version did not write (truncated, wrong magic, too many attributes).
// Attributes with a snapshot read it instead of the array (arrayData points so that the indices land
// in the copy); `uses` (optional) lists them.
inline bool Decode(const uint8_t* payload, size_t size, View& view, std::vector<PrimeDlVertices::Attribute>& attributes,
                   std::vector<SnapshotUse>* uses = nullptr) {
    if (payload == nullptr || size < sizeof(Header)) return false;
    Header header;
    std::memcpy(&header, payload, sizeof(header));
    if (header.magic != kMagic || header.attributeCount > kMaxAttributes) return false;
    const size_t expected = sizeof(Header) + size_t(header.attributeCount) * sizeof(Packed) + header.sourceBytes +
                            header.snapshotBytes;
    if (size != expected) return false;
    attributes.resize(header.attributeCount);
    if (uses) uses->clear();
    const uint8_t* at = payload + sizeof(Header);
    for (auto& a : attributes) {
        Packed packed;
        std::memcpy(&packed, at, sizeof(packed));
        at += sizeof(packed);
        a.bytes = packed.bytes;
        a.indexBytes = packed.indexBytes;
        a.groups = packed.groups;
        a.arrayBase = packed.arrayBase;
        a.arrayStride = packed.arrayStride;
        a.arrayData = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(packed.arrayData));
        a.arrayBytes = static_cast<size_t>(packed.arrayBytes);
        if (packed.snapshotOffset != ~0u) {
            if (size_t(packed.snapshotOffset) + packed.snapshotSize > size ||
                size_t(packed.snapshotSkip) + packed.snapshotSize > a.arrayBytes)
                return false;
            const uint8_t* copy = payload + packed.snapshotOffset;
            if (uses) uses->push_back({a.arrayData + packed.snapshotSkip, copy, packed.snapshotSize, a.arrayBase});
            // Indices outside [skip, skip + size) are never read: the producer copied their whole range.
            a.arrayData = copy - packed.snapshotSkip;
            a.arrayBytes = size_t(packed.snapshotSkip) + packed.snapshotSize;
        }
    }
    view = {header.command, header.count, at, header.sourceBytes, header.verifyHash};
    return true;
}

}  // namespace PrimeDeferredDraw
