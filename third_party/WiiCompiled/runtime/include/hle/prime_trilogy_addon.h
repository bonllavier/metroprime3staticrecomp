#pragma once
// Metroid Prime Trilogy add-on (decision of 16/11/2026, recomp/docs/publicacion_github.md).
//
// The public release is two archives of the same version: the main one (Corruption_3 PC, everything for
// Metroid Prime 3: Corruption USA) and the Trilogy add-on, which holds only the two executables translated
// from the Trilogy disc (its menu and its Metroid Prime 3). The add-on is unzipped into the main folder: the
// executables share its DLLs or libraries, shader seed, UserData, settings and disc path.
//
// Both halves are built from one source tree and share UserData, so they must be the same version.
// Corruption_3 PC starts the Trilogy menu with kBaseVersionVariable set to its own version, and the
// Trilogy executables refuse to run under another version (PrimeTrilogyAddon::CheckBaseVersion).
#include "prime_port_version.h"

#include <cstdlib>
#include <string>
#include <string_view>

namespace PrimeTrilogyAddon {

inline constexpr const char* kBaseVersionVariable = "CORRUPTION3_BASE_VERSION";

#if defined(_WIN32)
inline constexpr const char* kMenuExecutable = "Corruption_3 Trilogy Menu.exe";
inline constexpr const char* kGameExecutable = "Corruption_3 Trilogy MP3.exe";
inline constexpr const char* kPlatformSuffix = "win64.zip";
#else
inline constexpr const char* kMenuExecutable = "Corruption_3 Trilogy Menu";
inline constexpr const char* kGameExecutable = "Corruption_3 Trilogy MP3";
inline constexpr const char* kPlatformSuffix = "linux-x86_64.tar.gz";
#endif

// The add-on archive of `version` (package_release.ps1 / package_release_linux.sh write these names).
inline std::string ArchiveName(std::string_view version = PRIME_PORT_VERSION) {
    return "Corruption_3-PC-v" + std::string(version) + "-Trilogy-addon-" + kPlatformSuffix;
}
inline std::string MainArchiveName(std::string_view version = PRIME_PORT_VERSION) {
    return "Corruption_3-PC-v" + std::string(version) + "-" + kPlatformSuffix;
}
inline std::string ReleasePage(std::string_view version = PRIME_PORT_VERSION) {
    return std::string(PRIME_PORT_REPOSITORY) + "/releases/tag/v" + std::string(version);
}

// Shown by Corruption_3 PC when a Trilogy disc is chosen and the add-on is missing or incomplete.
inline std::string MissingText(std::string_view missing) {
    return "This disc is Metroid Prime Trilogy (USA). Playing its Metroid Prime 3 needs the Trilogy add-on, "
           "which is not in this folder (missing: " + std::string(missing) + ").\n\n"
           "Download " + ArchiveName() + " from\n  " + ReleasePage() + "\n"
           "and unzip it into this same folder, next to " PRIME_PORT_EXECUTABLE ". It must be the same "
           "version as this build (v" PRIME_PORT_VERSION ").\n\n"
           "Or choose a Metroid Prime 3: Corruption (USA) disc image instead.";
}

// Shown by a Trilogy executable started by another version of Corruption_3 PC.
inline std::string VersionMismatchText(std::string_view baseVersion) {
    return "The Metroid Prime Trilogy add-on in this folder is v" PRIME_PORT_VERSION ", but Corruption_3 PC is v" +
           std::string(baseVersion) + ".\n\n"
           "Both must be the same version. Download " + ArchiveName(baseVersion) + " from\n  " +
           ReleasePage(baseVersion) + "\n"
           "and unzip it into the game folder (or update Corruption_3 PC to v" PRIME_PORT_VERSION ").";
}

// Trilogy executables, first thing at start-up: the base version this process was started under, when it
// differs from this build's. Empty when it matches or when the variable is absent (started directly).
inline std::string MismatchedBaseVersion() {
    const char* base = std::getenv(kBaseVersionVariable);
    if (base == nullptr || base[0] == '\0' || std::string_view(base) == PRIME_PORT_VERSION) return {};
    return base;
}

}  // namespace PrimeTrilogyAddon
