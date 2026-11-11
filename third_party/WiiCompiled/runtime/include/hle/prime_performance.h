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
#if !defined(MKW_PRIME_QA_TOOLS)
    return false;  // public builds carry no performance trace
#else
    static const bool enabled = [] {
        const char* value = std::getenv("WII_COMPILED_PRIME_PERFORMANCE_TRACE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
#endif
}
// The GX thread enters BpApply ~9,000 times and PaletteScan ~3,000 times per
// frame in heavy scenes: timing each call (two clock reads and two atomics) took
// ~5 % of that thread with the trace on (F8 Norion, 2026-10-30). Hot paths time
// one call in kSampleEvery per thread and count the batch; the reported totals
// are estimates, rounded to whole batches per thread.
inline constexpr uint32_t kSampleEvery = 16;
inline constexpr bool Sampled(Path path) { return path == BpApply || path == PaletteScan || path == TextureBind; }
class Scope {
    using Clock = std::chrono::steady_clock;
    const Path path_;
    bool timed_ = false;
    Clock::time_point started_{};
public:
    explicit Scope(Path path) : path_(path) {
        if (!Enabled()) return;
        if (Sampled(path)) {
            thread_local std::array<uint32_t, Count> ticks{};
            if (++ticks[path] % kSampleEvery != 0) return;
        }
        timed_ = true;
        started_ = Clock::now();
    }
    ~Scope() {
        if (!timed_) return;
        const uint64_t weight = Sampled(path_) ? kSampleEvery : 1;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-started_).count();
        counters[path_].nanoseconds.fetch_add(uint64_t(elapsed) * weight, std::memory_order_relaxed);
        counters[path_].calls.fetch_add(weight, std::memory_order_relaxed);
    }
};
inline void Report(std::ostream& out) {
    constexpr std::array<const char*, Count> names{"bp_apply", "texture_bind", "palette_scan", "raw_draw", "color_copy", "iso_read"};
    out << "[prime-render-cpu] inclusive=1 sampled_hot_paths=1/" << kSampleEvery;
    for (unsigned i = 0; i < Count; ++i) {
        const auto calls = counters[i].calls.exchange(0, std::memory_order_relaxed);
        const auto ns = counters[i].nanoseconds.exchange(0, std::memory_order_relaxed);
        out << ' ' << names[i] << "_calls=" << calls << ' ' << names[i] << "_ms=" << double(ns)/1000000.0;
    }
    out << std::endl;
}
} // namespace PrimePerformance
