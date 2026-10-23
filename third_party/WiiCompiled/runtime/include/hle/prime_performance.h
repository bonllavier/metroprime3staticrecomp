#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ostream>

namespace PrimePerformance {
enum Path { BpApply, TextureBind, PaletteScan, RawDraw, ColorCopy, IsoRead, Count };
struct Counter { std::atomic<uint64_t> calls{0}, nanoseconds{0}; };
inline std::array<Counter, Count> counters;
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("WII_COMPILED_PRIME_PERFORMANCE_TRACE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}
class Scope {
    using Clock = std::chrono::steady_clock;
    const Path path_;
    const bool enabled_;
    Clock::time_point started_{};
public:
    explicit Scope(Path path) : path_(path), enabled_(Enabled()) {
        if (enabled_) started_ = Clock::now();
    }
    ~Scope() {
        if (!enabled_) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-started_).count();
        counters[path_].nanoseconds.fetch_add(elapsed, std::memory_order_relaxed);
        counters[path_].calls.fetch_add(1, std::memory_order_relaxed);
    }
};
inline void Report(std::ostream& out) {
    constexpr std::array<const char*, Count> names{"bp_apply", "texture_bind", "palette_scan", "raw_draw", "color_copy", "iso_read"};
    out << "[prime-render-cpu] inclusive=1";
    for (unsigned i = 0; i < Count; ++i) {
        const auto calls = counters[i].calls.exchange(0, std::memory_order_relaxed);
        const auto ns = counters[i].nanoseconds.exchange(0, std::memory_order_relaxed);
        out << ' ' << names[i] << "_calls=" << calls << ' ' << names[i] << "_ms=" << double(ns)/1000000.0;
    }
    out << std::endl;
}
} // namespace PrimePerformance
