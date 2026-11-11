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

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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
#endif
}  // namespace

extern "C" bool PrimeHostOsRestart(CpuContext* ctx) {
    RT_LOG(RT_TAG_OS) << "[prime-restart] OSRestart reset_code=0x" << std::hex << (ctx ? ctx->gpr[3] : 0u)
                      << std::dec << ": relaunching the game" << std::endl;
#if defined(_WIN32)
    // Whether or not the relaunch started, this process ends: rebooting is the
    // only thing the guest expects after OSRestart.
    PrimeRelaunchSelf();
#endif
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
#endif
    throw std::runtime_error("This disc is Metroid Prime Trilogy, but its menu could not be started:\n  Corruption_3 Trilogy Menu.exe\n\n"
                             "It must be next to this executable.");
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
#endif
}
