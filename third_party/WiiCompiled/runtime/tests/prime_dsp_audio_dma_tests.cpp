#include "hle/prime_dsp_audio_dma.h"
#include "hle/prime_dsp_boot_registers.h"

#include <cstdlib>
#include <iostream>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

int main() {
    PrimeDspBootRegisters dsp;
    dsp.Write(0x110u, 0); // Enable AID and DSP mailbox masks.
    dsp.RaiseAIDInterrupt();
    Check((dsp.Read(0) & 0x88u) == 0x08u,
          "audio DMA raises AID bit 3 without inventing a DSP mailbox interrupt");
    dsp.control |= 0x80u; // Independent pending DSP mailbox.
    dsp.Write((dsp.Read(0) & 0xFF5Fu) | 8u, 0); // Original __AIDHandler ack.
    Check((dsp.Read(0) & 0x88u) == 0x80u,
          "AID acknowledgment preserves the separate pending DSP mailbox");
    dsp.Write(0x150u, 0);
    Check((dsp.Read(0) & 0x150u) == 0x150u, "all three DSP-bank IRQ masks survive writes");
    PrimeDspAudioDma dma;
    dma.Write(PrimeDspAudioDma::kStartHi, 0x005Du);
    dma.Write(PrimeDspAudioDma::kStartLo, 0xD61Fu);
    Check(dma.StartAddress() == 0x005DD600u, "start address aligns to 32-byte blocks");
    dma.Write(PrimeDspAudioDma::kControlLength, 0x800Cu);
    Check(dma.Enabled(), "enable edge starts DMA");
    Check(dma.Read(PrimeDspAudioDma::kBlocksLeft) == 11u, "blocks-left reports count minus one");

    PrimeDspAudioDma::Transfer block{};
    dma.Accumulate(249u);
    Check(!dma.TakeTransfer(block), "no block before 250 microseconds");
    dma.Accumulate(1u);
    Check(dma.TakeTransfer(block), "one block at 250 microseconds");
    Check(block.address == 0x005DD600u && !block.bufferCompleted,
          "first block uses the programmed physical address");
    Check(dma.Read(PrimeDspAudioDma::kBlocksLeft) == 10u, "remaining count decrements");

    dma.Accumulate(11u * PrimeDspAudioDma::kMicrosPerBlock);
    for (uint32_t i = 1; i < 12; ++i) {
        Check(dma.TakeTransfer(block), "expected contiguous ring block");
        Check(block.address == 0x005DD600u + i * 32u, "DMA advances by 32 bytes");
        Check(block.bufferCompleted == (i == 11u), "completion occurs on final block only");
    }
    Check(dma.Read(PrimeDspAudioDma::kBlocksLeft) == 11u, "ring reloads count after completion");
    dma.Accumulate(250u);
    Check(dma.TakeTransfer(block) && block.address == 0x005DD600u,
          "next ring starts again at programmed base");

    dma.Write(PrimeDspAudioDma::kControlLength, 0x000Cu);
    Check(!dma.Enabled() && dma.Read(PrimeDspAudioDma::kBlocksLeft) == 0u,
          "disabling clears pending blocks");
    dma.Accumulate(1000u);
    Check(!dma.TakeTransfer(block), "disabled DMA does not transfer buffered time");

    dma.Reset();
    Check(dma.StartAddress() == 0u && !dma.Enabled(), "reset clears register state");
    return 0;
}
