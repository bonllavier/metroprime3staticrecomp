#pragma once

// State the inline gather-pipe append (guest_abi/gather_inline.h) reads: pointers into
// memory.cpp's CPU FIFO capture, filled by its static initialisation. No macros here, so
// memory.cpp, which defines the out-of-line GX_HLE_FIFO_Write* functions, can include it.

#include <atomic>
#include <cstdint>

struct MkwGatherState {
    uint8_t* pending;               // PrimeCpuFifo::Capture::pending (32 bytes)
    unsigned* count;                // PrimeCpuFifo::Capture::count
    std::atomic<uint32_t>* pi;      // PI FIFO base (0xCC00300C), end (0xCC003010), write pointer (0xCC003014)
    const bool* replaying;          // g_primeFifoReplaying (gx_fifo.cpp)
};
extern MkwGatherState g_mkwGatherState;
