// Guest OSContext bookkeeping and the OSLoadContext context switch.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include "abi_bridge.h"
#include "memory.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "os_internal.h"
#if defined(MKW_GAME_PROFILE_RM3E01_00)
#include "hle/prime_host_continuations.h"
#include "hle/prime_trace_budget.h"

namespace PrimeContextRuntime {
thread_local bool bootstrapping = false;
thread_local std::unique_ptr<PrimeHostContinuations> continuations;

void Bootstrap(uint32_t id, CpuContext* cpu)
{
    // Execute the original register/GQR loader once on the new host stack.
    bootstrapping = true;
    try {
        cpu->gpr[3] = id;
        InvokeIndirectCpu(0x804E01C4u, cpu);
        bootstrapping = false;
    } catch (...) { bootstrapping = false; throw; }
    cpu->pc = cpu->srr0;
    MkwApplyHostNiMode(cpu->fpscr);
    const uint32_t entry = cpu->srr0;
    const uint32_t exit = cpu->lr;
    RT_LOG(RT_TAG_OS) << "[prime-host-thread-start] context=0x" << std::hex << id
        << " entry=0x" << entry << " exit=0x" << exit << " sp=0x" << cpu->gpr[1]
        << std::dec << std::endl;
    InvokeIndirectCpu(entry, cpu);
    // OSInitContext supplies the thread's real return/exit continuation.
    if (!exit) throw std::runtime_error("Prime thread entry returned without an exit address");
    InvokeIndirectCpu(exit, cpu);
    throw std::runtime_error("Prime guest thread exit continuation returned");
}
} // namespace PrimeContextRuntime
#endif

extern "C" void OS_HLE_PrimeTransferContext(CpuContext* cpu)
{
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    using namespace PrimeContextRuntime;
    if (bootstrapping) return;
    if (!continuations) {
        continuations = std::make_unique<PrimeHostContinuations>(Bootstrap);
        continuations->BindRoot(0x8060B418u, cpu);
    }
    const uint32_t requested = cpu->gpr[3];
    const uint32_t source = continuations->Active();
    static PrimeTraceBudget switchTrace(128);
    if (requested != source && switchTrace.Take()) {
        RT_LOG(RT_TAG_OS) << "[prime-host-thread-switch] from=0x" << std::hex << source
            << " to=0x" << requested << std::dec << std::endl;
    }
    const uint32_t resumed = continuations->Transfer(requested, cpu);
    // The caller is the suspended SelectThread frame for this context.
    // Its original OSSaveContext saved the continuation at 0x804E6CD4.
    if (::Memory::Read32(resumed + 0x198u) != 0x804E6CD4u ||
        ::Memory::Read32(resumed + 4u) != cpu->gpr[1])
        throw std::runtime_error("Prime context continuation is not the saved SelectThread frame");
    cpu->gpr[3] = resumed;
#else
    (void)cpu;
#endif
}

extern "C" uint32_t OS__GetCurrentThread_801a98b0_hle()
{
    try {
        return ::Memory::Read32(kOSRunningContextAddr);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OS__GetCurrentThread_801a98b0", e);
        return 0;
    }
}

extern "C" void OS__ClearContext_801a2098(uint32_t contextAddr)
{
    if (contextAddr == 0) {
        return;
    }

    try {
        ::Memory::Write16(contextAddr + 0x1a0u, 0);
        ::Memory::Write16(contextAddr + 0x1a2u, 0);

        const uint32_t exceptionCtx = ::Memory::Read32(kOSExceptionContextAddr);
        if (exceptionCtx == contextAddr) {
            ::Memory::Write32(kOSExceptionContextAddr, 0);
        }
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OS__ClearContext_801a2098", e);
    }
}

extern "C" void OS__SetCurrentContext_801a1e70(uint32_t contextAddr)
{
    try {
        if (contextAddr == 0) {
            ::Memory::Write32(kOSPhysicalContextAddr, 0);
            ::Memory::Write32(kOSCurrentContextAddr, 0);
            return;
        }

        const uint32_t physicalMask = contextAddr & 0x3fffffffu;
        ::Memory::Write32(kOSPhysicalContextAddr, physicalMask);
        ::Memory::Write32(kOSCurrentContextAddr, contextAddr);

        const uint32_t exceptionCtx = ::Memory::Read32(kOSExceptionContextAddr);
        uint32_t flags = ::Memory::Read32(contextAddr + 0x19cu);
        if (exceptionCtx == contextAddr) {
            flags |= 0x2000u;
        } else {
            flags &= ~0x2000u;
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        ::Memory::Write32(contextAddr + 0x19cu, flags);

        uint16_t modeFlags = ::Memory::Read16(contextAddr + 0x1a2u);
        if (g_interrupts_enabled.load()) {
            modeFlags |= 0x0002u;
        } else {
            modeFlags &= static_cast<uint16_t>(~0x0002u);
        }
        ::Memory::Write16(contextAddr + 0x1a2u, modeFlags);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OS__SetCurrentContext_801a1e70", e);
    }
}

PPC_NATIVE_OVERRIDE_VOID(801A2098, OS__ClearContext_801a2098, (uint32_t contextAddr), (contextAddr));
PPC_NATIVE_OVERRIDE_VOID(801A1E70, OS__SetCurrentContext_801a1e70, (uint32_t contextAddr), (contextAddr));
REGISTER_NATIVE_FUNCTION(0x801A98B0, OS__GetCurrentThread_801a98b0_hle);

// OSLoadContext (0x801A1F58): restores CPU state from a guest OSContext and jumps to SRR0 by hand,
// since the real function's privileged mtspr/rfi can't be translated.

extern "C" [[noreturn]] void OS__LoadContext_801a1f58(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    const uint32_t guestContextAddr = cpu->gpr[3];

    RT_LOG(RT_TAG_OS) << "OS__LoadContext_801a1f58 called: ctx=0x" << std::hex << guestContextAddr << std::dec << std::endl;

    if (guestContextAddr == 0) {
        RT_LOG(RT_TAG_OS) << "OS__LoadContext: FATAL - null context address!" << std::endl;
        std::fflush(stderr);
        std::abort();
    }

    try {
        // Validate SRR0 (the jump target, at 0x198) before mutating any other state.
        uint32_t srr0 = ::Memory::Read32(guestContextAddr + 0x198u);
        
        // Validate the jump target exists in our translated function registry
        if (srr0 == 0) {
            RT_LOG(RT_TAG_OS) << "OS__LoadContext: FATAL - SRR0 (jump target) is NULL!" << std::endl;
            std::fflush(stderr);
            std::abort();
        }

        // ---- Load GPRs ----
        for (int i = 0; i < 32; ++i) {
            cpu->gpr[i] = ::Memory::Read32(guestContextAddr + static_cast<uint32_t>(i * 4));
        }

        // ---- Load Special Registers ----
        cpu->cr  = ::Memory::Read32(guestContextAddr + 0x80u);
        cpu->lr  = ::Memory::Read32(guestContextAddr + 0x84u);
        cpu->ctr = ::Memory::Read32(guestContextAddr + 0x88u);
        cpu->xer = ::Memory::Read32(guestContextAddr + 0x8Cu);

        // ---- Load GQRs (Graphics Quantization Registers) ----
        // GQR0 is always 0 (not saved/restored)
        cpu->gqr[0] = 0;
        cpu->gqr[1] = ::Memory::Read32(guestContextAddr + 0x1A8u);
        cpu->gqr[2] = ::Memory::Read32(guestContextAddr + 0x1ACu);
        cpu->gqr[3] = ::Memory::Read32(guestContextAddr + 0x1B0u);
        cpu->gqr[4] = ::Memory::Read32(guestContextAddr + 0x1B4u);
        cpu->gqr[5] = ::Memory::Read32(guestContextAddr + 0x1B8u);
        cpu->gqr[6] = ::Memory::Read32(guestContextAddr + 0x1BCu);
        cpu->gqr[7] = ::Memory::Read32(guestContextAddr + 0x1C0u);

        // ---- Clear exception context flag if set ----
        uint16_t modeFlags = ::Memory::Read16(guestContextAddr + 0x1A2u);
        if ((modeFlags & 0x0002u) != 0) {
            modeFlags &= ~0x0002u;
            ::Memory::Write16(guestContextAddr + 0x1A2u, modeFlags);
        }

        // ---- Update PC and SRR registers ----
        cpu->srr0 = srr0;
        cpu->srr1 = ::Memory::Read32(guestContextAddr + 0x19Cu);
        cpu->pc = srr0;

        RT_LOG(RT_TAG_OS) << "OS__LoadContext: jumping to target=0x" << std::hex << srr0 
                  << " SP=0x" << cpu->gpr[1] << std::dec << std::endl;

        // Equivalent to 'rfi': jump to SRR0 with MSR from SRR1.
        InvokeIndirectJump(srr0, cpu);

        // If we get here, the function returned normally (shouldn't happen for context switches)
        // But for safety, just return (the caller's state is now the loaded context)
        RT_LOG(RT_TAG_OS) << "OS__LoadContext: WARNING - target returned, this is unexpected!" << std::endl;

    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_OS, "OS__LoadContext", e);
        std::fflush(stderr);
        std::abort();
    }

    // This function should never return (rfi is a non-returning jump)
    // If InvokeIndirectJump returns, we have a problem
    std::fflush(stderr);
    std::abort();
}

PPC_NATIVE_OVERRIDE_VOID(801A1F58, OS__LoadContext_801a1f58, (CpuContext* ctx), (ctx));
