#include "stack_profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <aurora/gfx.h>

uint64_t PrimeDvdBytesReadTotal();  // hle/storage/dvd.cpp

// The sampler suspends threads and reads their context and stack: diagnostic-build only
// (MKW_PRIME_QA_TOOLS). Public builds leave it out, since that API set is what antivirus
// machine-learning models associate with malware (VirusTotal Trapmine flag, 28/10/2026).
#if defined(_WIN32) && defined(MKW_PRIME_QA_TOOLS)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <psapi.h>
#include <dbghelp.h>
#endif

namespace StackProfiler {
namespace {
std::atomic_bool g_running{false};
std::mutex g_statusMutex;
std::string g_status = "sin captura";

void SetStatus(std::string text) {
    std::lock_guard lock(g_statusMutex);
    g_status = std::move(text);
}

#if defined(_WIN32) && defined(MKW_PRIME_QA_TOOLS)
struct Module {
    uint64_t base = 0, size = 0;
    const RUNTIME_FUNCTION* functions = nullptr;
    uint32_t functionCount = 0;
    std::string path;
};

// Read the exception directory straight from the mapped image so unwinding
// never takes the loader's function-table lock, which the suspended thread
// may hold.
std::vector<Module> EnumerateModules() {
    std::vector<Module> modules;
    std::vector<HMODULE> handles(1024);
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), handles.data(),
                               DWORD(handles.size() * sizeof(HMODULE)), &needed)) return modules;
    handles.resize(std::min<size_t>(handles.size(), needed / sizeof(HMODULE)));
    for (HMODULE handle : handles) {
        MODULEINFO info{};
        if (!K32GetModuleInformation(GetCurrentProcess(), handle, &info, sizeof(info))) continue;
        Module module;
        module.base = reinterpret_cast<uint64_t>(info.lpBaseOfDll);
        module.size = info.SizeOfImage;
        char path[MAX_PATH]{};
        K32GetModuleFileNameExA(GetCurrentProcess(), handle, path, MAX_PATH);
        module.path = path;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE &&
                nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
                const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                if (dir.VirtualAddress && dir.Size) {
                    module.functions = reinterpret_cast<const RUNTIME_FUNCTION*>(module.base + dir.VirtualAddress);
                    module.functionCount = dir.Size / sizeof(RUNTIME_FUNCTION);
                }
            }
        }
        modules.push_back(std::move(module));
    }
    std::sort(modules.begin(), modules.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
    return modules;
}

const Module* FindModule(const std::vector<Module>& modules, uint64_t address) {
    auto it = std::upper_bound(modules.begin(), modules.end(), address,
                               [](uint64_t value, const Module& m) { return value < m.base; });
    if (it == modules.begin()) return nullptr;
    --it;
    return address - it->base < it->size ? &*it : nullptr;
}

RUNTIME_FUNCTION* FindFunction(const Module& module, uint64_t address) {
    const uint32_t rva = uint32_t(address - module.base);
    uint32_t lo = 0, hi = module.functionCount;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const auto& entry = module.functions[mid];
        if (rva < entry.BeginAddress) hi = mid;
        else if (rva >= entry.EndAddress) lo = mid + 1;
        else {
            auto* found = const_cast<RUNTIME_FUNCTION*>(&entry);
            // Indirect entry: UnwindData points at the primary RUNTIME_FUNCTION.
            if (found->UnwindData & 1u)
                found = reinterpret_cast<RUNTIME_FUNCTION*>(module.base + (found->UnwindData & ~1u));
            return found;
        }
    }
    return nullptr;
}

constexpr unsigned kMaxFrames = 96;

// dbghelp's x64 walker reads the suspended thread's stack only through the
// ReadMemory callback (ReadProcessMemory), so a bogus frame ends the walk
// instead of faulting inside the game process. Function tables come from the
// module list above, never from the loader's lock-protected lookup.
const std::vector<Module>* g_walkModules = nullptr;

BOOL CALLBACK WalkReadMemory(HANDLE process, DWORD64 address, PVOID buffer, DWORD size, LPDWORD read) {
    SIZE_T copied = 0;
    const BOOL ok = ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), buffer, size, &copied);
    if (read) *read = static_cast<DWORD>(copied);
    return ok;
}

PVOID CALLBACK WalkFunctionTable(HANDLE, DWORD64 address) {
    const Module* module = FindModule(*g_walkModules, address);
    return module && module->functions ? FindFunction(*module, address) : nullptr;
}

DWORD64 CALLBACK WalkModuleBase(HANDLE, DWORD64 address) {
    const Module* module = FindModule(*g_walkModules, address);
    return module ? module->base : 0;
}

using StackWalk64Fn = decltype(&StackWalk64);

StackWalk64Fn LoadStackWalk() {
    static StackWalk64Fn walk = [] {
        HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dbghelp ? reinterpret_cast<StackWalk64Fn>(GetProcAddress(dbghelp, "StackWalk64")) : nullptr;
    }();
    return walk;
}

unsigned Unwind(const std::vector<Module>& modules, HANDLE thread, CONTEXT context, uint64_t* frames) {
    const StackWalk64Fn walk = LoadStackWalk();
    frames[0] = context.Rip;
    if (!walk) return 1;
    g_walkModules = &modules;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    unsigned count = 1;
    uint64_t previousStack = context.Rsp;
    while (count < kMaxFrames &&
           walk(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), thread, &frame, &context, WalkReadMemory,
                WalkFunctionTable, WalkModuleBase, nullptr)) {
        if (count == 1 && frame.AddrPC.Offset == frames[0]) {
            // The first call reports the starting frame itself.
            previousStack = frame.AddrStack.Offset;
            if (!walk(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), thread, &frame, &context, WalkReadMemory,
                      WalkFunctionTable, WalkModuleBase, nullptr)) break;
        }
        if (!frame.AddrPC.Offset || frame.AddrStack.Offset < previousStack) break;
        previousStack = frame.AddrStack.Offset;
        frames[count++] = frame.AddrPC.Offset;
        if (!FindModule(modules, frame.AddrPC.Offset)) break;
    }
    return count;
}

void Run(std::vector<uint32_t> threadIds, double seconds, std::filesystem::path outputDirectory) {
    struct Finish { ~Finish() { g_running.store(false, std::memory_order_release); } } finish;
    std::vector<HANDLE> threads;
    for (uint32_t id : threadIds) {
        HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                   FALSE, id);
        if (!handle) {
            for (HANDLE opened : threads) CloseHandle(opened);
            SetStatus("no se pudo abrir el hilo");
            return;
        }
        threads.push_back(handle);
    }
    const uint32_t threadId = threadIds.front();
    HANDLE thread = threads.front();
    const auto modules = EnumerateModules();
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    std::vector<uint64_t> data;
    data.reserve(size_t(seconds * 1100.0) * 25 * threads.size());
    std::vector<uint64_t> frames(kMaxFrames);
    unsigned samples = 0, failures = 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    auto cpu = [&] {
        GetThreadTimes(thread, &created, &exited, &kernel, &user);
        return ((uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime));
    };
    const uint64_t cpuStart = cpu();
    const auto start = std::chrono::steady_clock::now();
    const auto end = start + std::chrono::duration<double>(seconds);
    SetStatus("grabando...");
    // Timeline for hitches: each sweep is stamped ("s <ms>") and a counter line
    // ("c <ms> presents dvd_bytes texture_upload_bytes pipelines_created") is
    // written whenever one of them changes.
    constexpr uint64_t kSweepMarker = ~uint64_t{0};
    std::vector<uint64_t> counters;
    uint64_t lastCounters[4] = {~uint64_t{0}, 0, 0, 0};
    while (std::chrono::steady_clock::now() < end) {
        const uint64_t nowUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
        data.push_back(kSweepMarker);
        data.push_back(nowUs);
        const AuroraStats* stats = aurora_get_stats();
        const uint64_t current[4] = {aurora_get_present_count(), PrimeDvdBytesReadTotal(),
                                     aurora_get_texture_upload_bytes_total(),
                                     stats ? uint64_t(stats->createdPipelines) : 0};
        if (!std::equal(current, current + 4, lastCounters)) {
            std::copy(current, current + 4, lastCounters);
            counters.push_back(nowUs);
            counters.insert(counters.end(), current, current + 4);
        }
        for (size_t t = 0; t < threads.size(); ++t) {
            if (SuspendThread(threads[t]) == DWORD(-1)) { ++failures; continue; }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            unsigned depth = 0;
            if (GetThreadContext(threads[t], &context)) depth = Unwind(modules, threads[t], context, frames.data());
            ResumeThread(threads[t]);
            if (depth) {
                data.push_back(threadIds[t]);
                data.push_back(depth);
                data.insert(data.end(), frames.begin(), frames.begin() + depth);
                ++samples;
            } else {
                ++failures;
            }
        }
        if (timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -10000;  // 1 ms
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 5);
        } else {
            Sleep(1);
        }
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double cpuMs = double(cpu() - cpuStart) / 1e4;
    if (timer) CloseHandle(timer);
    for (HANDLE opened : threads) CloseHandle(opened);

    std::error_code ec;
    std::filesystem::create_directories(outputDirectory, ec);
    char name[64];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::strftime(name, sizeof(name), "stack_profile_%Y%m%d_%H%M%S.txt", &local);
    const auto path = outputDirectory / name;
    std::ofstream out(path);
    if (!out) {
        SetStatus("no se pudo escribir " + path.string());
        return;
    }
    out << "# stack_profile v3\n";
    out << "tid " << threadId << " seconds " << elapsed << " thread_cpu_ms " << cpuMs
        << " samples " << samples << " failures " << failures << "\n";
    // Order given by the caller: game, GX consumer, frame worker, presenter.
    out << "threads";
    for (const uint32_t id : threadIds) out << " " << id;
    out << "\n";
    for (const auto& m : modules) {
        out << "module 0x" << std::hex << m.base << " 0x" << m.size << std::dec << " " << m.path << "\n";
        std::error_code sizeError;
        const auto bytes = std::filesystem::file_size(m.path, sizeError);
        // Lets the summarizer refuse symbols from a rebuilt executable.
        if (!sizeError && m.path.size() > 4 && _stricmp(m.path.c_str() + m.path.size() - 4, ".exe") == 0)
            out << "exe_size " << bytes << " " << m.path << "\n";
    }
    // Milliseconds with three fixed decimals: the default precision drops the decimal point from
    // 100 s on, which the summarizer used to misread.
    const auto ms = [](uint64_t us) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.3f", us / 1000.0);
        return std::string(text);
    };
    for (size_t i = 0; i + 5 <= counters.size(); i += 5)
        out << "c " << ms(counters[i]) << " " << counters[i + 1] << " " << counters[i + 2] << " "
            << counters[i + 3] << " " << counters[i + 4] << "\n";
    out << std::hex;
    for (size_t i = 0; i < data.size();) {
        if (data[i] == kSweepMarker) {
            out << "s " << ms(data[i + 1]) << "\n";
            i += 2;
            continue;
        }
        const uint64_t sampleThread = data[i++];
        const size_t depth = data[i++];
        out << "t " << std::dec << sampleThread << std::hex;
        for (size_t f = 0; f < depth; ++f) out << " " << data[i + f];
        out << "\n";
        i += depth;
    }
    SetStatus(std::to_string(samples) + " muestras -> " + path.string());
}
#endif
}  // namespace

bool Start(std::vector<uint32_t> threadIds, double seconds, const std::filesystem::path& outputDirectory) {
#if defined(_WIN32) && defined(MKW_PRIME_QA_TOOLS)
    bool expected = false;
    if (threadIds.empty() || !g_running.compare_exchange_strong(expected, true)) return false;
    std::thread(Run, std::move(threadIds), seconds, outputDirectory).detach();
    return true;
#else
    (void)threadIds; (void)seconds; (void)outputDirectory;
    SetStatus("only in diagnostic builds");
    return false;
#endif
}

bool IsRunning() { return g_running.load(std::memory_order_acquire); }

std::string Status() {
    std::lock_guard lock(g_statusMutex);
    return g_status;
}
}  // namespace StackProfiler
