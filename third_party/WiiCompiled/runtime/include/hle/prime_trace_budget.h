#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Bound repetitive informational traces only. Errors must bypass this budget.
// Cache the opt-in once; changing the environment requires a new process.
inline bool PrimeVerboseTraceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("WII_COMPILED_PRIME_VERBOSE_TRACE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

class PrimeTraceBudget {
    std::atomic<uint32_t> remaining_;
public:
    explicit PrimeTraceBudget(uint32_t limit) : remaining_(limit) {}
    bool Take(bool verbose = PrimeVerboseTraceEnabled()) {
        if (verbose) return true;
        auto remaining = remaining_.load(std::memory_order_relaxed);
        while (remaining != 0) {
            if (remaining_.compare_exchange_weak(remaining, remaining - 1,
                                               std::memory_order_relaxed)) return true;
        }
        return false;
    }
};
