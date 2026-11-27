#pragma once

// Verification build of the translator's ctx-flush elision (emit-build-shards
// --ctx-flush-elision verify). Each store the analysis proved redundant becomes
// MKW_CTX_FLUSH_CHECK: when the slot does not hold the local's value the
// analysis was wrong for that site, so it is reported (once per function and
// register) and the store is performed, keeping the game correct. A build that
// plays without reports supports emitting the same shards with "elide".

#include <cstdint>

#include "guest_abi/cpu_context.h"

extern "C" void MkwCtxFlushMismatch(const char* function, const char* reg,
                                    uint64_t slotBits, uint64_t localBits) noexcept;

inline bool MkwCtxFlushSame(uint32_t slot, uint32_t local) noexcept { return slot == local; }
inline bool MkwCtxFlushSame(const PPC_FPR& slot, const PPC_FPR& local) noexcept { return slot.raw == local.raw; }
inline uint64_t MkwCtxFlushBits(uint32_t value) noexcept { return value; }
inline uint64_t MkwCtxFlushBits(const PPC_FPR& value) noexcept { return value.raw; }

#define MKW_CTX_FLUSH_CHECK(slot, local, function, reg)                                          \
    do {                                                                                         \
        if (!MkwCtxFlushSame((slot), (local))) [[unlikely]] {                                    \
            MkwCtxFlushMismatch((function), (reg), MkwCtxFlushBits(slot), MkwCtxFlushBits(local)); \
            (slot) = (local);                                                                    \
        }                                                                                        \
    } while (false)

// Verification of the nonvolatile reload elision (--nonvolatile-reload-elision verify): a reload of
// r1/r2/r13-r31 after a direct call that the analysis proved redundant. A mismatch means the callee
// did not preserve the register; it is reported and the reload is performed.
#define MKW_CTX_RELOAD_CHECK(local, slot, function, reg)                                         \
    do {                                                                                         \
        if (!MkwCtxFlushSame((slot), (local))) [[unlikely]] {                                    \
            MkwCtxFlushMismatch((function), (reg), MkwCtxFlushBits(slot), MkwCtxFlushBits(local)); \
            (local) = (slot);                                                                    \
        }                                                                                        \
    } while (false)
