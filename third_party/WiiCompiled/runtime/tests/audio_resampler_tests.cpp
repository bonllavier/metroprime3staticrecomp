#include "audio_resampler.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

constexpr double kPi = 3.14159265358979323846;

// Feeds a sine in uneven blocks (as AI DMA delivers 8-frame bursts) and
// returns the least-squares amplitude and the residual RMS relative to it.
void MeasureSine(uint32_t inRate, uint32_t outRate, double frequency, double& gain, double& residualDb) {
    AudioResampler resampler;
    Check(resampler.Configure(inRate, outRate), "configure");
    const size_t inFrames = inRate;  // one second
    std::vector<float> input(inFrames * 2);
    for (size_t i = 0; i < inFrames; ++i) {
        const float v = static_cast<float>(0.5 * std::sin(2.0 * kPi * frequency * i / inRate));
        input[i * 2] = v;
        input[i * 2 + 1] = -v;
    }
    std::vector<float> output;
    for (size_t offset = 0; offset < inFrames;) {
        const size_t block = std::min<size_t>(inFrames - offset, offset % 3 == 0 ? 8 : 13);
        resampler.Process(input.data() + offset * 2, block, output);
        offset += block;
    }
    const size_t outFrames = output.size() / 2;
    Check(outFrames + 2 >= static_cast<size_t>(outRate) && outFrames <= static_cast<size_t>(outRate) + 2,
          "output frame count follows the rate ratio");
    // Skip the filter delay and the warm-up edge, then fit a*sin + b*cos.
    const size_t first = outRate / 10, last = outFrames - outRate / 10;
    double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
    for (size_t n = first; n < last; ++n) {
        const double w = 2.0 * kPi * frequency * n / outRate;
        const double s = std::sin(w), c = std::cos(w), y = output[n * 2];
        ss += s * s; sc += s * c; cc += c * c; ys += y * s; yc += y * c;
        Check(std::fabs(output[n * 2] + output[n * 2 + 1]) < 1e-6, "channels stay independent");
    }
    const double det = ss * cc - sc * sc;
    const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    gain = std::sqrt(a * a + b * b) / 0.5;
    double err = 0;
    for (size_t n = first; n < last; ++n) {
        const double w = 2.0 * kPi * frequency * n / outRate;
        const double e = output[n * 2] - (a * std::sin(w) + b * std::cos(w));
        err += e * e;
    }
    const double rms = std::sqrt(err / (last - first));
    residualDb = 20.0 * std::log10(rms / (0.5 / std::sqrt(2.0)));
}
}  // namespace

int main() {
    double gain = 0, residual = 0;
    MeasureSine(32000, 44100, 1000.0, gain, residual);
    std::printf("32k->44.1k 1 kHz gain=%.6f residual=%.1f dB\n", gain, residual);
    Check(std::fabs(20.0 * std::log10(gain)) < 0.01, "1 kHz passband gain within 0.01 dB");
    Check(residual < -80.0, "1 kHz residual below -80 dB");

    MeasureSine(32000, 44100, 12000.0, gain, residual);
    std::printf("32k->44.1k 12 kHz gain=%.6f residual=%.1f dB\n", gain, residual);
    Check(std::fabs(20.0 * std::log10(gain)) < 0.1, "12 kHz passband gain within 0.1 dB");
    Check(residual < -70.0, "12 kHz residual below -70 dB");

    MeasureSine(32000, 48000, 1000.0, gain, residual);
    std::printf("32k->48k 1 kHz gain=%.6f residual=%.1f dB\n", gain, residual);
    Check(std::fabs(20.0 * std::log10(gain)) < 0.01, "48 kHz passband gain");
    Check(residual < -80.0, "48 kHz residual");

    AudioResampler same;
    Check(same.Configure(32000, 32000), "passthrough configure");
    const float block[4] = {0.25f, -0.5f, 0.75f, -1.0f};
    std::vector<float> out;
    same.Process(block, 2, out);
    Check(out.size() == 4 && out[0] == 0.25f && out[3] == -1.0f, "equal rates pass samples through unchanged");
    std::printf("PASS\n");
    return 0;
}
