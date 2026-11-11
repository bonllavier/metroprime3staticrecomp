#pragma once
// The physical core kept for the game thread on CPUs with SMT and few cores (an i7-4790 has 4 cores /
// 8 threads). Background work (pipeline compiles, texture decodes) stays off both of its logical
// processors, so a compile burst never shares the game thread's core through hyperthreading; thread
// priorities cannot prevent that. The game thread only prefers the core (ideal processor), it is not
// pinned. Disabled (mask 0) with WII_COMPILED_GAME_CORE=0, on multi-group systems, without SMT, and
// with fewer than 3 or more than 8 physical cores (pick_game_core, unit tested in render_policy_tests).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aurora::host_cores {

struct HostCore {
  uint64_t mask = 0;  // its logical processors
  bool smt = false;   // two logical processors share it
};

// The last core with SMT, on a single processor group with 3..8 physical cores; 0 otherwise (more
// cores leave room anyway, fewer cannot spare one).
inline uint64_t pick_game_core(const std::vector<HostCore>& cores, unsigned processorGroups) noexcept {
  if (processorGroups != 1 || cores.size() < 3 || cores.size() > 8) return 0;
  for (size_t i = cores.size(); i-- > 0;) {
    if (cores[i].smt) return cores[i].mask;
  }
  return 0;
}

}  // namespace aurora::host_cores

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>

namespace aurora::host_cores {

inline KAFFINITY game_core_mask() noexcept {
  static const KAFFINITY mask = []() -> KAFFINITY {
    if (const char* value = std::getenv("WII_COMPILED_GAME_CORE"); value != nullptr && value[0] == '0') {
      return 0;
    }
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0) return 0;
    std::vector<uint8_t> buffer(length);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                                          &length)) {
      return 0;
    }
    std::vector<HostCore> cores;
    for (DWORD offset = 0; offset < length;) {
      const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
      if (entry->Relationship == RelationProcessorCore) {
        cores.push_back({static_cast<uint64_t>(entry->Processor.GroupMask[0].Mask),
                         entry->Processor.Flags == LTP_PC_SMT});
      }
      offset += entry->Size;
    }
    return static_cast<KAFFINITY>(pick_game_core(cores, GetActiveProcessorGroupCount()));
  }();
  return mask;
}

// For background workers: every logical processor of the process except the game core's.
inline void keep_off_game_core() noexcept {
  const KAFFINITY game = game_core_mask();
  if (game == 0) return;
  DWORD_PTR process = 0;
  DWORD_PTR system = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) return;
  const DWORD_PTR rest = process & ~static_cast<DWORD_PTR>(game);
  if (rest != 0) SetThreadAffinityMask(GetCurrentThread(), rest);
}

// For the game thread: prefer the first logical processor of the game core.
inline void prefer_game_core() noexcept {
  const KAFFINITY game = game_core_mask();
  if (game == 0) return;
  SetThreadIdealProcessor(GetCurrentThread(), static_cast<DWORD>(__builtin_ctzll(static_cast<uint64_t>(game))));
}

}  // namespace aurora::host_cores
#endif
