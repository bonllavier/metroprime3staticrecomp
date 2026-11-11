#include "hle/prime_mouse_look.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

#define require(ok) do { if (!(ok)) { std::cerr << "FAIL line " << __LINE__ << ": " << #ok << "\n"; std::exit(1); } } while (false)

int main() {
    using namespace PrimeMouseLook;
    const float rad = SensitivityToRadPerPixel(0.2f);
    require(std::fabs(rad - 0.2f * kDegToRad) < 1e-7f);
    require(SensitivityToRadPerPixel(100.0f) == kMaxSensitivityDegPerPixel * kDegToRad);
    require(SensitivityToRadPerPixel(0.0f) == kMinSensitivityDegPerPixel * kDegToRad);

    // Mouse right turns clockwise: negative factor; the game's angular velocity
    // factor * speed covers -dx * rad in one frame.
    const float dt = 1.0f / 60.0f, speed = 3.0f;
    const float f = TurnFactor(10.0f, rad, dt, speed);
    require(f < 0 && std::fabs(f * speed * dt - (-10.0f * rad)) < 1e-6f);
    require(TurnFactor(-10.0f, rad, dt, speed) == -f);
    // A faster frame asks for a larger factor for the same pixels, saturating at 1.
    require(TurnFactor(10.0f, rad, dt / 2, speed) < f);
    require(TurnFactor(10000.0f, rad, dt, speed) == -1.0f && TurnFactor(-10000.0f, rad, dt, speed) == 1.0f);
    require(TurnFactor(10.0f, rad, 0.0f, speed) == 0.0f && TurnFactor(10.0f, rad, dt, 0.0f) == 0.0f);
    require(TurnFactor(0.0f, rad, dt, speed) == 0.0f);

    // Yaw rate: the game's first-order integrator (q + dt * 0.5 * (0, w) * q, normalized)
    // turns by exactly the requested angle, far beyond the game's own 2.5 rad/s, at any frame time.
    for (float pixels : {1.0f, 12.0f, 100.0f, 500.0f})  // 500 at 0.2 deg per pixel: 100 degrees in one frame
        for (float frame : {1.0f / 120.0f, dt, 1.0f / 30.0f}) {
            const float w = YawRate(pixels, rad, frame);
            require(w < 0);
            const float turned = 2.0f * std::atan(w * frame * 0.5f);
            require(std::fabs(turned + pixels * rad) < 1e-4f);
            require(YawRate(-pixels, rad, frame) == -w);
        }
    require(std::fabs(YawRate(100.0f, rad, dt)) > 2.5f);  // the cap that used to apply: 12 pixels per frame at 0.2
    require(YawRate(0.0f, rad, dt) == 0.0f && YawRate(10.0f, rad, 0.0f) == 0.0f);
    require(std::isfinite(YawRate(1e7f, rad, dt)) && std::isfinite(YawRate(-1e7f, rad, dt)));
    require(std::fabs(2.0f * std::atan(YawRate(1e7f, rad, dt) * dt * 0.5f) + kMaxYawPerFrame) < 1e-4f);

    // Mouse up (negative dy) looks up; invert flips; clamped to the game's limits.
    require(PitchStep(0.0f, -10.0f, rad, false) > 0 && PitchStep(0.0f, -10.0f, rad, true) < 0);
    require(PitchStep(0.0f, -100000.0f, rad, false) == kPitchMax);
    require(PitchStep(0.0f, 100000.0f, rad, false) == kPitchMin);
    require(PitchStep(0.1f, 0.0f, rad, false) == 0.1f);

    // Surface multiplier table layout of 0x802E7058.
    require(SurfaceMultiplierOffset(0, 0) == 0xDC && SurfaceMultiplierOffset(1, 0) == 0xFC && SurfaceMultiplierOffset(2, 0) == 0x11C);
    require(SurfaceMultiplierOffset(1, 5) == 0x114 && SurfaceMultiplierOffset(1, 6) == 0x110 && SurfaceMultiplierOffset(1, 7) == 0x118);
    require(SurfaceMultiplierOffset(1, 8) == 0xFC && SurfaceMultiplierOffset(1, 9) == 0xFC && SurfaceMultiplierOffset(1, 42) == 0xFC);

    // Smoothing: 0 passes deltas through; higher values carry part of each delta
    // into the next frames and sum to the same total travel.
    {
        float carry = 0;
        require(SmoothDelta(carry, 10.0f, 0.0f) == 10.0f && SmoothDelta(carry, 0.0f, 0.0f) == 0.0f);
        carry = 0;
        float total = 0;
        const float first = SmoothDelta(carry, 10.0f, 0.5f);
        require(first == 5.0f);
        total += first;
        for (int i = 0; i < 40; ++i) total += SmoothDelta(carry, 0.0f, 0.5f);
        require(std::fabs(total - 10.0f) < 1e-3f);
    }
    // Camera pitch leg: the game's cap min(angle, dt*deg2rad*speed*attenuation)
    // limits the stock speed but passes any angle with the substituted speed.
    {
        const float stock = 120.0f;  // a plausible tweak value, degrees per second
        require(CameraPitchStep(0.5f, dt, stock, 1.0f) < 0.5f);
        require(std::fabs(CameraPitchStep(0.01f, dt, stock, 1.0f) - 0.01f) < 1e-6f);
        for (float angle : {1e-4f, 0.01f, 0.5f, 3.14159f})
            for (float frame : {1.0f / 240.0f, dt, 1.0f / 30.0f})
                require(std::fabs(CameraPitchStep(angle, frame, kCameraPitchSpeedDegPerSecond, 1.0f) - angle) <= angle * 1e-6f);
        require(CameraPitchStep(0.5f, dt, kCameraPitchSpeedDegPerSecond, 0.0f) == 0.0f);  // the game's own freeze stays
    }
    std::cout << "PASS Prime mouse look: sensitivity, turn factor, pitch, surface table, smoothing, camera cap\n";
}
