#include "hle/prime_host_continuations.h"
#include <cassert>
#include <iostream>

int main() {
    HostContext::Handle root = nullptr;
    assert(HostContext::InitializeScheduler(&root));
    CpuContext mainCpu{};
    mainCpu.gpr[1] = 0x80690000;
    mainCpu.gpr[31] = 0x11223344;
    mainCpu.fpscr = 4; // NI enabled, mirrored through host MXCSR.
    {
        CpuContextScope scope(&mainCpu);
        const uint32_t rootControl = MkwGetHostFpControl();
        PrimeHostContinuations* transport = nullptr;
        uint32_t completed = 0;
        PrimeHostContinuations continuations([&](uint32_t id, CpuContext* cpu) {
            assert(id == 2 && transport->Active() == 2);
            cpu->gpr[1] = 0x80980000;
            cpu->gpr[31] = 0x55667788;
            cpu->fpscr = 0;
            MkwApplyHostNiMode(0);
            volatile uint64_t localCanary = UINT64_C(0x0123456789ABCDEF);
            for (uint32_t iteration = 0; iteration < 100; ++iteration) {
                assert(TryGetCpuContext() == cpu);
                assert(cpu->gpr[1] == 0x80980000 && cpu->gpr[31] == 0x55667788);
                assert(localCanary == UINT64_C(0x0123456789ABCDEF));
                ++completed;
                assert(transport->Transfer(1, cpu) == 2);
                assert(TryGetCpuContext() == cpu);
                assert((MkwGetHostFpControl() & 0x8040u) == 0);
            }
            throw std::runtime_error("expected bootstrap failure");
        });
        transport = &continuations;
        continuations.BindRoot(1, &mainCpu);
        assert(continuations.Transfer(1, &mainCpu) == 1);
        volatile uint64_t mainCanary = UINT64_C(0xFEDCBA9876543210);
        for (uint32_t iteration = 0; iteration < 100; ++iteration) {
            assert(continuations.Transfer(2, &mainCpu) == 1);
            assert(TryGetCpuContext() == &mainCpu);
            assert(mainCpu.gpr[1] == 0x80690000 && mainCpu.gpr[31] == 0x11223344);
            assert(mainCanary == UINT64_C(0xFEDCBA9876543210));
            assert((MkwGetHostFpControl() & 0x8040u) == (rootControl & 0x8040u));
            assert(completed == iteration + 1);
        }
        bool relayed = false;
        try { continuations.Transfer(2, &mainCpu); }
        catch (const std::runtime_error& error) {
            relayed = std::string(error.what()) == "expected bootstrap failure";
        }
        assert(relayed && TryGetCpuContext() == &mainCpu);
        assert(continuations.Active() == 1);
    }
    HostContext::ShutdownScheduler(root);
    std::cout << "PASS 200 host transfers, local stacks, CPU identity/registers, NI and exception relay\n";
}
