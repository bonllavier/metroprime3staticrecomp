#pragma once
#include <cstdint>

// Snapshot of what the keyboard/mouse remote last published to the game (KPAD
// and raw WPAD), for the F6 motion panel. Written by the guest thread on each
// KPAD read, read by the overlay.
namespace PrimeRemoteDebug {
struct Snapshot {
    bool valid = false;       // keyboard/mouse mode active and at least one read published
    uint32_t hold = 0;        // WPAD buttons
    float acc[3] = {};        // Wiimote KPAD acc (g)
    float nunchukAcc[3] = {}; // Nunchuk KPAD acc (g)
    float stick[2] = {};
    float roll = 0;           // radians, counterclockwise positive
    float horizon[2] = {1, 0};
    bool pointerValid = false;
    float pointer[2] = {};    // KPAD pos, -1..1
    float distance = 1;       // KPAD dist
    bool irValid = false;
    float irDots[2][2] = {};  // raw DPD marks, camera pixels
    bool mouseLook = false;   // the camera follows the mouse this frame (pointer centred)
    float mouseLookFactor = 0, mouseLookPitch = 0, mouseLookTurnSpeed = 0;
};
bool Read(Snapshot& out);
}  // namespace PrimeRemoteDebug
