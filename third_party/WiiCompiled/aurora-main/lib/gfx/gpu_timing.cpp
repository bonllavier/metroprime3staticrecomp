#include "gpu_timing.hpp"

#include <aurora/gfx.h>

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>

namespace aurora::gfx::gpu_timing {
namespace {
Module Log("aurora::gfx::gpu_timing");

constexpr uint32_t kPairsPerFrame = 2048;  // passes timed per frame; later passes go untimed
constexpr size_t kSlots = 4;              // frames in flight
constexpr size_t kCategories = static_cast<size_t>(Category::Count);

enum class SlotState : uint8_t { Free, Recording, Submitted, Mapping };
struct Slot {
  wgpu::QuerySet querySet;
  wgpu::Buffer resolve;
  wgpu::Buffer readback;
  std::array<uint8_t, kPairsPerFrame> categories{};
  std::array<const char*, kPairsPerFrame> labels{};
  uint32_t pairs = 0;
  SlotState state = SlotState::Free;
};

std::mutex g_mutex;
std::array<Slot, kSlots> g_slots;
int g_recording = -1;
wgpu::PassTimestampWrites g_writes;
bool g_ready = false;
bool g_failed = false;

// Accumulated since the last report (map callbacks).
struct Totals {
  uint32_t frames = 0;
  std::array<double, kCategories> sum{};
  std::array<double, kCategories> worst{};
  double totalSum = 0.0, totalWorst = 0.0;
  uint32_t passes = 0;
  std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
};
Totals g_totals;

// Per labelled pass (post-process effects), averaged over the frames of the report.
struct LabelTotal {
  const char* label = nullptr;
  double sum = 0.0, worst = 0.0;
};
constexpr size_t kMaxLabels = 64;
std::array<LabelTotal, kMaxLabels> g_labels{};
size_t g_labelCount = 0;
std::atomic_bool g_captureActive{false};

void add_label_time(const char* label, double ms) {
  // g_mutex held. Labels are string literals: the pointer identifies them.
  size_t i = 0;
  while (i < g_labelCount && g_labels[i].label != label) ++i;
  if (i == g_labelCount) {
    if (g_labelCount == kMaxLabels) return;
    g_labels[g_labelCount++] = LabelTotal{label, 0.0, 0.0};  // the slot may hold an earlier report's sums
  }
  g_labels[i].sum += ms;
  g_labels[i].worst = std::max(g_labels[i].worst, ms);
}

bool ensure_ready() {
  if (g_ready) return true;
  if (g_failed || !webgpu::g_device || !webgpu::g_device.HasFeature(wgpu::FeatureName::TimestampQuery)) {
    g_failed = true;
    return false;
  }
  for (auto& slot : g_slots) {
    const wgpu::QuerySetDescriptor qsd{.label = "GPU timing queries", .type = wgpu::QueryType::Timestamp,
                                       .count = kPairsPerFrame * 2};
    slot.querySet = webgpu::g_device.CreateQuerySet(&qsd);
    const wgpu::BufferDescriptor rd{.label = "GPU timing resolve",
                                    .usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc,
                                    .size = uint64_t{kPairsPerFrame} * 2 * sizeof(uint64_t)};
    slot.resolve = webgpu::g_device.CreateBuffer(&rd);
    const wgpu::BufferDescriptor bd{.label = "GPU timing readback",
                                    .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                    .size = uint64_t{kPairsPerFrame} * 2 * sizeof(uint64_t)};
    slot.readback = webgpu::g_device.CreateBuffer(&bd);
  }
  g_ready = true;
  Log.info("GPU timing on: timestamp queries around game, copy and post-process passes");
  return true;
}

void report_if_due() {
  // g_mutex held.
  const auto now = std::chrono::steady_clock::now();
#if defined(MKW_PRIME_PERF_CAPTURE)
  constexpr auto kInterval = std::chrono::seconds(1);
#else
  constexpr auto kInterval = std::chrono::seconds(5);
#endif
  if (now - g_totals.since < kInterval || g_totals.frames == 0) return;
  const double n = g_totals.frames;
  Log.info("[gpu-time] frames={} avg_ms total={:.2f} game={:.2f} copy={:.2f} post={:.2f} | worst_ms total={:.2f} "
           "game={:.2f} copy={:.2f} post={:.2f} | passes/frame={:.0f}",
           g_totals.frames, g_totals.totalSum / n, g_totals.sum[0] / n, g_totals.sum[1] / n, g_totals.sum[2] / n,
           g_totals.totalWorst, g_totals.worst[0], g_totals.worst[1], g_totals.worst[2], g_totals.passes / n);
  if (g_labelCount != 0) {
    std::array<size_t, kMaxLabels> order{};
    for (size_t i = 0; i < g_labelCount; ++i) order[i] = i;
    std::sort(order.begin(), order.begin() + g_labelCount,
              [](size_t a, size_t b) { return g_labels[a].sum > g_labels[b].sum; });
    std::string line;
    for (size_t k = 0; k < g_labelCount; ++k) {
      const auto& l = g_labels[order[k]];
      line += fmt::format(" | {} avg={:.2f} worst={:.2f}", l.label, l.sum / n, l.worst);
    }
    Log.info("[gpu-pass] frames={}{}", g_totals.frames, line);
  }
  g_labelCount = 0;
  g_totals = Totals{};
  g_totals.since = now;
}
}  // namespace

void set_capture_active(bool active) noexcept {
#if defined(MKW_PRIME_PERF_CAPTURE)
  if (g_captureActive.exchange(active) == active) return;
  std::lock_guard lock(g_mutex);
  g_totals = Totals{};  // a capture's report starts clean
  g_labelCount = 0;
  Log.info("[gpu-time] capture {}", active ? "started" : "stopped");
#else
  (void)active;
#endif
}

bool requested() noexcept {
#if defined(MKW_PRIME_PERF_CAPTURE)
  return g_captureActive.load(std::memory_order_relaxed);
#elif !defined(MKW_PRIME_QA_TOOLS)
  return false;  // test builds only
#else
  static const bool value = [] {
    const char* v = std::getenv("WII_COMPILED_GPU_TIMING");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return value;
#endif
}

const wgpu::PassTimestampWrites* pass_writes(Category category, const char* label) noexcept {
  if (!requested()) return nullptr;
  std::lock_guard lock(g_mutex);
  if (!ensure_ready()) return nullptr;
  if (g_recording < 0) {
    for (size_t i = 0; i < g_slots.size(); ++i) {
      if (g_slots[i].state == SlotState::Free) {
        g_recording = static_cast<int>(i);
        g_slots[i].state = SlotState::Recording;
        g_slots[i].pairs = 0;
        break;
      }
    }
    if (g_recording < 0) return nullptr;  // every slot still in flight: skip this frame
  }
  auto& slot = g_slots[static_cast<size_t>(g_recording)];
  if (slot.pairs >= kPairsPerFrame) return nullptr;
  const uint32_t pair = slot.pairs++;
  slot.categories[pair] = static_cast<uint8_t>(category);
  slot.labels[pair] = label;
  g_writes = wgpu::PassTimestampWrites{
      .querySet = slot.querySet,
      .beginningOfPassWriteIndex = pair * 2,
      .endOfPassWriteIndex = pair * 2 + 1,
  };
  return &g_writes;
}

void encode_resolve(const wgpu::CommandEncoder& encoder) noexcept {
  // Not requested(): a frame recorded just before a capture stops must still be resolved.
  std::lock_guard lock(g_mutex);
  if (!g_ready) return;
  if (g_recording < 0) return;
  auto& slot = g_slots[static_cast<size_t>(g_recording)];
  g_recording = -1;
  if (slot.pairs == 0) {
    slot.state = SlotState::Free;
    return;
  }
  const uint32_t count = slot.pairs * 2;
  encoder.ResolveQuerySet(slot.querySet, 0, count, slot.resolve, 0);
  encoder.CopyBufferToBuffer(slot.resolve, 0, slot.readback, 0, uint64_t{count} * sizeof(uint64_t));
  slot.state = SlotState::Submitted;
}

void after_submit() noexcept {
  std::array<size_t, kSlots> pending{};
  size_t count = 0;
  {
    std::lock_guard lock(g_mutex);
    if (!g_ready) return;
    for (size_t i = 0; i < g_slots.size(); ++i) {
      if (g_slots[i].state != SlotState::Submitted) continue;
      g_slots[i].state = SlotState::Mapping;
      pending[count++] = i;
    }
  }
  for (size_t k = 0; k < count; ++k) {
    const size_t idx = pending[k];
    auto& slot = g_slots[idx];
    const uint64_t bytes = uint64_t{slot.pairs} * 2 * sizeof(uint64_t);
    slot.readback.MapAsync(
        wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::AllowSpontaneous,
        [idx, bytes](wgpu::MapAsyncStatus status, wgpu::StringView) {
          std::lock_guard lock(g_mutex);
          auto& s = g_slots[idx];
          if (status == wgpu::MapAsyncStatus::Success) {
            const auto* ts = static_cast<const uint64_t*>(s.readback.GetConstMappedRange(0, bytes));
            if (ts != nullptr) {
              std::array<double, kCategories> frame{};
              double total = 0.0;
              for (uint32_t p = 0; p < s.pairs; ++p) {
                const uint64_t begin = ts[p * 2], end = ts[p * 2 + 1];
                if (end <= begin) continue;
                const double ms = static_cast<double>(end - begin) / 1.0e6;  // nanoseconds
                if (ms > 1000.0) continue;  // a pass whose timestamps were never written
                frame[s.categories[p]] += ms;
                if (s.labels[p] != nullptr) add_label_time(s.labels[p], ms);
                total += ms;
              }
              ++g_totals.frames;
              g_totals.passes += s.pairs;
              g_totals.totalSum += total;
              g_totals.totalWorst = std::max(g_totals.totalWorst, total);
              for (size_t c = 0; c < kCategories; ++c) {
                g_totals.sum[c] += frame[c];
                g_totals.worst[c] = std::max(g_totals.worst[c], frame[c]);
              }
            }
            s.readback.Unmap();
          }
          s.state = SlotState::Free;
          report_if_due();
        });
  }
}

}  // namespace aurora::gfx::gpu_timing

void aurora_set_gpu_timing_capture(bool active) { aurora::gfx::gpu_timing::set_capture_active(active); }
