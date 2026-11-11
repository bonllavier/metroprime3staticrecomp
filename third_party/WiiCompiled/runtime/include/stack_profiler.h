#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// In-process sampling profiler for performance diagnosis. A background thread
// briefly suspends the target thread (~1 kHz), unwinds its native stack with
// the modules' own x64 unwind tables and resumes it. Samples are written as
// text to <output>/stack_profile_<time>.txt and symbolized offline by
// recomp/tools/summarize_stack_profile.py. No guest state is read or edited.
namespace StackProfiler {
// Starts a capture of the given OS threads (first = primary). Returns false
// if a capture is running.
bool Start(std::vector<uint32_t> threadIds, double seconds, const std::filesystem::path& outputDirectory);
bool IsRunning();
// Human readable status of the last/current capture (path or failure).
std::string Status();
}
