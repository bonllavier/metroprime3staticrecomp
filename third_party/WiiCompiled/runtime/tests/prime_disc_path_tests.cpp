// Disc image path checks (GitHub discussions #4 and #5): shell locations from
// Proton/Wine dialogs, pasted paths, the source order and the wrong-disc text.
#include "hle/prime_disc_path.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#define require(ok) do { if (!(ok)) { std::cerr << "FAIL line " << __LINE__ << ": " << #ok << "\n"; std::exit(1); } } while (false)

namespace fs = std::filesystem;
using namespace PrimeDiscPath;

namespace {
bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
}

int main() {
    // The value discussion #4 found saved in Config.toml.
    const std::string protonValue =
        "::{20d04fe0-3aea-1069-a2d8-08002b30309d}\\Metroid Prime 3 - Corruption (Europe) (En,Fr,De,Es,It)";
    require(IsShellLocation(protonValue));
    require(IsShellLocation("C:\\Users\\me\\::{031E4825-7B94-4dc3-B131-E946B44C8DD5}\\Videos"));
    require(IsShellLocation("shell:Libraries\\Games"));
    require(IsShellLocation("SHELL:Desktop"));
    require(!IsShellLocation("Z:/run/media/me/Dumps/RM3E01.rvz"));
    require(!IsShellLocation("D:\\Games\\Metroid Prime 3.iso"));
    require(!IsShellLocation("\\\\server\\share\\mp3.iso"));
    require(!IsShellLocation("C:\\a:b"));
    require(CheckText(protonValue) == Problem::ShellLocation);
    require(CheckText("   ") == Problem::Empty);
    require(CheckText("D:/x.iso") == Problem::None);

    // Pasted text: spaces, line breaks and the quotes of "Copy as path".
    require(Normalize("  \"D:\\Games\\MP3.rvz\"  \r\n") == "D:\\Games\\MP3.rvz");
    require(Normalize("'Z:/home/me/MP3.iso'") == "Z:/home/me/MP3.iso");
    require(Normalize("\" D:/spaced.iso \"") == "D:/spaced.iso");
    require(Normalize("\"") == "\"");
    require(Normalize("D:/no quotes.iso") == "D:/no quotes.iso");
    require(Normalize("\"D:/one side.iso") == "\"D:/one side.iso");

    // File system checks against a scratch folder.
    const fs::path scratch = fs::temp_directory_path() / "mkw_prime_disc_path_tests";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch / "game" / "folder.iso");
    const fs::path saved = scratch / "saved.rvz";
    const fs::path gameA = scratch / "game" / "a.iso";
    { std::ofstream(saved) << "x"; std::ofstream(gameA) << "x"; }
    require(Check(saved, saved.string()) == Problem::None);
    require(Check(scratch / "moved.rvz", (scratch / "moved.rvz").string()) == Problem::Missing);
    require(Check(scratch / "game", (scratch / "game").string()) == Problem::NotAFile);
    require(Check(fs::path(protonValue), protonValue) == Problem::ShellLocation);
    require(!ProblemText(Problem::ShellLocation).empty() && Contains(ProblemText(Problem::ShellLocation), "Z:"));
    require(ProblemText(Problem::None).empty());

    // Source order.
    int listings = 0;
    auto gameFolder = [&] { ++listings; return gameA; };
    auto emptyGameFolder = [&] { ++listings; return fs::path{}; };
    // --iso wins even when it does not exist: its error is what the player must see.
    Choice choice = Choose(scratch / "typo.iso", saved, saved.string(), gameFolder);
    require(choice.source == Source::Override && choice.path == scratch / "typo.iso" && listings == 0);
    // A valid saved path is used and the game folder is never listed.
    choice = Choose({}, saved, saved.string(), gameFolder);
    require(choice.source == Source::Config && choice.path == saved && listings == 0);
    // The Proton shell value gives way to a dump in the game folder...
    choice = Choose({}, fs::path(protonValue), protonValue, gameFolder);
    require(choice.source == Source::GameFolder && choice.path == gameA && listings == 1);
    // ...and so does a moved file.
    choice = Choose({}, scratch / "moved.rvz", (scratch / "moved.rvz").string(), gameFolder);
    require(choice.source == Source::GameFolder && choice.path == gameA);
    // With nothing in the game folder the bad saved value stays, so the error names it.
    choice = Choose({}, fs::path(protonValue), protonValue, emptyGameFolder);
    require(choice.source == Source::Config && choice.path == fs::path(protonValue));
    // Nothing saved: the game folder, else nothing.
    choice = Choose({}, {}, "", gameFolder);
    require(choice.source == Source::GameFolder && choice.path == gameA);
    choice = Choose({}, {}, "  ", emptyGameFolder);
    require(choice.source == Source::None && choice.path.empty());
    require(std::string(SourceName(Source::Override)) == "--iso / CORRUPTION3_ISO");

    // Wrong-disc text names what was found (discussion #5, item 3).
    std::string text = WrongDiscText("RM3P01", 0, 0);
    require(Contains(text, "Found RM3P01") && Contains(text, "Metroid Prime 3: Corruption (Europe)") &&
            Contains(text, "Other regions") && Contains(text, "Spanish"));
    text = WrongDiscText("RM3J01", 0, 0);
    require(Contains(text, "(Japan)"));
    text = WrongDiscText("R3MP01", 0, 0);
    require(Contains(text, "Found R3MP01") && Contains(text, "Metroid Prime Trilogy (Europe)"));
    text = WrongDiscText("RMCE01", 0, 0);
    require(Contains(text, "another game") && !Contains(text, "Other regions"));
    text = WrongDiscText("RM3E01", 0, 2);
    require(Contains(text, "revision 2") && Contains(text, "Only disc 1, revision 0"));
    text = WrongDiscText("RM3E01", 1, 0);
    require(Contains(text, "disc 2"));
    // The Trilogy executables expect R3ME01; the Corruption disc is then "wrong".
    text = WrongDiscText("RM3E01", 0, 0, "R3ME01");
    require(Contains(text, "Found RM3E01 - Metroid Prime 3: Corruption (USA)"));
    text = WrongDiscText(std::string("RM\x01" "E\x7F" "1", 6), 0, 0);
    require(Contains(text, "Found RM?E?1"));
    require(PrintableId("ABC") == "ABC???");

    fs::remove_all(scratch, ec);
    std::cout << "PASS Prime disc path: shell locations, pasted text, file checks, source order, wrong-disc text\n";
    return 0;
}
