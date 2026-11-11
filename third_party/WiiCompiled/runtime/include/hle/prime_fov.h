#pragma once
// First-person field of view for RM3E01, modelled on PrimeHack's view modifier
// for Metroid Prime 3 standalone NTSC-U (shiiion/dolphin, PrimeHack/Mods/
// ViewModifier.cpp and AddressDBInit.cpp; GPL, read as a reference only, no code
// taken). Addresses checked against our DOL (recomp/docs/prime_fov.md).
//
// Every game frame, the chosen FOV is written into the active camera's
// parameter block and into camera 4's, unless a cinematic owns the camera.
// The game keeps its FOV in two floats of that block: +0x18 is the current
// value, +0x1C the target it eases toward (setter 0x8007F0BC). Objects reach
// the block as camera +0x178, component 5 of 0x8009CED0. Four places would put
// the game's own FOV back, and PrimeHack turns them into nops:
//   0x800DD87C, 0x800DCEB0, 0x8017D40C  bl 0x8009CF90 (CGameCamera set FOV)
//   0x8007F504  the easing step's store of the current FOV (leaf 0x8007F0AC)
// Here they are skipped through runtime hooks instead, only while the option
// is on: the calls by their return address, the store in the easing function.
// The camera switch at 0x802B26C0 (stw r0,0x14(r6) in 0x802B2668) applies the
// FOV to the new camera at once, as PrimeHack's vmcall does.
// Wider views reach objects that the game's culling test (0x80316A1C, box
// against plane) drops. Above 94 degrees, or when the player asks, that test
// answers 1 (PrimeHack: li r3,1; blr): everything is drawn, at a GPU cost.
// The arm cannon (CTweakGun +0xE0: left, forward, up) moves in and z-near
// (perspective info +0x8C) drops to 0.1 between 75 and 125 degrees, as
// PrimeHack's automatic arm adjustment does.
#include <algorithm>
#include <cstdint>

namespace PrimeFov {

constexpr float kMinFov = 40.0f, kMaxFov = 170.0f;  // PrimeHack clamps to 170
constexpr float kDefaultFov = 75.0f;
constexpr float kCullingThreshold = 94.0f;          // above this the culling test is bypassed

// RM3E01 addresses (PrimeHack's PRIME_3_STANDALONE NTSC-U entries).
constexpr uint32_t kStateManager = 0x805C4F98u;      // -> +0x0 -> +0x1010: object list
                                                     // -> +0x10 -> +0xC, +0x16: camera manager (u16 active id)
constexpr uint32_t kTweakGun = 0x8067D78Cu;          // -> -> +0xE0: arm position
constexpr uint32_t kCameraParams = 0x178u, kCameraCinematic = 0x2E4u;
constexpr uint32_t kParamsFov = 0x18u, kParamsTargetFov = 0x1Cu;
constexpr uint32_t kCinematicFlags = 0x38u, kCinematicFlag = 0x20u;
constexpr uint32_t kArmOffset = 0xE0u, kZNearOffset = 0x8Cu;
constexpr uint16_t kNoCamera = 0xFFFFu, kForcedCamera = 4;  // PrimeHack also updates object 4

// Return addresses (call + 4) of the three calls to 0x8009CF90 PrimeHack nops.
constexpr uint32_t kHeldSetFovReturns[3] = {0x800DD880u, 0x800DCEB4u, 0x8017D410u};
inline bool HeldSetFovCaller(uint32_t returnAddress) {
    for (const uint32_t held : kHeldSetFovReturns)
        if (returnAddress == held) return true;
    return false;
}

inline float ClampFov(float fov) {
    return fov == fov ? std::clamp(fov, kMinFov, kMaxFov) : kDefaultFov;
}
inline bool CullingBypassed(float fov, bool playerAsked) {
    return playerAsked || ClampFov(fov) > kCullingThreshold;
}

struct Arm {
    float left = 0.25f, forward = 0.30f, up = -0.35f;
};
constexpr Arm kWideArm{0.22f, -0.02f, -0.35f};
constexpr float kAdjustedZNear = 0.1f;  // 0x3DCCCCCD
// Automatic arm placement for `fov` starting from the game's own values: unchanged
// below 75 degrees, eased toward kWideArm up to 125, kWideArm above. `lowerZNear`
// tells whether z-near drops (from 75 degrees on).
inline Arm AutoArm(float fov, Arm base, bool& lowerZNear) {
    fov = ClampFov(fov);
    lowerZNear = fov >= 75.0f;
    if (fov > 125.0f) return {kWideArm.left, kWideArm.forward, base.up};
    if (fov < 75.0f) return base;
    const float t = (fov - 75.0f) / 50.0f;
    return {base.left + (kWideArm.left - base.left) * t, base.forward + (kWideArm.forward - base.forward) * t, base.up};
}

// --- runtime (src/hle/prime_fov.cpp) ---------------------------------------
// Config.toml [prime] fov_enabled (off by default), fov, fov_culling_off,
// fov_arm_adjust; the setters apply live and the caller saves them.
bool Enabled();
float Fov();
bool CullingAsked();
bool ArmAdjust();
float GameFov();  // the game's own FOV of the first camera seen, 0 until then
void SetEnabled(bool enabled);
void SetFov(float fov);
void SetCullingAsked(bool asked);
void SetArmAdjust(bool adjust);
// RetroAchievements on: the option stays off whatever the setting says (the
// game as shipped); the setting is kept for when achievements are turned off.
void SetBlocked(bool blocked);
bool Blocked();
// Game thread, once per game frame: writes the FOV (or restores the game's
// values once after the option goes off).
void Frame();

}  // namespace PrimeFov
