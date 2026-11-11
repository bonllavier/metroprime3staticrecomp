// RM3E01 first-person field of view (model and addresses: hle/prime_fov.h).
#if defined(MKW_GAME_PROFILE_RM3E01_00)
#include "hle/prime_fov.h"

#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <atomic>
#include <cstring>
#include <vector>

namespace PrimeFov {
namespace {
// Settings: written by the F1 overlay, read on the game thread.
std::atomic<bool> g_loaded{false}, g_enabled{false}, g_cullingAsked{false}, g_armAdjust{true}, g_blocked{false};
std::atomic<float> g_fov{kDefaultFov};
// What the hooks read on every call: set by Frame() and by the setters.
std::atomic<bool> g_active{false}, g_cullingBypassed{false};

// Game thread only: the game's own values, put back when the option goes off.
struct SavedParams { uint32_t address; uint32_t fov, target; };
std::vector<SavedParams> g_savedParams;
uint32_t g_armAddress = 0, g_zNearAddress = 0;
uint32_t g_armSaved[3]{}, g_zNearSaved = 0;
bool g_armTouched = false, g_zNearTouched = false, g_applied = false;
float g_gameFov = 0;  // the first camera's own FOV, for the overlay

void Load() {
    if (g_loaded.exchange(true)) return;
    const auto& config = RuntimeConfigFile::Get();
    g_enabled.store(config.primeFovEnabled.value_or(false));
    g_fov.store(ClampFov(config.primeFov.value_or(kDefaultFov)));
    g_cullingAsked.store(config.primeFovCullingOff.value_or(false));
    g_armAdjust.store(config.primeFovArmAdjust.value_or(true));
}

bool Valid(uint32_t address, size_t length = 4) { return address != 0 && Memory::Contains(address, length); }
uint32_t Deref(uint32_t address) { return Valid(address) ? Memory::Read32(address) : 0; }
float ReadFloat(uint32_t address) {
    const uint32_t bits = Memory::Read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
void WriteFloat(uint32_t address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    Memory::Write32(address, bits);
}

uint32_t ObjectList() { return Deref(Deref(kStateManager) + 0x1010u); }
// The camera manager's active-id field (u16), and the block behind it.
uint32_t CameraManager() {
    const uint32_t base = Deref(Deref(kStateManager + 0x10u) + 0xCu);
    return base ? base + 0x16u : 0;
}
uint32_t CameraParams(uint32_t objectList, uint16_t id) {
    if (!objectList) return 0;
    const uint32_t camera = Deref(objectList + ((uint32_t(id) & 0x7FFu) << 3) + 4u);
    if (!Valid(camera + kCameraCinematic)) return 0;
    // A cinematic owns this camera: leave its FOV to the game.
    const uint32_t cinematic = Deref(camera + kCameraCinematic);
    if (Valid(cinematic + kCinematicFlags, 1) && (Memory::Read8(cinematic + kCinematicFlags) & kCinematicFlag) != 0)
        return 0;
    const uint32_t params = Deref(camera + kCameraParams);
    return Valid(params + kParamsFov, 8) ? params : 0;
}

void ApplyToCamera(uint32_t objectList, uint16_t id, float fov) {
    const uint32_t params = CameraParams(objectList, id);
    if (!params) return;
    bool known = false;
    for (const auto& saved : g_savedParams) known = known || saved.address == params;
    if (!known) {
        g_savedParams.push_back({params, Memory::Read32(params + kParamsFov), Memory::Read32(params + kParamsTargetFov)});
        if (g_gameFov == 0) {
            g_gameFov = ReadFloat(params + kParamsTargetFov);
            RT_LOG(RT_TAG_RUNTIME) << "[prime-fov] camera " << (id & 0x7FFu) << " game fov=" << g_gameFov
                                   << " current=" << ReadFloat(params + kParamsFov) << std::endl;
        }
        if (g_savedParams.size() > 64) g_savedParams.erase(g_savedParams.begin());
    }
    WriteFloat(params + kParamsTargetFov, fov);
    WriteFloat(params + kParamsFov, fov);
}

void ApplyArm(uint32_t cameraManager, float fov) {
    const uint32_t arm = Deref(Deref(kTweakGun));
    const uint32_t perspective = cameraManager ? Deref(Deref(cameraManager + 2u) + 0x14u) : 0;
    if (Valid(arm + kArmOffset, 12)) {
        const uint32_t address = arm + kArmOffset;
        if (!g_armTouched || g_armAddress != address) {
            g_armAddress = address;
            for (int i = 0; i < 3; ++i) g_armSaved[i] = Memory::Read32(address + 4u * i);
            g_armTouched = true;
            RT_LOG(RT_TAG_RUNTIME) << "[prime-fov] arm left=" << ReadFloat(address) << " forward=" << ReadFloat(address + 4)
                                   << " up=" << ReadFloat(address + 8) << std::endl;
        }
        Arm base;
        std::memcpy(&base.left, &g_armSaved[0], 4);  // the game's values as floats
        std::memcpy(&base.forward, &g_armSaved[1], 4);
        std::memcpy(&base.up, &g_armSaved[2], 4);
        bool lowerZNear = false;
        const Arm placed = g_armAdjust.load() ? AutoArm(fov, base, lowerZNear) : base;
        WriteFloat(address, placed.left);
        WriteFloat(address + 4u, placed.forward);
        WriteFloat(address + 8u, placed.up);
        if (Valid(perspective + kZNearOffset)) {
            const uint32_t zNear = perspective + kZNearOffset;
            if (!g_zNearTouched || g_zNearAddress != zNear) {
                g_zNearAddress = zNear;
                g_zNearSaved = Memory::Read32(zNear);
                g_zNearTouched = true;
            }
            if (lowerZNear) WriteFloat(zNear, kAdjustedZNear);
            else Memory::Write32(zNear, g_zNearSaved);
        }
    }
}

// The option went off: the game's own values back where they still belong.
void Restore() {
    const uint32_t objectList = ObjectList();
    const uint32_t manager = CameraManager();
    if (manager && Valid(manager, 2)) {
        const uint16_t active = Memory::Read16(manager);
        for (const uint16_t id : {active, kForcedCamera}) {
            if (id == kNoCamera) continue;
            const uint32_t params = CameraParams(objectList, id);
            for (const auto& saved : g_savedParams)
                if (params && saved.address == params) {
                    Memory::Write32(params + kParamsFov, saved.fov);
                    Memory::Write32(params + kParamsTargetFov, saved.target);
                }
        }
    }
    const uint32_t arm = Deref(Deref(kTweakGun));
    if (g_armTouched && arm + kArmOffset == g_armAddress && Valid(g_armAddress, 12))
        for (int i = 0; i < 3; ++i) Memory::Write32(g_armAddress + 4u * i, g_armSaved[i]);
    if (g_zNearTouched && Valid(g_zNearAddress)) {
        const uint32_t perspective = manager ? Deref(Deref(manager + 2u) + 0x14u) : 0;
        if (perspective + kZNearOffset == g_zNearAddress) Memory::Write32(g_zNearAddress, g_zNearSaved);
    }
    g_savedParams.clear();
    g_armTouched = g_zNearTouched = false;
    g_applied = false;
    RT_LOG(RT_TAG_RUNTIME) << "[prime-fov] off: game values restored" << std::endl;
}
}  // namespace

bool Enabled() { Load(); return g_enabled.load(); }
float Fov() { Load(); return g_fov.load(); }
bool CullingAsked() { Load(); return g_cullingAsked.load(); }
bool ArmAdjust() { Load(); return g_armAdjust.load(); }
float GameFov() { return g_gameFov; }
void SetEnabled(bool enabled) { Load(); g_enabled.store(enabled); }
void SetFov(float fov) { Load(); g_fov.store(ClampFov(fov)); }
void SetCullingAsked(bool asked) { Load(); g_cullingAsked.store(asked); }
void SetArmAdjust(bool adjust) { Load(); g_armAdjust.store(adjust); }
void SetBlocked(bool blocked) { g_blocked.store(blocked); }
bool Blocked() { return g_blocked.load(); }

// Game thread, once per KPAD read (every game frame).
void Frame() {
    Load();
    const bool enabled = g_enabled.load() && !g_blocked.load();
    if (!enabled) {
        g_active.store(false);
        g_cullingBypassed.store(false);
        if (g_applied) Restore();
        return;
    }
    const float fov = g_fov.load();
    g_active.store(true);
    g_cullingBypassed.store(CullingBypassed(fov, g_cullingAsked.load()));
    const uint32_t manager = CameraManager();
    if (!Valid(manager, 2)) return;
    const uint16_t id = Memory::Read16(manager);
    if (id == kNoCamera) return;
    const uint32_t objectList = ObjectList();
    ApplyToCamera(objectList, id, fov);
    if ((id & 0x7FFu) != kForcedCamera) ApplyToCamera(objectList, kForcedCamera, fov);
    ApplyArm(manager, fov);
    if (!g_applied) {
        g_applied = true;
        RT_LOG(RT_TAG_RUNTIME) << "[prime-fov] on: fov=" << fov << " culling_bypassed=" << g_cullingBypassed.load()
                               << " arm_adjust=" << g_armAdjust.load() << std::endl;
    }
}

}  // namespace PrimeFov

// Runtime hooks (prepare_prime_runtime_hooks.py), game thread.
extern "C" bool PrimeFovSkipCameraSetFov(uint32_t returnAddress) {
    return PrimeFov::g_active.load(std::memory_order_relaxed) && PrimeFov::HeldSetFovCaller(returnAddress);
}
extern "C" bool PrimeFovSkipEasedFov() { return PrimeFov::g_active.load(std::memory_order_relaxed); }
extern "C" bool PrimeFovCullingBypassed() { return PrimeFov::g_cullingBypassed.load(std::memory_order_relaxed); }
extern "C" void PrimeFovOnCameraChange(uint32_t value) {
    if (!PrimeFov::g_active.load(std::memory_order_relaxed)) return;
    PrimeFov::ApplyToCamera(PrimeFov::ObjectList(), static_cast<uint16_t>(value), PrimeFov::g_fov.load());
}
#endif
