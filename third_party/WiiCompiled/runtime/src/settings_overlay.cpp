#include "settings_overlay.h"
#include "audio_backend.h"
#include "aurora_events.h"
#include "controller_button_names.h"
#include "controller_mapping_wizard.h"
#include "input_bindings.h"
#include "game_graphics_options.h"
#include "music_attenuation.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "wii_remote_input.h"

#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <iostream>
#include <string>
#include <string_view>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif
#if defined(MKW_PRIME_QA_TOOLS)
#include "host_performance.h"
#include "stack_profiler.h"
#endif
#include "hle/prime_key_bindings.h"
#include "hle/prime_remote_debug.h"
#include "hle/prime_mouse_look.h"
#include "hle/prime_key_icons.h"
#include "hle/prime_gamepad_runtime.h"
#include "hle/prime_fov.h"
#include "hle/prime_achievements.h"
#include "hle/prime_achievements_ui.h"
#include "hle/prime_materials.h"
#include "hle/prime_photo_mode.h"
bool PrimeFrontEndActive();  // prime_rso_runtime.cpp: title / file-select menus loaded
#include "prime_host_options.h"
#include "bug_report.h"
#include "screenshot.h"
#include "hle/prime_disc_picker.h"
#include "hle/prime_widescreen.h"
#include "update_check.h"
#include "post_effects.h"
#include "reshade_loader.h"
#include "prime_port_version.h"
#include "runtime_product.h"
#include <dolphin/gx/GXAurora.h>
#include "hle/gx_gpu_thread.h"

#include <dolphin/pad.h>

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled);
#include <dolphin/vi.h>
#include <aurora/aurora.h>
#if defined(__linux__)
#include <aurora/host_cores.h>
#endif
#include <aurora/gfx.h>

extern "C" int g_gxFrameCount;

// The graphics driver's frame limiter for this program, 0 = none (dxgi_frame_latency.cpp).
extern "C" uint32_t DriverFrameRateLimit();

// Defined in runtime/src/hle/audio/ax_mix.cpp. That header is private to the HLE
// directory and is not on this target's include path.
namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

namespace settings_overlay {
namespace {

const char* GraphicsApiDisplayName() {
    switch (aurora_get_backend()) {
    case BACKEND_D3D11: return "Direct3D 11";
    case BACKEND_D3D12: return "Direct3D 12";
    case BACKEND_METAL: return "Metal";
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_OPENGL: return "OpenGL";
    case BACKEND_OPENGLES: return "OpenGL ES";
    case BACKEND_WEBGPU: return "WebGPU";
    case BACKEND_NULL: return "Null";
    case BACKEND_AUTO: return "Automatic";
    }
    return "Unknown";
}

bool g_topBarVisible = false;
bool g_exitPromptOpen = false;
bool g_rumbleEnabled = RuntimeConfigFile::RumbleEnabled(true);
int g_controllerPort = 0;
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
int g_musicVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::MusicVolume(1.0f) * 100.0f));
int g_soundEffectsVolumePercent =
    static_cast<int>(std::lround(RuntimeConfigFile::SoundEffectsVolume(1.0f) * 100.0f));
int g_uiVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::UiVolume(1.0f) * 100.0f));
int g_voicesVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::VoicesVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
int32_t g_muteHotkey = RuntimeConfigFile::MuteHotkey(SDL_SCANCODE_BACKSLASH);
bool g_audioMixWorker = RuntimeConfigFile::AudioMixWorkerEnabled(true);
bool g_attenuateMusicWhenMediaPlays = RuntimeConfigFile::AttenuateMusicWhenMediaPlays(false);
int g_frameInterpolationMode = [] {
    const auto& choices = RuntimeConfigFile::kFrameInterpolationChoices;
    const auto found = std::find(choices.begin(), choices.end(), RuntimeConfigFile::FrameInterpolationFps(0));
    return found == choices.end() ? 0 : static_cast<int>(found - choices.begin());
}();
int g_displayMode = [] {
    const std::string mode = RuntimeConfigFile::DisplayMode("windowed");
    if (mode == "borderless") {
        return static_cast<int>(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    if (mode == "exclusive") {
        return static_cast<int>(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
    return static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED);
}();
#if defined(MKW_GAME_PROFILE_RM3E01_00)
// Preserve scene contents during first-use shader compilation in Prime QA.
bool g_skipUnreadyPipelines = RuntimeConfigFile::SkipUnreadyPipelines(false);
#else
bool g_skipUnreadyPipelines = RuntimeConfigFile::SkipUnreadyPipelines(true);
#endif
bool g_ubershader = RuntimeConfigFile::Ubershader(true);
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_showFps = RuntimeConfigFile::ShowFps(true);
bool g_forceAspect169 = RuntimeConfigFile::ForceAspect169Enabled();
#if defined(__APPLE__)
bool g_metalFxSpatialUpscaling = RuntimeConfigFile::MetalFxSpatialUpscaling(false);
#endif
uint32_t g_disabledPostProcessingPaths = RuntimeConfigFile::DisabledPostProcessingPaths(0);
std::array<int32_t, PAD_MAX_CONTROLLERS> g_configuredControllerIndices = [] {
    std::array<int32_t, PAD_MAX_CONTROLLERS> indices{};
    indices.fill(std::numeric_limits<int32_t>::min());
    return indices;
}();

using ControllerNames::kNativeButtons;
using ControllerNames::NativeButtonItem;
constexpr const auto& kControllerButtons = ControllerNames::kGameCubeButtons;

// Classic Controller Pro layout, indexed like kControllerButtons: the SNES-style
// diamond (A right, B bottom, X top, Y left) with digital bumpers driving the GC
// triggers and Z on Back/Select (the same home the NSO GC default gives it).
constexpr std::array<const char*, PAD_BUTTON_COUNT> kClassicProPreset = {
    "east",           // A
    "south",          // B
    "north",          // X
    "west",           // Y
    "start",          // Start
    "back",           // Z
    "left_shoulder",  // L
    "right_shoulder", // R
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

// PlayStation layout: bumpers drive the GC triggers, Z moves to Create/Share.
constexpr std::array<const char*, PAD_BUTTON_COUNT> kPlayStationPreset = {
    "south", "east", "west", "north", "start", "back",
    "left_shoulder", "right_shoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

struct ResolutionItem {
    const char* label;
    float scale;
};

using Clock = std::chrono::steady_clock;

constexpr auto kCursorAutoHideDelay = std::chrono::seconds(5);
Clock::time_point g_lastMouseActivity{Clock::now()};
bool g_cursorHidden = false;

constexpr std::array<std::string_view, 3> kDisplayModeConfigNames = {
    "windowed", "borderless", "exclusive",
};

uint64_t g_presentedFrame = 0;
std::atomic_bool g_strapInputAccepted = false;
std::atomic_uint64_t g_startupDismissFrame = UINT64_MAX;
constexpr uint64_t kStrapTransitionCoverFrames = 60;
std::atomic_bool g_bootShadersReady = false;
bool g_bootShaderNotice = false;
Clock::time_point g_bootShaderWaitStart{};
constexpr uint32_t kBootShaderNoticeThreshold = 100;
constexpr auto kBootShaderWaitLimit = std::chrono::minutes(3);

constexpr std::array<ResolutionItem, 8> kResolutions = {{
    {"Auto (window size)", 0.0f}, {"Native (1x)", 1.0f}, {"1.5x", 1.5f}, {"2x", 2.0f},
    {"3x", 3.0f}, {"4x", 4.0f}, {"6x", 6.0f}, {"8x", 8.0f},
}};

bool IgnoreDisplayLimitsForQa() {
#if defined(MKW_PRIME_QA_TOOLS)
    // Private QA: rates above the refresh of the test machine are measured with the presenter simulated.
    return std::getenv("WII_COMPILED_QA_INTERP_IGNORE_MONITOR") != nullptr;
#else
    return false;
#endif
}

// True when the monitor cannot show anything above the game's own 60 images per second. Its images
// would all be discarded, and trying costs the game real frames (58.8/s against 59.9 on such a PC).
bool MonitorStopsAt60() {
    if (IgnoreDisplayLimitsForQa()) return false;
    const float hz = aurora_get_display_refresh_rate();
    return hz > 0.f && hz < static_cast<float>(AURORA_FRAME_INTERPOLATION_MIN_FPS) + 2.f;
}

// The graphics driver's own frame limiter (NVIDIA "Max Frame Rate"), read at start-up; 0 = none.
// dxgi_frame_latency.cpp.
uint32_t DriverFrameCap() {
    return IgnoreDisplayLimitsForQa() ? 0u : DriverFrameRateLimit();
}

// A driver cap at or below 60 shows no inserted image at all, and its wait inside Present costs real
// frames, exactly like a 60 Hz monitor.
bool DriverStopsAt60() {
    const uint32_t cap = DriverFrameCap();
    return cap != 0 && cap < AURORA_FRAME_INTERPOLATION_MIN_FPS + 2u;
}

// What Aurora should aim for under the current F1 choice.
uint32_t FrameInterpolationTarget() {
    const uint32_t choice = RuntimeConfigFile::kFrameInterpolationChoices[static_cast<size_t>(g_frameInterpolationMode)];
    if (choice == 0 || MonitorStopsAt60() || DriverStopsAt60()) {
        return 0;
    }
    uint32_t target = choice;
    if (choice == RuntimeConfigFile::kFrameInterpolationUncapped) {
        target = AURORA_FRAME_INTERPOLATION_UNCAPPED;
    } else if (choice == RuntimeConfigFile::kFrameInterpolationMatchMonitor) {
        const uint32_t hz = static_cast<uint32_t>(std::lround(aurora_get_display_refresh_rate()));
        target = hz > AURORA_FRAME_INTERPOLATION_MIN_FPS ? std::min(hz, AURORA_FRAME_INTERPOLATION_MAX_FPS) : 0u;
    }
    // Images past the driver's cap are never shown; aiming above it only feeds the rate controller misses.
    const uint32_t cap = DriverFrameCap();
    if (target != 0 && cap != 0 && (target == AURORA_FRAME_INTERPOLATION_UNCAPPED || target > cap)) {
        target = std::min(cap, AURORA_FRAME_INTERPOLATION_MAX_FPS);
    }
    return target;
}

// The window may move to another monitor, or the desktop change its refresh rate. Once a second.
void FollowMonitorForFrameInterpolation() {
    if (g_frameInterpolationMode == 0) return;
    static auto nextCheck = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now < nextCheck) return;
    nextCheck = now + std::chrono::seconds(1);
    uint32_t target = FrameInterpolationTarget();
#if defined(MKW_PRIME_QA_TOOLS)
    // Private QA: exercises turning it on and off in play, surface reconfigure included.
    if (const char* period = std::getenv("WII_COMPILED_QA_INTERP_TOGGLE_SECONDS")) {
        static uint32_t seconds = 0;
        if ((++seconds / std::max(1, std::atoi(period))) % 2 != 0) target = 0;
    }
#endif
    if (target != aurora_get_frame_interpolation_fps()) {
        aurora_set_frame_interpolation_fps(target);
    }
}

bool IsHighResolutionScale(float scale) {
    return std::fabs(scale - 6.0f) < 0.001f || std::fabs(scale - 8.0f) < 0.001f;
}

bool IsHighFrameRateMode() {
    return g_frameInterpolationMode != 0;
}

void SetResolutionScale(float scale) {
    g_resolutionScale = scale;
    VISetFrameBufferScale(scale);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

void LimitResolutionForFrameRate() {
    if (IsHighFrameRateMode() && IsHighResolutionScale(g_resolutionScale)) {
        SetResolutionScale(4.0f);
    }
}

void ApplyFrameInterpolationMode() {
    LimitResolutionForFrameRate();
    aurora_set_frame_interpolation_fps(FrameInterpolationTarget());
    RuntimeConfigFile::SetFrameInterpolationFps(
        RuntimeConfigFile::kFrameInterpolationChoices[static_cast<size_t>(g_frameInterpolationMode)]);
    if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_EXCLUSIVE) {
        // Re-apply exclusive mode so the display refresh tracks the new target.
        aurora_set_display_mode(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
}

// Same order as RuntimeConfigFile::kFrameInterpolationChoices.
constexpr std::array<const char*, RuntimeConfigFile::kFrameInterpolationChoices.size()> kFrameInterpolationLabels{
    "Off (60 images/s)", "Match monitor", "120 images/s", "144 images/s", "165 images/s",
    "180 images/s",      "240 images/s",  "360 images/s", "Uncapped (as high as this PC sustains)",
};

void DrawFrameInterpolationCombo(const char* label) {
    int mode = g_frameInterpolationMode;
    if (ImGui::Combo(label, &mode, kFrameInterpolationLabels.data(), static_cast<int>(kFrameInterpolationLabels.size()))) {
        g_frameInterpolationMode = mode;
        ApplyFrameInterpolationMode();
    }
}

using ControllerNames::FindNativeButton;

uint32_t ConfiguredNativeButton(const NativeButtonItem& item, const std::string& token) {
    if (!PADIsAxisButton(item.nativeButton)) return item.nativeButton;
    const size_t separator = token.find('@');
    if (separator == std::string::npos) return item.nativeButton;
    uint32_t threshold = 0;
    const char* end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data() + separator + 1, end, threshold);
    if (parsed.ec != std::errc{} || parsed.ptr != end || threshold < 1 || threshold > 100)
        return item.nativeButton;
    return PADAxisButtonIdentity(item.nativeButton) | (threshold << 8);
}

struct ControllerBindingPair {
    std::string primary;
    std::string secondary;
};


// Config values hold up to two comma-separated button names ("dpad_up" or
// "dpad_up,left_shoulder"); pressing either one counts as the GC button.
ControllerBindingPair SplitControllerBinding(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {ControllerNames::TrimToken(value), {}};
    }
    return {ControllerNames::TrimToken(value.substr(0, comma)), ControllerNames::TrimToken(value.substr(comma + 1))};
}

using ControllerNames::NativeButtonForValue;

std::string NativeBindingConfig(uint32_t binding) {
    std::string value = NativeButtonForValue(binding).configName;
    if (PADIsAxisButton(binding)) value += '@' + std::to_string(PADAxisButtonThreshold(binding));
    return value;
}


void SetTopBarVisible(bool visible) {
    if (g_topBarVisible == visible) {
        return;
    }
    g_topBarVisible = visible;
}

void ApplyConfiguredMappings() {
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const int32_t controllerIndex = PADGetIndexForPort(port);
        if (controllerIndex == g_configuredControllerIndices[port]) {
            continue;
        }
        g_configuredControllerIndices[port] = controllerIndex;
        if (controllerIndex < 0) {
            continue;
        }
        // The [controller] bindings are positional and shared by every port, so
        // they describe whatever pad the user set them up with (usually an Xbox
        // layout: a = south). A Wii U Pro Controller has a fixed, known layout
        // (A on the east position) that aurora already maps by name; applying
        // the shared bindings on top swaps A/B and X/Y. (Wii Remotes with any
        // extension never reach the PAD layer: the game reads them through KPAD.)
        if (WiiRemoteInput::KindForPort(port) == WiiRemoteInput::Kind::WiiUPro) {
            continue;
        }

        uint32_t count = 0;
        if (PADGetButtonMappings(port, &count) == nullptr || count != PAD_BUTTON_COUNT) {
            continue;
        }
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            const auto& configured = RuntimeConfigFile::ControllerButton(i);
            if (!configured) {
                continue;
            }
            const ControllerBindingPair binding = SplitControllerBinding(*configured);
            if (const NativeButtonItem* native = FindNativeButton(binding.primary)) {
                PADSetButtonMapping(port, PADButtonMapping{ConfiguredNativeButton(*native, binding.primary), kControllerButtons[i].padButton});
            } else {
                RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                          << " button '" << binding.primary << "'" << std::endl;
            }
            uint32_t altNative = PAD_NATIVE_BUTTON_INVALID;
            if (!binding.secondary.empty()) {
                if (const NativeButtonItem* native = FindNativeButton(binding.secondary)) {
                    altNative = ConfiguredNativeButton(*native, binding.secondary);
                } else {
                    RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                              << " secondary button '" << binding.secondary << "'" << std::endl;
                }
            }
            PADSetAltButtonMapping(port, PADButtonMapping{altNative, kControllerButtons[i].padButton});
        }
    }
}

bool g_wiiRemotesEnabled = RuntimeConfigFile::WiiRemotesEnabled(true);
bool g_wiiContinuousScan = RuntimeConfigFile::WiiContinuousScanEnabled(false);

// Accelerometer readout and zero-point calibration for a bare remote / remote + Nunchuk.
void DrawWiiRemoteAccelerometer(uint32_t port) {
    ImGui::SeparatorText("Accelerometer");
    float sdlG[3] = {};
    float kpad[3] = {};
    if (WiiRemoteInput::ReadAccelDebug(port, sdlG, kpad)) {
        ImGui::Text("KPAD acc: x %+.2f  y %+.2f  z %+.2f g", kpad[0], kpad[1], kpad[2]);
        ImGui::TextDisabled("Flat, buttons up: (0, -1, 0). Sideways as a wheel: (1, 0, 0); z follows the turn.");
    } else {
        ImGui::TextDisabled("No accelerometer data yet.");
    }
    // Residual per-axis bias left after SDL's factory-calibrated decode, measured
    // here with the remote at rest.
    if (WiiRemoteInput::IsAccelCalibrating(port)) {
        ImGui::ProgressBar(WiiRemoteInput::AccelCalibrationProgress(), ImVec2(220.0f, 0.0f), "Hold still...");
    } else if (ImGui::Button("Calibrate (remote lying flat, buttons up)")) {
        WiiRemoteInput::StartAccelCalibration(port);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Put the remote down on a flat surface with the buttons facing up and do not touch it\n"
                          "for about two seconds. Corrects the steering offset of a remote held sideways.");
    }
    ImGui::SameLine();
    const auto stored = WiiRemoteInput::ReadAccelOffset(port);
    const std::array<double, 3> offset = stored.value_or(std::array<double, 3>{});
    const bool calibrated = offset[0] != 0.0 || offset[1] != 0.0 || offset[2] != 0.0;
    ImGui::BeginDisabled(!calibrated || WiiRemoteInput::IsAccelCalibrating(port));
    if (ImGui::Button("Clear calibration")) {
        WiiRemoteInput::ClearAccelCalibration(port);
    }
    ImGui::EndDisabled();
    if (const char* message = WiiRemoteInput::AccelCalibrationMessage(port)) {
        ImGui::TextWrapped("%s", message);
    } else if (calibrated) {
        ImGui::TextDisabled("Stored offset: x %+.3f  y %+.3f  z %+.3f g", offset[0], offset[1], offset[2]);
    } else {
        ImGui::TextDisabled("No trim stored.");
    }
}

// Wii Remotes (Bluetooth) menu: driver switch, pairing help, continuous scanning and the port's controller kind.
void DrawWiiRemoteSettings(uint32_t selectedGamePort) {
    if (!ImGui::BeginMenu("Wii Remotes (Bluetooth)")) {
        return;
    }
    if (ImGui::Checkbox("Use Wii Remotes / Wii U Pro Controllers", &g_wiiRemotesEnabled)) {
        RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Takes effect on the next launch. Turn this off if you use a Mayflash DolphinBar.");
    }
#if defined(__linux__)
    ImGui::TextDisabled("Pairing: system Bluetooth settings (or bluetoothctl) > add device, then press 1+2");
#else
    ImGui::TextDisabled("Pairing: Windows Settings > Bluetooth > Add device, then press 1+2");
#endif
    ImGui::TextDisabled("(or the red SYNC button) on the remote. Leave the PIN empty.");
    ImGui::TextDisabled("A remote that was paired before also needs to be turned on with 1+2/SYNC.");
    if (ImGui::Checkbox("Keep scanning for Wii Remotes (like Dolphin's Continuous Scanning)",
                        &g_wiiContinuousScan)) {
        RuntimeConfigFile::SetWiiContinuousScanEnabled(g_wiiContinuousScan);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("While no Wii controller is connected, re-check Bluetooth every 2 seconds so a\n"
                          "remote that dropped out (\"Communications with the controller have been\n"
                          "interrupted\") or was turned on after launch comes back by itself.");
    }
    // The driver hint is only read at launch, so a rescan after the user turned
    // the setting off would still re-enumerate Wii devices in this session.
    ImGui::BeginDisabled(!g_wiiRemotesEnabled);
    if (ImGui::Button("Rescan now")) {
        WiiRemoteInput::RescanNow();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (WiiRemoteInput::IsScanning()) {
        ImGui::TextDisabled("Scanning... (%u so far) - press 1+2 on the remote", WiiRemoteInput::ScanCount());
    } else {
        ImGui::TextDisabled("Not scanning");
    }
    ImGui::Separator();

    const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForPort(selectedGamePort);
    ImGui::Text("Port %u: %s", static_cast<unsigned>(selectedGamePort + 1), WiiRemoteInput::KindLabel(kind));
    if (kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        WiiRemoteInput::KpadSample sample;
        if (WiiRemoteInput::ReadKpadSample(selectedGamePort, sample)) {
            // WPAD_CL_BUTTON_* bits, in the game's own layout (no mapping involved).
            const auto held = [&](uint32_t bit, const char* on, const char* off) { return (sample.clHold & bit) ? on : off; };
            ImGui::Text("Classic: %s %s %s %s  %s %s  %s %s  %s %s  %s %s %s %s", held(0x0010, "A", "a"),
                        held(0x0040, "B", "b"), held(0x0008, "X", "x"), held(0x0020, "Y", "y"), held(0x2000, "L", "l"),
                        held(0x0200, "R", "r"), held(0x0080, "ZL", "zl"), held(0x0004, "ZR", "zr"),
                        held(0x0400, "PLUS", "plus"), held(0x1000, "MINUS", "minus"), held(0x0001, "UP", "up"),
                        held(0x4000, "DOWN", "down"), held(0x0002, "LEFT", "left"), held(0x8000, "RIGHT", "right"));
            ImGui::Text("Sticks: L %+.2f %+.2f (WPAD %+d %+d)  R %+.2f %+.2f (WPAD %+d %+d)", sample.clLStick[0],
                        sample.clLStick[1], static_cast<int>(sample.clLStickRaw[0]),
                        static_cast<int>(sample.clLStickRaw[1]), sample.clRStick[0], sample.clRStick[1],
                        static_cast<int>(sample.clRStickRaw[0]), static_cast<int>(sample.clRStickRaw[1]));
            ImGui::TextDisabled("Capitals = held. The game reads this Classic Controller through KPAD, as on the");
            ImGui::TextDisabled("console: its buttons mean what the game says they mean, no mapping applies.");
        }
    }
    if (kind == WiiRemoteInput::Kind::WiiUPro) {
        if (SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(selectedGamePort))) {
            // SDL's Wii driver posts the D-pad as joystick buttons 11-14 (the
            // SDL_GAMEPAD_BUTTON_DPAD_* values) while its default HIDAPI mapping
            // expects a hat, so SDL_GetGamepadButton never sees them; read the
            // joystick directly, like the fallback in aurora's PADRead does.
            SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
            const auto rawButton = [&](int index) {
                return joystick != nullptr && SDL_GetJoystickButton(joystick, index);
            };
            ImGui::Text("Raw D-pad: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_DPAD_UP) ? "UP" : "up",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? "DOWN" : "down",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? "LEFT" : "left",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? "RIGHT" : "right");
            ImGui::Text("Raw face buttons: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_EAST) ? "A" : "a",
                        rawButton(SDL_GAMEPAD_BUTTON_SOUTH) ? "B" : "b", rawButton(SDL_GAMEPAD_BUTTON_NORTH) ? "X" : "x",
                        rawButton(SDL_GAMEPAD_BUTTON_WEST) ? "Y" : "y");
            ImGui::Text("Raw ZL/ZR: %d / %d (pressed above 0)",
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            ImGui::TextDisabled("Capitals = held. If a button never turns to capitals while physically held,");
            ImGui::TextDisabled("that press is not reaching SDL at all (a driver-level issue, not a mapping one).");
            ImGui::TextDisabled("This pad uses Nintendo's own layout (a/b/x/y as labelled); the shared");
            ImGui::TextDisabled("button mapping above does not apply to it.");
        }
    }
    if (kind == WiiRemoteInput::Kind::Remote || kind == WiiRemoteInput::Kind::RemoteWithNunchuk ||
        kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        DrawWiiRemoteAccelerometer(selectedGamePort);
    }

    ImGui::EndMenu();
}

const char* KeyBindingName(int scancode) {
    switch (scancode) {
    case PAD_KEY_MOUSE_LEFT: return "Mouse left";
    case PAD_KEY_MOUSE_RIGHT: return "Mouse right";
    case PAD_KEY_MOUSE_MIDDLE: return "Mouse middle";
    case PAD_KEY_MOUSE_X1: return "Mouse side 1";
    case PAD_KEY_MOUSE_X2: return "Mouse side 2";
    case PAD_KEY_INVALID: return "Unmapped";
    default:
        return scancode >= 0 && scancode < SDL_SCANCODE_COUNT
            ? SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)) : "Unknown";
    }
}

enum class RebindKind { KeyboardButton, KeyboardAxis, Controller, MuteHotkey };
struct RebindState {
    bool active = false;
    bool openPopup = false;
    RebindKind kind{};
    uint32_t port = 0;
    uint16_t target = 0;
    bool secondary = false;
    SDL_JoystickID instance = 0;
    Clock::time_point deadline{};
    std::string label;
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    uint32_t mouse = 0;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<bool, SDL_GAMEPAD_AXIS_COUNT> axesReady{};
} g_rebind;

void BeginRebind(RebindKind kind, uint16_t target, const char* label, bool secondary = false) {
    g_rebind = {};
    g_rebind.active = true;
    g_rebind.openPopup = true;
    g_rebind.kind = kind;
    g_rebind.port = static_cast<uint32_t>(g_controllerPort);
    g_rebind.target = target;
    g_rebind.secondary = secondary;
    g_rebind.label = label;
    g_rebind.deadline = Clock::now() + std::chrono::seconds(10);
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    std::copy_n(keys, std::min(count, static_cast<int>(g_rebind.keys.size())), g_rebind.keys.begin());
    g_rebind.mouse = SDL_GetMouseState(nullptr, nullptr);
    const int index = PADGetIndexForPort(g_rebind.port);
    if (kind == RebindKind::Controller && index >= 0) {
        if (auto* pad = PADGetSDLGamepadForIndex(index)) {
            g_rebind.instance = SDL_GetGamepadID(pad);
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
                g_rebind.buttons[i] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i)
                g_rebind.axesReady[i] = std::abs(static_cast<int>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i)))) < 8000;
        }
    }
}

void CompleteRebind(uint32_t value) {
    const auto& capture = g_rebind;
    if (capture.kind == RebindKind::Controller) {
        const int index = PADGetIndexForPort(capture.port);
        auto* pad = index >= 0 ? PADGetSDLGamepadForIndex(index) : nullptr;
        if (pad == nullptr || SDL_GetGamepadID(pad) != capture.instance) {
            g_rebind.active = false;
            return;
        }
        if (capture.secondary) PADSetAltButtonMapping(capture.port, {value, capture.target});
        else PADSetButtonMapping(capture.port, {value, capture.target});
        uint32_t count = 0, altCount = 0;
        auto* primary = PADGetButtonMappings(capture.port, &count);
        auto* alternate = PADGetAltButtonMappings(capture.port, &altCount);
        uint32_t primaryValue = PAD_NATIVE_BUTTON_INVALID, alternateValue = PAD_NATIVE_BUTTON_INVALID;
        for (uint32_t i = 0; i < count; ++i)
            if (primary[i].padButton == capture.target) primaryValue = primary[i].nativeButton;
        for (uint32_t i = 0; i < altCount; ++i)
            if (alternate[i].padButton == capture.target) alternateValue = alternate[i].nativeButton;
        std::string config = NativeBindingConfig(primaryValue);
        if (alternateValue != PAD_NATIVE_BUTTON_INVALID) config += ',' + NativeBindingConfig(alternateValue);
        for (size_t i = 0; i < kControllerButtons.size(); ++i)
            if (kControllerButtons[i].padButton == capture.target) RuntimeConfigFile::SetControllerButton(i, config);
    } else if (capture.kind == RebindKind::MuteHotkey) {
        g_muteHotkey = static_cast<int32_t>(value);
        RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
        g_rebind.active = false;
        return;
    } else if (capture.kind == RebindKind::KeyboardButton) {
        PADSetKeyButtonBinding(capture.port, {static_cast<int32_t>(value), capture.target});
    } else {
        PADSetKeyAxisBinding(capture.port, {static_cast<int32_t>(value), capture.target, 1});
    }
    PADSerializeMappings();
    g_rebind.active = false;
}

void DrawRebindPrompt() {
    if (g_rebind.openPopup) {
        ImGui::OpenPopup("Rebind input");
        g_rebind.openPopup = false;
    }
    if (!ImGui::BeginPopupModal("Rebind input", &g_rebind.active, ImGuiWindowFlags_AlwaysAutoResize)) {
        g_rebind.active = false;
        return;
    }
    if (g_rebind.active) {
        ImGui::Text("Rebind: %s", g_rebind.label.c_str());
        ImGui::TextUnformatted(g_rebind.kind == RebindKind::Controller
            ? "Press a controller button, pull a trigger, or move a stick."
            : g_rebind.kind == RebindKind::MuteHotkey
                ? "Press a keyboard key."
                : "Press a keyboard key or click a mouse button.");
        ImGui::TextUnformatted("Release any held input first. Backspace or Delete clears the mapping.");
        ImGui::TextUnformatted("Escape can be bound. F10 is reserved for settings.");
        const float remaining = std::chrono::duration<float>(g_rebind.deadline - Clock::now()).count();
        ImGui::Text("Unmapped in %d seconds", std::max(0, static_cast<int>(std::ceil(remaining))));
        const bool clear = ImGui::Button("Clear mapping");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) g_rebind.active = false;
        // UI clicks must not become mouse bindings (buttons activate on release).
        const bool overControl = ImGui::IsAnyItemHovered();
        if (g_rebind.active && (clear || remaining <= 0.0f)) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind != RebindKind::Controller) {
            int count = 0;
            const bool* keys = SDL_GetKeyboardState(&count);
            for (int i = 1; i < std::min(count, static_cast<int>(SDL_SCANCODE_COUNT)) && g_rebind.active; ++i) {
                if (keys[i] && !g_rebind.keys[i] && i != SDL_SCANCODE_F10) CompleteRebind(i);
                g_rebind.keys[i] = keys[i];
            }
            const uint32_t mouse = SDL_GetMouseState(nullptr, nullptr);
            for (int i = 1; i <= 5 && g_rebind.active; ++i)
                if (!overControl && g_rebind.kind != RebindKind::MuteHotkey &&
                    (mouse & ~g_rebind.mouse & (1u << (i - 1))) != 0) CompleteRebind(static_cast<uint32_t>(-i - 1));
            g_rebind.mouse = mouse;
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind == RebindKind::Controller) {
            auto* pad = SDL_GetGamepadFromID(g_rebind.instance);
            if (pad != nullptr) {
                for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && g_rebind.active; ++i) {
                    const bool pressed = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
                    if (pressed && !g_rebind.buttons[i]) CompleteRebind(i);
                    g_rebind.buttons[i] = pressed;
                }
                for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT && g_rebind.active; ++i) {
                    const int value = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i));
                    if (std::abs(value) < 8000) g_rebind.axesReady[i] = true;
                    if (g_rebind.axesReady[i] && std::abs(value) >= 16384)
                        CompleteRebind(PADEncodeAxisButton(i, value < 0));
                }
            }
        }
    }
    if (!g_rebind.active) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawKeyBinding(const char* label, int scancode, RebindKind kind, uint16_t target,
                    float width = 220.0f) {
    const std::string caption = std::string(KeyBindingName(scancode)) + "##binding";
    if (ImGui::Button(caption.c_str(), ImVec2(width, 0.0f))) BeginRebind(kind, target, label);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);

}

bool DrawKeyboardSettings(uint32_t port) {
    uint32_t count = 0;
    auto* buttons = PADGetKeyButtonBindings(port, &count);
    bool enabled = buttons != nullptr;
    bool usePreset = false;
    if (ImGui::Checkbox("Keyboard and mouse", &enabled)) {
        PADSetKeyboardActive(port, enabled);
        PADSerializeMappings();
        buttons = PADGetKeyButtonBindings(port, &count);
        usePreset = enabled && std::all_of(buttons, buttons + count, [](const auto& binding) {
            return binding.scancode == PAD_KEY_INVALID;
        });
    }
    if (!enabled) return false;
    ImGui::TextDisabled("Replaces the gamepad on this port. F10 opens settings.");
    if (ImGui::Button("Use WASD + mouse preset") || usePreset) {
        const std::array<int, PAD_BUTTON_COUNT> keys = {
            PAD_KEY_MOUSE_LEFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_E, SDL_SCANCODE_Q,
            SDL_SCANCODE_RETURN, PAD_KEY_MOUSE_MIDDLE, SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
            SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        };
        for (size_t i = 0; i < keys.size(); ++i)
            PADSetKeyButtonBinding(port, {keys[i], kControllerButtons[i].padButton});
        const std::array<int, PAD_AXIS_COUNT> axes = {
            SDL_SCANCODE_D, SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_S,
            SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_I, SDL_SCANCODE_K,
            SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
        };
        uint32_t axisCount = 0;
        auto* mappings = PADGetKeyAxisBindings(port, &axisCount);
        for (uint32_t i = 0; i < axisCount; ++i)
            PADSetKeyAxisBinding(port, {axes[i], mappings[i].padAxis, 1});
        PADSerializeMappings();
    }
    ImGui::SeparatorText("Button mapping");
    for (uint32_t i = 0; i < count; ++i) {
        int key = buttons[i].scancode;
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(220.0f);
        DrawKeyBinding(PADGetButtonName(buttons[i].padButton), key, RebindKind::KeyboardButton, buttons[i].padButton);
        ImGui::PopID();
    }
    ImGui::SeparatorText("Stick and trigger mapping");
    uint32_t axisCount = 0;
    auto* axes = PADGetKeyAxisBindings(port, &axisCount);
    for (uint32_t i = 0; i < axisCount; ++i) {
        int key = axes[i].scancode;
        ImGui::PushID(static_cast<int>(count + i));
        const char* direction = PADGetAxisDirectionLabel(axes[i].padAxis);
        const std::string label = std::string(PADGetAxisName(axes[i].padAxis)) + " " +
                                  (direction != nullptr ? direction : "");
        ImGui::SetNextItemWidth(220.0f);
        DrawKeyBinding(label.c_str(), key, RebindKind::KeyboardAxis, axes[i].padAxis);
        ImGui::PopID();
    }
    return true;
}

// Controller settings menu: port selection, controller assignment and button mapping.
int ExpressionResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

void DrawExpressionSettings() {
    ImGui::SeparatorText("Expressions (Dolphin syntax)");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440.0f);
    ImGui::TextDisabled(
        "Optional. An expression overrides nothing: its result is combined with the "
        "button mapping above. Operators ! & | ^ and functions if, min, max, clamp, "
        "timer, toggle, hold, tap, pulse, smooth, deadzone behave as they do in Dolphin.");
    ImGui::PopTextWrapPos();

    static std::array<std::string, InputBindings::kControls.size()> errors;
    static std::array<std::string, InputBindings::kControls.size()> buffers;
    static std::string importStatus;
    static int loadedPort = -1;
    static bool reloadBuffers = true;
    const auto port = static_cast<uint32_t>(g_controllerPort);

    if (loadedPort != g_controllerPort || reloadBuffers) {
        for (size_t i = 0; i < buffers.size(); ++i) {
            buffers[i] = InputBindings::GetExpression(port, i);
        }
        errors.fill(std::string());
        loadedPort = g_controllerPort;
        reloadBuffers = false;
    }

    if (ImGui::Button("Import from Dolphin")) {
        const std::string path = InputBindings::DefaultDolphinConfigPath();
        std::string summary;
        std::string error;
        if (InputBindings::ImportDolphinConfig(path, g_controllerPort + 1, port, summary, error) < 0) {
            importStatus = error;
        } else {
            importStatus = summary;
            errors.fill(std::string());
            reloadBuffers = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reads [GCPad%d] from %%APPDATA%%\\Dolphin Emulator\\Config\\GCPadNew.ini,\n"
                          "or GCPadNew.ini next to the executable.", g_controllerPort + 1);
    }
    if (!importStatus.empty()) {
        ImGui::TextDisabled("%s", importStatus.c_str());
    }

    for (size_t i = 0; i < InputBindings::kControls.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 2000);
        std::string& text = buffers[i];
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::InputText(InputBindings::kControls[i].label, text.data(), text.capacity() + 1,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize,
                             ExpressionResizeCallback, &text)) {
            std::string error;
            errors[i] = InputBindings::SetExpression(port, i, text, error) ? std::string() : error;
        }
        if (InputBindings::IsActive(port, i)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "active");
        }
        if (!errors[i].empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "%s", errors[i].c_str());
        }
        ImGui::PopID();
    }
}

void DrawRumbleSettings() {
    ImGui::SeparatorText("Vibration");
    if (ImGui::Checkbox("Controller vibration", &g_rumbleEnabled)) {
        PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
        RuntimeConfigFile::SetRumbleEnabled(g_rumbleEnabled);
        if (!g_rumbleEnabled) {
            // Stop whatever is already running: the game will not send another
            // motor command until its own state machine decides to.
            constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
                PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD,
            };
            PADControlAllMotors(stopAll.data());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Applies to every port.");
    }
}

void DrawControllerSettings() {
    for (int port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const std::string label = "Port " + std::to_string(port + 1);
        ImGui::RadioButton(label.c_str(), &g_controllerPort, port);
        if (port + 1 < PAD_MAX_CONTROLLERS) {
            ImGui::SameLine();
        }
    }

    ImGui::Separator();
    const uint32_t selectedGamePort = static_cast<uint32_t>(g_controllerPort);
    if (DrawKeyboardSettings(selectedGamePort)) {
        return;
    }
    ImGui::Separator();
    const char* currentName = PADGetName(selectedGamePort);
    ImGui::Text("Assigned: %s", currentName != nullptr ? currentName : "None");
    if (ImGui::MenuItem("Unassign controller")) {
        PADClearPort(selectedGamePort);
        g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
    }
    ImGui::Separator();
    controller_mapping_wizard::DrawSetupList();
    DrawWiiRemoteSettings(selectedGamePort);
    const uint32_t controllerCount = PADCount();
    if (controllerCount == 0) {
        ImGui::TextDisabled("No controller connected");
        return;
    }

    if (ImGui::BeginMenu("Assign connected controller")) {
        for (uint32_t index = 0; index < controllerCount; ++index) {
            const char* name = PADGetNameForControllerIndex(index);
            std::string label = name != nullptr ? name : "Unknown controller";
            for (int port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
                if (PADGetIndexForPort(port) == static_cast<s32>(index)) {
                    label += " (Port " + std::to_string(port + 1) + ")";
                    break;
                }
            }
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::MenuItem(label.c_str())) {
                PADSetPortForIndex(index, selectedGamePort);
                g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
                ApplyConfiguredMappings();
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }

    if (WiiRemoteInput::IsRemoteChannel(selectedGamePort)) {
        ImGui::TextDisabled("The game reads this controller as a Wii Remote, so the button mapping does not apply.");
        DrawRumbleSettings();
        return;
    }

    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(static_cast<uint32_t>(g_controllerPort), &mappingCount);
    if (mappings == nullptr || mappingCount != PAD_BUTTON_COUNT) {
        ImGui::TextDisabled("Assign a controller to edit its buttons");
        return;
    }

    uint32_t altMappingCount = 0;
    PADButtonMapping* altMappings =
        PADGetAltButtonMappings(static_cast<uint32_t>(g_controllerPort), &altMappingCount);

    const auto writeBinding = [](size_t index, uint32_t primaryNative, uint32_t altNative) {
        std::string value = NativeBindingConfig(primaryNative);
        if (altNative != PAD_NATIVE_BUTTON_INVALID) {
            value += ',';
            value += NativeBindingConfig(altNative);
        }
        RuntimeConfigFile::SetControllerButton(index, value);
    };

    // Which rows show the second-binding combo without one being bound yet;
    // reset when the user switches ports so a stale "+" click doesn't linger.
    static std::array<bool, PAD_BUTTON_COUNT> altRowExpanded{};
    static int altRowExpandedPort = -1;
    if (altRowExpandedPort != g_controllerPort) {
        altRowExpandedPort = g_controllerPort;
        altRowExpanded.fill(false);
    }

    ImGui::SeparatorText("Presets");
    if (ImGui::Button("GameCube")) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        PADRestoreDefaultMapping(port);
        uint32_t restoredCount = 0;
        if (PADButtonMapping* restored = PADGetButtonMappings(port, &restoredCount)) {
            for (size_t i = 0; i < kControllerButtons.size(); ++i) {
                const auto it = std::find_if(restored, restored + restoredCount, [&](const PADButtonMapping& mapping) {
                    return mapping.padButton == kControllerButtons[i].padButton;
                });
                if (it != restored + restoredCount) {
                    RuntimeConfigFile::SetControllerButton(i, NativeButtonForValue(it->nativeButton).configName);
                }
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    }
    const auto applyPreset = [&](const std::array<const char*, PAD_BUTTON_COUNT>& preset) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            if (const NativeButtonItem* native = FindNativeButton(preset[i])) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                PADSetAltButtonMapping(port,
                                       PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                RuntimeConfigFile::SetControllerButton(i, preset[i]);
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    };

    ImGui::SameLine();
    if (ImGui::Button("Classic Controller Pro")) {
        applyPreset(kClassicProPreset);
    }
    ImGui::SameLine();
    if (ImGui::Button("PlayStation")) {
        applyPreset(kPlayStationPreset);
    }

    ImGui::SeparatorText("Button mapping");
    ImGui::TextDisabled("LT / L2 = left trigger. RT / R2 = right trigger.");
    ImGui::TextDisabled("LB / L1 = left shoulder. RB / R1 = right shoulder.");
    ImGui::TextDisabled("Click a binding, then press an input. No input for 10 seconds clears it.");
    const float bindingWidth = ImGui::CalcTextSize("Right shoulder (RB / R1)").x +
                               ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    for (size_t i = 0; i < kControllerButtons.size(); ++i) {
        auto mappingIt = std::find_if(mappings, mappings + mappingCount, [&](const PADButtonMapping& mapping) {
            return mapping.padButton == kControllerButtons[i].padButton;
        });
        if (mappingIt == mappings + mappingCount) {
            continue;
        }
        PADButtonMapping* altIt = nullptr;
        if (altMappings != nullptr && altMappingCount == PAD_BUTTON_COUNT) {
            const auto it = std::find_if(altMappings, altMappings + altMappingCount, [&](const PADButtonMapping& mapping) {
                return mapping.padButton == kControllerButtons[i].padButton;
            });
            if (it != altMappings + altMappingCount) {
                altIt = it;
            }
        }

        const NativeButtonItem& current = NativeButtonForValue(mappingIt->nativeButton);
        ImGui::PushID(static_cast<int>(i));
        const auto drawThreshold = [&](PADButtonMapping* mapping, bool secondary) {
            if (!PADIsAxisButton(mapping->nativeButton)) return;
            int threshold = static_cast<int>(PADAxisButtonThreshold(mapping->nativeButton));
            ImGui::SetNextItemWidth(bindingWidth);
            if (ImGui::SliderInt(secondary ? "##altThreshold" : "##primaryThreshold", &threshold,
                                 1, 100, "Threshold: %d%%", ImGuiSliderFlags_AlwaysClamp)) {
                const PADButtonMapping updated = {
                    PADAxisButtonIdentity(mapping->nativeButton) | (static_cast<uint32_t>(threshold) << 8),
                    mapping->padButton,
                };
                if (secondary) PADSetAltButtonMapping(selectedGamePort, updated);
                else PADSetButtonMapping(selectedGamePort, updated);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                writeBinding(i, mappingIt->nativeButton,
                             altIt != nullptr ? altIt->nativeButton : PAD_NATIVE_BUTTON_INVALID);
                PADSerializeMappings();
            }
        };
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(bindingWidth);
        const std::string primaryCaption = std::string(current.label) + "##primary";
        if (ImGui::Button(primaryCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
            BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label);
        }
        drawThreshold(mappingIt, false);
        ImGui::EndGroup();
        if (altIt != nullptr) {
            const bool altBound = altIt->nativeButton != PAD_NATIVE_BUTTON_INVALID;
            if (!altBound && !altRowExpanded[i]) {
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) {
                    altRowExpanded[i] = true;
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Add a second binding; pressing either one works");
                }
            } else {
                ImGui::SameLine();
                ImGui::TextUnformatted("or");
                ImGui::SameLine();
                ImGui::BeginGroup();
                const char* altLabel = altBound ? NativeButtonForValue(altIt->nativeButton).label : "None";
                ImGui::SetNextItemWidth(bindingWidth);
                const std::string altCaption = std::string(altLabel) + "##alt";
                if (ImGui::Button(altCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                drawThreshold(altIt, true);
                ImGui::EndGroup();
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(kControllerButtons[i].label);
        ImGui::PopID();
    }
    DrawExpressionSettings();
    DrawRumbleSettings();
}

void DrawAudioSettings() {
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderInt("Master", &g_audioVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_audioVolumePercent) / 100.0f;
        AudioBackend::Instance().SetMasterVolume(volume);
        RuntimeConfigFile::SetAudioVolume(volume);
    }
    if (ImGui::SliderInt("Music", &g_musicVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_musicVolumePercent) / 100.0f;
        MusicAttenuation::SetMusicVolume(volume);
        RuntimeConfigFile::SetMusicVolume(volume);
    }
    if (ImGui::SliderInt("Sound Effects", &g_soundEffectsVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_soundEffectsVolumePercent) / 100.0f;
        MusicAttenuation::SetSoundEffectsVolume(volume);
        RuntimeConfigFile::SetSoundEffectsVolume(volume);
    }
    if (ImGui::SliderInt("UI", &g_uiVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_uiVolumePercent) / 100.0f;
        MusicAttenuation::SetUiVolume(volume);
        RuntimeConfigFile::SetUiVolume(volume);
    }
    if (ImGui::SliderInt("Voices", &g_voicesVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_voicesVolumePercent) / 100.0f;
        MusicAttenuation::SetVoicesVolume(volume);
        RuntimeConfigFile::SetVoicesVolume(volume);
    }
    const float labelColumn = ImGui::GetCursorPosX() + ImGui::CalcItemWidth();
    if (ImGui::Checkbox("Mute", &g_audioMuted)) {
        AudioBackend::Instance().SetMuted(g_audioMuted);
        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
    }
    ImGui::SameLine();
    DrawKeyBinding("Mute shortcut", g_muteHotkey, RebindKind::MuteHotkey, 0,
                   std::max(60.0f, labelColumn - ImGui::GetCursorPosX()));
    ImGui::Separator();
    if (ImGui::Checkbox("Mix audio on a worker thread", &g_audioMixWorker)) {
        // Applies immediately: SetMixWorkerEnabled joins any in-flight mix
        // before switching, so the change never lands mid-frame.
        AxDspHle::SetMixWorkerEnabled(g_audioMixWorker);
        RuntimeConfigFile::SetAudioMixWorker(g_audioMixWorker);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Runs the AX/DSP voice mix off the game thread. Turn this off if you "
            "suspect an audio problem; the mix then runs inline as it used to.");
    }
    ImGui::Separator();
    if (ImGui::Checkbox("Mute game music while external media is playing",
                        &g_attenuateMusicWhenMediaPlays)) {
        MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
        RuntimeConfigFile::SetAttenuateMusicWhenMediaPlays(g_attenuateMusicWhenMediaPlays);
    }
#if defined(__APPLE__)
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Detects other apps with active audio output on macOS 14.2 or later. "
            "Apps that keep an output stream running silently may keep game music muted.");
    }
#endif
    if (g_attenuateMusicWhenMediaPlays) {
        if (MusicAttenuation::IsExternalMediaPlaying()) {
            ImGui::TextDisabled("External media is playing; game music is muted.");
        } else if (!MusicAttenuation::IsMediaControlInitializationComplete()) {
            ImGui::TextDisabled("Checking external audio...");
        } else if (!MusicAttenuation::IsMediaControlAvailable()) {
#if defined(__APPLE__)
            ImGui::TextDisabled("External audio detection unavailable (requires macOS 14.2 or later).");
#else
            ImGui::TextDisabled("Media controls are unavailable.");
#endif
        } else {
            ImGui::TextDisabled("No external media is currently playing.");
        }
    }
}

void DrawGraphicsSettings() {
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    if (ImGui::Checkbox("Force 16:9", &g_forceAspect169)) {
        SetMkwForceAspect169(g_forceAspect169);
        RuntimeConfigFile::SetForceAspect169(g_forceAspect169);
    }
    ImGui::TextDisabled("Keep a 16:9 image with black bars when the window has another shape.");
    ImGui::Separator();
    struct EffectFlag {
        const char* label;
        uint32_t flag;
    };
    static constexpr std::array<EffectFlag, 1> kEffectFlags = {{
        {"Disable bloom", 0x10u},
    }};

    for (const auto& effect : kEffectFlags) {
        bool disabled = (g_disabledPostProcessingPaths & effect.flag) != 0;
        if (ImGui::Checkbox(effect.label, &disabled)) {
            if (disabled) {
                g_disabledPostProcessingPaths |= effect.flag;
            } else {
                g_disabledPostProcessingPaths &= ~effect.flag;
            }
            RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
            RuntimeConfigFile::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
        }
    }
    ImGui::TextDisabled("Applied when the next scene renderer is created.");
    ImGui::Separator();
    static constexpr const char* kDisplayModes[] = {
        "Windowed",
        "Borderless fullscreen",
        "Exclusive fullscreen",
    };
    if (ImGui::Combo("Display mode", &g_displayMode, kDisplayModes, static_cast<int>(std::size(kDisplayModes)))) {
        const auto mode = static_cast<AuroraDisplayMode>(g_displayMode);
        aurora_set_display_mode(mode);
        const AuroraDisplayMode activeMode = aurora_get_display_mode();
        if (activeMode == mode) {
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        } else {
            g_displayMode = static_cast<int>(activeMode);
        }
    }
    if (g_displayMode == AURORA_DISPLAY_MODE_EXCLUSIVE) {
        ImGui::TextDisabled(
            "Requests the closest native-resolution display mode to the output frame "
            "rate (60 Hz, or the frame interpolation target).");
    }
    DrawFrameInterpolationCombo("Race frame interpolation (experimental)");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
    ImGui::TextDisabled("Frame interpolation is experimental, you might find visual artifacts");
    ImGui::PopTextWrapPos();
#if defined(__APPLE__)
    const bool metalFxSupported = aurora_is_metalfx_spatial_supported();
    ImGui::BeginDisabled(!metalFxSupported);
    if (ImGui::Checkbox("MetalFX spatial upscaling", &g_metalFxSpatialUpscaling)) {
        aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
        RuntimeConfigFile::SetMetalFxSpatialUpscaling(g_metalFxSpatialUpscaling);
    }
    ImGui::EndDisabled();
    switch (aurora_get_metalfx_status()) {
    case AURORA_METALFX_ACTIVE:
        ImGui::TextDisabled("Active: upscaling the game image before the overlay.");
        break;
    case AURORA_METALFX_NOT_UPSCALING:
        ImGui::TextDisabled("Choose a lower internal resolution to use MetalFX.");
        break;
    case AURORA_METALFX_UNSUPPORTED:
        ImGui::TextDisabled("Requires macOS 13+, Metal, and a MetalFX-capable GPU.");
        break;
    case AURORA_METALFX_ERROR:
        ImGui::TextDisabled("Unavailable after a renderer error; toggle off and on to retry.");
        break;
    case AURORA_METALFX_DISABLED:
        if (metalFxSupported) {
            ImGui::TextDisabled("Render below output resolution for sharper lower-cost output.");
        } else {
            ImGui::TextDisabled("MetalFX spatial upscaling is unavailable on this device.");
        }
        break;
    }
#endif
    if (ImGui::Checkbox("Disable copy filter", &g_disableCopyFilter)) {
        aurora_set_disable_copy_filter(g_disableCopyFilter);
        RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
    }
    if (ImGui::Checkbox("Skip draws while shaders compile", &g_skipUnreadyPipelines)) {
        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
    }
    if (ImGui::Checkbox("Show FPS", &g_showFps)) {
        RuntimeConfigFile::SetShowFps(g_showFps);
    }
    ImGui::Separator();
    ImGui::Text("Graphics API: %s", GraphicsApiDisplayName());
}

#if defined(MKW_PRIME_QA_TOOLS)
std::filesystem::path DrawDumpDirectory() {
    return RuntimeConfigFile::DrawCaptureDirectory();
}

// The next frame's draw list and image go to <capture dir>/capture_<time> ([paths] capture_dir).
void RequestDrawDump(const char* trigger, uint32_t frames = 1) {
    std::error_code error;
    std::filesystem::create_directories(DrawDumpDirectory(), error);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char name[64];
    std::strftime(name, sizeof(name), "capture_%Y%m%d_%H%M%S", &local);
    const auto base = DrawDumpDirectory() / name;
    if (frames > 1) aurora_request_draw_dump_burst(RuntimeConfigFile::PathToUtf8(base).c_str(), frames);
    else aurora_request_draw_dump(RuntimeConfigFile::PathToUtf8(base).c_str());
    RT_LOG(RT_TAG_CONFIG) << "[draw-dump] trigger=" << trigger << " base=" << RuntimeConfigFile::PathToUtf8(base)
                          << " frames=" << frames << std::endl;
}

// Off unless chosen: while visible, the panel samples host CPU on the game
// thread and enables producer profiling. F9 / the checkbox choice is kept.
bool g_showPerformanceDiagnostics=false;
// Length of an F8 recording: long enough to walk into a room and play through its loads.
constexpr double kF8ProfileSeconds = 120.0;

// F8 records without the F9 panel; this keeps the recording visible.
void DrawProfileRecordingNotice() {
#if defined(_WIN32) || defined(__linux__)
    static bool wasRunning=false;
    static std::chrono::steady_clock::time_point startedAt, finishedAt;
    const bool running=StackProfiler::IsRunning();
    const auto now=std::chrono::steady_clock::now();
    if(running && !wasRunning)startedAt=now;
    if(!running && wasRunning)finishedAt=now;
    wasRunning=running;
    const bool showSaved=!running && finishedAt.time_since_epoch().count()!=0 && now-finishedAt<std::chrono::seconds(4);
    if(!running && !showSaved)return;
    ImGui::SetNextWindowPos(ImVec2(12,12),ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    if(ImGui::Begin("##profile_recording",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|
                    ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing|
                    ImGuiWindowFlags_NoNav)) {
        if(running) {
            const float elapsed=std::chrono::duration<float>(now-startedAt).count();
            ImGui::TextColored(ImVec4(1,0.3f,0.3f,1),"Recording F8 profile: %.0f / %.0f s",elapsed,kF8ProfileSeconds);
        } else {
            ImGui::TextUnformatted("F8 profile saved to Logs/profiles");
        }
    }
    ImGui::End();
#endif
}

#endif

// "Screenshot saved" notice for three seconds after an F11 screenshot (drawn after the capture, so
// it never appears in the picture).
void DrawScreenshotNotice() {
    const std::string status = Screenshot::Status();
    if (status.empty() || Screenshot::SecondsSinceStatus() > 3.0) return;
    ImGui::SetNextWindowPos(ImVec2(12, 48), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (ImGui::Begin("##screenshot_notice", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav)) {
        ImGui::TextUnformatted(status.c_str());
    }
    ImGui::End();
}

#if defined(MKW_PRIME_QA_TOOLS)
void DrawPerformanceDiagnostics() {
    static bool persisted=[] {
        g_showPerformanceDiagnostics=RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primeShowPerformancePanel,
                                                                   "WII_COMPILED_PRIME_SHOW_PERFORMANCE",false);
        return g_showPerformanceDiagnostics;
    }();
    if(g_showPerformanceDiagnostics!=persisted) {
        persisted=g_showPerformanceDiagnostics;
        RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeShowPerformancePanel,"show_performance_panel",persisted);
    }
    aurora_set_producer_profiling(g_showPerformanceDiagnostics);
    if(!g_showPerformanceDiagnostics)return;
    static HostPerformance::Sampler sampler;
    const auto& host=sampler.Poll();
    static auto nextHostLog=std::chrono::steady_clock::now();
    const auto hostNow=std::chrono::steady_clock::now();
    if(host.ready && hostNow>=nextHostLog) {
        nextHostLog=hostNow+std::chrono::seconds(5);
        std::ostringstream hostLine;
        hostLine<<"[prime-host-load] process_cpu_percent="<<host.processPercent
            <<" busy_logical_processors="<<host.busyLogicalProcessors
            <<" logical_processors="<<host.logicalProcessors
            <<" gpu_valid="<<host.gpuReady<<" gpu_engine_max_percent="<<host.gpuPercent
            <<" gpu_engine="<<host.gpuEngine;
        for(size_t i=0;i<std::min(size_t(4),host.threads.size());++i)
            hostLine<<" tid="<<host.threads[i].id<<" cpu_one_logical_percent="<<host.threads[i].percent;
        RT_LOG(RT_TAG_CONFIG)<<hostLine.str()<<std::endl;
    }
    AuroraPresentTiming present{};aurora_get_present_timing(&present);
    AuroraProducerTiming producer{};aurora_get_producer_timing(&producer);
    ImGui::SetNextWindowPos(ImVec2(10,g_topBarVisible ? 58.0f : 36.0f),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.82f);
    if(ImGui::Begin("Game performance [F9]",&g_showPerformanceDiagnostics,
                    ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|
                    ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNav)) {
        ImGui::Text("FPS %.1f | frame %.2f ms | p95 %.2f ms",present.framesPerSecond,
                    present.averageFrameTimeMs,present.p95FrameTimeMs);
        if(host.ready) {
            ImGui::Text("Game CPU: %.1f%% total | equiv. %.2f/%u logical CPUs",
                        host.processPercent,host.busyLogicalProcessors,host.logicalProcessors);
            ImGui::TextDisabled("Per thread: 100%% = one busy logical processor");
            for(size_t i=0;i<std::min(size_t(4),host.threads.size());++i) {
                const auto& thread=host.threads[i];
                char label[80];std::snprintf(label,sizeof(label),"TID %u: %.1f%%",thread.id,thread.percent);
                ImGui::ProgressBar(float(std::clamp(thread.percent/100,0.0,1.0)),ImVec2(360,0),label);
            }
        } else ImGui::TextDisabled("CPU: waiting for the first sample...");
        if(host.gpuReady)ImGui::Text("Game GPU: %.1f%% | engine: %s",host.gpuPercent,host.gpuEngine.c_str());
#if defined(__linux__)
        else ImGui::TextDisabled("Game GPU: driver counter unavailable (the NVIDIA driver may not publish it) / starting");
        ImGui::TextDisabled("GPU: DRM driver usage; GPU time in ms is not instrumented");
#else
        else ImGui::TextDisabled("Game GPU: Windows counter unavailable / starting");
        ImGui::TextDisabled("GPU: Windows usage; GPU time in ms is not instrumented");
#endif
        const auto* stats=aurora_get_stats();
        if(stats) {
            ImGui::Text("Draws: %u | batched: %u | pending shaders: %u",
                        stats->drawCallCount,stats->mergedDrawCallCount,stats->queuedPipelines);
            ImGui::Text("Vertices %.1f KiB | indices %.1f KiB | textures %.1f KiB",
                        stats->lastVertSize/1024.0,stats->lastIndexSize/1024.0,stats->lastTextureUploadSize/1024.0);
        }
        ImGui::Separator();
        ImGui::TextUnformatted("CPU render stages: ms/s incl. waits (estimated)");
        if(producer.windowMs>0) {
            constexpr const char* names[]{"Assemble unbatched draws","Textures (incl. validation)",
                                         "Static textures (sub-stage)","Bindings", "Uniforms"};
            for(unsigned i=0;i<5;++i) {
                const double msPerSecond=1000*producer.estimatedCpuMs[i]/producer.windowMs;
                ImGui::Text("%s: %.1f ms/s",names[i],msPerSecond);
            }
            ImGui::TextDisabled("Sampling 1/64; window %.1f s. Inclusive times: do not add them up.",producer.windowMs/1000);
        } else ImGui::TextDisabled("Waiting for renderer samples (~5 s)...");
        ImGui::TextDisabled("F9 shows/hides. Host CPU/GPU refresh every second.");
        ImGui::Text("F8 stack profile %.0f s: %s",kF8ProfileSeconds,StackProfiler::Status().c_str());
    }
    ImGui::End();
}

#endif

#if defined(MKW_GAME_PROFILE_RM3E01_00)
bool g_showRemoteMotion=false;

// F6: what the keyboard/mouse virtual Wiimote and Nunchuk are sending to the
// game this frame (KPAD values), so twist, push/pull and gestures can be seen.
void DrawRemoteMotionPanel() {
    if (!g_showRemoteMotion) return;
    PrimeRemoteDebug::Snapshot m;
    if (!PrimeRemoteDebug::Read(m)) return;
    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    ImGui::SetNextWindowPos(ImVec2(kMargin, io.DisplaySize.y - kMargin), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Remote Motion", nullptr, kFlags)) { ImGui::End(); return; }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 kFrame = IM_COL32(150, 150, 150, 255), kRemote = IM_COL32(235, 235, 235, 255),
                kAccent = IM_COL32(255, 90, 60, 255), kIr = IM_COL32(120, 200, 255, 255),
                kGuide = IM_COL32(90, 90, 90, 255);
    constexpr float kPi = 3.14159265f;
    ImGui::TextUnformatted("Wii Remote / Nunchuk (keyboard)  F6");
    ImGui::Separator();

    // Twist: the remote seen from behind (the player's view). Positive roll
    // is counterclockwise, which is a negative angle with screen y down.
    ImGui::BeginGroup();
    ImGui::Text("Roll %+.0f degrees", m.roll * 180.0f / kPi);
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        constexpr float kSize = 110.0f;
        const ImVec2 c(origin.x + kSize * 0.5f, origin.y + kSize * 0.5f);
        draw->AddCircle(c, kSize * 0.46f, kFrame, 48, 1.0f);
        draw->AddLine(ImVec2(c.x - kSize * 0.46f, c.y), ImVec2(c.x + kSize * 0.46f, c.y), kGuide);
        const float a = -m.roll, ca = std::cos(a), sa = std::sin(a);
        auto at = [&](float x, float y) { return ImVec2(c.x + x * ca - y * sa, c.y + x * sa + y * ca); };
        draw->AddQuadFilled(at(-20, -32), at(20, -32), at(20, 32), at(-20, 32), kRemote);
        draw->AddCircleFilled(at(0, -18), 6.0f, kAccent); // the A button marks the top
        ImGui::Dummy(ImVec2(kSize, kSize));
    }
    ImGui::EndGroup();
    ImGui::SameLine(0.0f, 16.0f);

    // Push/pull: KPAD distance (1 = rest, smaller = pushed toward the screen).
    ImGui::BeginGroup();
    ImGui::Text("Depth %.2f", m.distance);
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        constexpr float kW = 22.0f, kH = 110.0f, kMin = 0.25f, kMax = 2.0f;
        draw->AddRect(origin, ImVec2(origin.x + kW, origin.y + kH), kFrame);
        const float t = std::clamp((m.distance - kMin) / (kMax - kMin), 0.0f, 1.0f);
        const float rest = origin.y + kH * (1.0f - (1.0f - kMin) / (kMax - kMin));
        const float y = origin.y + kH * (1.0f - t);
        draw->AddRectFilled(ImVec2(origin.x + 2, std::min(y, rest)), ImVec2(origin.x + kW - 2, std::max(y, rest)),
                            m.distance < 1.0f ? kAccent : kIr);
        draw->AddLine(ImVec2(origin.x - 4, rest), ImVec2(origin.x + kW + 4, rest), kRemote, 2.0f);
        draw->AddText(ImVec2(origin.x + kW + 6, origin.y), kGuide, "far");
        draw->AddText(ImVec2(origin.x + kW + 6, origin.y + kH - ImGui::GetTextLineHeight()), kGuide, "screen");
        ImGui::Dummy(ImVec2(kW + 64.0f, kH));
    }
    ImGui::EndGroup();
    ImGui::SameLine(0.0f, 16.0f);

    // IR camera: the sensor-bar marks as the remote sees them (1024 x 768).
    ImGui::BeginGroup();
    ImGui::Text("IR camera %s", m.irValid ? "(2 points)" : "(no points)");
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        constexpr float kW = 136.0f, kH = 102.0f;
        draw->AddRect(origin, ImVec2(origin.x + kW, origin.y + kH), kFrame);
        if (m.irValid) {
            const ImVec2 a(origin.x + m.irDots[0][0] / 1024.0f * kW, origin.y + m.irDots[0][1] / 768.0f * kH);
            const ImVec2 b(origin.x + m.irDots[1][0] / 1024.0f * kW, origin.y + m.irDots[1][1] / 768.0f * kH);
            draw->AddLine(a, b, IM_COL32(80, 120, 160, 255), 1.0f);
            draw->AddCircleFilled(a, 4.0f, kIr);
            draw->AddCircleFilled(b, 4.0f, kIr);
        }
        ImGui::Dummy(ImVec2(kW, kH));
        ImGui::Text("Horizon %+.2f %+.2f", m.horizon[0], m.horizon[1]);
    }
    ImGui::EndGroup();

    ImGui::Separator();
    // Accelerometers in g, KPAD axes; gravity at rest is y = -1. Red past 1.2 g
    // (a gesture rather than a tilt).
    auto accBars = [&](const char* name, const float (&acc)[3]) {
        ImGui::TextUnformatted(name);
        static const char* const kAxis[3] = {"x", "y", "z"};
        for (int i = 0; i < 3; ++i) {
            ImGui::SameLine(i == 0 ? 90.0f : 0.0f, i == 0 ? -1.0f : 14.0f);
            ImGui::TextUnformatted(kAxis[i]);
            ImGui::SameLine();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            constexpr float kW = 90.0f, kRange = 3.0f;
            const float h = ImGui::GetTextLineHeight();
            const float mid = origin.x + kW * 0.5f;
            draw->AddRect(origin, ImVec2(origin.x + kW, origin.y + h), kFrame);
            draw->AddLine(ImVec2(mid, origin.y), ImVec2(mid, origin.y + h), kGuide);
            const float x = mid + std::clamp(acc[i] / kRange, -1.0f, 1.0f) * kW * 0.5f;
            draw->AddRectFilled(ImVec2(std::min(mid, x), origin.y + 2), ImVec2(std::max(mid, x), origin.y + h - 2),
                                std::abs(acc[i]) > 1.2f ? kAccent : kIr);
            ImGui::Dummy(ImVec2(kW, h));
            ImGui::SameLine();
            ImGui::Text("%+.2f", acc[i]);
        }
    };
    accBars("Wiimote (g)", m.acc);
    accBars("Nunchuk (g)", m.nunchukAcc);

    ImGui::Separator();
    ImGui::Text("Stick %+.2f %+.2f", m.stick[0], m.stick[1]);
    ImGui::SameLine();
    if (m.pointerValid) ImGui::Text("   Pointer %+.2f %+.2f", m.pointer[0], m.pointer[1]);
    else ImGui::TextDisabled("   Pointer outside the window");
    if (m.mouseLook)
        ImGui::Text("Mouse: camera (yaw %+.2f, pitch %+.1f deg, turn speed %.2f)", m.mouseLookFactor,
                    m.mouseLookPitch * 57.29578f, m.mouseLookTurnSpeed);
    else
        ImGui::TextDisabled("Mouse: absolute pointer");
    // WPAD button bits.
    static const std::pair<uint32_t, const char*> kButtons[] = {
        {0x0800, "A"}, {0x0400, "B"}, {0x0010, "+"}, {0x1000, "-"}, {0x0200, "1"}, {0x0100, "2"},
        {0x8000, "HOME"}, {0x2000, "Z"}, {0x4000, "C"}, {0x0008, "Up"}, {0x0004, "Down"},
        {0x0001, "Left"}, {0x0002, "Right"}};
    for (size_t i = 0; i < std::size(kButtons); ++i) {
        if (i) ImGui::SameLine();
        const bool down = (m.hold & kButtons[i].first) != 0;
        ImGui::TextColored(down ? ImVec4(0.43f, 0.9f, 0.43f, 1) : ImVec4(0.45f, 0.45f, 0.45f, 1), "%s",
                           kButtons[i].second);
    }
    ImGui::End();
}
#endif

// Bottom-right notice for 30 s after a newer release is detected; F1 > Info keeps it.
void DrawUpdateNotice() {
    const UpdateCheck::Status status = UpdateCheck::Get();
    if (status.state != UpdateCheck::State::UpdateAvailable) return;
    static double firstSeen = -1.0;
    const double now = ImGui::GetTime();
    if (firstSeen < 0.0) firstSeen = now;
    if (now - firstSeen > 30.0) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, io.DisplaySize.y - 12.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin("##update_notice", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s v%s is available (you have v%s)", PRIME_PORT_NAME, status.latestVersion.c_str(), PRIME_PORT_VERSION);
        ImGui::TextDisabled("F1 > Info > Open download page");
    }
    ImGui::End();
}

void DrawFpsOverlay() {
    AuroraPresentTiming presentTiming{};
    aurora_get_present_timing(&presentTiming);
    if (!g_showFps) {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("FPS Overlay", nullptr, kFlags)) {
        if (presentTiming.sampleCount == 0) {
            ImGui::TextUnformatted("FPS: --");
        } else {
            // Present timing includes the additional frames produced by
            // interpolation, so this remains the actual displayed FPS.
            ImGui::Text("FPS: %.1f", presentTiming.framesPerSecond);
            if (aurora_get_ubershader()) {
                // Visible proof for recordings: draws per second that used the ubershader instead of waiting.
                static uint64_t lastDraws = 0;
                static double lastTime = 0.0;
                static double rate = 0.0;
                const double now = ImGui::GetTime();
                if (now - lastTime >= 0.5) {
                    const uint64_t draws = aurora_get_ubershader_draws();
                    rate = static_cast<double>(draws - lastDraws) / (now - lastTime);
                    lastDraws = draws;
                    lastTime = now;
                }
                if (rate > 0.5) {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 1.0f, 1.0f), "UBER: %.0f draws/s", rate);
                } else {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "UBER: on (idle)");
                }
            }
            // Replay-unsafe frames hold the presented cadence with duplicated
            // slots, so the counter alone reads 180 while the motion on screen
            // is 60 Hz. Surface the divergence instead of hiding it.
            if (presentTiming.effectiveFramesPerSecond <
                presentTiming.framesPerSecond * 0.95) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Motion: %.1f",
                                   presentTiming.effectiveFramesPerSecond);
            }
        }
    }
    ImGui::End();
}

void DrawShaderCompilationStatus() {
    const uint32_t queuedPipelines = aurora_get_queued_pipeline_count();
    if (queuedPipelines == 0) {
        return;
    }

    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(kMargin, top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.0f, 4.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Shader Compilation Status", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(0.85f);
        ImGui::Text("%u shader%s compiling", queuedPipelines, queuedPipelines == 1 ? "" : "s");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DrawStartupScreen() {
    if (!StartupScreenVisible()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("Wiicompiled Startup", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(1.25f);
        const char* kTitle = RuntimeProduct::Active().displayName.data();
        const ImVec2 titleSize = ImGui::CalcTextSize(kTitle);
        const float titleX = std::max(0.0f, (viewport->Size.x - titleSize.x) * 0.5f);
        const float startY = std::max(0.0f, (viewport->Size.y - titleSize.y) * 0.5f);
        ImGui::SetCursorPos(ImVec2(titleX, startY));
        ImGui::TextUnformatted(kTitle);
        if (g_bootShaderNotice && !g_bootShadersReady.load(std::memory_order_relaxed)) {
            ImGui::SetWindowFontScale(0.9f);
            char line[96];
            std::snprintf(line, sizeof(line), "Compiling shaders, please hold on: %u remaining",
                          aurora_get_queued_pipeline_count());
            const float lineX = std::max(0.0f, (viewport->Size.x - ImGui::CalcTextSize(line).x) * 0.5f);
            ImGui::SetCursorPos(ImVec2(lineX, startY + titleSize.y * 1.8f));
            ImGui::TextUnformatted(line);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void UpdateBootShaderState() {
    if (g_bootShadersReady.load(std::memory_order_relaxed)) {
        return;
    }
    const auto now = Clock::now();
    if (g_bootShaderWaitStart == Clock::time_point{}) {
        g_bootShaderWaitStart = now;
    }
    const uint32_t queued = aurora_get_queued_pipeline_count();
    g_bootShaderNotice |= queued > kBootShaderNoticeThreshold;
    if (queued == 0 || now - g_bootShaderWaitStart > kBootShaderWaitLimit) {
        g_bootShadersReady.store(true, std::memory_order_release);
    }
}

void DrawExitPrompt() {
    constexpr const char* kTitle = "Exit";
    if (g_exitPromptOpen && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    if (!ImGui::BeginPopupModal(kTitle, &g_exitPromptOpen, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Quit the game?");
    if (ImGui::Button("Exit", ImVec2(120.0f, 0.0f))) ExitForAuroraWindowClose();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) g_exitPromptOpen = false;
    if (!g_exitPromptOpen) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawTopBar() {
    if (!g_topBarVisible) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, 70));
    constexpr float kHintMargin = 10.0f;
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                 viewport->Pos.y + ImGui::GetFrameHeight() + kHintMargin),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("Settings input hint", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing)) {
        for (const char* line : {"Settings open - game controls disabled.",
                                 "Press F10 to return to the game."}) {
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(line).x) * 0.5f);
            ImGui::TextUnformatted(line);
        }
    }
    ImGui::End();
    if (!ImGui::BeginMainMenuBar()) return;

    ImGui::TextUnformatted(RuntimeProduct::Active().displayName.data());
    ImGui::Separator();
    const auto resolutionIt = std::find_if(kResolutions.begin(), kResolutions.end(), [](const ResolutionItem& item) {
        return std::fabs(item.scale - g_resolutionScale) < 0.001f;
    });
    const char* resolutionLabel = resolutionIt != kResolutions.end() ? resolutionIt->label : "Custom";
    const std::string resolutionMenuLabel = std::string("Resolution: ") + resolutionLabel;
    if (ImGui::BeginMenu(resolutionMenuLabel.c_str())) {
        for (const auto& resolution : kResolutions) {
            const bool selected = std::fabs(resolution.scale - g_resolutionScale) < 0.001f;
            const bool disabled = IsHighFrameRateMode() && IsHighResolutionScale(resolution.scale);
            ImGui::BeginDisabled(disabled);
            const bool clicked = ImGui::MenuItem(resolution.label, nullptr, selected);
            ImGui::EndDisabled();
            if (clicked) {
                SetResolutionScale(resolution.scale);
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Graphics")) {
        DrawGraphicsSettings();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Controller settings")) {
        DrawControllerSettings();
        // Nest capture under this menu so opening/closing the modal preserves
        // the settings popup and its current port and scroll position.
        DrawRebindPrompt();
        ImGui::EndMenu();
    }

    const std::string audioLabel = g_audioMuted
        ? "Audio: Muted"
        : "Audio: " + std::to_string(g_audioVolumePercent) + "%";
    // Keep the popup ID stable while the Master slider changes the visible
    // label. Without the ### suffix, ImGui treats every new percentage as a
    // different menu and closes the popup on the first drag update.
    const std::string audioMenuLabel = audioLabel + "###AudioSettingsMenu";
    if (ImGui::BeginMenu(audioMenuLabel.c_str())) {
        DrawAudioSettings();
        DrawRebindPrompt();
        ImGui::EndMenu();
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float hideWidth = ImGui::CalcTextSize("Hide (F10)").x + style.FramePadding.x * 2.0f;
    const float exitWidth = ImGui::CalcTextSize("X").x + style.FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                  ImGui::GetWindowWidth() - hideWidth - exitWidth - style.ItemSpacing.x - 8.0f));
    if (ImGui::MenuItem("Hide (F10)")) {
        SetTopBarVisible(false);
    }
    if (ImGui::MenuItem("X")) {
        g_exitPromptOpen = true;
    }
    ImGui::EndMainMenuBar();
}

bool IsToggleKey(const SDL_Event& event, SDL_Scancode code) {
    return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == code;
}

bool IsMouseActivity(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}

// Runs on the thread that pumps SDL events (the same one that calls Draw), so
// the SDL cursor calls are safe here.
// ---------------------------------------------------------------------------
// F1: Metroid Prime 3 options, grouped by area. Settings that the game or the
// renderer only read at startup are saved and flagged "al reiniciar".
// ---------------------------------------------------------------------------
bool g_primeOptionsOpen = false;
bool g_achievementsOpen = false;  // F2: RetroAchievements window
uint64_t g_photoStepsShown = 0;   // photo mode frame steps, for the indicator
// Hint geometry ([prime] mouse_look_hint_scale / _x / _y): size multiplier and
// the panel centre as a fraction of the window width, top edge as a fraction of
// its height. Adjusted from F1 > Controls; the panel shows while those sliders are used.
constexpr float kHintDefaultScale = 0.8f, kHintDefaultX = 0.5f, kHintDefaultY = 0.012f;
uint64_t g_hintEditFrame = 0;  // last overlay frame that drew the hint sliders
uint64_t g_hintFrame = 0;

bool g_primeHideCursor = RuntimeConfigFile::Get().primeHideCursor.value_or(true);

void RestartNote() { ImGui::SameLine(); ImGui::TextDisabled("(after restart)"); }

// F1 > Diagnostics: change the disc image by dialog or by a typed/pasted path
// (discussions #4 and #5). Either way the image is opened and checked first,
// and only a disc this executable runs is saved to [paths] iso_path.
std::string g_discImageStatus;
bool g_discImageStatusOk = false;
std::array<char, 1024> g_discImageTyped{};

void TryDiscImage(const std::filesystem::path& path) {
    const std::string problem = PrimeDiscPicker::CheckImage(path);
    if (!problem.empty()) {
        g_discImageStatus = problem;
        g_discImageStatusOk = false;
        return;
    }
    if (!RuntimeConfigFile::SetPrimeIsoPath(path)) {
        g_discImageStatus = "The image is valid but UserData/Config.toml could not be written.";
        g_discImageStatusOk = false;
        return;
    }
    g_discImageStatus = "Saved. It is used from the next start.";
    g_discImageStatusOk = true;
    RT_LOG(RT_TAG_CONFIG) << "[prime-iso-pick] F1 saved iso_path=" << RuntimeConfigFile::PathToUtf8(path) << std::endl;
}

void DiscImageSection() {
    RuntimeConfigFile::PrimeIsoSource source = RuntimeConfigFile::PrimeIsoSource::None;
    const auto current = RuntimeConfigFile::ResolvedPrimeIsoPath(&source);
    ImGui::TextUnformatted("Disc image");
    ImGui::TextWrapped("Current: %s", current.empty() ? "(none)" : RuntimeConfigFile::PathToUtf8(current).c_str());
    ImGui::TextDisabled("From: %s", RuntimeConfigFile::PrimeIsoSourceName(source));
    if (source == RuntimeConfigFile::PrimeIsoSource::Override) {
        ImGui::TextDisabled("This start used --iso or CORRUPTION3_ISO; a saved path applies when started without it.");
    }
    if (ImGui::Button("Select disc image...")) {
        if (const auto chosen = PrimeDiscPicker::Prompt({}); !chosen.empty()) TryDiscImage(chosen);
    }
    RestartNote();
    ImGui::SetNextItemWidth(-160.0f);
    const bool entered = ImGui::InputTextWithHint("##disc_path", "or paste the path to the .iso/.rvz/.wbfs file",
                                                  g_discImageTyped.data(), g_discImageTyped.size(),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Use this path") || entered) && g_discImageTyped[0] != '\0') {
        const std::string typed = PrimeDiscPath::Normalize(g_discImageTyped.data());
        if (PrimeDiscPath::IsShellLocation(typed)) {
            g_discImageStatus = PrimeDiscPath::ProblemText(PrimeDiscPath::Problem::ShellLocation);
            g_discImageStatusOk = false;
        } else {
            std::filesystem::path path = RuntimeConfigFile::PathFromUtf8(typed);
            if (path.is_relative()) {
                g_discImageStatus = "Type the full path, starting with the drive letter (under Proton or Wine, Z: is the Linux root).";
                g_discImageStatusOk = false;
            } else {
                TryDiscImage(path.lexically_normal());
            }
        }
    }
    ImGui::TextDisabled("Example: D:\\Games\\Metroid Prime 3 - Corruption (USA).rvz   or   Z:/home/you/Dumps/MP3.rvz");
    if (!g_discImageStatus.empty()) {
        const ImVec4 colour = g_discImageStatusOk ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.55f, 0.4f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        ImGui::TextWrapped("%s", g_discImageStatus.c_str());
        ImGui::PopStyleColor();
    }
}

void PrimeOptionCheckbox(const char* label, std::optional<bool> RuntimeUserConfig::*field, const char* key,
                         bool fallback, const char* help) {
    bool value = (RuntimeConfigFile::Get().*field).value_or(fallback);
    if (ImGui::Checkbox(label, &value)) RuntimeConfigFile::SetPrimeOption(field, key, value);
    RestartNote();
    if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
}

void DrawPrimeVideoOptions() {
    bool widescreen = RuntimeConfigFile::WidescreenEnabled(true);
    // 16:9 and 4:3 are the game's own modes; the ultrawide entries widen the
    // 16:9 view (Hor+, hle/prime_widescreen.h, GitHub issue #7).
    const PrimeWidescreen::Setting wide = PrimeWidescreen::CurrentSetting();
    int aspect = !widescreen ? 1
               : wide.mode == PrimeWidescreen::Mode::Window ? 2
               : wide.mode == PrimeWidescreen::Mode::Fixed && std::fabs(wide.aspect - PrimeWidescreen::kAspect21x9) < 0.001f ? 3
               : wide.mode == PrimeWidescreen::Mode::Fixed && std::fabs(wide.aspect - PrimeWidescreen::kAspect32x9) < 0.001f ? 4
               : wide.mode == PrimeWidescreen::Mode::Fixed ? 5 : 0;
    static constexpr const char* kAspects[] = {"Widescreen 16:9", "Standard 4:3", "Ultrawide: match the window / monitor",
                                               "Ultrawide 21:9", "Super ultrawide 32:9", "Custom (Config.toml)"};
    if (ImGui::Combo("Aspect ratio", &aspect, kAspects, wide.mode == PrimeWidescreen::Mode::Fixed && aspect == 5 ? 6 : 5)) {
        widescreen = aspect != 1;
        PrimeWidescreen::Setting next = wide;
        if (aspect == 0) next = PrimeWidescreen::Setting{};
        if (aspect == 2) next = PrimeWidescreen::Setting{PrimeWidescreen::Mode::Window, PrimeWidescreen::kBaseAspect};
        if (aspect == 3) next = PrimeWidescreen::Setting{PrimeWidescreen::Mode::Fixed, PrimeWidescreen::kAspect21x9};
        if (aspect == 4) next = PrimeWidescreen::Setting{PrimeWidescreen::Mode::Fixed, PrimeWidescreen::kAspect32x9};
        if (aspect != 1 && aspect != 5) {
            PrimeWidescreen::SetSetting(next);
            RuntimeConfigFile::SetPrimeAspectRatio(PrimeWidescreen::Format(next));
        }
        if (widescreen != RuntimeConfigFile::WidescreenEnabled(true)) {
            RuntimeConfigFile::SetWidescreen(widescreen);
            PrimeHostOptions::ApplyPresentation(widescreen);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Ultrawide shows more of the scene to the sides (Hor+): the vertical view and the HUD keep the\n"
                          "16:9 layout. 'Match the window' follows the window or monitor shape, so a 16:9 monitor\n"
                          "looks exactly like Widescreen 16:9. Pre-rendered videos stay 16:9.");
    }
    if (widescreen && PrimeWidescreen::ShownFactor() > 1.001f) {
        ImGui::TextDisabled("Image %.2f:1, the view is %.2fx wider than 16:9.", PrimeWidescreen::ShownAspect(),
                            PrimeWidescreen::ShownFactor());
    }
    ImGui::TextDisabled("16:9 and ultrawide change right away; switching to or from 4:3 re-lays out the game's image after a restart.");
    ImGui::Separator();

    struct Resolution { const char* label; float scale; };
    static constexpr Resolution kOptions[] = {
        {"Monitor / window native (recommended)", 0.0f},
        {"Original Wii 640x456 (1x)", 1.0f},
        {"1.5x", 1.5f}, {"2x (1280x912)", 2.0f}, {"3x (1920x1368)", 3.0f},
        {"4x (2560x1824)", 4.0f}, {"6x", 6.0f}, {"8x", 8.0f},
    };
    ImGui::TextUnformatted("Internal resolution");
    for (const auto& option : kOptions) {
        if (ImGui::RadioButton(option.label, std::fabs(g_resolutionScale - option.scale) < 0.001f))
            SetResolutionScale(option.scale);
    }
    u32 renderWidth = 0, renderHeight = 0;
    AuroraGetRenderSize(&renderWidth, &renderHeight);
    u32 surfaceWidth = 0, surfaceHeight = 0;
    AuroraGetSurfaceSize(&surfaceWidth, &surfaceHeight);
    ImGui::TextDisabled("Current render: %ux%u   Window: %ux%u", renderWidth, renderHeight,
                        surfaceWidth, surfaceHeight);
    if (ImGui::Button("Use the monitor resolution (borderless fullscreen)")) {
        SetResolutionScale(0.0f);
        aurora_set_display_mode(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    ImGui::Separator();

    g_displayMode = static_cast<int>(aurora_get_display_mode());
    static constexpr const char* kModes[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
    if (ImGui::Combo("Display mode", &g_displayMode, kModes, 3)) {
        aurora_set_display_mode(static_cast<AuroraDisplayMode>(g_displayMode));
        g_displayMode = static_cast<int>(aurora_get_display_mode());
        RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
    }

#if defined(__linux__)
    // Dawn has no Direct3D on Linux: main.cpp only accepts auto and vulkan there (d3d12 would be ignored).
    static constexpr const char* kApis[] = {"Automatic (Vulkan)", "Vulkan"};
    static constexpr const char* kApiNames[] = {"auto", "vulkan"};
    const std::string api = RuntimeConfigFile::GraphicsApi();
    int apiIndex = api == "vulkan" ? 1 : 0;
    if (ImGui::Combo("Graphics API", &apiIndex, kApis, 2)) RuntimeConfigFile::SetGraphicsApi(kApiNames[apiIndex]);
    RestartNote();
    ImGui::TextDisabled("In use: %s. Direct3D 12 is Windows only; Linux renders with Vulkan.", GraphicsApiDisplayName());
#else
    static constexpr const char* kApis[] = {"Automatic (Direct3D 12)", "Direct3D 12", "Vulkan"};
    static constexpr const char* kApiNames[] = {"auto", "d3d12", "vulkan"};
    const std::string api = RuntimeConfigFile::GraphicsApi();
    int apiIndex = api == "vulkan" ? 2 : api == "d3d12" ? 1 : 0;
    if (ImGui::Combo("Graphics API", &apiIndex, kApis, 3)) RuntimeConfigFile::SetGraphicsApi(kApiNames[apiIndex]);
    RestartNote();
    ImGui::TextDisabled("In use: %s. Vulkan: slightly lower latency, possible tearing.", GraphicsApiDisplayName());
#endif

    bool copyFilter = !g_disableCopyFilter;
    if (ImGui::Checkbox("Console copy filter (softens the image like the Wii)", &copyFilter)) {
        g_disableCopyFilter = !copyFilter;
        aurora_set_disable_copy_filter(g_disableCopyFilter);
        RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
    }
    if (ImGui::Checkbox("Show FPS", &g_showFps)) RuntimeConfigFile::SetShowFps(g_showFps);
    ImGui::Separator();

    // Antialiasing: part of every pipeline and of the framebuffers (startup only).
    static constexpr const char* kMsaaLabels[] = {"No antialiasing (1x)", "MSAA 2x", "MSAA 4x", "MSAA 8x"};
    static constexpr uint32_t kMsaaValues[] = {1, 2, 4, 8};
    const uint32_t configuredMsaa = RuntimeConfigFile::Msaa(1);
    int msaaIndex = 0;
    for (int i = 0; i < 4; ++i) if (kMsaaValues[i] == configuredMsaa) msaaIndex = i;
    if (ImGui::Combo("Antialiasing", &msaaIndex, kMsaaLabels, 4)) RuntimeConfigFile::SetMsaa(kMsaaValues[msaaIndex]);
    RestartNote();
    const uint32_t activeMsaa = aurora_get_msaa_samples();
    if (activeMsaa != configuredMsaa)
        ImGui::TextDisabled("In use now: %ux. Applies after restart.", activeMsaa);
    ImGui::TextDisabled("Each MSAA level has its own shaders: the first session with a new level");
    ImGui::TextDisabled("compiles them again (possible stutters) until the cache covers it.");

    // Anisotropic filtering: the game asks for 1x on nearly everything, so the
    // option forces the sampler value (live; Aurora caches samplers by descriptor).
    static constexpr const char* kAnisoLabels[] = {"As the game requests (1x)", "2x", "4x", "8x", "16x"};
    static constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
    const uint32_t configuredAniso = RuntimeConfigFile::AnisotropicFiltering(0);
    int anisoIndex = 0;
    for (int i = 0; i < 5; ++i) if (kAnisoValues[i] == configuredAniso) anisoIndex = i;
    if (ImGui::Combo("Anisotropic filtering", &anisoIndex, kAnisoLabels, 5)) {
        RuntimeConfigFile::SetAnisotropicFiltering(kAnisoValues[anisoIndex]);
        aurora_set_texture_anisotropy(static_cast<uint16_t>(kAnisoValues[anisoIndex]));
    }
    ImGui::TextDisabled("Only affects textures with mipmaps and trilinear filtering; applies immediately.");

    // Per-pixel lighting: the XF lighting formula moves from the vertex to the
    // fragment stage (same 8-bit rounding). Each mode has its own pipelines.
    bool perPixelLighting = RuntimeConfigFile::PerPixelLighting(false);
    if (ImGui::Checkbox("Per-pixel lighting", &perPixelLighting)) {
        RuntimeConfigFile::SetPerPixelLighting(perPixelLighting);
        aurora_set_per_pixel_lighting(perPixelLighting);
    }
    ImGui::TextDisabled("Dynamic lights, spotlights and smooth highlights instead of per-vertex. Applies immediately;");
    ImGui::TextDisabled("the first time it compiles new shaders (possible stutters until the cache fills).");

    // Enhanced metal: per-pixel environment reflection + Fresnel + specular on metallic materials.
    {
        bool enhancedMetal = RuntimeConfigFile::EnhancedMetal(true);
        float metalStrength = RuntimeConfigFile::EnhancedMetalStrength(0.30f);
        if (!perPixelLighting) ImGui::BeginDisabled();
        bool metalChanged = false;
        if (ImGui::Checkbox("Enhanced metal", &enhancedMetal)) {
            RuntimeConfigFile::SetEnhancedMetal(enhancedMetal);
            metalChanged = true;
        }
        if (enhancedMetal && ImGui::SliderFloat("Metal strength", &metalStrength, 0.0f, 2.0f, "%.2f")) {
            RuntimeConfigFile::SetEnhancedMetalStrength(metalStrength);
            metalChanged = true;
        }
        if (metalChanged) aurora_set_enhanced_metal(enhancedMetal, metalStrength);
        if (enhancedMetal) {
            const auto& vc = RuntimeConfigFile::Get();
            float rough = vc.enhancedMetalRoughness.value_or(0.25f);
            float metallic = vc.enhancedMetalMetallic.value_or(0.62f);
            float edge = vc.enhancedMetalEdge.value_or(0.50f);
            float detail = vc.enhancedMetalDetail.value_or(1.0f);
            bool paramsChanged = false;
            const auto metalKnob = [&](const char* label, float* value, std::optional<float> RuntimeUserConfig::*field,
                                       const char* key, const char* tip) {
                if (ImGui::SliderFloat(label, value, 0.0f, 1.0f, "%.2f")) {
                    RuntimeConfigFile::SetVideoFloat(field, key, *value);
                    paramsChanged = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            };
            metalKnob("Metal roughness", &rough, &RuntimeUserConfig::enhancedMetalRoughness, "enhanced_metal_roughness",
                      "Low = polished, small sharp highlights. High = brushed, wide soft highlights.");
            metalKnob("Metallic", &metallic, &RuntimeUserConfig::enhancedMetalMetallic, "enhanced_metal_metallic",
                      "How much the surface behaves like metal: coloured reflections and less painted look.");
            metalKnob("Edge sheen", &edge, &RuntimeUserConfig::enhancedMetalEdge, "enhanced_metal_edge",
                      "Extra reflection at grazing angles (Fresnel) on the game's own reflection map.");
            metalKnob("Surface detail", &detail, &RuntimeUserConfig::enhancedMetalDetail, "enhanced_metal_detail",
                      "Relief taken from the textures so panel lines and seams catch the light.");
            if (ImGui::Button("Reset metal")) {
                rough = 0.25f; metallic = 0.62f; edge = 0.50f; detail = 1.0f; metalStrength = 0.30f;
                RuntimeConfigFile::SetVideoFloat(&RuntimeUserConfig::enhancedMetalRoughness, "enhanced_metal_roughness", rough);
                RuntimeConfigFile::SetVideoFloat(&RuntimeUserConfig::enhancedMetalMetallic, "enhanced_metal_metallic", metallic);
                RuntimeConfigFile::SetVideoFloat(&RuntimeUserConfig::enhancedMetalEdge, "enhanced_metal_edge", edge);
                RuntimeConfigFile::SetVideoFloat(&RuntimeUserConfig::enhancedMetalDetail, "enhanced_metal_detail", detail);
                RuntimeConfigFile::SetEnhancedMetalStrength(metalStrength);
                aurora_set_enhanced_metal(enhancedMetal, metalStrength);
                paramsChanged = true;
            }
            if (paramsChanged) aurora_set_enhanced_metal_params(rough, metallic, edge, detail);
        }
        if (!perPixelLighting) ImGui::EndDisabled();
        ImGui::TextDisabled(perPixelLighting
                                ? "Sharper reflections, edge sheen and highlights on metal (Samus' suit, machinery)."
                                : "Needs per-pixel lighting.");
    }

    // Enhanced materials: per-texture classes (ship hulls) get physically based shading.
    {
        bool materials = RuntimeConfigFile::EnhancedMaterials(true);
        if (!perPixelLighting) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Enhanced materials", &materials)) {
            RuntimeConfigFile::SetEnhancedMaterials(materials);
            aurora_set_enhanced_materials(materials);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ship hulls and other identified surfaces get relief from their textures, sharp\n"
                              "highlights and reflections. Uses the original textures only. On by default.");
        if (!perPixelLighting) ImGui::EndDisabled();
    }

    // Modern lighting, level 1 (docs/iluminacion_motor.md): host-side options, all live. The master
    // switch turns every part off at once (the picture is then exactly as before).
    {
        const auto& vc = RuntimeConfigFile::Get();
        bool modernChanged = false;
        const auto option = [&](const char* label, std::optional<bool> RuntimeUserConfig::*field, const char* key,
                                bool fallback, const char* tip) {
            bool value = (vc.*field).value_or(fallback);
            if (ImGui::Checkbox(label, &value)) {
                RuntimeConfigFile::SetVideoOption(field, key, value);
                modernChanged = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            return value;
        };
        const auto slider = [&](const char* label, std::optional<float> RuntimeUserConfig::*field, const char* key,
                                float fallback, float lo, float hi, const char* format, const char* tip) {
            float value = (vc.*field).value_or(fallback);
            if (ImGui::SliderFloat(label, &value, lo, hi, format)) {
                RuntimeConfigFile::SetVideoFloat(field, key, value);
                modernChanged = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        ImGui::SeparatorText("Modern lighting");
        const bool modern = option("Modern lighting (all parts below)", &RuntimeUserConfig::modernLighting, "modern_lighting",
                                   true, "Turns every part of this section on or off at once. Off = the lighting looks\n"
                                         "exactly as before. Each part also has its own switch.");
        if (!modern) ImGui::BeginDisabled();
        ImGui::Indent();
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        option("Real engine lights", &RuntimeUserConfig::modernEngineLights, "engine_lights", true,
               "Highlights on walls and floors, light shafts and contact shadows use the lights the game\n"
               "itself placed in the room (and its shots and explosions), read every frame, instead of\n"
               "lights guessed from the characters. Lights baked into the walls only add highlights,\n"
               "never more light. Needs the environment material (F1 > Effects). Cost: low.");
        option("All dynamic lights on walls", &RuntimeUserConfig::modernExtraDynamicLights, "extra_dynamic_lights", true,
               "The game lights each piece of a wall with at most four shots or explosions and leaves the\n"
               "rest out. Pieces that reach that limit also get the overlapping lights it left out.\n"
               "Only visible in heavy fights. Cost: very low.");
        option("Smooth character lighting", &RuntimeUserConfig::modernActorLightFade, "actor_light_fade", true,
               "Characters and objects fade between the room lights the game picks for them instead of\n"
               "switching at once as they move (no sudden brightness jumps). Cost: none.");
#endif
        const bool contact = option("Contact shadows", &RuntimeUserConfig::modernContactShadows, "contact_shadows", true,
                                    "Short, sharp shadows where objects touch the floor and walls and on characters\n"
                                    "themselves, cast towards the strongest lights of the scene (screen-space: only\n"
                                    "what is on screen casts them). Needs antialiasing (MSAA) off. Cost: low to medium.");
        if (contact) {
            slider("Contact shadow strength", &RuntimeUserConfig::modernContactStrength, "contact_shadows_strength", 0.6f,
                   0.0f, 1.0f, "%.2f", "How dark the contact shadows get.");
            slider("Contact shadow length", &RuntimeUserConfig::modernContactLength, "contact_shadows_length", 0.6f, 0.1f,
                   2.0f, "%.2f m", "How far a contact shadow reaches (metres). Longer = more shadow, more artefacts.");
            option("Temporal stabilisation", &RuntimeUserConfig::modernTemporal, "temporal_stability", true,
                   "Blends the contact shadows with the previous frame's (moved with the camera), so they\n"
                   "do not shimmer. Off while frame interpolation is on. Cost: very low.");
        }
        option("Lightmap dithering", &RuntimeUserConfig::modernLightmapDither, "lightmap_dither", true,
               "Removes the bands of the baked light in dark gradients of walls and floors with an\n"
               "invisible fine noise. Needs the environment material (F1 > Effects). Cost: none.");
        const bool exposure = option("Automatic exposure", &RuntimeUserConfig::modernAutoExposure, "auto_exposure", true,
                                     "Like the eye or a camera: very dark rooms brighten a little and very bright places\n"
                                     "darken a little, over a second or two; bright areas roll off instead of clipping.\n"
                                     "The HUD and visor are not affected. Changes the overall look. Cost: very low.");
        if (exposure) {
            slider("Exposure range", &RuntimeUserConfig::modernExposureRange, "exposure_range", 0.6f, 0.0f, 2.0f, "%.2f EV",
                   "Largest brightening or darkening, in camera stops (EV). 0 = none.");
            slider("Exposure speed", &RuntimeUserConfig::modernExposureSpeed, "exposure_speed", 1.5f, 0.2f, 5.0f, "%.1f s",
                   "How long the adaptation takes (seconds).");
        }
        ImGui::Unindent();
        if (!modern) ImGui::EndDisabled();
        ImGui::TextDisabled("Applies immediately. Contact shadows and exposure need antialiasing (MSAA) off.");
        if (modernChanged) PostEffects::Apply();
    }

    // Load-time Super-xBR texture upscaling (CPU, no files). New uploads only.
    static constexpr const char* kUpscaleLabels[] = {"Off", "2x (Super-xBR)", "4x (Super-xBR)"};
    static constexpr uint32_t kUpscaleValues[] = {0, 2, 4};
    static constexpr const char* kUpscaleSizeLabels[] = {"up to 128x128", "up to 256x256", "up to 512x512", "up to 1024x1024"};
    static constexpr uint32_t kUpscaleSizeValues[] = {128, 256, 512, 1024};
    const uint32_t configuredUpscale = RuntimeConfigFile::TextureUpscale(0);
    const uint32_t configuredUpscaleSize = RuntimeConfigFile::TextureUpscaleMaxSize(256);
    int upscaleIndex = 0;
    for (int i = 0; i < 3; ++i) if (kUpscaleValues[i] == configuredUpscale) upscaleIndex = i;
    int upscaleSizeIndex = 1;
    for (int i = 0; i < 4; ++i) if (kUpscaleSizeValues[i] == configuredUpscaleSize) upscaleSizeIndex = i;
    bool upscaleChanged = ImGui::Combo("Texture upscaling (experimental)", &upscaleIndex, kUpscaleLabels, 3);
    upscaleChanged |= ImGui::Combo("Largest texture to upscale", &upscaleSizeIndex, kUpscaleSizeLabels, 4);
    static float upscaleSharpen = RuntimeConfigFile::TextureUpscaleSharpen(0.0f);
    if (ImGui::SliderFloat("Sharpen after upscaling", &upscaleSharpen, 0.0f, 1.0f, "%.2f")) upscaleChanged = true;
    if (upscaleChanged) {
        RuntimeConfigFile::SetTextureUpscale(kUpscaleValues[upscaleIndex]);
        RuntimeConfigFile::SetTextureUpscaleMaxSize(kUpscaleSizeValues[upscaleSizeIndex]);
        RuntimeConfigFile::SetTextureUpscaleSharpen(upscaleSharpen);
        aurora_set_texture_upscale(kUpscaleValues[upscaleIndex], kUpscaleSizeValues[upscaleSizeIndex], upscaleSharpen);
    }
    {
        const AuroraTextureUpscaleStats st = aurora_get_texture_upscale_stats();
        ImGui::TextDisabled("Super-xBR filter on a worker thread as each texture loads; no files, no AI. The original");
        ImGui::TextDisabled("texture shows until the upscaled one is ready. Restart to apply it to every texture. Known issue: magenta polygons in some rooms.");
        ImGui::TextDisabled("Upscaled: %llu, reused: %llu, skipped: %llu, pending: %llu, GPU cache: %llu MB, CPU: %.1f s",
                            static_cast<unsigned long long>(st.upscaled), static_cast<unsigned long long>(st.cacheHits),
                            static_cast<unsigned long long>(st.skipped), static_cast<unsigned long long>(st.pending),
                            static_cast<unsigned long long>(st.cacheBytes / (1024ull * 1024ull)),
                            static_cast<double>(st.cpuMicroseconds) / 1.0e6);
    }
    ImGui::Separator();

#if defined(MKW_GAME_PROFILE_RM3E01_00)
    // First-person field of view, as PrimeHack's view modifier (hle/prime_fov.h).
    bool fovEnabled = PrimeFov::Enabled();
    const bool fovBlocked = PrimeAchievements::Enabled();
    ImGui::BeginDisabled(fovBlocked);
    if (ImGui::Checkbox("Custom field of view", &fovEnabled)) {
        PrimeFov::SetEnabled(fovEnabled);
        RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeFovEnabled, "fov_enabled", fovEnabled);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Off: the game's own field of view, untouched. Cinematics always keep theirs.");
    if (fovBlocked)
        ImGui::TextDisabled("Off while RetroAchievements is on (F2): achievements are played with the game as shipped.");
    if (fovEnabled && !fovBlocked) {
        float fov = PrimeFov::Fov();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderFloat("Field of view", &fov, PrimeFov::kMinFov, PrimeFov::kMaxFov, "%.0f degrees")) {
            PrimeFov::SetFov(fov);
            RuntimeConfigFile::SetPrimeFloatOption(&RuntimeUserConfig::primeFov, "fov", PrimeFov::Fov());
        }
        if (PrimeFov::GameFov() > 0) ImGui::TextDisabled("The game's own value: %.0f degrees.", PrimeFov::GameFov());
        bool armAdjust = PrimeFov::ArmAdjust();
        if (ImGui::Checkbox("Move the arm cannon to suit the field of view", &armAdjust)) {
            PrimeFov::SetArmAdjust(armAdjust);
            RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeFovArmAdjust, "fov_arm_adjust", armAdjust);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("From 75 degrees the arm moves in and closer, up to 125 degrees, so it does not\n"
                              "look far away and stretched (as PrimeHack's automatic arm position).");
        bool cullingOff = PrimeFov::CullingAsked();
        if (ImGui::Checkbox("Always draw objects outside the original view", &cullingOff)) {
            PrimeFov::SetCullingAsked(cullingOff);
            RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeFovCullingOff, "fov_culling_off", cullingOff);
        }
        ImGui::TextDisabled(PrimeFov::CullingBypassed(PrimeFov::Fov(), PrimeFov::CullingAsked())
                                ? "Culling off: objects at the edges no longer pop in, but every object is drawn (costs GPU time)."
                                : "Up to 94 degrees the game's culling stays on; above it, everything is drawn.");
    }
    ImGui::Separator();
#endif

    // V-Sync: Fifo presentation (aurora_set_vsync). Takes effect at the next frame.
    bool vsync = aurora_get_vsync();
    if (ImGui::Checkbox("V-Sync (no tearing)", &vsync)) {
        aurora_set_vsync(vsync);
        RuntimeConfigFile::SetVsync(vsync);
    }
    ImGui::TextDisabled("Each image waits for the monitor's refresh: no tearing, about one refresh more latency.");
    ImGui::TextDisabled("Present mode in use: %s.", aurora_get_present_mode_name());
    if (vsync && g_frameInterpolationMode != 0 && std::strcmp(aurora_get_present_mode_name(), "Fifo") == 0)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "This driver has no Mailbox mode: with V-Sync the interpolated rate falls back to 60.\n"
                           "For more than 60, turn V-Sync off.");
    ImGui::Separator();

    // Aurora frame interpolation: the game keeps its 60 Hz logic and VI timing;
    // the renderer inserts transform-interpolated images between two logical
    // frames (decision of 2026-10-25: allowed because only the host layer changes).
    DrawFrameInterpolationCombo("Frame rate above 60 (interpolated)");
    ImGui::TextDisabled("Game logic, speed and achievements stay at 60 Hz: images are only inserted between");
    ImGui::TextDisabled("two frames. Each real frame reaches the screen a little later (more aiming latency).");
    if (g_frameInterpolationMode != 0) {
        bool lowLatency = aurora_get_frame_interpolation_low_latency();
        if (ImGui::Checkbox("Low latency (predicted in-between images)", &lowLatency)) {
            aurora_set_frame_interpolation_low_latency(lowLatency);
            RuntimeConfigFile::SetFrameInterpolationLowLatency(lowLatency);
        }
        ImGui::TextDisabled("Shows each real frame as soon as it is ready, about half a frame sooner, and");
        ImGui::TextDisabled("predicts the images after it. Motion that stops or turns overshoots slightly.");
        bool displaySync = aurora_get_frame_interpolation_display_sync();
        if (ImGui::Checkbox("Sync images to the monitor's refresh", &displaySync)) {
            aurora_set_frame_interpolation_display_sync(displaySync);
            RuntimeConfigFile::SetFrameInterpolationDisplaySync(displaySync);
        }
        ImGui::TextDisabled("Each image waits for its own refresh, so none is shown twice or skipped while the");
        ImGui::TextDisabled("game's clock drifts against the monitor's. Costs about 4 ms of latency.");
        if (displaySync) {
            const float syncRate = aurora_get_display_sync_rate();
            if (syncRate > 0.0f && aurora_get_display_sync_paused())
                ImGui::TextDisabled("Synced to %.2f Hz; paused while the PC has no room to spare (heavy scene).", syncRate);
            else if (syncRate > 0.0f)
                ImGui::TextDisabled("Synced to %.2f Hz.", syncRate);
            else
                ImGui::TextDisabled("Not synced yet (needs a steady refresh; off with G-Sync/FreeSync).");
        }
    }
    ImGui::TextDisabled("Needs a monitor above 60 Hz to be seen; the rate drops by itself if the PC cannot");
    ImGui::TextDisabled("keep up. With 6x/8x the resolution drops to 4x.");
    if (g_frameInterpolationMode != 0 && MonitorStopsAt60()) {
        ImGui::TextDisabled("This monitor runs at %.0f Hz, so this stays off until the window is on a faster one.",
                            aurora_get_display_refresh_rate());
    } else if (g_frameInterpolationMode != 0 && DriverFrameCap() != 0) {
        const uint32_t cap = DriverFrameCap();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           DriverStopsAt60() ? "The NVIDIA driver limits this program to %u FPS, so this stays off."
                                             : "The NVIDIA driver limits this program to %u FPS.",
                           cap);
        ImGui::TextDisabled("NVIDIA Control Panel > Manage 3D settings > Program Settings > Corruption_3 PC.exe:");
        ImGui::TextDisabled("set Max Frame Rate to Off (or to your monitor's rate), then restart the game.");
    }
#if defined(MKW_PRIME_QA_TOOLS)
    if (g_frameInterpolationMode != 0) {
        AuroraFrameInterpolationDiagnostics diag{};
        aurora_get_frame_interpolation_diagnostics(&diag);
        const uint32_t percent = diag.matchable ? diag.matches * 100u / diag.matchable : 0u;
        ImGui::TextDisabled("Last frame: %u in-between (target %u), %u/%u draws paired (%u%%), %s.",
                            diag.activeSamples, diag.targetSamples, diag.matches, diag.matchable, percent,
                            diag.replaySafe ? "replayable" : "duplicated (EFB copy to RAM)");
    }
#endif
    ImGui::Separator();

    // Dolphin-style custom textures (indexed once at startup).
    bool replacements = RuntimeConfigFile::TextureReplacements(false);
    if (ImGui::Checkbox("Custom textures (texture_replacements folder)", &replacements))
        RuntimeConfigFile::SetTextureReplacements(replacements);
    RestartNote();
    bool dumps = RuntimeConfigFile::TextureDumps(false);
    if (ImGui::Checkbox("Dump game textures to Cache/texture_dumps", &dumps))
        RuntimeConfigFile::SetTextureDumps(dumps);
    RestartNote();
    ImGui::TextDisabled("Dumping requires custom textures to be enabled; it uses disk space.");
}

#if defined(MKW_GAME_PROFILE_RM3E01_00)
// Two host inputs per Wii action; the mouse always moves the pointer.
void DrawPrimeKeyBindings() {
    ImGui::TextDisabled("The mouse moves the pointer. Click a cell, then press the key or mouse button.");
    ImGui::TextDisabled("Right-click a cell to clear it. Changes apply and save immediately.");
    size_t captureAction = 0, captureSlot = 0;
    const bool capturing = PrimeKeyBindings::Capturing(&captureAction, &captureSlot);
    if (ImGui::BeginTable("prime_key_bindings", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Secondary", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (size_t action = 0; action < PrimeKeyBindings::ActionCount(); ++action) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(PrimeKeyBindings::Action(action).label);
            for (size_t slot = 0; slot < PrimeKeyBindings::kSlots; ++slot) {
                ImGui::TableSetColumnIndex(static_cast<int>(slot + 1));
                ImGui::PushID(static_cast<int>(action * PrimeKeyBindings::kSlots + slot));
                const bool waiting = capturing && captureAction == action && captureSlot == slot;
                const std::string text =
                    waiting ? "Press a key..." : PrimeKeyBindings::BindingName(PrimeKeyBindings::Binding(action, slot));
                if (ImGui::Button(text.c_str(), ImVec2(-FLT_MIN, 0))) PrimeKeyBindings::BeginCapture(action, slot);
                if (!capturing && ImGui::IsItemClicked(ImGuiMouseButton_Right)) PrimeKeyBindings::SetBinding(action, slot, 0);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    if (capturing && ImGui::Button("Cancel binding")) PrimeKeyBindings::CancelCapture();
    if (ImGui::Button("Restore default keys")) PrimeKeyBindings::ResetDefaults();
}

// Gamepad (SDL3): drives the same virtual Wii Remote + Nunchuk as the keys.
void DrawPrimeGamepadOptions() {
    ImGui::TextUnformatted("Gamepad");
    bool enabled = PrimeGamepadRuntime::Enabled();
    if (ImGui::Checkbox("Play with a gamepad too (Xbox, PlayStation, Switch Pro, ...)", &enabled)) {
        PrimeGamepadRuntime::SetEnabled(enabled);
        RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeGamepad, "gamepad", enabled);
    }
    const std::string name = PrimeGamepadRuntime::PadName();
    ImGui::TextDisabled("Detected: %s", name.empty() ? "none" : name.c_str());
    ImGui::TextDisabled("Left stick moves. The right stick turns the camera where the mouse would (mouse look on),\n"
                        "and moves the pointer in menus, visor select, lock-on and scanning.");
    auto settings = PrimeGamepadRuntime::CurrentSettings();
    bool changed = false;
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderFloat("Camera speed", &settings.lookDegPerSecond, 30.0f, 720.0f, "%.0f degrees/s")) {
        changed = true;
        RuntimeConfigFile::SetPrimeFloatOption(&RuntimeUserConfig::primePadLookSpeed, "pad_look_speed", settings.lookDegPerSecond);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("At full deflection. The game's own maximum turn rate still applies\n"
                          "(game menu > Options > Controls > Sensitivity).");
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderFloat("Camera response curve", &settings.lookCurve, 1.0f, 3.0f, "%.1f")) {
        changed = true;
        RuntimeConfigFile::SetPrimeFloatOption(&RuntimeUserConfig::primePadLookCurve, "pad_look_curve", settings.lookCurve);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("1 = linear. Higher values give finer aim near the centre of the stick.");
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderFloat("Pointer speed", &settings.pointerSpeed, 0.3f, 5.0f, "%.1f screens/s")) {
        changed = true;
        RuntimeConfigFile::SetPrimeFloatOption(&RuntimeUserConfig::primePadPointerSpeed, "pad_pointer_speed", settings.pointerSpeed);
    }
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderFloat("Stick dead zone", &settings.deadzone, 0.0f, 0.5f, "%.2f")) {
        changed = true;
        RuntimeConfigFile::SetPrimeFloatOption(&RuntimeUserConfig::primePadDeadzone, "pad_deadzone", settings.deadzone);
    }
    if (ImGui::Checkbox("Invert the camera's vertical axis (gamepad)", &settings.invertY)) {
        changed = true;
        RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primePadInvertY, "pad_invert_y", settings.invertY);
    }
    if (changed) PrimeGamepadRuntime::ApplySettings(settings);
    // The motion mode button doubles as the shift of the third column.
    const size_t motionAction = PrimePadBindings::ActionCount() - 1;
    PrimeGamepad::Input shift = PrimePadBindings::Binding(motionAction, 0);
    if (shift == PrimeGamepad::Input::None) shift = PrimePadBindings::Binding(motionAction, 1);
    const std::string shiftName = shift == PrimeGamepad::Input::None ? std::string("motion button")
                                                                      : PrimePadBindings::BindingName(shift);
    ImGui::TextDisabled("Click a cell, then press a button, trigger or stick direction. Right-click clears it.");
    ImGui::TextDisabled("Third column: hold %s and press that button. Pads with few buttons reach every motion\n"
                        "(lasso pull, tilts, Nunchuk twists) this way; buttons without a combination keep working.",
                        shiftName.c_str());
    size_t captureAction = 0, captureSlot = 0;
    const bool capturing = PrimePadBindings::Capturing(&captureAction, &captureSlot);
    if (ImGui::BeginTable("prime_pad_bindings", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        const std::string layerHeader = "With " + shiftName + " held";
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Secondary", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(layerHeader.c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (size_t action = 0; action < PrimePadBindings::ActionCount(); ++action) {
            // The stick rows of the keyboard table are the left stick itself on a pad.
            if (std::strncmp(PrimePadBindings::ActionId(action), "stick_", 6) == 0) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(PrimePadBindings::ActionLabel(action));
            for (size_t slot = 0; slot < PrimePadBindings::kSlots; ++slot) {
                ImGui::TableSetColumnIndex(static_cast<int>(slot + 1));
                ImGui::PushID(static_cast<int>(10000 + action * PrimePadBindings::kSlots + slot));
                const bool waiting = capturing && captureAction == action && captureSlot == slot;
                const std::string text =
                    waiting ? "Press a button..." : PrimePadBindings::BindingName(PrimePadBindings::Binding(action, slot));
                if (ImGui::Button(text.c_str(), ImVec2(-FLT_MIN, 0))) PrimePadBindings::BeginCapture(action, slot);
                if (!capturing && ImGui::IsItemClicked(ImGuiMouseButton_Right))
                    PrimePadBindings::SetBinding(action, slot, PrimeGamepad::Input::None);
                ImGui::PopID();
            }
            ImGui::TableSetColumnIndex(3);
            if (action == motionAction) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("(the shift)");
                continue;
            }
            const size_t layer = PrimePadBindings::kLayerSlot;
            ImGui::PushID(static_cast<int>(20000 + action));
            const bool waiting = capturing && captureAction == action && captureSlot == layer;
            const PrimeGamepad::Input bound = PrimePadBindings::Binding(action, layer);
            const std::string text = waiting ? "Press a button..."
                                     : bound == PrimeGamepad::Input::None ? std::string("-")
                                                                           : shiftName + " + " + PrimePadBindings::BindingName(bound);
            if (ImGui::Button(text.c_str(), ImVec2(-FLT_MIN, 0))) PrimePadBindings::BeginCapture(action, layer);
            if (!capturing && ImGui::IsItemClicked(ImGuiMouseButton_Right))
                PrimePadBindings::SetBinding(action, layer, PrimeGamepad::Input::None);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (capturing && ImGui::Button("Cancel gamepad binding")) PrimePadBindings::CancelCapture();
    if (ImGui::Button("Restore default gamepad buttons")) PrimePadBindings::ResetDefaults();
}
#endif

void DrawPrimeControlOptions() {
    {
        bool keyboardMouse = RuntimeConfigFile::Get().primeKeyboardMouse.value_or(true);
        if (ImGui::Checkbox("Keyboard and mouse mode (virtual Wii Remote + Nunchuk)", &keyboardMouse))
            RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeKeyboardMouse, "keyboard_mouse", keyboardMouse);
        RestartNote();
        ImGui::TextDisabled("Off: play with a real Wii Remote paired over Bluetooth (F10 > Controller settings). Active now: %s.",
                            RuntimeConfigFile::PrimeKeyboardMouseMode() ? "keyboard and mouse" : "Wii Remote");
    }
#if defined(_WIN32)
    if (ImGui::Checkbox("Hide the Windows cursor while playing", &g_primeHideCursor))
#else
    if (ImGui::Checkbox("Hide the mouse cursor while playing", &g_primeHideCursor))
#endif
        RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeHideCursor, "hide_cursor", g_primeHideCursor);
    ImGui::TextDisabled("It comes back while F1 or F10 is open.");
    {
        bool batteryFull = RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primeWiimoteBatteryFull,
                                                          "WII_COMPILED_PRIME_WIIMOTE_BATTERY_FULL", true);
        if (ImGui::Checkbox("Wii Remote battery always full (no low-battery warning)", &batteryFull))
            RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeWiimoteBatteryFull, "wiimote_battery_full", batteryFull);
        ImGui::TextDisabled("The game polls the remote every few seconds; it is answered 4/4 right away.");
    }
    PrimeOptionCheckbox("Keyboard and mouse icons in the game's prompts", &RuntimeUserConfig::primeKeyboardIcons,
                        "keyboard_icons", false,
                        "Replaces the Wii Remote and Nunchuk button textures (A, B, Z, C, +, -, 1, 2, D-pad, "
                        "stick, gestures) with key caps showing the primary binding of this tab. Generated at startup in "
#if defined(_WIN32)
                        "UserData/texture_replacements/keyboard_icons; key names follow the Windows display language. The game's text still says 'button'.");
#else
                        "UserData/texture_replacements/keyboard_icons; key names follow the keyboard layout. The game's text still says 'button'.");
#endif
    ImGui::Separator();
    ImGui::TextUnformatted("Aiming sensitivity");
    ImGui::TextWrapped("This is the game's own option, saved in your save file: game menu > Options > "
                       "Controls > Sensitivity (Basic / Standard / Advanced). Advanced has the smallest dead "
                       "zone and the fastest turn.");
    ImGui::Separator();
    ImGui::TextUnformatted("Keyboard and mouse");
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    {
        bool mouseLook = RuntimeConfigFile::Get().primeMouseLook.value_or(true);
        mouseLook = PrimeMouseLook::Enabled();  // the hotkey can flip it while the panel is closed
        if (ImGui::Checkbox("Camera follows the mouse in first person (mouse look)", &mouseLook)) {
            RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeMouseLook, "mouse_look", mouseLook);
            PrimeMouseLook::SetEnabled(mouseLook);
        }
        {
            // On-screen hint: size and position (shown live while these are used).
            ImGui::TextUnformatted("Mouse look hint (top of the screen)");
            const auto& config = RuntimeConfigFile::Get();
            float hintScale = config.primeMouseLookHintScale.value_or(kHintDefaultScale);
            float hintX = config.primeMouseLookHintX.value_or(kHintDefaultX) * 100.0f;
            float hintY = config.primeMouseLookHintY.value_or(kHintDefaultY) * 100.0f;
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat("Hint size", &hintScale, 0.4f, 2.0f, "%.2fx"))
                RuntimeConfigFile::SetPrimeFloatOption(&RuntimeUserConfig::primeMouseLookHintScale, "mouse_look_hint_scale", hintScale);
            const bool a1 = ImGui::IsItemActive() || ImGui::IsItemHovered();
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat("Hint position X", &hintX, 0.0f, 100.0f, "%.1f %%"))
                RuntimeConfigFile::SetPrimeFloatOption(&RuntimeUserConfig::primeMouseLookHintX, "mouse_look_hint_x", hintX / 100.0f);
            const bool a2 = ImGui::IsItemActive() || ImGui::IsItemHovered();
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat("Hint position Y", &hintY, 0.0f, 95.0f, "%.1f %%"))
                RuntimeConfigFile::SetPrimeFloatOption(&RuntimeUserConfig::primeMouseLookHintY, "mouse_look_hint_y", hintY / 100.0f);
            const bool a3 = ImGui::IsItemActive() || ImGui::IsItemHovered();
            bool a4 = false;
            if (ImGui::Button("Reset hint size and position")) {
                RuntimeConfigFile::SetPrimeFloatOption(&RuntimeUserConfig::primeMouseLookHintScale, "mouse_look_hint_scale", kHintDefaultScale);
                RuntimeConfigFile::SetPrimeFloatOption(&RuntimeUserConfig::primeMouseLookHintX, "mouse_look_hint_x", kHintDefaultX);
                RuntimeConfigFile::SetPrimeFloatOption(&RuntimeUserConfig::primeMouseLookHintY, "mouse_look_hint_y", kHintDefaultY);
                a4 = true;
            }
            a4 = a4 || ImGui::IsItemHovered();
            if (a1 || a2 || a3 || a4) g_hintEditFrame = g_hintFrame;
            ImGui::TextDisabled("X = centre of the panel across the screen, Y = its top edge from the top. The panel is shown\n"
                                "while you hover or drag these sliders; the toggle key is \"Mouse look on/off\" below.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Moving the mouse turns the camera, like a PC game; the game's pointer stays centered.\n"
                              "Only while aiming in first person without a lock-on: in menus, the map, Morph Ball and\n"
                              "cutscenes the cursor is absolute again. F1 releases the mouse.");
        float sensitivity = RuntimeConfigFile::Get().primeMouseLookSensitivity.value_or(PrimeMouseLook::kDefaultSensitivityDegPerPixel);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderFloat("Mouse sensitivity", &sensitivity, PrimeMouseLook::kMinSensitivityDegPerPixel,
                               PrimeMouseLook::kMaxSensitivityDegPerPixel, "%.2f degrees per pixel")) {
            RuntimeConfigFile::SetPrimeFloatOption(&RuntimeUserConfig::primeMouseLookSensitivity, "mouse_look_sensitivity", sensitivity);
            PrimeMouseLook::SetSensitivityDegPerPixel(sensitivity);
        }
        float smoothing = RuntimeConfigFile::Get().primeMouseLookSmoothing.value_or(0.0f);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderFloat("Mouse smoothing", &smoothing, 0.0f, PrimeMouseLook::kMaxSmoothing, "%.2f")) {
            RuntimeConfigFile::SetPrimeFloatOption(&RuntimeUserConfig::primeMouseLookSmoothing, "mouse_look_smoothing", smoothing);
            PrimeMouseLook::SetSmoothing(smoothing);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = none: the camera follows the mouse instantly on both axes (the game's own\n"
                              "smoothing is bypassed). Higher values spread each movement over several frames.");
        bool invert = RuntimeConfigFile::Get().primeMouseLookInvertY.value_or(false);
        if (ImGui::Checkbox("Invert the mouse's vertical axis", &invert)) {
            RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeMouseLookInvertY, "mouse_look_invert_y", invert);
            PrimeMouseLook::SetInvertY(invert);
        }
        ImGui::TextDisabled("The maximum turn rate is the game's own (Sensitivity option above).");
    }
    PrimeOptionCheckbox("Pointer without the game's smoothing (less latency)", &RuntimeUserConfig::primeRawPointer,
                        "raw_pointer", true,
                        "The game averages the Wii Remote pointer position to remove jitter; with a mouse\n"
                        "it only adds delay. Affects the pointer in menus, the map and first person.");
    ImGui::Separator();
    ImGui::Checkbox("Wii Remote and Nunchuk motion panel (F6)", &g_showRemoteMotion);
    ImGui::TextDisabled("Keyboard and mouse mode only: roll, depth, IR camera and accelerometers.");
    DrawPrimeKeyBindings();
    ImGui::Separator();
    DrawPrimeGamepadOptions();
#endif
}

// The console language (SYSCONF IPL.LNG, prime_host_options.cpp). The US disc has every
// text in these five languages; the game reads the choice once, when it starts.
void DrawPrimeLanguageOptions() {
    static constexpr const char* kLanguages[] = {"English", "Español", "Français", "Deutsch", "Italiano"};
    static constexpr const char* kLanguageNames[] = {"english", "spanish", "french", "german", "italian"};
    const std::string language = RuntimeConfigFile::PrimeLanguage();
    int languageIndex = 0;
    for (int i = 0; i < 5; ++i)
        if (language == kLanguageNames[i]) languageIndex = i;
    // What the running game uses: the setting as it was when this session started.
    static const int startupIndex = languageIndex;
    ImGui::TextWrapped("Language of the game's text: menus, messages, the HUD, scans and the logbook.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::Combo("Game language", &languageIndex, kLanguages, 5))
        RuntimeConfigFile::SetPrimeLanguage(kLanguageNames[languageIndex]);
    ImGui::Spacing();
    ImGui::Text("In use now: %s", kLanguages[startupIndex]);
    if (languageIndex != startupIndex) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           "Saved. Close the game and start it again to see %s.", kLanguages[languageIndex]);
    } else {
        ImGui::TextDisabled("Changes apply the next time the game starts.");
    }
    ImGui::Separator();
    ImGui::TextDisabled("Your save is not affected: the same save works in every language.");
    ImGui::TextDisabled("A few system notices (disc and Nunchuk warnings) stay in English.");
}

void DrawPrimeAudioOptions() {
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderInt("Master volume", &g_audioVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_audioVolumePercent) / 100.0f;
        AudioBackend::Instance().SetMasterVolume(volume);
        RuntimeConfigFile::SetAudioVolume(volume);
    }
    if (ImGui::Checkbox("Mute", &g_audioMuted)) {
        AudioBackend::Instance().SetMuted(g_audioMuted);
        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
    }
    ImGui::Separator();
    static constexpr const char* kModes[] = {"Stereo", "Surround (Dolby Pro Logic II)", "Mono"};
    static constexpr const char* kModeNames[] = {"stereo", "surround", "mono"};
    const std::string mode = RuntimeConfigFile::PrimeSoundMode();
    int modeIndex = mode == "surround" ? 1 : mode == "mono" ? 2 : 0;
    if (ImGui::Combo("Console sound mode", &modeIndex, kModes, 3))
        RuntimeConfigFile::SetPrimeSoundMode(kModeNames[modeIndex]);
    RestartNote();
    ImGui::TextDisabled("Surround is matrix-encoded: only useful with a Pro Logic II decoder.");
    PrimeOptionCheckbox("High-quality resampling (recommended)", &RuntimeUserConfig::primeHostAudioResampler,
                        "host_audio_resampler", true,
                        "Converts the game's 32 kHz to your sound card's rate with a 128-tap\n"
                        "polyphase filter. Off uses SDL's converter.");
    if (ImGui::Checkbox("Mix audio on its own thread (like the Wii's DSP)", &g_audioMixWorker)) {
        AxDspHle::SetMixWorkerEnabled(g_audioMixWorker);
        RuntimeConfigFile::SetAudioMixWorker(g_audioMixWorker);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Mixes voices and effects in parallel with the game, like the Wii's sound chip,\n"
                          "and notifies the game only when done. Frees the game thread. Same result;\n"
                          "turn it off if you notice any sound problem. Applies immediately.");
}

void DrawPrimePerformanceOptions() {
    PrimeOptionCheckbox("Dedicated GPU thread", &RuntimeUserConfig::primeGpuThread, "gpu_thread", true,
                        "The emulated GPU runs on its own core, in parallel with the game.");
    PrimeOptionCheckbox("Overlapped frame encoding", &RuntimeUserConfig::primeOverlapEncode, "overlap_encode",
                        true, "Prepares the previous frame for the graphics card without holding up the next one.");
    PrimeOptionCheckbox("Batch draws", &RuntimeUserConfig::primeLazyEndDrain, "lazy_end_drain", true,
                        "Processes consecutive primitives together instead of one by one.");
    PrimeOptionCheckbox("Aurora decoder on its own thread (experimental)",
                        &RuntimeUserConfig::primeAuroraDecodeThread, "aurora_decode_thread", true,
                        "Splits interpreting the Wii's commands and translating them for the graphics card\n"
                        "across two threads. Spreads the GX thread load on CPUs with 4 or more cores.");
    PrimeOptionCheckbox("Expand draws on the decoder thread", &RuntimeUserConfig::primeDeferDlExpansion,
                        "defer_dl_expansion", false,
                        "With the Aurora decoder thread: lets that thread expand the game's packed draws\n"
                        "when it is not busy, taking load off the GX thread. Takes effect after a restart.");
#if defined(_WIN32)
    // A DXGI setting: dxgi_frame_latency.cpp is a no-op on Linux, so the option is not shown there.
    PrimeOptionCheckbox("Single-frame present queue (less latency)", &RuntimeUserConfig::primePresentQueueLimit,
                        "present_queue_limit", true,
                        "Limits the DirectX 12 swap chain queue to one frame: the image shown is the\n"
                        "last one finished, not one from two or three frames ago.");
#endif
    PrimeOptionCheckbox("Present each frame as soon as it is ready (less latency)",
                        &RuntimeUserConfig::primePresentOnCopy, "present_on_copy", true,
                        "Does not wait for the next retrace to show a finished frame.");
    bool parallelDecode = RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primeParallelTextureDecode,
                                                         "WII_COMPILED_AURORA_PARALLEL_TEXTURE_DECODE", true);
    if (ImGui::Checkbox("Decode textures on several cores", &parallelDecode)) {
        RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeParallelTextureDecode, "parallel_texture_decode",
                                          parallelDecode);
        aurora_set_parallel_texture_decode(parallelDecode);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Spreads new large textures over several cores; same result. Applies immediately.");
    bool gpuDecode = RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primeGpuTextureDecode,
                                                    "WII_COMPILED_AURORA_GPU_CMPR_DECODE", true);
    if (ImGui::Checkbox("Decode compressed textures on the GPU", &gpuDecode)) {
        RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeGpuTextureDecode, "gpu_texture_decode", gpuDecode);
        aurora_set_gpu_cmpr_decode(gpuDecode);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uploads CMPR textures as-is (8 times less data) and the graphics card decodes them\n"
                          "with the Wii's exact formula; identical result. Affects new textures.");
    bool fastDisc = RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primeFastDiscReads,
                                                   "WII_COMPILED_PRIME_FAST_DISC_READS", true);
    if (ImGui::Checkbox("Fast disc reads", &fastDisc))
        RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeFastDiscReads, "fast_disc_reads", fastDisc);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Delivers disc reads almost instantly instead of simulating the Wii drive's speed. "
                          "Avoids dips while loading rooms. Applies immediately.");
    bool asyncDisc = RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primeAsyncDiscReads,
                                                    "WII_COMPILED_PRIME_ASYNC_DISC_READS", true);
    if (ImGui::Checkbox("Read the disc on its own thread", &asyncDisc))
        RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeAsyncDiscReads, "async_disc_reads", asyncDisc);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reads and decrypts the disc image in parallel with the game, like the Wii drive, and hands\n"
                          "the data over when done. Takes disc reads off the game thread while loading areas. Applies immediately.");
    ImGui::Separator();
    if (ImGui::Checkbox("Experimental: do not wait for new shaders", &g_skipUnreadyPipelines)) {
        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When something uses a shader that is not compiled yet, wait at most 4 ms per\n"
                          "frame and skip what is not ready until it finishes (a fraction of a second,\n"
                          "first time only). Draws that feed a one-shot screen copy still wait. With the\n"
                          "ubershader on, this only applies to the few materials it cannot cover. Applies immediately.");
    if (ImGui::Checkbox("Ubershader while shaders compile", &g_ubershader)) {
        aurora_set_ubershader(g_ubershader);
        RuntimeConfigFile::SetUbershader(g_ubershader);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When a shader is still compiling, draw with a generic shader that reads the material\n"
                          "from memory instead of waiting. The compiled shaders and the shader cache keep priority;\n"
                          "this only covers the gap. Costs more GPU for those few frames, about 100 MiB more shader\n"
                          "cache and 2 s more at start-up. Not used while frame interpolation is on. On by default.");
    {
        const auto purgeMarker = RuntimeConfigFile::ApplicationDataDirectory() / "Cache" / "purge_shader_cache.flag";
        std::error_code purgeError;
        const bool pending = std::filesystem::exists(purgeMarker, purgeError);
        static bool confirm = false;
        if (pending) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Shader cache will be purged the next time the game starts.");
            if (ImGui::SmallButton("Cancel purge")) std::filesystem::remove(purgeMarker, purgeError);
        } else if (!confirm) {
            if (ImGui::Button("Purge shader cache...")) confirm = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Deletes the compiled shader databases of this profile (Cache folder) at the next start. "
                                  "The first launch afterwards recompiles every shader and takes a few minutes. Saves, settings and bindings are not touched.");
        } else {
            ImGui::TextUnformatted("Delete shader cache at the next start?");
            if (ImGui::Button("Yes, purge")) {
                if (FILE* marker = std::fopen(purgeMarker.string().c_str(), "wb")) std::fclose(marker);
                confirm = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("No")) confirm = false;
        }
    }
    ImGui::TextDisabled("Frame limit: 60 FPS (the game logic runs at 60 Hz).");
}

// F1 > Effects: Aurora image effects (live) and the bundled ReShade.
void DrawPrimeEffectsOptions() {
    const auto& cfg = RuntimeConfigFile::Get();
    bool changed = false;
    const auto flag = [&](const char* label, std::optional<bool> RuntimeUserConfig::*field, const char* key,
                          bool fallback = false) {
        bool value = (cfg.*field).value_or(fallback);
        if (ImGui::Checkbox(label, &value)) { RuntimeConfigFile::SetEffectsOption(field, key, value); changed = true; }
        return value;
    };
    const auto knob = [&](const char* label, std::optional<float> RuntimeUserConfig::*field, const char* key,
                          float fallback, float lo, float hi, const char* format = "%.2f") {
        float value = (cfg.*field).value_or(fallback);
        if (ImGui::SliderFloat(label, &value, lo, hi, format)) { RuntimeConfigFile::SetEffectsFloat(field, key, value); changed = true; }
    };
    ImGui::TextUnformatted("Aurora image effects");
    ImGui::TextDisabled("Full-screen passes over the finished frame, in this order. All apply immediately and cost little.");
    ImGui::Separator();
    flag("Fix glow ghost copies", &RuntimeUserConfig::effectsGlowFix, "glow_ghost_fix", true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The game's own glow (light strips, engine flames) blurs a copy of the picture at offsets made\n"
                          "for the Wii's resolution; at higher resolutions thin lights showed separate copies beside them.\n"
                          "This softens that copy to the Wii's footprint so the glow blends as on the console. Cost: very low.");
    ImGui::Separator();
    if (flag("Ambient occlusion (SSAO)", &RuntimeUserConfig::effectsSsao, "ssao", true)) {
        knob("SSAO radius", &RuntimeUserConfig::effectsSsaoRadius, "ssao_radius", 1.0f, 0.25f, 3.0f);
        knob("SSAO intensity", &RuntimeUserConfig::effectsSsaoIntensity, "ssao_intensity", 0.70f, 0.0f, 2.0f);
        knob("SSAO softness", &RuntimeUserConfig::effectsSsaoSoftness, "ssao_softness", 2.0f, 1.0f, 4.0f);
        {
            static constexpr const char* kQuality[] = {"Low (8 samples)", "Medium (12)", "High (16)", "Ultra (24)"};
            static constexpr float kSamples[] = {8.0f, 12.0f, 16.0f, 24.0f};
            const float current = cfg.effectsSsaoSamples.value_or(8.0f);
            int index = 1;
            for (int i = 0; i < 4; ++i) if (kSamples[i] == current) index = i;
            if (ImGui::Combo("SSAO quality", &index, kQuality, 4)) {
                RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsSsaoSamples, "ssao_samples", kSamples[index]);
                changed = true;
            }
        }
        knob("SSAO bias (avoid self-shadowing)", &RuntimeUserConfig::effectsSsaoBias, "ssao_bias", 1.2f, 0.0f, 5.0f, "%.2f %%");
        knob("SSAO power (contrast)", &RuntimeUserConfig::effectsSsaoPower, "ssao_power", 0.99f, 0.5f, 3.0f);
        knob("SSAO fade distance (0 = off)", &RuntimeUserConfig::effectsSsaoFade, "ssao_fade_distance", 0.0f, 0.0f, 200.0f, "%.0f");
        knob("SSAO edge sharpness", &RuntimeUserConfig::effectsSsaoEdge, "ssao_edge_sharpness", 0.52f, 0.0f, 1.0f);
        knob("SSAO glow protection", &RuntimeUserConfig::effectsSsaoGlow, "ssao_glow_protect", 1.0f, 0.0f, 1.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Keeps occlusion off surfaces the game marks as glowing: Samus' visor, lights, energy.");
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        {
            bool skipFace = cfg.effectsSsaoSkipFace.value_or(true);
            if (ImGui::Checkbox("No SSAO on Samus' face", &skipFace)) {
                RuntimeConfigFile::SetEffectsOption(&RuntimeUserConfig::effectsSsaoSkipFace, "ssao_skip_face", skipFace);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Her face behind the visor glass gets no occlusion; the helmet and suit keep it. On by default.");
        }
#endif
        {
            bool underEffects = cfg.effectsSsaoUnderEffects.value_or(true);
            if (ImGui::Checkbox("SSAO under fog and effects", &underEffects)) {
                RuntimeConfigFile::SetEffectsOption(&RuntimeUserConfig::effectsSsaoUnderEffects, "ssao_under_effects",
                                                    underEffects);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Fog, explosions and particles are drawn over the occlusion instead of being darkened\n"
                                  "by the surface behind them (also keeps floor reflections off them). On by default.");
        }
        if (ImGui::Button("Reset SSAO")) {
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsSsaoRadius, "ssao_radius", 1.0f);
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsSsaoIntensity, "ssao_intensity", 0.70f);
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsSsaoSoftness, "ssao_softness", 2.0f);
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsSsaoSamples, "ssao_samples", 8.0f);
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsSsaoBias, "ssao_bias", 1.2f);
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsSsaoPower, "ssao_power", 0.99f);
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsSsaoFade, "ssao_fade_distance", 0.0f);
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsSsaoEdge, "ssao_edge_sharpness", 0.52f);
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsSsaoGlow, "ssao_glow_protect", 1.0f);
            changed = true;
        }
        ImGui::TextDisabled("Darkens corners and contact areas from the depth buffer. Needs antialiasing (MSAA) off.");
    }
    ImGui::SeparatorText("World lighting (experimental)");
    ImGui::TextDisabled("Walls and floors of the levels have their light baked into a texture. Each option works on its own.");
    flag("1. Environment material", &RuntimeUserConfig::effectsEnvMaterial, "env_material", true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Walls and floors react to the view: a soft sheen, a reflection of the surroundings lit by the\n"
                          "baked light, and relief from the texture. Tune it in F1 > Materials > World surfaces.");
    flag("2. Borrowed lights", &RuntimeUserConfig::effectsEnvLights, "env_borrowed_lights", true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Highlights on walls and floors from the lights the game uses for characters in the room.\n"
                          "They follow the camera; none where the baked light is in shadow.");
    const bool ssrOn = flag("3. Floor reflections (SSR)", &RuntimeUserConfig::effectsSsr, "ssr", true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Smooth floors reflect what is on screen, stronger at grazing angles. Busy textures (rock,\n"
                          "grating) reflect less. Needs antialiasing (MSAA) off.");
    flag("Material in rooms with live lights", &RuntimeUserConfig::effectsLitRooms, "lit_room_material", true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The world material also on walls lit by the room's own lights (e.g. the Aurora Unit\n"
                          "chamber), with highlights from those lights. Cost: low.");
    flag("Smooth lightmaps", &RuntimeUserConfig::effectsSmoothLightmaps, "smooth_lightmaps", true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Softer gradients in the baked light of walls and floors (no stair or diamond steps).\n"
                          "Same texture reads as before. Cost: none. Needs the environment material.");
    flag("Soft highlights", &RuntimeUserConfig::effectsSoftHighlights, "soft_highlights", true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Added highlights fade into white like a camera instead of clipping to flat white.\n"
                          "Applies to every enhanced material. Cost: none.");
    flag("Area-coloured reflections", &RuntimeUserConfig::effectsAreaReflections, "area_reflections", false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Metal and varnish reflect the colours of the area (above and below the horizon) instead\n"
                          "of a generic sky. The colours are measured from the picture and arrive a few frames late,\n"
                          "smoothed: only the reflection tint lags, never the game. Cost: low (a tiny GPU read-back).");
    flag("Soft character shadows", &RuntimeUserConfig::effectsSoftShadows, "soft_shadows", true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The game's projected shadows get soft edges instead of stepped ones. Cost: low.");
    flag("Light shafts", &RuntimeUserConfig::effectsLightShafts, "light_shafts", false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Beams of light from the scene's lights through bright and glowing areas (screen-space,\n"
                          "only for lights on screen). PERFORMANCE: medium, a quarter-resolution pass of up to\n"
                          "96 samples per pixel. Turn it off first if the frame rate drops.");
    if (cfg.effectsLightShafts.value_or(false))
        knob("Light shafts intensity", &RuntimeUserConfig::effectsLightShaftsIntensity, "light_shafts_intensity", 0.5f, 0.0f, 2.0f);
    flag("Linear light blending", &RuntimeUserConfig::effectsLinearLighting, "linear_lighting", false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Adds highlights and reflections in linear light (physically closer, brighter highlights).\n"
                          "Changes the look of every enhanced material, so it is off by default. Cost: none.");
    if (ssrOn) {
        flag("Reflections respect occlusion", &RuntimeUserConfig::effectsSpecOcclusion, "specular_occlusion", true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Creases and corners darkened by SSAO reflect less too. Needs SSAO on. Cost: none.");
        knob("Reflection strength", &RuntimeUserConfig::effectsSsrStrength, "ssr_strength", 0.7f, 0.0f, 1.0f);
        knob("Floor roughness", &RuntimeUserConfig::effectsSsrRoughness, "ssr_roughness", 0.35f, 0.0f, 1.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = mirror. Higher = the reflection blurs with distance, like worn or brushed metal.");
        knob("Floor relief", &RuntimeUserConfig::effectsSsrRelief, "ssr_relief", 1.0f, 0.0f, 2.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How much the floor texture's panels, seams and grime bend and break up the reflection.");
    }
    // Separate, experimental section: switching it off leaves the floor reflections and everything
    // above as they were.
    ImGui::Separator();
    ImGui::TextUnformatted("Wall reflections (experimental)");
    const bool wallOn = flag("Reflections on smooth walls", &RuntimeUserConfig::effectsWallSsr, "wall_reflections", true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Screen-space reflections, like the floor ones, on walls whose texture is very smooth\n"
                          "(polished panels). Only what is on screen can be reflected: they fade near the screen\n"
                          "edges and where the wall would show what is behind the camera, and the material's own\n"
                          "reflection stays there. Samus herself is never reflected (first person). Uses the floor\n"
                          "roughness and relief settings. PERFORMANCE: low to medium (more of the screen is walls).");
    if (wallOn)
        knob("Wall reflection strength", &RuntimeUserConfig::effectsWallSsrStrength, "wall_reflections_strength", 0.8f,
             0.0f, 1.0f);
    ImGui::Separator();
    if (flag("Bloom / glow", &RuntimeUserConfig::effectsBloom, "bloom")) {
        knob("Glow threshold", &RuntimeUserConfig::effectsBloomThreshold, "bloom_threshold", 0.7f, 0.0f, 1.0f);
        knob("Glow intensity", &RuntimeUserConfig::effectsBloomIntensity, "bloom_intensity", 0.35f, 0.0f, 2.0f);
        knob("Glow radius", &RuntimeUserConfig::effectsBloomRadius, "bloom_radius", 1.0f, 0.5f, 3.0f);
        {
            bool worldOnly = cfg.effectsBloomWorldOnly.value_or(true);
            if (ImGui::Checkbox("Glow only from the 3D scene (not the visor HUD)", &worldOnly)) {
                RuntimeConfigFile::SetEffectsOption(&RuntimeUserConfig::effectsBloomWorldOnly, "bloom_world_only", worldOnly);
                changed = true;
            }
        }
        ImGui::TextDisabled("Extra glow on bright areas; the game's own bloom stays. Keep the intensity low.");
    }
    if (flag("Sharpen (CAS)", &RuntimeUserConfig::effectsSharpen, "sharpen")) {
        knob("Sharpen amount", &RuntimeUserConfig::effectsSharpenAmount, "sharpen_amount", 0.5f, 0.0f, 1.0f);
        ImGui::TextDisabled("Contrast-adaptive sharpening (AMD FidelityFX CAS): recovers texture detail without halos.");
    }
    if (flag("Antialiasing (FXAA)", &RuntimeUserConfig::effectsFxaa, "fxaa")) {
        ImGui::TextDisabled("Post-process edge smoothing; cheap, slightly softens text. MSAA (Video tab) is the sharper option.");
    }
    if (flag("Colour grading", &RuntimeUserConfig::effectsColor, "color")) {
        knob("Brightness", &RuntimeUserConfig::effectsBrightness, "brightness", 0.0f, -0.5f, 0.5f);
        knob("Contrast", &RuntimeUserConfig::effectsContrast, "contrast", 1.0f, 0.5f, 1.5f);
        knob("Saturation", &RuntimeUserConfig::effectsSaturation, "saturation", 1.0f, 0.0f, 2.0f);
        knob("Gamma", &RuntimeUserConfig::effectsGamma, "gamma", 1.0f, 0.5f, 2.0f);
        knob("Temperature (cool / warm)", &RuntimeUserConfig::effectsTemperature, "temperature", 0.0f, -1.0f, 1.0f);
        knob("Vignette", &RuntimeUserConfig::effectsVignette, "vignette", 0.0f, 0.0f, 1.0f);
        if (ImGui::Button("Reset colour")) {
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsBrightness, "brightness", 0.0f);
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsContrast, "contrast", 1.0f);
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsSaturation, "saturation", 1.0f);
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsGamma, "gamma", 1.0f);
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsTemperature, "temperature", 0.0f);
            RuntimeConfigFile::SetEffectsFloat(&RuntimeUserConfig::effectsVignette, "vignette", 0.0f);
            changed = true;
        }
    }
    if (changed) PostEffects::Apply();
    if (const char* note = aurora_post_process_status_note(); note && *note) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", note);
#if defined(_WIN32)
    // ReShade is bundled for Direct3D 12 on Windows only; other hosts hide the section.
    ImGui::Separator();
    ImGui::TextUnformatted("ReShade (bundled, optional)");
    {
        bool reshade = RuntimeConfigFile::ReshadeEnabled();
        if (ImGui::Checkbox("Load ReShade at startup", &reshade)) RuntimeConfigFile::SetReshadeEnabled(reshade);
        RestartNote();
        ImGui::TextDisabled("Status: %s", ReShadeLoader::Status().c_str());
        ImGui::TextDisabled("In game: Home opens the ReShade menu with every effect and parameter, End toggles all effects,");
        ImGui::TextDisabled("Print Screen saves a screenshot to reshade/Screenshots. Direct3D 12 only (Vulkan needs ReShade's own installer).");
        ImGui::TextDisabled("Bundled effects: CAS, SMAA, FXAA, LumaSharpen, MagicBloom, AmbientLight, Vibrance, Tonemap, Curves, Levels, LUT...");
        ImGui::TextDisabled("ReShade effects pause in menus, videos and while this panel or another port panel is open.");
        if (ImGui::Button("Open ReShade folder")) {
            const std::string folder = RuntimeConfigFile::PathToUtf8(ReShadeLoader::Directory());
            ::ShellExecuteA(nullptr, "open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
#endif
}

void DrawPrimeOptionsPanel() {
    // WII_COMPILED_PRIME_OPTIONS_TAB=<0..4>: open on that tab once (screenshots/validation).
    static int openTab = [] {
        const char* value = std::getenv("WII_COMPILED_PRIME_OPTIONS_TAB");
        return value ? std::atoi(value) : -1;
    }();
    if (openTab >= 0) g_primeOptionsOpen = true;
    if (!g_primeOptionsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    static const std::string kPanelTitle = std::string(PRIME_PORT_NAME) + " - Options [F1]";
    if (ImGui::Begin(kPanelTitle.c_str(), &g_primeOptionsOpen, ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::BeginTabBar("PrimeOptionsTabs")) {
            const auto tabFlags = [&](int tab) { return openTab == tab ? ImGuiTabItemFlags_SetSelected : 0; };
            if (ImGui::BeginTabItem("Video", nullptr, tabFlags(0))) { DrawPrimeVideoOptions(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Controls", nullptr, tabFlags(1))) { DrawPrimeControlOptions(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Audio", nullptr, tabFlags(2))) { DrawPrimeAudioOptions(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Language", nullptr, tabFlags(8))) { DrawPrimeLanguageOptions(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Performance", nullptr, tabFlags(3))) { DrawPrimePerformanceOptions(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Effects", nullptr, tabFlags(6))) {
                DrawPrimeEffectsOptions();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Materials", nullptr, tabFlags(7))) {
                PrimeMaterials::DrawTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Info", nullptr, tabFlags(5))) {
                ImGui::Text("%s", PRIME_PORT_NAME);
                ImGui::Text("Version %s   Build %s %s", PRIME_PORT_VERSION, __DATE__, __TIME__);
                ImGui::Text("Created by %s", PRIME_PORT_AUTHOR);
                if (ImGui::Button("Open project page")) UpdateCheck::OpenDownloadPage();
                ImGui::SameLine(); ImGui::TextDisabled("%s", PRIME_PORT_REPOSITORY);
                ImGui::Separator();
                {
                    const UpdateCheck::Status status = UpdateCheck::Get();
                    const char* label = "";
                    switch (status.state) {
                    case UpdateCheck::State::Disabled: label = "Update check disabled"; break;
                    case UpdateCheck::State::Idle: label = "Update check not run"; break;
                    case UpdateCheck::State::Checking: label = "Checking for a newer build..."; break;
                    case UpdateCheck::State::UpToDate: label = "Up to date"; break;
                    case UpdateCheck::State::UpdateAvailable: label = "NEW VERSION AVAILABLE"; break;
                    case UpdateCheck::State::Failed: label = "Update check failed"; break;
                    }
                    if (status.state == UpdateCheck::State::UpdateAvailable)
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s: v%s (you have v%s)", label, status.latestVersion.c_str(), PRIME_PORT_VERSION);
                    else
                        ImGui::Text("%s%s%s", label, status.message.empty() ? "" : ": ", status.message.c_str());
                    if (ImGui::Button("Check now")) UpdateCheck::CheckNow();
                    if (status.state == UpdateCheck::State::UpdateAvailable) { ImGui::SameLine(); if (ImGui::Button("Open download page")) UpdateCheck::OpenDownloadPage(); }
                    bool checkUpdates = RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primeCheckUpdates, "WII_COMPILED_PRIME_CHECK_UPDATES", true);
                    if (ImGui::Checkbox("Check for a newer build at startup (one request to api.github.com)", &checkUpdates))
                        RuntimeConfigFile::SetPrimeOption(&RuntimeUserConfig::primeCheckUpdates, "check_updates", checkUpdates);
                }
                ImGui::Separator();
                if (const auto exe = RuntimeConfigFile::ExecutableDirectory()) ImGui::TextWrapped("Folder: %s", RuntimeConfigFile::PathToUtf8(*exe).c_str());
                ImGui::TextWrapped("Settings: %s", RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::ResolveConfigPath()).c_str());
                ImGui::TextWrapped("Disc image: %s", RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::ResolvedPrimeIsoPath()).c_str());
                ImGui::TextWrapped("Reports: %s", BugReport::Directory().c_str());
                ImGui::TextDisabled("Every F12 report records this version in its file name and in system.txt.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Diagnostics", nullptr, tabFlags(4))) {
#if defined(MKW_PRIME_QA_TOOLS)
                ImGui::Checkbox("Performance panel (F9)", &g_showPerformanceDiagnostics);
#endif
#if defined(MKW_PRIME_QA_TOOLS)
                ImGui::TextDisabled("F8 records a %.0f s profile to Logs/profiles.",kF8ProfileSeconds);
                ImGui::Separator();
                ImGui::TextUnformatted("F7: captures the frame and its draws to Logs/captures.");
                ImGui::TextUnformatted("F5: captures 60 consecutive frames (one second, for flicker).");
                ImGui::Separator();
#endif
                if (ImGui::Button("Create bug report (F12)")) BugReport::Create("F1 panel");
                ImGui::SameLine();
                ImGui::TextDisabled("Screenshot + this run's log + settings + system info, zipped in UserData/Reports.");
                if (const std::string status = BugReport::Status(); !status.empty()) ImGui::TextWrapped("%s", status.c_str());
                ImGui::Separator();
                DiscImageSection();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
            openTab = -1;
        }
        ImGui::Separator();
        ImGui::TextDisabled("F1 closes this panel. Changes are saved to UserData/Config.toml.");
        ImGui::TextDisabled("%s %s - created by %s. F12 saves a bug report.", PRIME_PORT_NAME, PRIME_PORT_VERSION, PRIME_PORT_AUTHOR);
    }
    ImGui::End();
}

void UpdateCursorAutoHide() {
    const bool overlayOpen = g_topBarVisible || g_primeOptionsOpen || g_achievementsOpen;
    const bool shouldHide = !overlayOpen &&
        (g_primeHideCursor || Clock::now() - g_lastMouseActivity >= kCursorAutoHideDelay);
    if (shouldHide == g_cursorHidden) {
        return;
    }
    g_cursorHidden = shouldHide;
    // ImGui_ImplSDL3_NewFrame calls SDL_ShowCursor every frame unless this flag is set.
    if (shouldHide) {
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SDL_HideCursor();
    } else {
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor();
    }
}

// Alt+Enter toggles the display mode inside aurora without going through the
// F10 combo, so the active mode is compared against the last persisted one
// every frame and written back on change.
void PersistDisplayModeIfChanged() {
    const int active = static_cast<int>(aurora_get_display_mode());
    if (active == g_displayMode) {
        return;
    }
    g_displayMode = active;
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(active)]));
}

void ApplyInputBlockState() {
    const bool blocked = controller_mapping_wizard::IsActive() || g_rebind.active ||
                         g_exitPromptOpen || g_topBarVisible || g_primeOptionsOpen || g_achievementsOpen ||
                         StartupScreenVisible();
    PADBlockInput(blocked);
    InputBindings::SetInputBlocked(blocked);
}
} // namespace

void InitializeRuntimeSettings() noexcept {
    PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
    InputBindings::Reload();
    controller_mapping_wizard::LoadPersistedMappings();
    ApplyConfiguredMappings();
    AudioBackend::Instance().SetMasterVolume(static_cast<float>(g_audioVolumePercent) / 100.0f);
    AudioBackend::Instance().SetMuted(g_audioMuted);
    MusicAttenuation::SetMusicVolume(static_cast<float>(g_musicVolumePercent) / 100.0f);
    MusicAttenuation::SetSoundEffectsVolume(static_cast<float>(g_soundEffectsVolumePercent) / 100.0f);
    MusicAttenuation::SetUiVolume(static_cast<float>(g_uiVolumePercent) / 100.0f);
    MusicAttenuation::SetVoicesVolume(static_cast<float>(g_voicesVolumePercent) / 100.0f);
    MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
#if defined(__APPLE__)
    aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
#endif
    LimitResolutionForFrameRate();
    aurora_set_frame_interpolation_low_latency(RuntimeConfigFile::FrameInterpolationLowLatency(false));
    aurora_set_frame_interpolation_display_sync(RuntimeConfigFile::FrameInterpolationDisplaySync(true));
    aurora_set_frame_interpolation_fps(FrameInterpolationTarget());
    aurora_set_vsync(RuntimeConfigFile::Vsync(false));
    aurora_set_display_mode(static_cast<AuroraDisplayMode>(g_displayMode));
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
    aurora_set_ubershader(g_ubershader);
    g_strapInputAccepted.store(false, std::memory_order_relaxed);
    g_startupDismissFrame.store(UINT64_MAX, std::memory_order_relaxed);
    g_bootShadersReady.store(false, std::memory_order_relaxed);
    g_bootShaderNotice = false;
    g_bootShaderWaitStart = {};
    PADBlockInput(false);
    InputBindings::SetInputBlocked(false);
}

#if (defined(_WIN32) || defined(__linux__)) && defined(MKW_PRIME_QA_TOOLS)
// The guest thread plus, when enabled, the GX consumer thread.
std::vector<uint32_t> ProfiledThreads() {
#if defined(_WIN32)
    std::vector<uint32_t> threads{GetCurrentThreadId()};
#else
    std::vector<uint32_t> threads{aurora::host_cores::native_thread_id()};
#endif
    if (const uint32_t gpu = GxGpuThread::NativeThreadId()) threads.push_back(gpu);
    if (const uint32_t decoder = aurora_get_decoder_thread_id()) threads.push_back(decoder);
    // Frame worker (encode, pipeline waits) and presenter.
    uint32_t render[2] = {};
    const uint32_t renderCount = aurora_get_render_thread_ids(render, 2);
    threads.insert(threads.end(), render, render + renderCount);
    return threads;
}
#endif

void HandleEvents(const AuroraEvent* events) noexcept {
#if (defined(_WIN32) || defined(__linux__)) && defined(MKW_PRIME_QA_TOOLS)
    // WII_COMPILED_STACK_PROFILE_AT=<seconds>: one unattended capture, for
    // validating the profiler without input.
    static const auto profileAt = [] {
        const char* value = std::getenv("WII_COMPILED_STACK_PROFILE_AT");
        return value ? std::chrono::steady_clock::now() + std::chrono::seconds(std::atoi(value))
                     : std::chrono::steady_clock::time_point::max();
    }();
    // WII_COMPILED_STACK_PROFILE_BELOW_FPS=<fps>: after that time, wait for the
    // presented rate to fall below the threshold (a heavy scene) before capturing.
    static const double profileBelowFps = [] {
        const char* value = std::getenv("WII_COMPILED_STACK_PROFILE_BELOW_FPS");
        return value ? std::atof(value) : 0.0;
    }();
    static bool profileStarted = false;
    if (!profileStarted && std::chrono::steady_clock::now() >= profileAt) {
        AuroraPresentTiming timing{};
        aurora_get_present_timing(&timing);
        if (profileBelowFps <= 0.0 || (timing.sampleCount > 30 && timing.framesPerSecond < profileBelowFps)) {
            profileStarted = StackProfiler::Start(ProfiledThreads(), 10.0,
                                                  RuntimeConfigFile::ApplicationDataDirectory() / "Logs" / "profiles");
        }
    }
#endif
#if defined(MKW_PRIME_QA_TOOLS)
    // Draw dumps for rendering diagnosis: F7, or a "request" file dropped in
    // Logs/captures by an assisting session (polled twice a second).
    {
        static auto nextPoll = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextPoll) {
            nextPoll = now + std::chrono::milliseconds(500);
            std::error_code error;
            if (std::filesystem::remove(DrawDumpDirectory() / "request", error)) RequestDrawDump("request");
        }
    }
#endif
    if (!events) {
        return;
    }
    for (const AuroraEvent* ev = events; ev->type != AURORA_NONE; ++ev) {
        if (ev->type == AURORA_CONTROLLER_ADDED || ev->type == AURORA_CONTROLLER_REMOVED) {
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
        }
        if (ev->type != AURORA_SDL_EVENT) {
            continue;
        }
        controller_mapping_wizard::HandleSdlEvent(ev->sdl);
        if (g_rebind.active && (IsToggleKey(ev->sdl, SDL_SCANCODE_BACKSPACE) ||
                                IsToggleKey(ev->sdl, SDL_SCANCODE_DELETE))) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F10)) {
            SetTopBarVisible(!g_topBarVisible);
            ApplyInputBlockState();
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F1)) {
            g_primeOptionsOpen = !g_primeOptionsOpen;
            ApplyInputBlockState();
        }
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        // F3: photo mode (freezes the game, keeps drawing); Right arrow steps one frame while on.
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F3)) {
            PrimePhotoMode::SetActive(!PrimePhotoMode::Active());
        }
        if (!g_rebind.active && PrimePhotoMode::Active() && IsToggleKey(ev->sdl, SDL_SCANCODE_RIGHT)) {
            PrimePhotoMode::StepFrame();
            ++g_photoStepsShown;
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F2)) {
            g_achievementsOpen = !g_achievementsOpen;
            ApplyInputBlockState();
        }
#endif
#if defined(MKW_PRIME_QA_TOOLS)
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F7)) {
            RequestDrawDump("F7");
        }
        // F5: eight consecutive frames, for flicker that lasts a frame.
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F5)) {
            RequestDrawDump("F5", 60);
        }
#endif
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F6)) {
            g_showRemoteMotion = !g_showRemoteMotion;
        }
#endif
#if (defined(_WIN32) || defined(__linux__)) && defined(MKW_PRIME_QA_TOOLS)
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F9)) {
            g_showPerformanceDiagnostics=!g_showPerformanceDiagnostics;
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F8)) {
            StackProfiler::Start(ProfiledThreads(), kF8ProfileSeconds,
                                 RuntimeConfigFile::ApplicationDataDirectory() / "Logs" / "profiles");
        }
#endif
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F12)) {
            BugReport::Create("F12");
        }
        // F11: screenshot of the game picture (with the image effects) to UserData/Screenshots.
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F11)) {
            Screenshot::Take();
        }
        if (!g_rebind.active && g_muteHotkey != PAD_KEY_INVALID &&
            IsToggleKey(ev->sdl, static_cast<SDL_Scancode>(g_muteHotkey))) {
            g_audioMuted = !g_audioMuted;
            AudioBackend::Instance().SetMuted(g_audioMuted);
            RuntimeConfigFile::SetAudioMuted(g_audioMuted);
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_ESCAPE)) {
            if (g_exitPromptOpen) {
                g_exitPromptOpen = false;
            } else if (g_topBarVisible) {
                SetTopBarVisible(false);
            } else {
                g_exitPromptOpen = true;
            }
            ApplyInputBlockState();
        }
        if (IsMouseActivity(ev->sdl)) {
            g_lastMouseActivity = Clock::now();
        }
    }
}

void ReleaseControllers() noexcept {
    // Aurora drives the LED white on first PADRead and never clears it, and the
    // exit paths terminate the process outright, so do it here.
    bool queued = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) continue;
        if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(static_cast<u32>(index))) {
            SDL_SetGamepadLED(pad, 0, 0, 0);
            queued = true;
        }
    }
    constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
        PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD};
    PADControlAllMotors(stopAll.data());
    // SDL hands LED and rumble reports to its own HIDAPI sender thread rather
    // than writing them here, so without this the process dies before the
    // controller ever receives them.
    if (queued) SDL_Delay(120);
}

#if defined(MKW_GAME_PROFILE_RM3E01_00)
namespace {
ImFont* g_hudFont = nullptr;  // Bahnschrift / Segoe UI with Latin-1 (accents, Ñ)

bool SpanishUi() {
#if defined(_WIN32)
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH;
#else
    return false;
#endif
}

std::string ToggleKeyLabel() {
    for (size_t action = 0; action < PrimeKeyBindings::ActionCount(); ++action) {
        if (std::string_view(PrimeKeyBindings::Action(action).id) != "mouse_look_toggle") continue;
        for (size_t slot = 0; slot < PrimeKeyBindings::kSlots; ++slot)
            if (const uint32_t code = PrimeKeyBindings::Binding(action, slot)) return PrimeKeyIcons::KeyLabel(code);
    }
    return {};
}

ImU32 Rgba(int r, int g, int b, float a) { return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f)); }

// Chamfered panel outline (Prime HUD style): corners cut at 45 degrees.
void ChamferPath(ImDrawList* dl, ImVec2 a, ImVec2 b, float cut) {
    dl->PathLineTo({a.x + cut, a.y});
    dl->PathLineTo({b.x - cut, a.y});
    dl->PathLineTo({b.x, a.y + cut});
    dl->PathLineTo({b.x, b.y - cut});
    dl->PathLineTo({b.x - cut, b.y});
    dl->PathLineTo({a.x + cut, b.y});
    dl->PathLineTo({a.x, b.y - cut});
    dl->PathLineTo({a.x, a.y + cut});
}

// Top-centre hint while the game is in first-person aiming (where mouse look
// applies): full for a few seconds after entering that state or toggling, then
// a compact status strip. Keyboard/mouse mode only; hidden under F1/F10.
void DrawMouseLookHint() {
    ++g_hintFrame;
    static bool wasEligible = false;
    static int64_t shownSinceNs = 0;
    static float alpha = 0.0f;
    // WII_COMPILED_PRIME_MOUSE_LOOK_HINT_PREVIEW=1 shows it anywhere (screenshots/QA of the look).
    static const bool preview = std::getenv("WII_COMPILED_PRIME_MOUSE_LOOK_HINT_PREVIEW") != nullptr;
    const bool editing = g_hintEditFrame + 2 >= g_hintFrame;
    const bool eligible = editing || (RuntimeConfigFile::PrimeKeyboardMouseMode() && (preview || PrimeMouseLook::Eligible()) &&
                                      !::PrimeFrontEndActive() &&
                                      !g_primeOptionsOpen && !g_topBarVisible);
    const int64_t now = PrimeMouseLook::NowNanoseconds();
    if (eligible && !wasEligible) shownSinceNs = now;
    wasEligible = eligible;
    const int64_t toggled = PrimeMouseLook::LastToggleNs();
    if (toggled > shownSinceNs) shownSinceNs = toggled;
    if (preview && now - shownSinceNs > 12'000'000'000LL) shownSinceNs = now;  // preview: cycle full / compact
    if (editing) shownSinceNs = now;                                         // sliders: show the full form
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
    alpha = editing ? 1.0f : std::clamp(alpha + (eligible ? dt : -dt) / 0.25f, 0.0f, 1.0f);
    if (alpha <= 0.0f) return;

    const auto& config = RuntimeConfigFile::Get();
    const float userScale = std::clamp(config.primeMouseLookHintScale.value_or(kHintDefaultScale), 0.4f, 2.0f);
    const float posX = std::clamp(config.primeMouseLookHintX.value_or(kHintDefaultX), 0.0f, 1.0f);
    const float posY = std::clamp(config.primeMouseLookHintY.value_or(kHintDefaultY), 0.0f, 1.0f);

    const bool on = PrimeMouseLook::Enabled();
    const bool spanish = SpanishUi();
    const std::string key = ToggleKeyLabel();
    const float sinceShown = static_cast<float>(now - shownSinceNs) / 1e9f;
    const bool full = sinceShown < 6.0f;
    const float sinceToggle = static_cast<float>(now - toggled) / 1e9f;
    const float flash = toggled > 0 && sinceToggle < 0.6f ? 1.0f - sinceToggle / 0.6f : 0.0f;

    ImFont* font = g_hudFont ? g_hudFont : ImGui::GetFont();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float s = std::clamp(display.y / 1080.0f, 0.6f, 2.0f) * userScale;
    const float titleSize = 20.0f * s, bodySize = 16.0f * s, keySize = 15.0f * s;

    const char* title = spanish ? "VISTA CON RAT\xC3\x93N" : "MOUSE LOOK";
    const char* state = on ? (spanish ? "ACTIVADA" : "ON") : (spanish ? "DESACTIVADA" : "OFF");
    const char* separator = "\xC2\xB7";
    std::string before, after;
    if (key.empty()) {
        before = spanish ? "Asigna una tecla en F1 > Controls" : "Assign a key in F1 > Controls";
    } else if (spanish) {
        before = "Pulsa";
        after = on ? "para desactivarla" : "para activarla";
    } else {
        before = "Press";
        after = on ? "to turn it off" : "to turn it on";
    }

    // One line: [lamp] TITLE · STATE   |   Press [key] to turn it off      (compact: [lamp] TITLE · STATE [key])
    const float padX = 12.0f * s, padY = 5.0f * s, gap = 7.0f * s, cut = 7.0f * s, lamp = 7.0f * s;
    const auto textW = [&](float size, const char* text) { return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x; };
    const float titleW = textW(titleSize, title), stateW = textW(titleSize, state), sepW = textW(titleSize, separator);
    const float keyTextW = textW(keySize, key.c_str());
    const float keyW = key.empty() ? 0.0f : keyTextW + 12.0f * s, keyH = keySize + 6.0f * s;
    const float lineH = std::max(titleSize, keyH);
    float content = lamp * 2 + gap + titleW + gap + sepW + gap + stateW;
    const float divider = 2.0f * gap + 1.0f;
    if (full) {
        content += divider + textW(bodySize, before.c_str());
        if (!key.empty()) content += gap + keyW + gap + textW(bodySize, after.c_str());
    } else if (!key.empty()) {
        content += gap * 1.5f + keyW;
    }
    const float width = content + padX * 2 + cut;
    const float height = lineH + padY * 2;
    ImVec2 a{std::floor(display.x * posX - width * 0.5f), std::floor(display.y * posY)};
    a.x = std::clamp(a.x, 0.0f, std::max(0.0f, display.x - width));
    a.y = std::clamp(a.y, 0.0f, std::max(0.0f, display.y - height));
    const ImVec2 b{a.x + width, a.y + height};
    const float k = alpha * (full ? 1.0f : 0.72f);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const int cr = 96, cg = 214, cb = 255;  // HUD cyan
    for (int i = 2; i >= 1; --i) {           // glow
        const float g = 2.0f * s * static_cast<float>(i);
        ChamferPath(dl, {a.x - g, a.y - g}, {b.x + g, b.y + g}, cut + g * 0.5f);
        dl->PathStroke(Rgba(cr, cg, cb, k * (0.06f + 0.12f * flash)), ImDrawFlags_Closed, 1.8f * s);
    }
    ChamferPath(dl, a, b, cut);
    dl->PathFillConvex(Rgba(6, 18, 30, k * 0.78f));
    for (float y = a.y + 3.0f * s; y < b.y - 2.0f * s; y += 3.0f * s)  // scan lines
        dl->AddLine({a.x + cut * 0.6f, y}, {b.x - cut * 0.6f, y}, Rgba(cr, cg, cb, k * 0.035f), 1.0f);
    ChamferPath(dl, a, b, cut);
    dl->PathStroke(Rgba(cr, cg, cb, k * (0.75f + 0.25f * flash)), ImDrawFlags_Closed, 1.4f * s);
    dl->AddLine({a.x + width * 0.35f, a.y}, {a.x + width * 0.65f, a.y}, Rgba(200, 245, 255, k * 0.9f), 2.0f * s);

    const float midY = a.y + height * 0.5f;
    const auto textAt = [&](float size, float x, ImU32 color, const char* text) {
        dl->AddText(font, size, {x, std::floor(midY - size * 0.5f)}, color, text);
    };
    const auto drawKey = [&](float kx) {
        const ImVec2 ka{kx, std::floor(midY - keyH * 0.5f)}, kb{kx + keyW, ka.y + keyH};
        ChamferPath(dl, ka, kb, 3.0f * s);
        dl->PathFillConvex(Rgba(cr, cg, cb, k * 0.16f));
        ChamferPath(dl, ka, kb, 3.0f * s);
        dl->PathStroke(Rgba(cr, cg, cb, k * 0.9f), ImDrawFlags_Closed, 1.2f * s);
        dl->AddText(font, keySize, {kx + (keyW - keyTextW) * 0.5f, std::floor(midY - keySize * 0.5f)},
                    Rgba(235, 252, 255, k), key.c_str());
    };

    float x = a.x + padX + cut * 0.5f;
    const ImVec2 lampCenter{x + lamp, midY};
    const ImU32 lampColor = on ? Rgba(90, 255, 170, k) : Rgba(255, 140, 60, k);
    dl->AddCircleFilled(lampCenter, lamp * 1.6f, on ? Rgba(90, 255, 170, k * 0.18f) : Rgba(255, 140, 60, k * 0.18f), 20);
    dl->AddQuadFilled({lampCenter.x, lampCenter.y - lamp}, {lampCenter.x + lamp, lampCenter.y},
                      {lampCenter.x, lampCenter.y + lamp}, {lampCenter.x - lamp, lampCenter.y}, lampColor);
    x += lamp * 2 + gap;
    textAt(titleSize, x, Rgba(cr, cg, cb, k), title);
    x += titleW + gap;
    textAt(titleSize, x, Rgba(cr, cg, cb, k * 0.6f), separator);
    x += sepW + gap;
    textAt(titleSize, x, on ? Rgba(170, 255, 215, k) : Rgba(255, 190, 130, k), state);
    x += stateW;
    if (full) {
        x += gap;
        dl->AddLine({x, a.y + padY + 2.0f * s}, {x, b.y - padY - 2.0f * s}, Rgba(cr, cg, cb, k * 0.45f), 1.0f);
        x += gap + 1.0f;
        const ImU32 body = Rgba(190, 236, 255, k * 0.92f);
        textAt(bodySize, x, body, before.c_str());
        if (!key.empty()) {
            x += textW(bodySize, before.c_str()) + gap;
            drawKey(x);
            x += keyW + gap;
            textAt(bodySize, x, body, after.c_str());
        }
    } else if (!key.empty()) {
        drawKey(x + gap * 1.5f);
    }
}

// Photo mode indicator (top left, HUD style): shown for a few seconds after entering it or
// stepping a frame, then gone so screenshots (F11) stay clean.
void DrawPhotoModeIndicator() {
    static bool wasActive = false;
    static double shownAt = -100.0;
    static uint64_t lastSteps = 0;
    const bool active = PrimePhotoMode::Active();
    const double now = ImGui::GetTime();
    if (active && !wasActive) shownAt = now;
    wasActive = active;
    if (g_photoStepsShown != lastSteps) { lastSteps = g_photoStepsShown; shownAt = now; }
    const double age = now - shownAt;
    if (!active || age > 4.0) return;
    const float k = static_cast<float>(std::min(1.0, (4.0 - age) / 0.6));
    ImFont* font = g_hudFont ? g_hudFont : ImGui::GetFont();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float s = std::clamp(display.y / 1080.0f, 0.6f, 2.0f);
    const char* title = "PHOTO MODE";
    const char* help = "F3 resume  Â·  Right arrow: next frame  Â·  F11 screenshot  Â·  F1 materials";
    const float titleSize = 20.0f * s, helpSize = 15.0f * s, pad = 12.0f * s, cut = 8.0f * s;
    const float w = std::max(font->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, title).x,
                             font->CalcTextSizeA(helpSize, FLT_MAX, 0.0f, help).x) + pad * 2 + cut;
    const float h = titleSize + helpSize + pad * 2 + 4.0f * s;
    const ImVec2 a{24.0f * s, 24.0f * s}, b{a.x + w, a.y + h};
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ChamferPath(dl, a, b, cut);
    dl->PathFillConvex(Rgba(6, 18, 30, k * 0.8f));
    ChamferPath(dl, a, b, cut);
    dl->PathStroke(Rgba(96, 214, 255, k * 0.85f), ImDrawFlags_Closed, 1.4f * s);
    dl->AddCircleFilled({a.x + pad + 5.0f * s, a.y + pad + titleSize * 0.5f}, 5.0f * s, Rgba(255, 150, 60, k), 16);
    dl->AddText(font, titleSize, {a.x + pad + 16.0f * s, a.y + pad}, Rgba(96, 214, 255, k), title);
    dl->AddText(font, helpSize, {a.x + pad, a.y + pad + titleSize + 4.0f * s}, Rgba(200, 235, 250, k * 0.9f), help);
}
}  // namespace
#endif

void ImGuiInit(const AuroraWindowSize*) noexcept {
#if defined(MKW_GAME_PROFILE_RM3E01_00) && defined(_WIN32)
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();  // keep the panel's font as the default
    char windows[MAX_PATH] = {};
    if (GetWindowsDirectoryA(windows, MAX_PATH) == 0) return;
    for (const char* file : {"bahnschrift.ttf", "segoeuib.ttf", "arialbd.ttf"}) {
        const std::filesystem::path path = std::filesystem::path(windows) / "Fonts" / file;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 2;
        g_hudFont = io.Fonts->AddFontFromFileTTF(path.string().c_str(), 34.0f, &config, io.Fonts->GetGlyphRangesDefault());
        if (g_hudFont) {
            RT_LOG(RT_TAG_RUNTIME) << "[mouse-look-hint] HUD font " << file << std::endl;
            break;
        }
    }
#endif
}

namespace {
// ImGui reports text-input focus (SDL_StartTextInput / SDL_SetTextInputArea,
// Windows IME) from ImGui::Render, which runs on Aurora's frame worker. Those
// calls need the window's thread, the game thread, which may be waiting for
// that same frame worker: the first text field (RetroAchievements login)
// deadlocked both. The frame worker only records the request; Draw applies it
// on the window's thread.
using ImeDataFn = void (*)(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData*);
ImeDataFn g_backendSetImeData = nullptr;
std::mutex g_imeMutex;
bool g_imePending = false;
ImGuiContext* g_imeContext = nullptr;
ImGuiViewport* g_imeViewport = nullptr;
ImGuiPlatformImeData g_imeData{};

void DeferredSetImeData(ImGuiContext* context, ImGuiViewport* viewport, ImGuiPlatformImeData* data) {
    std::lock_guard lock(g_imeMutex);
    g_imeContext = context;
    g_imeViewport = viewport;
    g_imeData = *data;
    g_imePending = true;
}

// Window thread, after the frame worker reached DONE.
void ApplyDeferredImeData() {
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    if (platform.Platform_SetImeDataFn != DeferredSetImeData) {
        g_backendSetImeData = platform.Platform_SetImeDataFn;
        platform.Platform_SetImeDataFn = DeferredSetImeData;
    }
    ImGuiPlatformImeData data;
    ImGuiContext* context;
    ImGuiViewport* viewport;
    {
        std::lock_guard lock(g_imeMutex);
        if (!g_imePending) return;
        g_imePending = false;
        data = g_imeData;
        context = g_imeContext;
        viewport = g_imeViewport;
    }
    if (g_backendSetImeData && viewport) g_backendSetImeData(context, viewport, &data);
}
}  // namespace

void Draw() noexcept {
    // Wait for the frame worker's DONE phase: it has replayed the previous frame's ImGui draw lists
    // and started the next ImGui frame, so all overlay callers can now safely issue ImGui commands.
    aurora_wait_for_frame_worker();
    ApplyDeferredImeData();
    FollowMonitorForFrameInterpolation();
    // Also drive the Wii Remote rescan from here: PADRead runs it too, but this
    // runs once per presented frame whatever the game is doing (e.g. sitting in
    // its "communications interrupted" prompt without polling pads). Same guest
    // thread as PADRead, so no concurrent access to the scanner's state.
    WiiRemoteInput::Poll();
    ApplyConfiguredMappings();
    PersistDisplayModeIfChanged();
    UpdateCursorAutoHide();
    UpdateBootShaderState();
    if (!StartupScreenVisible()) {
        DrawShaderCompilationStatus();
    }
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    const bool photoMode = PrimePhotoMode::Active();
#else
    const bool photoMode = false;
#endif
    if (!photoMode) DrawFpsOverlay();
    DrawUpdateNotice();
    // ReShade effects: only on the 3D scene, never while a port panel is on screen.
    ReShadeLoader::SetEffectsAllowed(aurora_post_process_scene_active() && !g_primeOptionsOpen && !g_achievementsOpen &&
                                     !g_topBarVisible &&
                                     !g_exitPromptOpen
#if defined(MKW_PRIME_QA_TOOLS)
                                     && !g_showPerformanceDiagnostics
#endif
#if defined(MKW_GAME_PROFILE_RM3E01_00)
                                     && !g_showRemoteMotion
#endif
    );
#if defined(MKW_PRIME_QA_TOOLS)
    {
        // Test hook: WII_COMPILED_PRIME_TEST_REPORT_AFTER=<seconds> creates one bug report unattended.
        static const char* after = std::getenv("WII_COMPILED_PRIME_TEST_REPORT_AFTER");
        static const auto started = std::chrono::steady_clock::now();
        static bool done = false;
        if (after && !done && std::chrono::steady_clock::now() - started > std::chrono::seconds(std::atoi(after))) {
            done = true;
            BugReport::Create("test");
        }
    }
    DrawPerformanceDiagnostics();
    DrawProfileRecordingNotice();
#endif
    DrawScreenshotNotice();
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    DrawRemoteMotionPanel();
    if (!photoMode) {
        DrawMouseLookHint();
        PrimeAchievementsUi::DrawHud(g_hudFont);
    }
    DrawPhotoModeIndicator();
#endif
    DrawTopBar();
    DrawPrimeOptionsPanel();
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    PrimeAchievementsUi::DrawPanel(&g_achievementsOpen);
#endif
    DrawExitPrompt();
    controller_mapping_wizard::Draw();
    ApplyInputBlockState();
    DrawStartupScreen();
}

bool StartupScreenVisible() noexcept {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    // The upstream cover waits for MKW's strap-input event, which Prime never
    // emits. Its own startup must remain visible through the guest framebuffer.
    return false;
#endif
    return !g_strapInputAccepted.load(std::memory_order_acquire) ||
           !g_bootShadersReady.load(std::memory_order_acquire) ||
           g_presentedFrame < g_startupDismissFrame.load(std::memory_order_relaxed);
}

void NotifyStrapInputAccepted() noexcept {
    bool expected = false;
    if (g_strapInputAccepted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        g_startupDismissFrame.store(g_presentedFrame + kStrapTransitionCoverFrames,
                                    std::memory_order_release);
    }
}

void AdvancePresentedFrame() noexcept { ++g_presentedFrame; }
} // namespace settings_overlay
