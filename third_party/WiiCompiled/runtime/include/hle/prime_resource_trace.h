#pragma once
#include "memory.h"
#include "runtime_log.h"
#include <cstdlib>
#include <cstdint>

// Read-only evidence before the original stream/decode operations.
inline void PrimeTraceResourceStream(uint32_t phase, uint32_t stream,
                                    uint32_t source, uint32_t length,
                                    uint32_t destination, uint32_t limit) {
    static const bool enabled = std::getenv("WII_COMPILED_TRACE_PRIME_RESOURCE") != nullptr;
    if (!enabled) return;
    static unsigned count = 0;
    if (count++ >= 384 || !Memory::Contains(stream, 40)) return;
    RT_LOG(RT_TAG_RUNTIME) << "[prime-resource-stream] phase=" << phase
        << " stream=0x" << std::hex << stream
        << " cursor=0x" << Memory::Read32(stream)
        << " end=0x" << Memory::Read32(stream+4)
        << " output_cursor=0x" << Memory::Read32(stream+8)
        << " input=0x" << Memory::Read32(stream+16)
        << " resident=0x" << Memory::Read32(stream+20)
        << " blocks=0x" << Memory::Read32(stream+24)
        << " source=0x" << source << " length=0x" << length
        << " destination=0x" << destination << " limit=0x" << limit
        << " source_word=0x" << (Memory::Contains(source,4)?Memory::Read32(source):0)
        << std::dec << std::endl;
}
