#pragma once
// Gamepad (SDL3) layer of RM3E01's virtual Wii Remote + Nunchuk.
//
// The keyboard/mouse mode already synthesises the remote the game reads
// (prime_keyboard_mouse.h: buttons, Nunchuk stick, IR pointer, tilts and the
// gesture accelerations its own analyser recognises) and turns the camera
// through the CPlayer post-hooks of prime_mouse_look.h. A gamepad feeds that
// same layer: its buttons press the same virtual keys (ORed with the
// keyboard's), the left stick is the Nunchuk stick, the right stick is mouse
// look while the game allows it and the pointer elsewhere (menus, visor
// select, lock-on, scan). Nothing of the keyboard path changes, and no game
// memory or code is patched beyond the hooks the keyboard mode already uses
// (unlike PrimeHack, which writes the player transform and patches the
// lasso; see recomp/docs/mando_gamepad.md).
//
// This header is the pure model (unit tested in prime_gamepad_tests.cpp): no
// SDL types. Button order mirrors SDL3's SDL_GamepadButton and the style
// table SDL_GamepadType (SDL 3.4), so the runtime casts between them.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace PrimeGamepad {

// Physical inputs, positional (SDL3 names South/East/West/North rather than
// the letters, which differ between Xbox, PlayStation and Nintendo pads).
enum class Input : uint8_t {
    None,
    // Buttons, in SDL_GamepadButton order (value - 1).
    South, East, West, North, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight, Misc1, RightPaddle1, LeftPaddle1, RightPaddle2, LeftPaddle2, Touchpad,
    // Analog inputs read as buttons past kPressThreshold.
    LeftTrigger, RightTrigger,
    LeftStickUp, LeftStickDown, LeftStickLeft, LeftStickRight,
    RightStickUp, RightStickDown, RightStickLeft, RightStickRight,
    Count
};
constexpr size_t kButtonCount = static_cast<size_t>(Input::Touchpad);  // South..Touchpad
constexpr size_t kInputCount = static_cast<size_t>(Input::Count);

// Config.toml [prime_pad] names, indexed by Input.
constexpr std::array<const char*, kInputCount> kInputNames{
    "", "south", "east", "west", "north", "back", "guide", "start", "left_stick", "right_stick",
    "left_shoulder", "right_shoulder", "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1",
    "right_paddle1", "left_paddle1", "right_paddle2", "left_paddle2", "touchpad", "left_trigger", "right_trigger",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right"};

inline const char* InputName(Input input) { return kInputNames[static_cast<size_t>(input) < kInputCount ? static_cast<size_t>(input) : 0]; }
inline Input InputFromName(std::string_view name) {
    if (name.empty()) return Input::None;
    for (size_t i = 1; i < kInputCount; ++i)
        if (name == kInputNames[i]) return static_cast<Input>(i);
    return Input::None;
}
// SDL_GamepadButton value of a button input, -1 for analog ones.
constexpr int SdlButton(Input input) {
    return input >= Input::South && input <= Input::Touchpad ? static_cast<int>(input) - 1 : -1;
}

// One controller read. Sticks are -1..1 with y up positive (SDL reports y down;
// the runtime flips it), triggers 0..1.
struct PadState {
    bool connected = false;
    std::array<bool, kButtonCount> buttons{};
    float leftX = 0, leftY = 0, rightX = 0, rightY = 0, leftTrigger = 0, rightTrigger = 0;
};

constexpr float kPressThreshold = 0.5f;   // triggers and stick directions as buttons
constexpr float kReleaseThreshold = 0.35f;  // hysteresis: an analog press holds until it falls below this

// Analog value of any input (buttons 0/1).
inline float Value(const PadState& pad, Input input) {
    if (input >= Input::South && input <= Input::Touchpad) return pad.buttons[static_cast<size_t>(input) - 1] ? 1.0f : 0.0f;
    switch (input) {
    case Input::LeftTrigger: return pad.leftTrigger;
    case Input::RightTrigger: return pad.rightTrigger;
    case Input::LeftStickUp: return std::max(pad.leftY, 0.0f);
    case Input::LeftStickDown: return std::max(-pad.leftY, 0.0f);
    case Input::LeftStickLeft: return std::max(-pad.leftX, 0.0f);
    case Input::LeftStickRight: return std::max(pad.leftX, 0.0f);
    case Input::RightStickUp: return std::max(pad.rightY, 0.0f);
    case Input::RightStickDown: return std::max(-pad.rightY, 0.0f);
    case Input::RightStickLeft: return std::max(-pad.rightX, 0.0f);
    case Input::RightStickRight: return std::max(pad.rightX, 0.0f);
    default: return 0.0f;
    }
}
// Digital view with hysteresis: `wasDown` is this input's previous answer.
inline bool Down(const PadState& pad, Input input, bool wasDown) {
    return Value(pad, input) >= (wasDown ? kReleaseThreshold : kPressThreshold);
}

// --- sticks -------------------------------------------------------------
struct Stick { float x = 0, y = 0; };
// Radial dead zone: magnitudes below `inner` read 0, above `outer` read 1, and
// the range between is rescaled linearly so small motions start from zero
// instead of jumping (direction kept).
inline Stick RadialDeadzone(float x, float y, float inner, float outer) {
    if (!std::isfinite(x) || !std::isfinite(y)) return {};
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= inner || outer <= inner) return {};
    const float scaled = std::min((magnitude - inner) / (outer - inner), 1.0f);
    return {x / magnitude * scaled, y / magnitude * scaled};
}
constexpr float kDefaultInnerDeadzone = 0.15f, kDefaultOuterDeadzone = 0.95f;

// Look: the right stick is a turn rate. Magnitude goes through a power curve
// (1 = linear; 2 by default, fine aim near the centre) and full deflection
// turns `maxDegPerSecond`. Returns the degrees to turn this frame (x right,
// y up), the same unit mouse look accepts once divided by its deg/pixel.
constexpr float kDefaultLookDegPerSecond = 220.0f, kDefaultLookExponent = 2.0f;
inline Stick LookDegrees(Stick stick, float maxDegPerSecond, float exponent, float dt) {
    const float magnitude = std::sqrt(stick.x * stick.x + stick.y * stick.y);
    if (!(magnitude > 0) || !(dt > 0) || !std::isfinite(dt)) return {};
    const float curved = std::pow(std::min(magnitude, 1.0f), std::max(exponent, 1.0f));
    const float scale = curved / magnitude * maxDegPerSecond * std::min(dt, 0.1f);
    return {stick.x * scale, stick.y * scale};
}

// Pointer: where the game shows a cursor, the right stick moves the virtual
// IR pointer at `unitsPerSecond` (the screen spans -1..1) and it stays where it
// was left, like a mouse cursor. y up on the stick moves the pointer up (the
// pointer's y grows downwards).
constexpr float kDefaultPointerUnitsPerSecond = 1.6f;
inline void PointerStep(float& x, float& y, Stick stick, float unitsPerSecond, float dt) {
    if (!(dt > 0) || !std::isfinite(dt)) return;
    const float step = unitsPerSecond * std::min(dt, 0.1f);
    x = std::clamp(x + stick.x * step, -1.0f, 1.0f);
    y = std::clamp(y - stick.y * step, -1.0f, 1.0f);
}

// --- glyphs ---------------------------------------------------------------
// The family of button symbols the prompts show for the active pad.
enum class Style : uint8_t { Xbox, PlayStation, Nintendo, GameCube, Generic };
// From SDL_GamepadType (SDL 3.4: 0 unknown, 1 standard, 2 Xbox 360, 3 Xbox One,
// 4 PS3, 5 PS4, 6 PS5, 7 Switch Pro, 8-10 Joy-Con left/right/pair, 11 GameCube).
inline Style StyleForSdlType(int type) {
    switch (type) {
    case 2: case 3: return Style::Xbox;
    case 4: case 5: case 6: return Style::PlayStation;
    case 7: case 8: case 9: case 10: return Style::Nintendo;
    case 11: return Style::GameCube;
    default: return Style::Generic;  // XInput-like layout, neutral symbols
    }
}

enum class Shape : uint8_t {
    Round,       // face button with a letter
    Cross, Circle, Square, Triangle,  // PlayStation face symbols, drawn as shapes
    Shoulder,    // rounded bar (bumpers, triggers, system buttons)
    Dpad,        // cross with one arm lit (or none for the whole pad)
    Stick,       // stick cap, with an arrow for a direction
};
struct Glyph {
    Shape shape = Shape::Shoulder;
    const char* text = "";   // letters drawn inside ("" for symbol-only shapes)
    uint32_t color = 0xFFE8E8E8u;  // ARGB of the symbol/letter
    int8_t dirX = 0, dirY = 0;     // arrow for d-pad arms and stick directions (y up)
};

// Generic geometric symbols in the familiar colours of each family; no logos.
inline Glyph GlyphFor(Style style, Input input) {
    constexpr uint32_t kGreen = 0xFF5DBB4Au, kRed = 0xFFE0473Cu, kBlue = 0xFF3D8BE0u, kYellow = 0xFFF2C230u,
                       kPink = 0xFFE07AB8u, kTeal = 0xFF3FC5B4u, kWhite = 0xFFE8E8E8u;
    const auto face = [&](const char* xbox, uint32_t xc, Shape ps, uint32_t pc, const char* nin, const char* gc) -> Glyph {
        switch (style) {
        case Style::PlayStation: return {ps, "", pc};
        case Style::Nintendo: return {Shape::Round, nin, kWhite};
        case Style::GameCube: return {Shape::Round, gc, kWhite};
        case Style::Xbox: return {Shape::Round, xbox, xc};
        default: return {Shape::Round, xbox, kWhite};
        }
    };
    const auto pick = [&](const char* xbox, const char* ps, const char* nin, const char* gc) -> Glyph {
        const char* text = style == Style::PlayStation ? ps : style == Style::Nintendo ? nin : style == Style::GameCube ? gc : xbox;
        return {Shape::Shoulder, text, kWhite};
    };
    switch (input) {
    // Nintendo layouts are positional opposites of Xbox: South is B, East is A.
    case Input::South: return face("A", kGreen, Shape::Cross, kBlue, "B", "B");
    case Input::East: return face("B", kRed, Shape::Circle, kRed, "A", "A");
    case Input::West: return face("X", kBlue, Shape::Square, kPink, "Y", "Y");
    case Input::North: return face("Y", kYellow, Shape::Triangle, kTeal, "X", "X");
    case Input::LeftShoulder: return pick("LB", "L1", "L", "L");
    case Input::RightShoulder: return pick("RB", "R1", "R", "Z");
    case Input::LeftTrigger: return pick("LT", "L2", "ZL", "L");
    case Input::RightTrigger: return pick("RT", "R2", "ZR", "R");
    case Input::Back: return pick("View", "Create", "-", "-");
    case Input::Start: return pick("Menu", "Options", "+", "Start");
    case Input::Guide: return pick("Guide", "PS", "Home", "Home");
    case Input::Misc1: return pick("Share", "Mic", "Capture", "-");
    case Input::Touchpad: return pick("Pad", "Touch", "-", "-");
    case Input::LeftStick: return {Shape::Stick, style == Style::PlayStation ? "L3" : "LS", kWhite};
    case Input::RightStick: return {Shape::Stick, style == Style::PlayStation ? "R3" : "RS", kWhite};
    case Input::DpadUp: return {Shape::Dpad, "", kWhite, 0, 1};
    case Input::DpadDown: return {Shape::Dpad, "", kWhite, 0, -1};
    case Input::DpadLeft: return {Shape::Dpad, "", kWhite, -1, 0};
    case Input::DpadRight: return {Shape::Dpad, "", kWhite, 1, 0};
    case Input::LeftStickUp: return {Shape::Stick, "L", kWhite, 0, 1};
    case Input::LeftStickDown: return {Shape::Stick, "L", kWhite, 0, -1};
    case Input::LeftStickLeft: return {Shape::Stick, "L", kWhite, -1, 0};
    case Input::LeftStickRight: return {Shape::Stick, "L", kWhite, 1, 0};
    case Input::RightStickUp: return {Shape::Stick, "R", kWhite, 0, 1};
    case Input::RightStickDown: return {Shape::Stick, "R", kWhite, 0, -1};
    case Input::RightStickLeft: return {Shape::Stick, "R", kWhite, -1, 0};
    case Input::RightStickRight: return {Shape::Stick, "R", kWhite, 1, 0};
    case Input::LeftPaddle1: return pick("P3", "LB", "P3", "P3");
    case Input::LeftPaddle2: return pick("P4", "LB2", "P4", "P4");
    case Input::RightPaddle1: return pick("P1", "RB", "P1", "P1");
    case Input::RightPaddle2: return pick("P2", "RB2", "P2", "P2");
    default: return {};
    }
}

// --- which device the prompts follow -------------------------------------
// Prompts show the device the player used last, as PC games do. A pad takes
// over on a button or a deliberate stick/trigger push (resting drift never
// does); the keyboard takes over on a key, a click or a real mouse motion: at
// least kDeviceSwitchMouseEvents motion events adding up to
// kDeviceSwitchMousePixels within kDeviceSwitchMouseWindow seconds. A single
// jump is the runtime warping the cursor (mouse look parks it), not the player.
enum class Device : uint8_t { Keyboard, Gamepad };
constexpr float kDeviceSwitchAnalog = 0.5f;
constexpr float kDeviceSwitchMousePixels = 24.0f;
constexpr int kDeviceSwitchMouseEvents = 3;
constexpr double kDeviceSwitchMouseWindow = 0.25;
class ActiveDevice {
    Device device_ = Device::Keyboard;
    bool changed_ = false;
    double mouseStart_ = -1e9;
    float mousePixels_ = 0;
    int mouseEvents_ = 0;
    void Set(Device d) { if (d != device_) { device_ = d; changed_ = true; } }
public:
    void NotePad(const PadState& pad) {
        if (!pad.connected) return;
        bool used = std::any_of(pad.buttons.begin(), pad.buttons.end(), [](bool b) { return b; });
        for (const float v : {pad.leftX, pad.leftY, pad.rightX, pad.rightY, pad.leftTrigger, pad.rightTrigger})
            used = used || std::fabs(v) >= kDeviceSwitchAnalog;
        if (used) Set(Device::Gamepad);
    }
    void NoteKeyOrClick() { Set(Device::Keyboard); }
    void NoteMouseMotion(float pixels, double nowSeconds) {
        if (nowSeconds - mouseStart_ > kDeviceSwitchMouseWindow) {
            mouseStart_ = nowSeconds;
            mousePixels_ = 0;
            mouseEvents_ = 0;
        }
        mousePixels_ += std::fabs(pixels);
        if (++mouseEvents_ >= kDeviceSwitchMouseEvents && mousePixels_ >= kDeviceSwitchMousePixels) Set(Device::Keyboard);
    }
    void NotePadRemoved() { Set(Device::Keyboard); }
    Device Current() const { return device_; }
    // True once after each switch (the runtime regenerates/switches the icons).
    bool TakeChanged() { const bool c = changed_; changed_ = false; return c; }
};

// --- default layout --------------------------------------------------------
// Positional defaults for the keyboard mode's Wii actions (kPrimeActions ids in
// kpad.cpp). Dual-stick like PrimeHack: left stick moves (analog, not bound
// here), right stick looks/points, triggers fire and lock on. RM3E01 prompts:
// hold - for visor select, hold + for Hypermode, d-pad down for missiles (hold
// for seekers), C morph ball, Z lock-on.
struct DefaultBinding {
    const char* action;
    Input slots[2];
};
// "pad_motion" is a pad-only action: while held, the right stick twists the
// remote (x) and pushes/pulls it (y) instead of looking, for keypads, handles
// and levers; the left stick still moves.
constexpr const char* kMotionModeAction = "pad_motion";
constexpr std::array<DefaultBinding, 17> kDefaultBindings{{
    {"a", {Input::RightTrigger, Input::None}},
    {"b", {Input::South, Input::None}},
    {"z", {Input::LeftTrigger, Input::None}},
    {"c", {Input::East, Input::None}},
    {"minus", {Input::LeftShoulder, Input::None}},
    {"plus", {Input::Start, Input::None}},
    {"dpad_up", {Input::DpadUp, Input::None}},
    {"dpad_down", {Input::DpadDown, Input::West}},
    {"dpad_left", {Input::DpadLeft, Input::None}},
    {"dpad_right", {Input::DpadRight, Input::None}},
    {"one", {Input::Back, Input::None}},
    {"two", {Input::Misc1, Input::Touchpad}},
    {"lasso_throw", {Input::North, Input::None}},
    {"spring_jump", {Input::LeftStick, Input::None}},
    {"shake", {Input::RightStick, Input::None}},
    {kMotionModeAction, {Input::RightShoulder, Input::None}},
    {"home", {Input::None, Input::None}},  // Guide is taken by Steam / Game Bar
}};

// --- motion layer -----------------------------------------------------------
// A pad has fewer inputs than the Wii actions it must reach (push, pull, twists,
// tilts, both lasso flicks...), and pads like the 8BitDo Ultimate 2C in XInput
// mode report no extra buttons at all. The motion mode button doubles as a
// shift: each action may also have one "layer" input, which drives it while the
// motion button is held (Y throws the lasso, motion + Y pulls it). Inputs
// without a layer binding keep their normal action under the shift, so the
// player still moves, fires and jumps while twisting. Stick directions are not
// layer inputs: under the shift the left stick still moves and the right stick
// twists and pushes.
inline bool LayerInputAllowed(Input input) {
    return input != Input::None && input < Input::LeftStickUp;
}
// Mouse look off makes the right stick the Wii pointer in first person too: the
// reticle follows it and the camera turns at the screen edges, as on the console.
constexpr std::array<DefaultBinding, 9> kDefaultLayerBindings{{
    {"mouse_look_toggle", {Input::Back, Input::None}},
    {"lasso_pull", {Input::North, Input::None}},
    {"remote_up", {Input::DpadUp, Input::None}},
    {"remote_down", {Input::DpadDown, Input::None}},
    {"nunchuk_twist_left", {Input::DpadLeft, Input::None}},
    {"nunchuk_twist_right", {Input::DpadRight, Input::None}},
    {"nunchuk_up", {Input::West, Input::None}},
    {"nunchuk_down", {Input::East, Input::None}},
    {"precision", {Input::LeftShoulder, Input::None}},
}};

// Which table each input follows, decided when it is pressed and kept until it
// is released: pressing the motion button while d-pad down is held for seeker
// missiles does not turn that press into a remote tilt, and releasing the
// motion button first does not end a tilt still held.
class LayerRouting {
    std::array<bool, kInputCount> layered_{}, wasDown_{};
public:
    // down: every input's digital state; shift: the motion button is held;
    // hasLayer: the input has a layer binding.
    void Update(const std::array<bool, kInputCount>& down, bool shift, const std::array<bool, kInputCount>& hasLayer) {
        for (size_t i = 1; i < kInputCount; ++i) {
            if (down[i] && !wasDown_[i]) layered_[i] = shift && hasLayer[i];
            if (!down[i]) layered_[i] = false;
            wasDown_[i] = down[i];
        }
    }
    bool Layered(Input input) const { return layered_[static_cast<size_t>(input)]; }
};

// Motion mode: right stick x is a twist target in radians (full deflection =
// the keyboard mode's 90 degrees), y a push/pull rate in the keyboard mode's
// depth units (-1..1).
constexpr float kMotionTwistMax = 1.5707964f;
inline void MotionFromStick(Stick right, float& twistTarget, float& depthRate) {
    twistTarget = std::clamp(-right.x, -1.0f, 1.0f) * kMotionTwistMax;  // stick right = clockwise twist
    depthRate = std::clamp(right.y, -1.0f, 1.0f);                       // stick up = push forward
}

}  // namespace PrimeGamepad
