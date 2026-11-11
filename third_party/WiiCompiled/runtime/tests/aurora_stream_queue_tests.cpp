#include "gx/stream_queue.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#define require(ok) do { if (!(ok)) { std::cerr << "FAIL line " << __LINE__ << ": " << #ok << "\n"; std::exit(1); } } while (false)

using aurora::gx::fifo::StreamChunk;
using aurora::gx::fifo::StreamQueue;

static StreamChunk make_chunk(StreamQueue& queue, const std::vector<uint8_t>& bytes) {
    StreamChunk chunk = queue.take_free(uint32_t(std::max<size_t>(64, bytes.size())));
    std::memcpy(chunk.data, bytes.data(), bytes.size());
    chunk.size = uint32_t(bytes.size());
    return chunk;
}

int main() {
    // Order and completeness: chunks decode in push order, sync waits for all of them.
    {
        StreamQueue queue(1 << 20, 64);
        std::vector<uint8_t> seen;
        std::atomic<int> hooks{0};
        queue.start(nullptr, [&] { ++hooks; },
                    [&](const uint8_t* data, uint32_t size) { seen.insert(seen.end(), data, data + size); });
        for (uint8_t i = 0; i < 50; ++i) queue.push(make_chunk(queue, {i, uint8_t(i + 100)}));
        queue.sync();
        require(queue.idle() && seen.size() == 100 && hooks == 50);
        for (uint8_t i = 0; i < 50; ++i) require(seen[i * 2] == i && seen[i * 2 + 1] == uint8_t(i + 100));
        require(queue.stats().chunks == 50 && queue.stats().bytes == 100);
        queue.stop(true);
    }
    // Buffers are recycled: with an empty pool, the decoded chunk comes back.
    {
        StreamQueue queue(1 << 20, 64);
        queue.start(nullptr, nullptr, [](const uint8_t*, uint32_t) {});
        StreamChunk a = queue.take_free(64);
        a.size = 1;
        queue.push(a);
        queue.sync();
        StreamChunk b = queue.take_free(64);
        require(b.data == a.data && b.size == 0);
        queue.stop(true);
    }
    // Back-pressure: a writer over budget blocks until the decoder drains.
    {
        StreamQueue queue(256, 4);
        std::atomic<bool> release{false};
        std::atomic<int> processed{0};
        queue.start(nullptr, nullptr, [&](const uint8_t*, uint32_t) {
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++processed;
        });
        std::atomic<bool> pushed{false};
        std::thread writer([&] {
            for (int i = 0; i < 6; ++i) queue.push(make_chunk(queue, std::vector<uint8_t>(100)));
            pushed = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        require(!pushed);  // 4 chunks of 100 bytes exceed the 256-byte budget
        release = true;
        writer.join();
        queue.sync();
        require(pushed && processed == 6);
        queue.stop(true);
    }
    // A decoder exception surfaces on the writer, once, at the next push or sync.
    {
        StreamQueue queue(1 << 20, 64);
        queue.start(nullptr, nullptr, [&](const uint8_t* data, uint32_t) {
            if (data[0] == 7) throw std::runtime_error("decoder failure");
        });
        queue.push(make_chunk(queue, {1}));
        queue.push(make_chunk(queue, {7}));
        bool caught = false;
        try { queue.sync(); } catch (const std::runtime_error& error) { caught = std::strcmp(error.what(), "decoder failure") == 0; }
        require(caught);
        queue.sync();  // rethrown only once
        require(!queue.failed());
        queue.stop(true);
    }
    // Idle reports a chunk still being decoded as busy.
    {
        StreamQueue queue(1 << 20, 64);
        std::atomic<bool> release{false};
        queue.start(nullptr, nullptr, [&](const uint8_t*, uint32_t) {
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
        queue.push(make_chunk(queue, {1}));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(!queue.idle() && queue.queued_chunks() == 0);
        release = true;
        queue.sync();
        require(queue.idle());
        queue.stop(true);
    }
    // The backlog (the CP idle bit waits on it) counts a chunk until its decode returns, and a stopped
    // queue never reports one, or the game would poll its breakpoint forever.
    {
        StreamQueue queue(1 << 20, 64);
        std::atomic<bool> release{false};
        queue.start(nullptr, nullptr, [&](const uint8_t*, uint32_t) {
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
        queue.push(make_chunk(queue, {1}));
        queue.push(make_chunk(queue, {2}));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(queue.backlog_hint() == 2);
        release = true;
        queue.sync();
        require(queue.backlog_hint() == 0);
        release = false;
        queue.push(make_chunk(queue, {3}));
        queue.push(make_chunk(queue, {4}));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        queue.stop();
        require(queue.backlog_hint() == 0);
        release = true;
        queue.stop(true);
        require(queue.backlog_hint() == 0);
    }
    std::cout << "PASS Aurora stream queue: order, recycling, back-pressure, failure, idle, backlog\n";
}
