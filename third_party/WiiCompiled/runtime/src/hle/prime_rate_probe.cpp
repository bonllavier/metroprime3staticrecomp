// Private QA probe (test build only): which game functions run once per drawn frame rather than per logic
// step, and what time value they receive. The runtime overlay (tools/prepare_prime_runtime_hooks.py, list in
// config/RM3E01_00/qa_rate_probe.json) calls PrimeRateProbe at the entry of each listed function; every 5 s
// the counts go to the log as [rate-probe] lines, with the frame rate and logic step of that window, so the
// same screen at 60 and at native 120 (recomp/qa/try_native_120.ps1) can be compared: a function whose calls
// double while its time value stays the same is advanced per frame. See recomp/docs/fps_altos.md.
#include "guest_abi/cpu_context.h"
#include "runtime_log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#if defined(MKW_PRIME_QA_TOOLS) && defined(MKW_GAME_PROFILE_RM3E01_00)
namespace {
constexpr uint32_t kSlots = 1024;
struct Slot {
    uint32_t address = 0;
    uint64_t calls = 0;
    uint64_t floatCalls = 0;  // calls whose f1 looked like a time value (0 < f1 < 1)
    double sum = 0.0;
    float min = 0.0f;
    float max = 0.0f;
};
std::mutex g_mutex;
std::array<Slot, kSlots> g_slots{};
uint64_t g_retraces = 0;
auto g_windowStart = std::chrono::steady_clock::now();
}  // namespace

extern "C" void PrimeRateProbe(CpuContext* ctx, uint32_t index, uint32_t address) {
    // WII_COMPILED_QA_NO_RATE_PROBE=1: timing measurements without the probe's lock on every call.
    static const bool off = [] {
        const char* value = std::getenv("WII_COMPILED_QA_NO_RATE_PROBE");
        return value && value[0] == '1';
    }();
    if (off || index >= kSlots) return;
    const float f1 = static_cast<float>(ctx->fpr[1].d);
    std::lock_guard<std::mutex> lock(g_mutex);
    Slot& slot = g_slots[index];
    slot.address = address;
    ++slot.calls;
    if (f1 > 0.0f && f1 < 1.0f) {
        if (slot.floatCalls == 0 || f1 < slot.min) slot.min = f1;
        if (slot.floatCalls == 0 || f1 > slot.max) slot.max = f1;
        slot.sum += f1;
        ++slot.floatCalls;
    }
}

// Once per retrace, from the VI (game thread).
void PrimeRateProbeRetrace(float logicStep, int nativeFactor, double presentsPerSecond) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_retraces;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - g_windowStart).count();
    if (seconds < 5.0) return;
    RT_LOG(RT_TAG_RUNTIME) << "[rate-probe] window_s=" << seconds << " native_factor=" << nativeFactor
                           << " retraces_per_s=" << static_cast<double>(g_retraces) / seconds
                           << " presents_per_s=" << presentsPerSecond << " logic_step=" << logicStep << std::endl;
    for (Slot& slot : g_slots) {
        if (slot.calls == 0) continue;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "[rate-probe] fn=%08X calls_per_s=%.2f per_retrace=%.3f f1_calls=%llu f1_mean=%.6f f1_min=%.6f "
                      "f1_max=%.6f",
                      slot.address, static_cast<double>(slot.calls) / seconds,
                      static_cast<double>(slot.calls) / static_cast<double>(g_retraces ? g_retraces : 1),
                      static_cast<unsigned long long>(slot.floatCalls),
                      slot.floatCalls ? slot.sum / static_cast<double>(slot.floatCalls) : 0.0,
                      static_cast<double>(slot.min), static_cast<double>(slot.max));
        RT_LOG(RT_TAG_RUNTIME) << line << std::endl;
        const uint32_t address = slot.address;
        slot = Slot{};
        slot.address = address;
    }
    g_retraces = 0;
    g_windowStart = now;
}
#else
extern "C" void PrimeRateProbe(CpuContext*, uint32_t, uint32_t) {}
void PrimeRateProbeRetrace(float, int, double) {}
#endif
