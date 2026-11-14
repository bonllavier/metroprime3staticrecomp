// RM3E01 "Quit" (pause menu > Quit, title confirmation): CMain's reset path
// (0x802ED6C4) saves its fast-reset block, prepares the drive for reset and
// calls OSRestart(0), which reboots the console into the same disc; when the
// drive reset fails it calls OSReturnToMenu, which goes back to the Wii Menu.
// The runtime has no in-process reset of guest memory, the HLE devices and
// Aurora, so the overlays of those two SDK functions (prepare_prime_runtime_hooks.py)
// end here: OSRestart relaunches this executable with the same command line and
// ends the process the way a window close does; OSReturnToMenu just closes.
// The new process waits for this one to exit before it opens the disc, the
// caches and the log folder (PrimeWaitForRestartedParent, start of RuntimeMain).
//
// Not reproduced: the 128-byte fast-reset block CMain stores at 0x811FFF80
// (options and two flags) does not survive the relaunch; the game reads its
// options from the save as on a cold boot.

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "aurora_events.h"
#include "ppc_runtime.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "memory.h"
#include "system_bridge.h"
#include <cstdio>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {
constexpr const char* kPrimeRestartParentVariable = "WII_COMPILED_PRIME_RESTART_PARENT";

#if defined(_WIN32)
bool PrimeRelaunchSelf() {
    std::wstring exe(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (length == 0) return false;
        if (length < exe.size()) { exe.resize(length); break; }
        exe.resize(exe.size() * 2);
    }
    // The child inherits the environment, which now names this process as the
    // one to wait for.
    std::wstring commandLine = GetCommandLineW();
    // --iso is the only argument. When it failed and the player picked another
    // image in the dialog, the relaunch must not bring the failing one back.
    if (RuntimeConfigFile::PrimeIsoOverride().empty()) commandLine = L"\"" + exe + L"\"";
    const std::string parent = std::to_string(GetCurrentProcessId());
    _putenv_s(kPrimeRestartParentVariable, parent.c_str());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL ok = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                   nullptr, &startup, &process);
    _putenv_s(kPrimeRestartParentVariable, "");
    if (!ok) {
        RT_LOG(RT_TAG_OS) << "[prime-restart] relaunch failed error=" << GetLastError() << std::endl;
        return false;
    }
    RT_LOG(RT_TAG_OS) << "[prime-restart] relaunched pid=" << process.dwProcessId << std::endl;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
#else
std::string SelfExecutable() {
    std::string exe(4096, '\0');
    const ssize_t length = readlink("/proc/self/exe", exe.data(), exe.size() - 1);
    if (length <= 0) return {};
    exe.resize(static_cast<size_t>(length));
    return exe;
}

// Starts `program` with `args` (args[0] included); the child inherits the
// environment, which names this process as the one to wait for.
bool SpawnWithParentMarker(const std::string& program, std::vector<std::string> args, pid_t& child) {
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    const std::string parent = std::to_string(getpid());
    setenv(kPrimeRestartParentVariable, parent.c_str(), 1);
    const int error = posix_spawn(&child, program.c_str(), nullptr, nullptr, argv.data(), environ);
    unsetenv(kPrimeRestartParentVariable);
    if (error != 0) {
        RT_LOG(RT_TAG_OS) << "[prime-restart] spawn failed program=" << program << " errno=" << error << std::endl;
        return false;
    }
    return true;
}

bool PrimeRelaunchSelf() {
    const std::string exe = SelfExecutable();
    if (exe.empty()) return false;
    std::vector<std::string> args{exe};
    // --iso is the only argument. When it failed and the player picked another
    // image in the dialog, the relaunch must not bring the failing one back.
    if (!RuntimeConfigFile::PrimeIsoOverride().empty()) {
        std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
        const std::string raw((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
        args.clear();
        for (size_t start = 0; start < raw.size();) {
            const size_t end = raw.find('\0', start);
            args.emplace_back(raw.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (args.empty()) args.push_back(exe);
    }
    pid_t child = 0;
    if (!SpawnWithParentMarker(exe, std::move(args), child)) return false;
    RT_LOG(RT_TAG_OS) << "[prime-restart] relaunched pid=" << child << std::endl;
    return true;
}
#endif
}  // namespace

extern "C" bool PrimeHostOsRestart(CpuContext* ctx) {
    RT_LOG(RT_TAG_OS) << "[prime-restart] OSRestart reset_code=0x" << std::hex << (ctx ? ctx->gpr[3] : 0u)
                      << std::dec << ": relaunching the game" << std::endl;
    // Whether or not the relaunch started, this process ends: rebooting is the
    // only thing the guest expects after OSRestart.
    PrimeRelaunchSelf();
    ExitForAuroraWindowClose();
}

// The game's own error handler (0x803F6160, called with error, OSContext*, DSISR, DAR). Error 0xFF
// with DAR 0xD1DD0D1E comes from rs_reallyfatal_printf (0x803F6758), which has just pointed
// 0x8067DF6C at its formatted message, e.g. "Alloc failed - Heap: ...". On the console the
// handler blacks the screen and halts; here its message goes to the fatal dialog and the crash
// folder, and the program ends.
extern "C" bool PrimeHostGameError(CpuContext* ctx) {
    const uint32_t error = static_cast<uint32_t>(ctx->gpr[3]);
    const uint32_t context = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t dsisr = static_cast<uint32_t>(ctx->gpr[5]);
    const uint32_t dar = static_cast<uint32_t>(ctx->gpr[6]);
    std::string message;
    if (error == 0xFFu) {
        const uint32_t text = Memory::Read32(0x8067DF6Cu);
        if (text != 0 && Memory::Contains(text, 1u)) {
            for (uint32_t i = 0; i < 1024u && Memory::Contains(text + i, 1u); ++i) {
                const char c = static_cast<char>(Memory::Read8(text + i));
                if (c == '\0') break;
                message.push_back(c);
            }
        }
        if (message.empty()) message = "(no message)";
        message = "The game reported a fatal error:\n\n" + message;
    } else {
        char line[160];
        uint32_t srr0 = 0, srr1 = 0;
        if (context != 0 && Memory::Contains(context + 0x198u, 8u)) {
            srr0 = Memory::Read32(context + 0x198u);
            srr1 = Memory::Read32(context + 0x19Cu);
        }
        std::snprintf(line, sizeof(line), "The game stopped on its own exception %u.\n\nSRR0 = 0x%08X  SRR1 = 0x%08X\nDSISR = 0x%08X  DAR = 0x%08X",
                      error, srr0, srr1, dsisr, dar);
        message = line;
    }
    RT_LOG(RT_TAG_OS) << "[prime-game-error] error=" << error << " lr=0x" << std::hex << ctx->lr << std::dec << ": "
                      << message << std::endl;
    RuntimeCrash::WriteCrashArtifacts("game_error", message);
    ShowRuntimeFatalPopup("Game error", message);
    MarkFatalErrorReported();
    ExitForAuroraWindowClose();
}

extern "C" bool PrimeHostOsReturnToMenu(CpuContext*) {
    RT_LOG(RT_TAG_OS) << "[prime-restart] OSReturnToMenu: closing the game" << std::endl;
    ExitForAuroraWindowClose();
}

// The chosen disc is Metroid Prime Trilogy (R3ME01). Its DOLs use the same guest
// addresses as this one, so they are separate executables next to this one
// (recomp/docs/trilogy_r3me01.md). Starts the Trilogy menu and ends this process;
// the executables share UserData, so they open the same image.
void PrimeStartTrilogy() {
    constexpr wchar_t kTrilogyMenu[] = L"Corruption_3 Trilogy Menu.exe";
#if defined(_WIN32)
    std::wstring exe(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (length == 0) throw std::runtime_error("Cannot locate this executable.");
        if (length < exe.size()) { exe.resize(length); break; }
        exe.resize(exe.size() * 2);
    }
    exe.resize(exe.find_last_of(L"\\/") + 1);
    const std::wstring menu = exe + kTrilogyMenu;
    std::wstring commandLine = L"\"" + menu + L"\"";
    const std::string parent = std::to_string(GetCurrentProcessId());
    _putenv_s(kPrimeRestartParentVariable, parent.c_str());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL ok = CreateProcessW(menu.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process);
    _putenv_s(kPrimeRestartParentVariable, "");
    if (ok) {
        RT_LOG(RT_TAG_OS) << "[prime-trilogy] started the Trilogy menu pid=" << process.dwProcessId << std::endl;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        ExitForAuroraWindowClose();
    }
    throw std::runtime_error("This disc is Metroid Prime Trilogy, but its menu could not be started:\n  Corruption_3 Trilogy Menu.exe\n\n"
                             "It must be next to this executable.");
#else
    (void)kTrilogyMenu;
    std::string exe = SelfExecutable();
    if (!exe.empty()) {
        const std::string menu = exe.substr(0, exe.find_last_of('/') + 1) + "Corruption_3 Trilogy Menu";
        // The image came from --iso (still set: it passed the disc check), so the menu opens it too.
        std::vector<std::string> args{menu};
        if (!RuntimeConfigFile::PrimeIsoOverride().empty()) args.insert(args.end(), {"--iso", RuntimeConfigFile::PrimeIsoOverride()});
        pid_t child = 0;
        if (SpawnWithParentMarker(menu, std::move(args), child)) {
            RT_LOG(RT_TAG_OS) << "[prime-trilogy] started the Trilogy menu pid=" << child << std::endl;
            ExitForAuroraWindowClose();
        }
    }
    throw std::runtime_error("This disc is Metroid Prime Trilogy, but its menu could not be started:\n  Corruption_3 Trilogy Menu\n\n"
                             "It must be next to this executable.");
#endif
}

// Start of RuntimeMain: a process started by PrimeHostOsRestart lets its parent
// finish first (it still holds the window, the disc image and the caches).
void PrimeWaitForRestartedParent() {
#if defined(_WIN32)
    const char* value = std::getenv(kPrimeRestartParentVariable);
    if (value == nullptr || value[0] == 0) return;
    const DWORD parent = static_cast<DWORD>(std::strtoul(value, nullptr, 10));
    _putenv_s(kPrimeRestartParentVariable, "");
    if (parent == 0) return;
    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, parent)) {
        WaitForSingleObject(process, 15000);
        CloseHandle(process);
    }
#else
    const char* value = std::getenv(kPrimeRestartParentVariable);
    if (value == nullptr || value[0] == 0) return;
    const pid_t parent = static_cast<pid_t>(std::strtol(value, nullptr, 10));
    unsetenv(kPrimeRestartParentVariable);
    if (parent <= 0) return;
    constexpr int kTimeoutMs = 15000;
#if defined(SYS_pidfd_open)
    // Linux 5.3+: a pidfd becomes readable when the process exits.
    const int fd = static_cast<int>(syscall(SYS_pidfd_open, parent, 0));
    if (fd >= 0) {
        pollfd waiter{fd, POLLIN, 0};
        poll(&waiter, 1, kTimeoutMs);
        close(fd);
        return;
    }
    if (errno == ESRCH) return;  // already gone
#endif
    // Older kernels: probe until the process is gone.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
    while (kill(parent, 0) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
}
