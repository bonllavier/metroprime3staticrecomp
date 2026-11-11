#pragma once

#include <filesystem>
#include <string>

// F11 screenshots: the presented game picture with the image effects, without the port's overlay,
// saved as JPEG (quality 92) in <UserData>/Screenshots as Corruption3PC_v<version>_<build>_<date>_<time>.jpg.
namespace Screenshot {

// Short identifier of this executable (8 hex digits from its link time and image size), so
// pictures taken with different builds of the same version can be told apart.
std::string BuildId();

// Where screenshots are written (<UserData>/Screenshots).
std::filesystem::path Directory();

// Requests a screenshot of the next presented frame and converts it to JPEG in the background.
// A request while the previous one is still being written is ignored. Returns false then.
bool Take();

// Last result for the on-screen notice ("Screenshot saved: ...") and how long ago it changed.
std::string Status();
double SecondsSinceStatus();

} // namespace Screenshot
