#pragma once
// Host CPU topology and thread roles. Windows already spreads threads over
// every logical processor; what the runtime decides is who wins when they
// compete: the game, GX and frame threads run above the background workers
// (pipeline prewarm, disc reads ahead), so spare cores absorb background work
// without delaying a frame. Audio mixing and presentation run highest.
#include <algorithm>
#include <cstdint>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vector>
#elif defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace HostThreads {

struct Topology {
    uint32_t logical = 1;
    uint32_t physical = 1;
};

inline Topology DetectTopology() {
    Topology topology;
    topology.logical = std::max(1u, std::thread::hardware_concurrency());
    topology.physical = topology.logical;
#if defined(_WIN32)
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length != 0) {
        std::vector<uint8_t> buffer(length);
        auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) {
            uint32_t cores = 0;
            for (DWORD offset = 0; offset < length;) {
                auto* entry = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
                if (entry->Relationship == RelationProcessorCore) ++cores;
                offset += entry->Size;
            }
            if (cores != 0) topology.physical = cores;
        }
    }
#endif
    return topology;
}

inline const Topology& CachedTopology() {
    static const Topology topology = DetectTopology();
    return topology;
}

enum class Role { Game, Gx, Background };

// Applies the role's priority and name to the calling thread.
inline void SetCurrentRole(Role role, const wchar_t* name) {
#if defined(_WIN32)
    int priority = THREAD_PRIORITY_NORMAL;
    switch (role) {
    case Role::Game:
    case Role::Gx: priority = THREAD_PRIORITY_ABOVE_NORMAL; break;
    case Role::Background: priority = THREAD_PRIORITY_BELOW_NORMAL; break;
    }
    SetThreadPriority(GetCurrentThread(), priority);
    if (name) SetThreadDescription(GetCurrentThread(), name);
#elif defined(__linux__)
    // Per-thread nice: background work yields (allowed to anyone); raising the game
    // and GX threads needs CAP_SYS_NICE and is skipped without it.
    int nice = 0;
    switch (role) {
    case Role::Game:
    case Role::Gx: nice = -5; break;
    case Role::Background: nice = 5; break;
    }
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice);
    if (name) {
        char shortName[16] = {};  // pthread names: 15 characters
        for (size_t i = 0; i < 15 && name[i]; ++i) shortName[i] = name[i] < 0x80 ? static_cast<char>(name[i]) : '?';
        pthread_setname_np(pthread_self(), shortName);
    }
#else
    (void)role;
    (void)name;
#endif
}

}  // namespace HostThreads
