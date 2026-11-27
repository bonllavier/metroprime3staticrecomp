// Reports from the ctx-flush elision verification build (guest_abi/ctx_flush_check.h).
// Linked in every build; only shards emitted with --ctx-flush-elision verify call it.

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "guest_abi/ctx_flush_check.h"
#include "runtime_log.h"

extern "C" void MkwCtxFlushMismatch(const char* function, const char* reg,
                                    uint64_t slotBits, uint64_t localBits) noexcept
{
    static std::atomic<uint64_t> total{0};
    static std::mutex mutex;
    static std::set<std::pair<std::string, std::string>> seen;
    const uint64_t count = total.fetch_add(1, std::memory_order_relaxed) + 1;
    try {
        std::lock_guard<std::mutex> lock(mutex);
        if (seen.size() >= 4096u) return;
        if (!seen.emplace(function ? function : "?", reg ? reg : "?").second) {
            if ((count & 0xFFFFu) == 0)
                RT_LOG(RT_TAG_HLE) << "[ctx-flush-check] mismatches so far: " << count << std::endl;
            return;
        }
        RT_LOG(RT_TAG_HLE) << "[ctx-flush-check] mismatch in " << (function ? function : "?")
                           << " " << (reg ? reg : "?") << " slot=0x" << std::hex << slotBits
                           << " local=0x" << localBits << std::dec << " (total " << count
                           << ", distinct " << seen.size() << ")" << std::endl;
    } catch (...) {
    }
}
