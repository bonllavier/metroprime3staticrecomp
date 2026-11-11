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
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <utility>

namespace aurora::host_cores {

// Linux has no processor groups; a mask covers logical processors 0..63.
inline uint64_t parse_cpu_list(const std::string& text) noexcept {
  uint64_t mask = 0;
  size_t i = 0;
  while (i < text.size()) {
    char* end = nullptr;
    const unsigned long first = std::strtoul(text.c_str() + i, &end, 10);
    if (end == text.c_str() + i) break;
    unsigned long last = first;
    i = static_cast<size_t>(end - text.c_str());
    if (i < text.size() && text[i] == '-') {
      last = std::strtoul(text.c_str() + i + 1, &end, 10);
      i = static_cast<size_t>(end - text.c_str());
    }
    for (unsigned long cpu = first; cpu <= last; ++cpu) {
      if (cpu >= 64) return 0;  // beyond one mask: leave the scheduler alone
      mask |= uint64_t{1} << cpu;
    }
    if (i < text.size() && text[i] == ',') ++i;
    else break;
  }
  return mask;
}

inline uint64_t game_core_mask() noexcept {
  static const uint64_t mask = []() -> uint64_t {
    if (const char* value = std::getenv("WII_COMPILED_GAME_CORE"); value != nullptr && value[0] == '0') return 0;
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return 0;
    std::set<uint64_t> siblings;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (!CPU_ISSET(cpu, &allowed)) continue;
      if (cpu >= 64) return 0;
      std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list");
      std::string list;
      if (!std::getline(file, list)) return 0;
      const uint64_t core = parse_cpu_list(list);
      if (core == 0) return 0;
      siblings.insert(core);
    }
    // Physical cores in the order of their lowest logical processor, as Windows lists them.
    std::vector<std::pair<int, uint64_t>> ordered;
    for (uint64_t core : siblings) ordered.emplace_back(__builtin_ctzll(core), core);
    std::sort(ordered.begin(), ordered.end());
    std::vector<HostCore> cores;
    for (const auto& entry : ordered) cores.push_back({entry.second, __builtin_popcountll(entry.second) > 1});
    return pick_game_core(cores, 1);
  }();
  return mask;
}

inline void set_thread_affinity(uint64_t mask) noexcept {
  if (mask == 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int cpu = 0; cpu < 64; ++cpu)
    if (mask & (uint64_t{1} << cpu)) CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// For background workers: every logical processor of the process except the game core's.
inline void keep_off_game_core() noexcept {
  const uint64_t game = game_core_mask();
  if (game == 0) return;
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return;
  uint64_t process = 0;
  for (int cpu = 0; cpu < 64; ++cpu)
    if (CPU_ISSET(cpu, &allowed)) process |= uint64_t{1} << cpu;
  set_thread_affinity(process & ~game);
}

// For the game thread: Linux has no "ideal processor" hint, so the thread is
// placed on the game core, which every background worker already avoids.
inline void prefer_game_core() noexcept { set_thread_affinity(game_core_mask()); }

// Per-thread nice value (Linux schedules threads individually). Raising above
// normal needs CAP_SYS_NICE or an RLIMIT_NICE grant and is silently skipped otherwise.
inline void set_thread_nice(int nice) noexcept {
  setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice);
}

// pthread names are limited to 15 characters plus the terminator.
inline void set_thread_name(const char* name) noexcept {
  char shortName[16] = {};
  for (size_t i = 0; i < 15 && name[i]; ++i) shortName[i] = name[i];
  pthread_setname_np(pthread_self(), shortName);
}

inline uint32_t native_thread_id() noexcept { return static_cast<uint32_t>(syscall(SYS_gettid)); }

}  // namespace aurora::host_cores
#endif
