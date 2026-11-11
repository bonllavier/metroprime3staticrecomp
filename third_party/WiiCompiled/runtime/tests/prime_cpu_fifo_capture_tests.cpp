#include "hle/prime_cpu_fifo_capture.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        PrimeCpuFifo::Capture capture;
        std::array<uint8_t, 160> ram;
        ram.fill(0xEE);
        std::vector<uint8_t> reference;
        unsigned bursts = 0;
        auto write = [&](uint32_t address, const uint8_t* data, size_t size) {
            Require(address >= 32 && address + size <= 128 && size == 32, "burst range");
            std::copy(data, data + size, ram.begin() + address);
            bursts++;
        };
        uint32_t pointer = 32;
        auto push = [&](uint32_t value, unsigned width) {
            for (unsigned i = width; i; --i) reference.push_back(uint8_t(value >> ((i - 1) * 8)));
            pointer = capture.Push(32, 96, pointer, value, width, write);
        };
        // Mixed widths, including a 32-bit store across the burst boundary.
        for (unsigned i = 0; i < 31; i++) push(i, 1);
        Require(bursts == 0 && pointer == 32 && capture.count == 31, "partial burst published");
        push(0xAABBCCDD, 4);
        Require(bursts == 1 && pointer == 64 && capture.count == 3, "cross-boundary store");
        push(0x1122, 2);
        while (reference.size() < 96) push(uint32_t(reference.size()), 1);
        Require(bursts == 3 && capture.count == 0 && pointer == (32 | 0x20000000u), "wrap flag");
        Require(std::equal(reference.begin(), reference.end(), ram.begin() + 32), "byte stream differs");
        for (unsigned i = 0; i < 32; i++) push(0xFF, 1);
        Require(pointer == (64 | 0x20000000u), "wrap flag not retained");
        Require(std::all_of(ram.begin() + 32, ram.begin() + 64, [](auto v){return v == 0xFF;}), "wrapped write");
        Require(std::all_of(ram.begin(), ram.begin() + 32, [](auto v){return v == 0xEE;}) &&
                std::all_of(ram.begin() + 128, ram.end(), [](auto v){return v == 0xEE;}), "outside bytes changed");
        for (const auto width : {0u, 3u, 8u}) {
            bool rejected = false;
            try { capture.Push(32, 96, 64, 0, width, write); }
            catch (const std::logic_error&) { rejected = true; }
            Require(rejected && capture.count == 0, "invalid store accepted");
        }
        bool rejected = false;
        try { capture.Push(32, 96, 65, 0, 1, write); }
        catch (const std::logic_error&) { rejected = true; }
        Require(rejected && capture.count == 0, "unaligned pointer accepted");
        // PI reconfiguration preserves a partially filled gather block. SDK
        // display-list restoration can leave NOP padding pending in that block.
        pointer = capture.Push(32, 96, 32, 0xDEADBEEF, 4, write);
        pointer = 64;
        for (unsigned i = 0; i < 28; i++)
            pointer = capture.Push(32, 96, pointer, 0, 1, write);
        Require(capture.count == 0 && pointer == 96, "partial reconfiguration count");
        Require(ram[64] == 0xDE && ram[65] == 0xAD && ram[66] == 0xBE && ram[67] == 0xEF &&
                std::all_of(ram.begin() + 68, ram.begin() + 96, [](auto v){return v == 0;}),
                "partial block lost across PI pointer change");
        // MEM2 above 0x14000000 is real RAM; bit 26 is not the wrap bit.
        unsigned highWrites = 0;
        pointer = 0x14000000u;
        for (unsigned i = 0; i < 8; i++)
            pointer = capture.Push(0x14000000u, 0x14000020u, pointer, 0x11223344, 4,
                [&](uint32_t address, const uint8_t*, size_t) {
                    Require(address == 0x14000000u, "upper MEM2 address lost"); highWrites++;
                });
        Require(highWrites == 1 && pointer == 0x14000020u, "upper MEM2 burst");
        // A store run copied with AppendWithinBurst, completing each burst with a one-byte Push
        // (PrimeGpuGatherWriteBulk), publishes exactly what pushing the run word by word does.
        {
            std::vector<uint8_t> stream(300);
            for (size_t i = 0; i < stream.size(); ++i) stream[i] = uint8_t(i * 37u + 11u);
            const size_t runs[] = {1, 3, 30, 31, 32, 33, 2, 64, 5, 99};
            struct Sink {
                std::array<uint8_t, 160> ram{};
                unsigned bursts = 0;
            };
            auto run = [&](bool bulk, Sink& sink, PrimeCpuFifo::Capture& fifo) {
                uint32_t at = 32;
                auto commit = [&](uint32_t address, const uint8_t* data, size_t size) {
                    std::copy(data, data + size, sink.ram.begin() + address);
                    sink.bursts++;
                };
                size_t offset = 0;
                for (const size_t length : runs) {
                    const size_t end = offset + length;
                    if (bulk) {
                        while (offset < end) {
                            offset += fifo.AppendWithinBurst(stream.data() + offset, end - offset);
                            if (offset == end) break;
                            at = fifo.Push(32, 96, at, stream[offset], 1, commit);
                            ++offset;
                        }
                    } else {
                        for (; offset + 4 <= end; offset += 4) {
                            const uint32_t word = uint32_t(stream[offset]) << 24 | uint32_t(stream[offset + 1]) << 16 |
                                                  uint32_t(stream[offset + 2]) << 8 | stream[offset + 3];
                            at = fifo.Push(32, 96, at, word, 4, commit);
                        }
                        for (; offset < end; ++offset) at = fifo.Push(32, 96, at, stream[offset], 1, commit);
                    }
                }
                return at;
            };
            Sink words, blocks;
            PrimeCpuFifo::Capture wordFifo, blockFifo;
            const uint32_t wordPointer = run(false, words, wordFifo);
            const uint32_t blockPointer = run(true, blocks, blockFifo);
            Require(wordPointer == blockPointer && words.bursts == blocks.bursts && words.bursts == 9,
                    "bulk append pointer or burst count differs");
            Require(wordFifo.count == blockFifo.count &&
                        std::equal(wordFifo.pending.begin(), wordFifo.pending.begin() + wordFifo.count,
                                   blockFifo.pending.begin()),
                    "bulk append pending bytes differ");
            Require(words.ram == blocks.ram, "bulk append published bytes differ");
        }
        std::cout << "PASS CPU FIFO: byte order, partial bursts, mixed widths, wrap, bounds, invalid inputs, bulk runs\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
