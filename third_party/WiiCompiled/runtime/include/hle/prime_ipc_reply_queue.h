#pragma once
#include <cstdint>
#include <deque>
#include <optional>

// Caller supplies monotonic microseconds; no sleeping or guest-state access.
class PrimeIpcReplyQueue {
public:
    static constexpr uint64_t ReadDelayMicros(uint32_t bytes) {
        // Buffered-drive lower bound, not a seek/rotational timing model.
        // Hardware measurements documented in Dolphin DVDInterface.cpp:
        // read startup 600 us, buffered transfer 32 MiB/s.
        return 600u + (uint64_t{bytes} * 1000000u + 33554431u) / 33554432u;
    }
    // A disc read reply also waits for the guest to have run on since the
    // submission (readyProgress, in guest poll boundaries): FMOD's read wrapper
    // submits, then suspends its own thread, and the reply's OSResumeThread lost
    // the race whenever the host preempted the game thread between the two for
    // longer than the wall-clock delay (music looped a buffer until the next
    // track, 2026-10-31). Wall time past readyAt + kProgressFallbackMicros
    // releases it anyway, so a guest that stops polling cannot strand a reply.
    static constexpr uint64_t kProgressFallbackMicros = 20000;
    void Push(uint32_t request, uint64_t readyAt, uint64_t readyProgress = 0) {
        pending_.push_back({request, readyAt, readyProgress});
    }
    std::optional<uint32_t> PopReady(uint64_t now, uint64_t progress = UINT64_MAX) {
        if (pending_.empty()) return std::nullopt;
        const auto& front = pending_.front();
        if (front.readyAt > now) return std::nullopt;
        if (front.readyProgress > progress && now < front.readyAt + kProgressFallbackMicros) return std::nullopt;
        const auto request = front.request;
        pending_.pop_front();
        return request;
    }
private:
    struct Reply { uint32_t request; uint64_t readyAt; uint64_t readyProgress; };
    std::deque<Reply> pending_;
};
