// RM3E01 gamepad layer: SDL3 pads drive the keyboard mode's virtual Wii Remote
// + Nunchuk (see hle/prime_gamepad_runtime.h and recomp/docs/mando_gamepad.md).
#if defined(MKW_GAME_PROFILE_RM3E01_00)
#include "hle/prime_gamepad_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>

#include "hle/prime_key_bindings.h"
#include "hle/prime_key_icons.h"
#include "runtime_config.h"

#include <aurora/aurora.h>
#include <thread>
#include "runtime_log.h"

namespace {
using PadInput = PrimeGamepad::Input;

std::mutex g_padMutex;  // bindings, capture, device arbitration, settings
std::atomic<SDL_JoystickID> g_activePad{0};
std::atomic<bool> g_padEnabledLoaded{false}, g_padEnabled{true};
PrimeGamepad::ActiveDevice g_padDevice;
PrimeGamepad::Style g_padStyle = PrimeGamepad::Style::Generic;
bool g_padStyleChanged = false;
PrimeGamepadRuntime::Settings g_padSettings;
bool g_padSettingsLoaded = false;

std::vector<std::array<PadInput, PrimePadBindings::kSlots>> g_padBindings;
std::vector<PadInput> g_padLayerBindings;  // motion layer, one input per action
bool g_padBindingsLoaded = false;
int g_padCaptureAction = -1, g_padCaptureSlot = 0;
// Game thread: digital state of every input, for the analog hysteresis.
std::array<bool, PrimeGamepad::kInputCount> g_padInputDown{};
PrimeGamepad::LayerRouting g_padRouting;

double PadNowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Real Wii Remotes reach SDL through its HIDAPI Wii driver as gamepads too; in
// keyboard mode they are not a pad of this layer.
bool PadIsWiiRemote(SDL_JoystickID id) {
    return SDL_GetGamepadVendorForID(id) == 0x057E &&
           (SDL_GetGamepadProductForID(id) == 0x0306 || SDL_GetGamepadProductForID(id) == 0x0330);
}

size_t PadKeyActionCount() { return std::min(PrimeKeyBindings::ActionCount(), PrimeGamepadRuntime::kMaxActions - 1); }
size_t PadMotionAction() { return PadKeyActionCount(); }

void PadLoadSettingsLocked() {
    if (g_padSettingsLoaded) return;
    g_padSettingsLoaded = true;
    const auto& config = RuntimeConfigFile::Get();
    g_padSettings.lookDegPerSecond = std::clamp(config.primePadLookSpeed.value_or(PrimeGamepad::kDefaultLookDegPerSecond), 30.0f, 720.0f);
    g_padSettings.lookCurve = std::clamp(config.primePadLookCurve.value_or(PrimeGamepad::kDefaultLookExponent), 1.0f, 3.0f);
    g_padSettings.pointerSpeed = std::clamp(config.primePadPointerSpeed.value_or(PrimeGamepad::kDefaultPointerUnitsPerSecond), 0.3f, 5.0f);
    g_padSettings.deadzone = std::clamp(config.primePadDeadzone.value_or(PrimeGamepad::kDefaultInnerDeadzone), 0.0f, 0.5f);
    g_padSettings.invertY = config.primePadInvertY.value_or(false);
}

std::array<PadInput, PrimePadBindings::kSlots> PadDefaultsFor(const char* id) {
    for (const auto& binding : PrimeGamepad::kDefaultBindings)
        if (std::strcmp(binding.action, id) == 0) return {binding.slots[0], binding.slots[1]};
    return {PadInput::None, PadInput::None};
}
PadInput PadLayerDefaultFor(const char* id) {
    for (const auto& binding : PrimeGamepad::kDefaultLayerBindings)
        if (std::strcmp(binding.action, id) == 0) return binding.slots[0];
    return PadInput::None;
}

void PadLoadBindingsLocked() {
    if (g_padBindingsLoaded) return;
    g_padBindingsLoaded = true;
    g_padBindings.assign(PrimePadBindings::ActionCount(), {PadInput::None, PadInput::None});
    g_padLayerBindings.assign(PrimePadBindings::ActionCount(), PadInput::None);
    const auto& configured = RuntimeConfigFile::Get().primePadBindings;
    const auto& layered = RuntimeConfigFile::Get().primePadLayerBindings;
    for (size_t i = 0; i < g_padBindings.size(); ++i) {
        const char* id = PrimePadBindings::ActionId(i);
        g_padBindings[i] = PadDefaultsFor(id);
        if (i != PadMotionAction()) g_padLayerBindings[i] = PadLayerDefaultFor(id);
        if (const auto it = layered.find(id); it != layered.end() && i != PadMotionAction()) {
            const PadInput input = it->second.empty() ? PadInput::None : PrimeGamepad::InputFromName(it->second[0]);
            g_padLayerBindings[i] = PrimeGamepad::LayerInputAllowed(input) ? input : PadInput::None;
        }
        const auto it = configured.find(id);
        if (it == configured.end()) continue;
        g_padBindings[i] = {PadInput::None, PadInput::None};
        for (size_t slot = 0; slot < PrimePadBindings::kSlots && slot < it->second.size(); ++slot)
            g_padBindings[i][slot] = PrimeGamepad::InputFromName(it->second[slot]);
    }
}

// The motion button doubles as the layer's shift.
bool PadIsShiftLocked(PadInput input) {
    const size_t motion = PadMotionAction();
    return input != PadInput::None && motion < g_padBindings.size() &&
           (g_padBindings[motion][0] == input || g_padBindings[motion][1] == input);
}

void PadSaveLayerLocked(size_t action) {
    const char* id = PrimePadBindings::ActionId(action);
    const PadInput input = g_padLayerBindings[action];
    std::vector<std::string> names;
    if (input != PadInput::None) names.emplace_back(PrimeGamepad::InputName(input));
    const std::string value = input == PadInput::None ? "[]" : std::string("[\"") + PrimeGamepad::InputName(input) + "\"]";
    RuntimeConfigFile::Mutable().primePadLayerBindings[id] = std::move(names);
    RuntimeConfigFile::WriteSetting("prime_pad_motion", id, value);
}

void PadSaveBindingLocked(size_t action) {
    std::string value = "[";
    std::vector<std::string> names;
    for (const PadInput input : g_padBindings[action]) {
        if (input == PadInput::None) continue;
        if (value.size() > 1) value += ", ";
        value += '"';
        value += PrimeGamepad::InputName(input);
        value += '"';
        names.emplace_back(PrimeGamepad::InputName(input));
    }
    value += "]";
    const char* id = PrimePadBindings::ActionId(action);
    RuntimeConfigFile::Mutable().primePadBindings[id] = std::move(names);
    RuntimeConfigFile::WriteSetting("prime_pad", id, value);
}

// Prompt icons: styles whose files are current, a draw in progress, and a
// re-check once it lands (the device may have changed meanwhile).
std::array<std::atomic<bool>, 5> g_padIconsReady{};
std::atomic<bool> g_padIconsBusy{false}, g_padIconsRecheck{false};
std::atomic<uint32_t> g_padIconsGeneration{0};  // bumped by every binding change
void PadIconsInvalidate() {
    g_padIconsGeneration.fetch_add(1);
    for (auto& ready : g_padIconsReady) ready.store(false);
    g_padIconsRecheck.store(true);
}

// A layer slot takes buttons and triggers other than the motion button, on any
// action but the motion mode itself.
bool PadLayerAcceptsLocked(size_t action, PadInput input) {
    return input == PadInput::None ||
           (action != PadMotionAction() && PrimeGamepad::LayerInputAllowed(input) && !PadIsShiftLocked(input));
}

void PadAssignLocked(size_t action, size_t slot, PadInput input) {
    if (slot == PrimePadBindings::kLayerSlot) {
        if (!PadLayerAcceptsLocked(action, input)) return;
        // One layer input drives one layer action.
        if (input != PadInput::None)
            for (size_t i = 0; i < g_padLayerBindings.size(); ++i)
                if (i != action && g_padLayerBindings[i] == input) {
                    g_padLayerBindings[i] = PadInput::None;
                    PadSaveLayerLocked(i);
                }
        g_padLayerBindings[action] = input;
        PadSaveLayerLocked(action);
        PadIconsInvalidate();
        RT_LOG(RT_TAG_RUNTIME) << "[prime-pad-binding] " << PrimePadBindings::ActionId(action) << "[motion] = "
                               << PrimeGamepad::InputName(input) << std::endl;
        return;
    }
    // A new motion button is the shift: it cannot also be a layer input.
    if (action == PadMotionAction() && input != PadInput::None)
        for (size_t i = 0; i < g_padLayerBindings.size(); ++i)
            if (g_padLayerBindings[i] == input) {
                g_padLayerBindings[i] = PadInput::None;
                PadSaveLayerLocked(i);
            }
    // One input drives one action: take it away from wherever it was bound.
    if (input != PadInput::None)
        for (size_t i = 0; i < g_padBindings.size(); ++i)
            for (size_t other = 0; other < PrimePadBindings::kSlots; ++other)
                if (g_padBindings[i][other] == input && (i != action || other != slot)) {
                    g_padBindings[i][other] = PadInput::None;
                    PadSaveBindingLocked(i);
                }
    g_padBindings[action][slot] = input;
    PadSaveBindingLocked(action);
    PadIconsInvalidate();
    RT_LOG(RT_TAG_RUNTIME) << "[prime-pad-binding] " << PrimePadBindings::ActionId(action) << "[" << slot << "] = "
                           << PrimeGamepad::InputName(input) << std::endl;
}

void PickPad() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    SDL_JoystickID chosen = 0;
    for (int i = 0; ids && i < count; ++i)
        if (!PadIsWiiRemote(ids[i]) && SDL_GetGamepadFromID(ids[i]) != nullptr) { chosen = ids[i]; break; }
    SDL_free(ids);
    g_activePad.store(chosen);
}

void NotePadUse(SDL_JoystickID which) {
    if (which != 0 && which != g_activePad.load() && !PadIsWiiRemote(which) && SDL_GetGamepadFromID(which)) {
        g_activePad.store(which);  // the pad the player just used
        RT_LOG(RT_TAG_RUNTIME) << "[prime-pad] active pad: " << (SDL_GetGamepadNameForID(which) ? SDL_GetGamepadNameForID(which) : "?")
                               << std::endl;
    }
}

void PadCaptureLocked(PadInput input) {
    if (g_padCaptureAction < 0 || input == PadInput::None) return;
    const size_t action = static_cast<size_t>(g_padCaptureAction), slot = static_cast<size_t>(g_padCaptureSlot);
    // A layer cell keeps waiting past the motion button (players press it out of
    // habit before the button they mean) and past stick pushes.
    if (slot == PrimePadBindings::kLayerSlot && !PadLayerAcceptsLocked(action, input)) return;
    g_padCaptureAction = -1;
    PadAssignLocked(action, slot, input);
}
}  // namespace

namespace PrimeGamepadRuntime {

bool Enabled() {
    if (!g_padEnabledLoaded.exchange(true))
        g_padEnabled.store(RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primeGamepad, "WII_COMPILED_PRIME_GAMEPAD", true));
    return g_padEnabled.load();
}
void SetEnabled(bool enabled) {
    Enabled();
    g_padEnabled.store(enabled);
}

void HandleEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!PadIsWiiRemote(event.gdevice.which)) {
            if (g_activePad.load() == 0 || !SDL_GetGamepadFromID(g_activePad.load())) PickPad();
            const char* name = SDL_GetGamepadNameForID(event.gdevice.which);
            RT_LOG(RT_TAG_RUNTIME) << "[prime-pad] connected: " << (name ? name : "?") << " type="
                                   << SDL_GetGamepadTypeForID(event.gdevice.which) << std::endl;
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (event.gdevice.which == g_activePad.load()) {
            g_activePad.store(0);
            PickPad();
            if (g_activePad.load() == 0) {
                std::lock_guard lock(g_padMutex);
                g_padDevice.NotePadRemoved();
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        NotePadUse(event.gbutton.which);
        std::lock_guard lock(g_padMutex);
        PadCaptureLocked(static_cast<PadInput>(event.gbutton.button + 1));
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const float value = event.gaxis.value / 32767.0f;
        if (std::fabs(value) < 0.6f) break;
        NotePadUse(event.gaxis.which);
        PadInput input = PadInput::None;
        switch (event.gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: input = PadInput::LeftTrigger; break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: input = PadInput::RightTrigger; break;
        case SDL_GAMEPAD_AXIS_LEFTX: input = value < 0 ? PadInput::LeftStickLeft : PadInput::LeftStickRight; break;
        case SDL_GAMEPAD_AXIS_LEFTY: input = value < 0 ? PadInput::LeftStickUp : PadInput::LeftStickDown; break;  // SDL y grows down
        case SDL_GAMEPAD_AXIS_RIGHTX: input = value < 0 ? PadInput::RightStickLeft : PadInput::RightStickRight; break;
        case SDL_GAMEPAD_AXIS_RIGHTY: input = value < 0 ? PadInput::RightStickUp : PadInput::RightStickDown; break;
        default: break;
        }
        if (value < 0 && (input == PadInput::LeftTrigger || input == PadInput::RightTrigger)) input = PadInput::None;
        std::lock_guard lock(g_padMutex);
        PadCaptureLocked(input);
        break;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        std::lock_guard lock(g_padMutex);
        g_padDevice.NoteKeyOrClick();
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        std::lock_guard lock(g_padMutex);
        g_padDevice.NoteMouseMotion(std::hypot(event.motion.xrel, event.motion.yrel), PadNowSeconds());
        break;
    }
    default:
        break;
    }
}

bool Poll(Frame& frame) {
    frame = {};
    if (!Enabled()) return false;
    SDL_Gamepad* gamepad = SDL_GetGamepadFromID(g_activePad.load());
    if (!gamepad) return false;
    PrimeGamepad::PadState pad;
    pad.connected = true;
    for (size_t i = 0; i < PrimeGamepad::kButtonCount; ++i)
        pad.buttons[i] = SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(i));
    const auto axis = [&](SDL_GamepadAxis a) { return std::clamp(SDL_GetGamepadAxis(gamepad, a) / 32767.0f, -1.0f, 1.0f); };
    pad.leftX = axis(SDL_GAMEPAD_AXIS_LEFTX);
    pad.leftY = -axis(SDL_GAMEPAD_AXIS_LEFTY);
    pad.rightX = axis(SDL_GAMEPAD_AXIS_RIGHTX);
    pad.rightY = -axis(SDL_GAMEPAD_AXIS_RIGHTY);
    pad.leftTrigger = std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0.0f);
    pad.rightTrigger = std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0.0f);

    std::lock_guard lock(g_padMutex);
    PadLoadSettingsLocked();
    PadLoadBindingsLocked();
    g_padDevice.NotePad(pad);
    const auto style = PrimeGamepad::StyleForSdlType(SDL_GetGamepadType(gamepad));
    if (style != g_padStyle) {
        g_padStyle = style;
        g_padStyleChanged = true;
    }
    for (size_t i = 1; i < PrimeGamepad::kInputCount; ++i)
        g_padInputDown[i] = PrimeGamepad::Down(pad, static_cast<PadInput>(i), g_padInputDown[i]);
    // Motion layer: while the motion button is held, an input with a layer
    // binding drives that action instead of its normal one.
    bool shift = false;
    std::array<bool, PrimeGamepad::kInputCount> hasLayer{};
    for (const PadInput input : g_padLayerBindings)
        if (input != PadInput::None && !PadIsShiftLocked(input)) hasLayer[static_cast<size_t>(input)] = true;
    if (PadMotionAction() < g_padBindings.size())
        for (const PadInput input : g_padBindings[PadMotionAction()])
            shift = shift || (input != PadInput::None && g_padInputDown[static_cast<size_t>(input)]);
    g_padRouting.Update(g_padInputDown, shift, hasLayer);
    for (size_t action = 0; action < g_padBindings.size() && action < kMaxActions; ++action) {
        for (const PadInput input : g_padBindings[action])
            if (input != PadInput::None && g_padInputDown[static_cast<size_t>(input)] && !g_padRouting.Layered(input))
                frame.actionDown[action] = true;
        const PadInput layer = action < g_padLayerBindings.size() ? g_padLayerBindings[action] : PadInput::None;
        if (layer != PadInput::None && g_padInputDown[static_cast<size_t>(layer)] && g_padRouting.Layered(layer))
            frame.actionDown[action] = true;
    }

    const float inner = g_padSettings.deadzone;
    const auto left = PrimeGamepad::RadialDeadzone(pad.leftX, pad.leftY, inner, PrimeGamepad::kDefaultOuterDeadzone);
    const auto right = PrimeGamepad::RadialDeadzone(pad.rightX, pad.rightY, inner, PrimeGamepad::kDefaultOuterDeadzone);
    // A stick direction bound to an action is a button, not also an analog axis.
    const auto boundTo = [&](PadInput a, PadInput b, PadInput c, PadInput d) {
        for (const auto& slots : g_padBindings)
            for (const PadInput input : slots)
                if (input == a || input == b || input == c || input == d) return true;
        return false;
    };
    if (!boundTo(PadInput::LeftStickUp, PadInput::LeftStickDown, PadInput::LeftStickLeft, PadInput::LeftStickRight)) {
        frame.analog.stickX = left.x;
        frame.analog.stickY = left.y;
    }
    const bool rightFree = !boundTo(PadInput::RightStickUp, PadInput::RightStickDown, PadInput::RightStickLeft, PadInput::RightStickRight);
    frame.analog.motion = PadMotionAction() < kMaxActions && frame.actionDown[PadMotionAction()];
    if (frame.analog.motion) {
        if (rightFree) PrimeGamepad::MotionFromStick(right, frame.analog.twistTarget, frame.analog.depthRate);
    } else if (rightFree) {
        frame.right = right;
    }
    frame.connected = true;
    return true;
}

PrimeGamepad::Device CurrentDevice() {
    std::lock_guard lock(g_padMutex);
    return g_padDevice.Current();
}
PrimeGamepad::Style CurrentStyle() {
    std::lock_guard lock(g_padMutex);
    return g_padStyle;
}
bool TakeIconChange() {
    std::lock_guard lock(g_padMutex);
    const bool changed = g_padDevice.TakeChanged() || g_padStyleChanged;
    g_padStyleChanged = false;
    return changed;
}
bool IconsEnabled() {
    static const bool enabled =
        RuntimeConfigFile::PrimeOption(&RuntimeUserConfig::primePadIcons, "WII_COMPILED_PRIME_PAD_ICONS", true);
    return enabled && Enabled();
}

void ServiceIcons() {
    if (!IconsEnabled()) return;
    const bool changed = TakeIconChange() | g_padIconsRecheck.exchange(false);
    if (!changed) return;
    if (CurrentDevice() != PrimeGamepad::Device::Gamepad) {
        aurora_set_texture_replacement_variant("");
        return;
    }
    const auto style = CurrentStyle();
    const size_t index = static_cast<size_t>(style);
    if (index >= g_padIconsReady.size()) return;
    if (g_padIconsReady[index].load()) {
        aurora_set_texture_replacement_variant(PrimeKeyIcons::PadVariant(style));
        return;
    }
    if (g_padIconsBusy.exchange(true)) {
        g_padIconsRecheck.store(true);  // after the draw in progress
        return;
    }
    // GDI+ draws ~50 small PNGs: off the game thread.
    std::thread([style, index] {
        const uint32_t generation = g_padIconsGeneration.load();
        PrimeKeyIcons::GeneratePadIcons(RuntimeConfigFile::ApplicationDataDirectory(), style);
        // A binding changed while drawing: these files are already stale.
        if (generation == g_padIconsGeneration.load()) g_padIconsReady[index].store(true);
        g_padIconsBusy.store(false);
        g_padIconsRecheck.store(true);
    }).detach();
}

std::string PadName() {
    const SDL_JoystickID id = g_activePad.load();
    const char* name = id ? SDL_GetGamepadNameForID(id) : nullptr;
    return name ? name : "";
}

Settings CurrentSettings() {
    std::lock_guard lock(g_padMutex);
    PadLoadSettingsLocked();
    return g_padSettings;
}
void ApplySettings(const Settings& settings) {
    std::lock_guard lock(g_padMutex);
    PadLoadSettingsLocked();
    g_padSettings = settings;
}

}  // namespace PrimeGamepadRuntime

namespace PrimePadBindings {
size_t ActionCount() { return PadKeyActionCount() + 1; }
const char* ActionId(size_t action) {
    return action < PadKeyActionCount() ? PrimeKeyBindings::Action(action).id : PrimeGamepad::kMotionModeAction;
}
const char* ActionLabel(size_t action) {
    return action < PadKeyActionCount() ? PrimeKeyBindings::Action(action).label
                                     : "Motion mode (hold): right stick twists / pushes the remote, and the third column's combinations";
}
PadInput Binding(size_t action, size_t slot) {
    std::lock_guard lock(g_padMutex);
    PadLoadBindingsLocked();
    if (slot == kLayerSlot) return action < g_padLayerBindings.size() ? g_padLayerBindings[action] : PadInput::None;
    return action < g_padBindings.size() && slot < kSlots ? g_padBindings[action][slot] : PadInput::None;
}
std::string BindingName(PadInput input) {
    if (input == PadInput::None) return "-";
    PrimeGamepad::Style style;
    {
        std::lock_guard lock(g_padMutex);
        style = g_padStyle;
    }
    const auto glyph = PrimeGamepad::GlyphFor(style, input);
    switch (glyph.shape) {
    case PrimeGamepad::Shape::Cross: return "Cross";
    case PrimeGamepad::Shape::Circle: return "Circle";
    case PrimeGamepad::Shape::Square: return "Square";
    case PrimeGamepad::Shape::Triangle: return "Triangle";
    case PrimeGamepad::Shape::Dpad:
        return glyph.dirY > 0 ? "D-pad up" : glyph.dirY < 0 ? "D-pad down" : glyph.dirX < 0 ? "D-pad left" : "D-pad right";
    case PrimeGamepad::Shape::Stick:
        if (glyph.dirX == 0 && glyph.dirY == 0) return std::string(glyph.text) + " (click)";
        return std::string(glyph.text) + (glyph.dirY > 0 ? " stick up" : glyph.dirY < 0 ? " stick down"
                                          : glyph.dirX < 0 ? " stick left" : " stick right");
    default: return glyph.text;
    }
}
void SetBinding(size_t action, size_t slot, PadInput input) {
    std::lock_guard lock(g_padMutex);
    PadLoadBindingsLocked();
    if (action < g_padBindings.size() && slot <= kLayerSlot) PadAssignLocked(action, slot, input);
}
void ResetDefaults() {
    std::lock_guard lock(g_padMutex);
    PadLoadBindingsLocked();
    g_padCaptureAction = -1;
    for (size_t i = 0; i < g_padBindings.size(); ++i) {
        g_padBindings[i] = PadDefaultsFor(ActionId(i));
        PadSaveBindingLocked(i);
        g_padLayerBindings[i] = i == PadMotionAction() ? PadInput::None : PadLayerDefaultFor(ActionId(i));
        PadSaveLayerLocked(i);
    }
    PadIconsInvalidate();
}
void BeginCapture(size_t action, size_t slot) {
    std::lock_guard lock(g_padMutex);
    PadLoadBindingsLocked();
    g_padCaptureAction = static_cast<int>(action);
    g_padCaptureSlot = static_cast<int>(slot);
}
void CancelCapture() {
    std::lock_guard lock(g_padMutex);
    g_padCaptureAction = -1;
}
bool Capturing(size_t* action, size_t* slot) {
    std::lock_guard lock(g_padMutex);
    if (g_padCaptureAction < 0) return false;
    if (action) *action = static_cast<size_t>(g_padCaptureAction);
    if (slot) *slot = static_cast<size_t>(g_padCaptureSlot);
    return true;
}
}  // namespace PrimePadBindings
#endif
