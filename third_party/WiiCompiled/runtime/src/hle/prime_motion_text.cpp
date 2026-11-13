// Motion prompts in the keyboard/mouse and gamepad modes.
//
// RM3E01 tells the player to move the Wii Remote or the Nunchuk ("Push the Wii Remote toward the
// screen to activate thrusters.", "Twist the Wii Remote.", the Grapple Lasso flicks...). Those are
// plain strings of its STRG tables, which CStringTable::GetString (0x803CDB38) hands to the UTF-8
// to wide conversion (0x803CC6A8) through a borrowed char view built by 0x803FC164. The hook in
// tools/prepare_prime_runtime_hooks.py calls PrimeMotionTextOverride with the string's address
// right before that view is built; a known prompt gets back the address of a replacement naming
// the key, mouse action or pad input bound to the gesture, in the same language. The game then
// converts and owns that copy as usual, so nothing else changes and remapping takes effect the
// next time the prompt is shown.
//
// The prompts are recognized by the FNV-1a 64 of their exact UTF-8 text (generated from the disc by
// tools/prime_icons/motion_text_hashes.py), so no game text is part of this source. Only the
// replacements below are. The text is written to a small guest block reserved below the DVD FST
// (system_bridge.cpp); the view is only read during GetString, so a rotating buffer suffices.
//
// The same hook applies the Spanish text fixes (prime_text_fixes.cpp) when the game runs in Spanish,
// whatever the input mode.
#include "hle/prime_gamepad.h"
#include "hle/prime_gamepad_runtime.h"
#include "hle/prime_key_bindings.h"
#include "hle/prime_key_icons.h"
#include "hle/prime_text_fixes.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

extern "C" uint32_t g_primeTextScratchBase;
extern "C" uint32_t g_primeTextScratchSize;

namespace PrimeMotionTextDetail {

// FNV-1a 64 of each original prompt -> prompt index and language (0 English, also used by the
// Japanese table; 1 German, 2 French, 3 Spanish, 4 Italian).
struct KnownPrompt { uint64_t hash; uint8_t prompt; uint8_t language; };
constexpr KnownPrompt kKnown[] = {
#include "prime_motion_text_hashes.inc"
};

// Replacements per prompt, in the language order above. {action} is the input bound to that
// keyboard-mode action; {point} the pointing device. A leading "&just=center;" and a trailing
// newline of the original are kept around the replacement.
constexpr size_t kLanguages = 5;
using Texts = std::array<const char*, kLanguages>;
constexpr Texts kPrompts[] = {
    // 0 twist clockwise
    {"Press {twist_right} to twist clockwise.", "{twist_right} drücken, um im Uhrzeigersinn zu drehen.",
     "Appuyez sur {twist_right} pour tourner dans le sens des aiguilles d'une montre.",
     "Pulsa {twist_right} para girar en sentido reloj.", "Premi {twist_right} per girare in senso orario."},
    // 1 push toward the screen
    {"Press {push} to push forward.", "{push} drücken, um nach vorn zu stoßen.",
     "Appuyez sur {push} pour pousser vers l'avant.", "Pulsa {push} para empujar hacia delante.",
     "Premi {push} per spingere in avanti."},
    // 2 twist counterclockwise
    {"Press {twist_left} to twist counterclockwise.", "{twist_left} drücken, um gegen den Uhrzeigersinn zu drehen.",
     "Appuyez sur {twist_left} pour tourner dans le sens contraire des aiguilles d'une montre.",
     "Pulsa {twist_left} para girar en sentido antirreloj.", "Premi {twist_left} per girare in senso antiorario."},
    // 3 pull away from the screen
    {"Press {pull} to pull back.", "{pull} drücken, um zurückzuziehen.", "Appuyez sur {pull} pour tirer vers vous.",
     "Pulsa {pull} para tirar hacia ti.", "Premi {pull} per tirare indietro."},
    // 4 thrusters
    {"Press {push} to activate thrusters.", "{push} drücken, um den Schub zu aktivieren.",
     "Appuyez sur {push} pour activer les propulseurs.", "Pulsa {push} para activar la propulsión.",
     "Premi {push} per attivare i propulsori."},
    // 5 Grapple Point (the original's grapple icon tag is kept)
    {"Lock on to Grapple Point &image=0xB37F161D2FDB6D7F; \nand press {lasso_throw} to fire. \nPress {lasso_pull} to grapple.",
     "Verankerungspunkt &image=0xB37F161D2FDB6D7F; erfassen und {lasso_throw} drücken, um auszuwerfen. Zum Zerren {lasso_pull} drücken.",
     "Verrouillez un point d'attache &image=0xB37F161D2FDB6D7F; \net appuyez sur {lasso_throw} pour lancer. \nAppuyez sur {lasso_pull} pour tirer.",
     "Fija el blanco en los Puntos de Enganche &image=0xB37F161D2FDB6D7F; \ny pulsa {lasso_throw} para soltar el lazo. \nPulsa {lasso_pull} para enganchar.",
     "Mira al Punto d'Aggancio &image=0xB37F161D2FDB6D7F; \ne premi {lasso_throw} per attivarlo. \nPremi {lasso_pull} per aggrapparti."},
    // 6 Spring Ball
    {"While in Morph Ball, press {spring_jump} to perform a Spring Ball Jump.",
     "Im Morph Ball-Modus {spring_jump} drücken, um einen Sprung auszulösen.",
     "En mode Boule Morphing, appuyez sur {spring_jump} pour effectuer un Saut Boule.",
     "En el modo Morfosfera, pulsa {spring_jump} para hacer un Rotosalto.",
     "Mentre sei in Morfosfera, premi {spring_jump} per effettuare un Salto Bomba."},
    // 7 lasso pull
    {"Press {lasso_pull}\nto pull with Grapple Lasso.", "{lasso_pull} drücken, um an der Verbindung zu zerren.",
     "Appuyez sur {lasso_pull} pour tirer avec le Grappin Lasso.",
     "Pulsa {lasso_pull} para enganchar y arrancar el objeto.", "Premi {lasso_pull} per usare il Fascio d'Aggancio."},
    // 8 lasso throw
    {"Press {lasso_throw} \nto fire the Grapple Lasso.", "{lasso_throw} drücken, um eine Verbindung herzustellen.",
     "Appuyez sur {lasso_throw} pour activer le Grappin Lasso.",
     "Pulsa {lasso_throw} para lanzar el lazo del Rayo Enganche.", "Premi {lasso_throw} per usare il Fascio d'Aggancio."},
    // 9 shake (the key shakes while held)
    {"Hold {shake} to break free!", "{shake} gedrückt halten, um die Umhüllung aufzubrechen.",
     "Maintenez {shake} pour vous libérer!", "¡Mantén {shake} para liberarte!", "Tieni premuto {shake} per liberarti!"},
    // 10 point at the screen
    {"Aim with {point}.", "Mit {point} zielen.", "Visez avec {point}.", "Apunta con {point}.", "Mira con {point}."},
    // 11 twist (either way)
    {"Press {twist_left} or {twist_right} to twist.", "{twist_left} oder {twist_right} drücken, um zu drehen.",
     "Appuyez sur {twist_left} ou {twist_right} pour tourner.", "Pulsa {twist_left} o {twist_right} para girar.",
     "Premi {twist_left} o {twist_right} per girare."},
    // 12 tilt down
    {"Press {remote_down} to tilt down.", "{remote_down} drücken, um nach unten zu neigen.",
     "Appuyez sur {remote_down} pour incliner vers le bas.", "Pulsa {remote_down} para inclinar hacia abajo.",
     "Premi {remote_down} per inclinare verso il basso."},
    // 13 tilt up
    {"Press {remote_up} to tilt up.", "{remote_up} drücken, um nach oben zu neigen.",
     "Appuyez sur {remote_up} pour incliner vers le haut.", "Pulsa {remote_up} para inclinar hacia arriba.",
     "Premi {remote_up} per inclinare verso l'alto."},
    // 14 power coupling (lock-on icon tag kept)
    {"Grapple the power coupling\nby locking on with &image=SI,1.0,0.68,0xE5A8DFED24B2F64B;\nand pressing {lasso_throw}.",
     "&image=SI,1.0,0.68,0xE5A8DFED24B2F64B; drücken, um den Kupplungspunkt zu erfassen, dann {lasso_throw} drücken.",
     "Utilisez le Grappin et verrouillez\nle générateur avec &image=SI,1.0,0.68,0xE5A8DFED24B2F64B; puis appuyez\nsur {lasso_throw}.",
     "Engánchate a la toma de energía fijando el objetivo con &image=SI,1.0,0.68,0xE5A8DFED24B2F64B;\ny pulsando {lasso_throw}.",
     "Aggancia il Trasmettitore di Energia mirando con &image=SI,1.0,0.68,0xE5A8DFED24B2F64B;\ne premendo {lasso_throw}."},
};
constexpr Texts kPointMouse = {"the mouse", "der Maus", "la souris", "el ratón", "il mouse"};
constexpr Texts kPointStick = {"the right stick", "dem rechten Stick", "le stick droit", "el stick derecho", "la levetta destra"};

uint64_t Fnv1a64(std::string_view text) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (const unsigned char c : text) hash = (hash ^ c) * 0x100000001B3ull;
    return hash;
}

// Longest string handled, terminator included. The longest of the USA discs is ~5.6 KB (a logbook
// entry; Trilogy's menu music list is ~5.1 KB); longer strings are left alone. The scratch block
// (16 KiB) holds at least one copy of that size.
constexpr size_t kMaxGuestText = 8192;
bool ReadGuestText(uint32_t address, std::string& out) {
    out.clear();
    while (out.size() < kMaxGuestText) {
        // 256-byte aligned chunks: a chunk never crosses into a region the string does not reach.
        const uint32_t at = address + static_cast<uint32_t>(out.size());
        const size_t chunk = std::min<size_t>(0x100 - (at & 0xFF), kMaxGuestText - out.size());
        if (!Memory::Contains(at, chunk)) return false;
        const char* bytes = reinterpret_cast<const char*>(Memory::GetPointer(at, chunk));
        if (const void* nul = std::memchr(bytes, 0, chunk)) {
            out.append(bytes, static_cast<const char*>(nul) - bytes);
            return true;
        }
        out.append(bytes, chunk);
    }
    return false;
}

// The game font has no arrow glyphs: arrow keys are spelled out.
std::string Printable(std::string label) {
    if (label == "\xE2\x86\x91") return "Up";
    if (label == "\xE2\x86\x93") return "Down";
    if (label == "\xE2\x86\x90") return "Left";
    if (label == "\xE2\x86\x92") return "Right";
    return label;
}

std::string KeyFor(const char* action) {
    for (size_t i = 0; i < PrimeKeyBindings::ActionCount(); ++i) {
        if (std::strcmp(PrimeKeyBindings::Action(i).id, action) != 0) continue;
        for (size_t slot = 0; slot < PrimeKeyBindings::kSlots; ++slot)
            if (const uint32_t code = PrimeKeyBindings::Binding(i, slot)) return Printable(PrimeKeyIcons::KeyLabel(code));
    }
    return {};
}

// A pad reaches the gestures through the motion button: push/pull/twists with the right stick while
// it is held, other gestures by their own input or by a motion-layer combination (prime_gamepad.h).
std::string PadFor(const char* action) {
    using PrimeGamepad::Input;
    const auto find = [](const char* id) {
        for (size_t i = 0; i < PrimePadBindings::ActionCount(); ++i)
            if (std::strcmp(PrimePadBindings::ActionId(i), id) == 0) return i;
        return SIZE_MAX;
    };
    const auto bound = [&](const char* id) {
        const size_t i = find(id);
        if (i == SIZE_MAX) return Input::None;
        for (size_t slot = 0; slot < PrimePadBindings::kSlots; ++slot)
            if (const Input input = PrimePadBindings::Binding(i, slot); input != Input::None) return input;
        return Input::None;
    };
    const auto layer = [&](const char* id) {
        const size_t i = find(id);
        return i == SIZE_MAX ? Input::None : PrimePadBindings::Binding(i, PrimePadBindings::kLayerSlot);
    };
    const Input motion = bound(PrimeGamepad::kMotionModeAction);
    const auto withMotion = [&](Input input) {
        if (motion == Input::None) return std::string{};
        return PrimePadBindings::BindingName(motion) + " + " + PrimePadBindings::BindingName(input);
    };
    if (std::strcmp(action, "push") == 0) return withMotion(Input::RightStickUp);
    if (std::strcmp(action, "pull") == 0) return withMotion(Input::RightStickDown);
    if (std::strcmp(action, "twist_left") == 0) return withMotion(Input::RightStickLeft);
    if (std::strcmp(action, "twist_right") == 0) return withMotion(Input::RightStickRight);
    if (const Input input = bound(action); input != Input::None) return PrimePadBindings::BindingName(input);
    if (const Input input = layer(action); input != Input::None) return withMotion(input);
    return {};
}

// Empty when a placeholder has no binding: the original prompt is then kept.
bool Expand(const char* text, size_t language, bool pad, std::string& out) {
    out.clear();
    for (const char* p = text; *p;) {
        if (*p != '{') {
            out.push_back(*p++);
            continue;
        }
        const char* end = std::strchr(p, '}');
        if (!end) return false;
        const std::string name(p + 1, end);
        std::string value;
        if (name == "point") value = (pad ? kPointStick : kPointMouse)[language];
        else value = pad ? PadFor(name.c_str()) : KeyFor(name.c_str());
        if (value.empty()) return false;
        out += value;
        p = end + 1;
    }
    return true;
}

// Prompts that look like motion prompts but are not in the table (another disc revision, Trilogy):
// logged once each in test builds so they can be added.
bool LooksLikeMotionPrompt(std::string_view text) {
    for (const char* word : {"Wii Remote", "Nunchuk", "Wii-Fernbedienung", "commande Wii", "mando de Wii", "telecomando Wii"})
        if (text.find(word) != std::string_view::npos) return true;
    return false;
}

std::mutex g_mutex;
uint32_t g_nextOffset = 0;

// Copies `text` into the scratch block and returns its guest address (0 if it does not fit). The
// block is used as a ring: GetString only reads the copy while it builds its own string.
uint32_t WriteScratch(const std::string& text) {
    const uint32_t size = static_cast<uint32_t>((text.size() + 1 + 31) & ~size_t{31});
    std::lock_guard lock(g_mutex);
    if (size > g_primeTextScratchSize) return 0;
    if (g_nextOffset + size > g_primeTextScratchSize) g_nextOffset = 0;
    const uint32_t address = g_primeTextScratchBase + g_nextOffset;
    if (!Memory::Contains(address, text.size() + 1)) return 0;
    uint8_t* out = Memory::GetPointer(address, text.size() + 1);
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = 0;
    g_nextOffset += size;
    return address;
}

}  // namespace PrimeMotionTextDetail

// Called on the game thread by the CStringTable::GetString hook with the address of a UTF-8 string;
// returns the address to build the view from (the same one unless it is a known motion prompt or a
// Spanish string with fixes).
extern "C" uint32_t PrimeMotionTextOverride(uint32_t guestText) {
    using namespace PrimeMotionTextDetail;
    static const bool motionPrompts = RuntimeConfigFile::PrimeKeyboardMouseMode();
    static const bool textFixes = PrimeTextFixes::Enabled();
    if ((!motionPrompts && !textFixes) || guestText == 0 || g_primeTextScratchBase == 0) return guestText;
    std::string original;
    if (!ReadGuestText(guestText, original)) return guestText;
    const uint64_t hash = Fnv1a64(original);

    if (textFixes) {
        std::string fixed = original;
        if (PrimeTextFixes::Apply(hash, fixed) && fixed.size() + 1 <= kMaxGuestText) {
            if (const uint32_t address = WriteScratch(fixed)) {
#if defined(MKW_PRIME_QA_TOOLS)
                RT_LOG(RT_TAG_RUNTIME) << "[prime-text-fix] fnv=" << std::hex << hash << std::dec
                                       << " text=" << fixed.substr(0, 160) << std::endl;
#endif
                return address;
            }
        }
    }
    if (!motionPrompts) return guestText;

    const KnownPrompt* known = nullptr;
    for (const auto& entry : kKnown)
        if (entry.hash == hash) { known = &entry; break; }
    if (!known) {
#if defined(MKW_PRIME_QA_TOOLS)
        if (LooksLikeMotionPrompt(original)) {
            static std::unordered_set<uint64_t> logged;
            std::lock_guard lock(g_mutex);
            if (logged.insert(hash).second)
                RT_LOG(RT_TAG_RUNTIME) << "[prime-motion-text] unknown prompt fnv=" << std::hex << hash << std::dec
                                       << " text=" << original << std::endl;
        }
#endif
        return guestText;
    }
    const bool pad = PrimeGamepadRuntime::CurrentDevice() == PrimeGamepad::Device::Gamepad;
    std::string body;
    if (!Expand(kPrompts[known->prompt][known->language], known->language, pad, body)) return guestText;
    constexpr std::string_view kCenter = "&just=center;";
    std::string text;
    if (original.rfind(kCenter, 0) == 0 && body.rfind(kCenter, 0) != 0) text = kCenter;
    text += body;
    if (!original.empty() && original.back() == '\n') text.push_back('\n');
    if (text.size() + 1 > kMaxGuestText) return guestText;
    const uint32_t address = WriteScratch(text);
    if (address == 0) return guestText;
#if defined(MKW_PRIME_QA_TOOLS)
    RT_LOG(RT_TAG_RUNTIME) << "[prime-motion-text] prompt=" << unsigned(known->prompt) << " language="
                           << unsigned(known->language) << " device=" << (pad ? "pad" : "keyboard") << " text=" << text
                           << std::endl;
#endif
    return address;
}
