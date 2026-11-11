#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
class PrimeTlutMemory {
    static constexpr uint32_t kGranuleSize = 512;
    static constexpr size_t kGranuleCount = (1024u * 1024u) / kGranuleSize;
    std::vector<uint8_t> bytes_ = std::vector<uint8_t>(1024u*1024u);
    std::vector<uint8_t> valid_ = std::vector<uint8_t>(1024u*1024u);
    uint32_t generation_ = 0;
    std::array<uint64_t, kGranuleCount> granuleGenerations_{};
public:
    void Load(uint32_t offset, const void* source, uint32_t count) {
        if (offset > bytes_.size() || count > bytes_.size()-offset || (!source && count))
            throw std::out_of_range("TLUT transfer exceeds TMEM");
        if (count) {
            const bool uninitialized = std::memchr(valid_.data()+offset,0,count)!=nullptr;
            const bool changed = uninitialized || std::memcmp(bytes_.data()+offset,source,count)!=0;
            if (changed) {
                std::memcpy(bytes_.data()+offset,source,count);
                const uint32_t first = offset / kGranuleSize;
                const uint32_t last = (offset + count - 1) / kGranuleSize;
                for (uint32_t granule = first; granule <= last; ++granule) ++granuleGenerations_[granule];
            }
            std::fill_n(valid_.data()+offset,count,uint8_t(1));
        }
        ++generation_;
    }
    const void* Span(uint32_t offset,uint32_t count) const {
        if (offset > bytes_.size() || count > bytes_.size()-offset ||
            // Validity bytes are exactly zero or one. The CRT's vectorized
            // zero search preserves the full-span proof without a scalar
            // branch for every palette byte on every indexed draw.
            (count && std::memchr(valid_.data()+offset,0,count)!=nullptr))
            throw std::runtime_error("Palette references TLUT bytes not loaded by the original GPU commands");
        return bytes_.data()+offset;
    }
    uint32_t Generation() const { return generation_; }
    uint32_t RangeGeneration(uint32_t offset, uint32_t count) const {
        if (offset > bytes_.size() || count > bytes_.size() - offset)
            throw std::out_of_range("TLUT generation range exceeds TMEM");
        if (count == 0) return 0;
        const uint32_t first = offset / kGranuleSize;
        const uint32_t last = (offset + count - 1) / kGranuleSize;
        uint64_t folded = 0;
        for (uint32_t granule = first; granule <= last; ++granule) folded += granuleGenerations_[granule];
        return static_cast<uint32_t>(folded);
    }
};
