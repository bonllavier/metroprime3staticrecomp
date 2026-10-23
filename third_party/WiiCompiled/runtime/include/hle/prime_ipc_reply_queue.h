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
    void Push(uint32_t request, uint64_t readyAt) { pending_.push_back({request, readyAt}); }
    std::optional<uint32_t> PopReady(uint64_t now) {
        if (pending_.empty() || pending_.front().readyAt > now) return std::nullopt;
        const auto request = pending_.front().request;
        pending_.pop_front();
        return request;
    }
private:
    struct Reply { uint32_t request; uint64_t readyAt; };
    std::deque<Reply> pending_;
};
