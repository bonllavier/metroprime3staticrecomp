#include "reshade_loader.h"

#include "runtime_config.h"
#include "runtime_log.h"

#include <mutex>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include "../../../reshade/include/reshade.hpp"
#endif
#include <atomic>

namespace ReShadeLoader {
namespace {
std::mutex g_mutex;
std::string g_status = "not started";
#if defined(_WIN32)
std::atomic<reshade::api::effect_runtime*> g_runtime{nullptr};
bool g_weDisabled = false;  // effects were switched off by the gate, not by the player

void OnInitRuntime(reshade::api::effect_runtime* runtime) { g_runtime.store(runtime); }
void OnDestroyRuntime(reshade::api::effect_runtime* runtime) {
    reshade::api::effect_runtime* expected = runtime;
    g_runtime.compare_exchange_strong(expected, nullptr);
}
#endif

#if defined(_WIN32)
std::string FileVersion(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    DWORD handle = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(wide.c_str(), &handle);
    if (size == 0) return {};
    std::string block(size, '\0');
    if (!::GetFileVersionInfoW(wide.c_str(), 0, size, block.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!::VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) || !info) return {};
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(info->dwFileVersionLS));
}
#endif
} // namespace

std::filesystem::path Directory() {
    if (const auto& root = RuntimeConfigFile::PortableRootDirectory()) return *root / "reshade";
    if (const auto exe = RuntimeConfigFile::ExecutableDirectory()) return *exe / "reshade";
    return std::filesystem::path("reshade");
}

void LoadIfEnabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!RuntimeConfigFile::ReshadeEnabled()) {
        g_status = "disabled";
        return;
    }
#if defined(_WIN32)
    const auto dll = Directory() / "ReShade64.dll";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(dll, ec)) {
        g_status = "not found: " + RuntimeConfigFile::PathToUtf8(dll);
        RT_LOG(RT_TAG_RUNTIME) << "[reshade] " << g_status << std::endl;
        return;
    }
    // Loaded by hand (not as a dxgi/d3d12 proxy) ReShade would take the executable's folder as its
    // base and refuse to start unless ReShade.ini sits there. The documented override points it at
    // our folder instead, so ReShade.ini, the preset, the log and the shader folders all live in
    // reshade/ and nothing is written beside the executable.
    const std::wstring base = Directory().wstring();
    ::SetEnvironmentVariableW(L"RESHADE_BASE_PATH_OVERRIDE", base.c_str());
    HMODULE module = ::LoadLibraryExW(dll.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        g_status = "failed to load (Windows error " + std::to_string(::GetLastError()) + ")";
        RT_LOG(RT_TAG_RUNTIME) << "[reshade] " << g_status << std::endl;
        return;
    }
    const std::string version = FileVersion(dll);
    g_status = "loaded" + (version.empty() ? std::string() : " " + version) + " (Home opens its menu)";
    // Register the executable as a ReShade add-on so effects can be limited to the 3D scene.
    if (reshade::register_addon(::GetModuleHandleW(nullptr), module)) {
        reshade::register_event<reshade::addon_event::init_effect_runtime>(&OnInitRuntime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyRuntime);
        g_status += "; effects limited to 3D scenes";
    } else {
        g_status += "; add-on API unavailable (effects apply to the whole picture)";
    }
    RT_LOG(RT_TAG_RUNTIME) << "[reshade] loaded " << RuntimeConfigFile::PathToUtf8(dll) << " version " << version << std::endl;
#else
    g_status = "not supported on this platform";
#endif
}

void SetEffectsAllowed(bool allowed) {
#if defined(_WIN32)
    reshade::api::effect_runtime* runtime = g_runtime.load();
    if (runtime == nullptr) return;
    if (allowed) {
        if (g_weDisabled) { runtime->set_effects_state(true); g_weDisabled = false; }
    } else if (runtime->get_effects_state()) {
        runtime->set_effects_state(false);
        g_weDisabled = true;
    }
#else
    (void)allowed;
#endif
}

std::string Status() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

} // namespace ReShadeLoader
