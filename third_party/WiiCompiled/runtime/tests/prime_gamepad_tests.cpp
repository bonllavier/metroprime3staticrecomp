#include "hle/prime_gamepad.h"
#include "hle/prime_keyboard_mouse.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <string>

#define require(ok) do { if (!(ok)) { std::cerr << "FAIL line " << __LINE__ << ": " << #ok << "\n"; std::exit(1); } } while (false)

int main() {
    using namespace PrimeGamepad;

    // Config names round-trip and are unique; unknown names bind nothing.
    {
        std::set<std::string> names;
        for (size_t i = 1; i < kInputCount; ++i) {
            const auto input = static_cast<Input>(i);
            require(InputFromName(InputName(input)) == input);
            require(names.insert(InputName(input)).second);
        }
        require(InputFromName("") == Input::None && InputFromName("triangle") == Input::None);
        // Button inputs carry SDL_GamepadButton values (South = 0 ... Touchpad = 20).
        require(SdlButton(Input::South) == 0 && SdlButton(Input::DpadDown) == 12 && SdlButton(Input::Touchpad) == 20);
        require(SdlButton(Input::LeftTrigger) == -1 && SdlButton(Input::None) == -1);
    }

    // Analog inputs as buttons, with hysteresis so a trigger held near the
    // threshold does not chatter.
    {
        PadState pad;
        pad.rightTrigger = 0.45f;
        require(!Down(pad, Input::RightTrigger, false));
        pad.rightTrigger = 0.55f;
        require(Down(pad, Input::RightTrigger, false));
        pad.rightTrigger = 0.40f;
        require(Down(pad, Input::RightTrigger, true) && !Down(pad, Input::RightTrigger, false));
        pad.rightTrigger = 0.30f;
        require(!Down(pad, Input::RightTrigger, true));
        pad.leftY = -0.8f;
        require(Down(pad, Input::LeftStickDown, false) && !Down(pad, Input::LeftStickUp, false));
        pad.buttons[SdlButton(Input::North)] = true;
        require(Value(pad, Input::North) == 1.0f && Value(pad, Input::South) == 0.0f);
    }

    // Radial dead zone: rest reads zero, the edge reads one, the direction is
    // kept and the response is continuous past the inner radius.
    {
        const Stick rest = RadialDeadzone(0.1f, -0.08f, kDefaultInnerDeadzone, kDefaultOuterDeadzone);
        require(rest.x == 0 && rest.y == 0);
        const Stick full = RadialDeadzone(0.0f, 1.0f, kDefaultInnerDeadzone, kDefaultOuterDeadzone);
        require(std::fabs(full.y - 1.0f) < 1e-6f && full.x == 0);
        const Stick diagonal = RadialDeadzone(0.7f, 0.7f, kDefaultInnerDeadzone, kDefaultOuterDeadzone);
        require(std::fabs(diagonal.x - diagonal.y) < 1e-6f && diagonal.x > 0);
        const Stick justOut = RadialDeadzone(kDefaultInnerDeadzone + 0.01f, 0.0f, kDefaultInnerDeadzone, kDefaultOuterDeadzone);
        require(justOut.x > 0 && justOut.x < 0.02f);
        float previous = 0;
        for (float m = 0.16f; m <= 1.0f; m += 0.01f) {
            const float v = RadialDeadzone(m, 0, kDefaultInnerDeadzone, kDefaultOuterDeadzone).x;
            require(v >= previous && v <= 1.0f);
            previous = v;
        }
        require(RadialDeadzone(NAN, 0, 0.1f, 0.9f).x == 0);
    }

    // Look: full deflection turns the configured rate, the curve slows small
    // deflections, and a long frame is capped like the keyboard model.
    {
        const float dt = 1.0f / 60.0f;
        const Stick full = LookDegrees({1, 0}, 240.0f, 2.0f, dt);
        require(std::fabs(full.x - 4.0f) < 1e-4f && full.y == 0);
        const Stick half = LookDegrees({0.5f, 0}, 240.0f, 2.0f, dt);
        require(std::fabs(half.x - 1.0f) < 1e-4f);  // 0.5^2 of the rate
        const Stick linear = LookDegrees({0.5f, 0}, 240.0f, 1.0f, dt);
        require(std::fabs(linear.x - 2.0f) < 1e-4f);
        const Stick up = LookDegrees({0, 1}, 240.0f, 2.0f, dt);
        require(up.y > 0 && up.x == 0);
        require(LookDegrees({1, 0}, 240.0f, 2.0f, 1.0f).x == LookDegrees({1, 0}, 240.0f, 2.0f, 0.1f).x);
        require(LookDegrees({0, 0}, 240.0f, 2.0f, dt).x == 0 && LookDegrees({1, 0}, 240.0f, 2.0f, 0).x == 0);
    }

    // Pointer: integrates and stays inside the screen; stick up moves it up.
    {
        float x = 0, y = 0;
        PointerStep(x, y, {1, 1}, 1.6f, 0.1f);
        require(std::fabs(x - 0.16f) < 1e-6f && std::fabs(y + 0.16f) < 1e-6f);
        for (int i = 0; i < 100; ++i) PointerStep(x, y, {1, 1}, 1.6f, 0.1f);
        require(x == 1.0f && y == -1.0f);
        PointerStep(x, y, {0, 0}, 1.6f, 0.1f);
        require(x == 1.0f && y == -1.0f);
    }

    // Styles from SDL_GamepadType, and the positional letters of each family.
    {
        require(StyleForSdlType(2) == Style::Xbox && StyleForSdlType(3) == Style::Xbox);
        require(StyleForSdlType(4) == Style::PlayStation && StyleForSdlType(5) == Style::PlayStation && StyleForSdlType(6) == Style::PlayStation);
        require(StyleForSdlType(7) == Style::Nintendo && StyleForSdlType(10) == Style::Nintendo);
        require(StyleForSdlType(11) == Style::GameCube);
        require(StyleForSdlType(0) == Style::Generic && StyleForSdlType(1) == Style::Generic && StyleForSdlType(99) == Style::Generic);
        require(std::strcmp(GlyphFor(Style::Xbox, Input::South).text, "A") == 0);
        require(std::strcmp(GlyphFor(Style::Nintendo, Input::South).text, "B") == 0);  // positional, not by letter
        require(std::strcmp(GlyphFor(Style::Nintendo, Input::East).text, "A") == 0);
        require(GlyphFor(Style::PlayStation, Input::South).shape == Shape::Cross);
        require(GlyphFor(Style::PlayStation, Input::North).shape == Shape::Triangle);
        require(std::strcmp(GlyphFor(Style::PlayStation, Input::LeftTrigger).text, "L2") == 0);
        require(std::strcmp(GlyphFor(Style::Xbox, Input::RightShoulder).text, "RB") == 0);
        require(GlyphFor(Style::Xbox, Input::DpadDown).shape == Shape::Dpad && GlyphFor(Style::Xbox, Input::DpadDown).dirY == -1);
        // Every input of every style has a glyph with something to draw.
        for (const Style style : {Style::Xbox, Style::PlayStation, Style::Nintendo, Style::GameCube, Style::Generic})
            for (size_t i = 1; i < kInputCount; ++i) {
                const Glyph g = GlyphFor(style, static_cast<Input>(i));
                require(g.text != nullptr);
                require(g.text[0] != 0 || g.shape != Shape::Shoulder);
            }
    }

    // The prompts follow the last device used; drift and small mouse motions never switch.
    {
        ActiveDevice active;
        require(active.Current() == Device::Keyboard && !active.TakeChanged());
        PadState pad;
        pad.connected = true;
        pad.leftX = 0.2f;
        pad.rightTrigger = 0.1f;
        active.NotePad(pad);
        require(active.Current() == Device::Keyboard);
        pad.buttons[SdlButton(Input::South)] = true;
        active.NotePad(pad);
        require(active.Current() == Device::Gamepad && active.TakeChanged() && !active.TakeChanged());
        // A single large jump is a cursor warp (mouse look parking it): ignored.
        active.NoteMouseMotion(400.0f, 1.0);
        require(active.Current() == Device::Gamepad);
        // Small jitter over a long time never adds up.
        for (int i = 0; i < 20; ++i) active.NoteMouseMotion(1.0f, 2.0 + i * 0.3);
        require(active.Current() == Device::Gamepad);
        // A real motion: several events adding up within the window.
        active.NoteMouseMotion(-10.0f, 10.0);
        active.NoteMouseMotion(-10.0f, 10.02);
        require(active.Current() == Device::Gamepad);
        active.NoteMouseMotion(-10.0f, 10.04);
        require(active.Current() == Device::Keyboard && active.TakeChanged());
        pad.buttons.fill(false);
        pad.leftY = -0.9f;
        active.NotePad(pad);
        require(active.Current() == Device::Gamepad);
        active.NotePadRemoved();
        require(active.Current() == Device::Keyboard);
        PadState disconnected;
        disconnected.buttons[0] = true;
        active.NotePad(disconnected);
        require(active.Current() == Device::Keyboard);
    }

    // Defaults: one physical input drives one action, and the right stick and
    // left stick are left free for look and movement.
    {
        std::set<Input> used;
        std::set<std::string> actions;
        for (const auto& binding : kDefaultBindings) {
            require(actions.insert(binding.action).second);
            for (const Input input : binding.slots) {
                if (input == Input::None) continue;
                require(used.insert(input).second);
                require(input < Input::LeftStickUp);
            }
        }
        require(actions.count("a") && actions.count("b") && actions.count("z") && actions.count("minus") &&
                actions.count("plus") && actions.count(kMotionModeAction));
    }

    // Motion layer defaults: one input per action, buttons only, never the
    // motion button itself, and actions of the keyboard table.
    {
        std::set<Input> used;
        std::set<std::string> actions;
        Input shift = Input::None;
        for (const auto& binding : kDefaultBindings)
            if (std::strcmp(binding.action, kMotionModeAction) == 0) shift = binding.slots[0];
        for (const auto& binding : kDefaultLayerBindings) {
            require(actions.insert(binding.action).second);
            require(std::strcmp(binding.action, kMotionModeAction) != 0);
            require(binding.slots[1] == Input::None);
            require(LayerInputAllowed(binding.slots[0]) && binding.slots[0] != shift);
            require(used.insert(binding.slots[0]).second);
        }
        require(actions.count("lasso_pull") && actions.count("remote_up") && actions.count("nunchuk_twist_left"));
        require(!LayerInputAllowed(Input::RightStickUp) && !LayerInputAllowed(Input::None) &&
                LayerInputAllowed(Input::RightTrigger));
    }

    // Motion layer routing: decided at the press, kept until the release.
    {
        LayerRouting routing;
        std::array<bool, kInputCount> down{}, hasLayer{};
        const auto up = static_cast<size_t>(Input::DpadUp), y = static_cast<size_t>(Input::North),
                   a = static_cast<size_t>(Input::South);
        hasLayer[up] = hasLayer[y] = true;
        // Pressed without the shift: normal, and stays normal when the shift comes.
        down[up] = true;
        routing.Update(down, false, hasLayer);
        require(!routing.Layered(Input::DpadUp));
        routing.Update(down, true, hasLayer);
        require(!routing.Layered(Input::DpadUp));
        // Pressed under the shift: layered, and stays layered after the shift goes.
        down[y] = true;
        routing.Update(down, true, hasLayer);
        require(routing.Layered(Input::North));
        routing.Update(down, false, hasLayer);
        require(routing.Layered(Input::North));
        // Released, then pressed again without the shift: normal.
        down[y] = false;
        routing.Update(down, false, hasLayer);
        down[y] = true;
        routing.Update(down, false, hasLayer);
        require(!routing.Layered(Input::North));
        // No layer binding: normal even under the shift.
        down[a] = true;
        routing.Update(down, true, hasLayer);
        require(!routing.Layered(Input::South));
    }

    // Motion mode: stick right twists clockwise (negative, as the keyboard's
    // twist-right key), stick up pushes the remote forward.
    {
        float twist = 0, depth = 0;
        MotionFromStick({1, 0}, twist, depth);
        require(std::fabs(twist + kMotionTwistMax) < 1e-6f && depth == 0);
        MotionFromStick({0, -0.5f}, twist, depth);
        require(twist == 0 && depth == -0.5f);
    }

    // Keyboard model: without a pad (nullptr) and with an idle pad the samples are
    // identical, so the keyboard path does not change; a pad's stick and motion
    // mode then act as designed.
    {
        using PrimeKeyboardMouse::Key;
        PrimeKeyboardMouse::State keys, keysIdlePad;
        keys.Focus(true);
        keysIdlePad.Focus(true);
        const PrimeKeyboardMouse::PadAnalog idle;
        for (int frame = 0; frame < 40; ++frame) {
            const bool down = frame >= 5 && frame < 25;
            for (const Key key : {Key::W, Key::D, Key::TwistLeft, Key::ForwardMotion, Key::Enter, Key::NunchukThrust}) {
                keys.Button(key, down);
                keysIdlePad.Button(key, down);
            }
            const auto a = keys.Read(1.0f / 60.0f);
            const auto b = keysIdlePad.Read(1.0f / 60.0f, &idle);
            require(a.hold == b.hold && a.stickX == b.stickX && a.stickY == b.stickY && a.roll == b.roll);
            require(a.remoteAccelZ == b.remoteAccelZ && a.pointerDistance == b.pointerDistance);
            require(a.remoteGesture == b.remoteGesture && a.nunchukGesture == b.nunchukGesture);
        }
        PrimeKeyboardMouse::State padOnly;
        padOnly.Focus(true);
        PrimeKeyboardMouse::PadAnalog pad;
        pad.stickX = 0.3f;
        pad.stickY = -0.6f;
        auto s = padOnly.Read(1.0f / 60.0f, &pad);
        require(s.stickX == 0.3f && s.stickY == -0.6f);
        // A held key wins over a smaller deflection.
        padOnly.Button(Key::W, true);
        s = padOnly.Read(1.0f / 60.0f, &pad);
        require(s.stickY == 1.0f && s.stickX == 0.0f);
        padOnly.Button(Key::W, false);
        // Motion mode: the twist follows the stick's target at the keyboard's rate.
        pad = {};
        pad.motion = true;
        pad.twistTarget = -0.5f;
        pad.depthRate = 0.5f;
        float roll = 0;
        for (int i = 0; i < 60; ++i) {
            s = padOnly.Read(1.0f / 60.0f, &pad);
            require(s.roll <= roll + 1e-6f);
            roll = s.roll;
        }
        require(std::fabs(roll + 0.5f) < 1e-5f && s.remoteAccelZ == 0.5f && s.pointerDistance < 1.0f);
    }

    std::cout << "PASS Prime gamepad: names, analog buttons, dead zone, look, pointer, glyph styles, device arbitration, defaults, motion layer, motion mode\n";
    return 0;
}
