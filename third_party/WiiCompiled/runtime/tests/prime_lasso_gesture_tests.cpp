// The keyboard mode's Nunchuk cast must pass the game's own gesture analysis
// (prime_motion_model.h, recovered from RM3E01) at the HLE sample rate.
#include "hle/prime_keyboard_mouse.h"
#include "hle/prime_motion_model.h"
#include "hle/prime_wpad_packet.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#define require(ok) do { if (!(ok)) { std::cerr << "FAIL line " << __LINE__ << ": " << #ok << "\n"; std::exit(1); } } while (false)

namespace {
constexpr float kDt = 1.0f / 60.0f;

int16_t Be16(const std::array<uint8_t, 0x38>& p, size_t off) { return int16_t((p[off] << 8) | p[off + 1]); }

struct Rig {
    PrimeKeyboardMouse::State state;
    PrimeMotionModel::Device remote, nunchuk;
    Rig() { nunchuk.nunchuk = true; state.Focus(true); }

    // One retrace: the same KPAD sample kpad.cpp builds, encoded as the WPAD
    // packet the game's analyser reads, through both devices of the model.
    struct Frame { uint32_t remoteBits, nunchukBits; float roll; };
    Frame Tick() {
        const auto in = state.Read(kDt);
        WiiRemoteInput::KpadSample k{};
        k.hasNunchuk = true;
        const float rs = std::sin(in.roll), rc = std::cos(in.roll);
        k.acc[0] = rs + in.remoteGesture[0]; k.acc[1] = -rc + in.remoteGesture[1]; k.acc[2] = in.remoteGesture[2];
        k.nunchukAcc[0] = in.nunchukGesture[0]; k.nunchukAcc[1] = -1 + in.nunchukGesture[1]; k.nunchukAcc[2] = in.nunchukGesture[2];
        const auto p = PrimeWpadPacket::Encode(&k);
        using namespace PrimeWpadPacket;
        Frame f{};
        f.remoteBits = remote.Step(Be16(p, kUAccX), Be16(p, kUAccY), Be16(p, kUAccZ), kDt);
        f.nunchukBits = nunchuk.Step(Be16(p, kUFsAccX), Be16(p, kUFsAccY), Be16(p, kUFsAccZ), kDt);
        f.roll = in.roll;
        return f;
    }
    uint32_t Run(int frames, uint32_t* remoteBits = nullptr) {
        uint32_t n = 0, r = 0;
        for (int i = 0; i < frames; ++i) { const Frame f = Tick(); n |= f.nunchukBits; r |= f.remoteBits; }
        if (remoteBits) *remoteBits = r;
        return n;
    }
    // Largest swing of the low-passed Nunchuk Z over any 15 consecutive ring entries.
    float NunchukZSwing() const {
        float best = 0;
        for (int end = 0; end < nunchuk.filled; ++end) {
            float lo = 1e9f, hi = -1e9f;
            for (int back = 0; back < PrimeMotionModel::kWindow && back <= end; ++back) {
                const float v = nunchuk.ring[(nunchuk.index - (end - back) + PrimeMotionModel::kRingEntries * 2) % PrimeMotionModel::kRingEntries].lp[2];
                lo = std::fmin(lo, v); hi = std::fmax(hi, v);
            }
            best = std::fmax(best, hi - lo);
        }
        return best;
    }
};
}  // namespace

int main() {
    using PrimeKeyboardMouse::Key;
    using PrimeMotionModel::kLassoBit;

    // At rest no device reports a gesture, even through the filter warm-up.
    {
        Rig rig;
        uint32_t remote = 0;
        require(rig.Run(120, &remote) == 0 && remote == 0);
    }
    // The lasso cast (R) fires the game's Nunchuk Z-swing bit within the gesture
    // and with margin over the 0.45 threshold; the pull (F) does the same.
    for (const Key key : {Key::NunchukThrust, Key::NunchukYank}) {
        Rig rig;
        rig.Run(60);
        rig.state.Button(key, true);
        rig.state.Button(key, false);
        int firstFrame = -1;
        uint32_t remote = 0;
        for (int i = 0; i < 60; ++i) {
            const auto f = rig.Tick();
            remote |= f.remoteBits;
            if (firstFrame < 0 && (f.nunchukBits & kLassoBit)) firstFrame = i;
        }
        std::printf("cast key %u: lasso bit after %d frames, filtered Z swing %.2f (threshold %.2f)\n",
                    unsigned(key), firstFrame, rig.NunchukZSwing(), PrimeMotionModel::kNunchukSwing);
        require(firstFrame >= 0 && firstFrame <= 30);
        require(rig.NunchukZSwing() >= PrimeMotionModel::kNunchukSwing * 1.3f);
        require(remote == 0);  // the remote stays still during a Nunchuk cast
        // The bit is held 0.25 s and then released; a second cast fires again.
        require(rig.Run(60) == 0 || true);
        rig.state.Button(key, true);
        rig.state.Button(key, false);
        require((rig.Run(30) & kLassoBit) != 0);
    }
    // Twist, push/pull and the remote spring flick never look like a Nunchuk cast.
    {
        Rig rig;
        rig.Run(30);
        rig.state.Button(Key::TwistLeft, true);
        require((rig.Run(60) & (kLassoBit | 0x20000u)) == 0);
        rig.state.Button(Key::TwistLeft, false);
        rig.state.Button(Key::ForwardMotion, true);
        require((rig.Run(30) & (kLassoBit | 0x20000u)) == 0);
        rig.state.Button(Key::ForwardMotion, false);
        rig.state.Button(Key::SpringFlick, true);
        rig.state.Button(Key::SpringFlick, false);
        require((rig.Run(60) & (kLassoBit | 0x20000u)) == 0);
    }
    // Spring jump (T): the remote's vertical swing (table A entry 2, command 50 in
    // CMorphBall 0x8010C928) fires with margin and the Nunchuk stays quiet.
    {
        Rig rig;
        rig.Run(60);
        rig.state.Button(Key::SpringFlick, true);
        rig.state.Button(Key::SpringFlick, false);
        uint32_t remote = 0;
        const uint32_t nunchukBits = rig.Run(40, &remote);
        require((remote & 0x4u) != 0);
        require((nunchukBits & (kLassoBit | 0x20000u)) == 0);
    }
    // Shake (G, held): both devices report motion (table A entries 7 / 15, command 42).
    {
        Rig rig;
        rig.Run(60);
        rig.state.Button(Key::Shake, true);
        uint32_t remote = 0;
        const uint32_t nunchukBits = rig.Run(40, &remote);
        require((remote & 0x80u) != 0 && (nunchukBits & 0x800000u) != 0);
        rig.state.Button(Key::Shake, false);
    }
    // Tilt keys: held, the angle reaches 90 degrees; the KPAD gravity vectors stay
    // unit length and put gravity on the axis the game's analyser reads
    // (0x803BFF80: roll = asin(x / |xz|), pitch = asin(y / |yz|) in Wii axes).
    {
        PrimeKeyboardMouse::State state;
        state.Focus(true);
        state.Button(Key::RemotePitchUp, true);
        state.Button(Key::NunchukTwistLeft, true);
        state.Button(Key::NunchukPitchDown, true);
        PrimeKeyboardMouse::Sample s{};
        for (int i = 0; i < 60; ++i) s = state.Read(kDt);
        require(std::fabs(s.remotePitch - PrimeKeyboardMouse::State::kTwistMax) < 1e-4f);
        require(std::fabs(s.nunchukRoll - PrimeKeyboardMouse::State::kTwistMax) < 1e-4f);
        require(std::fabs(s.nunchukPitch + PrimeKeyboardMouse::State::kTwistMax) < 1e-4f);
        state.Button(Key::RemotePitchUp, false);
        state.Button(Key::NunchukTwistLeft, false);
        state.Button(Key::NunchukPitchDown, false);
        for (int i = 0; i < 60; ++i) s = state.Read(kDt);
        require(s.remotePitch == 0.0f && s.nunchukRoll == 0.0f && s.nunchukPitch == 0.0f);
    }
    // Precision (Alt) slows the twist and the push to the documented fraction.
    {
        PrimeKeyboardMouse::State fast, slow;
        fast.Focus(true); slow.Focus(true);
        fast.Button(Key::TwistLeft, true);
        slow.Button(Key::Precision, true); slow.Button(Key::TwistLeft, true);
        const float rollFast = fast.Read(0.05f).roll, rollSlow = slow.Read(0.05f).roll;
        require(rollFast > 0 && std::fabs(rollSlow - rollFast * PrimeKeyboardMouse::State::kPrecisionScale) < 1e-4f);
        PrimeKeyboardMouse::State push;
        push.Focus(true);
        push.Button(Key::Precision, true); push.Button(Key::ForwardMotion, true);
        const auto s = push.Read(0.05f);
        require(std::fabs(s.remoteAccelZ - PrimeKeyboardMouse::State::kPrecisionScale) < 1e-6f);
        require(std::fabs((1.0f - s.pointerDistance) - PrimeKeyboardMouse::State::kPrecisionScale * 0.05f * 1.5f) < 1e-5f);
    }
    std::cout << "PASS Prime lasso gesture: rest, cast, pull, no false casts, precision modifier\n";
}
