#pragma once
#include "hle/prime_cp_fifo.h"
#include <array>
#include <cstdint>
#include <cstring>
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
        // Most GX writes are whole words inside a burst: store all four bytes at once.
        if (width == 4 && count <= pending.size() - 4) {
            pending[count] = uint8_t(value >> 24);
            pending[count + 1] = uint8_t(value >> 16);
            pending[count + 2] = uint8_t(value >> 8);
            pending[count + 3] = uint8_t(value);
            count += 4;
            if (count == pending.size()) {
                write(address, pending.data(), pending.size());
                count = 0;
                pointer = next | (pointer & 0x20000000u) | (address == end ? 0x20000000u : 0u);
            }
            return pointer;
        }
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

    // Appends the bytes of a store run that leave the burst at most one byte short of
    // complete; the byte that completes it must go through Push, which publishes the burst.
    // The caller validates the PI registers as Push would. Returns the bytes taken.
    size_t AppendWithinBurst(const uint8_t* data, size_t size) {
        const size_t room = pending.size() - 1u - count;
        const size_t take = size < room ? size : room;
        std::memcpy(pending.data() + count, data, take);
        count += static_cast<unsigned>(take);
        return take;
    }
};
}
