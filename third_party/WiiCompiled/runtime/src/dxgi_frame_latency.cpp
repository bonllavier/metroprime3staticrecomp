// Presentation latency on Windows (D3D12 through Dawn).
//
// Measured with the presenter's [present-latency] trace: Present() blocked for
// a whole display period (~15 ms) on every frame. A flip-model swap chain made
// without DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT lets DXGI queue up
// to three frames and blocks Present() once the queue is full, so the image on
// screen is two to three frames behind the one just rendered. Dawn creates its
// swap chain internally; this file hooks IDXGIFactory2::CreateSwapChainForHwnd
// (one shared vtable per process) to add the waitable flag, limits the queue to
// `frames` with IDXGISwapChain2::SetMaximumFrameLatency, keeps the flag across
// Dawn's ResizeBuffers calls and waits on the latency object right before each
// Present/Present1, which is the documented use of that object. Off: config
// [prime] swapchain_frame_latency = 0.
//
// Exclusive fullscreen: Aurora presents without V-Sync (Immediate) because the
// guest paces itself. Windowed and borderless, DWM composes at the display's
// refresh, so nothing tears; exclusive fullscreen flips directly and the swap
// landed mid-scanout (horizontal bands of the previous frame, user report
// 2026-10-31). There the hooks present with sync interval 1 and no tearing
// flag; the latency wait above already holds the presenter to one queued frame,
// so Present does not block longer than it did composed.
// WII_COMPILED_EXCLUSIVE_VSYNC=0 keeps the tearing presentation (A/B).
//
// Frame interpolation: images come faster than the display flips and the latency wait would hold each
// one for a whole refresh, with the device lock taken, stalling encoding and through it the game
// (exactly 60 images/s and 30 real frames on a 60 Hz monitor). DXGI enforces the queue limit by itself
// as well, so while it is on the limit is raised, the object is only drained, the swap chain is in
// Mailbox and the newest image is shown at each refresh, without tearing. The higher limit still
// applies backpressure when the GPU falls behind, which the rate controller then sees.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>

#include "runtime_log.h"

#include <aurora/aurora.h>
#include <aurora/gfx.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

// COM vtable slots (IUnknown 3, IDXGIObject 4, IDXGIDeviceSubObject 1).
constexpr size_t kFactory2CreateSwapChainForHwnd = 15;
constexpr size_t kSwapChainPresent = 8, kSwapChainResizeBuffers = 13, kSwapChain1Present1 = 22;

CreateSwapChainForHwndFn g_originalCreate = nullptr;
ResizeBuffersFn g_originalResize = nullptr;
PresentFn g_originalPresent = nullptr;
Present1Fn g_originalPresent1 = nullptr;
std::atomic<UINT> g_frames{1};
constexpr UINT kInterpolationFrames = 4;
std::atomic<UINT> g_appliedFrames{0};
std::atomic<HANDLE> g_waitable{nullptr};
std::atomic<IDXGISwapChain*> g_swapChain{nullptr};
bool g_swapChainVtablePatched = false;

bool PatchSlot(void** vtable, size_t slot, void* replacement, void** original) {
    void** entry = vtable + slot;
    DWORD previous = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &previous)) return false;
    *original = *entry;
    *entry = replacement;
    DWORD ignored = 0;
    VirtualProtect(entry, sizeof(void*), previous, &ignored);
    return true;
}

// The queue limit follows interpolation, which F1 can turn on and off at any time.
void ApplyFrameLatency(IDXGISwapChain* self) {
    const UINT wanted = aurora_get_frame_interpolation_fps() != 0 ? kInterpolationFrames : g_frames.load();
    if (g_appliedFrames.load() == wanted) return;
    IDXGISwapChain2* swapChain2 = nullptr;
    if (SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&swapChain2))) && swapChain2) {
        if (SUCCEEDED(swapChain2->SetMaximumFrameLatency(wanted))) {
            RT_LOG(RT_TAG_RUNTIME) << "[dxgi-frame-latency] swap chain queue limit now " << wanted << std::endl;
        }
        swapChain2->Release();
    }
    g_appliedFrames.store(wanted);
}

void WaitForLatencyObject() {
    HANDLE handle = g_waitable.load();
    if (handle) WaitForSingleObjectEx(handle, aurora_get_frame_interpolation_fps() != 0 ? 0 : 100, FALSE);
}

// Sync to the display in exclusive fullscreen (see the header comment).
void SyncInExclusiveFullscreen(UINT& syncInterval, UINT& flags) {
    static const bool enabled = [] {
        const char* value = std::getenv("WII_COMPILED_EXCLUSIVE_VSYNC");
        return !(value && value[0] == '0');
    }();
    if (!enabled || aurora_get_display_mode() != AURORA_DISPLAY_MODE_EXCLUSIVE) return;
    if (aurora_get_frame_interpolation_fps() != 0) return;  // Mailbox already avoids tearing, and must not block
    if (syncInterval == 0) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            RT_LOG(RT_TAG_RUNTIME) << "[dxgi-frame-latency] exclusive fullscreen: presenting with V-Sync to avoid tearing" << std::endl;
    }
    syncInterval = 1;
    flags &= ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING);
}

#if defined(MKW_PRIME_QA_TOOLS)
// Private QA: where a Present call spends its time, every 600 calls.
struct PresentProbe {
    const char* name;
    uint64_t calls = 0, waitTicks = 0, presentTicks = 0, maxPresentTicks = 0;
    void Note(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER c, UINT syncInterval, UINT flags) {
        waitTicks += b.QuadPart - a.QuadPart;
        const uint64_t present = c.QuadPart - b.QuadPart;
        presentTicks += present;
        if (present > maxPresentTicks) maxPresentTicks = present;
        if (++calls < 600) return;
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const double toMs = 1000.0 / static_cast<double>(frequency.QuadPart);
        RT_LOG(RT_TAG_RUNTIME) << "[dxgi-present] " << name << " calls=" << calls << " wait_ms=" << waitTicks * toMs / calls
                               << " present_ms=" << presentTicks * toMs / calls << " max_present_ms=" << maxPresentTicks * toMs
                               << " sync=" << syncInterval << " flags=0x" << std::hex << flags << std::dec << std::endl;
        calls = waitTicks = presentTicks = maxPresentTicks = 0;
    }
};
#endif

#if defined(MKW_PRIME_QA_TOOLS)
// Private QA (WII_COMPILED_QA_DISPLAY_CADENCE=1): what actually reached the screen, from DXGI's frame
// statistics. Each statistic names the newest present on screen and the refresh it first appeared at. A
// thread of its own reads them about every millisecond, so every present that reaches the screen is seen
// (each stays at least one refresh): between two in a row, every present in between was dropped and every
// refresh past the first repeated an image. Read right after each Present instead, a reading could land
// exactly when DXGI updates and miss one, which looked like a drop and a repeat that never happened.
// The swap chain is held with a reference of our own, released before a new one is created.
std::mutex g_cadenceMutex;
IDXGISwapChain* g_cadenceSwapChain = nullptr;  // guarded by g_cadenceMutex

bool DisplayCadenceEnabled() {
    static const bool enabled = std::getenv("WII_COMPILED_QA_DISPLAY_CADENCE") != nullptr;
    return enabled;
}

void ReleaseDisplayCadenceSwapChain() {
    if (!DisplayCadenceEnabled()) return;
    std::lock_guard<std::mutex> lock(g_cadenceMutex);
    if (g_cadenceSwapChain) g_cadenceSwapChain->Release();
    g_cadenceSwapChain = nullptr;
}

void DisplayCadenceMain() {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */,
                                          TIMER_MODIFY_STATE | SYNCHRONIZE);
    LARGE_INTEGER due{};
    due.QuadPart = -10000;  // 1 ms
    if (timer) SetWaitableTimerEx(timer, &due, 1, nullptr, nullptr, nullptr, 0);
    bool started = false;
    UINT lastPresent = 0, lastRefresh = 0;
    uint64_t shown = 0, clean = 0, repeats = 0, dropped = 0, failures = 0, polls = 0;
    LARGE_INTEGER frequency, nextReport{};
    QueryPerformanceFrequency(&frequency);
    for (;;) {
        if (timer) WaitForSingleObject(timer, 100); else Sleep(1);
        DXGI_FRAME_STATISTICS stats{};
        HRESULT hr = E_FAIL;
        {
            std::lock_guard<std::mutex> lock(g_cadenceMutex);
            if (g_cadenceSwapChain) hr = g_cadenceSwapChain->GetFrameStatistics(&stats);
        }
        ++polls;
        if (FAILED(hr)) {
            if (hr != E_FAIL) ++failures;
            started = false;
        } else if (!started) {
            started = true;
            lastPresent = stats.PresentCount;
            lastRefresh = stats.PresentRefreshCount;
        } else if (stats.PresentCount != lastPresent) {
            const UINT presents = stats.PresentCount - lastPresent;
            const UINT refreshes = stats.PresentRefreshCount - lastRefresh;
            lastPresent = stats.PresentCount;
            lastRefresh = stats.PresentRefreshCount;
            if (presents > 600 || refreshes > 600) continue;  // a restart of the counters, not a gap
            ++shown;
            if (presents == 1 && refreshes == 1) ++clean;
            dropped += presents - 1;
            repeats += refreshes > 1 ? refreshes - 1 : 0;
            lastPresent = stats.PresentCount;
            lastRefresh = stats.PresentRefreshCount;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart < nextReport.QuadPart) continue;
        const bool first = nextReport.QuadPart == 0;
        nextReport.QuadPart = now.QuadPart + frequency.QuadPart * 10;
        if (!first) {
            RT_LOG(RT_TAG_RUNTIME) << "[display-cadence] polls=" << polls << " shown=" << shown << " clean_steps=" << clean
                                   << " repeated_refreshes=" << repeats << " dropped_presents=" << dropped
                                   << " failures=" << failures << std::endl;
        }
        shown = clean = repeats = dropped = failures = polls = 0;
    }
}

// Called after each Present of the game's swap chain: adopts it for the reader and starts the reader once.
void NoteDisplayCadence(IDXGISwapChain* swapChain) {
    if (!DisplayCadenceEnabled()) return;
    {
        std::lock_guard<std::mutex> lock(g_cadenceMutex);
        if (g_cadenceSwapChain != swapChain) {
            if (g_cadenceSwapChain) g_cadenceSwapChain->Release();
            swapChain->AddRef();
            g_cadenceSwapChain = swapChain;
        }
    }
    static std::once_flag once;
    std::call_once(once, [] { std::thread(DisplayCadenceMain).detach(); });
}
#endif

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* self, UINT syncInterval, UINT flags) {
#if defined(MKW_PRIME_QA_TOOLS)
    LARGE_INTEGER started, waited, presented;
    QueryPerformanceCounter(&started);
#endif
    if (self == g_swapChain.load()) {
        ApplyFrameLatency(self);
        WaitForLatencyObject();
        SyncInExclusiveFullscreen(syncInterval, flags);
    }
#if defined(MKW_PRIME_QA_TOOLS)
    QueryPerformanceCounter(&waited);
    const HRESULT result = g_originalPresent(self, syncInterval, flags);
    QueryPerformanceCounter(&presented);
    static PresentProbe probe{"Present"};
    probe.Note(started, waited, presented, syncInterval, flags);
    if (self == g_swapChain.load()) NoteDisplayCadence(self);
    return result;
#else
    return g_originalPresent(self, syncInterval, flags);
#endif
}

HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* self, UINT syncInterval, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* parameters) {
    if (self == g_swapChain.load()) {
        ApplyFrameLatency(self);
        WaitForLatencyObject();
        SyncInExclusiveFullscreen(syncInterval, flags);
    }
#if defined(MKW_PRIME_QA_TOOLS)
    const HRESULT result = g_originalPresent1(self, syncInterval, flags, parameters);
    if (self == g_swapChain.load()) NoteDisplayCadence(self);
    return result;
#else
    return g_originalPresent1(self, syncInterval, flags, parameters);
#endif
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* self, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                              UINT flags) {
    if (self == g_swapChain.load()) flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    return g_originalResize(self, count, width, height, format, flags);
}

void AdoptSwapChain(IDXGISwapChain1* swapChain) {
    IDXGISwapChain2* swapChain2 = nullptr;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain2))) || !swapChain2) {
        RT_LOG(RT_TAG_RUNTIME) << "[dxgi-frame-latency] IDXGISwapChain2 unavailable; queue left at the DXGI default" << std::endl;
        return;
    }
    const HRESULT latency = swapChain2->SetMaximumFrameLatency(g_frames.load());
    g_appliedFrames.store(g_frames.load());
    HANDLE handle = SUCCEEDED(latency) ? swapChain2->GetFrameLatencyWaitableObject() : nullptr;
    HANDLE previous = g_waitable.exchange(handle);
    if (previous) CloseHandle(previous);
    g_swapChain.store(swapChain);
    if (!g_swapChainVtablePatched) {
        void** vtable = *reinterpret_cast<void***>(swapChain);
        g_swapChainVtablePatched =
            PatchSlot(vtable, kSwapChainResizeBuffers, reinterpret_cast<void*>(&HookedResizeBuffers), reinterpret_cast<void**>(&g_originalResize)) &&
            PatchSlot(vtable, kSwapChainPresent, reinterpret_cast<void*>(&HookedPresent), reinterpret_cast<void**>(&g_originalPresent)) &&
            PatchSlot(vtable, kSwapChain1Present1, reinterpret_cast<void*>(&HookedPresent1), reinterpret_cast<void**>(&g_originalPresent1));
    }
    RT_LOG(RT_TAG_RUNTIME) << "[dxgi-frame-latency] swap chain queue limited to " << g_frames.load() << " frame(s): hr=0x" << std::hex
                           << static_cast<uint32_t>(latency) << std::dec << " waitable=" << (handle != nullptr)
                           << " vtable_hooked=" << g_swapChainVtablePatched << std::endl;
    swapChain2->Release();
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window,
                                                       const DXGI_SWAP_CHAIN_DESC1* desc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output,
                                                       IDXGISwapChain1** out) {
    if (!desc) return g_originalCreate(factory, device, window, desc, fullscreen, output, out);
#if defined(MKW_PRIME_QA_TOOLS)
    ReleaseDisplayCadenceSwapChain();  // the window takes one flip swap chain at a time
#endif
    DXGI_SWAP_CHAIN_DESC1 modified = *desc;
    modified.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    HRESULT hr = g_originalCreate(factory, device, window, &modified, fullscreen, output, out);
    if (FAILED(hr)) {
        RT_LOG(RT_TAG_RUNTIME) << "[dxgi-frame-latency] waitable swap chain refused (hr=0x" << std::hex << static_cast<uint32_t>(hr)
                               << std::dec << "); creating it as Dawn asked" << std::endl;
        return g_originalCreate(factory, device, window, desc, fullscreen, output, out);
    }
    if (out && *out) AdoptSwapChain(*out);
    return hr;
}
}  // namespace

// The NVIDIA driver's own frame limiter ("Max Frame Rate" in the NVIDIA Control Panel) caps every
// presented image of the process, inserted ones included, and sleeps inside Present to do it. On the
// user's PC (2026-11-07) the global profile held 60: Present took ~10 ms per call with the GPU at 14 %,
// so frame interpolation could never pass 60 images/s and only cost real frames. Read once, read only:
// the profile of this executable if it has one, else the global profile. 0 = no limit or no NVIDIA.
namespace {
struct NvDrsSetting {
    uint32_t version;
    uint16_t name[2048];
    uint32_t settingId;
    uint32_t settingType;
    uint32_t settingLocation;
    uint32_t isCurrentPredefined;
    uint32_t isPredefinedValid;
    uint8_t predefinedValue[4100];
    uint8_t currentValue[4100];
};
struct NvDrsApplicationV1 {
    uint32_t version;
    uint32_t isPredefined;
    uint16_t appName[2048];
    uint16_t userFriendlyName[2048];
    uint16_t launcher[2048];
};

uint32_t ReadNvidiaFrameRateLimit() {
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi) return 0;
    using QueryFn = void*(__cdecl*)(uint32_t);
    const auto query = reinterpret_cast<QueryFn>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!query) return 0;
    using InitializeFn = int(__cdecl*)();
    using CreateSessionFn = int(__cdecl*)(void**);
    using SessionFn = int(__cdecl*)(void*);
    using FindApplicationFn = int(__cdecl*)(void*, const wchar_t*, void**, NvDrsApplicationV1*);
    using GlobalProfileFn = int(__cdecl*)(void*, void**);
    using GetSettingFn = int(__cdecl*)(void*, void*, uint32_t, NvDrsSetting*);
    const auto initialize = reinterpret_cast<InitializeFn>(query(0x0150E828u));
    const auto createSession = reinterpret_cast<CreateSessionFn>(query(0x0694D52Eu));
    const auto loadSettings = reinterpret_cast<SessionFn>(query(0x375DBD6Bu));
    const auto destroySession = reinterpret_cast<SessionFn>(query(0xDAD9CFF8u));
    const auto findApplication = reinterpret_cast<FindApplicationFn>(query(0xEEE566B2u));
    const auto globalProfile = reinterpret_cast<GlobalProfileFn>(query(0x617BFF9Fu));
    const auto getSetting = reinterpret_cast<GetSettingFn>(query(0x73BF8338u));
    if (!initialize || !createSession || !loadSettings || !destroySession || !findApplication || !globalProfile ||
        !getSetting || initialize() != 0) {
        return 0;
    }
    void* session = nullptr;
    if (createSession(&session) != 0 || !session) return 0;
    uint32_t limit = 0;
    if (loadSettings(session) == 0) {
        constexpr uint32_t kFrameRateLimitId = 0x10835002u;  // FRL_FPS_ID, "Max Frame Rate"
        const auto read = [&](void* profile, uint32_t& value) {
            auto setting = std::make_unique<NvDrsSetting>();
            std::memset(setting.get(), 0, sizeof(NvDrsSetting));
            setting->version = static_cast<uint32_t>(sizeof(NvDrsSetting)) | (1u << 16);
            if (getSetting(session, profile, kFrameRateLimitId, setting.get()) != 0) return false;
            std::memcpy(&value, setting->currentValue, sizeof(value));
            return true;
        };
        wchar_t path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        const wchar_t* name = path;
        for (DWORD i = 0; i < length; ++i) {
            if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
        }
        auto application = std::make_unique<NvDrsApplicationV1>();
        std::memset(application.get(), 0, sizeof(NvDrsApplicationV1));
        application->version = static_cast<uint32_t>(sizeof(NvDrsApplicationV1)) | (1u << 16);
        void* profile = nullptr;
        uint32_t value = 0;
        const bool fromApplication = findApplication(session, name, &profile, application.get()) == 0 && profile &&
                                     read(profile, value);
        if (!fromApplication) {
            profile = nullptr;
            if (globalProfile(session, &profile) == 0 && profile && read(profile, value)) limit = value;
        } else {
            limit = value;
        }
        RT_LOG(RT_TAG_RUNTIME) << "[frame-limit] NVIDIA driver Max Frame Rate for this program: "
                               << (limit != 0 ? std::to_string(limit) + " FPS" : std::string("off"))
                               << (fromApplication ? " (program profile)" : " (global profile)") << std::endl;
    }
    destroySession(session);
    return limit;
}
}  // namespace

extern "C" uint32_t DriverFrameRateLimit() {
    static const uint32_t limit = ReadNvidiaFrameRateLimit();
    return limit;
}

// Call before the graphics backend creates its swap chain. `frames` 0 disables.
extern "C" void InstallDxgiFrameLatencyLimit(unsigned frames) {
    if (frames == 0 || g_originalCreate) return;
    g_frames.store(frames);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return;
    using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto createFactory1 = reinterpret_cast<CreateFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (!createFactory1) return;
    IDXGIFactory2* factory = nullptr;
    if (FAILED(createFactory1(IID_PPV_ARGS(&factory))) || !factory) {
        RT_LOG(RT_TAG_RUNTIME) << "[dxgi-frame-latency] IDXGIFactory2 unavailable; presentation queue unchanged" << std::endl;
        return;
    }
    void** vtable = *reinterpret_cast<void***>(factory);
    const bool hooked = PatchSlot(vtable, kFactory2CreateSwapChainForHwnd, reinterpret_cast<void*>(&HookedCreateSwapChainForHwnd),
                                  reinterpret_cast<void**>(&g_originalCreate));
    factory->Release();
    RT_LOG(RT_TAG_RUNTIME) << "[dxgi-frame-latency] CreateSwapChainForHwnd hook " << (hooked ? "installed" : "failed")
                           << ", target queue " << frames << " frame(s)" << std::endl;
}
#else
#include <cstdint>
extern "C" void InstallDxgiFrameLatencyLimit(unsigned) {}
extern "C" uint32_t DriverFrameRateLimit() { return 0; }
#endif
