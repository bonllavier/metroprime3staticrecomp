#pragma once
#include "memory.h"
#include "runtime_log.h"
#include <cstdlib>
#include <cstdint>

inline void PrimeTraceNandUsage(uint32_t phase, uint32_t source,
                                uint32_t destination, uint32_t bytes) {
    static const bool enabled = std::getenv("WII_COMPILED_TRACE_PRIME_NAND") != nullptr;
    if (!enabled) return;
    static uint32_t observations = 0;
    if (observations++ >= 48) return;
    RT_LOG(RT_TAG_NAND) << "[prime-nand-sdk] phase=" << phase
        << " source=0x" << std::hex << source << " destination=0x" << destination
        << " bytes=0x" << bytes << " source_word=0x"
        << (Memory::Contains(source, 4) ? Memory::Read32(source) : 0)
        << " destination_word=0x"
        << (destination && Memory::Contains(destination, 4) ? Memory::Read32(destination) : 0)
        << std::dec << std::endl;
}
