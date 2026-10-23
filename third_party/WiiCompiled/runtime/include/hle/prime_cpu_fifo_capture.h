#pragma once
#include "hle/prime_cp_fifo.h"
#include <array>
#include <cstdint>
#include <stdexcept>

namespace PrimeCpuFifo {
// The CPU gather pipe writes RAM in 32-byte bursts even while detached from GP.
// The caller supplies physical PI registers and the bounded memory writer.
struct Capture {
    std::array<uint8_t, 32> pending{};
    unsigned count = 0;

    template<class Write>
    uint32_t Push(uint32_t base, uint32_t end, uint32_t pointer,
                  uint32_t value, unsigned width, Write write) {
        if (width != 1 && width != 2 && width != 4)
            throw std::logic_error("Invalid CPU FIFO store width");
        const uint32_t address = pointer & ~0x20000000u;
        // Validate before changing pending bytes, even for a partial burst.
        const uint32_t next = PrimeCpFifo::AdvanceBurst(base, end, address);
        for (unsigned i = width; i; --i) {
            pending[count++] = uint8_t(value >> ((i - 1) * 8));
            if (count == pending.size()) {
                write(address, pending.data(), pending.size());
                count = 0;
                pointer = next | (pointer & 0x20000000u) |
                          (address == end ? 0x20000000u : 0u);
            }
        }
        return pointer;
    }
};
}
