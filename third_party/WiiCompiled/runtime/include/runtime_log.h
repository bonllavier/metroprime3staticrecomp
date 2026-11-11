#pragma once

#ifndef MKW_RUNTIME_LOG_H
#define MKW_RUNTIME_LOG_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "memory.h"

// Canonical module tags. Use one of these; never write a bare "[...]" prefix
// into a message. (A message whose text spans several output lines repeats the
// tag inline on the continuation lines - the macros only prefix the first.)
#define RT_TAG_RUNTIME "runtime"
#define RT_TAG_CONFIG "runtime-config"
#define RT_TAG_MEMORY "memory"
#define RT_TAG_MOD "mod"
#define RT_TAG_HLE "hle"
#define RT_TAG_OS "os"
#define RT_TAG_GX "gx"
#define RT_TAG_AUDIO "audio"
#define RT_TAG_NET "net"
#define RT_TAG_DVD "dvd"
#define RT_TAG_NAND "nand"
#define RT_TAG_RIIVOLUTION "riivolution"
#define RT_TAG_VI "vi"

// Stream form:  RT_LOG(RT_TAG_OS) << "OSCreateThread failed" << std::endl;
#define RT_LOG(tag) (std::cerr << "[" tag "] ")

// Per-I/O chatter (every disc read, IOS ioctl): the first occurrences of a
// site are logged, the rest only with WII_COMPILED_TRACE_PRIME_IO=1.
//   static std::atomic<uint32_t> seen; if (RtLogRepeated(seen)) RT_LOG(...) << ...;
inline bool RtLogRepeated(std::atomic<uint32_t>& seen, uint32_t first = 32) {
    static const bool all = std::getenv("WII_COMPILED_TRACE_PRIME_IO") != nullptr;
    return all || seen.fetch_add(1, std::memory_order_relaxed) < first;
}

// printf form:  RT_LOGF(RT_TAG_GX, "invalid GXTexObj @0x%08X\n", addr);
// `tag` and the format string must both be literals; they are concatenated.
#define RT_LOGF(tag, ...) std::fprintf(stderr, "[" tag "] " __VA_ARGS__)

// Shared epilogue for the `catch (const Memory::AccessViolation& e)` handlers
// spread across the HLE. `who` is the guest function or operation that faulted;
// the tag is its module.
inline void LogMemoryError(const char* tag, const char* who,
                           const ::Memory::AccessViolation& e)
{
    std::cerr << "[" << tag << "] " << who << ": memory error at 0x" << std::hex
              << e.address() << std::dec << " (" << e.reason() << ")" << std::endl;
}

#endif // MKW_RUNTIME_LOG_H
