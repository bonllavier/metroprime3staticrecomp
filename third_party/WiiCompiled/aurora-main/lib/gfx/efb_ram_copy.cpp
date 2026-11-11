#include "../gx/render_policy.hpp"
#include <cstdlib>
#include "efb_ram_copy.hpp"

#include "efb_ram_encoder.hpp"
#include "tex_copy_conv.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <magic_enum.hpp>

namespace aurora::gfx::efb_ram {
namespace {

Module Log("aurora::gfx::efb_ram");

using webgpu::g_device;
using webgpu::g_instance;

// CPU-consumed probe copies: the lens-flare occlusion probe (4x4 Z24X8, 64 bytes) and RM3E01's
// scan-visor target probe around the reticle (A8 ~768 bytes, then Z16 2048 and Z8L 1024 bytes of
// the same rectangle; the game picks the scan target from the alpha object ID and depth). They
// read back asynchronously, one frame late, instead of stalling the producer every frame.
constexpr size_t kAsyncReadbackMaxBytes = 4096;
// Each destination keeps its readback buffer while it is in use. The cap stops one-shot
// destinations from leaking GPU buffers; when it is reached the least recently scheduled idle
// slot is recycled. Without recycling, a long session (new lens-flare and scan-visor probe
// buffers per area, a rebuilt HUD) filled all slots and every later probe fell back to the lazy
// path, which the game never triggers: guest RAM went stale and nothing could be scanned until
// the game was restarted (RM3E01, 2026-10-25).
constexpr size_t kMaxAsyncSlotsCap = MaxAsyncReadbackSlots;
// Test hook: WII_COMPILED_AURORA_ASYNC_READBACK_SLOTS=<1..32> lowers the cap to exercise recycling.
const size_t kMaxAsyncSlots = [] {
  if (const char* value = std::getenv("WII_COMPILED_AURORA_ASYNC_READBACK_SLOTS")) {
    const long parsed = std::strtol(value, nullptr, 10);
    if (parsed >= 1 && parsed <= static_cast<long>(kMaxAsyncSlotsCap)) return static_cast<size_t>(parsed);
  }
  return kMaxAsyncSlotsCap;
}();

struct PendingCopy {
  void* dest = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  GXTexFmt format = GX_TF_RGBA8;
  TextureHandle texture;
  TextureHandle nativeTexture;
  Range nativeBlitUniform;
  uint64_t nativeUniformEpoch = 0;
};

struct Download {
  PendingCopy copy;
  wgpu::Buffer buffer;
  uint32_t bytesPerRow = 0;
  uint64_t bufferSize = 0;
};

enum class AsyncState : uint8_t {
  Idle,
  Encoding,
  CopySubmitted,
  MapPending,
  // The map finished (mapStatus says how); the frame thread reads it back at its next drain.
  Mapped,
  // The frame thread owns the slot while it encodes, notifies and unmaps with the mutex released.
  Completing,
};

// One readback per destination address, reused for that destination's lifetime: the probe hits
// the same buffer every frame, so pooling avoids a per-frame allocation.
struct AsyncSlot {
  wgpu::Buffer buffer;
  TextureHandle nativeTexture;
  uint64_t bufferSize = 0;
  uint32_t bytesPerRow = 0;
  // Latched at encode time and read by the map callback.
  void* dest = nullptr;
  GXTexFmt format = GX_TF_RGBA8;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t hostWidth = 0;
  uint32_t hostHeight = 0;
  HostPixelOrder order = HostPixelOrder::RGBA;
  AsyncState state = AsyncState::Idle;
  wgpu::MapAsyncStatus mapStatus = wgpu::MapAsyncStatus::Success;
  uint64_t lastScheduled = 0;  // g_asyncScheduleClock value of the latest request
};

std::vector<PendingCopy> g_pending;
std::vector<Download> g_downloads;

// Recorded by the producer, moved to the worker-private sealed list while the
// producer is excluded.
std::vector<PendingCopy> g_asyncPending;
std::vector<PendingCopy> g_asyncSealed;

std::mutex g_asyncMutex;
std::unordered_map<void*, AsyncSlot> g_asyncSlots;
uint32_t g_asyncMapsInFlight = 0;
uint64_t g_asyncGeneration = 1;
uint64_t g_asyncScheduleClock = 0;
uint64_t g_asyncRecycled = 0;

uint32_t align_to(uint32_t value, uint32_t alignment) noexcept { return (value + alignment - 1) & ~(alignment - 1); }

// An internally scaled copy cannot be read back directly, so blit it down to native-resolution
// RGBA8 first. Pushes a uniform, so it must run while the staging buffers are still mapped.
void ensure_native_texture(PendingCopy& pending, TextureHandle* cache = nullptr) noexcept {
  if (pending.texture->size.width == pending.width && pending.texture->size.height == pending.height) {
    return;
  }
  if (pending.nativeTexture && pending.nativeUniformEpoch == staging_epoch()) return;
  if (pending.nativeTexture) {
    // Keep the texture; its old staging range belongs to a submitted batch.
  } else if (cache != nullptr && *cache && (*cache)->size.width == pending.width &&
      (*cache)->size.height == pending.height) {
    pending.nativeTexture = *cache;
  } else {
    pending.nativeTexture = new_conv_texture(pending.width, pending.height, GX_TF_RGBA8, "GX EFB RAM native readback");
    if (cache != nullptr) {
      *cache = pending.nativeTexture;
    }
  }
  // The shared blit shader clamps Y to flags.z/w; preserve the full source.
  const std::array nativeBlitUniform{
      0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 64.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
  };
  pending.nativeBlitUniform = push_uniform(nativeBlitUniform);
  pending.nativeUniformEpoch = staging_epoch();
}

void encode_native_blit(const wgpu::CommandEncoder& encoder, const PendingCopy& pending) noexcept {
  const tex_copy_conv::ConvRequest blitRequest{
      .fmt = GX_TF_RGBA8,
      .srcView = pending.texture->sampleTextureView,
      .uniformRange = pending.nativeBlitUniform,
      .dst = pending.nativeTexture,
      .sampleFilter = pending.format == GX_TF_Z16 || pending.format == GX_TF_Z24X8
                          ? tex_copy_conv::SampleFilter::Nearest
                          : tex_copy_conv::SampleFilter::Linear,
  };
  tex_copy_conv::blit(encoder, blitRequest);
}

HostPixelOrder texture_pixel_order(const TextureHandle& texture) noexcept {
  return texture->format == wgpu::TextureFormat::BGRA8Unorm ? HostPixelOrder::BGRA : HostPixelOrder::RGBA;
}

// Map callback. AllowSpontaneous lets Dawn run it on any thread, from inside its own calls and with its
// locks held, so it touches nothing but the slot state: no Dawn call, no hook, no wait. The readback used to
// happen here, Unmap and the guest-write hook included, and froze the game twice in QA replays at 120
// (2026-11-08): first with g_asyncMutex held (the GX thread waited for it in schedule() from GXCopyTex),
// then, with the mutex released, inside wgpuBufferUnmap itself while the frame worker waited on Dawn.
void complete_async_slot(void* dest, uint64_t generation, wgpu::MapAsyncStatus status,
                         wgpu::StringView message) noexcept {
  {
    std::lock_guard lock{g_asyncMutex};
    if (generation != g_asyncGeneration) return;
    const auto it = g_asyncSlots.find(dest);
    if (it == g_asyncSlots.end()) {
      return;
    }
    auto& slot = it->second;
    if (slot.state != AsyncState::MapPending) return;
    if (g_asyncMapsInFlight > 0) --g_asyncMapsInFlight;
    slot.mapStatus = status;
    slot.state = AsyncState::Mapped;
  }
  if (status != wgpu::MapAsyncStatus::Success && status != wgpu::MapAsyncStatus::CallbackCancelled &&
      status != wgpu::MapAsyncStatus::Aborted) {
    Log.warn("Async EFB RAM readback mapping failed {}: {}", magic_enum::enum_name(status), message);
  }
}

// Frame thread: writes finished readbacks to guest RAM, notifies and unmaps, with g_asyncMutex released
// (the Completing state keeps each slot from being reused, mapped or recycled meanwhile, and the local
// buffer references keep the buffers alive until after the mutex).
void harvest_mapped_slots() noexcept {
  std::vector<AsyncSlot> ready;
  {
    std::lock_guard lock{g_asyncMutex};
    for (auto& [dest, slot] : g_asyncSlots) {
      if (slot.state != AsyncState::Mapped) continue;
      slot.state = AsyncState::Completing;
      AsyncSlot copy;
      copy.buffer = slot.buffer;
      copy.bufferSize = slot.bufferSize;
      copy.bytesPerRow = slot.bytesPerRow;
      copy.dest = dest;
      copy.format = slot.format;
      copy.width = slot.width;
      copy.height = slot.height;
      copy.hostWidth = slot.hostWidth;
      copy.hostHeight = slot.hostHeight;
      copy.order = slot.order;
      copy.mapStatus = slot.mapStatus;
      ready.push_back(std::move(copy));
    }
  }
  if (ready.empty()) return;
  for (auto& copy : ready) {
    if (copy.mapStatus != wgpu::MapAsyncStatus::Success) continue;
    const auto* pixels = static_cast<const uint8_t*>(copy.buffer.GetConstMappedRange(0, copy.bufferSize));
    if (pixels != nullptr) {
      // Writes guest RAM while the guest may be reading it. The only consumer min/maxes depth for a
      // fade factor, so a torn tile just mixes two frames' depths.
      const size_t outputSize = encoded_size(copy.format, copy.width, copy.height);
      if (!encode(copy.dest, outputSize, copy.format, copy.width, copy.height, pixels, copy.hostWidth, copy.hostHeight,
                  copy.bytesPerRow, copy.order)) {
        Log.error("Failed to encode async EFB RAM copy format=0x{:x} size={}x{}", static_cast<unsigned>(copy.format),
                  copy.width, copy.height);
      }
      // Guest RAM written from outside the embedder, so nothing bumps its write generation and a
      // texture cached over this range would keep its digest.
      notify_guest_write(copy.dest, outputSize);
    }
    copy.buffer.Unmap();
  }
  std::lock_guard lock{g_asyncMutex};
  for (const auto& copy : ready) {
    const auto it = g_asyncSlots.find(copy.dest);
    if (it != g_asyncSlots.end() && it->second.state == AsyncState::Completing &&
        it->second.buffer.Get() == copy.buffer.Get()) {
      it->second.state = AsyncState::Idle;
    }
  }
}

// Runs the completion callbacks for maps whose GPU work has finished, then reads the finished ones back.
// Frame thread only, never with g_asyncMutex held: complete_async_slot takes it.
void drain_async_events() noexcept {
  bool inFlight = false;
  {
    std::lock_guard lock{g_asyncMutex};
    inFlight = g_asyncMapsInFlight != 0;
  }
  if (inFlight && g_instance) {
    g_instance.ProcessEvents();
  }
  harvest_mapped_slots();
}

} // namespace

void schedule(void* dest, uint32_t width, uint32_t height, GXTexFmt format, TextureHandle texture) noexcept {
  if (dest == nullptr || width == 0 || height == 0 || !texture) {
    return;
  }
  if (!supports_format(format)) {
    Log.fatal("Unsupported CPU-visible EFB copy format 0x{:x}", static_cast<unsigned>(format));
  }

  PendingCopy request{
      .dest = dest,
      .width = width,
      .height = height,
      .format = format,
      .texture = std::move(texture),
  };

  const size_t encodedSize = encoded_size(format, width, height);
  // Offscreen copies resolve on a separate pass list that the sealed frame's
  // native render does not replay, so they stay on the lazy path.
  bool async = !is_offscreen() && encodedSize != 0 && encodedSize <= kAsyncReadbackMaxBytes;
  AsyncSlot recycled;  // released after the mutex: buffer destruction may run callbacks
  size_t notifyFreshBytes = 0;  // notified after the mutex, like every hook call
  if (async) {
    std::lock_guard lock{g_asyncMutex};
    ++g_asyncScheduleClock;
    auto it = g_asyncSlots.find(dest);
    if (it == g_asyncSlots.end() && g_asyncSlots.size() >= kMaxAsyncSlots) {
      std::vector<void*> ids;
      std::vector<gx::policy::SlotCandidate> candidates;
      ids.reserve(g_asyncSlots.size());
      candidates.reserve(g_asyncSlots.size());
      for (const auto& [slotDest, slot] : g_asyncSlots) {
        ids.push_back(slotDest);
        candidates.push_back({slot.state == AsyncState::Idle, slot.lastScheduled});
      }
      const size_t victim = gx::policy::pick_recycle_slot(candidates);
      if (victim != SIZE_MAX) {
        auto oldest = g_asyncSlots.find(ids[victim]);
        const uint64_t idleRequests = g_asyncScheduleClock - oldest->second.lastScheduled;
        const void* oldDest = oldest->first;
        recycled = std::move(oldest->second);
        g_asyncSlots.erase(oldest);
        if (++g_asyncRecycled <= 16 || g_asyncRecycled % 256 == 0) {
          Log.info("[efb-ram-async] slots full ({}): recycled the idle slot for {} (unused for {} requests) for {}; "
                   "{} recycled so far", kMaxAsyncSlots, oldDest, idleRequests, static_cast<const void*>(dest), g_asyncRecycled);
        }
      }
    }
    if (it != g_asyncSlots.end()) {
      it->second.lastScheduled = g_asyncScheduleClock;
    } else {
      if (g_asyncSlots.size() >= kMaxAsyncSlots) {
        async = false;  // every slot is mid-readback this frame; retry next time
      } else {
        g_asyncSlots.try_emplace(dest).first->second.lastScheduled = g_asyncScheduleClock;
        // No readback has landed here yet. 0xff decodes to far Z, which the probe reads as unobstructed
        // so a new flare fades in; zero-filled RAM would decode as fully occluded.
        std::memset(dest, 0xff, encodedSize);
        notifyFreshBytes = encodedSize;
      }
    }
  }
  if (notifyFreshBytes != 0) {
    notify_guest_write(dest, notifyFreshBytes);
  }

  auto& list = async ? g_asyncPending : g_pending;
  if (auto it = std::find_if(list.begin(), list.end(),
                             [dest](const PendingCopy& pending) { return pending.dest == dest; });
      it != list.end()) {
    *it = std::move(request);
  } else {
    list.push_back(std::move(request));
  }
}

bool has_pending(void* dest) noexcept {
  if (dest == nullptr) return !g_pending.empty() || !g_downloads.empty();
  return std::any_of(g_pending.begin(), g_pending.end(),
                     [dest](const PendingCopy& pending) { return pending.dest == dest; }) ||
         std::any_of(g_downloads.begin(), g_downloads.end(),
                     [dest](const Download& download) { return download.copy.dest == dest; });
}

bool prepare_downloads(void* dest) {
  uint64_t copies = 0;
  for (const auto& pending : g_pending) {
    if (dest != nullptr && pending.dest != dest) continue;
    if (pending.texture->size.width != pending.width || pending.texture->size.height != pending.height) ++copies;
  }
  // Reserve all copies, even already-prepared ones: a split retires their ranges.
  ensure_staging_space({0, copies * staging_uniform_bytes(48), 0, 0});
  bool found = false;
  for (auto& pending : g_pending) {
    if (dest != nullptr && pending.dest != dest) continue;
    found = true;
    ensure_native_texture(pending);
  }
  return found;
}

void encode_downloads(const wgpu::CommandEncoder& encoder, void* dest) noexcept {
  if (!g_downloads.empty()) {
    Log.fatal("Attempted to encode new EFB RAM downloads before completing the previous batch");
  }

  g_downloads.reserve(g_pending.size());
  for (auto it = g_pending.begin(); it != g_pending.end();) {
    if (dest != nullptr && it->dest != dest) {
      ++it;
      continue;
    }
    PendingCopy pending = std::move(*it);
    it = g_pending.erase(it);
    if (pending.nativeTexture) {
      encode_native_blit(encoder, pending);
    }
    const auto& texture = pending.nativeTexture ? pending.nativeTexture : pending.texture;
    const uint32_t bytesPerRow = align_to(texture->size.width * 4, 256);
    const uint64_t bufferSize = static_cast<uint64_t>(bytesPerRow) * texture->size.height;
    const wgpu::BufferDescriptor descriptor{
        .label = "GX EFB RAM copy readback",
        .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
        .size = bufferSize,
    };
    auto buffer = g_device.CreateBuffer(&descriptor);
    const wgpu::TexelCopyTextureInfo source{
        .texture = texture->texture,
        .mipLevel = 0,
        .origin = {0, 0, 0},
        .aspect = wgpu::TextureAspect::All,
    };
    const wgpu::TexelCopyBufferInfo destination{
        .layout =
            wgpu::TexelCopyBufferLayout{
                .offset = 0,
                .bytesPerRow = bytesPerRow,
                .rowsPerImage = texture->size.height,
            },
        .buffer = buffer,
    };
    encoder.CopyTextureToBuffer(&source, &destination, &texture->size);
    g_downloads.push_back({
        .copy = std::move(pending),
        .buffer = std::move(buffer),
        .bytesPerRow = bytesPerRow,
        .bufferSize = bufferSize,
    });
  }
}

bool complete_downloads() noexcept {
  bool success = true;
  for (auto& download : g_downloads) {
    // WaitAny may time out before Dawn delivers cancellation. The callback must
    // own its result rather than retaining references to this stack frame.
    struct MapResult {
      std::mutex mutex;
      wgpu::MapAsyncStatus status = wgpu::MapAsyncStatus::CallbackCancelled;
      std::string message;
    };
    const auto result = std::make_shared<MapResult>();
    const auto future =
        download.buffer.MapAsync(wgpu::MapMode::Read, 0, download.bufferSize, wgpu::CallbackMode::WaitAnyOnly,
                                 [result](wgpu::MapAsyncStatus status, wgpu::StringView message) {
                                   std::lock_guard lock{result->mutex};
                                   result->status = status;
                                   if (message.data != nullptr) {
                                     size_t length = 0;
                                     while (length < 512 && length < message.length && message.data[length] != '\0') {
                                       ++length;
                                     }
                                     result->message.assign(message.data, length);
                                   }
                                 });
    const auto waitStatus = g_instance.WaitAny(future, 5000000000);
    wgpu::MapAsyncStatus mapStatus;
    std::string mapMessage;
    {
      std::lock_guard lock{result->mutex};
      mapStatus = result->status;
      mapMessage = result->message;
    }
    if (waitStatus != wgpu::WaitStatus::Success || mapStatus != wgpu::MapAsyncStatus::Success) {
      Log.error("EFB RAM readback failed wait={} map={} message={}", magic_enum::enum_name(waitStatus),
                magic_enum::enum_name(mapStatus), mapMessage);
      success = false;
      continue;
    }

    const auto* pixels = static_cast<const uint8_t*>(download.buffer.GetConstMappedRange(0, download.bufferSize));
    const auto& readbackTexture = download.copy.nativeTexture ? download.copy.nativeTexture : download.copy.texture;
    const auto order = texture_pixel_order(readbackTexture);
    const size_t outputSize = encoded_size(download.copy.format, download.copy.width, download.copy.height);
    if (!encode(download.copy.dest, outputSize, download.copy.format, download.copy.width, download.copy.height, pixels,
                readbackTexture->size.width, readbackTexture->size.height, download.bytesPerRow, order)) {
      Log.error("Failed to encode EFB RAM copy format=0x{:x} size={}x{}", static_cast<unsigned>(download.copy.format),
                download.copy.width, download.copy.height);
      success = false;
    }
    notify_guest_write(download.copy.dest, outputSize);
    download.buffer.Unmap();
  }
  g_downloads.clear();
  return success;
}

void cancel() noexcept {
  g_pending.clear();
  g_downloads.clear();
  g_asyncPending.clear();
}

// No Dawn call may run under g_asyncMutex, here or in encode_async_downloads. Dawn delivers a finished
// map from inside any of its calls, with its device lock held, and complete_async_slot then takes
// the mutex: a thread holding the mutex and waiting for the device lock freezes the game for good. It
// took a Present in another thread to line up, so more presented images made it far more likely.
void seal_async_downloads() noexcept {
  for (auto& pending : g_asyncPending) {
    TextureHandle cached;
    bool hasSlot = false;
    {
      std::lock_guard lock{g_asyncMutex};
      if (const auto it = g_asyncSlots.find(pending.dest); it != g_asyncSlots.end()) {
        cached = it->second.nativeTexture;
        hasSlot = true;
      }
    }
    ensure_native_texture(pending, hasSlot ? &cached : nullptr);
    if (hasSlot) {
      std::lock_guard lock{g_asyncMutex};
      if (const auto it = g_asyncSlots.find(pending.dest); it != g_asyncSlots.end()) {
        it->second.nativeTexture = cached;
      }
    }
  }
  g_asyncSealed = std::move(g_asyncPending);
  g_asyncPending.clear();
}

void encode_async_downloads(const wgpu::CommandEncoder& encoder) noexcept {
  if (g_asyncSealed.empty()) {
    return;
  }
  // Retire the previous frame's map first so its slot is free here, otherwise a single-slot
  // destination could only be sampled every other frame.
  drain_async_events();
  for (auto& pending : g_asyncSealed) {
    wgpu::Buffer buffer;
    uint64_t currentBufferSize = 0;
    {
      std::lock_guard lock{g_asyncMutex};
      const auto it = g_asyncSlots.find(pending.dest);
      if (it == g_asyncSlots.end()) {
        continue;
      }
      auto& slot = it->second;
      // Only one readback per destination may be outstanding and the previous map still owns the pooled
      // buffer. Dropping this request just leaves the guest on a slightly older probe.
      if (slot.state != AsyncState::Idle) {
        continue;
      }
      // Keeps the slot out of reach (never recycled, nor mapped) while the mutex is released.
      slot.state = AsyncState::Encoding;
      buffer = slot.buffer;
      currentBufferSize = slot.bufferSize;
    }
    if (pending.nativeTexture) {
      encode_native_blit(encoder, pending);
    }
    const auto& texture = pending.nativeTexture ? pending.nativeTexture : pending.texture;
    const uint32_t bytesPerRow = align_to(texture->size.width * 4, 256);
    const uint64_t bufferSize = static_cast<uint64_t>(bytesPerRow) * texture->size.height;
    if (!buffer || currentBufferSize != bufferSize) {
      const wgpu::BufferDescriptor descriptor{
          .label = "GX EFB RAM async readback",
          .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
          .size = bufferSize,
      };
      buffer = g_device.CreateBuffer(&descriptor);
    }
    if (!buffer) {
      std::lock_guard lock{g_asyncMutex};
      if (const auto it = g_asyncSlots.find(pending.dest); it != g_asyncSlots.end()) {
        it->second.state = AsyncState::Idle;
      }
      continue;
    }
    const wgpu::TexelCopyTextureInfo source{
        .texture = texture->texture,
        .mipLevel = 0,
        .origin = {0, 0, 0},
        .aspect = wgpu::TextureAspect::All,
    };
    const wgpu::TexelCopyBufferInfo destination{
        .layout =
            wgpu::TexelCopyBufferLayout{
                .offset = 0,
                .bytesPerRow = bytesPerRow,
                .rowsPerImage = texture->size.height,
            },
        .buffer = buffer,
    };
    encoder.CopyTextureToBuffer(&source, &destination, &texture->size);
    std::lock_guard lock{g_asyncMutex};
    const auto it = g_asyncSlots.find(pending.dest);
    if (it == g_asyncSlots.end()) {
      continue;
    }
    auto& slot = it->second;
    // Swapped: a replaced buffer is released after the mutex, since destroying one may run callbacks.
    std::swap(slot.buffer, buffer);
    slot.bufferSize = bufferSize;
    slot.bytesPerRow = bytesPerRow;
    slot.dest = pending.dest;
    slot.format = pending.format;
    slot.width = pending.width;
    slot.height = pending.height;
    slot.hostWidth = texture->size.width;
    slot.hostHeight = texture->size.height;
    slot.order = texture_pixel_order(texture);
    slot.state = AsyncState::CopySubmitted;
  }
  g_asyncSealed.clear();
}

void after_submit() noexcept {
  struct PendingMap {
    void* dest;
    wgpu::Buffer buffer;
    uint64_t bufferSize;
    uint64_t generation;
  };
  std::vector<PendingMap> pendingMaps;
  {
    std::lock_guard lock{g_asyncMutex};
    for (auto& [dest, slot] : g_asyncSlots) {
      if (slot.state != AsyncState::CopySubmitted) {
        continue;
      }
      slot.state = AsyncState::MapPending;
      ++g_asyncMapsInFlight;
      pendingMaps.push_back({dest, slot.buffer, slot.bufferSize, g_asyncGeneration});
    }
  }

  for (const auto& pending : pendingMaps) {
    pending.buffer.MapAsync(wgpu::MapMode::Read, 0, pending.bufferSize, wgpu::CallbackMode::AllowSpontaneous,
                            [dest = pending.dest, generation = pending.generation](wgpu::MapAsyncStatus status,
                                                                                 wgpu::StringView message) {
                              complete_async_slot(dest, generation, status, message);
                            });
  }

  // Nothing else pumps the WebGPU event queue on this thread, so a completed map would sit
  // unharvested with its slot busy. ProcessEvents does not block.
  drain_async_events();
}

void abort_async() noexcept { g_asyncSealed.clear(); }

void shutdown() noexcept {
  cancel();
  g_asyncSealed.clear();
  // Retire callbacks before releasing buffers, and release outside their mutex:
  // destruction may itself deliver an AllowSpontaneous cancellation callback.
  decltype(g_asyncSlots) retiredSlots;
  {
    std::lock_guard lock{g_asyncMutex};
    ++g_asyncGeneration;
    retiredSlots.swap(g_asyncSlots);
    g_asyncMapsInFlight = 0;
  }
}

} // namespace aurora::gfx::efb_ram
