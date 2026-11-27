#pragma once
// Performance capture for a tester's machine (Linux, MKW_PRIME_PERF_CAPTURE builds only).
// F8 records 60 s: CPU stack samples of the game, GX, decoder and render threads
// (StackProfiler, gzipped to UserData/Logs/profiles), GPU time per frame and per post-process
// pass ("[gpu-time]"/"[gpu-pass]" each second) and, each second, the GPU load and clocks, CPU
// clocks and power profile ("[perf-sys]"). When it ends an F12 report is saved with all of it.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace PerfCapture {
constexpr double kSeconds = 60.0;
// Starts a capture of these OS threads; false if one is running.
bool Start(std::vector<uint32_t> threadIds);
// Once per frame: per-second system samples, and the report when the capture ends.
void Tick();
// On-screen notice while recording and right after.
void DrawNotice();
// Power profile, CPU governor and GPU power state, for system.txt (one "key: value" per line).
std::string PowerSummary();
// Profiles written during this run (newest first), for the report.
std::vector<std::filesystem::path> ProfilesOfThisRun();
}  // namespace PerfCapture
