#include "hle/prime_ai_registers.h"
#include <cassert>
#include <iostream>

int main() {
    PrimeAiRegisters ai;
    ai.Write(0, 3u, 0); // Start 48 kHz.
    assert(ai.Read(8, 1'000'000'000) == 48000);
    ai.Write(0, 1u, 1'000'000'000); // Switch to 32 kHz, preserve accrued count.
    assert(ai.Read(8, 2'000'000'000) == 80000);
    ai.Write(0, 0u, 2'000'000'000);
    assert(ai.Read(8, 3'000'000'000) == 80000); // Stop freezes counter.
    ai.Write(0, 0x21u, 3'000'000'000); // Reset and resume 32 kHz.
    assert(ai.samples == 0 && (ai.control & 0x20u) == 0);
    ai.Write(12, 32, 3'000'000'000);
    assert((ai.Read(0, 3'001'000'000) & 8u) != 0); // Timing crossing.
    ai.Write(0, 9u, 3'001'000'000);
    assert((ai.control & 8u) == 0); // W1C.
    ai.Write(0, 0x11u, 3'001'000'000); // Hold interrupt status.
    ai.Write(12, 33, 3'001'000'000);
    assert((ai.Read(0, 3'002'000'000) & 8u) == 0);
    ai.Write(8, 0xFFFFFFF0u, 3'002'000'000);
    ai.Write(12, 0, 3'002'000'000);
    ai.Write(0, 1u, 3'002'000'000);
    assert(ai.Read(8, 3'003'000'000) == 16); // Wrap at 32 samples.
    assert((ai.control & 8u) != 0);
    ai.Write(4, 0x1234, 3'003'000'000);
    assert(ai.Read(4, 3'003'000'000) == 0x1234);
    PrimeAiRegisters sliced;
    sliced.Write(0, 3, 0);
    for (uint64_t n = 1; n <= 1000; ++n) sliced.Read(8, n * 1000);
    assert(sliced.samples == 48); // Fractional sample accumulation.
    std::cout << "PASS AI rate/stop/reset/W1C/hold/wrap/volume/fractional contracts\n";
}
