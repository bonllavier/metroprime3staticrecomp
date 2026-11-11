// See include/hle/prime_icon_canvas.h. Non-Windows hosts (Windows draws with GDI+).
#if !defined(_WIN32)
#include "hle/prime_icon_canvas.h"

#include "runtime_log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <mutex>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"

namespace PrimeIconCanvas {
namespace {

float Clamp01(float value) { return std::min(1.0f, std::max(0.0f, value)); }
// One pixel of falloff around the edge of a signed distance (negative inside).
float CoverageFromDistance(float distance) { return Clamp01(0.5f - distance); }

float RoundRectDistance(float px, float py, float x, float y, float w, float h, float r) {
    const float cx = x + w * 0.5f, cy = y + h * 0.5f;
    r = std::min(r, std::min(w, h) * 0.5f);
    const float qx = std::fabs(px - cx) - (w * 0.5f - r), qy = std::fabs(py - cy) - (h * 0.5f - r);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

// Approximate distance to an axis-aligned ellipse (exact for circles).
float EllipseDistance(float px, float py, float x, float y, float w, float h) {
    const float rx = w * 0.5f, ry = h * 0.5f;
    const float dx = (px - (x + rx)) / rx, dy = (py - (y + ry)) / ry;
    const float k = std::sqrt(dx * dx + dy * dy);
    if (k == 0.0f) return -std::min(rx, ry);
    const float gx = dx / (rx * k * k), gy = dy / (ry * k * k);  // gradient of k, scaled
    const float grad = std::sqrt(gx * gx + gy * gy) * k;
    return (k - 1.0f) / std::max(grad, 1e-6f);
}

float SegmentDistance(float px, float py, float x0, float y0, float x1, float y1) {
    const float vx = x1 - x0, vy = y1 - y0, wx = px - x0, wy = py - y0;
    const float len2 = vx * vx + vy * vy;
    const float t = len2 > 0 ? Clamp01((wx * vx + wy * vy) / len2) : 0.0f;
    const float dx = wx - vx * t, dy = wy - vy * t;
    return std::sqrt(dx * dx + dy * dy);
}

bool InsidePolygon(float px, float py, const std::vector<Point>& points) {
    bool inside = false;
    for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
        const Point& a = points[i];
        const Point& b = points[j];
        if ((a.y > py) != (b.y > py) && px < (b.x - a.x) * (py - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

// --- font -------------------------------------------------------------------
struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
};

const Font& LoadFont() {
    static Font font;
    static std::once_flag once;
    std::call_once(once, [] {
        // Bold sans faces present on most distributions; Liberation Sans matches Arial's metrics.
        static const char* kCandidates[] = {
            "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/opentype/noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
        };
        for (const char* candidate : kCandidates) {
            std::ifstream in(candidate, std::ios::binary);
            if (!in) continue;
            font.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (!font.data.empty() &&
                stbtt_InitFont(&font.info, font.data.data(), stbtt_GetFontOffsetForIndex(font.data.data(), 0))) {
                font.ok = true;
                RT_LOG(RT_TAG_RUNTIME) << "[prime-icons] font " << candidate << std::endl;
                return;
            }
        }
        RT_LOG(RT_TAG_RUNTIME) << "[prime-icons] no bold sans TrueType font found (install fonts-noto, "
                                  "fonts-dejavu or fonts-liberation); prompt icons keep the Wii pictures"
                               << std::endl;
    });
    return font;
}

std::vector<uint32_t> Codepoints(const std::string& utf8) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = c;
        size_t extra = 0;
        if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
        if (i + extra >= utf8.size()) break;  // truncated sequence
        for (size_t k = 1; k <= extra; ++k) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

}  // namespace

Canvas::Canvas(int width, int height)
    : width_(std::max(width, 1)), height_(std::max(height, 1)), rgba_(static_cast<size_t>(width_) * height_ * 4, 0.0f) {}

void Canvas::Blend(int x, int y, uint32_t color, float coverage) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_ || coverage <= 0.0f) return;
    const float a = ((color >> 24) & 0xFF) / 255.0f * Clamp01(coverage);
    const float r = ((color >> 16) & 0xFF) / 255.0f, g = ((color >> 8) & 0xFF) / 255.0f, b = (color & 0xFF) / 255.0f;
    float* p = &rgba_[(static_cast<size_t>(y) * width_ + x) * 4];
    p[0] = r * a + p[0] * (1 - a);
    p[1] = g * a + p[1] * (1 - a);
    p[2] = b * a + p[2] * (1 - a);
    p[3] = a + p[3] * (1 - a);
}

template <typename Coverage>
void Canvas::Shade(float x0, float y0, float x1, float y1, uint32_t color, Coverage coverage) {
    const int ix0 = std::max(0, static_cast<int>(std::floor(x0)) - 2), iy0 = std::max(0, static_cast<int>(std::floor(y0)) - 2);
    const int ix1 = std::min(width_ - 1, static_cast<int>(std::ceil(x1)) + 2), iy1 = std::min(height_ - 1, static_cast<int>(std::ceil(y1)) + 2);
    for (int y = iy0; y <= iy1; ++y)
        for (int x = ix0; x <= ix1; ++x) Blend(x, y, color, coverage(x + 0.5f, y + 0.5f));
}

void Canvas::FillRoundRect(float x, float y, float w, float h, float radius, uint32_t color) {
    Shade(x, y, x + w, y + h, color, [&](float px, float py) {
        return CoverageFromDistance(RoundRectDistance(px, py, x, y, w, h, radius));
    });
}

void Canvas::StrokeRoundRect(float x, float y, float w, float h, float radius, float thickness, uint32_t color) {
    const float half = thickness * 0.5f;
    Shade(x - half, y - half, x + w + half, y + h + half, color, [&](float px, float py) {
        return CoverageFromDistance(std::fabs(RoundRectDistance(px, py, x, y, w, h, radius)) - half);
    });
}

void Canvas::FillEllipse(float x, float y, float w, float h, uint32_t color) {
    Shade(x, y, x + w, y + h, color, [&](float px, float py) {
        return CoverageFromDistance(EllipseDistance(px, py, x, y, w, h));
    });
}

void Canvas::StrokeEllipse(float x, float y, float w, float h, float thickness, uint32_t color) {
    const float half = thickness * 0.5f;
    Shade(x - half, y - half, x + w + half, y + h + half, color, [&](float px, float py) {
        return CoverageFromDistance(std::fabs(EllipseDistance(px, py, x, y, w, h)) - half);
    });
}

void Canvas::FillRect(float x, float y, float w, float h, uint32_t color) { FillRoundRect(x, y, w, h, 0.0f, color); }

void Canvas::StrokeRect(float x, float y, float w, float h, float thickness, uint32_t color) {
    StrokeRoundRect(x, y, w, h, 0.0f, thickness, color);
}

void Canvas::Line(float x0, float y0, float x1, float y1, float thickness, uint32_t color) {
    const float half = thickness * 0.5f;
    Shade(std::min(x0, x1) - half, std::min(y0, y1) - half, std::max(x0, x1) + half, std::max(y0, y1) + half, color,
          [&](float px, float py) { return CoverageFromDistance(SegmentDistance(px, py, x0, y0, x1, y1) - half); });
}

void Canvas::FillPolygon(const std::vector<Point>& points, uint32_t color) {
    if (points.size() < 3) return;
    float x0 = points[0].x, y0 = points[0].y, x1 = x0, y1 = y0;
    for (const auto& p : points) { x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y); }
    Shade(x0, y0, x1, y1, color, [&](float px, float py) {
        float edge = 1e9f;
        for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++)
            edge = std::min(edge, SegmentDistance(px, py, points[j].x, points[j].y, points[i].x, points[i].y));
        return CoverageFromDistance(InsidePolygon(px, py, points) ? -edge : edge);
    });
}

void Canvas::StrokePolygon(const std::vector<Point>& points, float thickness, uint32_t color) {
    if (points.size() < 2) return;
    const float half = thickness * 0.5f;
    float x0 = points[0].x, y0 = points[0].y, x1 = x0, y1 = y0;
    for (const auto& p : points) { x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y); }
    // One pass over the union of the edges, so the joints are not blended twice.
    Shade(x0 - half, y0 - half, x1 + half, y1 + half, color, [&](float px, float py) {
        float edge = 1e9f;
        for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++)
            edge = std::min(edge, SegmentDistance(px, py, points[j].x, points[j].y, points[i].x, points[i].y));
        return CoverageFromDistance(edge - half);
    });
}

bool Canvas::FontAvailable() { return LoadFont().ok; }

bool Canvas::Measure(const std::string& utf8, float pixelSize, float& width, float& height) {
    const Font& font = LoadFont();
    if (!font.ok) return false;
    const float scale = stbtt_ScaleForMappingEmToPixels(&font.info, pixelSize);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&font.info, &ascent, &descent, &gap);
    float advance = 0;
    const auto cps = Codepoints(utf8);
    for (size_t i = 0; i < cps.size(); ++i) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font.info, static_cast<int>(cps[i]), &adv, &lsb);
        advance += adv * scale;
        if (i + 1 < cps.size()) advance += stbtt_GetCodepointKernAdvance(&font.info, static_cast<int>(cps[i]), static_cast<int>(cps[i + 1])) * scale;
    }
    width = advance;
    height = (ascent - descent) * scale;
    return true;
}

void Canvas::Text(const std::string& utf8, float x, float y, float w, float h, float pixelSize, uint32_t color) {
    const Font& font = LoadFont();
    float textWidth = 0, lineHeight = 0;
    if (!font.ok || !Measure(utf8, pixelSize, textWidth, lineHeight)) return;
    const float scale = stbtt_ScaleForMappingEmToPixels(&font.info, pixelSize);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&font.info, &ascent, &descent, &gap);
    float penX = x + (w - textWidth) * 0.5f;
    const float baseline = y + (h - lineHeight) * 0.5f + ascent * scale;
    const auto cps = Codepoints(utf8);
    for (size_t i = 0; i < cps.size(); ++i) {
        const int cp = static_cast<int>(cps[i]);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font.info, cp, &adv, &lsb);
        const float shiftX = penX - std::floor(penX);
        int gw = 0, gh = 0, ox = 0, oy = 0;
        unsigned char* bitmap = stbtt_GetCodepointBitmapSubpixel(&font.info, scale, scale, shiftX, 0.0f, cp, &gw, &gh, &ox, &oy);
        if (bitmap) {
            const int originX = static_cast<int>(std::floor(penX)) + ox, originY = static_cast<int>(std::lround(baseline)) + oy;
            for (int row = 0; row < gh; ++row)
                for (int col = 0; col < gw; ++col)
                    Blend(originX + col, originY + row, color, bitmap[row * gw + col] / 255.0f);
            stbtt_FreeBitmap(bitmap, nullptr);
        }
        penX += adv * scale;
        if (i + 1 < cps.size()) penX += stbtt_GetCodepointKernAdvance(&font.info, cp, static_cast<int>(cps[i + 1])) * scale;
    }
}

bool Canvas::SaveFlippedPng(const std::filesystem::path& path) const {
    SDL_Surface* surface = SDL_CreateSurface(width_, height_, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return false;
    for (int y = 0; y < height_; ++y) {
        auto* row = static_cast<uint8_t*>(surface->pixels) + static_cast<size_t>(height_ - 1 - y) * surface->pitch;
        for (int x = 0; x < width_; ++x) {
            const float* p = &rgba_[(static_cast<size_t>(y) * width_ + x) * 4];
            const float a = p[3];
            const auto channel = [&](float premultiplied) {
                return static_cast<uint8_t>(std::lround(Clamp01(a > 0 ? premultiplied / a : 0.0f) * 255.0f));
            };
            row[x * 4 + 0] = channel(p[0]);
            row[x * 4 + 1] = channel(p[1]);
            row[x * 4 + 2] = channel(p[2]);
            row[x * 4 + 3] = static_cast<uint8_t>(std::lround(Clamp01(a) * 255.0f));
        }
    }
    const bool ok = SDL_SavePNG(surface, path.c_str());
    SDL_DestroySurface(surface);
    return ok;
}

}  // namespace PrimeIconCanvas
#endif
