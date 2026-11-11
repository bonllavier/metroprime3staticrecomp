#pragma once
#include <cstdint>

// Optional dedicated GX consumer thread (WII_COMPILED_PRIME_GPU_THREAD=1).
//
// The guest keeps writing the gather pipe on its own thread; every committed
// 32-byte burst is queued instead of decoded inline, and a host thread runs
// the FIFO decoder, display-list interpreter and Aurora submission. This is
// the same CPU/GP overlap the console has: the guest already synchronizes
// with the GP through PE tokens and CP status, which this thread publishes
// as it consumes the stream in order.
//
// Rules:
//  * The consumer never runs guest code or pumps window events.
//  * One "GX lock" serializes all renderer state. The consumer holds it while
//    it processes queued bursts; host code on the guest thread that touches
//    Aurora/GX state (VI presentation, overlay, window events) takes Lock.
//  * Guest MMIO that depends on FIFO order (CP registers, PI FIFO reset) takes
//    Sync, which also decodes every queued burst inline before returning.
// With the variable unset Lock/Sync do nothing and SubmitBurst decodes the
// stream synchronously exactly as before.
namespace GxGpuThread {
bool Enabled();
bool OnGpuThread();
// OS thread id of the consumer, 0 until it has started (diagnostics).
uint32_t NativeThreadId();
// Queue one committed gather burst (guest order). Decodes inline if disabled.
void SubmitBurst(const uint8_t* bytes32);
// The CP breakpoint read stall is decided when a burst is submitted (as the
// synchronous decoder did); stalled bursts wait here until the guest resumes
// GP reads. True while the calling thread decodes a queued entry, which must
// not re-test the stall against newer CP state.
bool DecodingQueued();
void ResumeRetained();
void DiscardRetained();
// Ordered EFB-copy invalidation for a guest RAM write notification.
// Returns false when the caller should apply it immediately itself.
bool QueueInvalidate(uint32_t addr, uint32_t size);
// True when nothing is queued and the consumer is not decoding.
bool Idle();
// Diagnostics: the guest thread reported a breakpoint the consumer reached.
void NoteBreakpointReported();
void Shutdown();

class Lock {
public:
    Lock();
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
private:
    bool held_ = false;
    bool upgraded_ = false;  // first full Lock inside a HostLock: drained here
};

// Host work that touches neither GX nor decoder state (window events, input,
// ImGui input): excludes the consumer and waits for the frame worker's SEALED
// phase like Lock, but does not drain the asynchronous decoder. Lock drained it
// at every retrace, ~3 % of the game thread waiting (F8 2026-10-31 00:07). A
// Lock taken inside it upgrades and drains as usual.
class HostLock {
public:
    HostLock();
    ~HostLock();
    HostLock(const HostLock&) = delete;
    HostLock& operator=(const HostLock&) = delete;
private:
    bool held_ = false;
};

// Lock plus inline decoding of everything queued before this point.
class Sync {
public:
    Sync();
    Sync(const Sync&) = delete;
    Sync& operator=(const Sync&) = delete;
private:
    Lock lock_;
};
}  // namespace GxGpuThread
