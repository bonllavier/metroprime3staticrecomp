#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace GxFifoStorage {
// Reentrant producers may append while a backend waits, but only the outer
// caller may advance the FIFO consumer. A rejected nested scope must not
// release the outer owner; exceptions must release ownership.
class ConsumerScope {
    bool& active_;
    bool owns_;
public:
    explicit ConsumerScope(bool& active) : active_(active), owns_(!active) { if (owns_) active_ = true; }
    ~ConsumerScope() { if (owns_) active_ = false; }
    ConsumerScope(const ConsumerScope&) = delete;
    ConsumerScope& operator=(const ConsumerScope&) = delete;
    explicit operator bool() const { return owns_; }
};

// A maximal XF packet is 262149 bytes; maximal direct vertex packets fit below
// 16 MiB (65535 vertices, nine matrix indices plus all legal attributes).
inline constexpr size_t MaxBufferedBytes = 16u * 1024u * 1024u;
// Direct GX vertices include every enabled matrix index and attribute. If all
// complete records are identical, a triangle assembled from them has zero area.
// This predicate says nothing about lines/points or indexed array records.
inline bool AllDirectVerticesIdentical(const uint8_t* data,size_t bytes,size_t count,size_t stride) {
    if (!data || count<3 || stride==0 || count>MaxBufferedBytes/stride || bytes!=count*stride) return false;
    for(size_t i=1;i<count;++i)
        if(std::memcmp(data,data+i*stride,stride)!=0) return false;
    return true;
}

inline void EnsureAppendSpace(std::vector<uint8_t>& bytes, size_t& readOffset,
                              size_t pending, size_t appended) {
    if (readOffset > bytes.size() || pending > bytes.size() - readOffset)
        throw std::logic_error("Invalid FIFO storage range");
    if (appended > MaxBufferedBytes || pending > MaxBufferedBytes - appended)
        throw std::length_error("FIFO packet exceeds supported hardware packet bound");
    if (appended <= bytes.size() - readOffset - pending) return;
    if (readOffset != 0) {
        std::memmove(bytes.data(), bytes.data() + readOffset, pending);
        readOffset = 0;
    }
    const size_t required = pending + appended;
    if (required > bytes.size())
        bytes.resize(std::min(MaxBufferedBytes, std::max(required, bytes.size() * 2u)));
}
}
