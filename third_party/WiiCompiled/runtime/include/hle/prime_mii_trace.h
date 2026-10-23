#pragma once
#include "memory.h"
#include "runtime_log.h"
#include <cstdlib>
#include <cstdint>

// Observation only: the original RFL routine decides whether a profile is valid.
inline void PrimeTraceMiiLookup(uint32_t index, uint32_t result) {
    static const bool enabled = std::getenv("WII_COMPILED_TRACE_PRIME_MII") != nullptr;
    if (!enabled) return;
    static uint32_t observations = 0;
    if (observations++ >= 256) return;
    const bool readable = result && Memory::Contains(result, 74);
    RT_LOG(RT_TAG_NAND) << "[prime-mii-lookup] index=" << index
        << " result=0x" << std::hex << result
        << " id0=0x" << (readable ? Memory::Read32(result + 24) : 0)
        << " id1=0x" << (readable ? Memory::Read32(result + 28) : 0)
        << " name0=0x" << (readable ? Memory::Read32(result + 2) : 0)
        << std::dec << std::endl;
}
