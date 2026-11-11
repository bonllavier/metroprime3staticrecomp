#pragma once
// Small antialiased RGBA canvas for the keyboard/gamepad prompt icons on hosts
// without GDI+ (see hle/prime_key_icons.cpp). Shapes are rendered from signed
// distances with one pixel of coverage falloff; text uses stb_truetype with a
// bold system sans font (Noto Sans, DejaVu Sans, Liberation Sans, ...).
// Colours are 0xAARRGGBB, like GDI+ Color(a, r, g, b).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace PrimeIconCanvas {

struct Point { float x, y; };

class Canvas {
public:
    Canvas(int width, int height);
    int Width() const { return width_; }
    int Height() const { return height_; }

    void FillRoundRect(float x, float y, float w, float h, float radius, uint32_t color);
    void StrokeRoundRect(float x, float y, float w, float h, float radius, float thickness, uint32_t color);
    void FillEllipse(float x, float y, float w, float h, uint32_t color);
    void StrokeEllipse(float x, float y, float w, float h, float thickness, uint32_t color);
    void FillRect(float x, float y, float w, float h, uint32_t color);
    void StrokeRect(float x, float y, float w, float h, float thickness, uint32_t color);
    void Line(float x0, float y0, float x1, float y1, float thickness, uint32_t color);
    void FillPolygon(const std::vector<Point>& points, uint32_t color);
    void StrokePolygon(const std::vector<Point>& points, float thickness, uint32_t color);

    // Text centred in the box at `pixelSize` (em size, like GDI+ UnitPixel).
    void Text(const std::string& utf8, float x, float y, float w, float h, float pixelSize, uint32_t color);
    // Width and line height of `utf8` at `pixelSize`; false without a usable font.
    static bool Measure(const std::string& utf8, float pixelSize, float& width, float& height);
    static bool FontAvailable();

    // PNG with rows flipped (Retro TXTR rows are stored bottom-up).
    bool SaveFlippedPng(const std::filesystem::path& path) const;

private:
    template <typename Coverage>
    void Shade(float x0, float y0, float x1, float y1, uint32_t color, Coverage coverage);
    void Blend(int x, int y, uint32_t color, float coverage);

    int width_, height_;
    std::vector<float> rgba_;  // premultiplied, 0..1
};

}  // namespace PrimeIconCanvas
