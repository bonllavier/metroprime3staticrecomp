#pragma once
// Model of RM3E01's own motion-gesture analysis, used to validate the gestures
// the keyboard mode synthesizes (tests) and to explain what the game needs.
//
// Recovered from main.dol (RM3E01, disc 0, rev 0) with Ghidra, 2026-10-25:
//   0x803C2670  input reader: KPADRead + WPADRead, one FUN_803C0A24 per sample
//   0x803C0A24  per-sample update of a 301-entry ring (0xB8 bytes each)
//   0x803BF22C  ring entry: raw * (1/512) [0x803BF358], |a|, two biquad sets
//               (0x803BF0C8 runs the same coefficients on x, y, z, |a|)
//   0x803BEFAC  biquad step, direct form 1; 0x803BEFF4 warms it 100 steps on
//               first use
//   0x803C19E4  coefficients: set 1 low-pass with w = 0.15707963705062866
//               (b1 = +2 b0), set 2 high-pass with w = 0.7853981852531433
//               (b1 = -2 b0); k = 1/tan(w), D = 1/(1 + sqrt(2) k + k^2)
//   0x803C11D8  gesture bits: |hp x| > 0.02 (hold 0.416667 s) and, over the
//               15 ring entries before the current one, range of lp x and
//               range of lp z > 0.25 (remote) / 0.45 (Nunchuk), held 0.25 s.
//               Remote bits 1, 2, 4; Nunchuk bits 0x10000, 0x20000, 0x40000.
//   0x803C3B94  bits -> gesture table A (16 entries of held/pressed/released
//               bytes; masks DAT_8059D678 = bit i), read by CFinalInput and
//               CControlMapper mapping type 3/4 (0x80061BA8).
// Tweaks (CTweakPlayerControl, read from a live memory dump): commands 28 and
// 29 (grapple lasso cast and pull, CPlayer 0x80175B54/0x8017F124 and
// CLassoPoint 0x800DF758) map to table A entry 10 = Nunchuk bit 0x40000: the
// low-passed Nunchuk Z acceleration must swing by more than 0.45 * 512 raw
// units within 15 samples. Raw units here are the WPAD accelerometer values
// (PrimeWpadPacket: 100 per g, clamped to +-512).
#include <array>
#include <cmath>
#include <cstdint>

namespace PrimeMotionModel {

struct Biquad {
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

// 0x803C19E4: coefficients from w (half the digital angular cutoff).
inline Biquad LowPass(double w) {
    const double k = 1.0 / std::tan(w), k2 = k * k, d = 1.0 / (1.0 + std::sqrt(2.0) * k + k2);
    return Biquad{float(d), float(2.0 * d), float(d), float(d * 2.0 * (1.0 - k2)), float(d * (1.0 - std::sqrt(2.0) * k + k2))};
}
inline Biquad HighPass(double w) {
    const double k = 1.0 / std::tan(w), k2 = k * k, d = 1.0 / (1.0 + std::sqrt(2.0) * k + k2);
    return Biquad{float(d), float(-2.0 * d), float(d), float(d * -2.0 * (1.0 - k2)), float(d * (1.0 - std::sqrt(2.0) * k + k2))};
}
constexpr double kLowPassW = 0.15707963705062866;   // DAT_80586038
constexpr double kHighPassW = 0.7853981852531433;   // DAT_80586040

// 0x803BEFAC / 0x803BEFF4.
struct BiquadState {
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    bool warmed = false;
    float Step(float x, const Biquad& c) {
        if (!warmed) {
            for (int i = 0; i < 100; ++i) Once(x, c);
            warmed = true;
        }
        return Once(x, c);
    }
    float Once(float x, const Biquad& c) {
        const float y = (c.b2 * x2 + c.b0 * x + c.b1 * x1) - c.a1 * y1 - c.a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

constexpr float kRawScale = 0.001953125f;  // FLOAT_806833F8 = 1/512
constexpr int kRingEntries = 301;
constexpr int kWindow = 15;
constexpr float kHighPassThreshold = 0.02f;   // FLOAT_80683428
constexpr float kHighPassHold = 0.4166667f;   // FLOAT_8068342C
constexpr float kWindowHold = 0.25f;          // FLOAT_80683430
constexpr float kRemoteSwing = 0.25f;         // FLOAT_80683430
constexpr float kNunchukSwing = 0.45f;        // FLOAT_80683434
// Deadband path (0x803C0E40): raw minus (50 + gravity unit) toward zero, over the
// gravity unit, clamped to +-1 g; 0x803BFF80 integrates |(x, z)| while it stays
// above 0.1 g; 0x803C11AC reports "any motion" (table A entry 7 / 15) past 0.01.
constexpr float kDeadbandRaw = 50.0f, kGravityUnitRaw = 100.0f;  // PrimeWpadPacket::kRawAccPerG
constexpr float kAnyMotionFloor = 0.1f, kAnyMotionThreshold = 0.01f;
inline float DeadbandAxis(int16_t raw) {
    const float band = kDeadbandRaw + kGravityUnitRaw;
    float value = 0.0f;
    if (raw > band) value = (raw - band) / kGravityUnitRaw;
    else if (raw < -band) value = (raw + band) / kGravityUnitRaw;
    return std::clamp(value, -1.0f, 1.0f);
}

// One device (remote or Nunchuk) of the game's analyser.
struct Device {
    bool nunchuk = false;
    Biquad lp = LowPass(kLowPassW), hp = HighPass(kHighPassW);
    std::array<BiquadState, 4> lpState{}, hpState{};
    struct Entry { std::array<float, 4> lp{}, hp{}; };
    std::array<Entry, kRingEntries> ring{};
    int index = 0, filled = 0;
    float holdHp = 0, holdWindow = 0;
    uint32_t windowBit = 0;
    float anyMotionIntegral = 0;

    uint32_t BitAnyMotion() const { return nunchuk ? 0x800000u : 0x80u; }

    uint32_t BitHp() const { return nunchuk ? 0x10000u : 0x1u; }
    uint32_t BitX() const { return nunchuk ? 0x20000u : 0x2u; }
    uint32_t BitZ() const { return nunchuk ? 0x40000u : 0x4u; }

    // One WPAD sample (raw accelerometer, 0 g = 0) after `dt` seconds. Returns
    // the gesture bits the game would publish for this device this sample.
    uint32_t Step(int16_t rawX, int16_t rawY, int16_t rawZ, float dt) {
        const float x = rawX * kRawScale, y = rawY * kRawScale, z = rawZ * kRawScale;
        const float sq = x * x + y * y + z * z;
        const float mag = sq != 0.0f ? std::sqrt(sq) : 0.0f;
        const std::array<float, 4> in{x, y, z, mag};
        index = (index + 1) % kRingEntries;
        Entry& e = ring[index];
        for (int i = 0; i < 4; ++i) {
            e.lp[i] = lpState[i].Step(in[i], lp);
            e.hp[i] = hpState[i].Step(in[i], hp);
        }
        if (filled < kRingEntries) ++filled;

        uint32_t bits = 0;
        {
            const float dx = DeadbandAxis(rawX), dz = DeadbandAxis(rawZ);
            const float magnitude = std::sqrt(dx * dx + dz * dz);
            anyMotionIntegral = magnitude < kAnyMotionFloor ? 0.0f : anyMotionIntegral + magnitude * dt;
            if (anyMotionIntegral > kAnyMotionThreshold) bits |= BitAnyMotion();
        }
        if (holdHp > 0.0f) {
            holdHp -= dt;
            if (holdHp > 0.0f) bits |= BitHp();
        } else if (std::fabs(e.hp[0]) > kHighPassThreshold) {
            holdHp = kHighPassHold;
            bits |= BitHp();
        }
        if (holdWindow > 0.0f) {
            holdWindow -= dt;
            if (holdWindow > 0.0f) bits |= windowBit;
        } else if (filled > kWindow) {
            float minX = 1000, maxX = -1000, minZ = 1000, maxZ = -1000;
            for (int back = kWindow; back >= 1; --back) {
                const Entry& p = ring[(index - back + kRingEntries) % kRingEntries];
                minX = std::fmin(minX, p.lp[0]); maxX = std::fmax(maxX, p.lp[0]);
                minZ = std::fmin(minZ, p.lp[2]); maxZ = std::fmax(maxZ, p.lp[2]);
            }
            const float swing = nunchuk ? kNunchukSwing : kRemoteSwing;
            if (maxX - minX > swing) { holdWindow = kWindowHold; windowBit = BitX(); bits |= BitX(); }
            if (maxZ - minZ > swing) { holdWindow = kWindowHold; windowBit = BitZ(); bits |= BitZ(); }
        }
        return bits;
    }
};

// Gesture table A entries (0x803C3B94 masks): the lasso commands read entry 10.
constexpr unsigned kLassoTableEntry = 10;
constexpr uint32_t kLassoBit = 0x40000u;

}  // namespace PrimeMotionModel
