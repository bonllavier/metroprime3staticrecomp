#include "hle/prime_input_latency.h"

#include "runtime_log.h"

#include <aurora/aurora.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>

namespace PrimeInputLatency {
namespace {
struct Stage {
    uint64_t count = 0;
    int64_t sum = 0, worst = 0;
    void Add(int64_t nanos) {
        if (nanos < 0) return;
        ++count;
        sum += nanos;
        worst = std::max(worst, nanos);
    }
    double AverageMs() const { return count ? double(sum) / double(count) / 1e6 : 0.0; }
    double WorstMs() const { return double(worst) / 1e6; }
};

// Guest-thread ring of KPAD read times, read by the consumer.
std::array<std::atomic<int64_t>, 16> g_inputs{};
std::atomic<uint32_t> g_inputIndex{0};
thread_local int64_t t_decodingSubmit = 0;

// Latest decoded copy (consumer -> guest thread).
std::atomic<int64_t> g_copyNanos{0}, g_copyInputNanos{0};
std::atomic<uint32_t> g_copySequence{0};
std::atomic<int64_t> g_inputToCopySum{0}, g_inputToCopyWorst{0};
std::atomic<uint64_t> g_inputToCopyCount{0};

// Guest-thread state.
uint32_t g_handedSequence = 0;
int64_t g_handoffNanos = 0, g_handoffInputNanos = 0, g_handoffCopyNanos = 0;
bool g_awaitingPresent = false;
Stage g_copyToHandoff, g_handoffToPresent, g_inputToPresent;
int64_t g_nextReport = 0;

int64_t LatestInputBefore(int64_t nanos) {
    int64_t best = 0;
    for (const auto& slot : g_inputs) {
        const int64_t value = slot.load(std::memory_order_acquire);
        if (value <= nanos && value > best) best = value;
    }
    return best;
}

void Poll() {
    const int64_t now = NowNanos();
    if (g_awaitingPresent) {
        const int64_t presented = aurora_last_present_steady_nanos();
        if (presented >= g_handoffNanos) {
            g_handoffToPresent.Add(presented - g_handoffNanos);
            if (g_handoffInputNanos) g_inputToPresent.Add(presented - g_handoffInputNanos);
            g_awaitingPresent = false;
        }
    }
    if (now < g_nextReport) return;
    if (g_nextReport != 0) {
        const uint64_t copies = g_inputToCopyCount.exchange(0);
        const int64_t copySum = g_inputToCopySum.exchange(0), copyWorst = g_inputToCopyWorst.exchange(0);
        RT_LOG(RT_TAG_RUNTIME) << "[prime-input-latency] frames=" << g_inputToPresent.count
            << " input_to_copy_avg_ms=" << (copies ? double(copySum) / copies / 1e6 : 0.0)
            << " input_to_copy_worst_ms=" << double(copyWorst) / 1e6
            << " copy_to_vi_avg_ms=" << g_copyToHandoff.AverageMs()
            << " vi_to_present_avg_ms=" << g_handoffToPresent.AverageMs()
            << " input_to_present_avg_ms=" << g_inputToPresent.AverageMs()
            << " input_to_present_worst_ms=" << g_inputToPresent.WorstMs() << std::endl;
    }
    g_copyToHandoff = {};
    g_handoffToPresent = {};
    g_inputToPresent = {};
    g_nextReport = now + 10'000'000'000;
}
}  // namespace

int64_t NowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void NoteInputRead() {
#if !defined(MKW_PRIME_QA_TOOLS)
    return;
#endif
    const uint32_t index = g_inputIndex.fetch_add(1, std::memory_order_relaxed) % g_inputs.size();
    g_inputs[index].store(NowNanos(), std::memory_order_release);
    Poll();
}

void SetDecodingSubmitTime(int64_t nanos) { t_decodingSubmit = nanos; }
int64_t DecodingSubmitTime() { return t_decodingSubmit; }

void NoteXfbCopy() {
#if !defined(MKW_PRIME_QA_TOOLS)
    return;
#endif
    const int64_t now = NowNanos();
    const int64_t submitted = t_decodingSubmit ? t_decodingSubmit : now;
    const int64_t input = LatestInputBefore(submitted);
    g_copyNanos.store(now, std::memory_order_relaxed);
    g_copyInputNanos.store(input, std::memory_order_relaxed);
    g_copySequence.fetch_add(1, std::memory_order_release);
    if (input) {
        const int64_t latency = now - input;
        g_inputToCopySum.fetch_add(latency, std::memory_order_relaxed);
        g_inputToCopyCount.fetch_add(1, std::memory_order_relaxed);
        int64_t worst = g_inputToCopyWorst.load(std::memory_order_relaxed);
        while (latency > worst && !g_inputToCopyWorst.compare_exchange_weak(worst, latency)) {}
    }
}

void NoteHandoff() {
#if !defined(MKW_PRIME_QA_TOOLS)
    return;
#endif
    const uint32_t sequence = g_copySequence.load(std::memory_order_acquire);
    if (sequence == g_handedSequence) return;
    g_handedSequence = sequence;
    g_handoffNanos = NowNanos();
    g_handoffCopyNanos = g_copyNanos.load(std::memory_order_relaxed);
    g_handoffInputNanos = g_copyInputNanos.load(std::memory_order_relaxed);
    g_copyToHandoff.Add(g_handoffNanos - g_handoffCopyNanos);
    g_awaitingPresent = true;
}
}  // namespace PrimeInputLatency
