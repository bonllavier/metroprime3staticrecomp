#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Asks the player for their Metroid Prime 3 disc image with the Windows file
// dialog. Used at startup when no image is configured (or the configured one
// cannot be opened) and from F1 > Diagnostics to change it. Returns an empty
// path when the player cancels.
namespace PrimeDiscPicker {

// `reason` is shown first in a message box (why the image is needed); pass an
// empty view to go straight to the file dialog.
std::filesystem::path Prompt(std::string_view reason);

// Opens `path` read-only and checks it is a disc this executable runs. Returns
// an empty string when it is, else the reason in the startup check's words.
// Used by F1 > Diagnostics before saving a new path.
std::string CheckImage(const std::filesystem::path& path);

} // namespace PrimeDiscPicker
