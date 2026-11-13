// Metroid Prime Trilogy: the menu (rs5fe_p.dol) and the game (rs5mp3_p.dol) hand
// over to each other with OSExec, keeping a block of memory in the save region
// (OSSetSaveRegion, read back with OSGetSavedRegion). Each DOL is its own
// executable here, because both use the same guest addresses. OSExec saves the
// region to a file and starts the other executable, the way OSRestart relaunches
// (prime_host_restart.cpp); the next process restores it before reading OSGetSavedRegion.
// Metroid Prime and Metroid Prime 2 are not translated: choosing them restarts the menu.
//
// Two processes cannot share a window, so the player sees one through a curtain: the next
// executable first copies what the old window shows into an identical window on top. Only
// then does the old process exit (it still owns the caches and the disc image, as in any
// OSRestart). The curtain goes away once the new window has presented its first frames.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "aurora_events.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_config.h"
#include "runtime_log.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <SDL3/SDL_messagebox.h>
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {
constexpr uint32_t kStateMagic = 0x52334D45u;  // R3ME
constexpr uint32_t kMaximumRegion = 0x00800000u;
#if defined(_WIN32)
constexpr const char* kMenuExecutable = "Corruption_3 Trilogy Menu.exe";
constexpr const char* kGameExecutable = "Corruption_3 Trilogy MP3.exe";
constexpr const char* kCurtainVariable = "WII_COMPILED_TRILOGY_CURTAIN";  // "<old window> <old pid>"
constexpr const wchar_t kCurtainClass[] = L"Corruption3TrilogyCurtain";
constexpr unsigned kCurtainFrames = 3;   // the presented frames that take it down; the first ones can still be queued
constexpr DWORD kCurtainWaitMs = 3000;   // the old process waits this long for it, then exits anyway
constexpr UINT kCurtainLifeMs = 30000;   // a new window that never presents must not stay covered
#else
// Linux has no curtain: the old window closes and the new one opens (docs/linux_port.md).
constexpr const char* kMenuExecutable = "Corruption_3 Trilogy Menu";
constexpr const char* kGameExecutable = "Corruption_3 Trilogy MP3";
#endif

std::filesystem::path StatePath() {
    return RuntimeConfigFile::ApplicationDataDirectory() / "trilogy_exec_state.bin";
}

std::string GuestString(uint32_t address) {
    std::string text;
    while (text.size() < 256 && Memory::Contains(address + text.size(), 1)) {
        const char c = static_cast<char>(Memory::Read8(address + static_cast<uint32_t>(text.size())));
        if (c == 0) break;
        text.push_back(c);
    }
    return text;
}

void SaveRegion(uint32_t start, uint32_t end) {
    const uint32_t size = end > start ? end - start : 0u;
    std::error_code ec;
    std::filesystem::remove(StatePath(), ec);
    if (size == 0 || size > kMaximumRegion || !Memory::Contains(start, size)) return;
    std::ofstream file(StatePath(), std::ios::binary);
    const uint32_t header[3] = {kStateMagic, start, size};
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(reinterpret_cast<const char*>(Memory::GetPointer(start, size)), size);
}

#if defined(_WIN32)
std::wstring CurtainEventName(DWORD oldProcess) {
    return L"Local\\Corruption3TrilogyCurtain" + std::to_wstring(oldProcess);
}

HWND OwnWindow() {
    struct Search { DWORD process; HWND window; } search{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& found = *reinterpret_cast<Search*>(parameter);
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process != found.process || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
        found.window = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

// The old window keeps answering while the new process puts its curtain up.
void WaitForCurtain(HANDLE ready) {
    const DWORD start = GetTickCount();
    for (;;) {
        const DWORD elapsed = GetTickCount() - start;
        if (elapsed >= kCurtainWaitMs) return;
        const DWORD result = MsgWaitForMultipleObjects(1, &ready, FALSE, kCurtainWaitMs - elapsed, QS_ALLINPUT);
        if (result != WAIT_OBJECT_0 + 1) return;
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

bool StartExecutable(const std::filesystem::path& executable) {
    const std::wstring path = executable.wstring();
    std::wstring commandLine = L"\"" + path + L"\"";
    const std::string parent = std::to_string(GetCurrentProcessId());
    const HWND window = OwnWindow();
    const std::string curtain = std::to_string(reinterpret_cast<uintptr_t>(window)) + " " + parent;
    HANDLE ready = window ? CreateEventW(nullptr, TRUE, FALSE, CurtainEventName(GetCurrentProcessId()).c_str()) : nullptr;
    _putenv_s("WII_COMPILED_PRIME_RESTART_PARENT", parent.c_str());
    _putenv_s(kCurtainVariable, ready ? curtain.c_str() : "");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL ok = CreateProcessW(path.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process);
    _putenv_s("WII_COMPILED_PRIME_RESTART_PARENT", "");
    _putenv_s(kCurtainVariable, "");
    if (ok) {
        AllowSetForegroundWindow(process.dwProcessId);  // the new window takes the focus this one has
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (ready) WaitForCurtain(ready);
    }
    if (ready) CloseHandle(ready);
    return ok != FALSE;
}

std::atomic<HWND> g_curtain{nullptr};

template <typename Function>
Function SystemFunction(const wchar_t* module, const char* name) {
    const HMODULE handle = LoadLibraryW(module);
    return handle ? reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(handle, name))) : nullptr;
}

LRESULT CALLBACK CurtainProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC target = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBITMAP image = reinterpret_cast<HBITMAP>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (HDC source = image ? CreateCompatibleDC(target) : nullptr) {
            HGDIOBJ previous = SelectObject(source, image);
            BitBlt(target, 0, 0, client.right, client.bottom, source, 0, 0, SRCCOPY);
            SelectObject(source, previous);
            DeleteDC(source);
        } else {
            FillRect(target, &client, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_TIMER:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        g_curtain.store(nullptr);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

// What the old window shows now, from the desktop: a flip-model swap chain has nothing for GDI to read.
HBITMAP CopyClientArea(HWND window) {
    RECT client{};
    POINT origin{0, 0};
    if (!GetClientRect(window, &client) || !ClientToScreen(window, &origin)) return nullptr;
    HDC screen = GetDC(nullptr);
    HDC target = CreateCompatibleDC(screen);
    HBITMAP image = CreateCompatibleBitmap(screen, client.right, client.bottom);
    HGDIOBJ previous = SelectObject(target, image);
    BitBlt(target, 0, 0, client.right, client.bottom, screen, origin.x, origin.y, SRCCOPY);
    SelectObject(target, previous);
    DeleteDC(target);
    ReleaseDC(nullptr, screen);
    return image;
}

HWND CreateCurtain(HWND old) {
    RECT frame{};
    if (GetForegroundWindow() != old || IsIconic(old) || !GetWindowRect(old, &frame)) return nullptr;
    WNDCLASSW type{};
    type.lpfnWndProc = CurtainProcedure;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = kCurtainClass;
    type.hIcon = LoadIconW(type.hInstance, MAKEINTRESOURCEW(1));
    type.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    RegisterClassW(&type);
    wchar_t title[256] = {};
    GetWindowTextW(old, title, 256);
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(old, GWL_STYLE)) & ~static_cast<DWORD>(WS_VISIBLE);
    HWND curtain = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, kCurtainClass, title, style, frame.left, frame.top,
                                   frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, type.hInstance, nullptr);
    if (!curtain) return nullptr;
    SetWindowLongPtrW(curtain, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(CopyClientArea(old)));
    using DwmSetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (auto set = SystemFunction<DwmSetAttribute>(L"dwmapi.dll", "DwmSetWindowAttribute")) {
        const BOOL noTransitions = TRUE;
        set(curtain, 3 /* DWMWA_TRANSITIONS_FORCEDISABLED */, &noTransitions, sizeof(noTransitions));
    }
    ShowWindow(curtain, SW_SHOWNOACTIVATE);
    UpdateWindow(curtain);
    using DwmFlushFunction = HRESULT(WINAPI*)();
    if (auto flush = SystemFunction<DwmFlushFunction>(L"dwmapi.dll", "DwmFlush")) flush();
    return curtain;
}

DWORD WINAPI CurtainThread(void* parameter) {
    const std::string description = *static_cast<std::string*>(parameter);
    delete static_cast<std::string*>(parameter);
    using SetAwareness = void*(WINAPI*)(void*);
    if (auto set = SystemFunction<SetAwareness>(L"user32.dll", "SetThreadDpiAwarenessContext"))
        set(reinterpret_cast<void*>(static_cast<intptr_t>(-4)));  // PER_MONITOR_AWARE_V2: real pixels, as SDL uses
    const size_t separator = description.find(' ');
    const HWND old = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::strtoull(description.c_str(), nullptr, 10)));
    const DWORD oldProcess = static_cast<DWORD>(std::strtoul(description.c_str() + separator + 1, nullptr, 10));
    HWND curtain = CreateCurtain(old);
    g_curtain.store(curtain);
    if (curtain) SetTimer(curtain, 1, kCurtainLifeMs, nullptr);
    if (HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, CurtainEventName(oldProcess).c_str())) {
        SetEvent(ready);
        CloseHandle(ready);
    }
    if (!curtain) return 0;
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

std::filesystem::path ExecutableDirectory() {
    std::wstring exe(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (length == 0) return {};
        if (length < exe.size()) { exe.resize(length); break; }
        exe.resize(exe.size() * 2);
    }
    return std::filesystem::path(exe).parent_path();
}
#else
// Starts the other executable, passing on --iso when the image came from it, and, like
// PrimeHostOsRestart, names this process as the one it waits for (prime_host_restart.cpp).
bool StartExecutable(const std::filesystem::path& executable) {
    std::vector<std::string> args{executable.string()};
    if (!RuntimeConfigFile::PrimeIsoOverride().empty()) args.insert(args.end(), {"--iso", RuntimeConfigFile::PrimeIsoOverride()});
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    const std::string parent = std::to_string(getpid());
    setenv("WII_COMPILED_PRIME_RESTART_PARENT", parent.c_str(), 1);
    pid_t child = 0;
    const int error = posix_spawn(&child, args[0].c_str(), nullptr, nullptr, argv.data(), environ);
    unsetenv("WII_COMPILED_PRIME_RESTART_PARENT");
    if (error != 0) {
        RT_LOG(RT_TAG_OS) << "[trilogy-exec] cannot start " << args[0] << " errno=" << error << std::endl;
        return false;
    }
    RT_LOG(RT_TAG_OS) << "[trilogy-exec] started " << args[0] << " pid=" << child << std::endl;
    return true;
}

std::filesystem::path ExecutableDirectory() {
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
}
#endif
}  // namespace

// First thing in RuntimeMain, before waiting for the old process to exit.
void PrimeTrilogyCurtainBegin() {
#if defined(_WIN32)
    // Both executables are one taskbar button, which a window change then does not move.
    using SetAppId = HRESULT(WINAPI*)(const wchar_t*);
    if (auto set = SystemFunction<SetAppId>(L"shell32.dll", "SetCurrentProcessExplicitAppUserModelID"))
        set(L"Corruption3PC.Trilogy");
    const char* value = std::getenv(kCurtainVariable);
    if (value == nullptr || value[0] == 0) return;
    auto* description = new std::string(value);
    _putenv_s(kCurtainVariable, "");
    if (HANDLE thread = CreateThread(nullptr, 0, CurtainThread, description, 0, nullptr)) CloseHandle(thread);
    else delete description;
#endif
}

// After each present of the window this process owns.
extern "C" void PrimeTrilogyFramePresented() {
#if defined(_WIN32)
    static unsigned frames = 0;
    if (frames >= kCurtainFrames || ++frames < kCurtainFrames) return;
    const HWND curtain = g_curtain.load();
    if (!curtain) return;
    RT_LOG(RT_TAG_OS) << "[trilogy-exec] curtain down" << std::endl;
    PostMessageW(curtain, WM_TIMER, 0, 0);
#endif
}

// Overlay at the entry of OSExec. r3 is the DOL path; regionVariable is the small-data
// pair OSSetSaveRegion wrote. It does not return when it acts.
extern "C" bool PrimeTrilogyExec(CpuContext* ctx, uint32_t regionVariable) {
    const std::string dol = GuestString(ctx->gpr[3]);
    const uint32_t start = Memory::Read32(regionVariable);
    const uint32_t end = Memory::Read32(regionVariable + 4u);
    RT_LOG(RT_TAG_OS) << "[trilogy-exec] OSExec " << dol << " save_region=0x" << std::hex << start << "..0x" << end << std::dec << std::endl;
#if defined(_WIN32)
    const bool game = dol == "/rs5mp3_p.dol";
    const bool menu = dol == "/rs5fe_p.dol";
    if (game || menu) {
        SaveRegion(start, end);
    } else {
        std::error_code ec;
        std::filesystem::remove(StatePath(), ec);
        MessageBoxA(nullptr, "This port only runs Metroid Prime 3: Corruption from Metroid Prime Trilogy.\n\nReturning to the menu.",
                    "Metroid Prime Trilogy", MB_OK | MB_ICONINFORMATION | MB_TASKMODAL);
    }
    const std::filesystem::path target = ExecutableDirectory() / (game ? kGameExecutable : kMenuExecutable);
    if (!StartExecutable(target)) {
        RT_LOG(RT_TAG_OS) << "[trilogy-exec] cannot start " << RuntimeConfigFile::PathToUtf8(target) << " error=" << GetLastError() << std::endl;
        MessageBoxW(nullptr, (L"Could not start:\n" + target.wstring()).c_str(), L"Metroid Prime Trilogy", MB_OK | MB_ICONERROR | MB_TASKMODAL);
    }
#else
    const bool game = dol == "/rs5mp3_p.dol";
    const bool menu = dol == "/rs5fe_p.dol";
    if (game || menu) {
        SaveRegion(start, end);
    } else {
        std::error_code ec;
        std::filesystem::remove(StatePath(), ec);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Metroid Prime Trilogy",
                                 "This port only runs Metroid Prime 3: Corruption from Metroid Prime Trilogy.\n\nReturning to the menu.", nullptr);
    }
    const std::filesystem::path target = ExecutableDirectory() / (game ? kGameExecutable : kMenuExecutable);
    if (!StartExecutable(target)) {
        const std::string message = "Could not start:\n" + target.string();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Metroid Prime Trilogy", message.c_str(), nullptr);
    }
#endif
    ExitForAuroraWindowClose();
}

// Overlay at the entry of OSGetSavedRegion's only caller, which has that leaf inlined:
// restores the block and fills the two words the leaf reads.
extern "C" void PrimeTrilogySavedRegion(uint32_t startVariable) {
    uint32_t start = 0, size = 0;
    std::ifstream file(StatePath(), std::ios::binary);
    uint32_t header[3] = {};
    if (file.read(reinterpret_cast<char*>(header), sizeof(header)) && header[0] == kStateMagic &&
        header[2] <= kMaximumRegion && Memory::Contains(header[1], header[2])) {
        std::vector<char> bytes(header[2]);
        if (file.read(bytes.data(), bytes.size())) {
            std::memcpy(Memory::GetPointer(header[1], header[2]), bytes.data(), bytes.size());
            start = header[1];
            size = header[2];
        }
    }
    file.close();
    std::error_code ec;
    std::filesystem::remove(StatePath(), ec);
    Memory::Write32(startVariable, start);
    Memory::Write32(startVariable + 4u, size ? start + size : 0u);
    RT_LOG(RT_TAG_OS) << "[trilogy-exec] saved region 0x" << std::hex << start << " size 0x" << size << std::dec << std::endl;
}
