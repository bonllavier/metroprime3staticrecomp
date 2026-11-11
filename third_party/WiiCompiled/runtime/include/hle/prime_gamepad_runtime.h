#pragma once
// Runtime side of the RM3E01 gamepad layer (model: prime_gamepad.h). Aurora
// opens every SDL3 gamepad and forwards its events; this module picks the pad
// in use, reads it at each KPAD read of the game and maps it through the
// [prime_pad] bindings onto the keyboard mode's Wii actions (kpad.cpp merges
// them with the keys). Design: recomp/docs/mando_gamepad.md.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "hle/prime_gamepad.h"
#include "hle/prime_keyboard_mouse.h"

union SDL_Event;

namespace PrimeGamepadRuntime {

constexpr size_t kMaxActions = 64;

struct Frame {
    bool connected = false;
    // Indexed like PrimeKeyBindings::Action (the keyboard mode's Wii actions).
    std::array<bool, kMaxActions> actionDown{};
    PrimeKeyboardMouse::PadAnalog analog;  // left stick, motion mode
    PrimeGamepad::Stick right;             // dead-zoned right stick (look / pointer) outside motion mode
};

// Host thread (every Aurora SDL event): hot-plug, binding capture, and which
// device the prompts follow (keyboard and mouse events count too).
void HandleEvent(const SDL_Event& event);
// Game thread, once per KPAD read. False when no pad is connected or the
// option is off; `frame` is then all zero.
bool Poll(Frame& frame);

bool Enabled();
void SetEnabled(bool enabled);
// The device whose symbols the prompts show, and the pad's symbol family.
PrimeGamepad::Device CurrentDevice();
PrimeGamepad::Style CurrentStyle();
// True once after the device or the pad's style changed.
bool TakeIconChange();
std::string PadName();  // "" when none

// Prompt icons ([prime] pad_icons): while the pad is the device in use the
// game's button textures show its symbols (prime_key_icons.h). Game thread,
// after every Poll; the icons of a style are drawn in the background the first
// time it is needed and again after a binding changes.
bool IconsEnabled();
void ServiceIcons();

struct Settings {
    float lookDegPerSecond = PrimeGamepad::kDefaultLookDegPerSecond;
    float lookCurve = PrimeGamepad::kDefaultLookExponent;
    float pointerSpeed = PrimeGamepad::kDefaultPointerUnitsPerSecond;
    float deadzone = PrimeGamepad::kDefaultInnerDeadzone;
    bool invertY = false;
};
Settings CurrentSettings();
void ApplySettings(const Settings& settings);  // live; the caller saves them

}  // namespace PrimeGamepadRuntime

// Gamepad bindings (F1 > Controls > Gamepad), the keyboard mode's actions plus
// the pad-only motion mode (last index).
namespace PrimePadBindings {
constexpr size_t kSlots = 2;
// The motion layer's input of an action ([prime_pad_motion]): the slot after the
// two normal ones in Binding / SetBinding / BeginCapture.
constexpr size_t kLayerSlot = kSlots;
size_t ActionCount();
const char* ActionId(size_t action);
const char* ActionLabel(size_t action);
PrimeGamepad::Input Binding(size_t action, size_t slot);
std::string BindingName(PrimeGamepad::Input input);  // in the current pad's style
// Assigns `input` to the slot, clearing it from any other slot of the same
// table (normal or layer), and saves. A layer slot takes only buttons and
// triggers that are not the motion button; anything else is ignored.
void SetBinding(size_t action, size_t slot, PrimeGamepad::Input input);
void ResetDefaults();
// The next pad button, trigger or stick push is assigned to (action, slot).
void BeginCapture(size_t action, size_t slot);
void CancelCapture();
bool Capturing(size_t* action, size_t* slot);
}  // namespace PrimePadBindings
