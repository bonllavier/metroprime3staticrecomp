#include "prime_host_options.h"

#include "hle/gx_gpu_thread.h"
#include "hle/prime_gx_stream.h"
#include "hle/wii_sysconf.h"

#include <aurora/aurora.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include "nand_path.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include "hle/prime_performance.h"

#include <aurora/host_cores.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace PrimeHostOptions {
namespace {
// An explicit environment value (the launchers', or a user's A/B test) wins;
// otherwise the configured value, defaulting to the tuned setting.
void ExportOption(std::optional<bool> RuntimeUserConfig::*field, const char* variable, bool fallback,
                  bool inverted = false) {
    if (const char* existing = std::getenv(variable); existing && *existing) return;
    const bool enabled = (RuntimeConfigFile::Get().*field).value_or(fallback);
#if defined(_WIN32)
    _putenv_s(variable, (enabled != inverted) ? "1" : "0");
#else
    setenv(variable, (enabled != inverted) ? "1" : "0", 0);
#endif
}

uint8_t SoundModeValue(const std::string& mode) {
    if (mode == "mono") return 0;
    if (mode == "surround") return 2;
    return 1;
}

// Wii console language codes (SCGetLanguage). The game turns them into its STRG
// language index (code - 1: ENGL GERM FREN SPAN ITAL) in 0x8038F65C.
uint8_t LanguageCode(const std::string& language) {
    if (language == "german") return 2;
    if (language == "french") return 3;
    if (language == "spanish") return 4;
    if (language == "italian") return 5;
    return 1;
}
std::atomic<bool> g_languageUnlocked{false};

// The SC library reads /shared2/sys/SYSCONF; without it every setting falls
// back to defaults, which put Prime in 4:3. Only these entries are provided,
// so all other console settings keep the defaults the game already used.
void WriteSysconf() {
    const bool widescreen = RuntimeConfigFile::WidescreenEnabled(true);
    const std::string soundMode = RuntimeConfigFile::PrimeSoundMode();
    const std::string language = RuntimeConfigFile::PrimeLanguage();
    const uint8_t languageCode = LanguageCode(language);
    g_languageUnlocked.store(languageCode != 1);
    const std::vector<WiiSysconf::Entry> entries{
        WiiSysconf::Byte("IPL.AR", widescreen ? 1 : 0),
        WiiSysconf::Byte("IPL.SND", SoundModeValue(soundMode)),
        WiiSysconf::Byte("IPL.LNG", languageCode),  // English unless the player picked another one
    };
    const auto bytes = WiiSysconf::Build(entries);
    const auto directory = RuntimeNandPath::DiscoverNandRootPath() / "shared2" / "sys";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto path = directory / "SYSCONF";
    std::ifstream existing(path, std::ios::binary);
    std::vector<uint8_t> current((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
    existing.close();
    if (current != bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            RT_LOG(RT_TAG_CONFIG) << "[prime-sysconf] could not write " << RuntimeConfigFile::PathToUtf8(path)
                                  << std::endl;
            return;
        }
    }
    RT_LOG(RT_TAG_CONFIG) << "[prime-sysconf] aspect=" << (widescreen ? "16:9" : "4:3")
                          << " sound=" << soundMode << " language=" << language << std::endl;
}
}  // namespace

#if defined(_WIN32)
namespace {
// Diagnostics with the performance trace only: two threads off the game core
// wake every 2 ms and log when a wake-up comes 25 ms or more late. F8 profiles
// showed 50-120 ms holes in which neither the sampler nor the GX pipeline ran
// while the game thread (on its reserved core) stayed on time. The time-critical
// watcher is only delayed by interrupts/DPCs or higher-priority system work;
// the normal one also by CPU contention. Their logs place such a stall.
// CPU accounting for a stall: the time-critical watcher (on time even when the
// normal one starves) samples system, process and per-thread CPU every 10 ms
// into a ring; a stall report adds who used the CPUs over its interval.
struct CpuSample {
    std::chrono::steady_clock::time_point at{};
    uint64_t systemIdle = 0, systemTotal = 0, process = 0;
    std::vector<std::pair<DWORD, uint64_t>> threads;  // thread id, kernel+user 100 ns
};
struct CpuRing {
    std::mutex mutex;
    std::array<CpuSample, 64> samples{};
    size_t next = 0, count = 0;
};
CpuRing& Ring() {
    static auto* ring = new CpuRing{};
    return *ring;
}
uint64_t FileTimeValue(const FILETIME& value) {
    return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

void SampleCpu(std::vector<std::pair<DWORD, HANDLE>>& threads, std::chrono::steady_clock::time_point& listedAt) {
    const auto now = std::chrono::steady_clock::now();
    if (threads.empty() || now - listedAt >= std::chrono::seconds(1)) {
        for (auto& [id, handle] : threads) CloseHandle(handle);
        threads.clear();
        listedAt = now;
#if defined(MKW_PRIME_QA_TOOLS)
        // Diagnostic builds only: public builds avoid the thread-enumeration API set that antivirus
        // heuristics flag (package_release.ps1 refuses it), so their stall lines list no threads.
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            const DWORD pid = GetCurrentProcessId();
            for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
                if (entry.th32OwnerProcessID != pid) continue;
                if (HANDLE handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID))
                    threads.emplace_back(entry.th32ThreadID, handle);
            }
            CloseHandle(snapshot);
        }
#endif
    }
    CpuSample sample;
    sample.at = now;
    FILETIME idle{}, kernel{}, user{}, created{}, exited{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        sample.systemIdle = FileTimeValue(idle);
        sample.systemTotal = FileTimeValue(kernel) + FileTimeValue(user);  // kernel includes idle
    }
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        sample.process = FileTimeValue(kernel) + FileTimeValue(user);
    sample.threads.reserve(threads.size());
    for (const auto& [id, handle] : threads)
        if (GetThreadTimes(handle, &created, &exited, &kernel, &user))
            sample.threads.emplace_back(id, FileTimeValue(kernel) + FileTimeValue(user));
    auto& ring = Ring();
    std::lock_guard lock(ring.mutex);
    ring.samples[ring.next] = std::move(sample);
    ring.next = (ring.next + 1) % ring.samples.size();
    ring.count = std::min(ring.count + 1, ring.samples.size());
}

// Who ran during [from, to]: system busy share, this process's share of all
// logical processors, and the process threads that used the most CPU.
std::string DescribeCpu(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
    auto& ring = Ring();
    std::lock_guard lock(ring.mutex);
    const CpuSample* first = nullptr;
    const CpuSample* last = nullptr;
    for (size_t i = 0; i < ring.count; ++i) {
        const auto& sample = ring.samples[(ring.next + ring.samples.size() - ring.count + i) % ring.samples.size()];
        if (sample.at <= from) first = &sample;
        if (sample.at <= to) last = &sample;
    }
    if (first == nullptr || last == nullptr || last == first) return " cpu=unsampled";
    const uint64_t total = last->systemTotal - first->systemTotal;
    const uint64_t idle = last->systemIdle - first->systemIdle;
    const uint64_t process = last->process - first->process;
    std::vector<std::pair<uint64_t, DWORD>> used;
    for (const auto& [id, cpu] : last->threads) {
        for (const auto& [previousId, previous] : first->threads) {
            if (previousId == id && cpu > previous) used.emplace_back(cpu - previous, id);
        }
    }
    std::sort(used.begin(), used.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const double windowMs = std::chrono::duration<double, std::milli>(last->at - first->at).count();
    char line[160];
    std::snprintf(line, sizeof(line), " sampled_ms=%.1f system_busy=%.0f%% process_of_all_cpus=%.0f%% threads:",
                  windowMs, total ? 100.0 * double(total - idle) / double(total) : 0.0,
                  total ? 100.0 * double(process) / double(total) : 0.0);
    std::string text = line;
    for (size_t i = 0; i < used.size() && i < 6; ++i) {
        std::string name = "?";
        if (HANDLE handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, used[i].second)) {
            PWSTR wide = nullptr;
            if (SUCCEEDED(GetThreadDescription(handle, &wide)) && wide != nullptr) {
                name.clear();
                for (const wchar_t* c = wide; *c; ++c) name += (*c < 128 && *c != L' ') ? char(*c) : '_';
                LocalFree(wide);
                if (name.empty()) name = "?";
            }
            CloseHandle(handle);
        }
        std::snprintf(line, sizeof(line), " %s(%lu)=%.1fms", name.c_str(), static_cast<unsigned long>(used[i].second),
                      double(used[i].first) / 1e4);
        text += line;
    }
    return text;
}

#if defined(MKW_PRIME_QA_TOOLS)
// Diagnostic builds only: the names of the player's other programs must never reach a console.log
// that a public F12 report would carry.
// Other processes' CPU around a stall. The stall reports showed system_busy 85-100 % while this
// process used 12-38 % of the CPUs (Rundas' fight, 2026-11-01): something outside the game took
// the cores of the GX threads. The normal watcher snapshots every process' CPU time every 250 ms
// (NtQuerySystemInformation, one call); a report diffs a fresh snapshot against the last one taken
// before the stall and names the processes that used the most CPU, with their base priority.
struct ProcessEntry {
    uint64_t pid = 0;
    uint64_t cpu = 0;  // kernel + user, 100 ns
    long priority = 0;
    std::string name;
};
struct ProcessSample {
    std::chrono::steady_clock::time_point at{};
    std::vector<ProcessEntry> processes;
};
// Layout of SYSTEM_PROCESS_INFORMATION up to UniqueProcessId (stable since Windows Vista, x64).
struct ProcessInfoHead {
    ULONG nextEntryOffset;
    ULONG numberOfThreads;
    LARGE_INTEGER workingSetPrivateSize;
    ULONG hardFaultCount;
    ULONG numberOfThreadsHighWatermark;
    ULONGLONG cycleTime;
    LARGE_INTEGER createTime;
    LARGE_INTEGER userTime;
    LARGE_INTEGER kernelTime;
    USHORT nameLength;
    USHORT nameMaximumLength;
    PWSTR nameBuffer;
    LONG basePriority;
    HANDLE uniqueProcessId;
};

bool SnapshotProcesses(ProcessSample& out) {
    using QueryFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<QueryFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (query == nullptr) return false;
    thread_local std::vector<uint8_t> buffer(1u << 20);
    ULONG needed = 0;
    LONG status = query(5 /* SystemProcessInformation */, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
    if (status == static_cast<LONG>(0xC0000004L) /* STATUS_INFO_LENGTH_MISMATCH */) {
        buffer.resize(static_cast<size_t>(needed) + (1u << 16));
        status = query(5, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
    }
    if (status < 0) return false;
    out.at = std::chrono::steady_clock::now();
    out.processes.clear();
    for (size_t offset = 0; offset + sizeof(ProcessInfoHead) <= buffer.size();) {
        const auto* info = reinterpret_cast<const ProcessInfoHead*>(buffer.data() + offset);
        ProcessEntry entry;
        entry.pid = reinterpret_cast<uint64_t>(info->uniqueProcessId);
        entry.cpu = uint64_t(info->userTime.QuadPart) + uint64_t(info->kernelTime.QuadPart);
        entry.priority = info->basePriority;
        if (info->nameBuffer != nullptr)
            for (USHORT i = 0; i < info->nameLength / 2; ++i)
                entry.name += (info->nameBuffer[i] < 128 && info->nameBuffer[i] != L' ') ? char(info->nameBuffer[i]) : '_';
        if (entry.name.empty()) entry.name = entry.pid == 0 ? "Idle" : "?";
        out.processes.push_back(std::move(entry));
        if (info->nextEntryOffset == 0) break;
        offset += info->nextEntryOffset;
    }
    return true;
}

std::string DescribeOtherProcesses(const ProcessSample& before, const ProcessSample& after) {
    const uint64_t self = GetCurrentProcessId();
    std::vector<std::pair<uint64_t, const ProcessEntry*>> used;
    for (const auto& now : after.processes) {
        if (now.pid == 0 || now.pid == self) continue;  // Idle, this game
        for (const auto& then : before.processes) {
            if (then.pid == now.pid && now.cpu > then.cpu) {
                used.emplace_back(now.cpu - then.cpu, &now);
                break;
            }
        }
    }
    std::sort(used.begin(), used.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const double windowMs = std::chrono::duration<double, std::milli>(after.at - before.at).count();
    char line[160];
    std::snprintf(line, sizeof(line), " others_over_ms=%.0f:", windowMs);
    std::string text = line;
    for (size_t i = 0; i < used.size() && i < 5; ++i) {
        std::snprintf(line, sizeof(line), " %s(%llu,prio%ld)=%.1fms", used[i].second->name.c_str(),
                      static_cast<unsigned long long>(used[i].second->pid), used[i].second->priority,
                      double(used[i].first) / 1e4);
        text += line;
    }
    return text;
}

#endif

void HostStallWatcher(int priority, const char* label) {
    SetThreadDescription(GetCurrentThread(), L"Host stall watcher");
    aurora::host_cores::keep_off_game_core();
    SetThreadPriority(GetCurrentThread(), priority);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) return;
    using Clock = std::chrono::steady_clock;
    constexpr auto kPeriod = std::chrono::milliseconds(2);
    constexpr auto kReport = std::chrono::milliseconds(25);
    uint32_t logged = 0;
    const bool sampler = priority == THREAD_PRIORITY_TIME_CRITICAL;
    std::vector<std::pair<DWORD, HANDLE>> threads;
    Clock::time_point listedAt{};
    uint32_t wakes = 0;
#if defined(MKW_PRIME_QA_TOOLS)
    ProcessSample processesBefore;  // normal watcher: last process snapshot (every 250 ms)
#endif
    for (;;) {
        if (sampler && (wakes++ % 5u) == 0) SampleCpu(threads, listedAt);
#if defined(MKW_PRIME_QA_TOOLS)
        if (!sampler && Clock::now() - processesBefore.at >= std::chrono::milliseconds(250))
            SnapshotProcesses(processesBefore);
#endif
        const auto armed = Clock::now();
        LARGE_INTEGER due{};
        due.QuadPart = -20000;  // 2 ms, relative, 100 ns units
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        const DWORD cpuBefore = GetCurrentProcessorNumber();
        WaitForSingleObject(timer, INFINITE);
        const auto woke = Clock::now();
        const auto late = woke - armed - kPeriod;
        if (late >= kReport && logged < 2000) {
            ++logged;
            // The time-critical sampler keeps sampling meanwhile; let it pass this wake-up.
            if (!sampler) std::this_thread::sleep_for(std::chrono::milliseconds(12));
            std::string others;
#if defined(MKW_PRIME_QA_TOOLS)
            if (!sampler && !processesBefore.processes.empty()) {
                ProcessSample after;
                if (SnapshotProcesses(after)) {
                    others = DescribeOtherProcesses(processesBefore, after);
                    processesBefore = std::move(after);
                }
            }
#endif
            RT_LOG(RT_TAG_RUNTIME) << "[host-stall] watcher=" << label << " late_ms="
                << std::chrono::duration<double, std::milli>(late).count()
                << " cpu_before=" << cpuBefore << " cpu_after=" << GetCurrentProcessorNumber()
                << DescribeCpu(armed, woke) << others << std::endl;
        }
    }
}

void StartHostStallWatchers() {
    if (!PrimePerformance::Enabled()) return;
    std::thread(HostStallWatcher, THREAD_PRIORITY_TIME_CRITICAL, "time_critical").detach();
    std::thread(HostStallWatcher, THREAD_PRIORITY_NORMAL, "normal").detach();
}
}  // namespace
#endif

void ApplyAtStartup() {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
#if defined(_WIN32)
    StartHostStallWatchers();
#endif
    ExportOption(&RuntimeUserConfig::primeGpuThread, "WII_COMPILED_PRIME_GPU_THREAD", true);
    // Aurora's command decoder on its own thread (off until validated): must be
    // decided before any GX work, and its stream callbacks need the dispatcher.
    PrimeGxStream::Install();
    aurora_set_async_decoder(RuntimeConfigFile::PrimeOption(
        &RuntimeUserConfig::primeAuroraDecodeThread, "WII_COMPILED_AURORA_DECODE_THREAD", true));
    ExportOption(&RuntimeUserConfig::primeOverlapEncode, "WII_COMPILED_AURORA_OVERLAP_ENCODE", true);
    ExportOption(&RuntimeUserConfig::primeLazyEndDrain, "WII_COMPILED_AURORA_LAZY_END_DRAIN", true);
    ExportOption(&RuntimeUserConfig::primeDeferDlExpansion, "WII_COMPILED_PRIME_DEFER_DL_EXPANSION", false);
#if defined(MKW_PRIME_QA_TOOLS)
    ExportOption(&RuntimeUserConfig::primeDeferDlVerify, "WII_COMPILED_PRIME_DEFER_VERIFY", false);
    // The vertex cache reads only whether the variable exists: set it only to disable.
    if (!RuntimeConfigFile::Get().primeDlVertexCache.value_or(true) &&
        std::getenv("WII_COMPILED_DISABLE_DL_VERTEX_CACHE") == nullptr) {
#if defined(_WIN32)
        _putenv_s("WII_COMPILED_DISABLE_DL_VERTEX_CACHE", "1");
#else
        setenv("WII_COMPILED_DISABLE_DL_VERTEX_CACHE", "1", 0);
#endif
    }
#endif
    ExportOption(&RuntimeUserConfig::primePresentOnCopy, "WII_COMPILED_PRIME_PRESENT_ON_COPY", true);
    // The host resampler is on unless disabled; its switch selects SDL instead.
    ExportOption(&RuntimeUserConfig::primeHostAudioResampler, "WII_COMPILED_AUDIO_SDL_RESAMPLER", true, true);
    // Read before Aurora decodes anything; switchable live from the F1 panel.
    aurora_set_parallel_texture_decode(RuntimeConfigFile::PrimeOption(
        &RuntimeUserConfig::primeParallelTextureDecode, "WII_COMPILED_AURORA_PARALLEL_TEXTURE_DECODE", true));
    aurora_set_gpu_cmpr_decode(RuntimeConfigFile::PrimeOption(
        &RuntimeUserConfig::primeGpuTextureDecode, "WII_COMPILED_AURORA_GPU_CMPR_DECODE", true));
    WriteSysconf();
#endif
}
void ApplyPresentation(bool widescreen) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    GxGpuThread::Lock gpuLock;
    if (widescreen) {
        VIUnlockAspectRatio();
        AuroraSetViewportPolicy(AURORA_VIEWPORT_16_9);
    } else {
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        VILockAspectRatio(4, 3);
    }
    RT_LOG(RT_TAG_CONFIG) << "[prime-presentation] aspect=" << (widescreen ? "16:9" : "4:3") << std::endl;
#else
    (void)widescreen;
#endif
}
}  // namespace PrimeHostOptions

// Overlay in 0x802F1378 (tools/prepare_prime_runtime_hooks.py). That function only honours
// the console language when the video format is PAL or EuRGB60 (flag r13-0x22C0, set by
// 0x803E80C8) or when the disc has translator.txt; on an NTSC console it answers English.
// The US disc carries every STRG in ENGL/GERM/FREN/SPAN/ITAL, so a non-English choice
// opens that same gate; English keeps the original path unchanged.
extern "C" bool PrimeGameLanguageUnlocked() {
    return PrimeHostOptions::g_languageUnlocked.load(std::memory_order_relaxed);
}
