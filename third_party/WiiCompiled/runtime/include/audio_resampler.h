#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

// Rational polyphase resampler for the stereo stream the Wii AI produces.
// The source content is not altered beyond band-limiting at the source
// Nyquist: windowed-sinc (Kaiser) prototype, every phase normalized to unity
// DC gain, float arithmetic, no dither and no other processing. Latency is
// half the filter length in source samples (128 taps -> 2 ms at 32 kHz).
class AudioResampler {
public:
    static constexpr unsigned kTapsPerPhase = 128;
    static constexpr unsigned kMaxPhases = 1024;

    // False when the rate pair needs more phases than kMaxPhases.
    bool Configure(uint32_t inputRate, uint32_t outputRate) {
        m_valid = false;
        if (!inputRate || !outputRate) return false;
        const uint32_t g = std::gcd(inputRate, outputRate);
        m_up = outputRate / g;
        m_down = inputRate / g;
        if (m_up > kMaxPhases) return false;
        m_passthrough = m_up == 1 && m_down == 1;
        BuildFilter(inputRate, outputRate);
        Reset();
        m_valid = true;
        return true;
    }
    bool Valid() const { return m_valid; }
    void Reset() {
        m_history.assign(kTapsPerPhase * 2, 0.0f);
        m_phase = 0;
    }
    // Appends output frames for `frames` interleaved stereo input frames.
    void Process(const float* input, size_t frames, std::vector<float>& output) {
        if (m_passthrough) {
            output.insert(output.end(), input, input + frames * 2);
            return;
        }
        // History holds the newest kTapsPerPhase frames before this block.
        m_work.resize(m_history.size() + frames * 2);
        std::copy(m_history.begin(), m_history.end(), m_work.begin());
        std::copy(input, input + frames * 2, m_work.begin() + m_history.size());
        // `newest` is the index (in frames) of the newest input frame usable for
        // the next output; outputs are produced while it lies inside m_work.
        const size_t totalFrames = m_work.size() / 2;
        while (m_cursor < totalFrames) {
            const float* taps = &m_filter[static_cast<size_t>(m_phase) * kTapsPerPhase];
            const float* x = &m_work[(m_cursor - (kTapsPerPhase - 1)) * 2];
            float left = 0.0f, right = 0.0f;
            // taps[j] weights x[newest - j]; x points at the oldest of the window.
            for (unsigned j = 0; j < kTapsPerPhase; ++j) {
                const float c = taps[kTapsPerPhase - 1 - j];
                left += c * x[j * 2];
                right += c * x[j * 2 + 1];
            }
            output.push_back(left);
            output.push_back(right);
            m_phase += m_down;
            while (m_phase >= m_up) {
                m_phase -= m_up;
                ++m_cursor;
            }
        }
        // Keep the last kTapsPerPhase frames and rebase the cursor onto them.
        std::copy(m_work.end() - m_history.size(), m_work.end(), m_history.begin());
        m_cursor -= totalFrames - kTapsPerPhase;
    }

private:
    static double BesselI0(double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 64; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < sum * 1e-17) break;
        }
        return sum;
    }
    static constexpr double kPi = 3.14159265358979323846;
    void BuildFilter(uint32_t inputRate, uint32_t outputRate) {
        // Cutoff a little below the lower Nyquist keeps images of the 32 kHz
        // source out of the audible band; the AX source is band-limited anyway.
        const double nyquist = 0.5 * std::min(inputRate, outputRate);
        const double cutoff = 0.965 * nyquist;
        const double upRate = static_cast<double>(inputRate) * m_up;
        const size_t length = static_cast<size_t>(kTapsPerPhase) * m_up;
        const double center = (static_cast<double>(length) - 1.0) / 2.0;
        constexpr double beta = 9.0;  // ~90 dB stopband
        const double norm = BesselI0(beta);
        std::vector<double> prototype(length);
        for (size_t n = 0; n < length; ++n) {
            const double t = (static_cast<double>(n) - center) / upRate;
            const double arg = 2.0 * cutoff * t;
            const double sinc = arg == 0.0 ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
            const double r = (static_cast<double>(n) - center) / center;
            const double window = BesselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / norm;
            prototype[n] = sinc * window;
        }
        // Phase p uses prototype[p + j * up]; normalize each phase to unity gain.
        m_filter.assign(length, 0.0f);
        for (uint32_t p = 0; p < m_up; ++p) {
            double sum = 0.0;
            for (unsigned j = 0; j < kTapsPerPhase; ++j) sum += prototype[p + static_cast<size_t>(j) * m_up];
            for (unsigned j = 0; j < kTapsPerPhase; ++j)
                m_filter[static_cast<size_t>(p) * kTapsPerPhase + j] =
                    static_cast<float>(prototype[p + static_cast<size_t>(j) * m_up] / sum);
        }
        m_cursor = kTapsPerPhase - 1;
    }

    uint32_t m_up = 1, m_down = 1, m_phase = 0;
    size_t m_cursor = kTapsPerPhase - 1;
    bool m_valid = false, m_passthrough = false;
    std::vector<float> m_filter, m_history, m_work;
};
