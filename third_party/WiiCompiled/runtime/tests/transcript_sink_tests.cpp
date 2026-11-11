#include "transcript_sink.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {
bool Require(bool value, const char* message) {
    if (!value) std::cerr << message << std::endl;
    return value;
}
using namespace std::chrono_literals;
}  // namespace

int main() {
    {
        // Everything pushed reaches the destination, in order, and Close drains it.
        std::mutex mutex;
        std::string out;
        TranscriptSink sink("test", 1 << 20, [&](const char* d, size_t n) {
            std::lock_guard<std::mutex> l(mutex);
            out.append(d, n);
        });
        for (int i = 0; i < 1000; ++i) {
            const std::string line = "line " + std::to_string(i) + "\n";
            sink.Push(line.data(), line.size());
        }
        if (!Require(sink.Close(5s), "sink did not drain within the timeout")) return 1;
        if (!Require(out.rfind("line 999\n") == out.size() - 9 && out.find("line 0\n") == 0, "chunks lost or reordered")) return 1;
        if (!Require(out.find("dropped") == std::string::npos, "nothing should be dropped below the cap")) return 1;
    }
    {
        // A stalled destination never blocks Push: chunks beyond the cap are dropped and reported once it moves.
        std::atomic<bool> release{false};
        std::mutex mutex;
        std::string out;
        TranscriptSink sink("console", 64, [&](const char* d, size_t n) {
            while (!release.load()) std::this_thread::sleep_for(1ms);
            std::lock_guard<std::mutex> l(mutex);
            out.append(d, n);
        });
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 10000; ++i) sink.Push("0123456789", 10);  // 100 KB against a 64-byte cap
        const auto pushMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (!Require(pushMs < 500, "Push blocked on a stalled destination")) return 1;
        release = true;
        if (!Require(sink.Close(5s), "sink did not finish after the destination resumed")) return 1;
        std::lock_guard<std::mutex> l(mutex);
        if (!Require(out.find("transcript: dropped") != std::string::npos, "dropped bytes were not reported")) return 1;
        if (!Require(out.size() < 2000, "the cap was not enforced")) return 1;
    }
    {
        // A destination stuck forever: Close gives up after the timeout and the process is not held.
        std::atomic<bool> release{false};
        auto* sink = new TranscriptSink("stuck", 1 << 20, [&](const char*, size_t) {
            while (!release.load()) std::this_thread::sleep_for(1ms);
        });
        sink->Push("x", 1);
        const auto start = std::chrono::steady_clock::now();
        const bool drained = sink->Close(200ms);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (!Require(!drained && ms < 2000, "Close did not give up on a stuck destination")) return 1;
        release = true;
        std::this_thread::sleep_for(100ms);  // the detached writer leaves the lambda before the sink goes away
        delete sink;
    }
    std::cout << "PASS transcript sink: ordered drain, non-blocking push with drop report, bounded close" << std::endl;
    return 0;
}
