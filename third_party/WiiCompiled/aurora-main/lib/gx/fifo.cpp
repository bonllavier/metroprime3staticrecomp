#include "fifo.hpp"
#include "command_processor.hpp"
#include "stream_queue.hpp"
#include "render_policy.hpp"
#include <aurora/host_cores.h>
#include "../gfx/common.hpp"
#include "../internal.hpp"

#include <atomic>
#include <mutex>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "tracy/Tracy.hpp"
#include "dolphin/gx/GXCommandList.h"

namespace aurora::gx::fifo {
static Module Log("aurora::gx::fifo");

namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
} // namespace detail

namespace {
// Asynchronous decoder state. The queue outlives static destruction on
// purpose: the decoder may be parked in a renderer wait at exit.
std::atomic<bool> g_asyncEnabled{false};
thread_local bool t_onDecoder = false;
StreamQueue* g_queue = nullptr;
std::once_flag g_decoderStarted;
std::atomic<uint32_t> g_decoderNativeId{0};
// Chunks are cut only at command boundaries (commit/hand_off callers).
constexpr uint32_t kChunkCapacity = 64 * 1024;
// Hand-off sizes: policy::hand_off_now (render_policy.hpp).
constexpr uint32_t kSoftChunkBytes = policy::kSoftHandOffBytes;
constexpr uint32_t kEagerChunkBytes = policy::kEagerHandOffBytes;
constexpr size_t kMaxQueuedBytes = 16u << 20;
constexpr size_t kMaxQueuedChunks = 1024;
std::atomic<uint64_t> g_statHandOffs{0}, g_statHandOffBytes{0};
// Vertex CP registers the decoder holds may differ from the SDK shadow.
bool g_vcdUnknown = true;
uint8_t g_vatUnknown = 0xFF;

void ensure_decoder_started() {
  std::call_once(g_decoderStarted, [] {
    g_queue = new StreamQueue(kMaxQueuedBytes, kMaxQueuedChunks);
    g_queue->start(
        [] {
          t_onDecoder = true;
#if defined(_WIN32)
          SetThreadDescription(GetCurrentThread(), L"Aurora GX decoder");
          // Busy decoding, it must not share the game thread's core (aurora/host_cores.h).
          aurora::host_cores::keep_off_game_core();
          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
          g_decoderNativeId.store(GetCurrentThreadId(), std::memory_order_release);
#elif defined(__linux__)
          aurora::host_cores::set_thread_name("Aurora GX dec");
          aurora::host_cores::keep_off_game_core();
          aurora::host_cores::set_thread_nice(-5);
          g_decoderNativeId.store(aurora::host_cores::native_thread_id(), std::memory_order_release);
#endif
        },
        [] {
          // As the synchronous drain did: the recorded pass list of the
          // previous frame must be sealed before new commands mutate it.
          const auto waited = aurora::wait_for_frame_worker_sealed();
          if (waited.count() > 0) UNLIKELY {
            TracyPlot("aurora: fifoDrainWaitUs", static_cast<int64_t>(waited.count() / 1000));
          }
        },
        [](const uint8_t* data, uint32_t size) { process_all(data, size, true); });
    Log.info("Asynchronous GX decoder started (chunk {} KiB, soft {} KiB, eager {} KiB)", kChunkCapacity / 1024,
             kSoftChunkBytes / 1024, kEagerChunkBytes / 1024);
  });
}
} // namespace

bool async_enabled() { return g_asyncEnabled.load(std::memory_order_relaxed); }
void set_async_enabled(bool enabled) { g_asyncEnabled.store(enabled, std::memory_order_relaxed); }
bool on_decoder_thread() { return t_onDecoder; }
uint32_t decoder_native_thread_id() { return g_decoderNativeId.load(std::memory_order_acquire); }

void hand_off() {
  // Never inside a draw: a vertex packet split across chunks is unparseable,
  // and GXEnd measures the packet against this buffer.
  if (!async_enabled() || t_onDecoder || detail::sInDisplayList || in_begin()) return;
  ensure_decoder_started();
  g_queue->rethrow();
  if (detail::sBufferSize == 0) return;
  g_statHandOffs.fetch_add(1, std::memory_order_relaxed);
  g_statHandOffBytes.fetch_add(detail::sBufferSize, std::memory_order_relaxed);
  g_queue->push(StreamChunk{detail::sBufferData, detail::sBufferSize, detail::sBufferCapacity});
  const StreamChunk fresh = g_queue->take_free(kChunkCapacity);
  detail::sBufferData = fresh.data;
  detail::sBufferSize = 0;
  detail::sBufferCapacity = fresh.capacity;
}

void commit() {
  if (!async_enabled() || t_onDecoder) return;
  const uint32_t size = detail::sBufferSize;
  if (size < kEagerChunkBytes) return;
  // Lock-free idle hint: commit runs once per draw, and the locked idle() query
  // here cost ~2 % of the GX thread in heavy scenes (F8 Norion, 2026-10-30).
  if (policy::hand_off_now(size, size < kSoftChunkBytes && g_queue != nullptr && g_queue->backlog_hint() == 0))
    hand_off();
}

void sync() {
  if (!async_enabled() || t_onDecoder) return;
  if (in_begin()) UNLIKELY {
    // The open draw stays in the writer buffer; whoever called sync() here
    // would have mutated decoder state under a half-written vertex packet.
    static std::atomic<uint32_t> warned{0};
    if (warned.fetch_add(1, std::memory_order_relaxed) < 8)
      Log.warn("sync() inside GXBegin..GXEnd: {} bytes stay queued until GXEnd", detail::sBufferSize);
  }
  hand_off();
  if (g_queue != nullptr) g_queue->sync();
  // Callers mutate decoder state next (the same barrier the synchronous drain gave them).
  aurora::wait_for_frame_worker_sealed();
}

uint32_t async_backlog_hint() {
  return !async_enabled() || g_queue == nullptr ? 0 : g_queue->backlog_hint();
}

bool async_idle() {
  if (!async_enabled() || g_queue == nullptr) return true;
  return g_queue->idle();
}

void shutdown_async() {
  if (g_queue != nullptr) g_queue->stop();
}

void init() {
  constexpr uint32_t initialCapacity = 64 * 1024;
  reset_cp_register_cache();
  mark_vertex_cp_unknown();
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
}

void write_data_grow(const void* data, uint32_t length) {
  uint32_t needed = detail::sBufferSize + length;
  uint32_t newCap = std::max(detail::sBufferCapacity * 2, needed);
  detail::sBufferData = static_cast<uint8_t*>(realloc(detail::sBufferData, newCap));
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCap;
}

void write_data_bulk(const void* data,uint32_t length) {
  write_data(data,length);
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  uint32_t bytesWritten = detail::sDlWritePos;
  uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

bool vertex_cp_unknown_vcd() { return g_vcdUnknown; }
bool vertex_cp_unknown_vat(uint8_t fmt) { return fmt < 8 && (g_vatUnknown & (1u << fmt)) != 0; }
// Emissions recorded into a display list reach the decoder only when that
// list is called; they do not make the live registers known.
void note_vcd_emitted() {
  if (!detail::sInDisplayList) g_vcdUnknown = false;
}
void note_vat_emitted(uint8_t fmt) {
  if (!detail::sInDisplayList && fmt < 8) g_vatUnknown &= static_cast<uint8_t>(~(1u << fmt));
}
void mark_vertex_cp_unknown() {
  g_vcdUnknown = true;
  g_vatUnknown = 0xFF;
}

bool note_display_list_called(const uint8_t* data, uint32_t nbytes) {
  // Command sizes the decoder uses (process()). The walk stops at the first
  // command whose size depends on vertex state: a draw or an Aurora command.
  bool touches = false;
  uint32_t pos = 0;
  while (pos < nbytes && !touches) {
    const uint8_t cmd = data[pos];
    const uint8_t opcode = cmd & 0xF8u;
    if (cmd == GX_NOP || opcode == GX_CMD_INVL_VC) {
      pos += 1;
    } else if (cmd == GX_LOAD_BP_REG) {
      pos += 5;
    } else if (opcode == GX_LOAD_CP_REG) {
      if (pos + 6 > nbytes) {
        touches = true;
        break;
      }
      const uint8_t reg = data[pos + 1];
      touches = reg >= 0x50 && reg <= 0x9F;
      pos += 6;
    } else if (opcode == GX_LOAD_XF_REG) {
      if (pos + 5 > nbytes) {
        touches = true;
        break;
      }
      const uint32_t words = ((uint32_t(data[pos + 1]) << 8) | data[pos + 2]) + 1;
      pos += 5 + words * 4;
    } else if (opcode >= GX_LOAD_INDX_A && opcode <= GX_LOAD_INDX_D) {
      pos += 5;
    } else if (opcode == GX_CMD_CALL_DL) {
      pos += 9;  // the decoder ignores nested lists
    } else {
      touches = true;  // draw, GX_LOAD_AURORA or unknown
    }
  }
  if (pos > nbytes) touches = true;
  if (touches) mark_vertex_cp_unknown();
  return touches;
}

// How much of the producer's frame is spent blocked before it may decode the next batch of GX commands.
static void note_drain_wait(uint64_t nanos) noexcept {
  ZoneScopedN("FIFO drain wait");
  TracyPlot("aurora: fifoDrainWaitUs", static_cast<int64_t>(nanos / 1000));
}

void drain() {
  if (async_enabled()) {
    // The decoder is the only thread that may touch decoder state; a drain
    // from it is already in order, and any other caller needs the queue empty.
    if (!t_onDecoder) sync();
    return;
  }
  // SEALED, not DONE.
  const auto waited = aurora::wait_for_frame_worker_sealed();
  if (waited.count() > 0) UNLIKELY {
    note_drain_wait(static_cast<uint64_t>(waited.count()));
  }
  if (detail::sBufferSize == 0) {
    return;
  }
  process_all(detail::sBufferData, detail::sBufferSize, true);
  detail::sBufferSize = 0;
}

void process_all(const uint8_t* data, uint32_t size, bool bigEndian) {
  uint32_t consumed = 0;
  bool retried = false;
  while (consumed < size) {
    const auto count = process(data + consumed, size - consumed, bigEndian);
    consumed += count;
    // process returned with its renderer lock released: a queued host callback
    // (GX_LOAD_AURORA_HOST_CALLBACK) runs here, in stream order.
    if (dispatch_pending_host_callback()) {
      retried = false;
      continue;
    }
    if (consumed == size) break;
    if (count == 0 && retried)
      throw gfx::StagingCapacityError("FIFO draw does not fit after capacity submission");
    // No recursive drain.
    gfx::split_staging_batch();
    retried = true;
  }
}

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }
void clear_buffer() {
  // Undecoded register writes may be among the dropped bytes.
  if (detail::sBufferSize != 0) mark_vertex_cp_unknown();
  detail::sBufferSize = 0;
}

} // namespace aurora::gx::fifo
