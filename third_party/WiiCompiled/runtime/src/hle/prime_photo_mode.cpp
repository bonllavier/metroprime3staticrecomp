// Photo mode for RM3E01 (hle/prime_photo_mode.h).
#if defined(MKW_GAME_PROFILE_RM3E01_00)
#include "hle/prime_photo_mode.h"

#include "memory.h"
#include "runtime_log.h"

#include <atomic>

namespace PrimePhotoMode {
namespace {
std::atomic<bool> g_photoActive{false};
std::atomic<int> g_photoSteps{0};

// The game's hang watchdog: a 1 s periodic alarm (0x802EB4E4) adds 1.0 to this float, and the
// update clears it every frame (0x802EBB74). At 10.0 the alarm calls rs_reallyfatal_printf
// ("INFINITE LOOP"): screen goes black and the game stops for good. A skipped update must clear it too.
constexpr uint32_t kWatchdogSeconds = 0x8067DC2Cu;

void FeedWatchdog() {
    if (Memory::Contains(kWatchdogSeconds, 4)) Memory::Write32(kWatchdogSeconds, 0u);  // 0.0f
}
}  // namespace

bool Active() { return g_photoActive.load(std::memory_order_relaxed); }

void SetActive(bool active) {
    if (g_photoActive.exchange(active) != active)
        RT_LOG(RT_TAG_RUNTIME) << "[photo-mode] " << (active ? "on: game update frozen" : "off") << std::endl;
    g_photoSteps.store(0);
}

void StepFrame() {
    if (Active()) g_photoSteps.fetch_add(1);
}

}  // namespace PrimePhotoMode

// Runtime hook at the entry of the game update 0x802EBAF4 (prepare_prime_runtime_hooks.py), game
// thread: true skips the update for this frame.
extern "C" bool PrimePhotoModeSkipUpdate() {
    using namespace PrimePhotoMode;
    if (!g_photoActive.load(std::memory_order_relaxed)) return false;
    int steps = g_photoSteps.load(std::memory_order_relaxed);
    while (steps > 0) {
        if (g_photoSteps.compare_exchange_weak(steps, steps - 1)) return false;  // one stepped frame
    }
    FeedWatchdog();
    return true;
}
#endif
