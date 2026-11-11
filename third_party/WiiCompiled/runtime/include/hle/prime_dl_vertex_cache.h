#pragma once
// Cross-frame reuse of expanded display-list vertices (PrimeDlVertices::Expand
// output). Static world geometry is drawn every frame, and several times per
// frame by multi-pass effects such as the scan visor; its expansion only
// depends on the draw's encoded bytes and on the array bytes its indices read.
//
// A cached expansion is reused only when all of these still hold:
//  * the same source pointer, byte count, vertex count, format and attribute
//    layout (sizes, index widths, groups, array bases and strides);
//  * the encoded source bytes are identical (64-bit XXH3 digest);
//  * the write generation of every array range the indices actually read is
//    unchanged. Generations advance on DMA and on the DC flush/store/zero
//    hooks (recomp/docs/copias_efb_y_cache.md), which the CPU must issue
//    before the GPU may see new vertex data.
// Anything untracked (uncached-mirror arrays, untracked generation ranges,
// multi-group attributes) is never cached, so it keeps the exact old path.
#include "hle/prime_dl_vertex_expansion.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

namespace PrimeDlVertexCache {
inline constexpr uint64_t kUntracked = ~0ull;

struct Key {
    const uint8_t* source = nullptr;
    uint32_t sourceBytes = 0;
    uint32_t count = 0;
    uint32_t format = 0;
    uint64_t layout = 0;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    size_t operator()(const Key& key) const noexcept {
        uint64_t h = reinterpret_cast<uintptr_t>(key.source) * 0x9E3779B97F4A7C15ull;
        h ^= (uint64_t(key.sourceBytes) << 32 | key.count) + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
        h ^= key.layout + (uint64_t(key.format) << 1) + (h << 6) + (h >> 2);
        return static_cast<size_t>(h);
    }
};

// Array bytes one attribute's indices read: [first, first + bytes).
struct Span {
    uint32_t first = 0;
    uint32_t bytes = 0;
};

inline uint64_t LayoutSignature(const std::vector<PrimeDlVertices::Attribute>& attributes) {
    uint64_t h = 0xCBF29CE484222325ull;
    const auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
    for (const auto& a : attributes) {
        mix(uint64_t(a.bytes) | uint64_t(a.indexBytes) << 16 | uint64_t(a.groups) << 24);
        mix(uint64_t(a.arrayBase) | uint64_t(a.arrayStride) << 32);
    }
    mix(attributes.size());
    return h;
}

// Index ranges the source reads, per indexed attribute. False when an
// attribute cannot be tracked (multiple groups, no array, overflow).
inline bool ReadSpans(const uint8_t* source, uint32_t count,
                      const std::vector<PrimeDlVertices::Attribute>& attributes, std::vector<Span>& spans) {
    spans.clear();
    size_t sourceStride = 0;
    for (const auto& a : attributes) sourceStride += a.indexBytes ? size_t(a.indexBytes) * a.groups : a.bytes;
    size_t offset = 0;
    for (const auto& a : attributes) {
        if (!a.indexBytes) {
            offset += a.bytes;
            continue;
        }
        if (a.groups != 1 || !a.arrayBase || !a.arrayStride) return false;
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t v = 0; v < count; ++v) {
            const uint8_t* at = source + v * sourceStride + offset;
            const uint32_t index = a.indexBytes == 2 ? (uint32_t(at[0]) << 8 | at[1]) : at[0];
            lo = index < lo ? index : lo;
            hi = index > hi ? index : hi;
        }
        const uint64_t first = uint64_t(a.arrayBase) + uint64_t(lo) * a.arrayStride;
        const uint64_t end = uint64_t(a.arrayBase) + uint64_t(hi) * a.arrayStride + a.bytes;
        if (end > 0x100000000ull || end <= first) return false;
        spans.push_back({uint32_t(first), uint32_t(end - first)});
        offset += a.indexBytes;
    }
    return true;
}

struct Entry {
    Key key;
    uint64_t sourceDigest = 0;
    uint64_t arrayGeneration = 0;
    std::vector<Span> spans;
    std::vector<uint8_t> vertices;
};

// Not thread safe: owned by the thread that interprets display lists.
class Cache {
public:
    explicit Cache(size_t byteBudget) : m_budget(byteBudget) {}

    // Generation(first, bytes) -> folded write generation or kUntracked.
    template <class Generation>
    static uint64_t ArrayGeneration(const std::vector<Span>& spans, Generation&& generation) {
        uint64_t folded = 0;
        for (const auto& span : spans) {
            const uint64_t g = generation(span.first, span.bytes);
            if (g == kUntracked) return kUntracked;
            folded = folded * 0x9E3779B97F4A7C15ull + g + 1;
        }
        return folded;
    }

    // The cached vertices when every proof still holds, else nullptr.
    template <class Generation>
    const std::vector<uint8_t>* Find(const Key& key, uint64_t sourceDigest, Generation&& generation) {
        ++m_lookups;
        const auto it = m_index.find(key);
        if (it == m_index.end()) return nullptr;
        Entry& entry = *it->second;
        if (entry.sourceDigest != sourceDigest ||
            ArrayGeneration(entry.spans, generation) != entry.arrayGeneration) {
            ++m_stale;
            Erase(it);
            return nullptr;
        }
        m_lru.splice(m_lru.begin(), m_lru, it->second);
        ++m_hits;
        return &entry.vertices;
    }

    void Store(const Key& key, uint64_t sourceDigest, uint64_t arrayGeneration, std::vector<Span> spans,
               const std::vector<uint8_t>& vertices) {
        if (arrayGeneration == kUntracked || vertices.size() > m_budget / 8) return;
        if (const auto it = m_index.find(key); it != m_index.end()) Erase(it);
        m_lru.push_front(Entry{key, sourceDigest, arrayGeneration, std::move(spans), vertices});
        m_index.emplace(key, m_lru.begin());
        m_bytes += m_lru.front().vertices.size();
        while (m_bytes > m_budget && !m_lru.empty()) {
            const auto last = std::prev(m_lru.end());
            Erase(m_index.find(last->key));
        }
    }

    size_t Bytes() const { return m_bytes; }
    size_t Entries() const { return m_index.size(); }
    uint64_t Lookups() const { return m_lookups; }
    uint64_t Hits() const { return m_hits; }
    uint64_t Stale() const { return m_stale; }

private:
    using List = std::list<Entry>;
    void Erase(typename std::unordered_map<Key, List::iterator, KeyHash>::iterator it) {
        m_bytes -= it->second->vertices.size();
        m_lru.erase(it->second);
        m_index.erase(it);
    }
    size_t m_budget;
    size_t m_bytes = 0;
    List m_lru;
    std::unordered_map<Key, List::iterator, KeyHash> m_index;
    uint64_t m_lookups = 0, m_hits = 0, m_stale = 0;
};
} // namespace PrimeDlVertexCache
