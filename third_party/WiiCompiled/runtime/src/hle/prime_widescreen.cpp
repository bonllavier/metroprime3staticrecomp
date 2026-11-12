// RM3E01 ultrawide (Hor+), GitHub issue #7. Model and addresses: hle/prime_widescreen.h.
//
// Game thread: the overlays in tools/prepare_prime_runtime_hooks.py call
//   PrimeWidescreenViewportAspect  end of 0x800BE034 (viewport aspect)
//   PrimeWidescreenNoteViewport    start of 0x800BF0C4 (the viewport being updated)
//   PrimeWidescreenFlush           0x803EB060, right before the projection matrix
// and PrimeWidescreenOnSurface runs at every frame boundary (aurora_events.h).
#if defined(MKW_GAME_PROFILE_RM3E01_00)
#include "hle/prime_widescreen.h"

#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace PrimeWidescreen {
namespace {

// CGraphics::mViewport: left, top, width, height (s32).
constexpr uint32_t kGraphicsViewport = 0x8059E188u;
// Return addresses of the GUI cameras' CGraphics::SetPerspective calls
// (CGuiCamera draw 0x80433BD8 and DrawSqueezedForWidescreen 0x80433CF8).
constexpr uint32_t kGuiCameraPerspectiveReturn = 0x80433C80u;
constexpr uint32_t kGuiCameraSqueezedPerspectiveReturn = 0x80433DA8u;
// CGameViewport vtable (constructor 0x800BDC50) and its "active" bit: 0x800BDFE0
// writes it (bit 7 of +0x58 = argument), and only active viewports are rendered
// and rebuilt. 0x802B2094 calls 0x800BE0DC for every active viewport each frame,
// which recomputes the aspect through 0x800BF0C4 -> 0x800BE034, so a new factor
// reaches the view on the next frame without touching the game's state.
// Never set this bit: 0x8006AAC0 creates "xray_viewport" (500x200, the X-ray
// visor's window in the middle of the EFB) and "cinematic_viewport" inactive;
// setting the bit on them (12/11/2026) drew a grey box of stale X-ray frames
// over the visor and rendered the world twice.
constexpr uint32_t kGameViewportVtable = 0x80591498u;
constexpr uint32_t kGameViewportFlags = 0x58u;
constexpr uint8_t kGameViewportActive = 0x80u;

std::mutex g_settingMutex;
Setting g_setting{};
bool g_settingLoaded = false;

// Game thread.
float g_factor = 1.0f;
float g_targetAspect = kBaseAspect;
bool g_presented = false;
uint32_t g_frame = 0;
// Aspects this factor produced in 0x800BE034 (and the GUI cameras), for the
// test build's flush policy; the list lives until the factor changes.
std::array<float, 8> g_widenedAspects{};
size_t g_widenedCount = 0;
void RememberWidened(float aspect) {
    for (size_t i = 0; i < g_widenedCount; ++i) {
        if (g_widenedAspects[i] == aspect) return;
    }
    if (g_widenedCount < g_widenedAspects.size()) {
        g_widenedAspects[g_widenedCount++] = aspect;
    } else {
        g_widenedAspects[g_frame % g_widenedAspects.size()] = aspect;
    }
}

// Pre-rendered movies (THP) are drawn as a full-screen quad: wider than 16:9 they
// would be stretched. While the decoder (0x804EB174) produces frames, and for a
// short while after, the image is presented at 16:9.
std::atomic<uint32_t> g_lastVideoFrame{0};
std::atomic<bool> g_videoSeen{false};
constexpr uint32_t kVideoHoldFrames = 20;
bool g_videoActive = false;

std::atomic<float> g_shownFactor{1.0f};
std::atomic<float> g_shownAspect{kBaseAspect};

bool Valid(uint32_t address, size_t length = 4) { return address != 0 && (address & 3u) == 0 && Memory::Contains(address, length); }

Setting LoadedSetting() {
    std::lock_guard<std::mutex> lock(g_settingMutex);
    if (!g_settingLoaded) {
        g_settingLoaded = true;
        if (const auto& text = RuntimeConfigFile::Get().primeAspectRatio) {
            if (auto parsed = Parse(*text)) g_setting = *parsed;
        }
    }
    return g_setting;
}

// The viewport currently set (CGraphics::mViewport).
void CurrentViewport(uint32_t& width, uint32_t& height) {
    width = Valid(kGraphicsViewport + 8) ? Memory::Read32(kGraphicsViewport + 8) : 0;
    height = Valid(kGraphicsViewport + 12) ? Memory::Read32(kGraphicsViewport + 12) : 0;
    if (width > 4096 || height > 4096) width = height = 0;
}

// The whole screen: the largest viewport the game has set (its EFB). Shadow
// maps and other render-to-texture passes use smaller viewports.
uint32_t g_screenWidth = 0, g_screenHeight = 0;
void ScreenSize(uint32_t& width, uint32_t& height) {
    uint32_t w = 0, h = 0;
    CurrentViewport(w, h);
    g_screenWidth = std::max(g_screenWidth, w);
    g_screenHeight = std::max(g_screenHeight, h);
    width = g_screenWidth;
    height = g_screenHeight;
}

#if defined(MKW_PRIME_QA_TOOLS)
// WII_COMPILED_PRIME_PROJECTION_TRACE=1: every distinct projection the game
// sets, with its callers, once, and a count per 10 s. Test build only.
bool TraceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("WII_COMPILED_PRIME_PROJECTION_TRACE");
        return value && value[0] == '1';
    }();
    return enabled;
}
std::map<std::string, uint32_t> g_traceCounts;
size_t g_traceKeys = 0;

std::string CallerChain(uint32_t sp, uint32_t firstLr) {
    char text[96];
    uint32_t lr[3] = {firstLr, 0, 0};
    for (int i = 1; i < 3; ++i) {
        if (!Valid(sp)) break;
        sp = Memory::Read32(sp);
        if (!Valid(sp + 4)) break;
        lr[i] = Memory::Read32(sp + 4);
    }
    std::snprintf(text, sizeof(text), "%08X<%08X<%08X", lr[0], lr[1], lr[2]);
    return text;
}

void Trace(const char* kind, uint32_t sp, uint32_t lr, double aspect, double widened, uint32_t w, uint32_t h) {
    if (!TraceEnabled()) return;
    uint32_t sw = 0, sh = 0;
    ScreenSize(sw, sh);
    char key[192];
    std::snprintf(key, sizeof(key), "%s %s aspect=%.4f size=%ux%u screen=%ux%u%s", kind, CallerChain(sp, lr).c_str(),
                  aspect, w, h, sw, sh, widened != aspect ? " widened" : "");
    auto [it, inserted] = g_traceCounts.emplace(key, 0);
    ++it->second;
    if (inserted && g_traceKeys++ < 400) {
        RT_LOG(RT_TAG_RUNTIME) << "[prime-uw-trace] new " << key << " factor=" << g_factor << " frame=" << g_frame
                               << std::endl;
    }
}

void TraceSummary() {
    if (!TraceEnabled() || g_frame % 600 != 0 || g_traceCounts.empty()) return;
    RT_LOG(RT_TAG_RUNTIME) << "[prime-uw-trace] frame=" << g_frame << " distinct=" << g_traceCounts.size() << std::endl;
    for (auto& [key, count] : g_traceCounts) {
        if (count) RT_LOG(RT_TAG_RUNTIME) << "[prime-uw-trace]   " << count << "x " << key << std::endl;
        count = 0;
    }
}
#else
inline void Trace(const char*, uint32_t, uint32_t, double, double, uint32_t, uint32_t) {}
inline void TraceSummary() {}
#endif

// Widening at the GX flush every perspective projection on a full-screen
// viewport (1) was measured on 2026-11-11 and rejected: it also widened the Mii
// icon render (0x800F27F4) and a square projection later drawn at 64x64
// (0x802C8238), and projections the game restores from saved copies were
// widened inconsistently. The release widens at the source instead (viewport
// aspect, GUI cameras), so 0 is the default; 1 stays for A/B in the test build.
int FlushPolicy() {
#if defined(MKW_PRIME_QA_TOOLS)
    static const int policy = [] {
        const char* value = std::getenv("WII_COMPILED_PRIME_UW_FLUSH");
        return value && value[0] >= '0' && value[0] <= '9' && !value[1] ? value[0] - '0' : 0;
    }();
    return policy;
#else
    return 0;
#endif
}

} // namespace

void SetSetting(const Setting& setting) {
    std::lock_guard<std::mutex> lock(g_settingMutex);
    g_settingLoaded = true;
    g_setting = setting;
}
Setting CurrentSetting() { return LoadedSetting(); }
float ShownFactor() { return g_shownFactor.load(std::memory_order_relaxed); }
float ShownAspect() { return g_shownAspect.load(std::memory_order_relaxed); }

} // namespace PrimeWidescreen

using namespace PrimeWidescreen;

void PrimeWidescreenNoteVideoFrame() {
    g_lastVideoFrame.store(g_frame, std::memory_order_relaxed);
    g_videoSeen.store(true, std::memory_order_relaxed);
}

// Frame boundary (game thread): the window may have changed shape, or the option.
void PrimeWidescreenOnSurface(uint32_t surfaceWidth, uint32_t surfaceHeight) {
    ++g_frame;
    TraceSummary();
    const bool video = g_videoSeen.load(std::memory_order_relaxed) &&
                       g_frame - g_lastVideoFrame.load(std::memory_order_relaxed) <= kVideoHoldFrames;
    if (video != g_videoActive) {
        g_videoActive = video;
        RT_LOG(RT_TAG_CONFIG) << "[prime-ultrawide] movie " << (video ? "started: 16:9" : "ended") << " frame=" << g_frame << std::endl;
    }
    const bool widescreen = RuntimeConfigFile::WidescreenEnabled(true);
    const float target = widescreen && !video ? TargetAspect(LoadedSetting(), surfaceWidth, surfaceHeight) : kBaseAspect;
    const float factor = widescreen ? Factor(target) : 1.0f;
    if (target == g_targetAspect && g_presented) return;
    const bool factorChanged = factor != g_factor;
    g_targetAspect = target;
    g_factor = factor;
    g_shownFactor.store(factor, std::memory_order_relaxed);
    g_shownAspect.store(target, std::memory_order_relaxed);
    // The 16:9 presentation policy keeps its shape at this aspect (4:3 keeps FIT).
    AuroraSetFixedAspect(target);
    g_presented = true;
    if (factorChanged) g_widenedCount = 0;
    RT_LOG(RT_TAG_CONFIG) << "[prime-ultrawide] aspect=" << target << " factor=" << factor
                          << " setting=" << Format(LoadedSetting()) << " window=" << surfaceWidth << "x" << surfaceHeight
                          << (widescreen ? "" : " (4:3 mode: off)") << std::endl;
}

// Entry of 0x800BF0C4 (a viewport's aspect is being recomputed). Read only: the
// test build's trace records whether the viewport was active.
extern "C" void PrimeWidescreenNoteViewport(uint32_t viewport) {
#if defined(MKW_PRIME_QA_TOOLS)
    if (!TraceEnabled() || !Valid(viewport, kGameViewportFlags + 1) || Memory::Read32(viewport) != kGameViewportVtable) return;
    const bool active = (Memory::Read8(viewport + kGameViewportFlags) & kGameViewportActive) != 0;
    char key[64];
    std::snprintf(key, sizeof(key), "viewport-object %08X %s", viewport, active ? "active" : "inactive");
    auto [it, inserted] = g_traceCounts.emplace(key, 0);
    ++it->second;
    if (inserted && g_traceKeys++ < 400) RT_LOG(RT_TAG_RUNTIME) << "[prime-uw-trace] new " << key << " frame=" << g_frame << std::endl;
#else
    (void)viewport;
#endif
}

// End of 0x800BE034: the aspect the game computed for a w x h viewport.
extern "C" double PrimeWidescreenViewportAspect(double aspect, uint32_t width, uint32_t height, uint32_t lr, uint32_t sp) {
    uint32_t sw = 0, sh = 0;
    ScreenSize(sw, sh);
    double result = aspect;
    if (g_factor > 1.0f && CoversScreen(width, height, sw, sh)) {
        result = WidenAspect(aspect, g_factor);
        RememberWidened(static_cast<float>(result));
    }
    Trace("viewport", sp, lr, aspect, result, width, height);
    // The game stores it with stfs: keep single precision, as fmuls produced it.
    return static_cast<double>(static_cast<float>(result));
}

// Entry of CGraphics::SetPerspective (0x803E91D4): `lr` is the caller's return
// address. Only the GUI cameras are widened here; the 3D view is widened
// through its viewport aspect, which arrives already widened.
extern "C" double PrimeWidescreenGuiAspect(double aspect, uint32_t lr, uint32_t sp) {
    double result = aspect;
    if (g_factor > 1.0f && (lr == kGuiCameraPerspectiveReturn || lr == kGuiCameraSqueezedPerspectiveReturn)) {
        result = static_cast<double>(static_cast<float>(WidenAspect(aspect, g_factor)));
        RememberWidened(static_cast<float>(result));
    }
    uint32_t w = 0, h = 0;
    CurrentViewport(w, h);
    Trace("setperspective", sp, lr, aspect, result, w, h);
    return result;
}

// 0x803EB060, before C_MTXFrustum / C_MTXOrtho: the projection the GX receives.
// The game's own projection state is not touched, so reading it back and
// setting it again never widens twice.
extern "C" void PrimeWidescreenFlush(uint32_t perspective, uint32_t sp, double* left, double* right, double top, double bottom) {
    const Frustum frustum{static_cast<float>(*left), static_cast<float>(*right), static_cast<float>(top), static_cast<float>(bottom)};
    const double aspect = FrustumAspect(frustum);
    double widened = aspect;
    uint32_t vw = 0, vh = 0, sw = 0, sh = 0;
    ScreenSize(sw, sh);
    CurrentViewport(vw, vh);
    const uint32_t lr = Valid(sp) && Valid(Memory::Read32(sp) + 4) ? Memory::Read32(Memory::Read32(sp) + 4) : 0;
    if (perspective && g_factor > 1.0f && FlushPolicy() == 1 && CoversScreen(vw, vh, sw, sh)) {
        bool fromViewport = false;
        for (size_t i = 0; i < g_widenedCount; ++i) {
            if (std::fabs(g_widenedAspects[i] - aspect) <= 0.002 * aspect) fromViewport = true;
        }
        if (!fromViewport) {
            const Frustum wide = WidenFrustum(frustum, g_factor);
            *left = wide.left;
            *right = wide.right;
            widened = FrustumAspect(wide);
        }
    }
    Trace(perspective ? "persp" : "ortho", Valid(sp) ? Memory::Read32(sp) : 0, lr, aspect, widened, vw, vh);
}
#endif
