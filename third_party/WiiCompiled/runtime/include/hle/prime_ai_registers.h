#pragma once

#include <cstdint>

// RM3E01 AI register contract. Time is supplied explicitly in nanoseconds so
// register semantics can be verified independently of the host clock/audio.
// This is the streaming counter and configuration bank, not DSP synthesis.
struct PrimeAiRegisters {
    uint32_t control = 0x42u; // AIS=48kHz, AID=32kHz, stopped.
    uint32_t volume = 0;
    uint32_t samples = 0;
    uint32_t interruptTiming = 0;
    uint64_t lastNanos = 0;
    uint64_t fractionalSamples = 0;

    void Advance(uint64_t now) {
        if (now < lastNanos) { lastNanos = now; return; }
        const uint64_t elapsed = now - lastNanos;
        lastNanos = now;
        if ((control & 1u) == 0) return;
        const uint64_t rate = (control & 2u) != 0 ? 48000u : 32000u;
        const uint64_t scaled = elapsed * rate + fractionalSamples;
        const uint32_t added = static_cast<uint32_t>(scaled / UINT64_C(1000000000));
        fractionalSamples = scaled % UINT64_C(1000000000);
        const uint32_t first = samples + 1u;
        samples += added;
        if (added != 0 && (control & 0x10u) == 0 &&
            static_cast<uint32_t>(interruptTiming - first) <= static_cast<uint32_t>(samples - first)) control |= 8u;
    }

    uint32_t Read(uint32_t offset, uint64_t now) {
        Advance(now);
        switch (offset) {
        case 0: return control;
        case 4: return volume;
        case 8: return samples;
        case 12: return interruptTiming;
        default: return 0;
        }
    }

    void Write(uint32_t offset, uint32_t value, uint64_t now) {
        Advance(now);
        switch (offset) {
        case 0:
            // AIINT is W1C; SCRESET is a strobe, not a retained status bit.
            control = (value & 0x57u) | ((value & 8u) ? 0u : (control & 8u));
            if (value & 0x20u) { samples = 0; fractionalSamples = 0; }
            break;
        case 4: volume = value; break;
        case 8: samples = value; fractionalSamples = 0; break;
        case 12: interruptTiming = value; break;
        }
    }
};
