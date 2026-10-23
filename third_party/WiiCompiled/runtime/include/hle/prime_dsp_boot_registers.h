#pragma once
#include <cstdint>

// Boot control only; no DSP execution, synthesis or interrupt dispatch.
// Explicit monotonic nanoseconds keep the register contract testable.
struct PrimeDspBootRegisters {
    enum class Transition { None, ResetRom, InitAudio };
    uint16_t control = 0;
    uint64_t initClearAt = 0;
    uint16_t Read(uint64_t now) {
        if (initClearAt != 0 && now >= initClearAt) {
            control &= ~0x400u;
            initClearAt = 0;
        }
        return control;
    }
    Transition Write(uint16_t value, uint64_t now) {
        Read(now);
        const bool initialize = (control & 0x800u) && !(value & 0x800u);
        // AID/ARAM/DSP are W1C; DMA busy belongs to the transfer engine.
        const uint16_t status = (control & 0xA8u) & ~value;
        control = (value & 0xD56u) | status | (control & 0x200u);
        Transition result = (value & 1u) ? Transition::ResetRom : Transition::None;
        if (initialize) {
            control |= 0x400u;
            // Approx. 130 Wii TB ticks at 60.75 MHz; not cycle accurate.
            initClearAt = now + 2140u;
            result = Transition::InitAudio;
        }
        return result;
    }
    void CompleteAramDma() { control = (control & ~0x200u) | 0x20u; }
    // AID is the audio FIFO interrupt (bit 3), distinct from DSP mailbox bit 7.
    void RaiseAIDInterrupt() { control |= 0x08u; }
    bool Halted() const { return (control & 4u) != 0; }
};
