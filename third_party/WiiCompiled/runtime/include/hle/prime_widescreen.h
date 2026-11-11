#pragma once

// Ultrawide (Hor+) for RM3E01 (GitHub issue #7). Pure model, covered by
// mkw_prime_widescreen_tests; the hooks live in src/hle/prime_widescreen.cpp.
//
// How the game sizes its 3D view (RM3E01, verified in the DOL):
//   0x800BE034 aspect(w, h) = tvFactor * wideFactor * w / h, where tvFactor comes
//              from 0x801B1374 (1.4685 with the 16:9 flag at 0x8067DF01) and
//              wideFactor is 0.93333 in 16:9. Its result is the viewport aspect.
//   0x800BF0C4 stores it at SViewportRenderData+0x68; 0x800BE0DC then builds
//              the frustum planes (0x8036F6AC, culling) and the projection
//              matrix (+0xD0, screen-space conversions) from it.
// Multiplying that aspect by target/(16:9) widens the view horizontally (Hor+)
// with culling and screen projections still agreeing; the vertical FOV is kept.
// Aurora presents the image at the target aspect instead of 16:9.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace PrimeWidescreen {

inline constexpr float kBaseAspect = 16.0f / 9.0f;
// 32:9 is 3.56; a little room above it, then the image is pillarboxed.
inline constexpr float kMaxAspect = 4.0f;
// "21:9" as monitors use it: 2560x1080 / 3440x1440 class panels (64:27).
inline constexpr float kAspect21x9 = 64.0f / 27.0f;
inline constexpr float kAspect32x9 = 32.0f / 9.0f;

enum class Mode : uint8_t {
    Off,     // 16:9, the game's own widescreen (default)
    Window,  // follow the window or monitor shape when it is wider than 16:9
    Fixed,   // a set shape (21:9, 32:9, ...), pillarboxed on wider windows
};

struct Setting {
    Mode mode = Mode::Off;
    float aspect = kBaseAspect;  // used by Fixed
    bool operator==(const Setting& other) const { return mode == other.mode && aspect == other.aspect; }
};

inline float ClampAspect(float aspect) {
    if (!std::isfinite(aspect)) return kBaseAspect;
    return std::clamp(aspect, kBaseAspect, kMaxAspect);
}

// Accepts "16:9"/"off", "window"/"auto", "21:9", "32:9", "W:H", "WxH" (a
// resolution) and plain ratios such as "2.39". Narrower than 16:9 means off.
inline std::optional<Setting> Parse(std::string_view text) {
    std::string value;
    for (char c : text) {
        if (c == ' ' || c == '\t') continue;
        value += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (value.empty()) return std::nullopt;
    if (value == "16:9" || value == "off" || value == "default") return Setting{};
    if (value == "window" || value == "auto" || value == "monitor") return Setting{Mode::Window, kBaseAspect};
    if (value == "21:9") return Setting{Mode::Fixed, kAspect21x9};
    if (value == "32:9") return Setting{Mode::Fixed, kAspect32x9};
    float ratio = 0.0f;
    const size_t split = value.find_first_of(":x");
    try {
        if (split != std::string::npos) {
            size_t usedW = 0, usedH = 0;
            const float w = std::stof(value.substr(0, split), &usedW);
            const float h = std::stof(value.substr(split + 1), &usedH);
            if (usedW != split || usedH != value.size() - split - 1 || !(w > 0.0f) || !(h > 0.0f)) return std::nullopt;
            ratio = w / h;
        } else {
            size_t used = 0;
            ratio = std::stof(value, &used);
            if (used != value.size()) return std::nullopt;
        }
    } catch (...) {
        return std::nullopt;
    }
    if (!(ratio > 0.0f) || !std::isfinite(ratio)) return std::nullopt;
    if (ratio <= kBaseAspect + 0.001f) return Setting{};
    return Setting{Mode::Fixed, ClampAspect(ratio)};
}

inline std::string Format(const Setting& setting) {
    switch (setting.mode) {
    case Mode::Off: return "16:9";
    case Mode::Window: return "window";
    case Mode::Fixed:
        if (std::fabs(setting.aspect - kAspect21x9) < 0.001f) return "21:9";
        if (std::fabs(setting.aspect - kAspect32x9) < 0.001f) return "32:9";
        break;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(setting.aspect));
    return buffer;
}

// The shape the image takes for a window of `width` x `height` pixels.
inline float TargetAspect(const Setting& setting, uint32_t width, uint32_t height) {
    switch (setting.mode) {
    case Mode::Off: return kBaseAspect;
    case Mode::Fixed: return ClampAspect(setting.aspect);
    case Mode::Window:
        if (width == 0 || height == 0) return kBaseAspect;
        return ClampAspect(static_cast<float>(width) / static_cast<float>(height));
    }
    return kBaseAspect;
}

// How much wider than 16:9 the view becomes (1 = unchanged).
inline float Factor(float targetAspect) { return ClampAspect(targetAspect) / kBaseAspect; }

// Only the viewport that covers the whole screen is widened: render-to-texture
// viewports (monitors, the map's own views, Mii icons) keep their shape.
inline bool CoversScreen(uint32_t width, uint32_t height, uint32_t screenWidth, uint32_t screenHeight) {
    if (width == 0 || height == 0 || screenWidth == 0 || screenHeight == 0) return false;
    auto closeTo = [](uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= 2u; };
    return closeTo(width, screenWidth) && closeTo(height, screenHeight);
}

inline double WidenAspect(double aspect, float factor) {
    if (!(factor > 1.0f) || !std::isfinite(aspect) || !(aspect > 0.0)) return aspect;
    return aspect * static_cast<double>(factor);
}

// A frustum given by its four planes (CGraphics projection state), widened
// about its horizontal centre.
struct Frustum {
    float left, right, top, bottom;
};
inline Frustum WidenFrustum(Frustum in, float factor) {
    if (!(factor > 1.0f)) return in;
    const float centre = 0.5f * (in.left + in.right);
    const float half = 0.5f * (in.right - in.left) * factor;
    return {centre - half, centre + half, in.top, in.bottom};
}
inline float FrustumAspect(const Frustum& f) {
    const float h = f.top - f.bottom;
    return h != 0.0f ? std::fabs((f.right - f.left) / h) : 0.0f;
}

// Runtime (src/hle/prime_widescreen.cpp). The setting is read from
// [video] aspect_ratio on first use; F1 changes it live through SetSetting.
void SetSetting(const Setting& setting);
Setting CurrentSetting();
float ShownFactor();   // what the game is using now, for the F1 panel
float ShownAspect();

} // namespace PrimeWidescreen

// Frame boundary (aurora_events.h): window size in pixels.
void PrimeWidescreenOnSurface(uint32_t surfaceWidth, uint32_t surfaceHeight);
