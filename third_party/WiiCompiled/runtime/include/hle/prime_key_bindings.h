#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Keyboard/mouse bindings of the Prime keyboard-and-mouse mode. Each Wii
// action accepts up to kSlots host inputs; a binding code is an SDL scancode,
// kMouseBit | SDL mouse button, or 0 when the slot is empty. Bindings persist
// in Config.toml [prime_keys] as readable names.
namespace PrimeKeyBindings {
constexpr size_t kSlots = 2;
constexpr uint32_t kMouseBit = 0x10000u;

struct ActionInfo {
    const char* id;     // Config.toml key
    const char* label;  // F1 label
};

size_t ActionCount();
const ActionInfo& Action(size_t action);
uint32_t Binding(size_t action, size_t slot);
std::string BindingName(uint32_t code);
// Assigns `code` to the slot, clearing it from any other slot, and saves.
void SetBinding(size_t action, size_t slot, uint32_t code);
void ResetDefaults();

// The next key or mouse button pressed in the game window is assigned to
// (action, slot). Overlay hotkeys (F1, F7-F10) are not captured.
void BeginCapture(size_t action, size_t slot);
void CancelCapture();
bool Capturing(size_t* action, size_t* slot);
}  // namespace PrimeKeyBindings
