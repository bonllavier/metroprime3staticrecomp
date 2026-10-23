#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
class PrimeTlutMemory {
    std::vector<uint8_t> bytes_ = std::vector<uint8_t>(1024u*1024u);
    std::vector<uint8_t> valid_ = std::vector<uint8_t>(1024u*1024u);
    uint32_t generation_ = 0;
public:
    void Load(uint32_t offset, const void* source, uint32_t count) {
        if (offset > bytes_.size() || count > bytes_.size()-offset || (!source && count))
            throw std::out_of_range("TLUT transfer exceeds TMEM");
        if (count) std::memcpy(bytes_.data()+offset,source,count);
        std::fill_n(valid_.data()+offset,count,uint8_t(1)); ++generation_;
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
};
