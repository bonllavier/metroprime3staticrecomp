#pragma once
// Mouse look for RM3E01's first-person aiming (keyboard/mouse mode).
//
// On the Wii the pointer only moves Samus's arm; the camera turns when the
// pointer leaves a dead zone. The game computes that turn in CPlayer:
//   0x8017A4B4  turn factor in [-1, 1] from pointer X (CControlMapper commands
//               2/3 through the sensitivity splines); 0x801799A8 multiplies it
//               by the tweak turn speed (CTweakPlayer +0x1D0 / +0x1D4) and the
//               surface table (0x802E7058, by the in-game sensitivity option
//               CGameState +0x142C) and applies it as angular velocity
//               (CPhysicsActor::SetAngularVelocityOR). A ramp (CPlayer +0x3C20,
//               saturates at 0.5 s) attenuates the start of a turn in Basic.
//   0x801522E0  free-look pitch: CPlayer +0x780 (radians) from pointer Y
//               through the spline, then an optional smoothing filter.
//   0x8007F904  first-person camera: builds the look direction from the eased
//               pitch (+0x77C) and free-look yaw (+0x774), then turns its
//               transform toward it with a quaternion LookAt whose angle per
//               frame is capped at dt * CTweakPlayer +0x3BC (deg/s), scaled by
//               the mapper attenuation and (1 - CPlayer +0x6E4). The yaw leg's
//               cap is 360 degrees, so only the pitch lags behind the input.
//   0x801799A8  applies the factor as a torque, not a velocity: target rate =
//               factor * speed * surface (capped there), torque =
//               |target - current| / (speed * surface) * tweak +0x5C.. (0x802E6D20),
//               with damping when the factor is 0. With the tweaks of RM3E01
//               (2.5 rad/s, 14000, mass 200) that closes 47% of the error per frame,
//               and 0x80137F40 turns with the velocity from before the torque, so
//               yaw had a 143 deg/s cap, inertia and a frame of lag; pitch has none.
// With mouse look on, the pointer the game sees stays centred and post-hooks
// replace those results with mouse deltas: the factor keeps feeding the game's
// movement code (0x8017A208), the yaw rate is written after 0x801799A8 and the
// pitch is written directly. Nothing else of the game's input path changes.
// Gate (both hooks): first person (morph state +0x358 not 2/3), no orbit or
// lock-on (+0x370 not 1/3), no cinematic (CStateManager +0x38 -> display
// manager +0x34 -> camera manager +0x30 -> +0x38 bit 29), window focused and
// no host overlay. Outside the gate the cursor is absolute again.
#include <algorithm>
#include <cmath>
#include <cstdint>

struct CpuContext;
struct SDL_Window;

namespace PrimeMouseLook {

// --- pure model (unit tested) -------------------------------------------
constexpr float kPitchMin = -0.523599f;     // FLOAT_80682524: 30 degrees down
constexpr float kPitchMax = 0.698132f;      // FLOAT_80682528: 40 degrees up
constexpr float kTurnRampSaturated = 0.5f;  // FLOAT_806808C8: ramp timer at full speed
constexpr float kDefaultSensitivityDegPerPixel = 0.2f;
constexpr float kMinSensitivityDegPerPixel = 0.02f, kMaxSensitivityDegPerPixel = 1.0f;
constexpr float kDegToRad = 0.017453292f;
constexpr float kActiveHoldSeconds = 0.25f;  // pointer stays centred this long after the last accepted frame

// Turn factor so that the game's angular velocity (factor * speed) turns by
// -dx * radPerPixel over this frame. Mouse right is a clockwise turn, which the
// game applies as negative Z angular velocity.
inline float TurnFactor(float dxPixels, float radPerPixel, float dt, float turnSpeedRadPerSec) {
    if (!(dt > 0.0f) || !(turnSpeedRadPerSec > 0.0f) || !std::isfinite(dxPixels)) return 0.0f;
    const float omega = -dxPixels * radPerPixel / dt;
    return std::clamp(omega / turnSpeedRadPerSec, -1.0f, 1.0f);
}

// Yaw rate that turns by -dx * radPerPixel in this frame. 0x80137F40 advances the
// orientation with the first-order q + dt * 0.5 * (0, w) * q and AddMotionState
// normalizes it, which turns by 2 * atan(w * dt / 2), so the rate is 2 * tan(angle / 2) / dt.
constexpr float kMaxYawPerFrame = 2.3f;  // radians; keeps tan() far from its pole
inline float YawRate(float dxPixels, float radPerPixel, float dt) {
    if (!(dt > 0.0f) || !std::isfinite(dxPixels)) return 0.0f;
    const float angle = std::clamp(-dxPixels * radPerPixel, -kMaxYawPerFrame, kMaxYawPerFrame);
    return 2.0f * std::tan(angle * 0.5f) / dt;
}

// Mouse up (negative dy) looks up unless inverted; the game's own clamp applies.
inline float PitchStep(float pitch, float dyPixels, float radPerPixel, bool invertY) {
    if (!std::isfinite(dyPixels)) return pitch;
    const float delta = (invertY ? dyPixels : -dyPixels) * radPerPixel;
    return std::clamp(pitch + delta, kPitchMin, kPitchMax);
}

// Byte offset of the turn-speed surface multiplier in CTweakPlayer for the
// in-game sensitivity option (0 Basic, 1 Standard, 2 Advanced) and the
// player's surface restraint (0x802E7058).
inline uint32_t SurfaceMultiplierOffset(uint32_t sensitivity, uint32_t surface) {
    const uint32_t base = sensitivity == 1 ? 0xFCu : sensitivity == 2 ? 0x11Cu : 0xDCu;
    static constexpr uint32_t kBySurface[10] = {0, 4, 8, 0xC, 0x10, 0x18, 0x14, 0x1C, 0, 0};
    return base + (surface < 10 ? kBySurface[surface] : 0);
}

inline float SensitivityToRadPerPixel(float degPerPixel) {
    return std::clamp(degPerPixel, kMinSensitivityDegPerPixel, kMaxSensitivityDegPerPixel) * kDegToRad;
}

// Camera pitch speed (deg/s) substituted for CTweakPlayer +0x3BC while the
// camera think runs under mouse look: the game computes
// min(angle, dt * deg2rad * speed * attenuation) and this value makes the cap
// larger than any possible angle at any frame time, so the result is the exact
// angle (CameraPitchStep reproduces the game's float arithmetic).
constexpr float kCameraPitchSpeedDegPerSecond = 1.0e6f;
inline float CameraPitchStep(float angleRad, float dt, float speedDegPerSecond, float attenuation) {
    const float cap = dt * kDegToRad * speedDegPerSecond * attenuation;
    if (!(cap > 0.0f)) return 0.0f;
    return std::clamp(angleRad / cap, 0.0f, 1.0f) * cap;
}

// --- runtime API ----------------------------------------------------------
bool Enabled();
void SetEnabled(bool enabled);
float SensitivityDegPerPixel();
void SetSensitivityDegPerPixel(float degPerPixel);
bool InvertY();
void SetInvertY(bool invert);
// Optional smoothing of the mouse deltas (0 = none, up to kMaxSmoothing): the
// only smoothing left on the look path once the game's own are bypassed.
constexpr float kMaxSmoothing = 0.9f;
float Smoothing();
void SetSmoothing(float value);
// Exponential smoothing of a per-frame delta: `smoothed` keeps the carried part.
inline float SmoothDelta(float& smoothed, float delta, float smoothing) {
    smoothed = smoothing * smoothed + (1.0f - smoothing) * delta;
    return smoothed;
}

// Host (event) thread.
void NoteWindow(SDL_Window* window);
void NoteFocus(bool focused);
void AddMotion(float dxPixels, float dyPixels);  // relative motion while capturing
// QA replay (test build): motion of the current frame as the host produced it (cleared on read), and
// motion fed from a recording. While a replay injects, host motion is ignored.
void TakeFrameMotion(float& dx, float& dy);
// Guest address of CPlayer as last seen by the turn/pitch hooks (0 before the first one): QA anchors.
uint32_t LastPlayer();
void InjectMotion(float dxPixels, float dyPixels);
bool Capturing();                               // the OS cursor is parked at the window centre
void ServiceHost();                             // once per host frame: start/stop parking the cursor
// Game thread, at every KPAD read: turns the cursor's motion since it was
// parked into look input and parks it again (no event-pump delay).
void SampleCursor();

// Game thread: the player's first-person turn code accepted mouse input recently,
// so the pointer the game sees is centred.
bool Active();
// The game is in the state where mouse look applies (first person, no lock-on,
// no interaction, no cinematic), whether or not it is switched on: drives the
// on-screen hint. Toggle flips and saves [prime] mouse_look (hotkey).
bool Eligible();
void Toggle();
int64_t LastToggleNs();
int64_t NowNanoseconds();

struct Debug {
    bool active = false, capturing = false;
    float factor = 0, pitch = 0, turnSpeed = 0;
};
Debug Snapshot();

}  // namespace PrimeMouseLook

// Post-hooks installed by prepare_prime_runtime_hooks.py on the translated
// CPlayer functions (ctx->fpr[1] holds the turn factor on return).
extern "C" void PrimeMouseLookAfterTurn(CpuContext* ctx, uint32_t player, uint32_t input, uint32_t stateManager);
extern "C" void PrimeMouseLookAfterPitch(CpuContext* ctx, uint32_t player, uint32_t input, uint32_t stateManager);
// After 0x801799A8 (r3 player, r4 input, r5 manager), before the physics integrate:
// sets the yaw rate of the motion the turn hook accepted, and removes the game's
// torque about Z. The game's own overrides of the turn are left alone.
extern "C" void PrimeMouseLookAfterMove(CpuContext* ctx, uint32_t player, uint32_t input, uint32_t stateManager);
// 0x80151F64 eases CPlayer +0x77C (the pitch the camera uses) toward +0x780 with
// an accelerating velocity (+0x3C1C): that is the vertical lag. With mouse look
// the eased value is set to the target directly. Arguments: r3 player, r4 manager.
extern "C" void PrimeMouseLookAfterFreeLookEase(CpuContext* ctx, uint32_t player, uint32_t stateManager, uint32_t unused);
// 0x8007F904 (first-person camera think, r3 camera, r4 manager): Begin runs at
// entry and, when the pitch hook accepted mouse input this frame, replaces the
// camera pitch speed tweak (CTweakPlayer +0x3BC) with
// kCameraPitchSpeedDegPerSecond, returning true and the original bits in
// *saved; End restores them after the body ran.
extern "C" bool PrimeMouseLookCameraBegin(CpuContext* ctx, uint32_t camera, uint32_t stateManager, uint32_t* saved);
extern "C" void PrimeMouseLookCameraEnd(uint32_t saved);
