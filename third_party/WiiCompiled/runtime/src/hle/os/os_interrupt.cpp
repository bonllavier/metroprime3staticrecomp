// Interrupt masking/dispatch plus the EXI/IPC hardware stubs that hang off it.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>

#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "os_internal.h"
#include "guest_interrupt_context.h"

extern "C" uint32_t SetInterruptMask_801a66e0(uint32_t mask, uint32_t enable);
extern "C" uint32_t MemoryPrimeIpcPollControl();
extern "C" uint16_t MemoryPrimeDspPollControl();

namespace OsHleInternal {
std::atomic<bool> g_interrupts_enabled{true};
std::atomic<uint32_t> g_interrupt_mask{0};
} // namespace OsHleInternal

namespace {
std::once_flag interrupt_init_log_once;
std::once_flag exception_init_log_once;
thread_local bool g_dispatchingPrimeIpcInterrupt = false;
thread_local bool g_dispatchingPrimeAlarm = false;
thread_local bool g_dispatchingPrimeDspInterrupt = false;

inline void UpdateCurrentContextInterruptFlag(bool enabled)
{
    // Both OSDisableInterrupts and OSRestoreInterrupts land here. A failed lookup means no
    // context exists yet (normal during early boot), so we just skip the write in that case.
    uint32_t currentContext = 0;
    if (!MemoryInline::TryReadGuestScalar(kOSCurrentContextAddr, currentContext) ||
        currentContext == 0) {
        return;
    }
    const uint32_t modeFlagsAddr = currentContext + 0x1A2u;
    uint16_t modeFlags = 0;
    if (!MemoryInline::TryReadGuestScalar(modeFlagsAddr, modeFlags)) {
        return;
    }
    const uint16_t updated = enabled ? static_cast<uint16_t>(modeFlags | 0x0002u)
                                     : static_cast<uint16_t>(modeFlags & static_cast<uint16_t>(~0x0002u));
    if (updated == modeFlags) {
        return;
    }
    MemoryInline::TryWriteGuestScalar(modeFlagsAddr, updated);
}

void DispatchPendingPrimeIpcInterrupt()
{
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    static std::atomic<uint32_t> dispatchTraceCount{0};
    if (g_dispatchingPrimeIpcInterrupt) {
        return;
    }

    // Prime's IPC client registers its handler at interrupt 0x1B (mask 0x10).
    // IOS replies raise the Hollywood IPC flag, and the PPC observes them when
    // it next restores external interrupts. The idle-thread polling path is not
    // sufficient: OSInit itself spins while waiting for the asynchronous
    // SYSCONF callback, so it may never enter the scheduler's idle context.
    constexpr uint32_t kPrimeIpcInterrupt = 0x1Bu;
    constexpr uint32_t kPrimeIpcInitializedFlag = 0x8067E848u;
    constexpr uint32_t kPrimeIpcHandler = 0x805041B0u;
    constexpr uint32_t kMaxCompletionsPerPump = 64u;
    g_dispatchingPrimeIpcInterrupt = true;
    try {
        uint32_t completionCount = 0;
        for (; completionCount < kMaxCompletionsPerPump; ++completionCount) {
            const bool primeIpcInitialized =
                ::Memory::Contains(kPrimeIpcInitializedFlag, sizeof(uint32_t)) &&
                ::Memory::Read32(kPrimeIpcInitializedFlag) != 0;
            const uint32_t control = MemoryPrimeIpcPollControl();
            const bool hasIpcStatus = (control & 0x06u) != 0;
            if ((control & 0x14u) != 0x14u && (control & 0x22u) != 0x22u) {
                break;
            }

            const uint32_t traceIndex = hasIpcStatus
                ? dispatchTraceCount.fetch_add(1, std::memory_order_relaxed)
                : UINT32_MAX;
            if (hasIpcStatus && traceIndex < 256u) {
                RT_LOG(RT_TAG_OS) << "[prime-ipc-dispatch] completion=" << completionCount
                                  << " control=0x" << std::hex << control << " enabled=" << std::dec
                                  << g_interrupts_enabled.load(std::memory_order_acquire)
                                  << " mask=0x" << std::hex
                                  << g_interrupt_mask.load(std::memory_order_acquire) << std::dec
                                  << std::endl;
            }

            const uint32_t table = ::Memory::Read32(OsHleInternal::kInterruptHandlerTablePtrAddr);
            uint32_t handler = 0;
            if (table != 0 && ::Memory::Contains(table + kPrimeIpcInterrupt * 4u, 4u)) {
                handler = ::Memory::Read32(table + kPrimeIpcInterrupt * 4u);
            }
            // The RM3E01_00 binary registers this exact handler from FUN_80504368.
            // Keep the verified entry as a fallback because the native early-boot
            // OS HLE does not populate every interrupt-table mirror used by games.
            if (handler != 0 && handler != kPrimeIpcHandler && primeIpcInitialized) {
                RT_LOG(RT_TAG_OS) << "[prime-ipc-dispatch] ignore unexpected handler=0x"
                                  << std::hex << handler << std::dec << std::endl;
                break;
            }
            if (handler == 0 || !primeIpcInitialized) {
                handler = kPrimeIpcHandler;
            }
            CpuContext* current = TryGetCpuContext();
            const uint32_t runningBefore = ::Memory::Read32(kOSRunningContextAddr);
            const uint32_t currentBefore = ::Memory::Read32(kOSCurrentContextAddr);
            const uint32_t interruptedLr = current ? current->lr : 0;
            const uint32_t interruptedR1 = current ? current->gpr[1] : 0;
            if (handler == 0 || current == nullptr || (current->msr & 0x00008000u) == 0) {
                if (hasIpcStatus && traceIndex < 256u) {
                    RT_LOG(RT_TAG_OS) << "[prime-ipc-dispatch] blocked handler=0x" << std::hex << handler
                                      << " context=" << current
                                      << " msr=0x" << (current ? current->msr : 0u) << std::dec
                                      << std::endl;
                }
                break;
            }

            GuestInterruptCallbackContext interrupted;
            CpuContext* callback = interrupted.get();
            callback->msr &= ~0x00008000u; // External exception entry masks EE.
            callback->gpr[3] = kPrimeIpcInterrupt;
            // Both Prime interrupt handlers receive the interrupted OSContext,
            // not the interrupted r4. Its reply handler restores this pointer.
            uint32_t interruptedOsContext = currentBefore;
            if (interruptedOsContext == 0 || !::Memory::Contains(interruptedOsContext, 0x2C8u)) {
                interruptedOsContext = runningBefore;
            }
            if (interruptedOsContext == 0 || !::Memory::Contains(interruptedOsContext, 0x2C8u)) {
                interruptedOsContext = 0x8060B830u;
            }
            callback->gpr[4] = interruptedOsContext;
            InvokeIndirectCpu(handler, callback);
            const uint32_t runningAfter = ::Memory::Read32(kOSRunningContextAddr);
            if (current != nullptr && runningAfter != runningBefore) {
                static uint32_t contextTransferTraceCount = 0;
                if (contextTransferTraceCount++ < 16u) {
                    RT_LOG(RT_TAG_OS) << "[prime-ipc-context-switch] running=0x" << std::hex
                                      << runningBefore << "->0x" << runningAfter
                                      << " current=0x" << currentBefore << "->0x"
                                      << ::Memory::Read32(kOSCurrentContextAddr)
                                      << " interrupted_lr=0x" << interruptedLr << " interrupted_r1=0x"
                                      << interruptedR1 << " callback_lr=0x" << callback->lr
                                      << " callback_r1=0x" << callback->gpr[1]
                                      << std::dec << std::endl;
                }
            }
            if (hasIpcStatus && traceIndex < 256u) {
                RT_LOG(RT_TAG_OS) << "[prime-ipc-dispatch] returned control=0x" << std::hex
                                  << ::Memory::Read32(0xCD000004u)
                                  << " worker_count=" << std::dec << ::Memory::Read32(0x80679C50u)
                                  << " queue_head=" << ::Memory::Read32(0x8060FE10u)
                                  << " queue_tail=" << ::Memory::Read32(0x8060FE14u)
                                  << std::endl;
            }
        }
        if (completionCount == kMaxCompletionsPerPump) {
            RT_LOG(RT_TAG_OS) << "[prime-ipc-dispatch] bounded pump reached "
                              << kMaxCompletionsPerPump << " completions" << std::endl;
        }
        g_dispatchingPrimeIpcInterrupt = false;
    } catch (const ::Memory::AccessViolation& e) {
        g_dispatchingPrimeIpcInterrupt = false;
        LogMemoryError(RT_TAG_OS, "DispatchPendingPrimeIpcInterrupt", e);
        throw;
    } catch (...) {
        g_dispatchingPrimeIpcInterrupt = false;
        throw;
    }
#endif
}
void DispatchPendingPrimeDspInterrupt()
{
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    CpuContext* current = TryGetCpuContext();
    if (current == nullptr || !(current->msr & 0x8000u) ||
        g_dispatchingPrimeDspInterrupt || g_dispatchingPrimeIpcInterrupt || g_dispatchingPrimeAlarm)
        return;
    // Most translated call boundaries have no mailbox IRQ. Avoid walking the
    // guest interrupt table and memory regions unless the device is pending.
    const uint16_t pendingControl = MemoryPrimeDspPollControl();
    if ((pendingControl & 0x180u) != 0x180u || (pendingControl & 4u)) return;
    const uint32_t table = ::Memory::Read32(kInterruptHandlerTablePtrAddr);
    if (!table || !::Memory::Contains(table + 7u * 4u, 4u)) return;
    const uint32_t handler = ::Memory::Read32(table + 7u * 4u);
    if (handler != 0x804C5514u) return;
    if ((::Memory::Read32(kInterruptMaskLoAddr) | ::Memory::Read32(kInterruptMaskHiAddr)) & 0x01000000u)
        return;
    g_dispatchingPrimeDspInterrupt = true;
    try {
        for (uint32_t count = 0; count < 16u; ++count) {
            const uint16_t control = ::Memory::Read16(0xCC00500Au);
            if ((control & 0x180u) != 0x180u || (control & 4u)) break;
            uint32_t interrupted = ::Memory::Read32(kOSCurrentContextAddr);
            if (!interrupted || !::Memory::Contains(interrupted, 0x2C8u))
                interrupted = ::Memory::Read32(kOSRunningContextAddr);
            if (!interrupted || !::Memory::Contains(interrupted, 0x2C8u))
                throw std::runtime_error("Prime DSP IRQ has no valid interrupted OSContext");
            GuestInterruptCallbackContext callbackContext;
            CpuContext* callback = callbackContext.get();
            callback->msr &= ~0x8000u;
            callback->gpr[3] = 7u;
            callback->gpr[4] = interrupted;
            static uint32_t traceCount = 0;
            if (traceCount++ < 32u)
                RT_LOG(RT_TAG_OS) << "[prime-dsp-irq] handler=0x804c5514 control=0x" << std::hex
                    << control << " task=0x" << ::Memory::Read32(0x8067E294u)
                    << " oscontext=0x" << interrupted << std::dec << std::endl;
            InvokeIndirectCpu(handler, callback);
        }
        g_dispatchingPrimeDspInterrupt = false;
    } catch (...) {
        g_dispatchingPrimeDspInterrupt = false;
        throw;
    }
#endif
}
} // namespace

extern "C" void OS_HLE_DispatchPendingPrimeIpcInterrupt()
{
    DispatchPendingPrimeIpcInterrupt();
}

extern "C" void OS_HLE_DispatchPendingPrimeDspInterrupt()
{
    DispatchPendingPrimeDspInterrupt();
}

extern "C" void OS_HLE_DispatchPendingPrimeAidInterrupt()
{
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    CpuContext* current = TryGetCpuContext();
    if (!current || !(current->msr & 0x8000u) || g_dispatchingPrimeDspInterrupt ||
        g_dispatchingPrimeIpcInterrupt || g_dispatchingPrimeAlarm) return;
    const uint32_t table = ::Memory::Read32(kInterruptHandlerTablePtrAddr);
    if (!table || !::Memory::Contains(table + 5u * 4u, 4u)) return;
    // AIInit 0x804BE930 installs __AIDHandler here, not DSP's mailbox handler.
    const uint32_t handler = ::Memory::Read32(table + 5u * 4u);
    if (handler != 0x804BEAB0u) return;
    if ((::Memory::Read32(kInterruptMaskLoAddr) | ::Memory::Read32(kInterruptMaskHiAddr)) &
        0x04000000u) return;
    const uint16_t control = ::Memory::Read16(0xCC00500Au);
    if ((control & 0x18u) != 0x18u) return;
    uint32_t interrupted = ::Memory::Read32(kOSCurrentContextAddr);
    if (!interrupted || !::Memory::Contains(interrupted, 0x2C8u))
        interrupted = ::Memory::Read32(kOSRunningContextAddr);
    if (!interrupted || !::Memory::Contains(interrupted, 0x2C8u))
        throw std::runtime_error("Prime AID IRQ has no valid interrupted OSContext");
    g_dispatchingPrimeDspInterrupt = true;
    try {
        GuestInterruptCallbackContext callbackContext;
        CpuContext* callback = callbackContext.get();
        callback->msr &= ~0x8000u;
        callback->gpr[3] = 5u;
        callback->gpr[4] = interrupted;
        static uint32_t traceCount = 0;
        if (traceCount++ < 8u)
            RT_LOG(RT_TAG_OS) << "[prime-aid-irq] handler=0x804beab0 control=0x"
                << std::hex << control << " callback=0x" << ::Memory::Read32(0x8067E130u)
                << std::dec << std::endl;
        InvokeIndirectCpu(handler, callback);
        g_dispatchingPrimeDspInterrupt = false;
    } catch (...) {
        g_dispatchingPrimeDspInterrupt = false;
        throw;
    }
#endif
}

extern "C" void OS_HLE_PollPendingPrimeIpcInterrupt(CpuContext* ctx, uint32_t guestTarget)
{
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    if (ctx == nullptr || (ctx->msr & 0x00008000u) == 0) return;
    const uint32_t control = MemoryPrimeIpcPollControl();
    static std::atomic<uint32_t> pollTraceCount{0};
    if ((control & 0x22u) == 0x22u && pollTraceCount.fetch_add(1) < 48u) {
        RT_LOG(RT_TAG_OS) << "[prime-ipc-poll-boundary] ctx=" << ctx
                          << " target=0x" << std::hex << guestTarget
                          << " pc=0x" << std::hex << (ctx ? ctx->pc : 0u)
                          << " msr=0x" << (ctx ? ctx->msr : 0u)
                          << " control=0x" << control << " init=0x"
                          << ::Memory::Read32(0x8067E848u) << std::dec << std::endl;
    }
    if (ctx == nullptr || (ctx->msr & 0x00008000u) == 0) {
        return;
    }
    CpuContextScope contextScope(ctx);
    if ((control & 0x14u) == 0x14u || (control & 0x22u) == 0x22u)
        DispatchPendingPrimeIpcInterrupt();
    DispatchPendingPrimeDspInterrupt();
#else
    (void)ctx;
    (void)guestTarget;
#endif
}

extern "C" bool OS_HLE_IsPrimeIpcInterruptDispatching()
{
    return g_dispatchingPrimeIpcInterrupt;
}

extern "C" void VI_HLE_PollRetraceGuestSpin(CpuContext* ctx);
extern bool VI_HLE_IsAdvancingRetrace();
extern "C" bool OS_HLE_PrimeGuestCallbacksDeferred();

extern "C" bool OS_HLE_ShouldDeferPrimeScheduler()
{
    // Leave the guest ready queues intact; the interrupted scheduler services
    // them after the isolated interrupt register file has been discarded.
    return g_dispatchingPrimeIpcInterrupt || g_dispatchingPrimeAlarm ||
        g_dispatchingPrimeDspInterrupt || VI_HLE_IsAdvancingRetrace() ||
        OS_HLE_PrimeGuestCallbacksDeferred();
}

extern "C" void OS_HLE_ServicePrimeIdle(CpuContext* ctx, uint32_t stack, uint32_t sda)
{
    if (ctx == nullptr || (ctx->msr & 0x8000u) == 0) return;
    static uint32_t traceCount = 0;
    if (traceCount++ < 20u) {
        RT_LOG(RT_TAG_OS) << "[prime-idle] sp=0x" << std::hex << stack
                          << " caller=0x" << ::Memory::Read32(stack + 20u)
                          << " current=0x" << ::Memory::Read32(0x800000D4u)
                          << " running=0x" << ::Memory::Read32(0x800000E4u)
                          << " ready=0x" << ::Memory::Read32(sda - 7368u)
                          << " main_state=" << ::Memory::Read16(0x8060B418u + 0x2C8u)
                          << " main_pc=0x" << ::Memory::Read32(0x8060B418u + 0x198u)
                          << std::dec << std::endl;
    }
    CpuContext interrupted = *ctx;
    interrupted.gpr[1] = stack;
    interrupted.gpr[13] = sda;
    CpuContextScope scope(&interrupted);
    OS_HLE_PollPendingPrimeIpcInterrupt(&interrupted, 0x804E6D20u);
    VI_HLE_PollRetraceGuestSpin(&interrupted);
    // Execute Prime's actual decrementer service when its head alarm is due.
    // This uses the guest OS alarm queue and callback instead of replaying a
    // Nintendo task directly or writing its completion state from the host.
    if (!OS_HLE_ShouldDeferPrimeScheduler()) {
        const uint32_t alarm = ::Memory::Read32(0x8067E470u);
        if (alarm != 0 && ::Memory::Contains(alarm, 0x30u)) {
            const uint64_t fire = (static_cast<uint64_t>(::Memory::Read32(alarm + 8u)) << 32u) |
                                  ::Memory::Read32(alarm + 12u);
            if (OsHleInternal::ReadSystemTime() >= fire) {
                CpuContext alarmRegisters = interrupted;
                alarmRegisters.msr &= ~0x8000u;
                alarmRegisters.gpr[3] = 8u;
                alarmRegisters.gpr[4] = ::Memory::Read32(0x800000D4u);
                static uint32_t alarmTrace = 0;
                if (alarmTrace++ < 12u) {
                    RT_LOG(RT_TAG_OS) << "[prime-decrementer] alarm=0x" << std::hex << alarm
                                      << " callback=0x" << ::Memory::Read32(alarm)
                                      << " context=0x" << alarmRegisters.gpr[4] << std::dec << std::endl;
                }
                g_dispatchingPrimeAlarm = true;
                try {
                    CpuContextScope alarmScope(&alarmRegisters);
                    InvokeIndirectCpu(0x804DEDFCu, &alarmRegisters);
                } catch (...) {
                    g_dispatchingPrimeAlarm = false;
                    throw;
                }
                g_dispatchingPrimeAlarm = false;
            }
        }
    }
}

extern "C" void OS_HLE_TracePrimeContextLoad(CpuContext* ctx)
{
    const uint32_t target = ctx->gpr[3];
    static uint32_t count = 0;
    if (count++ >= 48u) return;
    RT_LOG(RT_TAG_OS) << "[prime-context-load] target=0x" << std::hex << target
                      << " pc=0x" << ::Memory::Read32(target + 0x198u)
                      << " msr=0x" << ::Memory::Read32(target + 0x19Cu)
                      << " stack=0x" << ::Memory::Read32(target + 4u)
                      << " caller=0x" << ctx->lr << std::dec << std::endl;
}

// Minimal, host-side replacements for early OS interrupt/exception helpers.
extern "C" int32_t OS__DisableInterrupts_801a65ac()
{
    const bool previous = g_interrupts_enabled.exchange(false);
    UpdateCurrentContextInterruptFlag(false);
    return previous ? 1 : 0;
}

extern "C" int32_t OS__RestoreInterrupts_801a65d4(int32_t level)
{
    const bool prev = g_interrupts_enabled.exchange(level != 0);
    UpdateCurrentContextInterruptFlag(level != 0);
    static std::atomic<uint32_t> restoreTraceCount{0};
    const uint32_t restoreIndex = restoreTraceCount.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ipcControl = ::Memory::Read32(0xCD000004u);
    if (restoreIndex < 96u || (ipcControl & 0x22u) == 0x22u) {
        CpuContext* current = TryGetCpuContext();
        RT_LOG(RT_TAG_OS) << "[prime-irq-boundary] restore=" << restoreIndex
                          << " level=" << level << " prev=" << prev
                          << " control=0x" << std::hex << ipcControl
                          << " msr=0x" << (current ? current->msr : 0u)
                          << " ee=" << ((current && (current->msr & 0x8000u)) ? 1 : 0)
                          << std::dec << std::endl;
    }
    if (level != 0) {
        DispatchPendingPrimeIpcInterrupt();
    }
    return prev ? 1 : 0;
}

extern "C" int32_t OS__EnableInterrupts_801a65c0()
{
    const bool prev = g_interrupts_enabled.exchange(true);
    UpdateCurrentContextInterruptFlag(true);
    DispatchPendingPrimeIpcInterrupt();
    return prev ? 1 : 0;
}

bool OS_HLE_InterruptsEnabled() noexcept
{
    return g_interrupts_enabled.load(std::memory_order_acquire);
}

extern "C" uint32_t OS____InterruptInit_801a661c(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8)
{
    (void)r3;
    (void)r4;
    (void)r5;
    (void)r6;
    (void)r7;
    (void)r8;

    std::call_once(interrupt_init_log_once, [&]() {
        RT_LOG(RT_TAG_OS) << "OS____InterruptInit_801a661c called: r3=" << r3 << " r4=" << r4 << " r5=" << r5
                  << " r6=" << r6 << " r7=" << r7 << " r8=" << r8 << std::endl;
        RT_LOG(RT_TAG_OS) << "OSInterruptInit: skipped hardware MMIO setup; marking interrupts initialized" << std::endl;
    });

    // Keep interrupts disabled until RestoreInterrupts decides otherwise.
    g_interrupts_enabled.store(false);

    try {
        // Initialize interrupt vector table pointer and clear the table (0x20 entries).
        ::Memory::Write32(kInterruptHandlerTablePtrAddr, kInterruptHandlerTableAddr);
        if (auto* table = ::Memory::GetPointer(kInterruptHandlerTableAddr, kInterruptHandlerTableBytes)) {
            std::memset(table, 0, kInterruptHandlerTableBytes);
        } else {
            for (size_t offset = 0; offset < kInterruptHandlerTableBytes; offset += 4) {
                ::Memory::Write32(kInterruptHandlerTableAddr + static_cast<uint32_t>(offset), 0);
            }
        }

        // Reset interrupt mask state tracked in guest memory and our host mirror.
        ::Memory::Write32(kInterruptMaskLoAddr, 0);
        ::Memory::Write32(kInterruptMaskHiAddr, 0);
        g_interrupt_mask.store(0);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OS____InterruptInit_801a661c", e);
    }

    return 0;
}

extern "C" uint32_t OS__ExceptionInit_801a00e0(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r20)
{
    (void)r3;
    (void)r4;
    (void)r5;
    (void)r6;
    (void)r7;
    (void)r8;
    (void)r20;

    std::call_once(exception_init_log_once, [&]() {
        RT_LOG(RT_TAG_OS) << "OS__ExceptionInit_801a00e0 called: r3=" << r3 << " r4=" << r4 << " r5=" << r5
                  << " r6=" << r6 << " r7=" << r7 << " r8=" << r8 << " r20=" << r20 << std::endl;
        RT_LOG(RT_TAG_OS) << "OSExceptionInit: skipped exception vector setup" << std::endl;
    });
    return 0;
}

extern "C" uint32_t __OSSetInterruptHandler_801a65f8_hle(uint32_t interrupt, uint32_t handler)
{
    constexpr uint32_t kMaxInterrupts = 32;

    uint32_t tableBase = 0;
    try {
        tableBase = ::Memory::Read32(kInterruptHandlerTablePtrAddr);
    } catch (const ::Memory::AccessViolation&) {
        tableBase = 0;
    }

    if (tableBase == 0) {
        tableBase = kInterruptHandlerTableAddr;
        try {
            ::Memory::Write32(kInterruptHandlerTablePtrAddr, tableBase);
        } catch (const ::Memory::AccessViolation&) {
            // If we cannot write the pointer, fail gracefully.
        }
    }

    if (interrupt >= kMaxInterrupts) {
        return 0;
    }

    const uint32_t entryAddr = tableBase + interrupt * 4u;
    uint32_t previous = 0;
    try {
        previous = ::Memory::Read32(entryAddr);
        ::Memory::Write32(entryAddr, handler);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "__OSSetInterruptHandler_801a65f8", e);
    }

    return previous;
}

extern "C" uint32_t __OSUnmaskInterrupts_801a69bc_hle(uint32_t mask)
{
    const int32_t level = OS__DisableInterrupts_801a65ac();

    uint32_t previous = 0;
    try {
        const uint32_t loMask = ::Memory::Read32(kInterruptMaskLoAddr);
        const uint32_t hiMask = ::Memory::Read32(kInterruptMaskHiAddr);
        previous = loMask;

        const uint32_t newLo = loMask & ~mask;
        ::Memory::Write32(kInterruptMaskLoAddr, newLo);
        g_interrupt_mask.store(newLo);

        const uint32_t combinedBefore = loMask | hiMask;
        const uint32_t combinedAfter = newLo | hiMask;
        for (uint32_t pending = mask & combinedBefore; pending != 0; pending = SetInterruptMask_801a66e0(pending, combinedAfter)) {
        }
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "__OSUnmaskInterrupts_801a69bc", e);
    }

    OS__RestoreInterrupts_801a65d4(level);
    return previous;
}

// EXI: early hardware init touches Hollywood registers (0xCD00xxxx). Provide a no-op stub.
extern "C" uint32_t EXIInit_80168fa0()
{
    RT_LOG(RT_TAG_OS) << "EXIInit_80168fa0 called: skipping MMIO register setup" << std::endl;
    return 0;
}

// ----------------------------------------------------------------------------
// Interrupt Controller - Mask/Unmask Hardware Interrupts
// ----------------------------------------------------------------------------

// SetInterruptMask (0x801a66e0): stub for Hollywood interrupt controller MMIO we don't emulate.
// Callers loop on this until it returns 0, so always return 0 to break the loop.
extern "C" uint32_t SetInterruptMask_801a66e0(uint32_t mask, uint32_t enable)
{
    // Keep the state tracking (useful for debugging)
    static std::atomic<int> call_count{0};
    const int prev_count = call_count.fetch_add(1);

    uint32_t old_mask = g_interrupt_mask.load();
    uint32_t new_mask = old_mask;
    if (enable) {
        new_mask = old_mask | mask;
    } else {
        new_mask = old_mask & ~mask;
    }
    g_interrupt_mask.store(new_mask);

    // Log occasionally
    if (prev_count < 8 || (prev_count & 0x3FF) == 0) {
        RT_LOG(RT_TAG_OS) << "SetInterruptMask_801a66e0 called: mask=0x" << std::hex << mask << std::dec
                  << " enable=" << enable
                  << " (forcing return 0 to break loop)" << std::endl;
    }
    return 0;
}

PPC_NATIVE_OVERRIDE(801A65AC, OS__DisableInterrupts_801a65ac, int32_t, (), ());
PPC_NATIVE_OVERRIDE(801A65C0, OS__EnableInterrupts_801a65c0, int32_t, (), ());
PPC_NATIVE_OVERRIDE(801A65D4, OS__RestoreInterrupts_801a65d4, int32_t, (int32_t level), (level));
#if !defined(MKW_GAME_PROFILE_RM3E01_00)
PPC_NATIVE_OVERRIDE(801A661C, OS____InterruptInit_801a661c, uint32_t, (uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8), (r3, r4, r5, r6, r7, r8));
#endif
PPC_NATIVE_OVERRIDE(801A66E0, SetInterruptMask_801a66e0, uint32_t, (uint32_t mask, uint32_t enable), (mask, enable));
PPC_NATIVE_OVERRIDE(801A00E0, OS__ExceptionInit_801a00e0, uint32_t, (uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r20), (r3, r4, r5, r6, r7, r8, r20));
PPC_NATIVE_OVERRIDE(80168FA0, EXIInit_80168fa0, uint32_t, (), ());
REGISTER_NATIVE_FUNCTION(0x801A65F8, __OSSetInterruptHandler_801a65f8_hle);
REGISTER_NATIVE_FUNCTION(0x801A69BC, __OSUnmaskInterrupts_801a69bc_hle);

// OS____MaskInterrupts (0x801a693c): stubbed out because its verification loop reads MMIO
// registers we don't emulate, which would spin forever.
extern "C" uint32_t OS____MaskInterrupts_801a693c(uint32_t mask, uint32_t unmask)
{
    static std::atomic<int> call_count{0};
    const int count = call_count.fetch_add(1);
    
    if (count < 5) {
        RT_LOG(RT_TAG_OS) << "OS____MaskInterrupts_801a693c called: mask=0x" << std::hex << mask
                  << " unmask=0x" << unmask << std::dec 
                  << " (stubbed to prevent MMIO verification loop)" << std::endl;
    }
    
    // Update our internal mask state
    uint32_t old_mask = g_interrupt_mask.load();
    uint32_t new_mask = old_mask;
    
    if (unmask) {
        new_mask = old_mask & ~mask;  // Clear bits
    } else {
        new_mask = old_mask | mask;   // Set bits
    }
    
    g_interrupt_mask.store(new_mask);
    
    // Return the previous mask state
    return old_mask;
}

PPC_NATIVE_OVERRIDE(801A693C, OS____MaskInterrupts_801a693c, uint32_t, (uint32_t mask, uint32_t unmask), (mask, unmask));

// ----------------------------------------------------------------------------
// EXISelect / EXIDeselect - HLE Stubs (0x801689d0 / 0x80168b00)
// Real implementation touches MMIO at 0xCD0068xx; stub returns 1 (success).
// ----------------------------------------------------------------------------

extern "C" uint32_t EXISelect_801689d0(uint32_t channel, uint32_t device, uint32_t frequency)
{
    // Log occasionally to avoid spam if polled frequently
    static int log_counter = 0;
    if (log_counter++ < 10) {
        RT_LOG(RT_TAG_OS) << "EXISelect_801689d0 called: channel=" << channel
                  << " device=" << device << " freq=" << frequency 
                  << " (stubbed success)" << std::endl;
    }
    return 1; // Return 1 (true) to indicate successful selection
}

// Body for the EXI stubs that take only a channel and report success: the real
// ops drive Hollywood MMIO we do not emulate, and the callers loop until they
// see success. Each keeps its own log budget and wording; the registrations
// stay spelled out below because the translator scans them by text.
#define EXI_CHANNEL_STUB(name, logLimit, channelLabel)                    \
    extern "C" uint32_t name(uint32_t channel)                            \
    {                                                                     \
        static int log_count = 0;                                         \
        if (log_count++ < (logLimit)) {                                   \
            RT_LOG(RT_TAG_OS) << #name " called: " channelLabel           \
                      << channel << " (stubbed success)" << std::endl;    \
        }                                                                 \
        return 1;                                                         \
    }

EXI_CHANNEL_STUB(EXIDeselect_80168b00, 10, "channel=")

// Register the functions
PPC_NATIVE_OVERRIDE(801689D0, EXISelect_801689d0, uint32_t, (uint32_t channel, uint32_t device, uint32_t frequency), (channel, device, frequency));
PPC_NATIVE_OVERRIDE(80168B00, EXIDeselect_80168b00, uint32_t, (uint32_t channel), (channel));

// ----------------------------------------------------------------------------
// SetExiInterruptMask (0x80167e78): stubbed no-op, our fake EXI devices need no interrupt masking.
// ----------------------------------------------------------------------------
extern "C" void SetExiInterruptMask_80167e78(uint32_t channel, uint32_t exi_struct_ptr)
{
    // channel: r3 (0, 1, 2)
    // exi_struct_ptr: r4
    // This function is void and typically just modifies internal OS masks.
    // We treat it as a successful no-op.
}

// Register the function
PPC_NATIVE_OVERRIDE_VOID(80167E78, SetExiInterruptMask_80167e78, (uint32_t channel, uint32_t exi_struct_ptr), (channel, exi_struct_ptr));

// ----------------------------------------------------------------------------
// EXI Transaction Stubs (Imm, Dma, Sync, Unlock)
// ----------------------------------------------------------------------------

// RVL__EXIImm / EXIImm
// Address: 0x80167f68
// Behavior: Performs an Immediate transfer (1-4 bytes) over EXI.
//           Stub: Return 1 (success). If it's a read, we clear the buffer.
extern "C" uint32_t EXIImm_80167f68(uint32_t channel, uint32_t buffer, uint32_t length, uint32_t type, uint32_t callback)
{
    // type: 0=Read, 1=Write, 2=RW
    // If reading, clear the destination buffer to 0 to be safe.
    if (type == 0 || type == 2) {
        try {
            for (uint32_t i = 0; i < length; ++i) {
                ::Memory::Write8(buffer + i, 0);
            }
        } catch (...) {
            RT_LOG(RT_TAG_OS) << "EXIImm: Failed to write to guest buffer 0x" << std::hex << buffer << std::dec << std::endl;
        }
    }
    
    // Log only occasionally
    static int log_count = 0;
    if (log_count++ < 5) {
        RT_LOG(RT_TAG_OS) << "EXIImm_80167f68 called: chan=" << channel << " len=" << length << " type=" << type << " (stubbed success)" << std::endl;
    }
    return 1; // Success
}

// RVL__EXIDma / EXIDma
// Address: 0x80168288
// Behavior: Performs a DMA transfer over EXI.
//           Stub: Return 1 (success).
extern "C" uint32_t EXIDma_80168288(uint32_t channel, uint32_t buffer, uint32_t length, uint32_t type, uint32_t callback)
{
    static int log_count = 0;
    if (log_count++ < 5) {
        RT_LOG(RT_TAG_OS) << "EXIDma_80168288 called: chan=" << channel << " len=" << length << " (stubbed success)" << std::endl;
    }
    return 1; // Success
}

// RVL__EXISync / EXISync
// Address: 0x80168380
// Behavior: Waits for the current EXI transfer to complete.
//           Stub: Return 1 (success) immediately.
EXI_CHANNEL_STUB(EXISync_80168380, 5, "chan=")

// RVL__EXIUnlock / EXIUnlock
// Address: 0x80169260
// Behavior: Unlocks the EXI channel and triggers any pending callbacks.
//           Stub: Return 1 (success) to bypass internal callback logic that causes the 0x0 crash.
EXI_CHANNEL_STUB(EXIUnlock_80169260, 5, "chan=")

// ----------------------------------------------------------------------------
// OSSetPowerCallback (0x801AB75C): sets the power-button callback pointer in the SDA (r13);
// the real STM/IOS registration is stubbed.
// ----------------------------------------------------------------------------
extern "C" uint32_t OSSetPowerCallback_801ab75c(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (!cpu) return 0;

    const uint32_t newCallback = cpu->gpr[3];
    const uint32_t r13 = cpu->gpr[13];

    // Assembly defines the default callback address as (0x801b0000 - 0x43f4)
    constexpr uint32_t kDefaultCallbackAddr = 0x801b0000u - 0x43f4u; // 0x801abc0c

    // Offsets from R13 (SDA2)
    const uint32_t kCallbackPtrAddr = r13 - 0x62b8u;
    const uint32_t kHandlerActiveAddr = r13 - 0x62c0u;

    RT_LOG(RT_TAG_OS) << "OSSetPowerCallback_801ab75c called: newCB=0x"
              << std::hex << newCallback << std::dec << std::endl;

    // 1. Disable Interrupts (Preserve atomicity as per SDK)
    int32_t oldLevel = OS__DisableInterrupts_801a65ac();

    uint32_t oldCallback = kDefaultCallbackAddr;

    try {
        // 2. Read the current callback
        if (::Memory::Contains(kCallbackPtrAddr, 4)) {
            oldCallback = ::Memory::Read32(kCallbackPtrAddr);
        }

        // 3. Update to the new callback (or reset to default if nullptr passed)
        uint32_t callbackToWrite = (newCallback != 0) ? newCallback : kDefaultCallbackAddr;
        ::Memory::Write32(kCallbackPtrAddr, callbackToWrite);

        // 4. Simulate STM Event Handler Registration
        // Real code calls IOS_IoctlAsync here. We just set the flag to 1 
        // to pretend the event handler was successfully registered.
        uint32_t isActive = ::Memory::Read32(kHandlerActiveAddr);
        if (isActive == 0) {
            ::Memory::Write32(kHandlerActiveAddr, 1);
        }

    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OSSetPowerCallback", e);
    }

    // 5. Restore Interrupts
    OS__RestoreInterrupts_801a65d4(oldLevel);

    // 6. Return logic: If old callback was default, return NULL. Otherwise return old address.
    if (oldCallback == kDefaultCallbackAddr) {
        return 0;
    }
    return oldCallback;
}

// Register the function
PPC_NATIVE_OVERRIDE(801AB75C, OSSetPowerCallback_801ab75c, uint32_t, (CpuContext* ctx), (ctx));

PPC_NATIVE_OVERRIDE(80167F68, EXIImm_80167f68, uint32_t, (uint32_t channel, uint32_t buffer, uint32_t length, uint32_t type, uint32_t callback), (channel, buffer, length, type, callback));
#if !defined(MKW_GAME_PROFILE_RM3E01_00)
PPC_NATIVE_OVERRIDE(80168288, EXIDma_80168288, uint32_t, (uint32_t channel, uint32_t buffer, uint32_t length, uint32_t type, uint32_t callback), (channel, buffer, length, type, callback));
#endif
PPC_NATIVE_OVERRIDE(80168380, EXISync_80168380, uint32_t, (uint32_t channel), (channel));
PPC_NATIVE_OVERRIDE(80169260, EXIUnlock_80169260, uint32_t, (uint32_t channel), (channel));

// ----------------------------------------------------------------------------
// IPC Register Access Stubs (0x80193020 write / 0x80193010 read): Broadway-IOS MMIO,
// unmapped here, so abort loudly instead of crashing silently.
// ----------------------------------------------------------------------------

extern "C" void IPCWriteReg_80193020(uint32_t index, uint32_t value)
{
    // index: Register index (0=Command, 1=Result, 2=Control, etc.)
    // value: 32-bit value to write
    // CRASH: We need to identify callers and stub them at a higher level
    RT_LOG(RT_TAG_OS) << "IPCWriteReg_80193020 called: idx=" << index
              << " val=0x" << std::hex << value << std::dec << std::endl;
    RT_LOG(RT_TAG_OS) << "IPC hardware access detected - stub the caller instead!" << std::endl;
    std::fflush(stderr);
    std::abort();
}

extern "C" uint32_t IPCReadReg_80193010(uint32_t index)
{
    // index: Register index
    // CRASH: We need to identify callers and stub them at a higher level
    RT_LOG(RT_TAG_OS) << "IPCReadReg_80193010 called: idx=" << index << std::endl;
    RT_LOG(RT_TAG_OS) << "IPC hardware access detected - stub the caller instead!" << std::endl;
    std::fflush(stderr);
    std::abort();
}

// Register the functions
PPC_NATIVE_OVERRIDE_VOID(80193020, IPCWriteReg_80193020, (uint32_t index, uint32_t value), (index, value));
PPC_NATIVE_OVERRIDE(80193010, IPCReadReg_80193010, uint32_t, (uint32_t index), (index));

// ----------------------------------------------------------------------------
// IPCCltInit (0x80193478): hardware parts (interrupt handler, MMIO) are stubbed, but we still
// call translated IPCInit (0x80192F7C) to init the IPC buffer globals; skip it and
// IPCGetBufferLo/Hi return 0, so ISFS_OpenLib fails with "APP ERROR: Not enough IPC arena".
// ----------------------------------------------------------------------------
extern "C" int32_t IPCCltInit_80193478(CpuContext* ctx)
{
    RT_LOG(RT_TAG_OS) << "IPCCltInit_80193478 called: calling IPCInit for buffer setup" << std::endl;
    
    // Sets 0x803867EC/F0/E8 (IPC buffer lo/hi + init flag) from __OSGetIPCBufferLo/Hi.
    InvokeIndirectCpu(0x80192F7Cu, ctx);

    // Advance the buffer lo pointer by 0x1000 (iosHeap size), matching real IPCCltInit.
    uint32_t bufferLo = Memory::Read32(ctx->gpr[13] + -25620); // 0x803867EC at r13-0x6414
    uint32_t newBufLo = bufferLo + 0x1000; // Advance by 4KB for iosHeap
    Memory::Write32(ctx->gpr[13] + -25620, newBufLo);
    
    RT_LOG(RT_TAG_OS) << "IPCCltInit: IPC buffer lo advanced from 0x" << std::hex << bufferLo
              << " to 0x" << newBufLo << std::dec << std::endl;
    
    // Skip the rest (interrupt handler, IPC MMIO access) - those are hardware-specific
    return 0; // Success
}

PPC_NATIVE_OVERRIDE(80193478, IPCCltInit_80193478, int32_t, (CpuContext* ctx), (ctx));
