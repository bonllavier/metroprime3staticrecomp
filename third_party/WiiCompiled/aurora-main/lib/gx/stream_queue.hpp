#pragma once
// Chunk queue between the GX stream writer (the thread holding the host's GX
// lock: the FIFO interpreter) and the asynchronous command decoder thread.
//
// The writer appends GX wire commands to its buffer; hand_off() moves that
// buffer here as one chunk, cut only at command boundaries, and the decoder
// thread runs `process` over chunks in order. The writer never touches a
// chunk after pushing it; the decoder returns the buffer to the free pool.
//
// Dependency-free so the ordering, back-pressure, sync and failure rules can
// be unit tested without a GPU.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace aurora::gx::fifo {

struct StreamChunk {
  uint8_t* data = nullptr;
  uint32_t size = 0;
  uint32_t capacity = 0;
};

class StreamQueue {
public:
  using Process = std::function<void(const uint8_t*, uint32_t)>;
  using Hook = std::function<void()>;

  StreamQueue(size_t maxQueuedBytes, size_t maxQueuedChunks)
  : m_maxBytes(maxQueuedBytes), m_maxChunks(maxQueuedChunks) {}
  ~StreamQueue() {
    // The decoder may be parked inside renderer waits at exit; the process
    // ends with it. Only buffers nobody holds are released.
    for (auto& chunk : m_free) std::free(chunk.data);
  }
  StreamQueue(const StreamQueue&) = delete;
  StreamQueue& operator=(const StreamQueue&) = delete;

  // `onStart` runs first on the decoder thread, `beforeChunk` before each
  // chunk (the renderer's sealed-frame wait), `process` decodes one chunk.
  void start(Hook onStart, Hook beforeChunk, Process process) {
    std::lock_guard lock(m_mutex);
    if (m_started) return;
    m_started = true;
    m_thread = std::thread([this, onStart = std::move(onStart), beforeChunk = std::move(beforeChunk),
                            process = std::move(process)] { run(onStart, beforeChunk, process); });
    m_thread.detach();
  }
  bool started() const {
    std::lock_guard lock(m_mutex);
    return m_started;
  }

  // Writer: queues a chunk in order. Blocks while the queue is over budget so
  // the writer cannot run unboundedly ahead of the decoder. Surfaces a decoder
  // failure instead of queueing behind it.
  void push(StreamChunk chunk) {
    std::unique_lock lock(m_mutex);
    rethrow_locked();
    m_space.wait(lock, [&] {
      return m_failed || m_stopped || (m_queuedBytes < m_maxBytes && m_queue.size() < m_maxChunks);
    });
    rethrow_locked();
    if (m_stopped) {
      m_free.push_back(chunk);
      return;
    }
    m_queuedBytes += chunk.size;
    ++m_statChunks;
    m_statBytes += chunk.size;
    m_queue.push_back(chunk);
    m_outstanding.fetch_add(1, std::memory_order_relaxed);
    lock.unlock();
    m_work.notify_one();
  }

  // Writer: a buffer of at least `minCapacity` bytes, recycled when possible.
  StreamChunk take_free(uint32_t minCapacity) {
    {
      std::lock_guard lock(m_mutex);
      for (auto it = m_free.begin(); it != m_free.end(); ++it) {
        if (it->capacity >= minCapacity) {
          StreamChunk chunk = *it;
          m_free.erase(it);
          chunk.size = 0;
          return chunk;
        }
      }
    }
    StreamChunk chunk{static_cast<uint8_t*>(std::malloc(minCapacity)), 0, minCapacity};
    if (chunk.data == nullptr) throw std::bad_alloc();
    return chunk;
  }

  // Writer: returns once every queued chunk has been decoded.
  void sync() {
    std::unique_lock lock(m_mutex);
    ++m_statSyncs;
    m_done.wait(lock, [&] { return m_failed || m_stopped || (m_queue.empty() && !m_busy); });
    rethrow_locked();
  }

  // Lock-free: chunks queued or being decoded right now. A hint for load
  // balancing (a chunk may land a moment later). Zero, once the writer has
  // handed its work off, also means the decoder's guest reads for it are done
  // (acquire pairs with the release after each chunk).
  uint32_t backlog_hint() const { return m_outstanding.load(std::memory_order_acquire); }

  // True when nothing is queued and no chunk is being decoded.
  bool idle() const {
    std::lock_guard lock(m_mutex);
    return m_queue.empty() && !m_busy;
  }
  size_t queued_chunks() const {
    std::lock_guard lock(m_mutex);
    return m_queue.size();
  }
  bool failed() const {
    std::lock_guard lock(m_mutex);
    return m_failed;
  }
  // Writer: rethrows a decoder exception (once) so it surfaces where the
  // synchronous decoder would have thrown.
  void rethrow() {
    std::lock_guard lock(m_mutex);
    rethrow_locked();
  }
  // Stops accepting work and wakes the decoder. `waitForExit` (tests) waits
  // until the decoder thread has left run(); Aurora never does, as the decoder
  // may be parked inside a renderer wait that only process exit releases.
  void stop(bool waitForExit = false) {
    std::unique_lock lock(m_mutex);
    m_stopped = true;
    m_outstanding.store(0, std::memory_order_release);  // nothing will be decoded: never report a backlog
    m_work.notify_all();
    m_space.notify_all();
    m_done.notify_all();
    if (waitForExit && m_started) m_exit.wait(lock, [&] { return m_exited; });
  }

  struct Stats {
    uint64_t chunks = 0, bytes = 0, syncs = 0, decoderIdleWaits = 0;
  };
  Stats stats() const {
    std::lock_guard lock(m_mutex);
    return {m_statChunks, m_statBytes, m_statSyncs, m_statIdleWaits};
  }

private:
  void rethrow_locked() {
    if (!m_failed) return;
    m_failed = false;
    std::exception_ptr failure = m_failure;
    m_failure = nullptr;
    std::rethrow_exception(failure);
  }

  void run(const Hook& onStart, const Hook& beforeChunk, const Process& process) {
    if (onStart) onStart();
    for (;;) {
      StreamChunk chunk;
      {
        std::unique_lock lock(m_mutex);
        if (m_queue.empty()) ++m_statIdleWaits;
        m_work.wait(lock, [&] { return m_stopped || !m_queue.empty(); });
        if (m_stopped) {
          m_busy = false;
          m_exited = true;
          m_done.notify_all();
          m_exit.notify_all();
          return;
        }
        chunk = m_queue.front();
        m_queue.pop_front();
        m_queuedBytes -= chunk.size;
        m_busy = true;
      }
      m_space.notify_all();
      try {
        if (beforeChunk) beforeChunk();
        process(chunk.data, chunk.size);
      } catch (...) {
        std::lock_guard lock(m_mutex);
        m_failure = std::current_exception();
        m_failed = true;
        m_stopped = true;
        m_busy = false;
        m_exited = true;
        m_outstanding.store(0, std::memory_order_release);
        m_free.push_back(chunk);
        m_done.notify_all();
        m_space.notify_all();
        m_exit.notify_all();
        return;
      }
      {
        std::lock_guard lock(m_mutex);
        m_busy = false;
        if (!m_stopped) m_outstanding.fetch_sub(1, std::memory_order_release);  // stop() already zeroed it
        if (m_free.size() < kFreePoolLimit) m_free.push_back(chunk);
        else std::free(chunk.data);
      }
      m_done.notify_all();
    }
  }

  static constexpr size_t kFreePoolLimit = 8;
  const size_t m_maxBytes, m_maxChunks;
  mutable std::mutex m_mutex;
  std::condition_variable m_work, m_space, m_done, m_exit;
  std::deque<StreamChunk> m_queue;
  std::vector<StreamChunk> m_free;
  size_t m_queuedBytes = 0;
  bool m_started = false, m_stopped = false, m_busy = false, m_failed = false, m_exited = false;
  std::atomic<uint32_t> m_outstanding{0};  // chunks pushed and not yet decoded
  std::exception_ptr m_failure;
  std::thread m_thread;
  uint64_t m_statChunks = 0, m_statBytes = 0, m_statSyncs = 0, m_statIdleWaits = 0;
};

} // namespace aurora::gx::fifo
