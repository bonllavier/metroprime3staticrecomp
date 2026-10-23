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
    std::cout << "PASS Prime IPC reply deadlines and ordering\n";
}
