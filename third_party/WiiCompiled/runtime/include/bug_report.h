#pragma once

#include <string>
#include <string_view>

// In-game bug reports for testers (F12 or F1 > Diagnostics > Create bug report).
//
// A report is a folder plus a .zip under UserData/Reports/ holding everything the
// developer needs to reproduce an issue without asking follow-up questions: a
// screenshot of the frame being shown, the console transcript of this run, any
// crash_*.txt the run already wrote, the player's Config.toml, the key bindings
// and a system summary (Windows build, CPU, RAM, GPU, port version). The work
// runs on its own thread so the game never stalls; when the zip is ready the
// folder opens in Explorer with the zip selected.
namespace BugReport {

// Starts a report. `trigger` is recorded in the summary ("F12", "F1 panel").
// A second request while one is still being written is ignored.
void Create(std::string_view trigger);

// One-line status for the overlay ("Writing report...", "Report saved: <path>").
std::string Status();

// Where reports are written (<UserData>/Reports).
std::string Directory();

} // namespace BugReport
