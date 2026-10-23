#pragma once

#include <algorithm>
#include <cstdint>

// Wii DSP audio DMA register/clock model. The DMA consumes eight stereo S16
// frames (32 bytes) every 250 us from the current guest address.
class PrimeDspAudioDma {
public:
    static constexpr uint32_t kStartHi = 0xCC005030u;
    static constexpr uint32_t kStartLo = 0xCC005032u;
    static constexpr uint32_t kUnusedLength = 0xCC005034u;
    static constexpr uint32_t kControlLength = 0xCC005036u;
    static constexpr uint32_t kUnused = 0xCC005038u;
    static constexpr uint32_t kBlocksLeft = 0xCC00503Au;
    static constexpr uint16_t kEnable = 0x8000u;
    static constexpr uint16_t kBlockMask = 0x7FFFu;
    static constexpr uint32_t kBytesPerBlock = 32u;
    static constexpr uint32_t kMicrosPerBlock = 250u;

    struct Transfer {
        uint32_t address = 0;
        bool bufferCompleted = false;
    };

    void Reset() { *this = {}; }

    void Write(uint32_t address, uint16_t value) {
        switch (address) {
        case kStartHi:
            m_startAddress = ((static_cast<uint32_t>(value) & 0x1FFFu) << 16u) |
                             (m_startAddress & 0xFFFFu);
            break;
        case kStartLo:
            m_startAddress = (m_startAddress & 0xFFFF0000u) | (value & 0xFFE0u);
            break;
        case kUnusedLength: m_unusedLength = value; break;
        case kControlLength: {
            const bool wasEnabled = Enabled();
            m_controlLength = value;
            if (!wasEnabled && Enabled()) {
                m_currentAddress = m_startAddress;
                m_remainingBlocks = BlockCount();
                m_elapsedMicros = 0;
            } else if (wasEnabled && !Enabled()) {
                m_remainingBlocks = 0;
                m_elapsedMicros = 0;
            }
            break;
        }
        case kUnused: m_unused = value; break;
        case kBlocksLeft: break; // Hardware read-only.
        default: break;
        }
    }

    uint16_t Read(uint32_t address) const {
        switch (address) {
        case kStartHi: return static_cast<uint16_t>((m_startAddress >> 16u) & 0x1FFFu);
        case kStartLo: return static_cast<uint16_t>(m_startAddress & 0xFFE0u);
        case kUnusedLength: return m_unusedLength;
        case kControlLength: return m_controlLength;
        case kUnused: return m_unused;
        case kBlocksLeft:
            return m_remainingBlocks > 0 ? static_cast<uint16_t>(m_remainingBlocks - 1u) : 0u;
        default: return 0;
        }
    }

    void Accumulate(uint32_t deltaMicros) {
        if (Enabled()) m_elapsedMicros += deltaMicros;
        else m_elapsedMicros = 0;
    }

    bool TakeTransfer(Transfer& transfer) {
        if (!Enabled() || m_elapsedMicros < kMicrosPerBlock) return false;
        m_elapsedMicros -= kMicrosPerBlock;
        transfer.address = m_currentAddress;
        transfer.bufferCompleted = false;
        if (m_remainingBlocks != 0) {
            --m_remainingBlocks;
            m_currentAddress += kBytesPerBlock;
        }
        if (m_remainingBlocks == 0) {
            m_currentAddress = m_startAddress;
            m_remainingBlocks = BlockCount();
            transfer.bufferCompleted = true;
        }
        return true;
    }

    bool Enabled() const { return (m_controlLength & kEnable) != 0; }
    uint32_t StartAddress() const { return m_startAddress; }
    uint16_t BlockCount() const { return m_controlLength & kBlockMask; }

private:
    uint32_t m_startAddress = 0;
    uint32_t m_currentAddress = 0;
    uint32_t m_remainingBlocks = 0;
    uint64_t m_elapsedMicros = 0;
    uint16_t m_unusedLength = 0;
    uint16_t m_controlLength = 0;
    uint16_t m_unused = 0;
};
