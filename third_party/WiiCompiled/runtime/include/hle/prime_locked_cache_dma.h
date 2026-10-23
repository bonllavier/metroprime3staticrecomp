#pragma once
#include <cstdint>
#include <stdexcept>

// Register fields cross-checked against the original RM3E01 SDK 0x804DFB30
// and Dolphin Gekko.h (DMAU/DMAL). This adapter completes byte transfers
// synchronously; it does not reproduce the hardware queue or DMA timing.
namespace PrimeLockedCacheDma {
struct Transfer { uint32_t memory, cache, bytes; bool load; };
inline Transfer Decode(uint32_t upper,uint32_t lower) {
    uint32_t blocks=((upper&31u)<<2)|((lower>>2)&3u);
    if(!blocks)blocks=128;
    const uint32_t offset=lower&0x3FFE0u;
    const uint32_t bytes=blocks*32;
    if(offset+bytes>0x4000u)throw std::runtime_error("Prime LC DMA exceeds 16 KiB locked cache");
    return {upper&0xFFFFFFE0u,0xE0000000u+offset,bytes,(lower&16u)!=0};
}
struct Registers {
    uint32_t upper=0,lower=0;
    void WriteUpper(uint32_t value){upper=value;}
    template<class Copy> void WriteLower(uint32_t value,Copy copy) {
        lower=value;
        if(lower&2u){copy(Decode(upper,lower));lower&=~2u;}
    }
};
}
