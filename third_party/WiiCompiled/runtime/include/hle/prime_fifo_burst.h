#pragma once
#include <cstddef>
#include <cstdint>

namespace PrimeFifoBurst {
// Translated consecutive stores are a byte stream, even if CP reads are
// stopped or another consumer owns the decoder. Never scan these bytes for
// commands here: zeroes can belong to an incomplete retained vertex packet.
template <class Writer>
void Write(const uint8_t* data, size_t size, Writer&& write) {
    size_t offset=0;
    while (offset<size) {
        const unsigned width=size-offset>=4 ? 4u : size-offset>=2 ? 2u : 1u;
        uint32_t value=0;
        for(unsigned i=0;i<width;++i) value=(value<<8)|data[offset+i];
        write(value,width);
        offset+=width;
    }
}
}
