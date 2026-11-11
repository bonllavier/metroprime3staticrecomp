#include "host_threads.h"
#include "hle/gx_gpu_thread.h"

#include "runtime_log.h"
#include "hle/prime_input_latency.h"

#include <aurora/aurora.h>
#include <aurora/host_cores.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
#include <immintrin.h>

extern "C" void GX_HLE_PrimeConsumeGatherBurst(const uint8_t* bytes);
extern "C" bool PrimeGpuCpReadBlocked();
extern "C" bool GX_HLE_PrimeInsideDraw();
void InvalidateEfbCopyDestinationsForRangeNow(uint32_t addr, uint32_t size);

namespace GxGpuThread {
namespace {
enum class Kind : uint32_t { Burst, Invalidate };
// One cache line per entry (44 bytes of payload): the producer never writes into a line the
// consumer is still reading when it runs right behind.
struct alignas(64) Entry {
    Kind kind;
    uint32_t a, b;
    uint8_t data[32];
};

// ~1.3 MiB of queued stream. A full queue stalls the producer, which bounds
// how far the guest may run ahead of the decoder (PE tokens bound it anyway).
constexpr uint64_t kCapacity = 1u << 15;

// The game thread pushes ~150-350 thousand entries a second and the consumer retires them on
// another core: what each side writes per entry lives on its own cache line, so the line holding
// `head` is not pulled back and forth on every push (it was, sharing a line with `tail`).
struct State {
    std::unique_ptr<Entry[]> ring{new Entry[kCapacity]};
    // Consumer side: written for every decoded entry.
    alignas(64) std::atomic<uint64_t> head{0};  // next entry to decode; advanced under gxMutex
    // Producer side: written for every pushed entry, under pushLock.
    alignas(64) std::atomic<uint64_t> tail{0};  // next free slot; advanced under pushLock
    uint64_t cachedHead = 0;  // last head the producer saw; reloaded only when the ring looks full
    std::atomic_flag pushLock = ATOMIC_FLAG_INIT;
    alignas(64) std::mutex gxMutex;
    std::atomic<int> hostWaiters{0};
    std::atomic<bool> busy{false};
    // Read on every push, written only when the consumer parks: kept away from the per-batch fields.
    alignas(64) std::atomic<bool> sleeping{false};
    alignas(64) std::mutex wakeMutex;
    std::condition_variable wakeCv;
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::exception_ptr failure;
    std::thread thread;
    std::once_flag started;
    // Diagnostics, reported every 10 s. The first line is written by the producer on every push.
    alignas(64) std::atomic<uint64_t> bursts{0}, maxDepth{0}, inlineDecoded{0}, fullStalls{0};
    alignas(64) std::atomic<uint64_t> syncs{0}, locks{0}, lockWaitNs{0};
    std::atomic<uint64_t> idleNs{0}, consumerLockWaitNs{0}, breakpointLatencyNs{0}, breakpoints{0};
    std::atomic<int64_t> idleSinceNs{0};  // 0 while work is pending or being decoded
};

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

State& S() {
    static State* state = new State;  // intentionally leaked: outlives static destructors
    return *state;
}

thread_local bool t_onGpuThread = false;
thread_local int t_lockDepth = 0;
// The outermost hold of this thread is a HostLock and no Lock inside it has
// drained the decoder yet.
thread_local bool t_holdUndrained = false;
thread_local int t_decodingQueued = 0;

// Bursts committed while the CP breakpoint stalls GP reads (guest thread only).
std::vector<uint8_t> g_retained;

void DecodeOne() {
    auto& s = S();
    const uint64_t index = s.head.load(std::memory_order_relaxed);
    // Copy and retire before decoding: decoding may re-enter Push and, on the
    // guest thread, decode the next entries inline.
    const Entry entry = s.ring[index % kCapacity];
    s.head.store(index + 1, std::memory_order_release);
    struct Scope {
        Scope() { ++t_decodingQueued; }
        ~Scope() { --t_decodingQueued; }
    } scope;
    if (entry.kind == Kind::Burst) {
        PrimeInputLatency::SetDecodingSubmitTime(int64_t(uint64_t(entry.a) | (uint64_t(entry.b) << 32)));
        GX_HLE_PrimeConsumeGatherBurst(entry.data);
        PrimeInputLatency::SetDecodingSubmitTime(0);
    } else {
        InvalidateEfbCopyDestinationsForRangeNow(entry.a, entry.b);
    }
}

bool QueueEmpty() {
    auto& s = S();
    return s.head.load(std::memory_order_acquire) == s.tail.load(std::memory_order_acquire);
}

void RethrowIfFailed() {
    auto& s = S();
    if (s.failed.load(std::memory_order_acquire)) {
        s.failed.store(false, std::memory_order_relaxed);
        std::rethrow_exception(s.failure);
    }
}

void Report() {
#if !defined(MKW_PRIME_QA_TOOLS)
    return;
#endif
    auto& s = S();
    static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    next = now + std::chrono::seconds(10);
    const uint64_t breakpoints = s.breakpoints.exchange(0);
    const uint64_t breakpointNs = s.breakpointLatencyNs.exchange(0);
    RT_LOG(RT_TAG_GX) << "[prime-gpu-thread] bursts=" << s.bursts.exchange(0)
        << " syncs=" << s.syncs.exchange(0) << " host_locks=" << s.locks.exchange(0)
        << " host_lock_wait_ms=" << s.lockWaitNs.exchange(0) / 1000000
        << " inline_decoded=" << s.inlineDecoded.exchange(0)
        << " full_stalls=" << s.fullStalls.exchange(0)
        << " max_depth=" << s.maxDepth.exchange(0)
        << " consumer_idle_ms=" << s.idleNs.exchange(0) / 1000000
        << " consumer_lock_wait_ms=" << s.consumerLockWaitNs.exchange(0) / 1000000
        << " breakpoints=" << breakpoints
        << " breakpoint_notify_avg_us=" << (breakpoints ? breakpointNs / breakpoints / 1000 : 0) << std::endl;
}
std::atomic<uint32_t> g_nativeThreadId{0};

void Main() {
    t_onGpuThread = true;
    aurora_set_thread_event_pump(false);
#if defined(_WIN32)
    SetThreadDescription(GetCurrentThread(), L"Prime GX consumer");
    // It runs beside the game thread all frame: never on the game's core (aurora/host_cores.h).
    aurora::host_cores::keep_off_game_core();
    HostThreads::SetCurrentRole(HostThreads::Role::Gx, nullptr);
    g_nativeThreadId.store(GetCurrentThreadId(), std::memory_order_release);
#endif
    auto& s = S();
    while (!s.stop.load(std::memory_order_acquire)) {
        if (QueueEmpty()) {
#if defined(MKW_PRIME_QA_TOOLS)
            const int64_t idleStart = NowNs();
            s.idleSinceNs.store(idleStart, std::memory_order_release);
#endif
            // Spin briefly: bursts usually arrive in quick succession.
            bool arrived = false;
            for (int i = 0; i < 4000 && !arrived; ++i) {
                _mm_pause();
                arrived = !QueueEmpty();
            }
            if (!arrived) {
                std::unique_lock lock(s.wakeMutex);
                s.sleeping.store(true, std::memory_order_seq_cst);
                s.wakeCv.wait_for(lock, std::chrono::milliseconds(2), [&] {
                    return !QueueEmpty() || s.stop.load(std::memory_order_acquire);
                });
                s.sleeping.store(false, std::memory_order_relaxed);
            }
#if defined(MKW_PRIME_QA_TOOLS)
            if (!QueueEmpty()) s.idleSinceNs.store(0, std::memory_order_release);
            s.idleNs.fetch_add(uint64_t(NowNs() - idleStart), std::memory_order_relaxed);
#endif
            continue;
        }
        // Let a waiting host section in first; the mutex is not fair.
#if defined(MKW_PRIME_QA_TOOLS)
        const int64_t lockStart = NowNs();
#endif
        while (s.hostWaiters.load(std::memory_order_acquire) > 0) std::this_thread::yield();
        std::unique_lock lock(s.gxMutex);
#if defined(MKW_PRIME_QA_TOOLS)
        s.consumerLockWaitNs.fetch_add(uint64_t(NowNs() - lockStart), std::memory_order_relaxed);
#endif
        s.busy.store(true, std::memory_order_release);
#if defined(MKW_PRIME_QA_TOOLS)
        s.idleSinceNs.store(0, std::memory_order_release);
#endif
        try {
            // Yield to a waiting host section only between draws.
            // `tail` is read once per run of entries, not once per entry (the producer writes its line
            // on every push). head only moves under gxMutex, which this thread holds.
            uint64_t available = s.tail.load(std::memory_order_acquire);
            for (;;) {
                if (s.head.load(std::memory_order_relaxed) == available) {
                    available = s.tail.load(std::memory_order_acquire);
                    if (s.head.load(std::memory_order_relaxed) == available) break;
                }
                if (s.hostWaiters.load(std::memory_order_acquire) != 0 && !GX_HLE_PrimeInsideDraw()) break;
                DecodeOne();
            }
            // Nothing more to interpret for now: give the decoder thread
            // whatever was written, however small (a guest waiting on a PE
            // token or draw-done produces no further bursts). Under the GX
            // lock: the stream buffer belongs to whoever holds it.
            if (QueueEmpty() && !GX_HLE_PrimeInsideDraw()) aurora_stream_flush();
        } catch (...) {
            s.failure = std::current_exception();
            s.failed.store(true, std::memory_order_release);
            s.busy.store(false, std::memory_order_release);
            RT_LOG(RT_TAG_GX) << "[prime-gpu-thread] consumer failed; error rethrown on guest thread" << std::endl;
            return;
        }
        s.busy.store(false, std::memory_order_release);
    }
}

void EnsureStarted() {
    auto& s = S();
    std::call_once(s.started, [&] {
        s.thread = std::thread(Main);
        RT_LOG(RT_TAG_GX) << "[prime-gpu-thread] started dedicated GX consumer" << std::endl;
    });
}

void Push(const Entry& entry) {
    auto& s = S();
    RethrowIfFailed();
    EnsureStarted();
    for (;;) {
        while (s.pushLock.test_and_set(std::memory_order_acquire)) _mm_pause();
        const uint64_t tail = s.tail.load(std::memory_order_relaxed);
        // A stale head only underestimates the free space; reload it when the ring looks full.
        uint64_t depth = tail - s.cachedHead;
        if (depth >= kCapacity) {
            s.cachedHead = s.head.load(std::memory_order_acquire);
            depth = tail - s.cachedHead;
        }
        if (depth < kCapacity) {
            s.ring[tail % kCapacity] = entry;
            s.tail.store(tail + 1, std::memory_order_release);
            s.pushLock.clear(std::memory_order_release);
#if defined(MKW_PRIME_QA_TOOLS)
            // depth may be measured against a stale head: sample the real one now and then.
            if ((tail & 255u) == 0) {
                const uint64_t real = tail + 1 - s.head.load(std::memory_order_relaxed);
                if (real > s.maxDepth.load(std::memory_order_relaxed)) s.maxDepth.store(real);
            }
#endif
            break;
        }
        s.pushLock.clear(std::memory_order_release);
        s.fullStalls.fetch_add(1, std::memory_order_relaxed);
        if (t_lockDepth > 0 || t_onGpuThread) {
            // This thread owns the GX lock: nobody else can decode. Do it here;
            // the oldest entries go first, so order is preserved.
            DecodeOne();
            s.inlineDecoded.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (s.failed.load(std::memory_order_acquire)) RethrowIfFailed();
            std::this_thread::yield();
        }
    }
    if (s.sleeping.load(std::memory_order_seq_cst)) {
        std::lock_guard lock(s.wakeMutex);
        s.wakeCv.notify_one();
    }
}
}  // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("WII_COMPILED_PRIME_GPU_THREAD");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

bool OnGpuThread() { return t_onGpuThread; }

uint32_t NativeThreadId() { return g_nativeThreadId.load(std::memory_order_acquire); }

void SubmitBurst(const uint8_t* bytes32) {
    if (!Enabled() || t_onGpuThread) {
        GX_HLE_PrimeConsumeGatherBurst(bytes32);
        return;
    }
    // The synchronous decoder tests the CP read stall when a burst arrives;
    // test it here, at the same point, so the consumer never reads CP state
    // that the guest has changed since.
    if (PrimeGpuCpReadBlocked()) {
        if (g_retained.size() > (64u << 20)) throw std::length_error("Stopped GP FIFO exceeded bounded storage");
        g_retained.insert(g_retained.end(), bytes32, bytes32 + 32);
        return;
    }
    // The submit time only feeds the input-latency report (which input preceded the XFB copy). The
    // game writes 150-350 thousand bursts a second, so reading the clock for each one cost about 1 %
    // of the game thread: read it every 16th burst, and always after a pause (the consumer went to
    // sleep), which keeps the stamp within ~0.1 ms of the real submit time.
#if defined(MKW_PRIME_QA_TOOLS)
    static thread_local uint64_t submitted = 0;
    static thread_local uint32_t sinceStamp = 0;
    if (submitted == 0 || (++sinceStamp & 15u) == 0 || S().sleeping.load(std::memory_order_relaxed))
        submitted = uint64_t(PrimeInputLatency::NowNanos());
    Entry entry{Kind::Burst, uint32_t(submitted), uint32_t(submitted >> 32), {}};
#else
    Entry entry{Kind::Burst, 0, 0, {}};
#endif
    std::memcpy(entry.data, bytes32, sizeof(entry.data));
    Push(entry);
    // Only this (guest) thread submits bursts: a plain load and store, not a locked add.
    auto& bursts = S().bursts;
    const uint64_t count = bursts.load(std::memory_order_relaxed);
    bursts.store(count + 1, std::memory_order_relaxed);
    if ((count & 4095u) == 0) Report();
}

bool DecodingQueued() { return t_decodingQueued > 0; }

void ResumeRetained() {
    if (!Enabled() || g_retained.empty() || PrimeGpuCpReadBlocked()) return;
    std::vector<uint8_t> pending;
    pending.swap(g_retained);
    for (size_t offset = 0; offset < pending.size(); offset += 32) {
        Entry entry{Kind::Burst, 0, 0, {}};
        std::memcpy(entry.data, pending.data() + offset, sizeof(entry.data));
        Push(entry);
    }
    static unsigned traces = 0;
    if (traces++ < 16) RT_LOG(RT_TAG_GX) << "[prime-gpu-thread] resumed retained bytes=" << pending.size() << std::endl;
}

void DiscardRetained() { g_retained.clear(); }

bool QueueInvalidate(uint32_t addr, uint32_t size) {
    // Always queued, even while this thread holds the GX lock: bursts queued
    // before the notification must still be decoded first.
    if (!Enabled() || t_onGpuThread) return false;
    Push(Entry{Kind::Invalidate, addr, size, {}});
    return true;
}

void NoteBreakpointReported() {
#if !defined(MKW_PRIME_QA_TOOLS)
    return;
#endif
    auto& s = S();
    const int64_t since = s.idleSinceNs.load(std::memory_order_acquire);
    if (since == 0) return;
    s.breakpoints.fetch_add(1, std::memory_order_relaxed);
    s.breakpointLatencyNs.fetch_add(uint64_t(NowNs() - since), std::memory_order_relaxed);
}

bool Idle() {
    if (!Enabled()) return true;
    return QueueEmpty() && !S().busy.load(std::memory_order_acquire);
}

void Shutdown() {
    if (!Enabled()) return;
    auto& s = S();
    s.stop.store(true, std::memory_order_release);
    {
        std::lock_guard lock(s.wakeMutex);
        s.wakeCv.notify_all();
    }
    // The consumer may be parked in a renderer wait, so do not join it. Keep
    // the GX lock until process exit instead: nothing decodes after this and
    // Aurora can be torn down safely.
    if (!t_onGpuThread && t_lockDepth == 0) {
        s.hostWaiters.fetch_add(1, std::memory_order_acq_rel);
        s.gxMutex.lock();
        t_lockDepth = 1;
    }
    aurora_stream_shutdown();
}

Lock::Lock() {
    if (!Enabled() || t_onGpuThread) {
        // Without the consumer thread this thread is the producer: the host
        // section still needs the asynchronous decoder caught up and idle.
        if (!t_onGpuThread) aurora_stream_sync();
        return;
    }
    held_ = true;
    if (t_lockDepth > 0 && t_holdUndrained) {
        // Inside a HostLock: the consumer is already excluded, but the decoder
        // was not drained. Do what the outermost Lock would have done.
        ++t_lockDepth;
        upgraded_ = true;
        t_holdUndrained = false;
        while (GX_HLE_PrimeInsideDraw() && !QueueEmpty()) {
            DecodeOne();
            S().inlineDecoded.fetch_add(1, std::memory_order_relaxed);
        }
        aurora_stream_sync();
    } else if (t_lockDepth++ == 0) {
        auto& s = S();
#if defined(MKW_PRIME_QA_TOOLS)
        const auto started = std::chrono::steady_clock::now();
#endif
        s.hostWaiters.fetch_add(1, std::memory_order_acq_rel);
        s.gxMutex.lock();
        s.hostWaiters.fetch_sub(1, std::memory_order_acq_rel);
#if defined(MKW_PRIME_QA_TOOLS)
        s.locks.fetch_add(1, std::memory_order_relaxed);
        s.lockWaitNs.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
#endif
        // The consumer stopped inside a multi-burst draw whose remaining bursts
        // are queued: finish it so host work never sees a partial GXBegin.
        while (GX_HLE_PrimeInsideDraw() && !QueueEmpty()) {
            DecodeOne();
            s.inlineDecoded.fetch_add(1, std::memory_order_relaxed);
        }
        // Host sections read and mutate decoder state (presentation, frame
        // begin/end, overlay): the asynchronous decoder must be caught up and
        // stays idle while this lock keeps every producer out.
        aurora_stream_sync();
    }
    try {
        RethrowIfFailed();
    } catch (...) {
        if (--t_lockDepth == 0) S().gxMutex.unlock();
        held_ = false;
        throw;
    }
}

Lock::~Lock() {
    // Leaving the upgrading Lock: the decoder may run again under the HostLock.
    if (upgraded_) t_holdUndrained = true;
    if (held_ && --t_lockDepth == 0) S().gxMutex.unlock();
}

HostLock::HostLock() {
    if (!Enabled() || t_onGpuThread || t_lockDepth > 0) {
        // No consumer thread, the consumer itself, or already inside a Lock or
        // HostLock: nothing lighter to do than a nested Lock.
        if (!Enabled() || t_onGpuThread) {
            if (!t_onGpuThread) aurora_stream_sync();
            return;
        }
        ++t_lockDepth;
        held_ = true;
        return;
    }
    auto& s = S();
#if defined(MKW_PRIME_QA_TOOLS)
    const auto started = std::chrono::steady_clock::now();
#endif
    s.hostWaiters.fetch_add(1, std::memory_order_acq_rel);
    s.gxMutex.lock();
    s.hostWaiters.fetch_sub(1, std::memory_order_acq_rel);
#if defined(MKW_PRIME_QA_TOOLS)
    s.locks.fetch_add(1, std::memory_order_relaxed);
    s.lockWaitNs.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
#endif
    t_lockDepth = 1;
    t_holdUndrained = true;
    held_ = true;
    try {
        // ImGui input is processed under this lock: the frame worker must be
        // past starting the ImGui frame, as under Lock.
        aurora_wait_for_frame_worker_sealed();
        RethrowIfFailed();
    } catch (...) {
        t_holdUndrained = false;
        t_lockDepth = 0;
        held_ = false;
        s.gxMutex.unlock();
        throw;
    }
}

HostLock::~HostLock() {
    if (!held_) return;
    if (--t_lockDepth == 0) {
        t_holdUndrained = false;
        S().gxMutex.unlock();
    }
}

Sync::Sync() {
    if (!Enabled() || t_onGpuThread) return;
    auto& s = S();
    s.syncs.fetch_add(1, std::memory_order_relaxed);
    while (!QueueEmpty()) {
        DecodeOne();
        s.inlineDecoded.fetch_add(1, std::memory_order_relaxed);
    }
}
}  // namespace GxGpuThread
