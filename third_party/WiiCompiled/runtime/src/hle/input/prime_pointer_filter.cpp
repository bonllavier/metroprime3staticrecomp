// RM3E01 smooths the pointer before any consumer sees it: the input object
// (gpController 0x8067DC18) keeps two filter objects per channel (+0xC54 x,
// +0xC68 y; class vtable 0x8059C1E0, constructor 0x8036B57C with type 2) that
// 0x803C2A74 runs on every sample. The type-2 step (0x8036BA70) is an adaptive
// exponential smoothing: alpha = |x - mean of the last 10 samples| / 0.25,
// capped at 0.3, so a slow precise motion is followed with alpha near 0 (tens
// of frames of lag) and a fast one with a 3-frame time constant. Worth it for a
// jittery IR camera, pure delay for a mouse. With the keyboard/mouse mode and
// the raw-pointer option, the filter's virtual step (0x8036BC98, post-hook)
// returns its input unchanged for those two objects only; the pitch filter and
// every other instance keep smoothing.
#include "guest_abi/cpu_context.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <atomic>
#include <cstdlib>
#include <string_view>

namespace {
constexpr uint32_t kController = 0x8067DC18u;
constexpr uint32_t kPointerFilterX = 0xC54u, kPointerFilterY = 0xC68u;

bool BypassEnabled() {
    static const bool enabled = [] {
        if (!RuntimeConfigFile::PrimeKeyboardMouseMode()) return false;
        return RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primeRawPointer, "WII_COMPILED_PRIME_RAW_POINTER", true);
    }();
    return enabled;
}
}  // namespace

extern "C" void PrimePointerFilterAfter(CpuContext* ctx, uint32_t self, double input) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    if (!BypassEnabled() || !Memory::Contains(kController, 4)) return;
    const uint32_t controller = Memory::Read32(kController);
    if (!Memory::Contains(controller + kPointerFilterY, 4)) return;
    if (self != Memory::Read32(controller + kPointerFilterX) && self != Memory::Read32(controller + kPointerFilterY)) return;
    ctx->fpr[1].d = input;
    static std::atomic<unsigned> traces{0};
    if (traces.fetch_add(1) < 2) RT_LOG(RT_TAG_RUNTIME) << "[prime-raw-pointer] bypassing the game's pointer smoothing" << std::endl;
#else
    (void)ctx; (void)self; (void)input;
#endif
}
