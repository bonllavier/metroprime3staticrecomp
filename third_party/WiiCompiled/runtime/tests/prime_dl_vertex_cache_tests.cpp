#include "hle/prime_dl_vertex_cache.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <map>

#define require(ok) do { if (!(ok)) { std::cerr << "FAIL line " << __LINE__ << ": " << #ok << "\n"; std::exit(1); } } while (false)

int main() {
    using namespace PrimeDlVertexCache;
    // Guest RAM: a position array of 4-byte entries at 0x1000, read through
    // 8-bit indices, plus a direct 2-byte attribute in each source vertex.
    std::array<uint8_t, 0x2000> ram{};
    for (size_t i = 0; i < 64; ++i)
        for (size_t b = 0; b < 4; ++b) ram[0x1000 + i * 4 + b] = uint8_t(i * 4 + b);
    std::vector<PrimeDlVertices::Attribute> attributes(2);
    attributes[0] = {4, 1, 1, 0x1000, 4, ram.data() + 0x1000, 255 * 4 + 4};
    attributes[1] = {2, 0, 1, 0, 0, nullptr, 0};
    std::vector<uint8_t> source{3, 0xA0, 0xA1, 7, 0xB0, 0xB1, 5, 0xC0, 0xC1};
    const auto read = [&](uint32_t address, size_t, const uint8_t*& span) { span = ram.data() + address; return true; };
    std::vector<uint8_t> expanded;
    require(PrimeDlVertices::Expand(source.data(), source.size(), 3, attributes, expanded, read, 4096));

    std::map<uint32_t, uint64_t> granuleGeneration;  // 0x100-byte granules for the test
    const auto generation = [&](uint32_t first, uint32_t bytes) {
        uint64_t sum = 0;
        for (uint32_t g = first >> 8; g <= (first + bytes - 1) >> 8; ++g) sum += granuleGeneration[g];
        return sum;
    };
    std::vector<Span> spans;
    require(ReadSpans(source.data(), 3, attributes, spans));
    require(spans.size() == 1 && spans[0].first == 0x1000 + 3 * 4 && spans[0].bytes == (7 - 3) * 4 + 4);

    Cache cache(1 << 20);
    const Key key{source.data(), uint32_t(source.size()), 3, 0, LayoutSignature(attributes)};
    const uint64_t digest = 0x1234;
    require(cache.Find(key, digest, generation) == nullptr);
    cache.Store(key, digest, Cache::ArrayGeneration(spans, generation), spans, expanded);
    const auto* hit = cache.Find(key, digest, generation);
    require(hit && *hit == expanded && cache.Hits() == 1);

    // A write outside the read range keeps the entry; one inside retires it.
    granuleGeneration[0x00]++;
    require(cache.Find(key, digest, generation) != nullptr);
    granuleGeneration[0x10]++;
    require(cache.Find(key, digest, generation) == nullptr && cache.Stale() == 1 && cache.Entries() == 0);

    // New source bytes (different digest) are a miss, never a reuse.
    cache.Store(key, digest, Cache::ArrayGeneration(spans, generation), spans, expanded);
    require(cache.Find(key, digest + 1, generation) == nullptr);

    // A different layout (array moved) is a different key.
    auto moved = attributes;
    moved[0].arrayBase = 0x1100;
    require(LayoutSignature(moved) != LayoutSignature(attributes));

    // Untracked ranges and multi-group attributes are never cached.
    cache.Store(key, digest, kUntracked, spans, expanded);
    require(cache.Entries() == 0);
    auto grouped = attributes;
    grouped[0].groups = 3;
    require(!ReadSpans(source.data(), 3, grouped, spans));

    // Budget: entries above an eighth of the budget are not kept; LRU evicts.
    Cache small(64);
    small.Store(key, digest, 1, {}, std::vector<uint8_t>(16));
    require(small.Entries() == 0);
    Cache lru(8 * 16);
    for (uint32_t i = 0; i < 12; ++i) {
        Key k = key;
        k.count = 100 + i;
        lru.Store(k, digest, 1, {}, std::vector<uint8_t>(16));
    }
    require(lru.Bytes() <= 8 * 16 && lru.Entries() == 8);

    std::cout << "PASS DL vertex cache: reuse, array and source invalidation, layout key, untracked, budget\n";
}
