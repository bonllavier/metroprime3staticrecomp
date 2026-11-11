#pragma once

// Disc image path checks and messages shared by the startup prompt, the F1 panel
// and the command line (GitHub discussions #4 and #5). Pure logic plus plain
// std::filesystem queries, so mkw_prime_disc_path_tests covers it without a disc.
//
// Why it exists: under Proton/Wine the old common file dialog can hand back a
// shell display name such as "::{20d04fe0-3aea-1069-a2d8-08002b30309d}\Metroid
// Prime 3" (This PC + item name) instead of a file system path. Saved as-is, it
// was joined to UserData as a relative path and the prompt came back at every
// start. Such values are now refused before they are saved.

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace PrimeDiscPath {

// Environment variable read once at startup. `--iso <path>` sets it too, so a
// relaunch (OSRestart) and the Trilogy executables inherit the same choice.
inline constexpr const char* kIsoEnvironmentVariable = "CORRUPTION3_ISO";
// Set to 1 for unattended runs: no dialog, the run ends with the message.
inline constexpr const char* kNoDialogEnvironmentVariable = "CORRUPTION3_NO_DISC_DIALOG";

enum class Problem {
    None,
    Empty,          // nothing configured
    ShellLocation,  // "::{CLSID}\..." or "shell:..." - a display name, not a file
    Missing,        // nothing at that path
    NotAFile,       // a folder or a device
};

// Trims spaces, tabs and line breaks, then one pair of surrounding quotes, as
// Explorer's "Copy as path" adds them.
inline std::string Normalize(std::string_view text) {
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    if (end - begin >= 2) {
        const char first = text[begin];
        const char last = text[end - 1];
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            ++begin;
            --end;
            while (begin < end && isSpace(text[begin])) ++begin;
            while (end > begin && isSpace(text[end - 1])) --end;
        }
    }
    return std::string(text.substr(begin, end - begin));
}

// Shell namespace locations have no file behind them: "::{GUID}" items (This PC,
// Libraries, Quick access) and "shell:" monikers.
inline bool IsShellLocation(std::string_view text) {
    if (text.size() >= 2 && text[0] == ':' && text[1] == ':') return true;
    if (text.find("::{") != std::string_view::npos) return true;
    if (text.size() >= 6) {
        std::string prefix(text.substr(0, 6));
        for (char& c : prefix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (prefix == "shell:") return true;
    }
    return false;
}

inline Problem CheckText(std::string_view utf8) {
    if (Normalize(utf8).empty()) return Problem::Empty;
    if (IsShellLocation(Normalize(utf8))) return Problem::ShellLocation;
    return Problem::None;
}

// `path` is the resolved (absolute or config-relative already joined) path.
inline Problem Check(const std::filesystem::path& path, std::string_view originalUtf8) {
    if (const Problem text = CheckText(originalUtf8); text != Problem::None) return text;
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec || status.type() == std::filesystem::file_type::not_found) return Problem::Missing;
    if (status.type() != std::filesystem::file_type::regular) return Problem::NotAFile;
    return Problem::None;
}

inline std::string ProblemText(Problem problem) {
    switch (problem) {
    case Problem::None: return {};
    case Problem::Empty: return "No disc image path is set.";
    case Problem::ShellLocation:
        return "This is a shell location (for example This PC or a library), not a file on a drive. "
               "Open the folder that holds the dump through its drive letter (under Proton or Wine, "
               "Z: is the Linux root) and pick the file there.";
    case Problem::Missing: return "There is no file at this path. It may have been moved, renamed or be on a drive that is not connected.";
    case Problem::NotAFile: return "This path is a folder, not a disc image file.";
    }
    return {};
}

// Where the disc image comes from, in order of precedence.
enum class Source { None, Override, Config, GameFolder };

inline const char* SourceName(Source source) {
    switch (source) {
    case Source::Override: return "--iso / CORRUPTION3_ISO";
    case Source::Config: return "Config.toml [paths] iso_path";
    case Source::GameFolder: return "game folder";
    case Source::None: break;
    }
    return "none";
}

struct Choice {
    std::filesystem::path path;
    Source source = Source::None;
};

// --iso / CORRUPTION3_ISO wins, even when it fails (its error is what the
// player must see). Then [paths] iso_path when it names an existing file. A
// saved value that is a shell location or no longer exists gives way to a dump
// in the game folder; with none there it is still chosen, so the error names it.
// `gameFolderImage` is called only when needed (it lists a directory).
template <typename GameFolderImage>
Choice Choose(const std::filesystem::path& overridePath, const std::filesystem::path& configuredPath,
              std::string_view configuredText, GameFolderImage&& gameFolderImage) {
    if (!overridePath.empty()) return {overridePath, Source::Override};
    if (!configuredPath.empty() || !Normalize(configuredText).empty()) {
        if (Check(configuredPath, configuredText) == Problem::None) return {configuredPath, Source::Config};
        std::filesystem::path detected = gameFolderImage();
        if (!detected.empty()) return {std::move(detected), Source::GameFolder};
        return {configuredPath, Source::Config};
    }
    std::filesystem::path detected = gameFolderImage();
    if (!detected.empty()) return {std::move(detected), Source::GameFolder};
    return {};
}

// Game IDs this executable accepts: Corruption USA, and Trilogy USA, which hands
// over to the Trilogy executables.
inline bool IsCorruptionUsa(std::string_view id) { return id == "RM3E01"; }
inline bool IsTrilogyUsa(std::string_view id) { return id == "R3ME01"; }

inline std::string RegionName(char code) {
    switch (code) {
    case 'E': return "USA";
    case 'P': return "Europe";
    case 'J': return "Japan";
    case 'K': return "Korea";
    case 'D': return "Germany";
    case 'F': return "France";
    case 'S': return "Spain";
    case 'I': return "Italy";
    case 'U': return "Australia";
    case 'X': case 'Y': case 'Z': return "Europe";
    default: return {};
    }
}

// Printable form of a 6-byte ID from the disc header (anything else becomes '?').
inline std::string PrintableId(std::string_view raw) {
    std::string id;
    for (size_t i = 0; i < 6; ++i) {
        const char c = i < raw.size() ? raw[i] : '\0';
        id += (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    return id;
}

// "Metroid Prime 3: Corruption (Europe)" for a recognised ID, else "another game".
inline std::string DescribeGameId(std::string_view id) {
    if (id.size() < 4) return "another game";
    const std::string region = RegionName(id[3]);
    const std::string suffix = region.empty() ? std::string{} : " (" + region + ")";
    if (id.substr(0, 3) == "RM3") return "Metroid Prime 3: Corruption" + suffix;
    if (id.substr(0, 3) == "R3M") return "Metroid Prime Trilogy" + suffix;
    return "another game";
}

// The disc opened but is not the one this executable runs (`expectedId` is
// RM3E01 here and R3ME01 in the Trilogy executables). Names what was found.
inline std::string WrongDiscText(std::string_view rawId, uint8_t discNumber, uint8_t revision,
                                 std::string_view expectedId = "RM3E01") {
    const std::string id = PrintableId(rawId);
    if (id == expectedId) {
        return "Found " + id + " (" + DescribeGameId(id) + ") but disc " + std::to_string(discNumber + 1) + ", revision " +
               std::to_string(revision) + ". Only disc 1, revision 0 is supported.";
    }
    std::string text = "Found " + id + " - " + DescribeGameId(id) +
                       ". This port only runs RM3E01, Metroid Prime 3: Corruption (USA), or R3ME01, Metroid Prime Trilogy (USA).";
    if (!IsCorruptionUsa(id) && !IsTrilogyUsa(id) && (id.substr(0, 3) == "RM3" || id.substr(0, 3) == "R3M")) {
        text += " Other regions run different game code and cannot be loaded. The USA disc includes English, Spanish, "
                "French, German and Italian.";
    }
    return text;
}

} // namespace PrimeDiscPath
