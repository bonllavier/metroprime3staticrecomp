#include "hle/prime_ipc_reply_queue.h"
#include <cstdlib>
#include <iostream>
void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    PrimeIpcReplyQueue replies;
    Check(!replies.PopReady(100), "empty queue");
    const auto delay = PrimeIpcReplyQueue::ReadDelayMicros(65536);
    Check(delay == 2554, "64 KiB buffered read lower bound");
    Check(PrimeIpcReplyQueue::ReadDelayMicros(0) == 600, "read command startup");
    replies.Push(0x1000, 100 + delay);
    replies.Push(0x2000, 100); // Later requests must not overwrite an in-flight reply.
    Check(!replies.PopReady(100), "no completion on submission stack");
    Check(!replies.PopReady(99 + delay), "deadline not reached");
    Check(replies.PopReady(100 + delay) == 0x1000, "read completion at deadline");
    Check(replies.PopReady(100 + delay) == 0x2000, "preserve acceptance order");
    Check(!replies.PopReady(100 + delay), "each callback delivered once");
    Check(PrimeIpcReplyQueue::ReadDelayMicros(0xffffffffu) > 128000000u,
          "large transfer must not overflow");
    // Guest progress gate: due by wall time, held until the guest ran on.
    PrimeIpcReplyQueue gated;
    gated.Push(0x3000, 100, 500);
    Check(!gated.PopReady(100, 499), "held until the guest progressed");
    Check(gated.PopReady(100, 500) == 0x3000, "released once the guest progressed");
    gated.Push(0x4000, 100, 500);
    gated.Push(0x5000, 100);
    Check(!gated.PopReady(100 + PrimeIpcReplyQueue::kProgressFallbackMicros - 1, 0), "fallback not reached");
    Check(gated.PopReady(100 + PrimeIpcReplyQueue::kProgressFallbackMicros, 0) == 0x4000,
          "wall-clock fallback releases a stalled guest");
    Check(gated.PopReady(100 + PrimeIpcReplyQueue::kProgressFallbackMicros, 0) == 0x5000, "order kept after the gate");
    std::cout << "PASS Prime IPC reply deadlines, ordering and guest progress gate\n";
}
