#pragma once
#include <cstdint>
#include <stdexcept>
namespace PrimeCpFifo {
inline uint32_t AdvanceBurst(uint32_t base, uint32_t end, uint32_t pointer) {
    if (base > end || pointer < base || pointer > end || ((base|end|pointer)&31u))
        throw std::logic_error("Invalid linked FIFO burst range");
    return pointer == end ? base : pointer + 32u;
}
inline bool AtBreakpoint(uint16_t control, uint32_t read, uint32_t breakpoint, bool commandBoundary) {
    return (control & 3u) == 3u && read == breakpoint && commandBoundary;
}
inline bool RetainBreakpoint(uint16_t control, uint16_t status, uint32_t read, uint32_t breakpoint) {
    return (control & 2u) != 0 && (status & 0x10u) != 0 && read == breakpoint;
}
inline bool InterruptPending(uint16_t control, uint16_t status) {
    return (control & 0x21u) == 0x21u && (status & 0x10u) != 0;
}
}
