#include "hle/prime_dsp_boot_registers.h"
#include <cassert>
#include <iostream>

int main() {
    using Transition = PrimeDspBootRegisters::Transition;
    PrimeDspBootRegisters dsp;
    // Replay the control/DMA observations in original OSInit 0x804DF620.
    assert(dsp.Write(0x8AC, 0) == Transition::None);
    assert(dsp.Halted());
    assert(dsp.Write(0x8AD, 0) == Transition::ResetRom);
    assert((dsp.Read(0) & 1) == 0);
    assert((dsp.Read(0) & 0xA8) == 0); // Writing flags acknowledges them.
    dsp.CompleteAramDma();
    assert((dsp.Read(0) & 0x20) != 0);
    dsp.Write(dsp.Read(0), 0);
    assert((dsp.Read(0) & 0x20) == 0);
    dsp.CompleteAramDma();
    dsp.Write(dsp.Read(0), 0);
    assert(dsp.Write(dsp.Read(0) & ~0x800u, 10) == Transition::InitAudio);
    assert((dsp.Read(2149) & 0x400) != 0);
    assert((dsp.Read(2150) & 0x400) == 0);
    dsp.Write(dsp.Read(2150) & ~4u, 2150);
    assert(!dsp.Halted());
    dsp.Write(dsp.Read(2150) | 4u, 2150);
    assert(dsp.Halted());
    dsp.Write(0x8AC, 2200);
    assert(dsp.Write(0x8AD, 2200) == Transition::ResetRom);
    assert((dsp.Read(2200) & 1) == 0);
    dsp.control |= 0x88;
    dsp.Write(0x800, 2300);
    assert((dsp.control & 0x88) == 0x88); // Zero preserves pending flags.
    dsp.Write(0x888, 2300);
    assert((dsp.control & 0x88) == 0);
    std::cout << "PASS DSP boot reset/halt/INIT deadline/ARAM DMA/W1C contracts\n";
}
