#include "hle_stubs.h"
#include "memory.h"
#include "abi_bridge.h"
#include "guest_interrupt_context.h"
#include "ppc_runtime.h"
#include "aurora_events.h"
#include "settings_overlay.h"
#include "fiber_manager.h"
#include "platform/host_platform.h"
#include "runtime_log.h"
#include "hle/prime_xfb_ready.h"
#include "hle/prime_performance.h"
#include "hle/prime_fov.h"
#include "hle/prime_achievements.h"
#include "hle/gx_gpu_thread.h"
#include "hle/prime_input_latency.h"

#include <dolphin/vi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <aurora/aurora.h>
#include <aurora/gfx.h>

// Forward declaration for OSWakeupThread - used to wake threads on VI retrace queue
extern "C" void OSWakeupThread_HLE_801aaaa4(CpuContext* ctx);
extern "C" void OS_HLE_DispatchPendingPrimeIpcInterrupt();
extern "C" bool PrimeGpuCpInterruptPending();
#if defined(MKW_GAME_PROFILE_RM3E01_00)
extern "C" void PrimeInputServiceEvents();
#endif
extern "C" bool GXSelectTrackedDisplayCopy(const void*);
void* GuestToHostPtr(uint32_t, size_t);

static void DispatchPrimeCpInterrupt(CpuContext* interrupted);
// Call-boundary entry (OS_HLE_PollPendingPrimeIpcInterrupt): a guest loop that
// waits on a PE token behind the breakpoint never reaches the VI/idle polls.
extern "C" void VI_HLE_DispatchPrimeCpInterrupt(CpuContext* interrupted) { DispatchPrimeCpInterrupt(interrupted); }

static void DispatchPrimeCpInterrupt(CpuContext* interrupted) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    static bool dispatching = false;
    if (dispatching || !interrupted || (interrupted->msr & 0x8000u) == 0 ||
        !PrimeGpuCpInterruptPending() || (Memory::Read32(0x800000C4u) & 0x4000u) != 0) return;
    const uint32_t table = Memory::Read32(0x8067E4D0u);
    const uint32_t handler = table ? Memory::Read32(table + 0x11u * 4u) : 0;
    if (!handler) return;
    uint32_t osContext = Memory::Read32(0x800000E4u);
    if (!osContext || !Memory::Contains(osContext, 0x2C8u)) osContext = Memory::Read32(0x800000D4u);
    if (!osContext || !Memory::Contains(osContext, 0x2C8u)) osContext = 0x8060B830u;
    CpuContext irq = *interrupted;
    irq.msr &= ~0x8000u;
    irq.gpr[3] = 0x11u; irq.gpr[4] = osContext;
    CpuContextScope scope(&irq);
    dispatching = true;
    try { InvokeIndirectCpu(handler, &irq); }
    catch (...) { dispatching = false; throw; }
    dispatching = false;
    static unsigned traceCount = 0;
    if (traceCount++ < 16)
        RT_LOG(RT_TAG_GX) << "[prime-cp-breakpoint-isr] handler=0x" << std::hex << handler
            << " callback=0x" << Memory::Read32(0x8067E3C4u)
            << " frame_state=" << Memory::Read32(0x8067DECCu) << std::dec << std::endl;
#endif
}

// Forward declaration for OSSleepThread - used by VIWaitForRetrace HLE
extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* cpu);
extern "C" int g_gxFrameCount;
extern "C" int32_t OS__DisableInterrupts_801a65ac();
extern "C" int32_t OS__RestoreInterrupts_801a65d4(int32_t level);

// Aurora frame cycle tracking - needs external linkage for the GX HLE
// (declared in gx_internal.h, consumed by gx_frame.cpp). We need to call
// aurora_begin_frame() before GX commands and aurora_end_frame() after.
std::atomic_bool g_auroraFrameActive{false};
std::atomic_bool g_auroraFrameHadWork{false};

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kGuestGxRenderModeCopyBytes = 0x39;

bool ReadGuestRenderModeObj(uint32_t guestPtr, GXRenderModeObj& out) {
    if (guestPtr == 0 || !Memory::Contains(guestPtr, kGuestGxRenderModeCopyBytes)) {
        return false;
    }

    try {
        out.viTVmode = static_cast<VITVMode>(Memory::Read32(guestPtr + 0x00));
        out.fbWidth = Memory::Read16(guestPtr + 0x04);
        out.efbHeight = Memory::Read16(guestPtr + 0x06);
        out.xfbHeight = Memory::Read16(guestPtr + 0x08);
        out.viXOrigin = Memory::Read16(guestPtr + 0x0a);
        out.viYOrigin = Memory::Read16(guestPtr + 0x0c);
        out.viWidth = Memory::Read16(guestPtr + 0x0e);
        out.viHeight = Memory::Read16(guestPtr + 0x10);
        out.xFBmode = static_cast<VIXFBMode>(Memory::Read32(guestPtr + 0x14));
        out.field_rendering = Memory::Read8(guestPtr + 0x18);
        out.aa = Memory::Read8(guestPtr + 0x19);

        for (size_t i = 0; i < 12; ++i) {
            out.sample_pattern[i][0] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2));
            out.sample_pattern[i][1] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2) + 1);
        }
        for (size_t i = 0; i < 7; ++i) {
            out.vfilter[i] = Memory::Read8(guestPtr + 0x32 + static_cast<uint32_t>(i));
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }

    return true;
}

struct ViState {
    bool initialized = false;
    uint32_t tvFormat = 0; // VIGetTvFormat returns VI_NTSC before VIInit
    uint32_t nextFrameBuffer = 0;
    uint32_t currentFrameBuffer = 0;
    uint32_t retraceCount = 0;
    bool black = false;
    uint32_t preRetraceCallback = 0;
    uint32_t postRetraceCallback = 0;
    uint32_t renderWidth = 640;
    uint32_t renderHeight = 480;
    uint32_t viXOrigin = 0;
    uint32_t viYOrigin = 0;
    uint32_t xfbWidth = 640;
    uint32_t xfbHeight = 480;
    bool fieldOdd = false;
    Clock::time_point lastRetrace = Clock::now();
    std::chrono::microseconds retraceInterval{16666us}; // ~60 Hz
    bool hasValidXfb = false; // True once we've received at least one GXCopyDisp
    uint32_t readyXfb = 0;    // XFB address from the most recent GXCopyDisp
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    PrimeXfbReady::Tracker primeReadyXfbs;
    uint32_t primeSelectedXfb = 0;
    uint64_t primeSelectedRevision = 0;
#endif

    // VIConfigure/VISetNextFrameBuffer/VISetBlack only write pending values below; VIFlush arms them but
    // the commit happens at the next retrace, matching real VI hardware. A VIFlush called from a
    // pre-retrace callback therefore misses the imminent field and lands one field late.

    // Pending values (written by VISetNextFrameBuffer, VISetBlack, VIConfigure)
    uint32_t pendingNextFrameBuffer = 0;
    bool pendingBlack = false;
    // Matches the active-state default (NTSC before VIInit) and the pending
    // 16666us interval below; a flush before any VIConfigure must not commit
    // PAL timing onto an NTSC-interval state.
    uint32_t pendingTvFormat = 0;
    uint32_t pendingRenderWidth = 640;
    uint32_t pendingRenderHeight = 480;
    uint32_t pendingViXOrigin = 0;
    uint32_t pendingViYOrigin = 0;
    uint32_t pendingXfbWidth = 640;
    uint32_t pendingXfbHeight = 480;
    std::chrono::microseconds pendingRetraceInterval{16666us};
    
    // Set by VIFlush(); cleared after commit in AdvanceRetrace
    bool flushArmed = false;
};

std::mutex g_viMutex;
ViState g_vi;


// Guest-side state addresses used by the SDK's VI globals.
constexpr uint32_t kViInitializedFlagAddr   = 0x80386b38;
constexpr uint32_t kViTvFormatAddr          = 0x80386ba8;
constexpr uint32_t kViRenderWidthAddr       = 0x80350864;
constexpr uint32_t kViRenderHeightAddr      = 0x80350866;
constexpr uint32_t kViXfbWidthAddr          = 0x80350872;
constexpr uint32_t kViXfbHeightAddr         = 0x8035087c;
constexpr uint32_t kViRetraceCountAddr      = 0x80386be4; // matches VIWaitForRetrace/handler
constexpr uint32_t kViTimingGuardAddr       = 0x80386b44;
constexpr uint32_t kViPreRetraceCallback    = 0x80386bb8;
constexpr uint32_t kViPostRetraceCallback   = 0x80386bb4;
constexpr uint32_t kViNextFrameBufferAddr   = 0x80386ba0;
constexpr uint32_t kViNextFrameBufferHwAddr = 0x80350890;
constexpr uint32_t kViRetraceQueueAddr      = 0x80386bc0; // Thread queue for VIWaitForRetrace

// EGG::BaseSystem::sSystem pointer - must be non-null before post-retrace callback is valid
constexpr uint32_t kEggSSystemAddr = 0x80386F60;

std::chrono::microseconds IntervalForFormat(uint32_t tvFormat) {
    // NTSC-ish defaults to 60 Hz; PAL uses 50 Hz.
    return tvFormat == 1 ? 20000us : 16666us;
}

// Shared busy-wait budget for deadline-precise sleeps (matches Aurora's
// presenter spin window). Larger windows burn a core for no visible gain.
constexpr std::chrono::microseconds kFinalSpinWindow{500};

void SleepPreciselyUntil(Clock::time_point deadline, bool finishWithSpin = false,
                         std::chrono::microseconds spinWindow = 750us) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return;
    }
    const auto timerDeadline =
        finishWithSpin && deadline - now > spinWindow ? deadline - spinWindow : deadline;
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    struct HighResolutionTimer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_MODIFY_STATE | SYNCHRONIZE);
        ~HighResolutionTimer() {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    };
    static thread_local HighResolutionTimer timer;
    if (timer.handle != nullptr) {
        const auto remaining100ns =
            std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(timerDeadline - now);
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<int64_t>(remaining100ns.count(), 1);
        if (SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
            WaitForSingleObject(timer.handle, INFINITE);
            if (!finishWithSpin) {
                return;
            }
        }
    }
#endif
    if (!finishWithSpin) {
        std::this_thread::sleep_until(deadline);
        return;
    }
    if (Clock::now() < timerDeadline) {
        std::this_thread::sleep_until(timerDeadline);
    }
    // This runs only for the final fraction of a VI interval. Do not yield the
    // host thread here: a scheduler quantum is larger than the remaining
    // budget and would recreate the 17-18 ms sawtooth this path removes.
    while (Clock::now() < deadline) {
#if defined(_WIN32)
        YieldProcessor();
#endif
    }
}

void ViSetR3(CpuContext* ctx, uint32_t value)
{
    if (ctx) {
        ctx->gpr[3] = value;
    }
}

void WriteGuestStateLocked() {
    try {
        Memory::Write8(kViInitializedFlagAddr, 1);
        Memory::Write8(kViTimingGuardAddr, 1);
        Memory::Write32(kViTvFormatAddr, g_vi.tvFormat);
        Memory::Write16(kViRenderWidthAddr, static_cast<uint16_t>(g_vi.renderWidth));
        Memory::Write16(kViRenderHeightAddr, static_cast<uint16_t>(g_vi.renderHeight));
        Memory::Write16(kViXfbWidthAddr, static_cast<uint16_t>(g_vi.xfbWidth));
        Memory::Write16(kViXfbHeightAddr, static_cast<uint16_t>(g_vi.xfbHeight));
        Memory::Write32(kViRetraceCountAddr, g_vi.retraceCount);
        Memory::Write32(kViPreRetraceCallback, g_vi.preRetraceCallback);
        Memory::Write32(kViPostRetraceCallback, g_vi.postRetraceCallback);
        // Write PENDING frame buffer to guest memory so SDK code sees the queued value
        Memory::Write32(kViNextFrameBufferAddr, g_vi.pendingNextFrameBuffer);
        Memory::Write32(kViNextFrameBufferHwAddr, g_vi.pendingNextFrameBuffer);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_VI, "WriteGuestStateLocked", e);
    }
}

void EnsureInitializedLocked() {
    if (g_vi.initialized) {
        return;
    }
    g_vi.initialized = true;
    g_vi.retraceInterval = IntervalForFormat(g_vi.tvFormat);
    g_vi.lastRetrace = Clock::now();
    WriteGuestStateLocked();
}

// GXRenderModeObj::viTVmode encodes the output family (0 NTSC, 1 PAL, 2 MPAL,
// 5 EURGB60) in bits [4:2].
uint32_t ExtractTvFormat(uint32_t tvMode) {
    return (tvMode >> 2) & 0x7;
}

// Re-entry guard to prevent AdvanceRetrace calling itself via OSWakeupThread -> SelectThread
static std::atomic<bool> s_inAdvanceRetrace{false};

// Set while VI_HLE_PresentFrame runs its seal/pace/pre-warm sequence. Guest callbacks
// serviced during that window (pace-loop alarms, GX timing polls) still see the stale
// hasValidXfb/g_auroraFrameActive flags; a retrace-context present fired from them would
// end the freshly pre-warmed empty frame and show it as a black frame group.
static std::atomic<bool> s_presentSequenceActive{false};

void AdvanceRetrace(CpuContext* ctx, Clock::time_point retraceStamp, bool serviceAurora) {
    // Prevent re-entry - this can happen if OSWakeupThread triggers SelectThread
    // which goes idle and calls ProcessTimerEvents again
    if (s_inAdvanceRetrace.exchange(true)) {
        return;
    }

#if defined(MKW_GAME_PROFILE_RM3E01_00)
    // RetroAchievements: rc_client evaluates its conditions once per frame
    // (hle/prime_achievements.h); first, so the FOV option sees its block.
    PrimeAchievements::Frame();
    // First-person FOV option: once per retrace on the game thread, as PrimeHack
    // rewrites it every frame (hle/prime_fov.h).
    PrimeFov::Frame();
    // Observation only: compare delivered VI events with wall time, including
    // late catch-up bursts and accidental future delivery. Do not change either
    // the guest clock or the retrace schedule to hide slow rendering.
    if (PrimePerformance::Enabled()) {
        static auto windowStart = Clock::now();
        static uint64_t delivered = 0;
        static double maxLateMs = 0, maxEarlyMs = 0;
        const auto now = Clock::now();
        const double lateMs = std::chrono::duration<double, std::milli>(now - retraceStamp).count();
        maxLateMs = std::max(maxLateMs, lateMs);
        maxEarlyMs = std::max(maxEarlyMs, -lateMs);
        ++delivered;
        const double seconds = std::chrono::duration<double>(now - windowStart).count();
        if (seconds >= 5.0) {
            RT_LOG(RT_TAG_VI) << "[prime-vi-cadence] wall_seconds=" << seconds
                << " delivered=" << delivered << " delivered_hz=" << double(delivered) / seconds
                << " max_late_ms=" << maxLateMs << " max_early_ms=" << maxEarlyMs << std::endl;
#if defined(MKW_PRIME_QA_TOOLS)
            {
                AuroraFrameInterpolationDiagnostics interp{};
                aurora_get_frame_interpolation_diagnostics(&interp);
                AuroraPresentTiming present{};
                aurora_get_present_timing(&present);
                RT_LOG(RT_TAG_VI) << "[prime-interp] target=" << interp.targetFps
                    << " slots=" << interp.activeSamples << "/" << interp.targetSamples
                    << " candidates=" << interp.candidates << " matchable=" << interp.matchable
                    << " matches=" << interp.matches << " replay_safe=" << interp.replaySafe
                    << " sealed=" << interp.framesSealed << " low_match=" << interp.framesLowMatch
                    << " unsafe=" << interp.framesReplayUnsafe << " reductions=" << interp.slotReductions
                    << " late_drops=" << interp.lateSealDrops
                    << " with_copy=" << interp.sealsWithBoundary << " multi_copy=" << interp.multiBoundary
                    << " carried=" << interp.sealsCarried << " complete=" << interp.sealsComplete
                    << " carried_draws=" << interp.carriedDraws << " replayed=" << interp.sealsReplayed
                    << " cuts=" << interp.cameraCuts << " borrowed=" << interp.borrowedDraws << " probe_restores=" << interp.probeRestores
                    << " camera_estimates=" << interp.cameraEstimates
                    << " presents_per_s=" << present.framesPerSecond
                    << " effective_per_s=" << present.effectiveFramesPerSecond << std::endl;
            }
#endif
            windowStart = now;
            delivered = 0;
            maxLateMs = maxEarlyMs = 0;
        }
    }
#endif
    
    uint32_t preCb = 0;
    uint32_t postCb = 0;
    uint32_t retraceValue = 0;
    bool hasXfbReady = false;
    uint32_t readyXfb = 0;
    uint32_t currentFb = 0;
    bool isBlack = false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        
        // Commit pending state if VIFlush armed it (see ViState).
        if (g_vi.flushArmed) {
            // Commit pending -> active
            g_vi.nextFrameBuffer = g_vi.pendingNextFrameBuffer;
            g_vi.black = g_vi.pendingBlack;
            g_vi.tvFormat = g_vi.pendingTvFormat;
            g_vi.renderWidth = g_vi.pendingRenderWidth;
            g_vi.renderHeight = g_vi.pendingRenderHeight;
            g_vi.viXOrigin = g_vi.pendingViXOrigin;
            g_vi.viYOrigin = g_vi.pendingViYOrigin;
            g_vi.xfbWidth = g_vi.pendingXfbWidth;
            g_vi.xfbHeight = g_vi.pendingXfbHeight;
            g_vi.retraceInterval = g_vi.pendingRetraceInterval;
            
            // Clear flush armed flag
            g_vi.flushArmed = false;
            
        }
        
        g_vi.retraceCount++;
        g_vi.fieldOdd = !g_vi.fieldOdd;
        g_vi.currentFrameBuffer = g_vi.nextFrameBuffer;
        currentFb = g_vi.currentFrameBuffer;
        g_vi.lastRetrace = retraceStamp;
        retraceValue = g_vi.retraceCount;
        preCb = g_vi.preRetraceCallback;
        postCb = g_vi.postRetraceCallback;
        hasXfbReady = g_vi.hasValidXfb;
        readyXfb = g_vi.readyXfb;
        isBlack = g_vi.black;
        WriteGuestStateLocked();
    }

#if defined(MKW_GAME_PROFILE_RM3E01_00)
    // Prime 3 installs its hardware retrace ISR in a private table at
    // 0x8067E4D0 via 0x804E3D08(0x18, handler). Deliver that guest ISR when the
    // guest external-interrupt bit and VI unmask bit permit it. The ordinary
    // HLE retrace callbacks below do not replace this per-title ISR.
    if (ctx != nullptr && (ctx->msr & 0x00008000u) != 0) {
        constexpr uint32_t kPrimeViInterrupt = 0x18u;
        constexpr uint32_t kPrimeViInterruptMask = 0x80u;
        constexpr uint32_t kPrimeViHandlerTablePointer = 0x8067E4D0u;
        const uint32_t enabledMask = Memory::Read32(0x800000C4u);
        const uint32_t handlerTable = Memory::Read32(kPrimeViHandlerTablePointer);
        const uint32_t handler = handlerTable == 0 ? 0 :
            Memory::Read32(handlerTable + kPrimeViInterrupt * 4u);
        if ((enabledMask & kPrimeViInterruptMask) == 0 && handler != 0) {
            CpuContext callbackContext = *ctx;
            callbackContext.msr &= ~0x00008000u; // External exception entry masks EE.
            CpuContextScope callbackScope(&callbackContext);
            uint32_t interruptedOsContext = Memory::Read32(0x800000E4u);
            if (interruptedOsContext == 0 || !Memory::Contains(interruptedOsContext, 0x2C8u)) {
                interruptedOsContext = Memory::Read32(0x800000D4u);
            }
            // Prime's scheduler at 0x804E6BD4 enters its own idle loop using
            // the OSContext at 0x8060B830 when no thread is running.
            if (interruptedOsContext == 0 || !Memory::Contains(interruptedOsContext, 0x2C8u)) {
                interruptedOsContext = 0x8060B830u;
            }
            static uint32_t primeViContextTraceCount = 0;
            if (primeViContextTraceCount < 32u) {
                std::fprintf(stderr, "[prime-vi-context] n=%u prior_r4=%08X current=%08X running=%08X selected=%08X valid=%u\n",
                             ++primeViContextTraceCount, callbackContext.gpr[4],
                             Memory::Read32(0x800000D4u), Memory::Read32(0x800000E4u),
                             interruptedOsContext,
                             interruptedOsContext != 0 && Memory::Contains(interruptedOsContext, 0x2C8u) ? 1u : 0u);
            }
            if (interruptedOsContext != 0 && Memory::Contains(interruptedOsContext, 0x2C8u)) {
                // The RM3E01_00 VI ISR uses r4 as the interrupted OSContext:
                // after its private save frame, it restores this pointer with
                // FUN_804E00DC. The host callback context's r4 is just the
                // interrupted guest's fourth register and is not that pointer.
                const uint32_t runningBefore = Memory::Read32(0x800000E4u);
                const uint32_t currentBefore = Memory::Read32(0x800000D4u);
                const uint32_t savedLr = callbackContext.lr;
                const uint32_t savedR1 = callbackContext.gpr[1];
                callbackContext.gpr[3] = kPrimeViInterrupt;
                callbackContext.gpr[4] = interruptedOsContext;
                InvokeIndirectCpu(handler, &callbackContext);
                static uint32_t primeViSwitchTraceCount = 0;
                if (primeViSwitchTraceCount < 32u) {
                    const uint32_t runningAfter = Memory::Read32(0x800000E4u);
                    if (runningAfter != runningBefore) {
                        std::fprintf(stderr,
                                     "[prime-vi-switch] n=%u before=%08X/%08X after=%08X/%08X callback_lr=%08X->%08X callback_r1=%08X->%08X callback_pc=%08X srr0=%08X\n",
                                     ++primeViSwitchTraceCount, runningBefore, currentBefore,
                                     runningAfter, Memory::Read32(0x800000D4u), savedLr,
                                     callbackContext.lr, savedR1, callbackContext.gpr[1],
                                     callbackContext.pc, callbackContext.srr0);
                    }
                }
            }
        }
    }
#endif

#if defined(MKW_GAME_PROFILE_RM3E01_00)
    // The original VI ISR commits the game's own VIFlush shadow and publishes
    // its active XFB at 8067E710. Do not infer the displayed XFB from the most
    // recent CPU copy or mutate the original driver's globals.
    currentFb = Memory::Read32(0x8067E710u);
    isBlack = (Memory::Read16(0xCC002002u) & 1u) == 0 ||
        (Memory::Read16(0xCC002000u) & 0x3FF0u) == 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        const uint64_t revision = g_vi.primeReadyXfbs.Revision(currentFb);
        hasXfbReady = revision != 0;
        readyXfb = hasXfbReady ? currentFb : 0;
    }
    // Scanning an old XFB again is not a new EFB copy. Sealing an unfinished
    // producer frame at every retrace can split its draw/copy sequence.
    hasXfbReady = hasXfbReady && currentFb != 0 && !isBlack;
#endif

    // Wake WiiCompiled's own VIWaitForRetrace queue. Prime has a different
    // guest queue layout and its translated VI ISR owns game-thread wakeups.
#if !defined(MKW_GAME_PROFILE_RM3E01_00)
    if (ctx) {
        ctx->gpr[3] = kViRetraceQueueAddr;
        OSWakeupThread_HLE_801aaaa4(ctx);
    }
#endif

    if (serviceAurora) {
        // Window events touch neither GX nor decoder state, so with a frame
        // already begun they need only the HostLock: draining the decoder here
        // on every retrace kept the game thread waiting ~3 % of its time (F8
        // 2026-10-31 00:07). Beginning a frame mutates renderer state and keeps
        // the full Lock.
        if (g_auroraFrameActive.load(std::memory_order_acquire)) {
            GxGpuThread::HostLock hostLock;
            // Process window events (but don't present - that happens in GXCopyDisp)
            UpdateAuroraAndProcessEvents();
        } else {
            GxGpuThread::Lock gpuLock;
            UpdateAuroraAndProcessEvents();
            // Start a new Aurora frame if one isn't already active
            if (!g_auroraFrameActive.load(std::memory_order_acquire)) {
                if (BeginAuroraFrame()) {
                    g_auroraFrameActive.store(true, std::memory_order_release);
                }
            }
        }
    }

    if (ctx) {
        ctx->gpr[3] = retraceValue;
        if (preCb) {
            InvokeIndirectCpu(preCb, ctx);
        }
        if (postCb) {
            // Guard: only invoke callback if sSystem is initialized
            // The callback dereferences sSystem which must be non-null
            uint32_t sSystemPtr = Memory::Read32(kEggSSystemAddr);
            if (sSystemPtr != 0) {
                InvokeIndirectCpu(postCb, ctx);
            }
        }
    }

    // VISetBlack(TRUE) keeps frame submission running but shows only the clear color: GX render work is
    // skipped and Aurora's end_frame() clears to black, matching the hardware manual's "signal continues,
    // pixels go black" behavior. When not black, submission waits for hasXfbReady (GXCopyDisp done).
    // Only a retrace that may submit needs the full Lock (the decoder drained
    // for GXSelectTrackedDisplayCopy and the present). The rest only pumps
    // window events. Every input below except the GX selection is VI state
    // this thread already copied.
    const bool mayPresentXfb = hasXfbReady && !isBlack && readyXfb != 0 && readyXfb == currentFb;
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    const bool mayPresentBlack = false;
#else
    const bool mayPresentBlack = isBlack;
#endif
    if (serviceAurora && !s_presentSequenceActive.load(std::memory_order_acquire) &&
        !(g_auroraFrameActive.load(std::memory_order_acquire) && (mayPresentXfb || mayPresentBlack))) {
        if (g_auroraFrameHadWork.load(std::memory_order_acquire) && !mayPresentXfb && !isBlack) {
            // GX work is in progress but frame not complete - just poll window events
            GxGpuThread::HostLock hostLock;
            UpdateAuroraAndProcessEvents();
        }
    } else if (serviceAurora && !s_presentSequenceActive.load(std::memory_order_acquire)) {
        GxGpuThread::Lock gpuLock;
        const bool frameActive = g_auroraFrameActive.load(std::memory_order_acquire);
        const bool xfbMatches = (readyXfb != 0 && readyXfb == currentFb);
        bool shouldPresentXfb = hasXfbReady && !isBlack && xfbMatches;
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        // Deferred retraces during a renderer wait must not mutate its source.
        // Select the original VI driver's destination only for a real submit.
        if (frameActive && shouldPresentXfb) {
            shouldPresentXfb = GXSelectTrackedDisplayCopy(GuestToHostPtr(currentFb, 0));
            if (shouldPresentXfb) {
                std::lock_guard<std::mutex> lock(g_viMutex);
                g_vi.primeSelectedXfb = currentFb;
                g_vi.primeSelectedRevision = g_vi.primeReadyXfbs.Revision(currentFb);
                static unsigned presentTraceCount = 0;
                if (presentTraceCount++ < 24)
                    RT_LOG(RT_TAG_GX) << "[prime-vi-xfb-selected] framebuffer=0x" << std::hex << currentFb
                        << std::dec << " retrace=" << retraceValue << std::endl;
            }
        }
#endif
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        // Initial black remains unpresented until an active guest XFB exists.
        // Later black transitions still need a dedicated blank presentation.
        const bool shouldPresentBlack = false;
#else
        const bool shouldPresentBlack = isBlack && frameActive;
#endif
        const bool shouldSubmit = frameActive && (shouldPresentXfb || shouldPresentBlack);

        if (shouldSubmit) {
            if (!isBlack || settings_overlay::StartupScreenVisible()) {
                // Normal presentation: draw overlay on top of GX content
                settings_overlay::Draw();
            }
            // Outside startup, VI black remains a pure black presentation.
            // Unpaced: this present already runs in retrace context.
            if (shouldPresentXfb) PrimeInputLatency::NoteHandoff();
            VI_HLE_PresentFrame(shouldPresentXfb, false);
        } else if (g_auroraFrameHadWork.load(std::memory_order_acquire) && !shouldPresentXfb && !isBlack) {
            // GX work is in progress but frame not complete - just poll window events
            // Don't call aurora_end_frame() as that would present incomplete work
            UpdateAuroraAndProcessEvents();
        }
    }

    // Clear re-entry guard
    s_inAdvanceRetrace.store(false);
}

bool AdvanceDueRetraces(CpuContext* ctx, int maxToProcess, bool serviceAurora)
{
    bool advancedAny = false;

    for (int catchUpCount = 0; catchUpCount < maxToProcess; ++catchUpCount) {
        Clock::time_point target;
        auto now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            if (!g_vi.initialized) {
                return advancedAny;
            }
            target = g_vi.lastRetrace + g_vi.retraceInterval;
            if (now < target) {
                return advancedAny;
            }
        }


        AdvanceRetrace(ctx, target, serviceAurora);
        advancedAny = true;
    }

    return advancedAny;
}

} // namespace

// Force one retrace boundary to pass, whether or not its wall-clock deadline
// has arrived, so the guest's retrace callbacks (AsyncDisplay's counters and
// friends) run. VI_HLE_PollRetrace below is the time-driven counterpart.
void VI_HLE_ForceRetrace(CpuContext* ctx) {
    Clock::time_point target;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        target = g_vi.lastRetrace + g_vi.retraceInterval;
    }
    AdvanceRetrace(ctx, target, true);
}

bool VI_HLE_IsAdvancingRetrace() {
    return s_inAdvanceRetrace.load(std::memory_order_acquire);
}

// Advance every retrace whose interval has already elapsed. Safe to call from
// busy loops (GX drawing, the scheduler's idle spin) to keep VBlank ticking.
void VI_HLE_PollRetrace(CpuContext* ctx) {
    // The translated Prime IOS queue raises Y2 when IOS accepts an async
    // request, separately from Y1 when its callback completes. Service that
    // pending PPC interrupt at the runtime's existing retrace pump when the
    // guest context has external interrupts enabled.
    if (ctx != nullptr && (ctx->msr & 0x00008000u) != 0) {
        CpuContextScope interruptScope(ctx);
        OS_HLE_DispatchPendingPrimeIpcInterrupt();
        DispatchPrimeCpInterrupt(ctx);
    }
    AdvanceDueRetraces(ctx, 8, true);
}

// RM3E01's translated OS scheduler has an interruptible busy-wait path of its
// own instead of entering WiiCompiled's MKW scheduler idle loop. Poll the host
// VI clock from that path using an isolated guest interrupt context. The caller
// checks the guest MSR.EE bit before reaching this helper, so it remains faithful
// to the guest's interrupt mask while still delivering the registered VI ISR.
namespace { void PresentPrimeCopyEarly(); }

extern "C" void VI_HLE_PollRetraceGuestSpin(CpuContext* interruptedCtx) {
    static std::atomic<uint32_t> diagnosticCalls{0};
    static std::atomic<uint32_t> eligibleCalls{0};
    static std::atomic<int64_t> lastPollNanos{0};
    const uint32_t call = diagnosticCalls.fetch_add(1, std::memory_order_relaxed);
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    if (interruptedCtx && PrimeGpuCpInterruptPending()) {
        static uint32_t previousMsr = ~0u, previousMask = ~0u, gateTraces = 0;
        const uint32_t mask = Memory::Read32(0x800000C4u);
        if (gateTraces < 32 && (interruptedCtx->msr != previousMsr || mask != previousMask)) {
            previousMsr = interruptedCtx->msr; previousMask = mask; ++gateTraces;
            RT_LOG(RT_TAG_GX) << "[prime-cp-poll-gate] msr=0x" << std::hex << previousMsr
                << " os_mask=0x" << mask << " lr=0x" << interruptedCtx->lr
                << " sp=0x" << interruptedCtx->gpr[1] << std::dec << std::endl;
        }
    }
#endif
    if (interruptedCtx == nullptr || (interruptedCtx->msr & 0x00008000u) == 0) {
        return;
    }
    // Not rate-limited: a finished frame should reach the screen as soon as
    // the guest reaches a safe point, not at the next 1 ms poll.
    PresentPrimeCopyEarly();

    // OSInit can enter its idle loop before Prime's VI driver is installed.
    // Do not start the title's VI clock until its private ISR slot is populated
    // and the game has unmasked the corresponding interrupt.
    constexpr uint32_t kPrimeViHandlerTablePointer = 0x8067E4D0u;
    constexpr uint32_t kPrimeViMask = 0x80u;
    const uint32_t handlerTable = Memory::Read32(kPrimeViHandlerTablePointer);
    const uint32_t handler = handlerTable == 0 ? 0 : Memory::Read32(handlerTable + 0x18u * 4u);
    const uint32_t mask = Memory::Read32(0x800000C4u);
    if (call < 8) {
        std::fprintf(stderr, "[prime-vi-gate] call=%u msr=%08X table=%08X handler=%08X mask=%08X\n",
                     call, interruptedCtx->msr, handlerTable, handler, mask);
    }
    if (handler == 0 ||
        (mask & kPrimeViMask) != 0) {
        return;
    }

    const int64_t nowNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 Clock::now().time_since_epoch())
                                 .count();
    int64_t last = lastPollNanos.load(std::memory_order_relaxed);
    if (nowNanos - last < 1'000'000 ||
        !lastPollNanos.compare_exchange_strong(last, nowNanos, std::memory_order_relaxed)) {
        return;
    }

    CpuContext interruptRegisters = *interruptedCtx;
    CpuContextScope interruptScope(&interruptRegisters);
    // PPCCTRL's Y2 acknowledgement can be raised while Prime has EE masked.
    // The PPC services that external interrupt only after an EE-enabled safe
    // point; this guest wait spin is such a boundary. An accepted asynchronous
    // OH1 read must not strand later control transfers while its endpoint waits
    // for an HCI event.
    OS_HLE_DispatchPendingPrimeIpcInterrupt();
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    PrimeInputServiceEvents();
#endif
    DispatchPrimeCpInterrupt(&interruptRegisters);
    if (call < 4) {
        std::fprintf(stderr, "[prime-vi-spin] call=%u msr=%08X retrace=%u handler=%08X mask=%08X\n", call,
                     interruptedCtx->msr, Memory::Read32(0x8067E754u),
                     handler, Memory::Read32(0x800000C4u));
    }
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
    }
    const uint32_t eligible = eligibleCalls.fetch_add(1, std::memory_order_relaxed);
    if (eligible < 12) {
        std::lock_guard<std::mutex> lock(g_viMutex);
        const auto next = g_vi.lastRetrace + g_vi.retraceInterval;
        std::fprintf(stderr, "[prime-vi-clock] eligible=%u retrace=%u initialized=%u due=%u\n",
                     eligible, g_vi.retraceCount, g_vi.initialized ? 1u : 0u,
                     Clock::now() >= next ? 1u : 0u);
    }
    OS_HLE_BeginDeferredGuestCallbacks();
    try {
        const bool advanced = AdvanceDueRetraces(&interruptRegisters, 1, true);
        // Prime uses this translated scheduler spin instead of MKW's host idle
        // pump. Advance AID even when rendering never enters a host frame wait;
        // otherwise SDL starves despite a valid, nonzero AX ring in guest RAM.
        Audio_HLE_PollDeferred();
        if (eligible < 12 || (advanced && Memory::Read32(0x8067E754u) % 600u == 0)) {
            std::fprintf(stderr, "[prime-vi-spin] advanced retrace=%u\n", Memory::Read32(0x8067E754u));
        }
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
}

// 0x804EFC64 masks interrupts while sampling the VI counter, then sleeps via
// the guest scheduler. Some translated scheduler paths return without entering
// the host fiber idle loop; at the caller's wait boundary, model the scheduler
// idle window with EE enabled on an isolated context.
extern "C" void VI_HLE_PollRetraceGuestWait(CpuContext* waitingCtx) {
    if (waitingCtx == nullptr) {
        return;
    }
    CpuContext idleContext = *waitingCtx;
    idleContext.msr |= 0x00008000u;
    VI_HLE_PollRetraceGuestSpin(&idleContext);
}

void VI_HLE_ProcessRetracesDeferred(int maxToProcess) {
    if (maxToProcess <= 0 || !OS_HLE_InterruptsEnabled()) {
        return;
    }

    // This entry point runs from the middle of an arbitrary translated function
    // (GX__Begin's timing service, the host frame loop). Retrace callbacks are
    // an interrupt from that function's point of view, so they get a private
    // copy of its register file exactly as the hardware interrupt would.
    GuestInterruptCallbackContext interrupt;
    CpuContext* cpu = interrupt.get();

    // Host renderer ownership is not a guest critical section. Deliver due VI
    // callbacks at wall-clock cadence while suppressing thread switches and
    // all recursive Aurora/event work until the native GX call has unwound.
    OS_HLE_BeginDeferredGuestCallbacks();
    try {
        AdvanceDueRetraces(cpu, maxToProcess, false);
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
}

void VI_HLE_WaitForNextRetracePoll() {
    Clock::time_point retraceDeadline;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        retraceDeadline = g_vi.lastRetrace + g_vi.retraceInterval;
    }

    const auto now = Clock::now();
    if (now >= retraceDeadline) {
        return;
    }
    // Audio DMA and alarm queues still need regular service even when the VI
    // deadline is farther away. The high-resolution wait removes the 1 ms
    // scheduler overshoot when retrace is the next event.
    const bool retraceIsNext = retraceDeadline <= now + 1ms;
    SleepPreciselyUntil(retraceIsNext ? retraceDeadline : now + 1ms, retraceIsNext,
                        kFinalSpinWindow);
}

namespace {

// Retrace count consumed by the most recent paced present. This is a slot memo
// against the VI timeline, not a second clock: it only answers "did a retrace
// already elapse while this frame was being produced?". Producer-thread only.
uint32_t s_lastPacedRetraceCount = ~0u;

// Last presentation anchor handed to Aurora, in nanoseconds on the VI retrace
// grid. Guarantees consecutive sealed frames never share an anchor (see the
// comment at the stamping site). Producer-thread only, like the memo above.
uint64_t s_lastPresentAnchorNanos = 0;

// Tells Aurora how the frame about to be sealed relates to the VI timeline. Producer-thread only.
void StampInterpolationSchedule(uint64_t baseNanos, uint64_t intervalNanos, bool retraceOnTime) {
    // Prime waits for its own retrace, so a frame quantized to two periods still sees one boundary:
    // the wall clock between frames is what tells it was late.
    const auto now = Clock::now();
    static Clock::time_point s_lastFrame{};
    const bool wallClockOnTime = s_lastFrame == Clock::time_point{} ||
        now - s_lastFrame < std::chrono::nanoseconds(intervalNanos + intervalNanos / 2);
    s_lastFrame = now;
    aurora_report_producer_paced(retraceOnTime && wallClockOnTime);
    // Stamp the sealed frame's presentation schedule so Aurora paces interpolated slots against
    // this same VI timeline. Anchor to the NEXT retrace boundary, not the period just produced,
    // since slots anchored to the current period would already be expired by seal time. Encoding
    // overruns are corrected by sliding the whole slot group forward onto a later boundary of this
    // same grid, so this stays the single cadence authority. Anchors must also be strictly
    // monotonic: two frames sealed before lastRetrace advances would collide on one boundary and
    // burst-present, so a colliding anchor steps onto the next grid point instead of repeating it.
    uint64_t anchorNanos = baseNanos + intervalNanos;
    if (s_lastPresentAnchorNanos != 0 && anchorNanos <= s_lastPresentAnchorNanos) {
        anchorNanos = s_lastPresentAnchorNanos + intervalNanos;
    }
    s_lastPresentAnchorNanos = anchorNanos;
    aurora_set_present_schedule(anchorNanos, intervalNanos);
}

// Sleeps to the same VI retrace boundary VIWaitForRetrace targets, servicing alarms every 1 ms so audio
// DMA and timers keep running, then delivers that retrace so guest logic starts exactly on the grid.
void PaceToRetraceBoundary(Clock::time_point deadline) {
    constexpr auto kServiceSlice = 1ms;
    for (;;) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }
        if (deadline - now > kServiceSlice + kFinalSpinWindow) {
            SleepPreciselyUntil(now + kServiceSlice);
            OS_HLE_ProcessAlarmsDeferred(8);
            // Audio DMA is a 3 ms cadence and this wait is up to a full display
            // period long. Without a pump here the blocks that came due during
            // the wait are all delivered at once when the guest next reaches
            // idle, which the guest observes as AI slack jitter.
            Audio_HLE_PollDeferred();
            continue;
        }
        SleepPreciselyUntil(deadline, true, kFinalSpinWindow);
        break;
    }
    VI_HLE_ProcessRetracesDeferred(1);
}

} // namespace

// Single owner of the Aurora frame presentation sequence: seals the active frame, optionally paces the
// producer to the VI retrace boundary, and pre-warms the next frame. Paced from GXCopyDisp; unpaced for
// the retrace-context black/boot present path in AdvanceRetrace.
void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace) {
    if (s_presentSequenceActive.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    struct SequenceGuard {
        ~SequenceGuard() { s_presentSequenceActive.store(false, std::memory_order_release); }
    } sequenceGuard;
    Clock::time_point paceDeadline{};
    bool paceThisFrame = false;
    if (paceToRetrace) {
        uint64_t baseNanos = 0;
        uint64_t intervalNanos = 0;
        uint32_t retraceCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            paceDeadline = g_vi.lastRetrace + g_vi.retraceInterval;
            retraceCount = g_vi.retraceCount;
            baseNanos = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g_vi.lastRetrace.time_since_epoch())
                    .count());
            intervalNanos = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(g_vi.retraceInterval)
                    .count());
        }
        // Hold the frame to its boundary only when no retrace elapsed during
        // its production. A frame that missed its boundary presents
        // immediately: hardware would quantize down to the next vblank here,
        // but free-running late frames matches the previous pacer and keeps a
        // heavy scene at e.g. 50 fps instead of hard 30.
        const uint32_t retracesElapsed = retraceCount - s_lastPacedRetraceCount;
        paceThisFrame = retracesElapsed == 0;
        s_lastPacedRetraceCount = retraceCount;
        // aurora_report_producer_paced needs a different signal than the pace-wait above: the guest
        // self-paces via VIWaitForRetrace, so one retrace per produced frame is the healthy locked-60
        // cadence, and zero only happens when production outruns VI. "Kept up" means <=1 retrace
        // elapsed; 2+ means a boundary was missed, so Aurora seals that frame without its interpolated
        // slots (a windowed backstop lowers the slot target only under sustained overload).
        StampInterpolationSchedule(baseNanos, intervalNanos, retracesElapsed <= 1);
    } else if (aurora_get_frame_interpolation_fps() != 0) {
        // Prime presents from the retrace itself, with nothing to pace, but its inserted images need
        // the same timeline as any other frame.
        uint64_t baseNanos = 0;
        uint64_t intervalNanos = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            baseNanos = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g_vi.lastRetrace.time_since_epoch())
                    .count());
            intervalNanos = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(g_vi.retraceInterval)
                    .count());
        }
        StampInterpolationSchedule(baseNanos, intervalNanos, true);
    } else {
        // Retrace-context presents (VI black, boot) have no display period of
        // their own to subdivide; present as soon as the frame is ready. The
        // schedule grid is gone, so the anchor cursor must not constrain the
        // next paced frame.
        s_lastPresentAnchorNanos = 0;
        aurora_set_present_schedule(0, 0);
    }

    const auto finishPresent = [presentedXfb] {
        settings_overlay::AdvancePresentedFrame();
        g_auroraFrameActive.store(false, std::memory_order_release);
        g_auroraFrameHadWork.store(false, std::memory_order_release);
        if (presentedXfb) {
            std::lock_guard<std::mutex> lock(g_viMutex);
#if defined(MKW_GAME_PROFILE_RM3E01_00)
            g_vi.primeReadyXfbs.Presented(g_vi.primeSelectedXfb, g_vi.primeSelectedRevision);
#endif
            g_vi.hasValidXfb = false;
            g_vi.readyXfb = 0;
        }
        // Pre-warm the next frame so subsequent GX work has a valid frame context.
        UpdateAuroraAndProcessEvents();
        if (BeginAuroraFrame()) {
            g_auroraFrameActive.store(true, std::memory_order_release);
        }
    };
    // With the GX consumer thread, begin the next frame before pacing and
    // without releasing the GX lock: a consumer drain waits for the frame
    // worker's SEALED phase, which needs this begin, so releasing the lock
    // between end and begin could deadlock both threads.
    const bool beginBeforePacing = GxGpuThread::Enabled();
    {
        GxGpuThread::Lock gpuLock;
        aurora_end_frame();
        if (beginBeforePacing) finishPresent();
    }
    if (paceThisFrame) {
        PaceToRetraceBoundary(paceDeadline);
        std::lock_guard<std::mutex> lock(g_viMutex);
        s_lastPacedRetraceCount = g_vi.retraceCount;
    }
    if (!beginBeforePacing) finishPresent();
}

namespace {
std::atomic<bool> g_primeEarlyPresentPending{false};
std::atomic<uint32_t> g_primeEarlyPresentFb{0};

bool PrimePresentOnCopyEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("WII_COMPILED_PRIME_PRESENT_ON_COPY");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// Prime flips to a finished frame only from its GPU-done callback, so the copy
// is displayed at the retrace after next: about one display period after the
// image exists. Presenting the finished copy at the guest's next safe poll point
// shows the same frame ~a period sooner. The tracker then has nothing pending
// for that XFB, so the retrace does not present it twice.
void PresentPrimeCopyEarly() {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    if (!g_primeEarlyPresentPending.load(std::memory_order_acquire)) return;
    if (s_presentSequenceActive.load(std::memory_order_acquire) ||
        s_inAdvanceRetrace.load(std::memory_order_acquire)) return;  // retried at the next poll
    g_primeEarlyPresentPending.store(false, std::memory_order_release);
    const uint32_t framebuffer = g_primeEarlyPresentFb.load(std::memory_order_acquire);
    const bool isBlack = (Memory::Read16(0xCC002002u) & 1u) == 0 ||
        (Memory::Read16(0xCC002000u) & 0x3FF0u) == 0;
    if (framebuffer == 0 || isBlack) return;
    GxGpuThread::Lock gpuLock;
    if (!g_auroraFrameActive.load(std::memory_order_acquire)) return;
    uint64_t revision = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        revision = g_vi.primeReadyXfbs.Revision(framebuffer);
    }
    if (revision == 0 || !GXSelectTrackedDisplayCopy(GuestToHostPtr(framebuffer, 0))) return;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        g_vi.primeSelectedXfb = framebuffer;
        g_vi.primeSelectedRevision = revision;
    }
    settings_overlay::Draw();
    PrimeInputLatency::NoteHandoff();
    VI_HLE_PresentFrame(true, false);
#endif
}
}  // namespace

void VI_HLE_NotePrimeXfbCopied(uint32_t xfbAddr) {
    if (!PrimePresentOnCopyEnabled()) return;
    g_primeEarlyPresentFb.store(xfbAddr, std::memory_order_release);
    g_primeEarlyPresentPending.store(true, std::memory_order_release);
}

// -----------------------------------------------------------------------------
// VI_HLE_SetXfbReady - Called by GXCopyDisp to signal EFB->XFB copy completed.
// This marks that we now have valid framebuffer data to present.
// -----------------------------------------------------------------------------
void VI_HLE_SetXfbReady(uint32_t xfbAddr) {
    std::lock_guard<std::mutex> lock(g_viMutex);
    g_vi.hasValidXfb = true;
    g_vi.readyXfb = xfbAddr;
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    g_vi.primeReadyXfbs.MarkCopy(xfbAddr);
#endif
    if (g_vi.currentFrameBuffer == 0 && g_vi.nextFrameBuffer == 0) {
        g_vi.currentFrameBuffer = xfbAddr;
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
        WriteGuestStateLocked();
    } else if (g_vi.nextFrameBuffer != xfbAddr) {
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
    }
}

// VIInit (0x801B94A4) and its lower-level helper __VIInit (0x801B9294) both
// program MMIO at 0xCC0020xx on hardware. We skip all hardware access and seed
// the same defaults instead, so the two entry points share one body.
static void SeedViStateForInit(CpuContext* ctx, const char* who)
{
    RT_LOG(RT_TAG_VI) << who << " called: seeding VI state (HLE)" << std::endl;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
    }
    ViSetR3(ctx, 0);
}

extern "C" void VIInit_HLE_801b94a4(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "VIInit_801b94a4");
}
PPC_NATIVE_OVERRIDE_VOID(801B94A4, VIInit_HLE_801b94a4, (CpuContext* ctx), (ctx));

extern "C" void __VIInit_HLE_801b9294(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "__VIInit_801b9294");
}
PPC_NATIVE_OVERRIDE_VOID(801B9294, __VIInit_HLE_801b9294, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// Helper stubs referenced by VIInit switch cases (case D variants).
// These are hardware-specific; treat as no-ops to keep control flow intact.
// -----------------------------------------------------------------------------
extern "C" void VIInit_caseD_0_HLE_801b9934(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_0_801b9934 stubbed" << std::endl;
}
PPC_NATIVE_OVERRIDE_VOID(801B9934, VIInit_caseD_0_HLE_801b9934, (CpuContext* ctx), (ctx));

extern "C" void VIInit_caseD_1_HLE_801b993c(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_1_801b993c stubbed" << std::endl;
}
PPC_NATIVE_OVERRIDE_VOID(801B993C, VIInit_caseD_1_HLE_801b993c, (CpuContext* ctx), (ctx));

extern "C" void VIInit_caseD_2_HLE_801b9944(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_2_801b9944 stubbed" << std::endl;
}
PPC_NATIVE_OVERRIDE_VOID(801B9944, VIInit_caseD_2_HLE_801b9944, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VISetPreRetraceCallback (0x801B90F4)
// -----------------------------------------------------------------------------
extern "C" void VISetPreRetraceCallback_HLE_801b90f4(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.preRetraceCallback;
        g_vi.preRetraceCallback = newCb;
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(801B90F4, VISetPreRetraceCallback_HLE_801b90f4, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VISetPostRetraceCallback (0x801B9138)
// -----------------------------------------------------------------------------
extern "C" void VISetPostRetraceCallback_HLE_801b9138(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.postRetraceCallback;
        g_vi.postRetraceCallback = newCb;
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(801B9138, VISetPostRetraceCallback_HLE_801b9138, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VIGetDTVStatus (0x801BAD38)
// Reads DTV status from VI hardware (MMIO 0xCC00206E). Stub to "not ready".
// -----------------------------------------------------------------------------
extern "C" void VIGetDTVStatus_HLE_801bad38(CpuContext* ctx)
{
    ViSetR3(ctx, 0); // return 0 -> not ready / disabled
}
PPC_NATIVE_OVERRIDE_VOID(801BAD38, VIGetDTVStatus_HLE_801bad38, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VIConfigure & related helpers: translate GXRenderModeObj into guest globals.
// -----------------------------------------------------------------------------
extern "C" void VIConfigure_HLE_801b9f6c(CpuContext* ctx)
{
    // One read of the guest GXRenderModeObj serves both the VI pending state and
    // aurora, rather than unpacking the same 0x39 bytes twice.
    const uint32_t renderModePtr = ctx ? ctx->gpr[3] : 0;
    GXRenderModeObj renderMode{};
    if (!ReadGuestRenderModeObj(renderModePtr, renderMode)) {
        RT_LOG(RT_TAG_VI) << "VIConfigure: invalid GXRenderModeObj pointer 0x"
                  << std::hex << renderModePtr << std::dec << std::endl;
        ViSetR3(ctx, 0);
        return;
    }

    const uint32_t decodedTvFormat =
        ExtractTvFormat(static_cast<uint32_t>(renderMode.viTVmode));
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingTvFormat = decodedTvFormat;
        g_vi.pendingRetraceInterval = IntervalForFormat(g_vi.pendingTvFormat);
        g_vi.pendingRenderWidth =
            renderMode.viWidth != 0 ? renderMode.viWidth : renderMode.fbWidth;
        g_vi.pendingRenderHeight =
            renderMode.viHeight != 0 ? renderMode.viHeight : renderMode.xfbHeight;
        g_vi.pendingViXOrigin = renderMode.viXOrigin;
        g_vi.pendingViYOrigin = renderMode.viYOrigin;
        g_vi.pendingXfbWidth = renderMode.fbWidth;
        g_vi.pendingXfbHeight =
            renderMode.xfbHeight != 0 ? renderMode.xfbHeight : renderMode.efbHeight;
    }

    ::VIConfigure(&renderMode);

    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801B9F6C, VIConfigure_HLE_801b9f6c, (CpuContext* ctx), (ctx));

extern "C" void VIFlush_HLE_801ba9a4(CpuContext* ctx)
{
    uint32_t guestNextFb = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        if (g_vi.pendingNextFrameBuffer == 0) {
            try {
                guestNextFb = Memory::Read32(kViNextFrameBufferAddr);
            } catch (const Memory::AccessViolation&) {
                guestNextFb = 0;
            }
            if (guestNextFb == 0) {
                try {
                    guestNextFb = Memory::Read32(kViNextFrameBufferHwAddr);
                } catch (const Memory::AccessViolation&) {
                    guestNextFb = 0;
                }
            }
            if (guestNextFb != 0) {
                g_vi.pendingNextFrameBuffer = guestNextFb;
            }
        }
        
        // Arm only; the commit happens at the next retrace (see ViState).
        g_vi.flushArmed = true;
    }

    // NOTE: We do NOT set hasValidXfb here. Frame readiness is signaled ONLY by
    // GXCopyDisp (which sets hasValidXfb = true), as that's when the EFB->XFB
    // copy is complete and we have a valid frame to present.

    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801BA9A4, VIFlush_HLE_801ba9a4, (CpuContext* ctx), (ctx));

extern "C" void VISetNextFrameBuffer_HLE_801baab8(CpuContext* ctx)
{
    const uint32_t fbPtr = ctx ? ctx->gpr[3] : 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingNextFrameBuffer = fbPtr;
        // Also update guest memory for SDK code that reads this directly
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801BAAB8, VISetNextFrameBuffer_HLE_801baab8, (CpuContext* ctx), (ctx));

extern "C" void VIGetNextFrameBuffer_HLE_801bab24(CpuContext* ctx)
{
    uint32_t fb = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Return PENDING value - what was set by VISetNextFrameBuffer
        fb = g_vi.pendingNextFrameBuffer;
    }
    ViSetR3(ctx, fb);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(801BAB24, VIGetNextFrameBuffer_HLE_801bab24, (CpuContext* ctx), (ctx));

extern "C" void VISetBlack_HLE_801bab2c(CpuContext* ctx)
{
    const bool makeBlack = ctx ? (ctx->gpr[3] != 0) : false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingBlack = makeBlack;
    }
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801BAB2C, VISetBlack_HLE_801bab2c, (CpuContext* ctx), (ctx));

extern "C" void VIGetRetraceCount_HLE_801baba4(CpuContext* ctx)
{
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        count = g_vi.retraceCount;
    }
    ViSetR3(ctx, count);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(801BABA4, VIGetRetraceCount_HLE_801baba4, (CpuContext* ctx), (ctx));

extern "C" void VIGetNextField_HLE_801babac(CpuContext* ctx)
{
    bool fieldOdd = false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        fieldOdd = g_vi.fieldOdd;
    }
    ViSetR3(ctx, fieldOdd ? 1 : 0);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(801BABAC, VIGetNextField_HLE_801babac, (CpuContext* ctx), (ctx));

extern "C" void VIGetCurrentLine_HLE_801bac48(CpuContext* ctx)
{
    uint32_t height = 480;
    std::chrono::microseconds interval{16666us};
    Clock::time_point last;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        height = g_vi.xfbHeight;
        interval = g_vi.retraceInterval;
        last = g_vi.lastRetrace;
    }
    const auto now = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last);
    uint32_t line = 0;
    if (interval.count() > 0 && height > 0) {
        const uint64_t scaled = static_cast<uint64_t>(elapsed.count()) * height;
        line = static_cast<uint32_t>(std::min<uint64_t>(height - 1, scaled / interval.count()));
    }
    ViSetR3(ctx, line);
}
PPC_NATIVE_OVERRIDE_VOID(801BAC48, VIGetCurrentLine_HLE_801bac48, (CpuContext* ctx), (ctx));

extern "C" void VIWaitForRetrace_HLE_801b99ec(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    
    if (Fiber::GuestFiberManager::IsInitialized()) {
        const int32_t irqState = OS__DisableInterrupts_801a65ac();
        uint32_t retraceCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            retraceCount = g_vi.retraceCount;
        }

        do {
            cpu->gpr[3] = kViRetraceQueueAddr;
            OSSleepThread_HLE_801aa9b8(cpu);

            {
                std::lock_guard<std::mutex> lock(g_viMutex);
                EnsureInitializedLocked();
                if (g_vi.retraceCount != retraceCount) {
                    break;
                }
            }
        } while (true);

        OS__RestoreInterrupts_801a65d4(irqState);
    } else {
        std::chrono::microseconds interval{16666us};
        Clock::time_point target;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            interval = g_vi.retraceInterval;
            target = g_vi.lastRetrace + interval;
        }

        const auto now = Clock::now();
        if (now < target) {
            SleepPreciselyUntil(target, true);
        }
        AdvanceRetrace(cpu, target, true);
    }
    ViSetR3(cpu, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801B99EC, VIWaitForRetrace_HLE_801b99ec, (CpuContext* ctx), (ctx));
