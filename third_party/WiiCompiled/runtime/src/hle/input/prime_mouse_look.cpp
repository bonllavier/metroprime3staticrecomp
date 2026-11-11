// Mouse look for RM3E01 (see hle/prime_mouse_look.h for the recovered game path).
#include "hle/prime_mouse_look.h"

#include "guest_abi/cpu_context.h"
#include "input_bindings.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <SDL3/SDL.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#if defined(MKW_GAME_PROFILE_RM3E01_00)
namespace PrimeMouseLook {
namespace {
// Small-data globals of RM3E01 (SDA1 0x806801C0): CPlayer::GetTweak 0x8015715C,
// 0x80157164 and 0x80162AD4 read them.
constexpr uint32_t kTweakPlayer = 0x8067D784u, kTweakPlayerControl = 0x8067D788u, kGameState = 0x8067DC0Cu;
constexpr uint32_t kTweakTurnSpeed = 0x1D0u, kTweakTurnSpeedAlt = 0x1D4u;
constexpr uint32_t kControlTweakAltTurn = 0x2864u, kGameStateSensitivity = 0x142Cu;
constexpr uint32_t kPlayerSurface = 0x30Cu, kPlayerMorphState = 0x358u, kPlayerOrbitState = 0x370u;
constexpr uint32_t kPlayerOrbitSubState = 0x730u, kPlayerFreeLookPitch = 0x780u, kPlayerPitchOverride = 0x3B40u;
constexpr uint32_t kPlayerAimMode = 0x3C18u, kPlayerTurnRamp = 0x3C20u;
// CPhysicsActor: 0x80137A58 derives the velocity as momentum / (mass * moment scale).
constexpr uint32_t kActorMass = 0x154u, kActorMomentScale = 0x158u, kActorMomentumZ = 0x174u, kActorRateZ = 0x18Cu,
                   kActorTorqueZ = 0x1BCu;
// 0x801799A8 zeroes the turn while +0x3AD4 runs (grapple targets) and turns through
// RotateToOR while this frame counter runs (the game's quick turn).
constexpr uint32_t kPlayerTurnLockTimer = 0x3AD4u, kQuickTurnFrames = 0x8067BDCCu;
// Control-hint flags (CPlayer +0x34EC..+0x34EF, defaults 0x38 at +0x34EC, re-applied
// by 0x8015FE50 when hints end; 0x8015FF30 copies a script control hint's flag
// word into them). Bit 5 of +0x34EC is set in normal play and cleared by the
// hint that is active inside the gunship, where the pointer drives Samus's arm
// instead of the view (captured 2026-10-25); mouse look stays off while clear.
constexpr uint32_t kPlayerControlHintFlags = 0x34ECu;
constexpr uint8_t kControlHintLookBit = 0x20u;
// CControlMapper (CPlayer +0x35C4): +0 command count (100), +4 one "enabled"
// byte per command; GetAnalogInput (0x80061BA8) returns 0 for a disabled
// command. Context interactions (keypads, levers, ...) disable the commands
// the game's own pointer look uses: 2/3 turn, 55..58 free-look pitch/yaw
// (captured at a keypad 2026-10-25). Mouse look follows the same table.
constexpr uint32_t kPlayerControlMapper = 0x35C4u, kMapperEnabledOffset = 4u;
constexpr uint32_t kCommandTurnLeft = 2u, kCommandTurnRight = 3u, kCommandPitchUp = 55u, kCommandPitchDown = 56u;

bool CommandEnabled(uint32_t player, uint32_t command) {
    const uint32_t address = player + kPlayerControlMapper + kMapperEnabledOffset + command;
    return Memory::Contains(address, 1) && Memory::Read8(address) != 0;
}
constexpr uint32_t kPlayerFreeLookCurrentPitch = 0x77Cu, kPlayerFreeLookVelocity = 0x3C1Cu;
constexpr uint32_t kTweakCameraPitchSpeed = 0x3BCu;  // deg/s, read by 0x802E6C4C for the camera's pitch leg
constexpr uint32_t kManagerDisplay = 0x38u, kDisplayCameraManager = 0x34u, kCameraManagerState = 0x30u,
                   kCameraStateFlags = 0x38u;

std::atomic<bool> g_loaded{false}, g_enabled{true}, g_invertY{false};
std::atomic<float> g_sensitivityDeg{kDefaultSensitivityDegPerPixel};
std::atomic<float> g_smoothing{0.0f};
float g_smoothDx = 0, g_smoothDy = 0;  // game thread only
float g_turnDx = 0;                    // game thread only: accepted by the turn hook, until the move hook
bool g_turnAccepted = false;           // game thread only
std::atomic<bool> g_focused{true}, g_capturing{false};
std::atomic<int64_t> g_activeUntilNs{0};
std::atomic<int64_t> g_eligibleUntilNs{0}, g_lastToggleNs{0};
std::atomic<float> g_debugFactor{0}, g_debugPitch{0}, g_debugSpeed{0};
std::atomic<bool> g_cameraExact{false};  // the pitch hook accepted mouse input this frame
std::mutex g_motionMutex;
float g_dx = 0, g_dy = 0;
std::mutex g_pitchMutex;
float g_pitch = 0;
#if defined(_WIN32)
// The capture keeps the OS cursor parked at the window centre: the game thread
// reads the cursor at every KPAD read (no event-pump delay) and parks it again.
std::atomic<HWND> g_hwnd{nullptr};
std::atomic<int32_t> g_centerX{0}, g_centerY{0};
POINT g_savedCursor{};
bool g_savedCursorValid = false;
#endif

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void EnsureLoaded() {
    if (g_loaded.exchange(true)) return;
    g_enabled.store(RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primeMouseLook, "WII_COMPILED_PRIME_MOUSE_LOOK", true));
    g_invertY.store(RuntimeConfigFile::Get().primeMouseLookInvertY.value_or(false));
    g_sensitivityDeg.store(std::clamp(RuntimeConfigFile::Get().primeMouseLookSensitivity.value_or(kDefaultSensitivityDegPerPixel),
                                      kMinSensitivityDegPerPixel, kMaxSensitivityDegPerPixel));
    g_smoothing.store(std::clamp(RuntimeConfigFile::Get().primeMouseLookSmoothing.value_or(0.0f), 0.0f, kMaxSmoothing));
}

bool Read32(uint32_t address, uint32_t& value) {
    if (!Memory::Contains(address, 4)) return false;
    value = Memory::Read32(address);
    return true;
}
bool ReadF32(uint32_t address, float& value) {
    if (!Memory::Contains(address, 4)) return false;
    value = Memory::ReadFloat32(address);
    return std::isfinite(value);
}

bool InCinematic(uint32_t stateManager) {
    uint32_t display = 0, cameras = 0, state = 0, flags = 0;
    if (!Read32(stateManager + kManagerDisplay, display) || !Read32(display + kDisplayCameraManager, cameras) ||
        !Read32(cameras + kCameraManagerState, state) || !Read32(state + kCameraStateFlags, flags))
        return true;  // unknown layout: do not inject
    return (flags >> 29 & 1u) != 0;
}

// The game-side conditions under which CPlayer turns with the pointer at all.
bool GameAllowsLook(uint32_t player, uint32_t stateManager) {
    uint32_t morph = 0, orbit = 0;
    if (!Read32(player + kPlayerMorphState, morph) || !Read32(player + kPlayerOrbitState, orbit)) return false;
    if (morph == 2 || morph == 3) return false;  // morph ball (0x8014B3F0)
    if (orbit == 1 || orbit == 3) return false;  // lock-on / orbit own the camera
    if (!Memory::Contains(player + kPlayerAimMode, 1)) return false;
    if ((Memory::Read8(player + kPlayerAimMode) & 0x80u) != 0) return false;  // alternate aim mode
    if (!Memory::Contains(player + kPlayerControlHintFlags, 1)) return false;
    if ((Memory::Read8(player + kPlayerControlHintFlags) & kControlHintLookBit) == 0) return false;  // gunship controls
    // Context interactions disable the pointer-look commands; so does mouse look.
    if (!CommandEnabled(player, kCommandTurnLeft) || !CommandEnabled(player, kCommandTurnRight)) return false;
    return !InCinematic(stateManager);
}

bool GateOpen(uint32_t player, uint32_t stateManager) {
    EnsureLoaded();
    if (!GameAllowsLook(player, stateManager)) return false;
    g_eligibleUntilNs.store(NowNs() + 300'000'000);  // the hint stays while this keeps being refreshed
    return g_enabled.load() && g_focused.load() && !InputBindings::InputBlocked();
}

void MarkActive() { g_activeUntilNs.store(NowNs() + int64_t(kActiveHoldSeconds * 1e9)); }

// Diagnostic (game thread): a compact snapshot of CPlayer/camera fields that
// may distinguish the gunship cockpit (arm follows the pointer) from normal
// play; logged when it changes, at most 4 lines a second.
void TraceStateSnapshot(uint32_t player, uint32_t stateManager, bool gate) {
    static std::string last;
    static int64_t lastNs = 0;
    static unsigned lines = 0;
    if (lines >= 400) return;
    const auto u32 = [&](uint32_t off) { uint32_t v = 0; Read32(player + off, v); return v; };
    const auto u8 = [&](uint32_t off) { return Memory::Contains(player + off, 1) ? unsigned(Memory::Read8(player + off)) : 0u; };
    uint32_t display = 0, cameras = 0, state = 0, flags = 0;
    if (Read32(stateManager + kManagerDisplay, display) && Read32(display + kDisplayCameraManager, cameras) &&
        Read32(cameras + kCameraManagerState, state))
        Read32(state + kCameraStateFlags, flags);
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "gate=%d morph=%u orbit=%u sub=%u aim=%08x f34EC=%08x b3B40=%u b6B4=%u camflags=%08x cmd2/3=%u%u cmd55/56=%u%u csi=%08x",
                  gate ? 1 : 0, u32(kPlayerMorphState), u32(kPlayerOrbitState), u32(kPlayerOrbitSubState), u32(kPlayerAimMode),
                  u32(0x34ECu), u8(kPlayerPitchOverride), u8(0x6B4u), flags,
                  CommandEnabled(player, kCommandTurnLeft) ? 1u : 0u, CommandEnabled(player, kCommandTurnRight) ? 1u : 0u,
                  CommandEnabled(player, kCommandPitchUp) ? 1u : 0u, CommandEnabled(player, kCommandPitchDown) ? 1u : 0u, u32(0x3E78u));
    if (buf == last) return;
    const int64_t now = NowNs();
    if (now - lastNs < 250'000'000) return;
    last = buf;
    lastNs = now;
    ++lines;
    RT_LOG(RT_TAG_RUNTIME) << "[prime-mouse-look] state " << buf << std::endl;
}

void DropMotion() {
    std::lock_guard lock(g_motionMutex);
    g_dx = g_dy = 0;
}

// Angular velocity for a turn factor of 1, as 0x801799A8 computes it.
float TurnSpeed(uint32_t player) {
    uint32_t tweakPtr = 0, tweak = 0, controlPtr = 0, control = 0, gameState = 0, altFlags = 0, sensitivity = 0, surface = 0;
    if (!Read32(kTweakPlayer, tweakPtr) || !Read32(tweakPtr, tweak) || !Read32(kTweakPlayerControl, controlPtr) ||
        !Read32(controlPtr, control) || !Read32(kGameState, gameState) || !Read32(control + kControlTweakAltTurn, altFlags) ||
        !Read32(gameState + kGameStateSensitivity, sensitivity) || !Read32(player + kPlayerSurface, surface))
        return 0.0f;
    float speed = 0, multiplier = 0;
    if (!ReadF32(tweak + ((altFlags & 1u) ? kTweakTurnSpeedAlt : kTweakTurnSpeed), speed) ||
        !ReadF32(tweak + SurfaceMultiplierOffset(sensitivity, surface), multiplier))
        return 0.0f;
    return speed * multiplier;
}

#if defined(_WIN32)
bool WindowCenter(HWND hwnd, POINT& center, RECT& screenRect) {
    RECT client{};
    if (!hwnd || !GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0) return false;
    POINT origin{0, 0};
    if (!ClientToScreen(hwnd, &origin)) return false;
    screenRect = {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
    center = {origin.x + client.right / 2, origin.y + client.bottom / 2};
    return true;
}
#endif
}  // namespace

bool Enabled() { EnsureLoaded(); return g_enabled.load(); }
void SetEnabled(bool enabled) { EnsureLoaded(); g_enabled.store(enabled); }
float SensitivityDegPerPixel() { EnsureLoaded(); return g_sensitivityDeg.load(); }
void SetSensitivityDegPerPixel(float degPerPixel) {
    EnsureLoaded();
    g_sensitivityDeg.store(std::clamp(degPerPixel, kMinSensitivityDegPerPixel, kMaxSensitivityDegPerPixel));
}
bool InvertY() { EnsureLoaded(); return g_invertY.load(); }
void SetInvertY(bool invert) { EnsureLoaded(); g_invertY.store(invert); }
float Smoothing() { EnsureLoaded(); return g_smoothing.load(); }
void SetSmoothing(float value) { EnsureLoaded(); g_smoothing.store(std::clamp(value, 0.0f, kMaxSmoothing)); }

void NoteWindow(SDL_Window* window) {
#if defined(_WIN32)
    if (!window) return;
    auto* hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd) g_hwnd.store(hwnd);
#else
    (void)window;
#endif
}
void NoteFocus(bool focused) {
    g_focused.store(focused);
    if (!focused) DropMotion();
}
float g_frameDx = 0, g_frameDy = 0;  // this frame's host motion, for the QA recorder
bool g_externalMotion = false;        // a replay owns the motion: host input is ignored
void AddMotion(float dxPixels, float dyPixels) {
    if (!std::isfinite(dxPixels) || !std::isfinite(dyPixels)) return;
    std::lock_guard lock(g_motionMutex);
    if (g_externalMotion) return;
    g_dx += dxPixels;
    g_dy += dyPixels;
    g_frameDx += dxPixels;
    g_frameDy += dyPixels;
}
void TakeFrameMotion(float& dx, float& dy) {
    std::lock_guard lock(g_motionMutex);
    dx = g_frameDx;
    dy = g_frameDy;
    g_frameDx = g_frameDy = 0;
}
void InjectMotion(float dxPixels, float dyPixels) {
    std::lock_guard lock(g_motionMutex);
    g_externalMotion = true;
    g_dx += dxPixels;
    g_dy += dyPixels;
}
bool Capturing() { return g_capturing.load(); }
bool Active() { return NowNs() < g_activeUntilNs.load(); }
bool Eligible() { return NowNs() < g_eligibleUntilNs.load(); }
int64_t LastToggleNs() { return g_lastToggleNs.load(); }
int64_t NowNanoseconds() { return NowNs(); }
void Toggle() {
    EnsureLoaded();
    const bool enabled = !g_enabled.load();
    g_enabled.store(enabled);
    RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeMouseLook, "mouse_look", enabled);
    if (!enabled) DropMotion();
    g_lastToggleNs.store(NowNs());
    RT_LOG(RT_TAG_RUNTIME) << "[prime-mouse-look] hotkey: " << (enabled ? "on" : "off") << std::endl;
}

// Host thread, once per presented frame: start or stop parking the cursor.
void ServiceHost() {
    EnsureLoaded();
#if defined(_WIN32)
    HWND hwnd = g_hwnd.load();
    const bool want = hwnd != nullptr && g_enabled.load() && g_focused.load() && !InputBindings::InputBlocked() && Active();
    POINT center{};
    RECT rect{};
    const bool haveCenter = want && WindowCenter(hwnd, center, rect);
    if (want && haveCenter) {
        if (!g_capturing.load()) {
            g_savedCursorValid = GetCursorPos(&g_savedCursor) != 0;
            SetCursorPos(center.x, center.y);
            DropMotion();
            g_capturing.store(true);
            static unsigned traces = 0;
            if (traces++ < 16) RT_LOG(RT_TAG_RUNTIME) << "[prime-mouse-look] capture=1" << std::endl;
        }
        g_centerX.store(center.x);
        g_centerY.store(center.y);
        ClipCursor(&rect);  // a fast swipe must not leave the window between two reads
        return;
    }
    if (g_capturing.exchange(false)) {
        ClipCursor(nullptr);
        // The host taking the mouse (F1/F10, focus lost, mouse look off) gives the
        // cursor back where it was. When the game closes the gate (Z free aim or
        // lock-on, Morph Ball, a menu) the pointer continues from the centre, where
        // the reticle was: the old position made the reticle jump (shots went there).
        const bool hostTookMouse = !g_enabled.load() || !g_focused.load() || InputBindings::InputBlocked();
        if (hostTookMouse && g_savedCursorValid) SetCursorPos(g_savedCursor.x, g_savedCursor.y);
        else if (hwnd != nullptr && WindowCenter(hwnd, center, rect)) SetCursorPos(center.x, center.y);
        DropMotion();
        static unsigned traces = 0;
        if (traces++ < 16)
            RT_LOG(RT_TAG_RUNTIME) << "[prime-mouse-look] capture=0 cursor=" << (hostTookMouse ? "restored" : "centre") << std::endl;
    }
#endif
}

// Game thread, at every KPAD read: the cursor moved since it was parked.
void SampleCursor() {
#if defined(_WIN32)
    if (!g_capturing.load()) return;
    POINT point{};
    if (!GetCursorPos(&point)) return;
    const int32_t cx = g_centerX.load(), cy = g_centerY.load();
    const int32_t dx = point.x - cx, dy = point.y - cy;
    if (dx == 0 && dy == 0) return;
    AddMotion(float(dx), float(dy));
    SetCursorPos(cx, cy);
#endif
}

Debug Snapshot() {
    Debug d;
    d.active = Active();
    d.capturing = g_capturing.load();
    d.factor = g_debugFactor.load();
    d.pitch = g_debugPitch.load();
    d.turnSpeed = g_debugSpeed.load();
    return d;
}
}  // namespace PrimeMouseLook

std::atomic<uint32_t> g_lastPlayer{0};
namespace PrimeMouseLook { uint32_t LastPlayer() { return g_lastPlayer.load(std::memory_order_relaxed); } }
extern "C" void PrimeMouseLookAfterTurn(CpuContext* ctx, uint32_t player, uint32_t input, uint32_t stateManager) {
    g_lastPlayer.store(player, std::memory_order_relaxed);
    using namespace PrimeMouseLook;
    if (!GateOpen(player, stateManager)) {
        g_debugFactor.store(0);
        return;
    }
    MarkActive();
    float dt = 0;
    if (!ReadF32(input, dt) || !(dt > 0.0f)) return;  // CFinalInput +0 is the frame time
    float dx = 0;
    {
        std::lock_guard lock(g_motionMutex);
        dx = g_dx;
        g_dx = 0;
    }
    dx = SmoothDelta(g_smoothDx, dx, g_smoothing.load());
    // Several callers can ask within one 0x801799A8 (0x8017A6FC runs first); the delta is taken once.
    g_turnDx += dx;
    g_turnAccepted = true;
    const float speed = TurnSpeed(player);
    g_debugSpeed.store(speed);
    if (!(speed > 0.0f)) {
        static unsigned warnings = 0;
        if (warnings++ < 4) RT_LOG(RT_TAG_RUNTIME) << "[prime-mouse-look] turn speed unavailable; no yaw this frame" << std::endl;
        return;
    }
    const float factor = TurnFactor(dx, SensitivityToRadPerPixel(g_sensitivityDeg.load()), dt, speed);
    g_debugFactor.store(factor);
    ctx->fpr[1].d = static_cast<double>(factor);
    // Skip the Basic-sensitivity start ramp: a mouse motion is already a rate.
    if (factor != 0.0f && Memory::Contains(player + kPlayerTurnRamp, 4))
        Memory::WriteFloat32(player + kPlayerTurnRamp, kTurnRampSaturated);
    static unsigned traces = 0;
    if (traces++ < 8)
        RT_LOG(RT_TAG_RUNTIME) << "[prime-mouse-look] turn dx=" << dx << " dt=" << dt << " speed=" << speed << " factor=" << factor << std::endl;
}

extern "C" void PrimeMouseLookAfterMove(CpuContext*, uint32_t player, uint32_t input, uint32_t) {
    using namespace PrimeMouseLook;
    if (!g_turnAccepted) return;
    g_turnAccepted = false;
    const float dx = g_turnDx;
    g_turnDx = 0;
    uint32_t orbit = 0, quickTurn = 0;
    float dt = 0, lockTimer = 0, mass = 0, momentScale = 0;
    if (!Read32(player + kPlayerOrbitState, orbit) || orbit != 0) return;  // only orbit 0 applies the game's torque
    if (!Read32(kQuickTurnFrames, quickTurn) || quickTurn != 0) return;
    if (!ReadF32(player + kPlayerTurnLockTimer, lockTimer) || lockTimer > 0.0f) return;
    if (!ReadF32(input, dt) || !ReadF32(player + kActorMass, mass) || !ReadF32(player + kActorMomentScale, momentScale)) return;
    if (!Memory::Contains(player + kActorTorqueZ, 4)) return;
    const float inertia = mass * momentScale;
    if (!(inertia > 0.0f)) return;
    const float rate = YawRate(dx, SensitivityToRadPerPixel(g_sensitivityDeg.load()), dt);
    Memory::WriteFloat32(player + kActorRateZ, rate);
    Memory::WriteFloat32(player + kActorMomentumZ, rate * inertia);
    Memory::WriteFloat32(player + kActorTorqueZ, 0.0f);
}

extern "C" void PrimeMouseLookAfterPitch(CpuContext*, uint32_t player, uint32_t, uint32_t stateManager) {
    using namespace PrimeMouseLook;
    float gamePitch = 0;
    const bool havePitch = ReadF32(player + kPlayerFreeLookPitch, gamePitch);
    g_cameraExact.store(false);
    const bool gateOpen = GateOpen(player, stateManager);
    TraceStateSnapshot(player, stateManager, gateOpen);
    if (!gateOpen) {
        // Follow the game's own pitch so a later activation continues from it.
        if (havePitch) {
            std::lock_guard lock(g_pitchMutex);
            g_pitch = std::clamp(gamePitch, kPitchMin, kPitchMax);
        }
        DropMotion();
        return;
    }
    MarkActive();
    uint32_t orbit = 0, orbitSub = 0;
    if (!Read32(player + kPlayerOrbitState, orbit) || !Read32(player + kPlayerOrbitSubState, orbitSub)) return;
    if (!Memory::Contains(player + kPlayerPitchOverride, 1) || Memory::Read8(player + kPlayerPitchOverride) == 1) return;
    if (orbit == 3 && orbitSub != 5) return;
    if (!CommandEnabled(player, kCommandPitchUp) || !CommandEnabled(player, kCommandPitchDown)) {
        DropMotion();
        return;
    }
    g_cameraExact.store(havePitch);
    float dy = 0;
    {
        std::lock_guard lock(g_motionMutex);
        dy = g_dy;
        g_dy = 0;
    }
    dy = SmoothDelta(g_smoothDy, dy, g_smoothing.load());
    float pitch = 0;
    {
        std::lock_guard lock(g_pitchMutex);
        g_pitch = PitchStep(g_pitch, dy, SensitivityToRadPerPixel(g_sensitivityDeg.load()), g_invertY.load());
        pitch = g_pitch;
    }
    g_debugPitch.store(pitch);
    if (havePitch) Memory::WriteFloat32(player + kPlayerFreeLookPitch, pitch);
}

extern "C" void PrimeMouseLookAfterFreeLookEase(CpuContext*, uint32_t player, uint32_t stateManager, uint32_t) {
    using namespace PrimeMouseLook;
    if (!GateOpen(player, stateManager)) return;
    float target = 0;
    if (!ReadF32(player + kPlayerFreeLookPitch, target)) return;
    if (!Memory::Contains(player + kPlayerFreeLookCurrentPitch, 4) || !Memory::Contains(player + kPlayerFreeLookVelocity, 4)) return;
    // The camera reads +0x77C: no easing between the mouse and the view.
    Memory::WriteFloat32(player + kPlayerFreeLookCurrentPitch, target);
    Memory::WriteFloat32(player + kPlayerFreeLookVelocity, 0.0f);
}

// The camera's pitch leg turns at most dt * tweak(+0x3BC) deg/s per frame
// toward the look direction (see the header). The player update (0x801507D0:
// pitch target 0x801522E0, then easing 0x80151F64) runs before the camera
// think in the same frame, so the pitch hook's decision gates this call.
extern "C" bool PrimeMouseLookCameraBegin(CpuContext*, uint32_t, uint32_t, uint32_t* saved) {
    using namespace PrimeMouseLook;
    if (!g_cameraExact.load() || !g_enabled.load()) return false;
    uint32_t tweakPtr = 0, tweak = 0;
    if (!Read32(kTweakPlayer, tweakPtr) || !Read32(tweakPtr, tweak)) return false;
    float speed = 0;
    if (!ReadF32(tweak + kTweakCameraPitchSpeed, speed)) return false;
    *saved = Memory::Read32(tweak + kTweakCameraPitchSpeed);
    Memory::WriteFloat32(tweak + kTweakCameraPitchSpeed, kCameraPitchSpeedDegPerSecond);
    static unsigned traces = 0;
    if (traces++ < 4)
        RT_LOG(RT_TAG_RUNTIME) << "[prime-mouse-look] camera pitch speed " << speed << " deg/s lifted for this frame" << std::endl;
    return true;
}

extern "C" void PrimeMouseLookCameraEnd(uint32_t saved) {
    using namespace PrimeMouseLook;
    uint32_t tweakPtr = 0, tweak = 0;
    if (!Read32(kTweakPlayer, tweakPtr) || !Read32(tweakPtr, tweak)) return;
    if (!Memory::Contains(tweak + kTweakCameraPitchSpeed, 4)) return;
    Memory::Write32(tweak + kTweakCameraPitchSpeed, saved);
}
#else
namespace PrimeMouseLook {
bool Enabled() { return false; }
void SetEnabled(bool) {}
float SensitivityDegPerPixel() { return kDefaultSensitivityDegPerPixel; }
void SetSensitivityDegPerPixel(float) {}
bool InvertY() { return false; }
void SetInvertY(bool) {}
float Smoothing() { return 0.0f; }
void SetSmoothing(float) {}
void NoteWindow(SDL_Window*) {}
void NoteFocus(bool) {}
void AddMotion(float, float) {}
void TakeFrameMotion(float& dx, float& dy) { dx = dy = 0; }
uint32_t LastPlayer() { return 0; }
void InjectMotion(float, float) {}
bool Capturing() { return false; }
void ServiceHost() {}
void SampleCursor() {}
bool Active() { return false; }
bool Eligible() { return false; }
void Toggle() {}
int64_t LastToggleNs() { return 0; }
int64_t NowNanoseconds() { return 0; }
Debug Snapshot() { return {}; }
}  // namespace PrimeMouseLook
extern "C" void PrimeMouseLookAfterTurn(CpuContext*, uint32_t, uint32_t, uint32_t) {}
extern "C" void PrimeMouseLookAfterPitch(CpuContext*, uint32_t, uint32_t, uint32_t) {}
extern "C" void PrimeMouseLookAfterMove(CpuContext*, uint32_t, uint32_t, uint32_t) {}
extern "C" void PrimeMouseLookAfterFreeLookEase(CpuContext*, uint32_t, uint32_t, uint32_t) {}
extern "C" bool PrimeMouseLookCameraBegin(CpuContext*, uint32_t, uint32_t, uint32_t*) { return false; }
extern "C" void PrimeMouseLookCameraEnd(uint32_t) {}
#endif
