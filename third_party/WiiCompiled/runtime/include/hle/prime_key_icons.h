#pragma once
// Keyboard/mouse icons for RM3E01's button prompts (keyboard/mouse mode).
//
// The game shows Wii Remote and Nunchuk buttons as textures: HUD tutorial
// icons looked up by name (TXTR_IconJumpButton, ...) and "&image=0x<id>;" tags
// inside its text tables, both plain TXTR resources on the disc. Aurora can
// substitute any game texture with a PNG named after the texture's size, the
// XXH64 of its base mip level and its GX format (Dolphin-style replacements),
// so no game code changes: when the option is on, the runtime renders one key
// cap per known button texture from the current bindings into
// <UserData>/texture_replacements/keyboard_icons before Aurora indexes that
// folder. The table of textures (ids, sizes, hashes) was produced offline from
// the disc's paks by recomp/tools/prime_icons/ and is embedded here.
#include <cstddef>
#include <filesystem>
#include <string>

#include "hle/prime_gamepad.h"

namespace PrimeKeyIcons {

struct TextureSpec {
    const char* file;      // replacement file name without extension (tex1_<w>x<h>_<hash>_<fmt>)
    unsigned width, height;
    const char* actionA;   // binding whose primary key is shown (nullptr: fixed text)
    const char* actionB;   // optional second binding ("A / B")
    const char* fixed;     // fixed text when actionA is null; "stick"/"dpad" are layouts
    const char* note;      // what the original texture shows
};
const TextureSpec* Table(size_t* count);

// Label for a binding code as the icons show it (layout-aware for Spanish
// keyboards, short names for modifiers and mouse buttons).
std::string KeyLabel(unsigned bindingCode);

// Writes the icons (overwriting that folder's previous contents) and returns
// how many files were written; 0 when rendering is unavailable.
size_t Generate(const std::filesystem::path& userDataDirectory);

// Gamepad prompts: the same textures drawn with the symbols of the pad input
// bound to each action, for one symbol family, in
// <UserData>/texture_replacements/variants/<PadVariant(style)>. Aurora applies a
// variant only while it is the active one (aurora_set_texture_replacement_variant),
// so the prompts follow the device in use without a restart.
const char* PadVariant(PrimeGamepad::Style style);
size_t GeneratePadIcons(const std::filesystem::path& userDataDirectory, PrimeGamepad::Style style);

}  // namespace PrimeKeyIcons
