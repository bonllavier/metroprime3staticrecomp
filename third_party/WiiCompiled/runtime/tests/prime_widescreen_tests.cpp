// Ultrawide (Hor+) model, GitHub issue #7.
#include "hle/prime_widescreen.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

#define require(ok) do { if (!(ok)) { std::cerr << "FAIL line " << __LINE__ << ": " << #ok << "\n"; std::exit(1); } } while (false)

using namespace PrimeWidescreen;

namespace {
bool Close(double a, double b, double tolerance = 1e-4) { return std::fabs(a - b) <= tolerance; }
}

int main() {
    // Parsing: names, ratios, resolutions; narrower than 16:9 means off; junk refused.
    require(Parse("16:9") && Parse("16:9")->mode == Mode::Off);
    require(Parse(" OFF ") && Parse(" OFF ")->mode == Mode::Off);
    require(Parse("window") && Parse("window")->mode == Mode::Window);
    require(Parse("Auto") && Parse("Auto")->mode == Mode::Window);
    require(Parse("21:9") && Parse("21:9")->mode == Mode::Fixed && Close(Parse("21:9")->aspect, 64.0 / 27.0));
    require(Parse("32:9") && Close(Parse("32:9")->aspect, 32.0 / 9.0));
    require(Parse("3440x1440") && Close(Parse("3440x1440")->aspect, 3440.0 / 1440.0));
    require(Parse("2.39") && Close(Parse("2.39")->aspect, 2.39));
    require(Parse("43:18") && Close(Parse("43:18")->aspect, 43.0 / 18.0));
    require(Parse("4:3") && Parse("4:3")->mode == Mode::Off);
    require(Parse("1920x1080") && Parse("1920x1080")->mode == Mode::Off);
    require(Parse("10:1") && Close(Parse("10:1")->aspect, kMaxAspect));
    require(!Parse("") && !Parse("wide") && !Parse("21:") && !Parse(":9") && !Parse("0:9") && !Parse("21:9x") &&
            !Parse("-2") && !Parse("nan"));

    // Format round-trips through Parse.
    for (const char* text : {"16:9", "window", "21:9", "32:9", "2.390"}) {
        const auto parsed = Parse(text);
        require(parsed && Format(*parsed) == text);
        require(Parse(Format(*parsed)) == parsed);
    }

    // Target shape: off is always 16:9, window follows the window but never
    // narrower than 16:9 nor wider than the cap, fixed ignores the window.
    const Setting off{}, window{Mode::Window, kBaseAspect}, fixed21{Mode::Fixed, kAspect21x9};
    require(TargetAspect(off, 3440, 1440) == kBaseAspect);
    require(Close(TargetAspect(window, 3440, 1440), 3440.0 / 1440.0));
    require(Close(TargetAspect(window, 5120, 1440), 32.0 / 9.0));
    require(TargetAspect(window, 1920, 1080) == kBaseAspect);
    require(TargetAspect(window, 1024, 768) == kBaseAspect);   // 4:3 window: plain 16:9 (pillar/letterbox as today)
    require(TargetAspect(window, 0, 0) == kBaseAspect);
    require(TargetAspect(window, 8000, 1000) == kMaxAspect);
    require(Close(TargetAspect(fixed21, 1920, 1080), 64.0 / 27.0));

    // Factor: how much wider than 16:9.
    require(Factor(kBaseAspect) == 1.0f);
    require(Close(Factor(3440.0f / 1440.0f), (3440.0 / 1440.0) / (16.0 / 9.0)));
    require(Close(Factor(kAspect32x9), 2.0));
    require(Factor(1.0f) == 1.0f);

    // Only the full-screen viewport is widened (shadow maps, monitors, icons keep their shape).
    require(CoversScreen(640, 456, 640, 456));
    require(CoversScreen(639, 456, 640, 456));
    require(!CoversScreen(256, 256, 640, 456));
    require(!CoversScreen(640, 228, 640, 456));
    require(!CoversScreen(640, 456, 0, 0));

    // Widening the game's own aspect: the 16:9 value (1.4685 * 0.9333 * 640/456)
    // times the factor; nothing changes at factor 1 or for bad input.
    const double gameAspect = 1.4685 * 0.93333334 * 640.0 / 456.0;
    require(Close(WidenAspect(gameAspect, 1.0f), gameAspect));
    require(Close(WidenAspect(gameAspect, 1.34375f), gameAspect * 1.34375));
    require(WidenAspect(-1.0, 2.0f) == -1.0);
    require(std::isnan(WidenAspect(std::nan(""), 2.0f)));

    // Hor+ keeps the vertical extent: a symmetric frustum just gets wider.
    const Frustum f{-0.1828f, 0.1828f, 0.1f, -0.1f};
    const Frustum w = WidenFrustum(f, 1.5f);
    require(Close(w.left, -0.2742) && Close(w.right, 0.2742) && w.top == f.top && w.bottom == f.bottom);
    require(Close(FrustumAspect(w), FrustumAspect(f) * 1.5));
    // An off-centre frustum (split screen, jitter) widens about its own centre.
    const Frustum offCentre{0.0f, 0.4f, 0.1f, -0.1f};
    const Frustum wideOff = WidenFrustum(offCentre, 2.0f);
    require(Close(wideOff.left, -0.2) && Close(wideOff.right, 0.6));
    require(WidenFrustum(f, 1.0f).left == f.left);

    std::cout << "PASS Prime widescreen: parsing, target shape, factor, screen test, aspect and frustum widening\n";
    return 0;
}
