#include "memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <optional>
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "system_bridge.h"
#include "hle/prime_ai_registers.h"
#include "hle/prime_trace_budget.h"
#include "hle/prime_ipc_reply_queue.h"
#include "hle/prime_cp_fifo.h"
#include "hle/prime_cpu_fifo_capture.h"
#include "hle/prime_dsp_boot_registers.h"
#include "hle_stubs.h"
#include "hle/audio/ax_dsp.h"
#include <mutex>
#include <sstream>
#include <unordered_map>

extern "C" int32_t NAND_IOS_Open_HLE(uint32_t pathPtr, uint32_t mode);
extern "C" bool DVD_HLE_ReadPrimePartition(uint32_t buffer, uint32_t length, uint64_t byteOffset);
extern "C" void GX_HLE_PrimeResetFifo();
extern "C" bool GX_HLE_PrimeFifoAtBoundary();
extern "C" void GX_HLE_PrimeResumeFifo();
extern "C" void GX_HLE_PrimeConsumeGatherBurst(const uint8_t* bytes);
extern "C" int32_t NAND_IOS_Close_HLE(uint32_t fd);
extern "C" int32_t NAND_IOS_Read_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length);
extern "C" int32_t NAND_IOS_Write_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length);
extern "C" int32_t NAND_IOS_Seek_HLE(uint32_t fd, int32_t offset, int32_t whence);
extern "C" int32_t NAND_IOS_Ioctl_HLE(uint32_t fd, uint32_t cmd,
                                        uint32_t inBufPtr, uint32_t inLen,
                                        uint32_t outBufPtr, uint32_t outLen);
extern "C" int32_t NAND_IOS_Ioctlv_HLE(uint32_t fd, uint32_t cmd,
                                         uint32_t numIn, uint32_t numOut,
                                         uint32_t vectorPtr);
extern "C" bool NAND_IOS_StartOh1InterruptRead_HLE(uint32_t fd, uint32_t cmd,
                                                    uint32_t numIn, uint32_t numOut,
                                                    uint32_t vectorPtr, uint32_t callback,
                                                    uint32_t callbackArg, uint32_t ipcRequest);

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

MemoryInline::PageEntry MemoryInline::g_pageTable[MemoryInline::kPageCount]{};
uintptr_t MemoryInline::g_fullPageBias[MemoryInline::kPageCount]{};
uintptr_t MemoryInline::g_fullReadablePageBias[MemoryInline::kPageCount]{};
uintptr_t MemoryInline::g_fullWritablePageBias[MemoryInline::kPageCount]{};
const MemoryInline::SparseWritablePageTable*
    MemoryInline::g_sparseWritablePageTables[MemoryInline::kPageCount]{};
uint8_t MemoryInline::g_deferredReadCoveredPages[MemoryInline::kPageCount]{};
template <typename T>
T MemoryInline::ReadResolvedFallback(uint32_t addr) {
    if constexpr (sizeof(T) == 1) return Memory::Read8(addr);
    if constexpr (sizeof(T) == 2) return Memory::Read16(addr);
    if constexpr (sizeof(T) == 4) return Memory::Read32(addr);
    return Memory::Read64(addr);
}
template uint8_t MemoryInline::ReadResolvedFallback<uint8_t>(uint32_t);
template uint16_t MemoryInline::ReadResolvedFallback<uint16_t>(uint32_t);
template uint32_t MemoryInline::ReadResolvedFallback<uint32_t>(uint32_t);
template uint64_t MemoryInline::ReadResolvedFallback<uint64_t>(uint32_t);
float MemoryInline::ReadResolvedFallbackFloat32(uint32_t addr) { return Memory::ReadFloat32(addr); }
double MemoryInline::ReadResolvedFallbackFloat64(uint32_t addr) { return Memory::ReadFloat64(addr); }
template <typename T>
void MemoryInline::WriteResolvedFallback(uint32_t addr, T value) {
    if constexpr (sizeof(T) == 1) Memory::Write8(addr, value);
    else if constexpr (sizeof(T) == 2) Memory::Write16(addr, value);
    else if constexpr (sizeof(T) == 4) Memory::Write32(addr, value);
    else Memory::Write64(addr, value);
}
template void MemoryInline::WriteResolvedFallback<uint8_t>(uint32_t, uint8_t);
template void MemoryInline::WriteResolvedFallback<uint16_t>(uint32_t, uint16_t);
template void MemoryInline::WriteResolvedFallback<uint32_t>(uint32_t, uint32_t);
template void MemoryInline::WriteResolvedFallback<uint64_t>(uint32_t, uint64_t);
void MemoryInline::WriteResolvedFallbackFloat32(uint32_t addr, double val) { Memory::WriteFloat32(addr, val); }
void MemoryInline::WriteResolvedFallbackFloat64(uint32_t addr, double val) { Memory::WriteFloat64(addr, val); }
namespace {
struct DeferredRead {
    uint64_t token = 0;
    uint32_t start = 0;
    size_t length = 0;
    Memory::DeferredReadCallback callback = nullptr;
    void* user = nullptr;
};

std::mutex& DeferredReadMutex() {
    static std::mutex mutex;
    return mutex;
}

#if defined(MKW_GAME_PROFILE_RM3E01_00)
// Broadway PI interrupt mask register (0xCC003004). Its upper/lower halfwords
// set/clear mask bits; keeping this device state separate from RAM lets Prime's
// translated OS interrupt initialization run without exposing MMIO as memory.
std::atomic<uint32_t>& PrimePiInterruptMask() {
    static std::atomic<uint32_t> mask{0};
    return mask;
}

std::atomic<uint32_t>& PrimePiStatusRegister() {
    static std::atomic<uint32_t> value{0};
    return value;
}

std::atomic<uint32_t>& PrimePiFifoRegister(uint32_t addr) {
    static std::array<std::atomic<uint32_t>, 3> values{};
    return values[(addr - 0xCC00300Cu) / 4u];
}

std::atomic<uint32_t>& PrimeMiProtectedRegion(uint32_t addr) {
    static std::array<std::atomic<uint32_t>, 4> regions{};
    return regions[(addr - 0xCC004000u) / 4u];
}

std::atomic<uint32_t>& PrimeSystemInterruptEnable() {
    static std::atomic<uint32_t> value{0};
    return value;
}

std::atomic<uint32_t>& PrimeGpioOutput() {
    static std::atomic<uint32_t> value{0};
    return value;
}

std::atomic<uint32_t>& PrimeGpioDirection() {
    static std::atomic<uint32_t> value{0};
    return value;
}

std::atomic<uint32_t>& PrimeIpcPpcControl() {
    static std::atomic<uint32_t> value{0};
    return value;
}

std::atomic<uint32_t>& PrimeIpcPpcMessage() {
    static std::atomic<uint32_t> value{0};
    return value;
}

std::atomic<uint32_t>& PrimeIpcArmMessage() {
    static std::atomic<uint32_t> value{0};
    return value;
}

std::atomic<uint32_t>& PrimeIpcPpcInterruptFlags() {
    static std::atomic<uint32_t> value{0};
    return value;
}

std::mutex g_primeReplyMutex;
PrimeIpcReplyQueue g_primeReplies;
std::atomic<uint32_t> g_primeReplyCount{0};

uint64_t PrimeIpcNowMicros() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void QueuePrimeIpcReply(uint32_t request, uint32_t command, uint64_t delayMicros = 0) {
    // Reply ABI shared by immediate and deferred IOS operations.
    Memory::Write32(request + 8u, command);
    Memory::Write32(request, 8u);
    std::lock_guard<std::mutex> lock(g_primeReplyMutex);
    g_primeReplies.Push(request, PrimeIpcNowMicros() + delayMicros);
    g_primeReplyCount.fetch_add(1, std::memory_order_release);
}

void PublishNextPrimeIpcReply() {
    // Called at translated call boundaries. An empty queue needs neither a
    // clock query nor a mutex; enqueue publishes the count after its payload.
    if (g_primeReplyCount.load(std::memory_order_acquire) == 0) return;
    std::lock_guard<std::mutex> lock(g_primeReplyMutex);
    if ((PrimeIpcPpcControl().load() & 4u) != 0) return;
    const auto request = g_primeReplies.PopReady(PrimeIpcNowMicros());
    if (!request) return;
    g_primeReplyCount.fetch_sub(1, std::memory_order_release);
    PrimeIpcArmMessage().store(*request, std::memory_order_release);
    const uint32_t control = PrimeIpcPpcControl().fetch_or(4u, std::memory_order_acq_rel) | 4u;
    if ((control & 0x10u) != 0) PrimeIpcPpcInterruptFlags().fetch_or(0x40000000u);
}

std::atomic<uint32_t>& PrimeIpcNextFileDescriptor() {
    static std::atomic<uint32_t> value{0};
    return value;
}

std::atomic<uint32_t>& PrimeIpcPendingEventHook() {
    static std::atomic<uint32_t> value{0};
    return value;
}

std::array<std::atomic<uint32_t>, 3>& PrimeStmVideoDimmingRegisters() {
    static std::array<std::atomic<uint32_t>, 3> registers{};
    return registers;
}

struct PrimeIpcDescriptorTable {
    std::mutex mutex;
    std::unordered_map<uint32_t, int32_t> backendFdByGuestFd;
    int32_t eventHookGuestFd = -1;
    int32_t immediateStmGuestFd = -1;
    int32_t diGuestFd = -1;
};

PrimeIpcDescriptorTable& PrimeIpcDescriptors() {
    static PrimeIpcDescriptorTable table;
    return table;
}

int32_t ResolvePrimeIpcBackendFd(uint32_t guestFd) {
    auto& table = PrimeIpcDescriptors();
    std::lock_guard<std::mutex> lock(table.mutex);
    const auto it = table.backendFdByGuestFd.find(guestFd);
    return it == table.backendFdByGuestFd.end() ? -1 : it->second;
}

uint32_t AllocatePrimeIpcBackendFd(int32_t backendFd) {
    const uint32_t guestFd = PrimeIpcNextFileDescriptor().fetch_add(1, std::memory_order_relaxed);
    auto& table = PrimeIpcDescriptors();
    std::lock_guard<std::mutex> lock(table.mutex);
    table.backendFdByGuestFd.emplace(guestFd, backendFd);
    return guestFd;
}

void ClosePrimeIpcDescriptor(uint32_t guestFd) {
    auto& table = PrimeIpcDescriptors();
    std::lock_guard<std::mutex> lock(table.mutex);
    table.backendFdByGuestFd.erase(guestFd);
    if (table.eventHookGuestFd == static_cast<int32_t>(guestFd)) table.eventHookGuestFd = -1;
    if (table.immediateStmGuestFd == static_cast<int32_t>(guestFd)) table.immediateStmGuestFd = -1;
    if (table.diGuestFd == static_cast<int32_t>(guestFd)) table.diGuestFd = -1;
}

bool IsPrimeIpcEventHookDescriptor(uint32_t guestFd) {
    auto& table = PrimeIpcDescriptors();
    std::lock_guard<std::mutex> lock(table.mutex);
    return table.eventHookGuestFd == static_cast<int32_t>(guestFd);
}

void SetPrimeIpcEventHookDescriptor(uint32_t guestFd) {
    auto& table = PrimeIpcDescriptors();
    std::lock_guard<std::mutex> lock(table.mutex);
    table.eventHookGuestFd = static_cast<int32_t>(guestFd);
}

bool IsPrimeIpcImmediateDescriptor(uint32_t guestFd) {
    auto& table = PrimeIpcDescriptors();
    std::lock_guard<std::mutex> lock(table.mutex);
    return table.immediateStmGuestFd == static_cast<int32_t>(guestFd);
}

void SetPrimeIpcImmediateDescriptor(uint32_t guestFd) {
    auto& table = PrimeIpcDescriptors();
    std::lock_guard<std::mutex> lock(table.mutex);
    table.immediateStmGuestFd = static_cast<int32_t>(guestFd);
}

bool IsPrimeIpcDiDescriptor(uint32_t guestFd) {
    auto& table = PrimeIpcDescriptors();
    std::lock_guard<std::mutex> lock(table.mutex);
    return table.diGuestFd == static_cast<int32_t>(guestFd);
}

void SetPrimeIpcDiDescriptor(uint32_t guestFd) {
    auto& table = PrimeIpcDescriptors();
    std::lock_guard<std::mutex> lock(table.mutex);
    table.diGuestFd = static_cast<int32_t>(guestFd);
}

bool SubmitPrimeIpcRequest(uint32_t request, bool& replyReady) {
    const uint32_t command = Memory::Read32(request);
    static std::atomic<uint32_t> requestTraceCount{0};
    if (requestTraceCount.fetch_add(1, std::memory_order_relaxed) < 64u) {
        RT_LOG(RT_TAG_OS) << "IOS request packet=0x" << std::hex << request
                          << " command=" << std::dec << command
                          << " fd=0x" << std::hex << Memory::Read32(request + 8u)
                          << " arg0=0x" << Memory::Read32(request + 12u)
                          << " arg1=0x" << Memory::Read32(request + 16u)
                          << " arg2=0x" << Memory::Read32(request + 20u)
                          << " arg3=0x" << Memory::Read32(request + 24u)
                          << " callback=0x" << Memory::Read32(request + 32u)
                          << " callback_arg=0x" << Memory::Read32(request + 36u)
                          << std::dec << std::endl;
    }
    replyReady = true;
    if (command == 6u) { // IOS_IOCTL
        const uint32_t fd = Memory::Read32(request + 8u);
        const uint32_t ioctl = Memory::Read32(request + 12u);
        if (IsPrimeIpcEventHookDescriptor(fd) && ioctl == 0x1000u) {
            // STM_EVENTHOOK remains pending until a physical power/reset event.
            PrimeIpcPendingEventHook().store(request, std::memory_order_release);
            replyReady = false;
            RT_LOG(RT_TAG_OS) << "IOS IPC registered pending STM event hook request=0x"
                              << std::hex << request << std::dec << std::endl;
            return true;
        }
        if (IsPrimeIpcImmediateDescriptor(fd)) {
            // Wii IOS/Dolphin treat STM_LEDMode (0x6002) as a successful
            // device operation; the desktop has no optical-drive slot LED.
            if (ioctl == 0x5001u) {
                const uint32_t input = Memory::Read32(request + 16u);
                const uint32_t inputLength = Memory::Read32(request + 20u);
                const uint32_t output = Memory::Read32(request + 24u);
                const uint32_t outputLength = Memory::Read32(request + 28u);
                if (inputLength < 7u * sizeof(uint32_t) ||
                    outputLength < 8u * sizeof(uint32_t) ||
                    !Memory::Contains(input, 7u * sizeof(uint32_t)) ||
                    !Memory::Contains(output, 8u * sizeof(uint32_t))) {
                    Memory::Write32(request + 4u, static_cast<uint32_t>(-4));
                    RT_LOG(RT_TAG_OS) << "Invalid /dev/stm/immediate VIDimming buffers" << std::endl;
                    return true;
                }

                auto& registers = PrimeStmVideoDimmingRegisters();
                uint32_t returnFlags = 0;
                for (uint32_t i = 0; i < registers.size(); ++i) {
                    const uint32_t setBits = Memory::Read32(input + i * 4u);
                    const uint32_t mask = Memory::Read32(input + (i + 4u) * 4u);
                    const uint32_t original = registers[i].load(std::memory_order_acquire);
                    Memory::Write32(output + i * 4u, original);
                    registers[i].store((original & mask) | (setBits & ~mask),
                                       std::memory_order_release);
                    if (mask != 0xFFFFFFFFu) returnFlags |= 1u << i;
                }
                Memory::Write32(output + 12u, returnFlags);
                for (uint32_t i = 4; i < 8; ++i) Memory::Write32(output + i * 4u, 0);
                Memory::Write32(request + 4u, 0);
                RT_LOG(RT_TAG_OS) << "IOS /dev/stm/immediate VIDimming masks=0x" << std::hex
                                  << Memory::Read32(input + 16u) << ",0x"
                                  << Memory::Read32(input + 20u) << ",0x"
                                  << Memory::Read32(input + 24u) << " flags=0x" << returnFlags
                                  << std::dec << std::endl;
                return true;
            }
            if (ioctl != 0x6002u) {
                RT_LOG(RT_TAG_OS) << "Unsupported /dev/stm/immediate ioctl=0x"
                                  << std::hex << ioctl << std::dec << std::endl;
                return false;
            }
            Memory::Write32(request + 4u, 0);
            RT_LOG(RT_TAG_OS) << "IOS /dev/stm/immediate LED mode accepted" << std::endl;
            return true;
        }
        if (IsPrimeIpcDiDescriptor(fd)) {
            if (ioctl == 0x7Au) { // DVDLowPrepareCoverRegister / GetCoverRegister.
                const uint32_t output = Memory::Read32(request + 24u);
                const uint32_t outputLength = Memory::Read32(request + 28u);
                if (!output || outputLength < 4u || !Memory::Contains(output, 4u)) return false;
                // Fixed research-disc mount: cover closed, interrupt disabled,
                // no pending cover-change event. Hot eject is not implemented.
                Memory::Write32(output, 0u);
                Memory::Write32(request + 4u, 1u);
                RT_LOG(RT_TAG_OS) << "IOS /dev/di cover-register -> closed, no cover IRQ" << std::endl;
                return true;
            }
            if (ioctl == 0x71u) { // DVDLowRead: lengths in bytes, positions in four-byte words.
                const uint32_t input = Memory::Read32(request + 16u);
                const uint32_t inputLength = Memory::Read32(request + 20u);
                if (input == 0 || inputLength != 32u || !Memory::Contains(input, inputLength)) return false;
                const uint32_t length = Memory::Read32(input + 4u);
                const uint64_t byteOffset = static_cast<uint64_t>(Memory::Read32(input + 8u)) * 4u;
                const uint32_t output = Memory::Read32(request + 24u);
                const uint32_t outputLength = Memory::Read32(request + 28u);
                if (outputLength < length || !DVD_HLE_ReadPrimePartition(output, length, byteOffset)) return false;
                Memory::Write32(request + 4u, 1u);
                return true;
            }
            if (ioctl == 0x12u) { // DVDLowInquiry
                const uint32_t output = Memory::Read32(request + 24u);
                const uint32_t outputLength = Memory::Read32(request + 28u);
                if (output == 0 || outputLength < 12u || !Memory::Contains(output, outputLength)) {
                    return false;
                }
                // Retail Wii drive identification, as recorded from hardware
                // in Dolphin HW/DVD/DVDInterface.cpp (DICommand::Inquiry).
                Memory::Write32(output, 0x00000002u);
                Memory::Write32(output + 4u, 0x20060526u);
                Memory::Write32(output + 8u, 0x41000000u);
                Memory::Write32(request + 4u, 1u);
                RT_LOG(RT_TAG_OS) << "IOS /dev/di inquiry -> retail drive identification" << std::endl;
                return true;
            }
            // Prime's observed startup command is DVDLowClearCoverInterrupt.
            // Dolphin's DI HLE returns DIResult::Success (1) for ioctl 0x86;
            // there is no modeled host cover-interrupt status to clear here.
            if (ioctl != 0x86u) {
                RT_LOG(RT_TAG_OS) << "Unsupported /dev/di ioctl=0x"
                                  << std::hex << ioctl << std::dec << std::endl;
                return false;
            }
            Memory::Write32(request + 4u, 1u);
            RT_LOG(RT_TAG_OS) << "IOS /dev/di clear-cover-interrupt -> DI success" << std::endl;
            return true;
        }
        const int32_t backendFd = ResolvePrimeIpcBackendFd(fd);
        if (backendFd < 0) return false;
        const int32_t result = NAND_IOS_Ioctl_HLE(static_cast<uint32_t>(backendFd), ioctl,
            Memory::Read32(request + 16u), Memory::Read32(request + 20u),
            Memory::Read32(request + 24u), Memory::Read32(request + 28u));
        Memory::Write32(request + 4u, static_cast<uint32_t>(result));
        RT_LOG(RT_TAG_OS) << "IOS ioctl fd=" << fd << " cmd=" << ioctl << " -> " << result << std::endl;
        return true;
    }
    if (command == 7u) { // IOS_IOCTLV
        const uint32_t fd = Memory::Read32(request + 8u);
        const int32_t backendFd = ResolvePrimeIpcBackendFd(fd);
        if (backendFd < 0) return false;
        const uint32_t ioctl = Memory::Read32(request + 12u);
        const uint32_t numIn = Memory::Read32(request + 16u);
        const uint32_t numOut = Memory::Read32(request + 20u);
        const uint32_t vectorPtr = Memory::Read32(request + 24u);
        if (NAND_IOS_StartOh1InterruptRead_HLE(
                static_cast<uint32_t>(backendFd), ioctl, numIn, numOut, vectorPtr,
                Memory::Read32(request + 32u), Memory::Read32(request + 36u), request)) {
            replyReady = false;
            RT_LOG(RT_TAG_OS) << "IOS OH1 endpoint read accepted; completion is pending request=0x"
                              << std::hex << request << std::dec << std::endl;
            return true;
        }
        const int32_t result = NAND_IOS_Ioctlv_HLE(static_cast<uint32_t>(backendFd), ioctl,
            numIn, numOut, vectorPtr);
        Memory::Write32(request + 4u, static_cast<uint32_t>(result));
        RT_LOG(RT_TAG_OS) << "IOS ioctlv fd=" << fd << " cmd=" << ioctl << " -> " << result << std::endl;
        return true;
    }
    if (command == 2u) { // IOS_CLOSE
        const uint32_t fd = Memory::Read32(request + 8u);
        if (IsPrimeIpcEventHookDescriptor(fd) || IsPrimeIpcImmediateDescriptor(fd) ||
            IsPrimeIpcDiDescriptor(fd)) {
            ClosePrimeIpcDescriptor(fd);
            Memory::Write32(request + 4u, 0);
            return true;
        }
        const int32_t backendFd = ResolvePrimeIpcBackendFd(fd);
        if (backendFd < 0) return false;
        const int32_t result = NAND_IOS_Close_HLE(static_cast<uint32_t>(backendFd));
        ClosePrimeIpcDescriptor(fd);
        Memory::Write32(request + 4u, static_cast<uint32_t>(result));
        return true;
    }
    if (command == 3u || command == 4u) { // IOS_READ / IOS_WRITE
        const uint32_t fd = Memory::Read32(request + 8u);
        const int32_t backendFd = ResolvePrimeIpcBackendFd(fd);
        if (backendFd < 0) return false;
        const uint32_t buffer = Memory::Read32(request + 12u);
        const uint32_t length = Memory::Read32(request + 16u);
        const int32_t result = command == 3u
            ? NAND_IOS_Read_HLE(static_cast<uint32_t>(backendFd), buffer, length)
            : NAND_IOS_Write_HLE(static_cast<uint32_t>(backendFd), buffer, length);
        Memory::Write32(request + 4u, static_cast<uint32_t>(result));
        RT_LOG(RT_TAG_OS) << (command == 3u ? "IOS read" : "IOS write")
                          << " fd=" << fd << " buffer=0x" << std::hex << buffer
                          << " length=0x" << length << std::dec << " -> " << result << std::endl;
        return true;
    }
    if (command == 5u) { // IOS_SEEK
        const uint32_t fd = Memory::Read32(request + 8u);
        const int32_t backendFd = ResolvePrimeIpcBackendFd(fd);
        if (backendFd < 0) return false;
        const int32_t offset = static_cast<int32_t>(Memory::Read32(request + 12u));
        const int32_t whence = static_cast<int32_t>(Memory::Read32(request + 16u));
        const int32_t result = NAND_IOS_Seek_HLE(static_cast<uint32_t>(backendFd), offset, whence);
        Memory::Write32(request + 4u, static_cast<uint32_t>(result));
        RT_LOG(RT_TAG_OS) << "IOS seek fd=" << fd << " offset=" << offset
                          << " whence=" << whence << " -> " << result << std::endl;
        return true;
    }
    if (command != 1u) return false; // IOS_OPEN

    const uint32_t pathAddress = Memory::Read32(request + 12u);
    std::string path;
    path.reserve(32);
    for (uint32_t i = 0; i < 128u; ++i) {
        const char ch = static_cast<char>(Memory::Read8(pathAddress + i));
        if (ch == '\0') break;
        path.push_back(ch);
    }

    // IOS registers this endpoint at boot. Its default Open operation returns
    // IPC_SUCCESS; the kernel then assigns the first available descriptor.
    int32_t fd = -1;
    if (path == "/dev/stm/immediate" || path == "/dev/stm/eventhook" || path == "/dev/di") {
        fd = static_cast<int32_t>(PrimeIpcNextFileDescriptor().fetch_add(1, std::memory_order_relaxed));
        if (path == "/dev/stm/eventhook") SetPrimeIpcEventHookDescriptor(static_cast<uint32_t>(fd));
        if (path == "/dev/stm/immediate") SetPrimeIpcImmediateDescriptor(static_cast<uint32_t>(fd));
        if (path == "/dev/di") SetPrimeIpcDiDescriptor(static_cast<uint32_t>(fd));
    } else {
        const int32_t backendFd = NAND_IOS_Open_HLE(pathAddress, Memory::Read32(request + 16u));
        if (backendFd < 0) {
            Memory::Write32(request + 4u, static_cast<uint32_t>(backendFd));
            return true;
        }
        fd = static_cast<int32_t>(AllocatePrimeIpcBackendFd(backendFd));
    }

    Memory::Write32(request + 4u, static_cast<uint32_t>(fd));
    RT_LOG(RT_TAG_OS) << "IOS IPC open " << path << " -> fd " << fd << std::endl;
    return true;
}

std::atomic<uint32_t>& PrimeSiSamplingControl() {
    static std::atomic<uint32_t> value{0};
    return value;
}

std::atomic<uint32_t>& PrimeWirelessPowerRegister(uint32_t addr) {
    static std::array<std::atomic<uint32_t>, 3> values{};
    switch (addr) {
    case 0xCD800180u: return values[0];
    case 0xCD8001CCu: return values[1];
    default: return values[2]; // CD8001D0
    }
}

std::atomic<uint32_t>& PrimeSiTransferControl() {
    static std::atomic<uint32_t> value{0};
    return value;
}

std::atomic<uint32_t>& PrimeSiChannelData(uint32_t addr) {
    static std::array<std::atomic<uint32_t>, 4> values{};
    return values[(addr - 0xCD006480u) / 4u];
}

std::atomic<uint32_t>& PrimeInterruptUnitRegister(uint32_t addr) {
    static std::array<std::atomic<uint32_t>, 14> values{};
    return values[(addr - 0xCD006800u) / 4u];
}

std::mutex g_primeAiMutex;
PrimeAiRegisters g_primeAi;
uint64_t PrimeAiNow() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::atomic<uint16_t>& PrimeInterruptSourceMask() {
    static std::atomic<uint16_t> value{0};
    return value;
}

std::atomic<uint16_t>& PrimeMemoryInterruptMask() {
    static std::atomic<uint16_t> value{0};
    return value;
}

std::atomic<uint16_t>& PrimeMemoryInterruptClear() {
    static std::atomic<uint16_t> value{0};
    return value;
}

PrimeDspBootRegisters g_primeDspControl;

std::atomic<uint16_t>& PrimeMiTimerRegister(uint32_t address) {
    static std::array<std::atomic<uint16_t>, 20> values{};
    return values[(address - 0xCC004032u) / 2u];
}

std::atomic<uint16_t>& PrimeDspAramInfo() {
    static std::atomic<uint16_t> value{0};
    return value;
}

struct PrimeDspRomState {
    bool axActive = false;
    uint32_t toDsp = 0;
    uint32_t nextParameter = 0;
    uint32_t source = 0;
    uint32_t length = 0;
    uint32_t destination = 0;
    uint32_t dmemLength = 0;
    uint32_t entry = 0;
    uint32_t lastMail = 0;
    std::deque<uint32_t> fromDsp{0x8071FEEDu};
};
std::mutex g_primeDspMutex;
PrimeDspRomState g_primeDspRom;

void PrimeDspCollectAxMails() {
    static PrimeTraceBudget replyTrace{64};
    while (AxDspHle::CheckMailFromDSP()) {
        const uint32_t mail = AxDspHle::ReadMailFromDSP();
        g_primeDspRom.fromDsp.push_back(mail);
        g_primeDspControl.control |= 0x80u;
        if (replyTrace.Take())
            RT_LOG(RT_TAG_AUDIO) << "[prime-dsp-ax-reply] value=0x" << std::hex << mail << std::dec << std::endl;
    }
}

void PrimeDspSendMail(uint32_t mail) {
    static PrimeTraceBudget mailTrace{64};
    if (mailTrace.Take())
        RT_LOG(RT_TAG_AUDIO) << "[prime-dsp-rom-mail] value=0x" << std::hex << mail << std::dec << std::endl;
    auto& rom = g_primeDspRom;
    if (rom.axActive) {
        AxDspHle::SendMailToDSP(mail);
        PrimeDspCollectAxMails();
        return;
    }
    if (rom.nextParameter == 0u) {
        if ((mail & 0xFFFF0000u) == 0x80F30000u) rom.nextParameter = mail;
        else rom.fromDsp.push_back(0xFEEE0000u | (mail & 0xFFFFu));
        return;
    }
    const uint32_t parameter = rom.nextParameter;
    rom.nextParameter = 0;
    switch (parameter) {
    case 0x80F3A001u: rom.source = mail; break;
    case 0x80F3A002u: rom.length = mail & 0xFFFFu; break;
    case 0x80F3B002u: rom.dmemLength = mail & 0xFFFFu; break;
    case 0x80F3C002u: rom.destination = mail & 0xFFFFu; break;
    case 0x80F3D001u: {
        rom.entry = mail & 0xFFFFu;
        if (rom.source == 0u || rom.length == 0u || !Memory::Contains(rom.source, rom.length)) {
            throw Memory::AccessViolation(rom.source, rom.length, "invalid DSP microcode upload range");
        }
        // HashEctor identifies DSP microcodes by guest bytes, not SDK version.
        uint32_t hash = 0;
        for (uint32_t i = 0; i < rom.length; ++i) {
            hash ^= Memory::Read8(rom.source + i);
            hash = (hash << 3u) | (hash >> 29u);
        }
        RT_LOG(RT_TAG_AUDIO) << "[prime-dsp-ucode] source=0x" << std::hex << rom.source
                            << " length=0x" << rom.length << " imem=0x" << rom.destination
                            << " dmem=0x" << rom.dmemLength << " entry=0x" << rom.entry
                            << " hash=0x" << hash << std::dec << std::endl;
        if (hash != 0x347112BAu)
            throw std::runtime_error("Unsupported Prime DSP microcode fingerprint");
        AxDspHle::InitPrimeMicrocode(hash);
        rom.axActive = true;
        rom.fromDsp.clear();
        PrimeDspCollectAxMails();
        RT_LOG(RT_TAG_AUDIO) << "[prime-dsp-ax-start] crc=0x347112ba raw_mailbox=1 mkw_task_globals=0" << std::endl;
        break;
    }
    default: throw std::runtime_error("Unsupported Prime DSP ROM upload parameter");
    }
}

std::atomic<uint32_t>& PrimeDspDmaRegister(uint32_t addr) {
    static std::array<std::atomic<uint32_t>, 3> values{};
    return values[(addr - 0xCC005020u) / 4u];
}

// VI is a 16-bit register bank. Keep writes visible to later guest reads;
// initialize DCR.ENB as the console's boot-time VI does.
std::array<std::atomic<uint16_t>, 0x40>& PrimeViRegisters() {
    static std::array<std::atomic<uint16_t>, 0x40> registers{};
    static const bool initialized = (registers[1].store(1, std::memory_order_relaxed), true);
    (void)initialized;
    return registers;
}

std::array<std::atomic<uint16_t>, 0x40>& PrimeCpRegisters() {
    static std::array<std::atomic<uint16_t>, 0x40> registers{};
    static const bool initialized = (registers[0].store(0x000Cu, std::memory_order_relaxed), true);
    (void)initialized;
    return registers;
}

PrimeCpuFifo::Capture g_primeCpuFifoCapture;
uint32_t& PrimeGatherBytes() { return g_primeCpuFifoCapture.count; }
bool PrimeCpuFifoCaptureActive() {
    return PrimePiFifoRegister(0xCC003010u).load() > PrimePiFifoRegister(0xCC00300Cu).load();
}
uint32_t PrimeCpPair(uint32_t offset) {
    return PrimeCpRegisters()[offset / 2].load() |
        (uint32_t(PrimeCpRegisters()[offset / 2 + 1].load()) << 16);
}
void PrimeCpSetPair(uint32_t offset, uint32_t value) {
    PrimeCpRegisters()[offset / 2].store(uint16_t(value));
    PrimeCpRegisters()[offset / 2 + 1].store(uint16_t(value >> 16));
}
void PrimeCpUpdateStatus() {
    const uint16_t control = PrimeCpRegisters()[1].load();
    // All guest bytes before this safe point have passed the synchronous FIFO
    // decoder. Linked read/write pointers converge when GP reads are enabled.
    const bool retained = PrimeCpFifo::RetainBreakpoint(control, PrimeCpRegisters()[0].load(),
        PrimeCpPair(0x38), PrimeCpPair(0x3C));
    if ((control & 0x11u) == 0x11u && !retained) {
        PrimeCpSetPair(0x38, PrimeCpPair(0x34));
        PrimeCpSetPair(0x30, 0);
    }
    const bool boundary = GX_HLE_PrimeFifoAtBoundary();
    const bool atBreakpoint = retained || PrimeCpFifo::AtBreakpoint(control, PrimeCpPair(0x38),
                                                       PrimeCpPair(0x3C), boundary);
    PrimeCpRegisters()[0].store(uint16_t((boundary ? 0xCu : 0u) | (atBreakpoint ? 0x10u : 0u)));
}

std::array<std::atomic<uint16_t>, 0x80>& PrimePeRegisters() {
    static std::array<std::atomic<uint16_t>, 0x80> registers{};
    return registers;
}

bool ReadPrimePiMmio32(uint32_t addr, uint32_t& value) {
    if (addr >= 0xCD006C00u && addr <= 0xCD006C0Cu && (addr & 3u) == 0u) {
        std::lock_guard<std::mutex> lock(g_primeAiMutex);
        value = g_primeAi.Read(addr - 0xCD006C00u, PrimeAiNow());
        return true;
    }
    if (addr == 0xCD8000C0u || addr == 0xCD0000C0u) {
        value = PrimeGpioOutput().load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCD8000C4u || addr == 0xCD0000C4u) {
        value = PrimeGpioDirection().load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCD8000C8u) {
        const uint32_t direction = PrimeGpioDirection().load(std::memory_order_acquire);
        const uint32_t output = PrimeGpioOutput().load(std::memory_order_acquire);
        value = output & direction; // Unconnected input pins are low in this runtime.
        return true;
    }
    if (addr == 0xCD000004u) {
        PublishNextPrimeIpcReply();
        value = PrimeIpcPpcControl().load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCD000008u) {
        value = PrimeIpcArmMessage().load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCD000030u) {
        PublishNextPrimeIpcReply();
        value = PrimeIpcPpcInterruptFlags().load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCC003024u) {
        value = PrimePiStatusRegister().load(std::memory_order_acquire);
        return true;
    }
    if (addr >= 0xCC004000u && addr <= 0xCC00400Cu && (addr & 3u) == 0) {
        value = PrimeMiProtectedRegion(addr).load(std::memory_order_acquire);
        return true;
    }
    if (addr >= 0xCC00300Cu && addr <= 0xCC003014u && (addr & 3u) == 0) {
        value = PrimePiFifoRegister(addr).load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCD006024u) {
        value = 0; // Cold boot reset code: no hardware reset request is pending.
        return true;
    }
    if (addr == 0xCC003004u) {
        value = PrimePiInterruptMask().load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCD000034u) {
        value = PrimeSystemInterruptEnable().load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCD006430u) {
        value = PrimeSiSamplingControl().load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCD800180u || addr == 0xCD8001CCu || addr == 0xCD8001D0u) {
        value = PrimeWirelessPowerRegister(addr).load(std::memory_order_acquire);
        return true;
    }
    if (addr >= 0xCC005020u && addr <= 0xCC005028u && (addr & 3u) == 0) {
        value = PrimeDspDmaRegister(addr).load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCD006434u) {
        value = PrimeSiTransferControl().load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCD006438u) {
        value = 0; // No GameCube SI devices are attached to the native input backend.
        return true;
    }
    if (addr >= 0xCD006480u && addr <= 0xCD00648Cu && (addr & 3u) == 0) {
        value = 0; // No SI pad response; Wii Remote input is a separate backend.
        return true;
    }
    if (addr >= 0xCD006800u && addr <= 0xCD006834u && (addr & 3u) == 0) {
        // EXI channel status registers report idle until a device backend exists.
        if (addr == 0xCD00680Cu || addr == 0xCD006820u || addr == 0xCD006834u) {
            value = 0;
            return true;
        }
        value = PrimeInterruptUnitRegister(addr).load(std::memory_order_acquire);
        return true;
    }
    return false;
}

bool WritePrimePiMmio32(uint32_t addr, uint32_t value) {
    if (addr >= 0xCD006C00u && addr <= 0xCD006C0Cu && (addr & 3u) == 0u) {
        std::lock_guard<std::mutex> lock(g_primeAiMutex);
        g_primeAi.Write(addr - 0xCD006C00u, value, PrimeAiNow());
        static uint32_t trace = 0;
        if (trace++ < 32u) RT_LOG(RT_TAG_OS) << "[prime-ai-write] reg=0x" << std::hex << addr
            << " value=0x" << value << " control=0x" << g_primeAi.control << std::dec << std::endl;
        return true;
    }
    if (addr == 0xCD8000C0u || addr == 0xCD0000C0u) {
        // GPIOB aliases share the Broadway-owned output latch. The observed
        // WUD startup caller sets SENSOR_BAR (0x100), not an IPC command.
        constexpr uint32_t owner = 0x0000C3A0u;
        uint32_t old = PrimeGpioOutput().load(std::memory_order_relaxed);
        while (!PrimeGpioOutput().compare_exchange_weak(old, (old & ~owner) | (value & owner))) {}
        return true;
    }
    if (addr == 0xCD8000C4u || addr == 0xCD0000C4u) {
        PrimeGpioDirection().store(value, std::memory_order_release);
        return true;
    }
    if (addr == 0xCD000030u) {
        // IPC PPC IRQ flags are write-one-to-clear; the Prime ISR acknowledges
        // the IOS reply after consuming ARMMSG.
        PrimeIpcPpcInterruptFlags().fetch_and(~value, std::memory_order_acq_rel);
        return true;
    }
    if (addr == 0xCD000000u) {
        // PPCMSG is the write-only guest pointer to the next 0x40-byte IOS
        // request packet. IOS command execution is triggered separately by X1.
        PrimeIpcPpcMessage().store(value, std::memory_order_release);
        static PrimeTraceBudget messageTrace(128);
        if (messageTrace.Take()) RT_LOG(RT_TAG_OS) << "IPC PPCMSG <- 0x" << std::hex << value << std::dec << std::endl;
        return true;
    }
    if (addr == 0xCD000004u) {
        // Wii IPC PPCCTRL: X1/X2 and interrupt enables are writable; Y1/Y2
        // are acknowledged by writing their respective bits. The observed
        // Prime boot write (0x38) only sets X2/IY1/IY2 and does not submit an
        // IOS command.
        auto& control = PrimeIpcPpcControl();
        uint32_t old = control.load(std::memory_order_relaxed);
        uint32_t desired;
        do {
            desired = (old & ~0x39u) | (value & 0x39u);
            if ((value & 0x04u) != 0) desired &= ~0x04u;
            if ((value & 0x02u) != 0) desired &= ~0x02u;
        } while (!control.compare_exchange_weak(old, desired, std::memory_order_release,
                                                std::memory_order_relaxed));
        if ((value & 4u) != 0) {
            PrimeIpcArmMessage().store(0u, std::memory_order_release);
            PublishNextPrimeIpcReply();
        }
        if (((value & 0x04u) != 0 && (desired & 0x10u) != 0) ||
            ((value & 0x02u) != 0 && (desired & 0x20u) != 0)) {
            PrimeIpcPpcInterruptFlags().fetch_or(0x40000000u, std::memory_order_release);
        }
        static PrimeTraceBudget controlTrace(256);
        if (controlTrace.Take()) RT_LOG(RT_TAG_OS) << "IPC PPCCTRL <- 0x" << std::hex << value
                          << " state=0x" << desired << " PPCMSG=0x"
                          << PrimeIpcPpcMessage().load(std::memory_order_acquire) << std::dec << std::endl;

        if ((value & 1u) != 0) {
            const uint32_t request = PrimeIpcPpcMessage().load(std::memory_order_acquire);
            const uint32_t command = Memory::Read32(request);
            // Record the operation before Submit/reply rewrites the IPC header.
            uint64_t replyDelayMicros = 0;
            if (command == 6u && IsPrimeIpcDiDescriptor(Memory::Read32(request + 8u)) &&
                Memory::Read32(request + 12u) == 0x71u) {
                const uint32_t input = Memory::Read32(request + 16u);
                if (input && Memory::Contains(input, 12u)) {
                    replyDelayMicros = PrimeIpcReplyQueue::ReadDelayMicros(Memory::Read32(input + 4u));
                }
            }
            bool replyReady = false;
            if (!SubmitPrimeIpcRequest(request, replyReady)) {
                RT_LOG(RT_TAG_OS) << "Unsupported IOS IPC submit: PPCCTRL=0x" << std::hex << value
                          << " PPCMSG=0x" << request
                          << " command=0x" << Memory::Read32(request)
                          << " arg0=0x" << Memory::Read32(request + 12u)
                          << " arg1=0x" << Memory::Read32(request + 16u)
                          << " arg2=0x" << Memory::Read32(request + 20u)
                          << " callback=0x" << Memory::Read32(request + 32u)
                          << std::dec << std::endl;
                return false;
            }
            control.fetch_and(~0x01u, std::memory_order_acq_rel); // IOS clears X1 on acknowledge.
            control.fetch_or(0x02u, std::memory_order_release);
            // Y2 acknowledges command acceptance; Y1 makes a completed reply
            // available through ARMMSG. STM_EVENTHOOK is intentionally pending.
            if (replyReady) {
                // IOS reply ABI: the fd slot becomes the original command and
                // the command slot becomes IPC_REPLY (8). Prime's ISR switches
                // on +8 to restore physical buffer pointers for that command.
                // Reference: Dolphin Core/IOS/IOS.cpp, EnqueueIPCReply.
                // Do not complete a disc read on the submission stack. FMOD's
                // original file callback submits, then suspends its thread;
                // a zero-latency reply can consume its wake before suspension.
                QueuePrimeIpcReply(request, command, replyDelayMicros);
            }
            PublishNextPrimeIpcReply();
            const uint32_t replyControl = control.load(std::memory_order_acquire);
            if (((replyReady && (replyControl & 0x10u) != 0) ||
                 (replyControl & 0x20u) != 0)) {
                PrimeIpcPpcInterruptFlags().fetch_or(0x40000000u, std::memory_order_release);
            }
        }
        return true;
    }
    if (addr == 0xCC003024u) {
        PrimePiStatusRegister().store(value, std::memory_order_release);
        return true;
    }
    if (addr >= 0xCC004000u && addr <= 0xCC00400Cu && (addr & 3u) == 0) {
        // MI protection-region registers hold the four protected page ranges.
        // Store guest-visible state; actual protection faults are not modeled.
        PrimeMiProtectedRegion(addr).store(value, std::memory_order_release);
        return true;
    }
    if (addr >= 0xCC00300Cu && addr <= 0xCC003014u && (addr & 3u) == 0) {
        // PI's CPU FIFO base/end/write pointer are 32-byte aligned physical addresses.
        PrimePiFifoRegister(addr).store(value & 0x17FFFFE0u, std::memory_order_release);
        return true;
    }
    if (addr == 0xCC003018u) {
        if (value & 1u) {
            static unsigned resetCallerCount = 0;
            if (resetCallerCount++ < 16) {
                const auto* cpu = TryGetCpuContext();
                RT_LOG(RT_TAG_GX) << "[prime-fifo-reset-caller] active=0x" << std::hex
                    << RecompMod::CurrentTranslatedExecutionAddress() << " lr=0x"
                    << (cpu ? cpu->lr : 0u) << " msr=0x" << (cpu ? cpu->msr : 0u)
                    << " os_mask=0x" << Memory::Read32(0x800000C4u)
                    << " cp_control=0x" << PrimeCpRegisters()[1].load()
                    << " cp_status=0x" << PrimeCpRegisters()[0].load()
                    << " read=0x" << PrimeCpPair(0x38) << " breakpoint=0x" << PrimeCpPair(0x3C)
                    << " frame_state=" << Memory::Read32(0x8067DECCu) << std::dec << std::endl;
            }
            PrimeGatherBytes() = 0;
            g_primeCpuFifoCapture = {};
            GX_HLE_PrimeResetFifo();
        }
        return true;
    }
    if (addr == 0xCD000034u) {
        // Prime's early OS interrupt setup writes 0x40000000 here. Runtime IRQ
        // delivery is host-driven; retain the register value for guest reads.
        PrimeSystemInterruptEnable().store(value, std::memory_order_release);
        return true;
    }
    if (addr >= 0xCC005020u && addr <= 0xCC005028u && (addr & 3u) == 0) {
        PrimeDspDmaRegister(addr).store(value, std::memory_order_release);
        if (addr == 0xCC005028u) {
            const uint32_t count = value & 0x03FFFFE0u;
            const uint32_t mainAddress = PrimeDspDmaRegister(0xCC005020u).load() & 0x03FFFFE0u;
            const uint32_t aramOffset = PrimeDspDmaRegister(0xCC005024u).load() & 0x03FFFFE0u;
            const uint32_t aramAddress = 0x10000000u + aramOffset;
            if (count == 0 || !Memory::Contains(mainAddress, count) || !Memory::Contains(aramAddress, count))
                throw Memory::AccessViolation(mainAddress, count, "invalid Prime ARAM DMA range");
            // Wii ARAM storage is MEM2. Transfer timing/IRQ dispatch are pending.
            const bool reverse = (value & 0x80000000u) != 0;
            const uint32_t source = reverse ? aramAddress : mainAddress;
            const uint32_t destination = reverse ? mainAddress : aramAddress;
            std::vector<uint8_t> bytes(count);
            for (uint32_t i = 0; i < count; ++i) bytes[i] = Memory::Read8(source + i);
            for (uint32_t i = 0; i < count; ++i) Memory::Write8(destination + i, bytes[i]);
            PrimeDspDmaRegister(0xCC005020u).store(mainAddress + count);
            PrimeDspDmaRegister(0xCC005024u).store(aramOffset + count);
            PrimeDspDmaRegister(0xCC005028u).store(value & 0x80000000u);
            std::lock_guard<std::mutex> lock(g_primeDspMutex);
            g_primeDspControl.CompleteAramDma();
            RT_LOG(RT_TAG_AUDIO) << "[prime-aram-dma] source=0x" << std::hex << source
                                << " destination=0x" << destination << " length=0x" << count
                                << std::dec << std::endl;
        }
        return true;
    }
    if (addr == 0xCD006430u) {
        PrimeSiSamplingControl().store(value, std::memory_order_release);
        return true;
    }
    if (addr == 0xCD800180u || addr == 0xCD8001CCu || addr == 0xCD8001D0u) {
        PrimeWirelessPowerRegister(addr).store(value, std::memory_order_release);
        return true;
    }
    if (addr == 0xCD006434u) {
        // A transfer has no physical SI device behind this host runtime. Complete
        // the command immediately by clearing the hardware busy/start bit.
        PrimeSiTransferControl().store(value & ~0x80000000u, std::memory_order_release);
        return true;
    }
    if (addr == 0xCD006438u) return true; // SI interrupt status: no device event to acknowledge.
    if (addr >= 0xCD006480u && addr <= 0xCD00648Cu && (addr & 3u) == 0) {
        PrimeSiChannelData(addr).store(value, std::memory_order_release);
        return true;
    }
    if (addr >= 0xCD006800u && addr <= 0xCD006834u && (addr & 3u) == 0) {
        PrimeInterruptUnitRegister(addr).store(value, std::memory_order_release);
        return true;
    }
    if (addr != 0xCC003004u) return false;
    auto& mask = PrimePiInterruptMask();
    uint32_t old = mask.load(std::memory_order_relaxed);
    uint32_t desired;
    do {
        desired = (old | (value >> 16)) & ~(value & 0xFFFFu);
    } while (!mask.compare_exchange_weak(old, desired, std::memory_order_release,
                                         std::memory_order_relaxed));
    return true;
}

bool ReadPrimeMmio16(uint32_t addr, uint16_t& value) {
    if (addr >= 0xCC000000u && addr <= 0xCC00007Eu && (addr & 1u) == 0) {
        const uint32_t index = (addr - 0xCC000000u) / 2u;
        if (index == 0) PrimeCpUpdateStatus();
        value = PrimeCpRegisters()[index].load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCC00206Cu || addr == 0xCC00206Eu) {
        value = 0; // VI reports the cold-start composite/no-DTV state.
        return true;
    }
    if (addr >= 0xCC002000u && addr <= 0xCC00207Eu && (addr & 1u) == 0) {
        value = PrimeViRegisters()[(addr - 0xCC002000u) / 2u].load(std::memory_order_acquire);
        return true;
    }
    if (addr >= 0xCC001000u && addr <= 0xCC0010FEu && (addr & 1u) == 0) {
        value = PrimePeRegisters()[(addr - 0xCC001000u) / 2u].load(std::memory_order_acquire);
        return true;
    }
    if (addr >= 0xCC004000u && addr <= 0xCC00400Eu && (addr & 1u) == 0) {
        const uint32_t regAddr = addr & ~3u;
        const uint32_t reg = PrimeMiProtectedRegion(regAddr).load(std::memory_order_acquire);
        value = static_cast<uint16_t>((addr & 2u) == 0 ? reg >> 16 : reg);
        return true;
    }
    if (addr == 0xCC00401Cu) {
        value = PrimeInterruptSourceMask().load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCC004010u) {
        value = PrimeMemoryInterruptMask().load(std::memory_order_acquire);
        return true;
    }
    if (addr == 0xCC00500Au) {
        std::lock_guard<std::mutex> lock(g_primeDspMutex);
        value = g_primeDspControl.Read(PrimeAiNow());
        return true;
    }
    if (addr >= 0xCC004032u && addr <= 0xCC004058u && (addr & 1u) == 0) {
        // Diagnostic MI timer latches, matching the reference MMIO bank.
        // Host RAM/GPU traffic is not a Wii bus measurement: no synthetic
        // activity counts are accumulated or used as a gameplay clock.
        value = PrimeMiTimerRegister(addr).load(std::memory_order_acquire);
        return true;
    }
    if (addr >= 0xCC005000u && addr <= 0xCC005006u && (addr & 1u) == 0) {
        std::lock_guard<std::mutex> lock(g_primeDspMutex);
        auto& rom = g_primeDspRom;
        if (addr == 0xCC005000u) value = static_cast<uint16_t>(rom.toDsp >> 16u);
        else if (addr == 0xCC005002u) value = static_cast<uint16_t>(rom.toDsp);
        else if (addr == 0xCC005004u) value = (rom.fromDsp.empty() || g_primeDspControl.Halted()) ? static_cast<uint16_t>((rom.lastMail >> 16u) & 0x7FFFu) : static_cast<uint16_t>(rom.fromDsp.front() >> 16u);
        else {
            if (!rom.fromDsp.empty() && !g_primeDspControl.Halted()) {
                rom.lastMail = rom.fromDsp.front(); rom.fromDsp.pop_front();
                if (rom.axActive && !rom.fromDsp.empty()) g_primeDspControl.control |= 0x80u;
            }
            value = static_cast<uint16_t>(rom.lastMail);
        }
        return true;
    }
    if (addr == 0xCC005012u) {
        value = PrimeDspAramInfo().load(std::memory_order_acquire);
        return true;
    }
    if (addr >= 0xCC005030u && addr <= 0xCC00503Au && (addr & 1u) == 0) {
        value = Audio_HLE_PrimeDspDmaRead(addr);
        return true;
    }
    return false;
}

bool WritePrimeMmio16(uint32_t addr, uint16_t value) {
    if (addr >= 0xCC000000u && addr <= 0xCC00007Eu && (addr & 1u) == 0) {
        const uint32_t index = (addr - 0xCC000000u) / 2u;
        if (addr == 0xCC000002u) {
            PrimeCpRegisters()[index].store(static_cast<uint16_t>(value & 0x003Fu),
                                            std::memory_order_release);
        } else if (addr == 0xCC000004u) {
            PrimeCpRegisters()[0].fetch_and(static_cast<uint16_t>(~(value & 0x0003u)),
                                            std::memory_order_acq_rel);
        } else if (addr != 0xCC000000u && addr != 0xCC00000Eu) {
            PrimeCpRegisters()[index].store(value, std::memory_order_release);
        }
        PrimeCpUpdateStatus();
        if (addr == 0xCC000002u && (value & 1u) != 0 &&
            (PrimeCpRegisters()[0].load() & 0x10u) == 0) {
            GX_HLE_PrimeResumeFifo();
            PrimeCpUpdateStatus();
        }
        static unsigned armTraceCount = 0;
        if (addr == 0xCC000002u && (value & 0x23u) == 0x23u && armTraceCount++ < 16) {
            RT_LOG(RT_TAG_GX) << "[prime-cp-arm] control=0x" << std::hex << value
                << " read=0x" << PrimeCpPair(0x38) << " write=0x" << PrimeCpPair(0x34)
                << " breakpoint=0x" << PrimeCpPair(0x3C) << " pi_write=0x"
                << PrimePiFifoRegister(0xCC003014u).load() << " status=0x"
                << PrimeCpRegisters()[0].load() << " gather=" << PrimeGatherBytes()
                << " boundary=" << GX_HLE_PrimeFifoAtBoundary() << std::dec << std::endl;
        }
        return true;
    }
    if (addr >= 0xCC002000u && addr <= 0xCC00207Eu && (addr & 1u) == 0) {
        PrimeViRegisters()[(addr - 0xCC002000u) / 2u].store(value, std::memory_order_release);
        return true;
    }
    if (addr >= 0xCC001000u && addr <= 0xCC0010FEu && (addr & 1u) == 0) {
        const uint32_t index = (addr - 0xCC001000u) / 2u;
        if (addr == 0xCC00100Au) {
            // PE finish/token status bits are write-one-to-clear; low bits are enables.
            auto& status = PrimePeRegisters()[index];
            uint16_t old = status.load(std::memory_order_relaxed);
            uint16_t desired;
            do {
                desired = static_cast<uint16_t>((old & 0x000Cu & ~(value & 0x000Cu)) |
                                                (value & 0x0003u));
            } while (!status.compare_exchange_weak(old, desired, std::memory_order_release,
                                                   std::memory_order_relaxed));
        } else {
            PrimePeRegisters()[index].store(value, std::memory_order_release);
        }
        return true;
    }
    if (addr >= 0xCC004000u && addr <= 0xCC00400Eu && (addr & 1u) == 0) {
        const uint32_t regAddr = addr & ~3u;
        auto& reg = PrimeMiProtectedRegion(regAddr);
        const uint32_t shift = (addr & 2u) == 0 ? 16u : 0u;
        const uint32_t mask = 0xFFFFu << shift;
        uint32_t old = reg.load(std::memory_order_relaxed);
        uint32_t desired;
        do {
            desired = (old & ~mask) | (static_cast<uint32_t>(value) << shift);
        } while (!reg.compare_exchange_weak(old, desired, std::memory_order_release,
                                            std::memory_order_relaxed));
        return true;
    }
    if (addr == 0xCC00401Cu) {
        PrimeInterruptSourceMask().store(value, std::memory_order_release);
        return true;
    }
    if (addr == 0xCC004010u) {
        // Prime updates the five two-bit MI channel masks with read/modify/write.
        // Preserve that observable register state; actual MI IRQ delivery is not modeled.
        PrimeMemoryInterruptMask().store(value, std::memory_order_release);
        return true;
    }
    if (addr == 0xCC004020u) {
        // Prime's MI startup writes zero to the interrupt-clear register. Keep the
        // written state for now; no MI interrupt source is generated by this runtime.
        PrimeMemoryInterruptClear().store(value, std::memory_order_release);
        return true;
    }
    if (addr == 0xCC00500Au) {
        std::lock_guard<std::mutex> lock(g_primeDspMutex);
        const auto transition = g_primeDspControl.Write(value, PrimeAiNow());
        if (transition == PrimeDspBootRegisters::Transition::ResetRom) {
            if (g_primeDspRom.axActive) AxDspHle::Stop();
            g_primeDspRom = {};
        } else if (transition == PrimeDspBootRegisters::Transition::InitAudio) {
            if (g_primeDspRom.axActive) AxDspHle::Stop();
            g_primeDspRom = {};
            g_primeDspRom.fromDsp = {0x80544348u};
        }
        static PrimeTraceBudget controlTrace{64};
        if (controlTrace.Take())
            RT_LOG(RT_TAG_AUDIO) << "[prime-dsp-control] write=0x" << std::hex << value
                                << " state=0x" << g_primeDspControl.control << std::dec << std::endl;
        return true;
    }
    if (addr >= 0xCC004032u && addr <= 0xCC004058u && (addr & 1u) == 0) {
        PrimeMiTimerRegister(addr).store(value, std::memory_order_release);
        return true;
    }
    if (addr == 0xCC005000u || addr == 0xCC005002u) {
        std::lock_guard<std::mutex> lock(g_primeDspMutex);
        if (addr == 0xCC005000u) g_primeDspRom.toDsp = (g_primeDspRom.toDsp & 0xFFFFu) | (static_cast<uint32_t>(value) << 16u);
        else {
            g_primeDspRom.toDsp = (g_primeDspRom.toDsp & 0xFFFF0000u) | value;
            PrimeDspSendMail(g_primeDspRom.toDsp);
            g_primeDspRom.toDsp &= 0x7FFFFFFFu;
        }
        return true;
    }
    if (addr == 0xCC005012u) {
        PrimeDspAramInfo().store(value & 0x7Fu, std::memory_order_release);
        return true;
    }
    if (addr >= 0xCC005030u && addr <= 0xCC00503Au && (addr & 1u) == 0) {
        Audio_HLE_PrimeDspDmaWrite(addr, value);
        return true;
    }
    return false;
}
#endif

std::vector<DeferredRead>& DeferredReads() {
    static std::vector<DeferredRead> reads;
    return reads;
}

uint64_t& NextDeferredReadToken() {
    static uint64_t token = 1;
    return token;
}

bool RangesOverlap(uint32_t firstStart, size_t firstLength,
                   uint32_t secondStart, size_t secondLength) {
    const uint64_t firstEnd = static_cast<uint64_t>(firstStart) + firstLength;
    const uint64_t secondEnd = static_cast<uint64_t>(secondStart) + secondLength;
    return static_cast<uint64_t>(firstStart) < secondEnd &&
           static_cast<uint64_t>(secondStart) < firstEnd;
}

void RefreshDeferredReadPage(uint32_t page) {
    const uint32_t pageStart = page << MemoryInline::kPageShift;
    const auto& reads = DeferredReads();
    const bool covered = std::any_of(reads.begin(), reads.end(), [pageStart](const DeferredRead& read) {
        return RangesOverlap(pageStart, MemoryInline::kPageSize, read.start, read.length);
    });
    MemoryInline::g_fullReadablePageBias[page] =
        !covered ? MemoryInline::g_fullPageBias[page] : 0;
    MemoryInline::g_deferredReadCoveredPages[page] = covered ? 1 : 0;
}

struct Region {
    Memory::RegionConfig config;
    uint8_t* storagePtr = nullptr;
    size_t storageSize = 0;
};

std::vector<Region>& Regions() {
    static std::vector<Region> regions;
    return regions;
}

std::mutex& RegionMutex() {
    static std::mutex mutex;
    return mutex;
}

std::array<std::unique_ptr<MemoryInline::SparseWritablePageTable>,
           MemoryInline::kPageCount>& SparseWritablePageTableStorage() {
    static std::array<std::unique_ptr<MemoryInline::SparseWritablePageTable>,
                      MemoryInline::kPageCount> storage;
    return storage;
}

void ClearPageTable() {
    auto* table = MemoryInline::g_pageTable;
    for (uint32_t i = 0; i < MemoryInline::kPageCount; ++i) {
        auto& entry = table[i];
        entry.base = nullptr;
        entry.limit = 0;
        MemoryInline::g_fullPageBias[i] = 0;
        MemoryInline::g_fullReadablePageBias[i] = 0;
        MemoryInline::g_fullWritablePageBias[i] = 0;
        MemoryInline::g_deferredReadCoveredPages[i] = 0;
        MemoryInline::g_sparseWritablePageTables[i] = nullptr;
        SparseWritablePageTableStorage()[i].reset();
    }
}

} // namespace

extern "C" uint32_t MemoryPrimeIpcPollControl()
{
    PublishNextPrimeIpcReply();
    return PrimeIpcPpcControl().load(std::memory_order_acquire);
}

extern "C" uint16_t MemoryPrimeDspPollControl()
{
    // Same register and lock as MMIO, avoiding the generic address decoder
    // at every translated call. The clock matters only during DSP init.
    std::lock_guard<std::mutex> lock(g_primeDspMutex);
    return g_primeDspControl.initClearAt ? g_primeDspControl.Read(PrimeAiNow())
                                       : g_primeDspControl.control;
}

extern "C" void PRIME_IOS_CompleteAsync(uint32_t request, int32_t result, uint32_t command)
{
    Memory::Write32(request + 4u, static_cast<uint32_t>(result));
    QueuePrimeIpcReply(request, command);
    // Publication happens at the current request's acceptance or the next Y1
    // acknowledgement, never by overwriting a mailbox reply still in flight.
}

uint64_t Memory::RegisterDeferredRead(uint32_t addr, size_t length,
                                      DeferredReadCallback callback, void* user) {
    if (length == 0 || callback == nullptr ||
        static_cast<uint64_t>(addr) + length > (uint64_t{1} << 32)) {
        return 0;
    }

    std::lock_guard lock{DeferredReadMutex()};
    uint64_t token = NextDeferredReadToken()++;
    if (token == 0) token = NextDeferredReadToken()++;
    DeferredReads().push_back({token, addr, length, callback, user});
    const uint32_t firstPage = addr >> MemoryInline::kPageShift;
    const uint32_t lastPage = static_cast<uint32_t>(
        (static_cast<uint64_t>(addr) + length - 1) >> MemoryInline::kPageShift);
    for (uint32_t page = firstPage; page <= lastPage; ++page) {
        MemoryInline::g_fullReadablePageBias[page] = 0;
        MemoryInline::g_deferredReadCoveredPages[page] = 1;
    }
    // Clearing the readable bias only intercepts the checked path. A flat read
    // needs the host pages themselves to trap, which is what PAGE_NOACCESS on
    // the guest view does; the vectored handler materializes the copy, restores
    // the protection and re-runs the access.
    GuestFlat::ProtectDeferredRange(addr, length);
    return token;
}

void Memory::ClearDeferredReads() {
    std::lock_guard lock{DeferredReadMutex()};
    auto& reads = DeferredReads();
    std::vector<uint32_t> pages;
    for (const auto& read : reads) {
        const uint32_t firstPage = read.start >> MemoryInline::kPageShift;
        const uint32_t lastPage = static_cast<uint32_t>(
            (static_cast<uint64_t>(read.start) + read.length - 1) >> MemoryInline::kPageShift);
        for (uint32_t page = firstPage; page <= lastPage; ++page) pages.push_back(page);
        GuestFlat::UnprotectDeferredRange(read.start, read.length);
    }
    reads.clear();
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    for (uint32_t page : pages) RefreshDeferredReadPage(page);
}

bool MemoryInline::ResolveDeferredReads(uint32_t addr, size_t length) {
    std::vector<DeferredRead> matches;
    std::vector<uint32_t> affectedPages;
    {
        std::lock_guard lock{DeferredReadMutex()};
        auto& reads = DeferredReads();
        for (auto it = reads.begin(); it != reads.end();) {
            if (!RangesOverlap(addr, length, it->start, it->length)) {
                ++it;
                continue;
            }
            const uint32_t firstPage = it->start >> kPageShift;
            const uint32_t lastPage = static_cast<uint32_t>(
                (static_cast<uint64_t>(it->start) + it->length - 1) >> kPageShift);
            for (uint32_t page = firstPage; page <= lastPage; ++page) affectedPages.push_back(page);
            matches.push_back(*it);
            // Idempotent: the vectored handler already dropped the protection
            // for the range whose trap brought us here.
            GuestFlat::UnprotectDeferredRange(it->start, it->length);
            it = reads.erase(it);
        }
        std::sort(affectedPages.begin(), affectedPages.end());
        affectedPages.erase(std::unique(affectedPages.begin(), affectedPages.end()), affectedPages.end());
        for (uint32_t page : affectedPages) RefreshDeferredReadPage(page);
    }

    // The range is removed before entering renderer code so any memory reads
    // made while submitting the copy cannot recursively trigger it.
    for (const auto& match : matches) {
        if (!match.callback(match.user)) {
            throw Memory::AccessViolation(addr, length, "deferred read materialization failed");
        }
    }
    return true;
}

namespace {

void BuildPageTable() {
    auto* table = MemoryInline::g_pageTable;
    for (const auto& region : Regions()) {
        const uint64_t base = region.config.baseAddress;
        const uint64_t size = region.storageSize;
        const uint64_t end = base + size;
        if (size == 0) {
            continue;
        }

        const uint32_t startPage = static_cast<uint32_t>(base >> MemoryInline::kPageShift);
        const uint32_t endPage = static_cast<uint32_t>((end - 1) >> MemoryInline::kPageShift);
        for (uint32_t page = startPage; page <= endPage; ++page) {
            const uint64_t pageBase = static_cast<uint64_t>(page) << MemoryInline::kPageShift;
            const uint64_t offset = pageBase - base;
            if (offset >= size) {
                continue;
            }
            const uint32_t limit = static_cast<uint32_t>(std::min<uint64_t>(MemoryInline::kPageSize, size - offset));
            table[page].base = region.storagePtr + static_cast<size_t>(offset);
            table[page].limit = limit;
        }
    }

    // The last page of each contiguous mapping remains on the checked path so
    // an access straddling its end cannot escape the mapped region. Every
    // preceding full page can safely service native accesses up to 8 bytes,
    // including a cross-page access into its contiguous successor.
    for (uint32_t page = 0; page + 1 < MemoryInline::kPageCount; ++page) {
        const auto& current = table[page];
        const auto& next = table[page + 1];
        if (!current.base || current.limit != MemoryInline::kPageSize ||
            !next.base || next.limit < MemoryInline::kMaxFastScalarSize - 1u ||
            next.base != current.base + MemoryInline::kPageSize)
            continue;
        const uintptr_t guestPageBase = static_cast<uintptr_t>(page) << MemoryInline::kPageShift;
        const uintptr_t bias = reinterpret_cast<uintptr_t>(current.base) - guestPageBase;
        MemoryInline::g_fullPageBias[page] = bias + 1u;
    }
}

void RefreshFastPathTables() {
    auto& sparseStorage = SparseWritablePageTableStorage();
    for (uint32_t page = 0; page < MemoryInline::kPageCount; ++page) {
        const uintptr_t bias = MemoryInline::g_fullPageBias[page];
        MemoryInline::g_fullReadablePageBias[page] = bias;
        MemoryInline::g_deferredReadCoveredPages[page] = 0;
        MemoryInline::g_sparseWritablePageTables[page] = nullptr;
        sparseStorage[page].reset();

        const bool coarseExecutable =
            RecompMod::g_executableWriteGuardCoarsePages[page].load(
                std::memory_order_relaxed) != 0;
        MemoryInline::g_fullWritablePageBias[page] = coarseExecutable ? 0 : bias;
        if (!coarseExecutable || bias == 0)
            continue;

        auto sparse = std::make_unique<MemoryInline::SparseWritablePageTable>();
        bool hasExecutableSubPage = false;
        bool hasWritableSubPage = false;
        const uint32_t firstExactPage =
            page * MemoryInline::kWritableSubPagesPerPage;
        for (uint32_t subPage = 0;
             subPage < MemoryInline::kWritableSubPagesPerPage; ++subPage) {
            const bool executable =
                RecompMod::g_executableWriteGuardPages[firstExactPage + subPage].load(
                    std::memory_order_relaxed) != 0;
            hasExecutableSubPage |= executable;
            hasWritableSubPage |= !executable;
            sparse->encodedBias[subPage] = executable ? 0 : bias;
        }

        // A homogeneous executable page has no writable fast path; a
        // homogeneous data page already uses g_fullWritablePageBias. Retain an
        // allocation only for the intended mixed case.
        if (hasExecutableSubPage && hasWritableSubPage) {
            MemoryInline::g_sparseWritablePageTables[page] = sparse.get();
            sparseStorage[page] = std::move(sparse);
        }
    }
    std::lock_guard lock{DeferredReadMutex()};
    for (const auto& read : DeferredReads()) {
        const uint32_t firstPage = read.start >> MemoryInline::kPageShift;
        const uint32_t lastPage = static_cast<uint32_t>(
            (static_cast<uint64_t>(read.start) + read.length - 1) >> MemoryInline::kPageShift);
        for (uint32_t page = firstPage; page <= lastPage; ++page) {
            MemoryInline::g_fullReadablePageBias[page] = 0;
            MemoryInline::g_deferredReadCoveredPages[page] = 1;
        }
    }
}

Region* TryResolveRegion(uint32_t address, size_t length) {
    auto& regions = Regions();
    for (auto& region : regions) {
        const uint64_t base = region.config.baseAddress;
        const uint64_t limit = base + region.storageSize;
        const uint64_t addr = address;
        const uint64_t end = addr + length;
        if (addr >= base && end <= limit) {
            return &region;
        }
    }
    return nullptr;
}

Region& ResolveRegion(uint32_t address, size_t length) {
    if (Region* region = TryResolveRegion(address, length)) {
        return *region;
    }
    // Emit extra context to help diagnose early-boot accesses that miss the map.
    if (auto* cpu = TryGetCpuContext()) {
        const uint32_t active = RecompMod::CurrentTranslatedExecutionAddress();
        RT_LOG(RT_TAG_MEMORY) << "AccessViolation ctx=" << cpu
                  << " pc=0x" << std::hex << cpu->pc
                  << " active=0x" << active
                  << " lr=0x" << cpu->lr
                  << " r1=0x" << cpu->gpr[1]
                  << " r8=0x" << cpu->gpr[8]
                  << " r9=0x" << cpu->gpr[9]
                  << " r12=0x" << cpu->gpr[12]
                  << " r30=0x" << cpu->gpr[30]
                  << " r31=0x" << cpu->gpr[31]
                  << std::dec
                  << " addr=0x" << std::hex << address
                  << " len=" << length << std::dec
                  << " reason=no mapped region" << std::endl;
    } else {
        RT_LOG(RT_TAG_MEMORY) << "AccessViolation ctx=(null) addr=0x" << std::hex << address
                  << " len=" << length << std::dec << " reason=no mapped region" << std::endl;
    }
    RT_LOG(RT_TAG_MEMORY) << "===== DUMPING CPU STATE =====" << std::endl;
    SystemBridge::DumpCpuState(TryGetCpuContext());
    // Hexdump guest memory around pointer-carrying registers so a corrupted
    // structure's surroundings (e.g. ASCII sprayed over a link pointer) are
    // visible in the report without a debugger attached.
    if (auto* cpu = TryGetCpuContext()) {
        for (const int reg : {4, 5, 6, 7, 8, 26, 27, 28, 29, 30, 31}) {
            const uint32_t base = cpu->gpr[reg];
            if (base < 0x80000000u || base >= 0x94000000u)
                continue;
            const uint32_t start = (base - 0x40u) & ~0xFu;
            RT_LOG(RT_TAG_MEMORY) << "hexdump around r" << reg << "=0x" << std::hex << base << ":" << std::endl;
            for (uint32_t row = 0; row < 16; ++row) {
                const uint32_t rowAddr = start + row * 16u;
                RT_LOG(RT_TAG_MEMORY) << "  0x" << std::hex << rowAddr << ":";
                char ascii[17] = {};
                for (uint32_t i = 0; i < 16; ++i) {
                    uint8_t byte = 0;
                    if (!MemoryInline::TryReadGuestScalar(rowAddr + i, byte)) {
                        std::cerr << " ??";
                        ascii[i] = '?';
                        continue;
                    }
                    std::cerr << " " << std::setw(2) << std::setfill('0') << static_cast<uint32_t>(byte);
                    ascii[i] = (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
                }
                std::cerr << "  |" << ascii << "|" << std::dec << std::setfill(' ') << std::endl;
            }
        }
    }
#if defined(_WIN32)
    void* frames[32]{};
    const USHORT captured = CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    const auto imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    HANDLE process = GetCurrentProcess();
    static const bool symbolsReady = [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        if (SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE)
            return true;
        // The runtime crash reporter may already own the process-wide DbgHelp
        // session. Reuse it rather than treating ERROR_INVALID_PARAMETER as
        // symbol unavailability.
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }();
    RT_LOG(RT_TAG_MEMORY) << "host stack at first invalid guest access:" << std::endl;
    for (USHORT index = 0; index < captured; ++index) {
        const auto addressValue = reinterpret_cast<uintptr_t>(frames[index]);
        RT_LOG(RT_TAG_MEMORY) << "  #" << index << " absolute=0x" << std::hex << addressValue;
        if (imageBase != 0 && addressValue >= imageBase)
            std::cerr << " image+0x" << (addressValue - imageBase);
        std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolBuffer{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (symbolsReady && SymFromAddr(process, addressValue, &displacement, symbol))
            std::cerr << " " << symbol->Name << "+0x" << displacement;
        std::cerr << std::dec << std::endl;
    }
#endif
    std::cerr.flush();
    throw Memory::AccessViolation(address, length, "no mapped region");
}

GuestFlat::Backing ClassifyBacking(uint32_t baseAddress) {
    // MEM1: physical (0x00000000), cached (0x80000000), uncached (0xC0000000)
    if ((baseAddress >= Memory::kMem1PhysicalBase &&
         baseAddress < Memory::kMem1PhysicalBase + Memory::kMem1Size) ||
        (baseAddress >= Memory::kMem1CachedBase &&
         baseAddress < Memory::kMem1CachedBase + Memory::kMem1Size) ||
        (baseAddress >= Memory::kMem1UncachedBase &&
         baseAddress < Memory::kMem1UncachedBase + Memory::kMem1Size)) {
        return GuestFlat::Backing::Mem1;
    }

    // NDEV-sized MEM2: physical (0x10000000), cached (0x90000000),
    // uncached (0xD0000000). Mario Kart Wii detects this configuration and
    // creates its original EGGRootDebug expansion heap.
    if ((baseAddress >= Memory::kMem2PhysicalBase &&
         baseAddress < Memory::kMem2PhysicalEnd) ||
        (baseAddress >= Memory::kMem2CachedBase &&
         baseAddress < Memory::kMem2CachedEnd) ||
        (baseAddress >= Memory::kMem2UncachedBase &&
         baseAddress < Memory::kMem2UncachedEnd)) {
        return GuestFlat::Backing::Mem2;
    }

    return GuestFlat::Backing::Owned;
}


template <typename T>
T ReadScalar(uint32_t address) {
    // This is the cold *Slow path; Memory::GetPointer already tries the fast
    // probe first, so repeating it here only guaranteed a second miss.
    auto* ptr = Memory::GetPointer(address, sizeof(T));
    if constexpr (sizeof(T) == 1) {
        return *ptr;
    } else {
        T value = 0;
        std::memcpy(&value, ptr, sizeof(T));
        return MemoryInline::MaybeByteSwap(value);
    }
}

template <typename T>
void WriteScalar(uint32_t address, T value) {
    if (RecompMod::HandleExecutableWrite(address, sizeof(T), static_cast<uint64_t>(value))) {
        return;
    }

    auto* ptr = Memory::GetPointer(address, sizeof(T));
    if constexpr (sizeof(T) == 1) {
        *ptr = static_cast<uint8_t>(value);
    } else {
        const T swapped = MemoryInline::MaybeByteSwap(value);
        std::memcpy(ptr, &swapped, sizeof(T));
    }
}

} // namespace

Memory::AccessViolation::AccessViolation(uint32_t address, size_t length, std::string_view reason)
    : std::runtime_error([&]() {
          std::ostringstream oss;
          oss << "Memory access violation at 0x" << std::hex << std::uppercase << address
              << " (+0x" << length << ") :: " << reason;
          return oss.str();
      }()),
      address_(address),
      length_(length),
      reason_(reason) {}

void Memory::RefreshWritableFastPathsForExecutableRanges() {
    std::lock_guard<std::mutex> lock(RegionMutex());
    RefreshFastPathTables();
}

Memory::Config Memory::Config::WiiDefaults() {
    // Physical / cached / uncached views of MEM1 and MEM2, then the locked cache.
    Config config;

    config.regions.push_back(RegionConfig{
        .name = "MEM1_PHYS",
        .baseAddress = Memory::kMem1PhysicalBase,
        .sizeBytes = Memory::kMem1Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM1",
        .baseAddress = Memory::kMem1CachedBase,
        .sizeBytes = Memory::kMem1Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM1_UNCACHED",
        .baseAddress = Memory::kMem1UncachedBase,
        .sizeBytes = Memory::kMem1Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM2_PHYS",
        .baseAddress = Memory::kMem2PhysicalBase,
        .sizeBytes = Memory::kMem2Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM2",
        .baseAddress = Memory::kMem2CachedBase,
        .sizeBytes = Memory::kMem2Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM2_UNCACHED",
        .baseAddress = Memory::kMem2UncachedBase,
        .sizeBytes = Memory::kMem2Size,
    });

    // Kamek module overlay: 2 MiB above MEM1, which the game believes ends at 0x81800000, so this
    // costs no arena space (unlike the old in-arena reservation that shrank the race scene heaps).
    // Base must stay within +/-32 MiB of every DOL/StaticR hook site for Kamek Rel24 branches to encode.
    config.regions.push_back(RegionConfig{
        .name = "MEM1_KAMEK_OVERLAY",
        .baseAddress = 0x81800000,
        .sizeBytes = 0x200000,
    });

    // Locked cache (THP decoder fast RAM) is really 16KB at 0xE0000000, but a sub-page mapping can
    // never enter the coarse 1MiB bias tables, forcing every THP load/store through the checked
    // fallback (the dominant cost of THP-heavy screens). Back a full 1MiB page plus the successor
    // page the bias builder needs, so locked-cache access stays on the two-instruction native path.
    constexpr size_t lcSize = MemoryInline::kPageSize + 4096u;
    config.regions.push_back(RegionConfig{
        .name = "LOCKED_CACHE",
        .baseAddress = 0xE0000000,
        .sizeBytes = lcSize,
    });

    return config;
}

void Memory::Init(size_t mem1Size) {
    Config config;
    config.regions.push_back(RegionConfig{.name = "MEM1", .baseAddress = 0x80000000, .sizeBytes = mem1Size});
    Init(config);
}

void Memory::Init(const Config& config) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    PrimePiInterruptMask().store(0, std::memory_order_release);
    PrimePiStatusRegister().store(0, std::memory_order_release);
    PrimeSystemInterruptEnable().store(0, std::memory_order_release);
    PrimeMemoryInterruptMask().store(0, std::memory_order_release);
    PrimeMemoryInterruptClear().store(0, std::memory_order_release);
    PrimeInterruptSourceMask().store(0, std::memory_order_release);
    g_primeDspControl = {};
    g_primeCpuFifoCapture = {};
    for (uint32_t address = 0xCC004032u; address <= 0xCC004058u; address += 2u)
        PrimeMiTimerRegister(address).store(0);
    PrimeDspAramInfo().store(0, std::memory_order_release);
    { std::lock_guard<std::mutex> lock(g_primeDspMutex); g_primeDspRom = {}; }
    for (uint32_t addr = 0xCC005020u; addr <= 0xCC005028u; addr += 4u) {
        PrimeDspDmaRegister(addr).store(0, std::memory_order_release);
    }
    Audio_HLE_PrimeDspDmaReset();
    PrimeSiSamplingControl().store(0, std::memory_order_release);
    for (const uint32_t addr : {0xCD800180u, 0xCD8001CCu, 0xCD8001D0u}) {
        PrimeWirelessPowerRegister(addr).store(0, std::memory_order_release);
    }
    PrimeSiTransferControl().store(0, std::memory_order_release);
    for (uint32_t addr = 0xCD006480u; addr <= 0xCD00648Cu; addr += 4u) {
        PrimeSiChannelData(addr).store(0, std::memory_order_release);
    }
    for (uint32_t addr = 0xCD006800u; addr <= 0xCD006834u; addr += 4u) {
        PrimeInterruptUnitRegister(addr).store(0, std::memory_order_release);
    }
    { std::lock_guard<std::mutex> lock(g_primeAiMutex); g_primeAi = {}; g_primeAi.lastNanos = PrimeAiNow(); }
#endif
    std::lock_guard<std::mutex> lock(RegionMutex());
    auto& regions = Regions();
    regions.clear();
    ClearPageTable();
    regions.reserve(config.regions.size());

    // The flat reservation backs every guest view from one shared section object, so cached/uncached/
    // physical mirrors alias as before. GetPointer still hands out the unprotected host alias, so
    // native code (image loading, DVD reads, HLE) is unaffected by the guest view's protections.
    {
        std::vector<GuestFlat::RegionRequest> flatRegions;
        flatRegions.reserve(config.regions.size());
        for (const auto& regionConfig : config.regions) {
            flatRegions.push_back(GuestFlat::RegionRequest{
                regionConfig.baseAddress, regionConfig.sizeBytes,
                ClassifyBacking(regionConfig.baseAddress)});
        }
        GuestFlat::Initialize(flatRegions);
    }

    // Map virtual regions onto that backing store, with mirroring.
    for (const auto& regionConfig : config.regions) {
        Region instance;
        instance.config = regionConfig;

        instance.storagePtr = GuestFlat::HostPointer(regionConfig.baseAddress);
        if (instance.storagePtr == nullptr && regionConfig.sizeBytes != 0) {
            throw std::runtime_error("Flat guest mapping is missing region '" + regionConfig.name + "'");
        }
        instance.storageSize = regionConfig.sizeBytes;

        regions.emplace_back(std::move(instance));
    }

    BuildPageTable();
    RefreshFastPathTables();
}

void MemoryPrimeDspRaiseAIDInterrupt() {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    std::lock_guard<std::mutex> lock(g_primeDspMutex);
    g_primeDspControl.RaiseAIDInterrupt();
#endif
}

void Memory::Reset() {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    Audio_HLE_PrimeDspDmaReset();
#endif
    ClearDeferredReads();
    std::lock_guard<std::mutex> lock(RegionMutex());
    Regions().clear();
    ClearPageTable();
}

namespace {
// MMIO reads have no backing store or generic HLE. Answering zero (the old sparse fallback) turned
// unimplemented devices into silent hangs, so unmapped reads throw like MMIO writes; every device
// we do implement is hooked at the function level, so a hit here means a missing HLE hook.
[[noreturn]] void ThrowMmioReadBlocked(uint32_t addr, size_t length) {
    std::ostringstream reason;
    reason << (MemoryInline::IsGpuFifoAddress(addr)
                   ? "GPU FIFO read blocked (the gather pipe is write-only)"
                   : "MMIO read blocked (non-GPU)")
           << "; add HLE for this device instead of answering zero"
           << " (active=0x" << std::hex << std::uppercase
           << RecompMod::CurrentTranslatedExecutionAddress() << std::dec << std::nouppercase << ")";
    if (const CpuContext* cpu = TryGetCpuContext()) {
        reason << " lr=0x" << std::hex << cpu->lr << " r1=0x" << cpu->gpr[1];
    }
    throw Memory::AccessViolation(addr, length, reason.str());
}
} // namespace

extern "C" bool PrimeGpuCpReadBlocked() {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    return (PrimeCpRegisters()[0].load() & 0x10u) != 0 &&
        (PrimeCpRegisters()[1].load() & 2u) != 0;
#else
    return false;
#endif
}

extern "C" bool PrimeGpuTryCaptureCpuFifo(uint32_t value, unsigned width) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    if (!PrimeCpuFifoCaptureActive()) return false;
    const uint32_t base = PrimePiFifoRegister(0xCC00300Cu).load();
    const uint32_t end = PrimePiFifoRegister(0xCC003010u).load();
    const uint32_t pointer = PrimePiFifoRegister(0xCC003014u).load();
    const bool linked = (PrimeCpRegisters()[1].load() & 0x10u) != 0;
    std::array<uint8_t, 32> completed{};
    bool committed = false;
    const uint32_t next = g_primeCpuFifoCapture.Push(base, end, pointer, value, width,
        [&](uint32_t address, const uint8_t* bytes, size_t size) {
            if (!Memory::Contains(address, size))
                throw Memory::AccessViolation(address, size, "CPU FIFO recording outside RAM");
            std::memcpy(Memory::GetPointer(address, size), bytes, size);
            std::memcpy(completed.data(), bytes, size);
            committed = true;
            static unsigned traces = 0;
            static unsigned linkedTraces = 0;
            if ((!linked && traces++ < 48) || (linked && linkedTraces++ < 16))
                RT_LOG(RT_TAG_GX) << "[prime-cpu-fifo-record] address=0x" << std::hex
                    << address << " base=0x" << base << " end=0x" << end << " linked=" << linked
                    << " word0=0x" << BigEndian::Read32(bytes) << std::dec << std::endl;
        });
    PrimePiFifoRegister(0xCC003014u).store(next);
    if (committed && linked) {
        PrimeCpSetPair(0x34, next & ~0x20000000u);
        GX_HLE_PrimeConsumeGatherBurst(completed.data());
    }
    return true;
#else
    return false;
#endif
}

extern "C" bool PrimeGpuCpInterruptPending() {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    PrimeCpUpdateStatus();
    return PrimeCpFifo::InterruptPending(PrimeCpRegisters()[1].load(), PrimeCpRegisters()[0].load());
#else
    return false;
#endif
}

uint8_t MemoryInline::Read8Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(uint8_t));
    }
    ResolveDeferredReads(addr, sizeof(uint8_t));
    return ReadScalar<uint8_t>(addr);
}

uint16_t MemoryInline::Read16Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        uint16_t value = 0;
        if (ReadPrimeMmio16(addr, value)) return value;
#endif
        ThrowMmioReadBlocked(addr, sizeof(uint16_t));
    }
    ResolveDeferredReads(addr, sizeof(uint16_t));
    return ReadScalar<uint16_t>(addr);
}

uint32_t MemoryInline::Read32Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        uint32_t value = 0;
        if (ReadPrimePiMmio32(addr, value)) return value;
#endif
        ThrowMmioReadBlocked(addr, sizeof(uint32_t));
    }
    ResolveDeferredReads(addr, sizeof(uint32_t));
    const uint32_t value = ReadScalar<uint32_t>(addr);
    return value;
}

uint64_t MemoryInline::Read64Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(uint64_t));
    }
    ResolveDeferredReads(addr, sizeof(uint64_t));
    return ReadScalar<uint64_t>(addr);
}

float MemoryInline::ReadFloat32Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(float));
    }
    ResolveDeferredReads(addr, sizeof(uint32_t));
    const auto bits = ReadScalar<uint32_t>(addr);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double MemoryInline::ReadFloat64Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        ThrowMmioReadBlocked(addr, sizeof(double));
    }
    ResolveDeferredReads(addr, sizeof(uint64_t));
    const auto bits = ReadScalar<uint64_t>(addr);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void MemoryInline::Write8Slow(uint32_t addr, uint8_t val) {
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write8(val);
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::Write16Slow(uint32_t addr, uint16_t val) {
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write16(val);
        return;
    }
    if (IsMmioAddress(addr)) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        if (WritePrimeMmio16(addr, val)) return;
#endif
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::Write32Slow(uint32_t addr, uint32_t val) {
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write32(val);
        return;
    }
    if (IsMmioAddress(addr)) {
#if defined(MKW_GAME_PROFILE_RM3E01_00)
        if (WritePrimePiMmio32(addr, val)) return;
#endif
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::Write64Slow(uint32_t addr, uint64_t val) {
    if (IsGpuFifoAddress(addr)) {
        // The gather pipe is a byte-stream FIFO; a 64-bit store is two big-endian
        // 32-bit pushes, high word first. Without this arm the write fell through
        // to WriteScalar, which found no mapped region behind 0xCC008000 and threw
        // "no mapped region" instead of reaching GX.
        GX_HLE_FIFO_Write32(static_cast<uint32_t>(val >> 32));
        GX_HLE_FIFO_Write32(static_cast<uint32_t>(val));
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::WriteFloat32Slow(uint32_t addr, double val) {
    // stfs stores the IEEE-754 bit pattern of the single-precision value, never a
    // truncated integer.
    const uint32_t bits = ConvertPpcDoubleToSingleBits(val);
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_WriteFloat(PpcSingleBitsToFloat(bits));
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(float), "MMIO write blocked (non-GPU)");
    }

    WriteScalar(addr, bits);
}

void MemoryInline::WriteFloat64Slow(uint32_t addr, double val) {
    // stfd stores the full 64-bit FPR bit pattern - load-bearing for the
    // fctiwz->stfd->lwz idiom, where the integer result lives in the low word.
    uint64_t bits;
    std::memcpy(&bits, &val, sizeof(bits));

    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write32(static_cast<uint32_t>(bits >> 32));
        GX_HLE_FIFO_Write32(static_cast<uint32_t>(bits));
        return;
    }
    if (IsMmioAddress(addr)) {
        throw Memory::AccessViolation(addr, sizeof(double), "MMIO write blocked (non-GPU)");
    }

    WriteScalar(addr, bits);
}

uint8_t* Memory::GetPointer(uint32_t addr) {
    return GetPointer(addr, 1);
}

uint8_t* Memory::GetPointer(uint32_t addr, size_t length) {
    if (auto* ptr = MemoryInline::GetPointerFast(addr, length)) {
        return ptr;
    }
    auto& region = ResolveRegion(addr, length);
    auto offset = static_cast<size_t>(addr - region.config.baseAddress);
    return region.storagePtr + offset;
}

bool Memory::Contains(uint32_t addr, size_t length) {
    if (MemoryInline::GetPointerFast(addr, length)) {
        return true;
    }
    // Contains is a membership predicate: callers probe arbitrary guest values
    // (e.g. gfx-node slots that may hold floats), so a miss must stay silent
    // rather than take ResolveRegion's crash-dump path.
    return TryResolveRegion(addr, length) != nullptr;
}

std::vector<Memory::RegionConfig> Memory::DescribeRegions() {
    std::lock_guard<std::mutex> lock(RegionMutex());
    std::vector<RegionConfig> info;
    info.reserve(Regions().size());
    for (const auto& region : Regions()) {
        info.push_back(RegionConfig{region.config.name, region.config.baseAddress, region.storageSize});
    }
    return info;
}
