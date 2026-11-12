#include <aurora/aurora.h>

#ifdef AURORA_ENABLE_GX
#include "diagnostic_env.hpp"
#include "gfx/post_process.hpp"
#include "gfx/common.hpp"
#include "gfx/tex_cmpr_decode.hpp"
#include "gfx/draw_dump.hpp"
#include "gfx/efb_ram_copy.hpp"
#include "gfx/gpu_timing.hpp"
#include "gfx/texture_upscale.hpp"
#include "gfx/texture_replacement.hpp"
#include "gx/fifo.hpp"
#include "gx/command_processor.hpp"
#include "gx/shader_info.hpp"
#include "imgui.hpp"
#include "webgpu/gpu.hpp"
#include "webgpu/metalfx.hpp"
#include <webgpu/webgpu_cpp.h>
#endif

#include "display_refresh.hpp"
#include "input.hpp"
#include "internal.hpp"
#include "window.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_thread.h>
#include <magic_enum.hpp>

#include "system_info.hpp"
#include "tracy/Tracy.hpp"

#if defined(__linux__)
#include <aurora/host_cores.h>
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef AURORA_ENABLE_GX
namespace aurora::gfx { TextureHandle diagnostic_resolve_snapshot(); TextureHandle diagnostic_isolated_glyph(); wgpu::Buffer diagnostic_vertex_result(); wgpu::Buffer diagnostic_raster_coverage(); }
namespace aurora::gx {
// Producer pacing feedback for the adaptive slot count, defined in lib/gx/shader_info.cpp;
// declared here so the C entry point at the bottom of this file can forward to it.
void report_producer_paced(bool paced) noexcept;
void set_uber_hybrid(bool enabled) noexcept; // lib/gx/pipeline.cpp
extern std::atomic<bool> g_perPixelLighting; // lib/gx/gx.cpp
} // namespace aurora::gx
#endif

namespace aurora {
AuroraConfig g_config;
uint32_t g_sdlCustomEventsStart;
char g_gameName[4];
std::atomic<AuroraFrameWorkerWaitCallback> g_frameWorkerWaitCallback{nullptr};
// Presentation schedule for the frame being sealed, set by the producer. Jobs carry absolute
// deadlines derived from it, so the presenter cannot drift. Zero means present when ready.
std::atomic<uint64_t> g_presentScheduleBaseNanos{0};
std::atomic<uint64_t> g_presentScheduleIntervalNanos{0};
std::atomic<bool> g_metalfxRequested{false};
std::atomic<bool> g_preserveEfbAcrossFrames{false};
std::atomic<bool> g_metalfxSupported{false};
std::atomic<AuroraMetalFXStatus> g_metalfxStatus{AURORA_METALFX_DISABLED};

namespace {
Module Log("aurora");

std::atomic<uint32_t> g_captureFrame{UINT32_MAX};
std::string g_captureOutputPath;
// F11 screenshot request: captured from the final presentation image (after the image effects).
std::string g_screenshotPath;
std::atomic<bool> g_screenshotPending{false};
// Frame number 0 never carries a diagnostic capture; the bug report path uses
// it to mean "whatever frame is presented next".
constexpr uint32_t kCaptureNextFrame = 0;
std::mutex g_captureRequestMutex;

using PresentClock = std::chrono::steady_clock;

struct PresentTimingSample {
  PresentClock::time_point presentedAt{};
  std::chrono::nanoseconds interval{};
  // Slot carried a copy of the native image instead of replayed interpolation, so the present
  // counts toward the rate but shows no new motion.
  bool duplicated = false;
};

std::mutex g_presentTimingMutex;
std::array<PresentTimingSample, 512> g_presentTimingSamples{};
size_t g_presentTimingWriteIndex = 0;
size_t g_presentTimingSampleCount = 0;
std::optional<PresentClock::time_point> g_lastSuccessfulPresent;
uint64_t g_totalPresentCount = 0;

void wait_until_precise(PresentClock::time_point deadline) noexcept {
  // Busy-spin tail covering the waitable timer's wakeup jitter. 150 us was not enough in
  // practice; 400 us absorbs the observed overshoot, about 1.6 ms of one core at 240 Hz.
  constexpr auto kSpinWindow = std::chrono::microseconds(400);
  auto now = PresentClock::now();
  if (now >= deadline) {
    return;
  }

  const auto timerDeadline = deadline - now > kSpinWindow ? deadline - kSpinWindow : deadline;
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
  struct HighResolutionTimer {
    HANDLE handle = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_MODIFY_STATE | SYNCHRONIZE);
    ~HighResolutionTimer() {
      if (handle != nullptr) {
        ::CloseHandle(handle);
      }
    }
  };
  static thread_local HighResolutionTimer timer;
  if (timer.handle != nullptr && timerDeadline > now) {
    const auto remaining100ns =
        std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
            timerDeadline - now);
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<int64_t>(remaining100ns.count(), 1);
    if (::SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
      ::WaitForSingleObject(timer.handle, INFINITE);
    }
  } else
#endif
  {
    std::this_thread::sleep_until(timerDeadline);
  }

  while (PresentClock::now() < deadline) {
#if defined(_WIN32)
    YieldProcessor();
#else
    std::this_thread::yield();
#endif
  }
}

void record_successful_present(bool, uint32_t,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds jobDuration, bool duplicated,
                               uint32_t, std::chrono::nanoseconds lateBy) noexcept {
  const auto now = PresentClock::now();
  std::chrono::nanoseconds interval{};
  {
    std::lock_guard lock(g_presentTimingMutex);
    if (g_lastSuccessfulPresent) {
      interval = std::chrono::duration_cast<std::chrono::nanoseconds>(now - *g_lastSuccessfulPresent);
    }
    g_lastSuccessfulPresent = now;
    g_presentTimingSamples[g_presentTimingWriteIndex] = {
        .presentedAt = now,
        .interval = interval,
        .duplicated = duplicated,
    };
    g_presentTimingWriteIndex = (g_presentTimingWriteIndex + 1) % g_presentTimingSamples.size();
    g_presentTimingSampleCount = std::min(g_presentTimingSampleCount + 1, g_presentTimingSamples.size());
    ++g_totalPresentCount;
  }
  // Display hitches the game thread does not see (its VI cadence stays on time):
  // a long job here is the presenter itself, a long gap with a short job is a
  // frame that reached the presenter late (frame worker or GPU queue).
  if (interval >= std::chrono::milliseconds(40)) {
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 2000) {
      Log.info("[present-gap] interval_ms={:.1f} presentation_job_ms={:.1f} late_by_ms={:.1f}",
               std::chrono::duration<double, std::milli>(interval).count(),
               std::chrono::duration<double, std::milli>(jobDuration).count(),
               std::chrono::duration<double, std::milli>(lateBy).count());
    }
  }
}

AuroraPresentTiming snapshot_present_timing() noexcept {
  constexpr auto kWindow = std::chrono::seconds(1);
  const auto cutoff = PresentClock::now() - kWindow;
  std::vector<double> milliseconds;
  uint64_t totalPresentCount = 0;
  size_t newMotionSamples = 0;
  {
    std::lock_guard lock(g_presentTimingMutex);
    totalPresentCount = g_totalPresentCount;
    milliseconds.reserve(g_presentTimingSampleCount);
    for (size_t i = 0; i < g_presentTimingSampleCount; ++i) {
      const auto& sample = g_presentTimingSamples[i];
      if (sample.presentedAt >= cutoff && sample.interval.count() > 0) {
        milliseconds.push_back(
            std::chrono::duration<double, std::milli>(sample.interval).count());
        if (!sample.duplicated) {
          ++newMotionSamples;
        }
      }
    }
  }

  AuroraPresentTiming result{
      .totalPresentCount = totalPresentCount,
      .sampleCount = static_cast<uint32_t>(milliseconds.size()),
  };
  if (milliseconds.empty()) {
    return result;
  }

  double totalMilliseconds = 0.0;
  for (const double value : milliseconds) {
    totalMilliseconds += value;
  }
  result.averageFrameTimeMs = totalMilliseconds / static_cast<double>(milliseconds.size());
  result.framesPerSecond = result.averageFrameTimeMs > 0.0 ? 1000.0 / result.averageFrameTimeMs : 0.0;
  // Duplicated presentation slots keep the presented cadence but carry no new
  // motion; scale them out so this reads as the rate the eye actually sees.
  result.effectiveFramesPerSecond =
      result.framesPerSecond *
      (static_cast<double>(newMotionSamples) / static_cast<double>(milliseconds.size()));
  for (const double value : milliseconds) {
    const double difference = value - result.averageFrameTimeMs;
    result.jitterMs += difference * difference;
  }
  result.jitterMs = std::sqrt(result.jitterMs / static_cast<double>(milliseconds.size()));
  std::sort(milliseconds.begin(), milliseconds.end());
  result.p95FrameTimeMs =
      milliseconds[static_cast<size_t>(std::floor(static_cast<double>(milliseconds.size() - 1) * 0.95))];
  return result;
}

// ImGui draw data lives in the shared context, so starting the next ImGui frame destroys draw
// lists the sealed slots still replay. That is why ImGui callers wait for DONE, not SEALED.
enum class ImGuiFramePolicy {
  // Start ImGui's next frame as part of renderer preparation, as the
  // synchronous path always has.
  Immediate,
  // Leave it unstarted; the caller owes an imgui::new_frame() once it has
  // finished replaying this frame's draw data.
  Deferred,
};

bool begin_frame_impl(bool pumpEvents, ImGuiFramePolicy imguiPolicy = ImGuiFramePolicy::Immediate,
                      bool* imguiNewFrameOwed = nullptr) noexcept;
bool begin_frame_render_state_impl(ImGuiFramePolicy imguiPolicy, bool* imguiNewFrameOwed) noexcept;
void end_frame_impl(bool pumpEvents, bool drainFifo);

// The two publication points of a frame-worker cycle, cleared together under `mutex`. Sealed:
// producer-shared renderer state is free again. Done: slots encoded, presented, ImGui restarted.
enum class FrameWorkerPhase {
  Sealed,
  Done,
};

// Native ids of the frame worker and presenter, for host profilers (0 = not started).
std::atomic<uint32_t> g_frameWorkerNativeId{0};
std::atomic<uint32_t> g_presenterNativeId{0};
uint32_t current_native_thread_id() noexcept {
#ifdef _WIN32
  return static_cast<uint32_t>(GetCurrentThreadId());
#elif defined(__linux__)
  return host_cores::native_thread_id();
#else
  return 0;
#endif
}

struct FrameWorkerState {
  std::mutex mutex;
  std::condition_variable cv;
  std::thread thread;
  std::thread::id threadId{};
  bool started = false;
  bool stop = false;
  bool jobPending = false;
  // Readiness is polled thousands of times per frame, so these flags double as a publication
  // barrier. `sealed` is released before `ready`, and both are cleared under `mutex`.
  std::atomic_bool sealed{true};
  std::atomic_bool ready{true};
  bool framePrepared = false;
  bool prepareAllowed = false;
};

FrameWorkerState g_frameWorker;
// steady_clock time of the latest successful surface Present (latency diagnostics).
std::atomic<int64_t> g_lastPresentSteadyNanos{0};
// Game image rectangle inside the window, as fractions of the surface: left, top, width, height.
std::array<std::atomic<float>, 4> g_presentViewportFraction{0.f, 0.f, 1.f, 1.f};

bool frame_worker_requested() noexcept {
#ifdef AURORA_ENABLE_GX
  static const bool enabled = [] {
#if defined(__APPLE__)
    // ImGui's SDL backend may raise an SDL window from ImGui::NewFrame(). On
    // macOS that reaches AppKit, whose window operations are main-thread-only;
    // doing it on the frame worker terminates the process with EXC_BREAKPOINT.
    // Keep all SDL/ImGui work on the calling thread until the worker no longer
    // owns frame preparation on Apple platforms.
    return false;
#endif
#if defined(_WIN32)
    // RenderDoc's D3D12 layer is injected before Aurora starts and needs device and command
    // ownership on one thread, so keep frame submission synchronous there.
    if (::GetModuleHandleW(L"renderdoc.dll") != nullptr) {
      return false;
    }
#endif
    return true;
  }();
#if defined(_WIN32)
  static const bool renderDocLoaded = ::GetModuleHandleW(L"renderdoc.dll") != nullptr;
  if (renderDocLoaded) {
    static const bool logged = [] {
      Log.info("Disabled asynchronous frame submission worker for RenderDoc capture");
      return true;
    }();
    (void)logged;
  }
#endif
  return enabled;
#else
  return false;
#endif
}

#ifdef AURORA_ENABLE_GX
// Returns false when a stop request was observed mid-cycle.
bool run_frame_worker_cycle(gfx::SealedFrame& sealedFrame) noexcept;
#endif

void frame_worker_main() noexcept {
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.threadId = std::this_thread::get_id();
  }
  g_frameWorkerNativeId.store(current_native_thread_id(), std::memory_order_relaxed);
#ifdef _WIN32
  // Encodes every frame and waits on pipelines; it competes with the game and GX
  // threads only, never with background compiles or decodes.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  SetThreadDescription(GetCurrentThread(), L"Aurora frame worker");
#elif defined(__linux__)
  host_cores::set_thread_name("Aurora frame");
  if (!host_cores::raise_thread_priority())
    Log.warn("Could not raise the frame worker thread priority: {}", SDL_GetError());
#endif

#ifdef AURORA_ENABLE_GX
  // Owned by the worker for its whole lifetime so the sealed pass vector and
  // its pooled command lists keep their capacity across frames.
  gfx::SealedFrame sealedFrame;
#endif

  for (;;) {
    {
      std::unique_lock lock(g_frameWorker.mutex);
      g_frameWorker.cv.wait(lock, [] { return g_frameWorker.stop || g_frameWorker.jobPending; });
      if (g_frameWorker.stop) {
        break;
      }
      g_frameWorker.jobPending = false;
    }

    // The CPU already decoded the sealed frame at its GX boundary; the worker only owns
    // encode/submit/present, so it never touches the producer's next FIFO buffer.
#ifdef AURORA_ENABLE_GX
    if (!run_frame_worker_cycle(sealedFrame)) {
      break;
    }
#endif
  }

  // A stop request can unblock either wait above mid-cycle, so release both phases before
  // stop_frame_worker() joins.
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.sealed.store(true, std::memory_order_release);
    g_frameWorker.ready.store(true, std::memory_order_release);
  }
  g_frameWorker.cv.notify_all();
}

void ensure_frame_worker_started() noexcept {
  if (!frame_worker_requested()) {
    return;
  }
  std::lock_guard lock(g_frameWorker.mutex);
  if (g_frameWorker.started) {
    return;
  }
  g_frameWorker.stop = false;
  g_frameWorker.jobPending = false;
  g_frameWorker.sealed.store(true, std::memory_order_release);
  g_frameWorker.ready.store(true, std::memory_order_release);
  g_frameWorker.prepareAllowed = false;
  g_frameWorker.started = true;
  g_frameWorker.thread = std::thread(frame_worker_main);
  Log.info("Enabled bounded asynchronous frame submission worker");
}

bool frame_worker_phase_reached(FrameWorkerPhase phase) noexcept {
  return phase == FrameWorkerPhase::Sealed ? g_frameWorker.sealed.load(std::memory_order_acquire)
                                           : g_frameWorker.ready.load(std::memory_order_acquire);
}

bool wait_for_frame_worker_private_for(FrameWorkerPhase phase, std::chrono::microseconds timeout) noexcept;

void wait_for_frame_worker_private(FrameWorkerPhase phase) noexcept {
  constexpr auto kWaitServiceInterval = std::chrono::milliseconds(1);
  while (!wait_for_frame_worker_private_for(phase, kWaitServiceInterval)) {
  }
}

bool wait_for_frame_worker_private_for(FrameWorkerPhase phase, std::chrono::microseconds timeout) noexcept {
  // Each phase's state is published before its flag is set, which keeps the common
  // display-list path out of the worker control mutex.
  if (frame_worker_phase_reached(phase)) {
    return true;
  }

  std::unique_lock lock(g_frameWorker.mutex);
  if (!g_frameWorker.started || g_frameWorker.threadId == std::this_thread::get_id()) {
    return true;
  }
  if (timeout <= std::chrono::microseconds::zero()) {
    return frame_worker_phase_reached(phase);
  }

  const bool reached =
      g_frameWorker.cv.wait_for(lock, timeout, [phase] { return frame_worker_phase_reached(phase); });
  if (reached) {
    return true;
  }

  lock.unlock();
  // Both phases service the guest's alarm/retrace pump identically; the
  // producer must keep its own timing alive however long it waits.
  // The asynchronous decoder thread never runs host timing or guest code.
  if (const auto callback = g_frameWorkerWaitCallback.load(std::memory_order_acquire);
      callback != nullptr && !gx::fifo::on_decoder_thread()) {
    callback();
  }
  return false;
}

void stop_frame_worker() noexcept {
  {
    std::lock_guard lock(g_frameWorker.mutex);
    if (!g_frameWorker.started) {
      return;
    }
    g_frameWorker.stop = true;
    g_frameWorker.prepareAllowed = true;
  }
  g_frameWorker.cv.notify_all();
  if (g_frameWorker.thread.joinable()) {
    g_frameWorker.thread.join();
  }
  std::lock_guard lock(g_frameWorker.mutex);
  g_frameWorker.started = false;
  g_frameWorker.threadId = {};
  g_frameWorker.framePrepared = false;
}

uint32_t align_to(uint32_t value, uint32_t alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

void append_u16(std::ofstream& out, uint16_t value) {
  const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8)};
  out.write(bytes, sizeof(bytes));
}

void append_u32(std::ofstream& out, uint32_t value) {
  const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8),
                        static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
  out.write(bytes, sizeof(bytes));
}

bool write_bmp(const char* path, const uint8_t* pixels, uint32_t width, uint32_t height,
               uint32_t bytesPerRow, bool bgra) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  constexpr uint32_t pixelOffset = 14 + 40;
  const uint32_t imageSize = width * height * 4;
  out.write("BM", 2);
  append_u32(out, pixelOffset + imageSize);
  append_u16(out, 0);
  append_u16(out, 0);
  append_u32(out, pixelOffset);
  append_u32(out, 40);
  append_u32(out, width);
  append_u32(out, height);
  append_u16(out, 1);
  append_u16(out, 32);
  append_u32(out, 0);
  append_u32(out, imageSize);
  append_u32(out, 2835);
  append_u32(out, 2835);
  append_u32(out, 0);
  append_u32(out, 0);
  std::vector<uint8_t> row(width * 4);
  for (uint32_t y = height; y-- > 0;) {
    const auto* source = pixels + static_cast<size_t>(y) * bytesPerRow;
    if (bgra) {
      out.write(reinterpret_cast<const char*>(source), row.size());
    } else {
      for (uint32_t x = 0; x < width; ++x) {
        row[x * 4 + 0] = source[x * 4 + 2];
        row[x * 4 + 1] = source[x * 4 + 1];
        row[x * 4 + 2] = source[x * 4 + 0];
        row[x * 4 + 3] = source[x * 4 + 3];
      }
      out.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
  }
  return out.good();
}

#ifdef AURORA_ENABLE_GX
// GPU
using webgpu::g_device;
using webgpu::g_instance;
using webgpu::g_queue;
using webgpu::g_surface;
std::recursive_mutex g_rendererGpuMutex;
std::mutex g_queueSubmitMutex;
// Surface ownership, held apart from the renderer mutex: a presentation slot touches the surface,
// its image and the queue, nothing recorded. Lock order is surface then renderer.
std::mutex g_surfaceMutex;
std::atomic<bool> g_surfaceReconfigurePending{false};
std::atomic<bool> g_surfaceRecreatePending{false};

void request_surface_reconfigure() noexcept {
  g_surfaceReconfigurePending.store(true, std::memory_order_release);
}

void request_surface_recreate() noexcept {
  g_surfaceRecreatePending.store(true, std::memory_order_release);
  g_surfaceReconfigurePending.store(true, std::memory_order_release);
}

struct PendingFrameCapture {
  wgpu::Buffer buffer;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bytesPerRow = 0;
  uint64_t bufferSize = 0;
  bool bgra = false;
  bool rawBuffers = false;
  std::string path;
  std::unique_ptr<PendingFrameCapture> accompanyingBuffers;
};

PendingFrameCapture encode_gpu_buffer_capture(const wgpu::CommandEncoder& encoder, const std::string& path) {
  const bool large=std::getenv("WII_COMPILED_GPU_CAPTURE_LARGE_BUFFERS")!=nullptr;
  const uint64_t vertexBytes=large?65536:8192,uniformBytes=large?131072:4096,indexBytes=large?16384:1024;
  const wgpu::BufferDescriptor desc{.label="GPU buffer diagnostic readback",
      .usage=wgpu::BufferUsage::CopyDst|wgpu::BufferUsage::MapRead,
      .size=vertexBytes+uniformBytes+indexBytes};
  auto buffer = g_device.CreateBuffer(&desc);
  encoder.CopyBufferToBuffer(gfx::g_vertexBuffer,0,buffer,0,vertexBytes);
  encoder.CopyBufferToBuffer(gfx::g_uniformBuffer,0,buffer,vertexBytes,uniformBytes);
  encoder.CopyBufferToBuffer(gfx::g_indexBuffer,0,buffer,vertexBytes+uniformBytes,indexBytes);
  return PendingFrameCapture{.buffer=std::move(buffer),.bufferSize=desc.size,.rawBuffers=true,.path=path};
}

std::optional<PendingFrameCapture> encode_frame_capture(const wgpu::CommandEncoder& encoder,
                                                        const webgpu::PresentSource& originalSource,
                                                        bool drawDumpFrame = false,
                                                        const std::string& drawDumpBase = {}) {
  auto source=originalSource;
  // The image of a draw-dump frame, captured as it is encoded.
  if (drawDumpFrame) {
    g_captureOutputPath=drawDumpBase+".bmp";
    g_captureFrame.store(gfx::current_frame(),std::memory_order_release);
  }
  // Opt-in, one-shot GPU image capture requested without replaying gameplay.
  // Poll at most once per second; the embedder chooses the private marker path.
  static const std::string captureMarker = [] {
    const char* value=std::getenv("WII_COMPILED_GPU_CAPTURE_MARKER");
    return value ? std::string(value) : std::string{};
  }();
  if (!captureMarker.empty()) {
    static auto nextPoll=std::chrono::steady_clock::now();
    const auto now=std::chrono::steady_clock::now();
    if (now>=nextPoll) {
      nextPoll=now+std::chrono::seconds(1);
      std::ifstream marker(captureMarker);
      if (marker.good()) {
        marker.close();
        if (std::remove(captureMarker.c_str())==0) {
          g_captureOutputPath=captureMarker+".bmp";
          g_captureFrame.store(gfx::current_frame(),std::memory_order_release);
        }
      }
    }
  }
  if (std::getenv("WII_COMPILED_GPU_CAPTURE_ISOLATED_GLYPH")) {
    if (auto target=gfx::diagnostic_isolated_glyph())
      source={.texture=target->texture,.size=target->size,.format=target->format};
  }
  // Optional diagnostic: read the actual GPU presentation source, independent
  // of desktop/window capture. Uses the existing bounded readback path.
  static bool environmentCaptureRequested = false;
  if (!environmentCaptureRequested) {
    environmentCaptureRequested = true;
    if (const char* path = std::getenv("WII_COMPILED_GPU_CAPTURE_PATH")) {
      g_captureOutputPath = path;
      const char* requested = std::getenv("WII_COMPILED_GPU_CAPTURE_FRAME");
      g_captureFrame.store(requested ? static_cast<uint32_t>(std::strtoul(requested,nullptr,10)) : 60,
                           std::memory_order_release);
    }
  }
  uint32_t requestedFrame = g_captureFrame.load(std::memory_order_acquire);
  const uint32_t currentFrame = gfx::current_frame();
  if (requestedFrame == UINT32_MAX || currentFrame < requestedFrame) return std::nullopt;
  g_captureFrame.store(UINT32_MAX, std::memory_order_release);
  if (requestedFrame == kCaptureNextFrame) {
    std::lock_guard<std::mutex> lock(g_captureRequestMutex);
    requestedFrame = currentFrame;
  }
  if (currentFrame != requestedFrame) {
    Log.error("Missed requested frame capture {} (current frame {})", requestedFrame, currentFrame);
    return std::nullopt;
  }
  if (std::getenv("WII_COMPILED_GPU_CAPTURE_BUFFERS"))
    return encode_gpu_buffer_capture(encoder,g_captureOutputPath);
  if (!source.texture || source.size.width == 0 || source.size.height == 0) {
    Log.error("Frame {} capture has no present-source texture", currentFrame);
    return std::nullopt;
  }
  const bool bgra = source.format == wgpu::TextureFormat::BGRA8Unorm ||
                    source.format == wgpu::TextureFormat::BGRA8UnormSrgb;
  const bool rgba = source.format == wgpu::TextureFormat::RGBA8Unorm ||
                    source.format == wgpu::TextureFormat::RGBA8UnormSrgb;
  if (!bgra && !rgba) {
    Log.error("Frame {} capture does not support texture format {}", currentFrame,
              magic_enum::enum_name(source.format));
    return std::nullopt;
  }
  const uint32_t bytesPerRow = align_to(source.size.width * 4, 256);
  const uint64_t bufferSize = static_cast<uint64_t>(bytesPerRow) * source.size.height;
  const wgpu::BufferDescriptor descriptor{
      .label = "Visual validation frame capture",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
      .size = bufferSize,
  };
  auto buffer = g_device.CreateBuffer(&descriptor);
  const wgpu::TexelCopyTextureInfo sourceInfo{
      .texture = source.texture,
      .mipLevel = 0,
      .origin = {0, 0, 0},
      .aspect = wgpu::TextureAspect::All,
  };
  const wgpu::TexelCopyBufferInfo destinationInfo{
      .layout = {.offset = 0, .bytesPerRow = bytesPerRow, .rowsPerImage = source.size.height},
      .buffer = buffer,
  };
  encoder.CopyTextureToBuffer(&sourceInfo, &destinationInfo, &source.size);
  PendingFrameCapture capture{
      .buffer = std::move(buffer),
      .width = source.size.width,
      .height = source.size.height,
      .bytesPerRow = bytesPerRow,
      .bufferSize = bufferSize,
      .bgra = bgra,
      .path = g_captureOutputPath,
  };
  if (std::getenv("WII_COMPILED_GPU_CAPTURE_ALSO_BUFFERS"))
    capture.accompanyingBuffers = std::make_unique<PendingFrameCapture>(
        encode_gpu_buffer_capture(encoder,g_captureOutputPath + ".buffers.bin"));
  if (auto vertices=gfx::diagnostic_vertex_result(); vertices && std::getenv("WII_COMPILED_GPU_VERTEX_SHADER_PATH")) {
    const wgpu::BufferDescriptor desc{.label="Transformed vertex readback",.usage=wgpu::BufferUsage::CopyDst|wgpu::BufferUsage::MapRead,.size=128};
    auto buffer=g_device.CreateBuffer(&desc);
    encoder.CopyBufferToBuffer(vertices,0,buffer,0,128);
    auto* tail=&capture;
    while(tail->accompanyingBuffers) tail=tail->accompanyingBuffers.get();
    tail->accompanyingBuffers=std::make_unique<PendingFrameCapture>(PendingFrameCapture{
        .buffer=std::move(buffer),.bufferSize=128,.rawBuffers=true,.path=g_captureOutputPath+".transformed.bin"});
  }
  if (auto vertices=gfx::diagnostic_raster_coverage(); vertices) {
    constexpr uint64_t bytes=524288;
    const wgpu::BufferDescriptor desc{.label="Actual raster coverage readback",.usage=wgpu::BufferUsage::CopyDst|wgpu::BufferUsage::MapRead,.size=bytes};
    auto buffer=g_device.CreateBuffer(&desc);
    encoder.CopyBufferToBuffer(vertices,0,buffer,0,bytes);
    auto* tail=&capture;
    while(tail->accompanyingBuffers) tail=tail->accompanyingBuffers.get();
    tail->accompanyingBuffers=std::make_unique<PendingFrameCapture>(PendingFrameCapture{
        .buffer=std::move(buffer),.bufferSize=bytes,.rawBuffers=true,.path=g_captureOutputPath+".raster_coverage.bin"});
  }
  return capture;
}

void complete_frame_capture(PendingFrameCapture& capture) {
  wgpu::MapAsyncStatus mapStatus = wgpu::MapAsyncStatus::CallbackCancelled;
  wgpu::StringView mapMessage{};
  const auto future = capture.buffer.MapAsync(
      wgpu::MapMode::Read, 0, capture.bufferSize, wgpu::CallbackMode::WaitAnyOnly,
      [&mapStatus, &mapMessage](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        mapStatus = status;
        mapMessage = message;
      });
  const auto waitStatus = g_instance.WaitAny(future, 5000000000);
  if (waitStatus != wgpu::WaitStatus::Success || mapStatus != wgpu::MapAsyncStatus::Success) {
    Log.error("Frame capture readback failed wait={} map={} message={}", magic_enum::enum_name(waitStatus),
              magic_enum::enum_name(mapStatus), mapMessage);
    return;
  }
  const auto* pixels = static_cast<const uint8_t*>(capture.buffer.GetConstMappedRange(0, capture.bufferSize));
  if (capture.rawBuffers) {
    std::ofstream out(capture.path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(pixels), capture.bufferSize);
    Log.info("Captured GPU buffers to '{}' bytes={} ok={}",capture.path,capture.bufferSize,out.good());
  } else if (write_bmp(capture.path.c_str(), pixels, capture.width, capture.height, capture.bytesPerRow, capture.bgra)) {
    Log.info("Captured rendered frame to '{}' ({}x{})", capture.path, capture.width, capture.height);
  } else {
    Log.error("Failed to write rendered frame capture to '{}'", capture.path);
  }
  capture.buffer.Unmap();
  if (capture.accompanyingBuffers) complete_frame_capture(*capture.accompanyingBuffers);
}
#endif

// AuroraBackend is an anonymous C typedef enum, which magic_enum cannot
// reflect under this toolchain -- name it by hand for diagnostics.
constexpr const char* backend_name(AuroraBackend backend) noexcept {
  switch (backend) {
  case BACKEND_AUTO:
    return "Auto";
  case BACKEND_D3D11:
    return "D3D11";
  case BACKEND_D3D12:
    return "D3D12";
  case BACKEND_METAL:
    return "Metal";
  case BACKEND_VULKAN:
    return "Vulkan";
  case BACKEND_OPENGL:
    return "OpenGL";
  case BACKEND_OPENGLES:
    return "OpenGLES";
  case BACKEND_WEBGPU:
    return "WebGPU";
  case BACKEND_NULL:
    return "Null";
  }
  return "Unknown";
}

#ifdef AURORA_ENABLE_GX
constexpr std::array PreferredBackendOrder{
#ifdef ENABLE_BACKEND_WEBGPU
    BACKEND_WEBGPU,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D12
    BACKEND_D3D12,
#endif
#if defined(_WIN32) && defined(DAWN_ENABLE_BACKEND_D3D11)
    BACKEND_D3D11,
#endif
#ifdef DAWN_ENABLE_BACKEND_METAL
    BACKEND_METAL,
#endif
#ifdef DAWN_ENABLE_BACKEND_VULKAN
    BACKEND_VULKAN,
#endif
#if !defined(_WIN32) && defined(DAWN_ENABLE_BACKEND_D3D11)
    BACKEND_D3D11,
#endif
// #ifdef DAWN_ENABLE_BACKEND_DESKTOP_GL
//     BACKEND_OPENGL,
// #endif
// #ifdef DAWN_ENABLE_BACKEND_OPENGLES
//     BACKEND_OPENGLES,
// #endif
#ifdef DAWN_ENABLE_BACKEND_NULL
    BACKEND_NULL,
#endif
};
#else
constexpr std::array<AuroraBackend, 0> PreferredBackendOrder{};
#endif

bool g_initialFrame = false;


AuroraInfo initialize(int argc, char* argv[], const AuroraConfig& config) noexcept {
  g_config = config;
  Log.info("Aurora initializing");
  log_system_information();
  if (g_config.appName == nullptr) {
    g_config.appName = "Aurora";
  } else {
    g_config.appName = strdup(g_config.appName);
  }
  if (g_config.userPath == nullptr) {
    g_config.userPath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.userPath = strdup(g_config.userPath);
  }
  if (g_config.cachePath == nullptr) {
    g_config.cachePath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.cachePath = strdup(g_config.cachePath);
  }
  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = SDL_GetBasePath();
  } else {
    g_config.resourcesPath = strdup(g_config.resourcesPath);
  }
  if (g_config.pipelineCachePath == nullptr) {
    g_config.pipelineCachePath = g_config.cachePath;
  } else {
    g_config.pipelineCachePath = strdup(g_config.pipelineCachePath);
  }
  if (g_config.msaa == 0) {
    g_config.msaa = 1;
  }
  if (g_config.maxTextureAnisotropy == 0) {
    g_config.maxTextureAnisotropy = 16;
  }
  if (g_config.cacheGeneration != nullptr) {
    g_config.cacheGeneration = strdup(g_config.cacheGeneration);
  }
#ifdef AURORA_ENABLE_GX
  aurora::gx::g_perPixelLighting.store(g_config.perPixelLighting, std::memory_order_relaxed);
  aurora::gx::g_enhancedMetal.store(g_config.enhancedMetal, std::memory_order_relaxed);
  aurora::gx::g_enhancedMetalStrength.store(g_config.enhancedMetalStrength, std::memory_order_relaxed);
  Log.info("render options: enhanced_metal={} strength={:.2f}", g_config.enhancedMetal, g_config.enhancedMetalStrength);
  aurora::gx::set_uber_hybrid(g_config.ubershader);
  Log.info("[uber] hybrid ubershader {}", g_config.ubershader ? "on" : "off");
  aurora::gfx::texture_upscale::set_config(g_config.textureUpscaleFactor, g_config.textureUpscaleMaxSize,
                                           g_config.textureUpscaleSharpen);
  Log.info("render options: per_pixel_lighting={} texture_upscale={}x (sources up to {}px, sharpen {:.2f})",
           g_config.perPixelLighting, aurora::gfx::texture_upscale::config().factor,
           aurora::gfx::texture_upscale::config().maxSourceSize, aurora::gfx::texture_upscale::config().sharpen);
#endif
  if (g_config.msaa != 1 && g_config.msaa != 2 && g_config.msaa != 4 && g_config.msaa != 8) {
    Log.warn("Unsupported msaa={}; using 1", g_config.msaa);
    g_config.msaa = 1;
  }
  ASSERT(window::initialize(), "Error initializing window");

  g_sdlCustomEventsStart = SDL_RegisterEvents(2);
  ASSERT(g_sdlCustomEventsStart, "Failed to allocate user events: {}", SDL_GetError());
  ASSERT(window::initialize_event_watch(), "Error initializing SDL event watch");

#ifdef AURORA_ENABLE_GX
  /* Attempt to create a window using the calling application's desired backend */
  const AuroraBackend requestedBackend = config.desiredBackend;
  AuroraBackend selectedBackend = requestedBackend;
  bool windowCreated = false;
  std::string firstGraphicsError;
  const auto rememberGraphicsError = [&] {
    if (firstGraphicsError.empty() && SDL_GetError()[0] != '\0') {
      firstGraphicsError = SDL_GetError();
    }
  };
  if (selectedBackend != BACKEND_AUTO) {
    Log.info("Requested graphics backend: {}", backend_name(selectedBackend));
    if (window::create_window(selectedBackend)) {
      if (webgpu::initialize(selectedBackend)) {
        windowCreated = true;
      } else {
        rememberGraphicsError();
        window::destroy_window();
      }
    } else {
      rememberGraphicsError();
      Log.error("Failed to create a window for backend {}: {}", backend_name(selectedBackend),
                SDL_GetError());
    }
    if (!windowCreated) {
      /* An explicitly requested backend that cannot be brought up falls back to the BACKEND_AUTO
       * search instead of aborting, and the substitution is always reported. */
      Log.error("Requested graphics backend {} is unavailable on this system; "
                "falling back to automatic selection",
                backend_name(requestedBackend));
    }
  }

  if (!windowCreated) {
    for (const auto backendType : PreferredBackendOrder) {
      selectedBackend = backendType;
      if (!window::create_window(selectedBackend)) {
        rememberGraphicsError();
        continue;
      }
      if (webgpu::initialize(selectedBackend)) {
        windowCreated = true;
        break;
      } else {
        rememberGraphicsError();
        window::destroy_window();
      }
    }
  }

  if (!windowCreated) {
    if (firstGraphicsError.empty()) firstGraphicsError = "No supported graphics backend is available";
    SDL_SetError("%s", firstGraphicsError.c_str());
    Log.error("Graphics initialization failed: {}", firstGraphicsError);
    return {
        .initializationStatus = AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE,
        .initializationError = SDL_GetError(),
    };
  }
  if (requestedBackend != BACKEND_AUTO && selectedBackend != requestedBackend) {
    Log.error("Graphics backend fallback in effect: video.graphics_api requested {}, "
              "running on {}",
              backend_name(requestedBackend), backend_name(selectedBackend));
  }

  // Initialize SDL_Renderer for ImGui when we can't use a Dawn backend
  if (webgpu::g_backendType == wgpu::BackendType::Null) {
    ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
  }
#else
  AuroraBackend selectedBackend = BACKEND_NULL;
  ASSERT(window::create_window(BACKEND_NULL), "Error creating window: {}", SDL_GetError());
  ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
#endif

  window::show_window();

#ifdef AURORA_ENABLE_GX
  gfx::initialize();

  g_metalfxSupported.store(webgpu::metalfx::supported(g_device, webgpu::g_backendType));
  g_metalfxStatus.store(AURORA_METALFX_DISABLED);

  imgui::create_context();
#endif
  const auto size = window::get_window_size();
  Log.info("Using framebuffer size {}x{} scale {}", size.fb_width, size.fb_height, size.scale);
#ifdef AURORA_ENABLE_GX
  if (g_config.imGuiInitCallback != nullptr) {
    g_config.imGuiInitCallback(&size);
  }
  imgui::initialize();
#endif

  g_initialFrame = true;
  g_config.desiredBackend = selectedBackend;
  return {
      .backend = selectedBackend,
      .userPath = g_config.userPath,
      .cachePath = g_config.cachePath,
      .window = window::get_sdl_window(),
      .windowSize = size,
  };
}

#ifdef AURORA_ENABLE_GX
struct AcquiredSurfaceTexture {
  wgpu::Texture texture;
  wgpu::TextureView view;
};

std::optional<AcquiredSurfaceTexture> acquire_surface_texture() noexcept {
  if (!window::is_presentable()) {
    request_surface_reconfigure();
    return std::nullopt;
  }
  if (!g_surface) {
    request_surface_reconfigure();
    return std::nullopt;
  }

  wgpu::SurfaceTexture surfaceTexture;
  g_surface.GetCurrentTexture(&surfaceTexture);
  switch (surfaceTexture.status) {
  case wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal:
    return AcquiredSurfaceTexture{
        .texture = surfaceTexture.texture,
        .view = surfaceTexture.texture.CreateView(),
    };
  case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
    Log.info("Surface texture is suboptimal, deferring swapchain reconfiguration");
    request_surface_reconfigure();
    return AcquiredSurfaceTexture{
        .texture = surfaceTexture.texture,
        .view = surfaceTexture.texture.CreateView(),
    };
  case wgpu::SurfaceGetCurrentTextureStatus::Timeout:
    Log.warn("Surface texture acquisition timed out");
    return std::nullopt;
  case wgpu::SurfaceGetCurrentTextureStatus::Outdated:
    Log.info("Surface texture is {}, reconfiguring swapchain", magic_enum::enum_name(surfaceTexture.status));
    request_surface_reconfigure();
    return std::nullopt;
  case wgpu::SurfaceGetCurrentTextureStatus::Lost:
    Log.warn("Surface texture is {}, requesting surface recreation", magic_enum::enum_name(surfaceTexture.status));
    request_surface_recreate();
    return std::nullopt;
  case wgpu::SurfaceGetCurrentTextureStatus::Error:
    Log.warn("Surface texture is {}, deferring surface recovery", magic_enum::enum_name(surfaceTexture.status));
    request_surface_reconfigure();
    return std::nullopt;
  default:
    Log.error("Failed to get surface texture: {}", magic_enum::enum_name(surfaceTexture.status));
    return std::nullopt;
  }
}

struct PresentationImage {
  webgpu::TextureWithSampler texture;
  wgpu::BindGroup bindGroup;
};

// Time the presenter spent on its jobs, schedule waits excluded, since the last sealed frame took it.
std::atomic<int64_t> g_presenterBusyNanos{0};

struct PresentationJob {
  std::shared_ptr<PresentationImage> image;
  uint32_t logicalFrame = 0;
  // Absolute deadline on PresentClock stamped by the producer's schedule.
  // Default (epoch) means "present as soon as ready" (no software pacing).
  PresentClock::time_point presentAt{};
  bool interpolated = false;
  // Slot carries a copy of the native image rather than a replayed interpolation. It counts as a
  // present but not toward effectiveFramesPerSecond.
  bool duplicated = false;
  // Whole display periods the group slid forward at encode time (late group).
  uint32_t slidPeriods = 0;
  // Handed to the presenter queue (latency diagnostics).
  PresentClock::time_point enqueuedAt{};
  // Steady time of this image for aligning it to the monitor's refresh (policy::align_to_refresh): the VI
  // grid plus a slowly following offset, free of the jitter of when the image was ready. Epoch: not aligned.
  PresentClock::time_point alignAt{};
  // Spacing of the group's images; alignment only applies when it is about a refresh period or longer.
  int64_t alignStepNanos = 0;
};

// Where the time between end_frame and Present goes, averaged over 10 s and
// logged by the presenter. Observation only.
struct PresentLatencyStats {
  std::atomic<int64_t> frames{0}, workerNs{0}, queueNs{0}, acquireNs{0}, encodeNs{0}, submitNs{0},
      presentNs{0};
  PresentClock::time_point nextReport{};
};
PresentLatencyStats g_presentLatency;

std::array<std::vector<std::shared_ptr<PresentationImage>>, gx::MaxInterpolatedFrames + 1>
    g_presentationImagePools;

std::shared_ptr<PresentationImage> acquire_presentation_image(size_t slot, uint32_t width,
                                                              uint32_t height) {
  auto& pool = g_presentationImagePools.at(slot);
  // A use count of one means only the pool holds the image, so no job can be reading it. Idle
  // images from an older surface size are dropped here instead of leaking for the run.
  for (auto it = pool.begin(); it != pool.end();) {
    const auto& image = *it;
    if (image->texture.size.width == width && image->texture.size.height == height) {
      if (image.use_count() == 1) {
        return image;
      }
      ++it;
      continue;
    }
    if (image.use_count() == 1) {
      it = pool.erase(it);
    } else {
      ++it;
    }
  }

  auto image = std::make_shared<PresentationImage>();
  image->texture = webgpu::create_render_texture(width, height, false);
  image->bindGroup = webgpu::create_copy_bind_group(image->texture);
  pool.emplace_back(image);
  return image;
}

bool present_presentation_job(const PresentationJob& job) {
  ZoneScoped;
  const auto submissionStarted = PresentClock::now();
  // Keep the threshold far above compositor and scheduling jitter. The timings below separate a
  // real surface stall from a bad deadline, and only the former needs a rebuild.
  constexpr auto kSurfaceStallThreshold = std::chrono::milliseconds(250);
  std::chrono::nanoseconds surfaceLockDuration{};
  std::chrono::nanoseconds acquireDuration{};
  std::chrono::nanoseconds encodeDuration{};
  std::chrono::nanoseconds finishDuration{};
  std::chrono::nanoseconds submitDuration{};
  std::chrono::nanoseconds scheduleWaitDuration{};
  std::chrono::nanoseconds presentDuration{};
  bool presented = false;
  if (g_surfaceReconfigurePending.load(std::memory_order_acquire) ||
      window::native_resize_pending() || !window::is_presentable()) {
    return false;
  }
#if defined(MKW_PRIME_QA_TOOLS)
  // Private QA: behave as a monitor of any refresh rate would. The surface is left alone and the
  // presenter holds back while more than a few images are unfinished, as a swap chain queue does,
  // so the GPU still sets the limit. This is how rates above the refresh of the test machine are
  // measured. It never waits inside Dawn: the device lock would stall everything else.
  if (AURORA_DIAGNOSTIC_ENV("WII_COMPILED_QA_SKIP_PRESENT") != nullptr) {
    static std::atomic<int> s_gpuPending{0};
    s_gpuPending.fetch_add(1, std::memory_order_acq_rel);
    g_queue.OnSubmittedWorkDone(wgpu::CallbackMode::AllowSpontaneous, [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {
      s_gpuPending.fetch_sub(1, std::memory_order_acq_rel);
    });
    const auto giveUp = PresentClock::now() + std::chrono::seconds(1);
    while (s_gpuPending.load(std::memory_order_acquire) > 4 && PresentClock::now() < giveUp) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const auto busy = PresentClock::now() - submissionStarted;
    if (job.presentAt != PresentClock::time_point{}) {
      wait_until_precise(job.presentAt);
    }
    g_presenterBusyNanos.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(busy).count(),
                                   std::memory_order_acq_rel);
    record_successful_present(job.interpolated, job.logicalFrame, {}, {}, {}, {}, {},
                              std::chrono::duration_cast<std::chrono::nanoseconds>(busy), job.duplicated,
                              job.slidPeriods, {});
    return true;
  }
#endif
  std::chrono::nanoseconds lateBy{};
  if (job.presentAt != PresentClock::time_point{}) {
    // How expired the deadline already is at dequeue. Positive values mean the
    // slot cannot be paced and fires immediately, which is a burst symptom.
    lateBy = std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() -
                                                                  job.presentAt);
  }
  {
    window::SurfaceLock surfaceLock;
    // Acquire, encode, submit and present are one unit against a configured swapchain, so the
    // surface lock covers all of them. The renderer mutex is deliberately not taken.
    const auto surfaceLockStarted = PresentClock::now();
    std::unique_lock surfaceOwnership(g_surfaceMutex);
    surfaceLockDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        PresentClock::now() - surfaceLockStarted);
    // Surface contention is waiting, not encoding, so keep it out of the encode timing where a
    // reconfigure would look like GPU command recording.
    const auto workStarted = PresentClock::now();
    bool surfaceSizeChanged = window::native_resize_pending();
    if (!surfaceSizeChanged &&
        !g_surfaceReconfigurePending.load(std::memory_order_acquire) &&
        window::is_presentable() && g_surface) {
      // native_window_size_matches compares the OS client size with the configured swapchain, which is
      // what native_fb_* reports. One window query instead of SDL's per-call ones.
      surfaceSizeChanged = !window::native_window_size_matches(
          webgpu::g_graphicsConfig.surfaceConfiguration.width,
          webgpu::g_graphicsConfig.surfaceConfiguration.height);
    }
    if (!surfaceSizeChanged && window::is_presentable()) {
      const auto acquireStarted = PresentClock::now();
      auto acquired = acquire_surface_texture();
      acquireDuration =
          std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - acquireStarted);
      if (acquired) {
        const wgpu::CommandEncoderDescriptor encoderDescriptor{
            .label = "Presentation encoder",
        };
        const auto encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
        const std::array attachments{
            wgpu::RenderPassColorAttachment{
                .view = acquired->view,
                .loadOp = wgpu::LoadOp::Clear,
                .storeOp = wgpu::StoreOp::Store,
            },
        };
        const wgpu::RenderPassDescriptor renderPassDescriptor{
            .label = "Presentation copy pass",
            .colorAttachmentCount = attachments.size(),
            .colorAttachments = attachments.data(),
        };
        const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
        pass.SetPipeline(webgpu::g_CopyPipeline);
        pass.SetBindGroup(0, job.image->bindGroup, 0, nullptr);
        pass.SetViewport(0.f, 0.f,
                         static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.width),
                         static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.height),
                         0.f, 1.f);
        pass.Draw(3);
        pass.End();
        const auto encodeFinished = PresentClock::now();
        encodeDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            encodeFinished - workStarted - acquireDuration);
        const wgpu::CommandBufferDescriptor cmdBufDescriptor{
            .label = "Presentation command buffer",
        };
        const auto finishStarted = PresentClock::now();
        const auto buffer = encoder.Finish(&cmdBufDescriptor);
        finishDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            PresentClock::now() - finishStarted);
        const auto submitStarted = PresentClock::now();
        {
          std::lock_guard submitLock(g_queueSubmitMutex);
          g_queue.Submit(1, &buffer);
        }
        submitDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            PresentClock::now() - submitStarted);
        // Pace the Present() call itself, not the whole unit, so acquire/encode/submit variance stays out
        // of the cadence. Holding the image across the wait is safe while the surface lock is held.
        if (job.presentAt != PresentClock::time_point{}) {
          const auto scheduleWaitStarted = PresentClock::now();
          wait_until_precise(job.presentAt);
          scheduleWaitDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
              PresentClock::now() - scheduleWaitStarted);
        }
        // A native resize can arrive after acquisition, so drop the obsolete image and let the render
        // worker reconfigure at its ordered frame boundary.
        if (!g_surfaceReconfigurePending.load(std::memory_order_acquire) &&
            !window::native_resize_pending() && window::is_presentable() &&
            window::native_window_size_matches(
                webgpu::g_graphicsConfig.surfaceConfiguration.width,
                webgpu::g_graphicsConfig.surfaceConfiguration.height)) {
          const auto presentStarted = PresentClock::now();
          wgpu::Status presentStatus;
          {
            std::lock_guard submitLock(g_queueSubmitMutex);
#if defined(MKW_PRIME_QA_TOOLS)
            // Private QA: split the cost into the lock and the call.
            static std::atomic<int64_t> s_lockNanos{0}, s_callNanos{0};
            static std::atomic<int64_t> s_samples{0};
            const auto lockTaken = PresentClock::now();
            presentStatus = g_surface.Present();
            s_lockNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(lockTaken - presentStarted).count();
            s_callNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - lockTaken).count();
            if (++s_samples >= 600) {
              Log.info("[present-split] submit_lock_ms={:.2f} surface_present_ms={:.2f}",
                       double(s_lockNanos.exchange(0)) / 600e6, double(s_callNanos.exchange(0)) / 600e6);
              s_samples = 0;
            }
#else
            presentStatus = g_surface.Present();
#endif
          }
          presentDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
              PresentClock::now() - presentStarted);
          if (presentStatus == wgpu::Status::Success) {
            presented = true;
            g_lastPresentSteadyNanos.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_release);
            if (!job.interpolated && job.enqueuedAt != PresentClock::time_point{}) {
              auto& stats = g_presentLatency;
              stats.frames.fetch_add(1, std::memory_order_relaxed);
              stats.queueNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                  submissionStarted - job.enqueuedAt).count(), std::memory_order_relaxed);
              stats.acquireNs.fetch_add(acquireDuration.count(), std::memory_order_relaxed);
              stats.encodeNs.fetch_add((encodeDuration + finishDuration).count(), std::memory_order_relaxed);
              stats.submitNs.fetch_add(submitDuration.count(), std::memory_order_relaxed);
              stats.presentNs.fetch_add(presentDuration.count(), std::memory_order_relaxed);
              const auto now = PresentClock::now();
              if (now >= stats.nextReport) {
                const bool first = stats.nextReport == PresentClock::time_point{};
                stats.nextReport = now + std::chrono::seconds(10);
                const int64_t frames = stats.frames.exchange(0);
                const auto avg = [&](std::atomic<int64_t>& value) {
                  return frames ? double(value.exchange(0)) / double(frames) / 1e6 : 0.0;
                };
                const double worker = avg(stats.workerNs), queue = avg(stats.queueNs),
                             acquire = avg(stats.acquireNs), encode = avg(stats.encodeNs),
                             submit = avg(stats.submitNs), present = avg(stats.presentNs);
                if (!first)
                  Log.info("[present-latency] frames={} worker_seal_encode_ms={:.2f} presenter_queue_ms={:.2f} "
                           "acquire_ms={:.2f} blit_encode_ms={:.2f} submit_ms={:.2f} present_ms={:.2f}",
                           frames, worker, queue, acquire, encode, submit, present);
              }
            }
            record_successful_present(
                job.interpolated, job.logicalFrame, acquireDuration, encodeDuration,
                finishDuration, submitDuration, presentDuration,
                std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() -
                                                                     submissionStarted),
                job.duplicated, job.slidPeriods, lateBy);
          } else {
            Log.warn("Surface present failed: {}", static_cast<int>(presentStatus));
            request_surface_reconfigure();
          }
        }
        acquired.reset();
      }
    }
  }
  const auto totalDuration =
      std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - submissionStarted);
  g_presenterBusyNanos.fetch_add((totalDuration - scheduleWaitDuration).count(), std::memory_order_acq_rel);
  constexpr int kStallRebuildThreshold = 3;
  constexpr auto kStallRebuildCooldown = std::chrono::seconds(5);
  static int s_consecutiveStalledPresents = 0;
  static PresentClock::time_point s_lastStallRebuild{};
  if (totalDuration >= kSurfaceStallThreshold) {
    const auto surfaceWorkDuration =
        acquireDuration + encodeDuration + finishDuration + submitDuration + presentDuration;
    const bool surfaceStalled = surfaceWorkDuration >= kSurfaceStallThreshold;
    bool rebuildRequested = false;
    if (surfaceStalled) {
      ++s_consecutiveStalledPresents;
      const auto now = PresentClock::now();
      if (s_consecutiveStalledPresents >= kStallRebuildThreshold &&
          (s_lastStallRebuild == PresentClock::time_point{} ||
           now - s_lastStallRebuild >= kStallRebuildCooldown)) {
        s_lastStallRebuild = now;
        s_consecutiveStalledPresents = 0;
        rebuildRequested = true;
      }
    } else {
      s_consecutiveStalledPresents = 0;
    }
    Log.warn("Presentation job took {:.1f} ms (surface lock {:.1f}, acquire {:.1f}, encode {:.1f}, "
             "finish {:.1f}, submit {:.1f}, schedule wait {:.1f}, present {:.1f}){}",
             std::chrono::duration<double, std::milli>(totalDuration).count(),
             std::chrono::duration<double, std::milli>(surfaceLockDuration).count(),
             std::chrono::duration<double, std::milli>(acquireDuration).count(),
             std::chrono::duration<double, std::milli>(encodeDuration).count(),
             std::chrono::duration<double, std::milli>(finishDuration).count(),
             std::chrono::duration<double, std::milli>(submitDuration).count(),
             std::chrono::duration<double, std::milli>(scheduleWaitDuration).count(),
             std::chrono::duration<double, std::milli>(presentDuration).count(),
             rebuildRequested ? "; rebuilding the surface" : "");
    if (rebuildRequested) {
      request_surface_reconfigure();
    }
  } else {
    s_consecutiveStalledPresents = 0;
    // Below the rebuild threshold, still worth a breakdown: 50-120 ms display
    // stalls during area loads (F8 2026-10-30 23:41) left no trace in the log.
    if (totalDuration >= std::chrono::milliseconds(40)) {
      static std::atomic<uint32_t> logged{0};
      if (logged.fetch_add(1, std::memory_order_relaxed) < 2000) {
        Log.info("[present-slow] job_ms={:.1f} surface_lock={:.1f} acquire={:.1f} encode={:.1f} finish={:.1f} "
                 "submit={:.1f} schedule_wait={:.1f} present={:.1f}",
                 std::chrono::duration<double, std::milli>(totalDuration).count(),
                 std::chrono::duration<double, std::milli>(surfaceLockDuration).count(),
                 std::chrono::duration<double, std::milli>(acquireDuration).count(),
                 std::chrono::duration<double, std::milli>(encodeDuration).count(),
                 std::chrono::duration<double, std::milli>(finishDuration).count(),
                 std::chrono::duration<double, std::milli>(submitDuration).count(),
                 std::chrono::duration<double, std::milli>(scheduleWaitDuration).count(),
                 std::chrono::duration<double, std::milli>(presentDuration).count());
      }
    }
  }
  return presented;
}

struct PresenterState {
  std::mutex mutex;
  std::condition_variable cv;
  std::thread thread;
  std::deque<PresentationJob> jobs;
  bool started = false;
  bool stop = false;
  bool presenting = false;
};

PresenterState g_presenter;
std::atomic<bool> g_presenterStarted{false};

// Moves a scheduled image onto the monitor's refresh timeline (F1 > Video "Sync images to the monitor's
// refresh"). Presenter thread only.
gx::policy::AlignTuning refresh_align_tuning() noexcept {
  gx::policy::AlignTuning tuning;
#if defined(MKW_PRIME_QA_TOOLS)
  // Private QA: try other phases and windows without a rebuild.
  static const gx::policy::AlignTuning qa = [] {
    gx::policy::AlignTuning t;
    if (const char* v = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_QA_ALIGN_PHASE")) t.phase = std::atof(v);
    if (const char* v = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_QA_ALIGN_EARLIEST")) t.earliest = std::atof(v);
    if (const char* v = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_QA_ALIGN_LATEST")) t.latest = std::atof(v);
    return t;
  }();
  tuning = qa;
#endif
  return tuning;
}

#if defined(MKW_PRIME_QA_TOOLS)
// Private QA: how aligned images reached the presenter, every 10 s.
struct RefreshAlignStats {
  uint64_t aligned = 0, unaligned = 0, waited = 0, late = 0, veryLate = 0;
  double addedNanos = 0.0;
  PresentClock::time_point next{};
};
RefreshAlignStats g_refreshAlignStats;
#endif

// Set while alignment steps aside for late images (F1 says so).
std::atomic<bool> g_displaySyncPaused{false};
// Set while the PC has no room to spare for alignment (heavy scene or a lowered image rate).
std::atomic<bool> g_displaySyncWaiting{true};

void align_to_display(PresentationJob& job) noexcept {
  static gx::policy::RefreshAlignment s_alignment;
  if (job.alignAt == PresentClock::time_point{}) {
    s_alignment.valid = false;
    return;
  }
  const auto grid = display_refresh::grid();
  // Faster than the refresh, images would queue up behind each other: leave those to the mailbox.
  if (grid.period <= 0 || job.alignStepNanos * 20 < grid.period * 19) {
    s_alignment.valid = false;
#if defined(MKW_PRIME_QA_TOOLS)
    ++g_refreshAlignStats.unaligned;
#endif
    return;
  }
  const int64_t wanted =
      std::chrono::duration_cast<std::chrono::nanoseconds>(job.alignAt.time_since_epoch()).count();
  int64_t at = gx::policy::align_to_refresh(s_alignment, grid, wanted, refresh_align_tuning());
  // Never hold an image more than a couple of refreshes past its own time, whatever the state says.
  if (at - wanted > 2 * grid.period || wanted - at > 2 * grid.period) {
    s_alignment.valid = false;
    at = wanted;
  }
  const auto now = PresentClock::now();
  const int64_t late = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() - at;
  static gx::policy::AlignHealth s_health;
  const bool wasSuspended = s_health.suspended;
  const bool apply = gx::policy::note_alignment_lateness(
      s_health, static_cast<double>(late) > gx::policy::kAlignLateAfter * static_cast<double>(grid.period));
  if (s_health.suspended != wasSuspended) {
    g_displaySyncPaused.store(s_health.suspended, std::memory_order_release);
    Log.info("[refresh-sync] {}", s_health.suspended
                                      ? "many images late for their refresh: presenting them as they come"
                                      : "images on time again: presents aligned to the refresh");
  }
  // Headroom: see policy::step_align_gate. Without it the schedule stays as it was.
  const bool headroom = gx::frame_interpolation_has_headroom();
  if (headroom != !g_displaySyncWaiting.load(std::memory_order_relaxed)) {
    g_displaySyncWaiting.store(!headroom, std::memory_order_release);
  }
  if (!apply || !headroom) {
#if defined(MKW_PRIME_QA_TOOLS)
    ++g_refreshAlignStats.unaligned;
#endif
    return;  // the schedule as it was; the alignment state keeps following in the background
  }
  job.presentAt = PresentClock::time_point{std::chrono::nanoseconds{at}};
#if defined(MKW_PRIME_QA_TOOLS)
  auto& stats = g_refreshAlignStats;
  ++stats.aligned;
  if (late <= 0) {
    ++stats.waited;
  } else if (late < grid.period / 2) {
    ++stats.late;
  } else {
    ++stats.veryLate;
  }
  stats.addedNanos += static_cast<double>(at - wanted);
  if (now >= stats.next) {
    const bool first = stats.next == PresentClock::time_point{};
    stats.next = now + std::chrono::seconds(10);
    if (!first) {
      const double n = static_cast<double>((std::max<uint64_t>)(stats.aligned, 1));
      Log.info("[refresh-align] aligned={} unaligned={} on_time={} late={} late_half_refresh={} "
               "mean_offset_ms={:.2f} rate_hz={:.3f}",
               stats.aligned, stats.unaligned, stats.waited, stats.late, stats.veryLate,
               stats.addedNanos / n / 1e6, display_refresh::locked_rate());
    }
    stats = {.next = stats.next};
  }
#endif
}

void presenter_main() noexcept {
  g_presenterNativeId.store(current_native_thread_id(), std::memory_order_relaxed);
  if (!SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH)) {
    Log.warn("Could not raise the asynchronous presenter thread priority: {}", SDL_GetError());
  }
  for (;;) {
    PresentationJob job;
    {
      std::unique_lock lock(g_presenter.mutex);
      g_presenter.cv.wait(lock, [] { return g_presenter.stop || !g_presenter.jobs.empty(); });
      if (g_presenter.stop && g_presenter.jobs.empty()) {
        break;
      }
      job = std::move(g_presenter.jobs.front());
      g_presenter.jobs.pop_front();
      g_presenter.presenting = true;
    }
    g_presenter.cv.notify_all();

    align_to_display(job);
    present_presentation_job(job);

    {
      std::lock_guard lock(g_presenter.mutex);
      g_presenter.presenting = false;
    }
    g_presenter.cv.notify_all();
  }
}

void ensure_presenter_started() {
  std::lock_guard lock(g_presenter.mutex);
  if (g_presenter.started) {
    return;
  }
  g_presenter.stop = false;
  g_presenter.presenting = false;
  g_presenter.thread = std::thread(presenter_main);
  g_presenter.started = true;
  g_presenterStarted.store(true, std::memory_order_release);
  Log.info("Enabled bounded asynchronous presentation worker");
  display_refresh::start();
}

void wait_for_presenter_idle() noexcept {
  if (!g_presenterStarted.load(std::memory_order_acquire)) {
    return;
  }
  std::unique_lock lock(g_presenter.mutex);
  g_presenter.cv.wait(
      lock, [] { return g_presenter.jobs.empty() && !g_presenter.presenting; });
}

void enqueue_presentations(std::vector<PresentationJob>&& jobs) {
  ensure_presenter_started();
  // Scale the bound with the group being handed over: a 240 Hz frame enqueues four jobs at once,
  // and a bound sized for two groups blocked the frame worker on a draining one.
  const size_t maximumQueuedJobs = (std::max)(static_cast<size_t>(8), 3 * jobs.size());
  std::unique_lock lock(g_presenter.mutex);
  if (g_presenter.stop) {
    return;
  }
  if (g_presenter.jobs.size() + jobs.size() > maximumQueuedJobs) {
    // Presentation is a real-time stream, not a lossless queue: waiting for room couples the guest
    // and audio clocks to a blocked Present(). Keep the newest group and drop obsolete images.
    const size_t dropped = g_presenter.jobs.size();
    g_presenter.jobs.clear();
    if (dropped != 0) {
      // A stuck Present can keep replacement mode active for a while.  Aggregate
      // the warning instead of turning a driver stall into a log flood.
      static size_t droppedSinceWarning = 0;
      static PresentClock::time_point lastWarning{};
      droppedSinceWarning += dropped;
      const auto now = PresentClock::now();
      if (lastWarning == PresentClock::time_point{} || now - lastWarning >= std::chrono::seconds(1)) {
        Log.warn("Presenter fell behind; dropped {} stale presentation jobs", droppedSinceWarning);
        droppedSinceWarning = 0;
        lastWarning = now;
      }
    }
  }
  const auto enqueuedAt = PresentClock::now();
  for (auto& job : jobs) {
    job.enqueuedAt = enqueuedAt;
    g_presenter.jobs.emplace_back(std::move(job));
  }
  lock.unlock();
  g_presenter.cv.notify_all();
}

void stop_presenter() noexcept {
  if (!g_presenterStarted.load(std::memory_order_acquire)) {
    return;
  }
  {
    std::lock_guard lock(g_presenter.mutex);
    g_presenter.stop = true;
  }
  g_presenter.cv.notify_all();
  if (g_presenter.thread.joinable()) {
    g_presenter.thread.join();
  }
  {
    std::lock_guard lock(g_presenter.mutex);
    g_presenter.jobs.clear();
    g_presenter.started = false;
    g_presenter.presenting = false;
  }
  g_presenterStarted.store(false, std::memory_order_release);
}

struct MetalFXSlot {
  webgpu::metalfx::Size size{};
  std::unique_ptr<webgpu::metalfx::SpatialScaler> scaler;
  wgpu::BindGroup bindGroup;
};
std::array<MetalFXSlot, gx::MaxInterpolatedFrames + 1> g_metalfxSlots;
size_t g_metalfxNextSlot = 0;
webgpu::metalfx::SpatialScaler* g_metalfxPendingOutput = nullptr;
bool g_metalfxFailed = false;

void metalfx_failed(const std::string& reason) {
  Log.warn("MetalFX spatial upscaling disabled: {}; using normal presentation", reason);
  g_metalfxFailed = true;
  g_metalfxStatus.store(AURORA_METALFX_ERROR);
  g_metalfxPendingOutput = nullptr;
  g_metalfxSlots = {};
}

wgpu::BindGroup upscale_presentation(wgpu::CommandEncoder& encoder,
                                     const webgpu::PresentSource& source,
                                     const webgpu::Viewport& viewport, bool enabled) {
  if (!enabled) {
    g_metalfxSlots = {};
    g_metalfxFailed = false;
    g_metalfxStatus.store(AURORA_METALFX_DISABLED);
    return {};
  }
  if (!g_metalfxSupported.load()) {
    g_metalfxStatus.store(AURORA_METALFX_UNSUPPORTED);
    return {};
  }
  if (g_metalfxFailed) return {};
  const webgpu::metalfx::Size size{
      source.size.width, source.size.height,
      static_cast<uint32_t>(viewport.width), static_cast<uint32_t>(viewport.height),
      webgpu::g_graphicsConfig.surfaceConfiguration.format,
  };
  // The existing copy path samples perceptual values from unorm game images.
  // Do not introduce implicit sRGB decoding or downscaling into MetalFX.
  if (!size.inputWidth || !size.inputHeight || size.inputWidth >= size.outputWidth ||
      size.inputHeight >= size.outputHeight ||
      (source.format != wgpu::TextureFormat::RGBA8Unorm && source.format != wgpu::TextureFormat::BGRA8Unorm)) {
    g_metalfxStatus.store(AURORA_METALFX_NOT_UPSCALING);
    return {};
  }
  auto& slot = g_metalfxSlots[g_metalfxNextSlot++ % g_metalfxSlots.size()];
  if (!slot.scaler || !(slot.size == size)) {
    slot = {};
    std::string error;
    slot.scaler = webgpu::metalfx::create(g_instance, g_device, size, error);
    if (!slot.scaler) {
      if (error.empty()) g_metalfxStatus.store(AURORA_METALFX_NOT_UPSCALING);
      else metalfx_failed(error);
      return {};
    }
    slot.size = size;
    wgpu::SamplerDescriptor samplerDescriptor{};
    samplerDescriptor.magFilter = wgpu::FilterMode::Linear;
    samplerDescriptor.minFilter = wgpu::FilterMode::Linear;
    slot.bindGroup = webgpu::create_copy_bind_group(slot.scaler->output_view(),
                                                   g_device.CreateSampler(&samplerDescriptor));
    Log.info("MetalFX spatial slot: {}x{} -> {}x{}", size.inputWidth, size.inputHeight,
             size.outputWidth, size.outputHeight);
  }
  if (!slot.scaler->begin_input()) {
    metalfx_failed(slot.scaler->error());
    return {};
  }
  const wgpu::RenderPassColorAttachment attachment{
      .view = slot.scaler->input_view(),
      .loadOp = wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
  };
  const wgpu::RenderPassDescriptor descriptor{
      .label = "MetalFX input copy",
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
  };
  auto pass = encoder.BeginRenderPass(&descriptor);
  pass.SetPipeline(webgpu::g_CopyPipeline);
  pass.SetBindGroup(0, source.bindGroup);
  pass.SetViewport(0, 0, static_cast<float>(size.inputWidth), static_cast<float>(size.inputHeight), 0, 1);
  pass.Draw(3);
  pass.End();
  // Submit the sealed scene and input copy before crossing to the native queue.
  // The replacement encoder composites the upscaled image and ImGui normally.
  auto buffer = encoder.Finish();
  {
    std::lock_guard submitLock(g_queueSubmitMutex);
    g_queue.Submit(1, &buffer);
  }
  encoder = g_device.CreateCommandEncoder();
  if (!slot.scaler->upscale()) {
    metalfx_failed(slot.scaler->error());
    return {};
  }
  g_metalfxPendingOutput = slot.scaler.get();
  g_metalfxStatus.store(AURORA_METALFX_ACTIVE);
  return slot.bindGroup;
}

// `presentSource` is latched in the seal prologue: by the time this encodes, the producer's next
// gfx::begin_frame() may already have cleared the display-copy override.
wgpu::BindGroup encode_presentation_snapshot(wgpu::CommandEncoder& encoder,
                                              const webgpu::PresentSource& presentSource,
                                              const PresentationImage& image,
                                              bool includeImGui, bool metalfxEnabled,
                                              const wgpu::BindGroup* cachedMetalFXOutput = nullptr) {
  ZoneScoped;
  auto viewport = webgpu::calculate_present_viewport(
      image.texture.size.width, image.texture.size.height, presentSource.size.width,
      presentSource.size.height);
  float presentAspect = 0.f;
  if (window::get_present_aspect_ratio(presentAspect)) {
    viewport = webgpu::calculate_present_viewport_for_aspect(
        image.texture.size.width, image.texture.size.height, presentAspect);
  }
  if (image.texture.size.width && image.texture.size.height && viewport.width > 0 && viewport.height > 0) {
    const float w = float(image.texture.size.width), h = float(image.texture.size.height);
    g_presentViewportFraction[0].store(viewport.left / w, std::memory_order_relaxed);
    g_presentViewportFraction[1].store(viewport.top / h, std::memory_order_relaxed);
    g_presentViewportFraction[2].store(viewport.width / w, std::memory_order_relaxed);
    g_presentViewportFraction[3].store(viewport.height / h, std::memory_order_release);
  }
  wgpu::BindGroup presentBindGroup = presentSource.bindGroup;
  wgpu::BindGroup newMetalFXOutput;
  if (cachedMetalFXOutput && *cachedMetalFXOutput) {
    presentBindGroup = *cachedMetalFXOutput;
  } else if (auto upscaled = upscale_presentation(encoder, presentSource, viewport, metalfxEnabled)) {
    presentBindGroup = std::move(upscaled);
    newMetalFXOutput = presentBindGroup;
  }
  {
    const std::array attachments{
        wgpu::RenderPassColorAttachment{
            .view = image.texture.view,
            .loadOp = wgpu::LoadOp::Clear,
            .storeOp = wgpu::StoreOp::Store,
        },
    };
    const wgpu::RenderPassDescriptor renderPassDescriptor{
        .label = "Interpolation snapshot pass",
        .colorAttachmentCount = attachments.size(),
        .colorAttachments = attachments.data(),
    };
    const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
    pass.SetPipeline(webgpu::g_CopyPipeline);
    pass.SetBindGroup(0, presentBindGroup, 0, nullptr);
    pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height,
                     viewport.znear, viewport.zfar);
    pass.Draw(3);
    pass.End();
  }
  // Host image effects (F1 > Effects) on the composed game picture, before the overlay.
  gfx::post::apply(encoder, image.texture, viewport.left, viewport.top, viewport.width, viewport.height);
  if (includeImGui) {
    const std::array attachments{
        wgpu::RenderPassColorAttachment{
            .view = image.texture.view,
            .loadOp = wgpu::LoadOp::Load,
            .storeOp = wgpu::StoreOp::Store,
        },
    };
    const wgpu::RenderPassDescriptor renderPassDescriptor{
        .label = "Snapshot ImGui pass",
        .colorAttachmentCount = attachments.size(),
        .colorAttachments = attachments.data(),
    };
    const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
    pass.SetViewport(0.f, 0.f, static_cast<float>(image.texture.size.width),
                     static_cast<float>(image.texture.size.height), 0.f, 1.f);
    imgui::render(pass);
    pass.End();
  }
  return newMetalFXOutput;
}
#endif

void shutdown() noexcept {
  stop_frame_worker();
#ifdef AURORA_ENABLE_GX
  stop_presenter();
  display_refresh::stop();
  g_presentationImagePools = {};
  g_metalfxSlots = {};
  g_metalfxPendingOutput = nullptr;
  g_metalfxNextSlot = 0;
  g_metalfxFailed = false;
  g_metalfxRequested.store(false);
  g_metalfxSupported.store(false);
  g_metalfxStatus.store(AURORA_METALFX_DISABLED);
  imgui::shutdown();
  gfx::shutdown();
  webgpu::shutdown();
#endif
  input::shutdown();
  window::shutdown();
}

const AuroraEvent* update() noexcept {
  ZoneScoped;
  if (g_initialFrame) {
    g_initialFrame = false;
    input::initialize();
  }
  return window::poll_events();
}

bool begin_frame_impl(bool pumpEvents, ImGuiFramePolicy imguiPolicy, bool* imguiNewFrameOwed) noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  webgpu::fail_if_device_lost();
  if (pumpEvents) {
    window::pump_events();
  }
  const bool surfaceReconfigurePending =
      g_surfaceReconfigurePending.load(std::memory_order_acquire);
  const bool surfaceMutationRequired =
      surfaceReconfigurePending || !window::is_presentable() || !g_surface ||
      window::native_resize_pending() ||
      !window::native_window_size_matches(
          webgpu::g_graphicsConfig.surfaceConfiguration.width,
          webgpu::g_graphicsConfig.surfaceConfiguration.height);
  if (surfaceMutationRequired) {
    wait_for_presenter_idle();
    window::SurfaceLock surfaceLock;
    // Reconfiguring invalidates any image the presenter holds, so take the surface exclusively; a
    // drained queue does not stop a later job. Order is surface before renderer everywhere.
    std::lock_guard surfaceOwnership(g_surfaceMutex);
    std::lock_guard gpuLock(g_rendererGpuMutex);
    // Surface configuration belongs to the render thread; the SDL thread must not reconfigure Dawn
    // while this worker is acquiring or presenting.
    if (!window::is_presentable()) {
      webgpu::release_surface();
      return false;
    }
    if (window::is_paused()) {
      return false;
    }
    const bool consumeSurfaceReconfigure =
        g_surfaceReconfigurePending.exchange(false, std::memory_order_acq_rel);
    const bool consumeSurfaceRecreate =
        g_surfaceRecreatePending.exchange(false, std::memory_order_acq_rel);
    if (!g_surface || consumeSurfaceReconfigure) {
      // Reconfigure in place unless the surface was actually lost. See
      // g_surfaceRecreatePending for why destroying a live surface here is fatal
      // while a capture overlay is attached.
      webgpu::refresh_surface(consumeSurfaceRecreate);
      if (!g_surface) {
        return false;
      }
    }
    if (window::native_resize_pending() ||
        !window::native_window_size_matches(webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                            webgpu::g_graphicsConfig.surfaceConfiguration.height)) {
      window::sync_frame_buffer_size();
    }
  } else if (window::is_paused()) {
    return false;
  }

  return begin_frame_render_state_impl(imguiPolicy, imguiNewFrameOwed);
#else
  (void)imguiPolicy;
  (void)imguiNewFrameOwed;
  return true;
#endif
}

bool begin_frame_render_state_impl(ImGuiFramePolicy imguiPolicy, bool* imguiNewFrameOwed) noexcept {
#ifdef AURORA_ENABLE_GX
  std::lock_guard gpuLock(g_rendererGpuMutex);
  // Note the debt before gfx::begin_frame() can fail: the synchronous path always started the
  // ImGui frame here, and the runtime's retry loop depends on that pairing.
  if (imguiPolicy == ImGuiFramePolicy::Immediate) {
    imgui::new_frame(window::get_window_size());
  } else if (imguiNewFrameOwed != nullptr) {
    *imguiNewFrameOwed = true;
  }
  if (!gfx::begin_frame(!g_preserveEfbAcrossFrames.load(std::memory_order_acquire))) {
    return false;
  }
#else
  (void)imguiPolicy;
  (void)imguiNewFrameOwed;
#endif
  return true;
}

#ifdef AURORA_ENABLE_GX
// Everything the mutex-free encode phase needs, latched while the renderer GPU mutex is held.
// None of it may be re-read from a global later; the producer has already begun the next frame.
struct SealedFrameContext {
  wgpu::CommandEncoder encoder; // slot 0's encoder; already holds the staging copies
  webgpu::PresentSource presentSource{};
  webgpu::PresentSource diagnosticTextureSource{};
  uint64_t scheduleBaseNanos = 0;
  uint64_t scheduleIntervalNanos = 0;
  uint32_t interpolatedFrameCount = 0;
  std::array<float, gx::MaxInterpolatedFrames> slotWeights{};
  double slotBias = 1.0;
  bool presentNative = true;
  uint32_t snapshotWidth = 1;
  uint32_t snapshotHeight = 1;
  uint32_t logicalFrame = 0;
  bool interpolationActive = false;
  bool replayInterpolatedFrames = false;
  // Low-latency mode: inserted images extrapolate after the frame, which is presented first.
  bool extrapolate = false;
  bool metalfxEnabled = false;
  bool diagnosticCapture = false; // the sealed frame was the draw-dump frame
  std::string diagnosticBase;      // its dump path, latched at seal (a burst moves on to the next)
};

// Phase 1: everything that touches producer-shared renderer state. Needs g_rendererGpuMutex and
// a FIFO already drained into the recorded pass list.
void seal_frame_locked(gfx::SealedFrame& sealedFrame, SealedFrameContext& ctx) {
  ZoneScopedN("Seal frame");
  const auto encoderDescriptor = wgpu::CommandEncoderDescriptor{
      .label = "Redraw encoder",
  };
  ctx.encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
  // Probe-sized CPU-consumed copies read back asynchronously. Their downscale blits push uniforms,
  // so prepare them while the producer's staging buffers are still mapped.
  gfx::efb_ram::seal_async_downloads();
  ctx.diagnosticCapture = gfx::draw_dump::finish_frame(gfx::current_frame());
  if (ctx.diagnosticCapture) ctx.diagnosticBase = gfx::draw_dump::finished_base_path();
  if (ctx.diagnosticCapture && gfx::draw_dump::g_copyTexture) {
    const auto& copy = gfx::draw_dump::g_copyTexture;
    ctx.diagnosticTextureSource = {.texture = copy->texture, .size = copy->size, .format = copy->format};
    gfx::draw_dump::g_copyTexture.reset();
  }
  gfx::end_frame(ctx.encoder);
  gfx::g_stats.presentedFrameCount = 0;
  gfx::g_stats.interpolatedFrameCount = 0;
  // Latched before the producer's next gfx::begin_frame() calls
  // gx::begin_frame_interpolation(), which resets both of these.
  ctx.interpolatedFrameCount = gx::interpolated_frame_count();
  ctx.interpolationActive = ctx.interpolatedFrameCount != 0;
  ctx.replayInterpolatedFrames = ctx.interpolationActive && gx::frame_interpolation_replay_safe();
  for (uint32_t sample = 0; sample < ctx.interpolatedFrameCount; ++sample) {
    ctx.slotWeights[sample] = gx::interpolated_frame_weight(sample);
  }
  ctx.slotBias = gx::interpolated_frame_bias();
  ctx.extrapolate = gx::interpolated_frame_extrapolates();
  ctx.presentNative = gx::interpolated_frame_presents_native();
  ctx.scheduleBaseNanos = g_presentScheduleBaseNanos.load(std::memory_order_acquire);
  ctx.scheduleIntervalNanos = g_presentScheduleIntervalNanos.load(std::memory_order_acquire);
  const auto windowSize = window::get_window_size();
  ctx.snapshotWidth = (std::max)(windowSize.native_fb_width, 1u);
  ctx.snapshotHeight = (std::max)(windowSize.native_fb_height, 1u);
  ctx.logicalFrame = gfx::current_frame();
  ctx.metalfxEnabled = g_metalfxRequested.load();
  // Latched before webgpu::clear_present_source_override() in the producer's
  // next gfx::begin_frame().
  ctx.presentSource = webgpu::current_present_source();
  if (std::getenv("WII_COMPILED_GPU_CAPTURE_EFB_CURRENT")) {
    const auto& texture = webgpu::present_source();
    ctx.diagnosticTextureSource = {.texture=texture.texture, .size=texture.size, .format=texture.format};
  }
  if (std::getenv("WII_COMPILED_GPU_CAPTURE_EFB_SNAPSHOT")) {
    const auto texture = gfx::diagnostic_resolve_snapshot();
    if (texture) ctx.diagnosticTextureSource = {.texture=texture->texture, .size=texture->size, .format=texture->format};
  }
  if (std::getenv("WII_COMPILED_GPU_CAPTURE_BOUND_TEXTURE")) {
    const auto& texture = gx::g_gxState.textures[0].ref;
    if (texture) ctx.diagnosticTextureSource = {.texture=texture->texture, .size=texture->size, .format=texture->format};
  }
  // ImGui draw lists are built once per frame and replayed by each slot's ImGui pass, which is why
  // the next ImGui frame cannot start until the encode phase is done.
  imgui::render_frame_data();
  // Drop the sealed frame's lazy RAM-readback requests while the producer is still excluded; it
  // starts registering the next frame's as soon as SEALED is published.
  gfx::efb_ram::cancel();
  // Detach the recorded passes. From here the producer's list is empty and the
  // encode phase reads only worker-private state.
  gfx::post::latch_frame();
  gfx::seal_frame(sealedFrame);
  // Inserted images replay the sealed passes, which must hold the presented game frame from its start:
  // the previous seal carried its passes after the display copy here. Otherwise they copy the native
  // image. Private QA: WII_COMPILED_QA_INTERP_NO_CARRY restores the old batch replay for comparison.
  static const bool noCarry = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_QA_INTERP_NO_CARRY") != nullptr;
  const bool startsGameFrame = gfx::sealed_frame_starts_game_frame(sealedFrame);
  if (!noCarry) {
    ctx.replayInterpolatedFrames = ctx.replayInterpolatedFrames && startsGameFrame;
  }
  if (gx::frame_interpolation_active()) {
    gx::note_frame_interpolation_seal(startsGameFrame, gfx::sealed_frame_ends_at_display_copy(sealedFrame),
                                      gfx::sealed_frame_carried_out_draws(sealedFrame),
                                      ctx.replayInterpolatedFrames);
  }
  // Private QA: only inserted images reach the screen, so a screen capture always shows one.
  if (ctx.replayInterpolatedFrames && AURORA_DIAGNOSTIC_ENV("WII_COMPILED_QA_INTERP_SLOTS_ONLY") != nullptr) {
    ctx.presentNative = false;
  }
  gfx::expire_bind_group_cache();
}

#if defined(MKW_PRIME_QA_TOOLS)
// Private QA (WII_COMPILED_QA_INTERP_COMPARE_EVERY=N, with WII_COMPILED_QA_INTERP_WEIGHT_ONE): every N
// groups, reads back the first inserted image and the native one. At weight 1 they must be identical; a
// difference means the inserted image did not draw the same game frame (EFB state from another frame).
struct InterpCompareReadback {
  wgpu::Buffer buffer;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bytesPerRow = 0;
};

InterpCompareReadback encode_interp_compare_readback(const wgpu::CommandEncoder& encoder,
                                                     const webgpu::TextureWithSampler& texture) {
  InterpCompareReadback readback{.width = texture.size.width, .height = texture.size.height};
  readback.bytesPerRow = align_to(readback.width * 4, 256);
  const wgpu::BufferDescriptor descriptor{
      .label = "Interpolation compare readback",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
      .size = uint64_t{readback.bytesPerRow} * readback.height,
  };
  readback.buffer = g_device.CreateBuffer(&descriptor);
  const wgpu::TexelCopyTextureInfo source{.texture = texture.texture};
  const wgpu::TexelCopyBufferInfo destination{
      .layout = {.offset = 0, .bytesPerRow = readback.bytesPerRow, .rowsPerImage = readback.height},
      .buffer = readback.buffer,
  };
  const wgpu::Extent3D extent{readback.width, readback.height, 1};
  encoder.CopyTextureToBuffer(&source, &destination, &extent);
  return readback;
}

const uint8_t* map_interp_compare_readback(InterpCompareReadback& readback) {
  wgpu::MapAsyncStatus status = wgpu::MapAsyncStatus::CallbackCancelled;
  const auto future = readback.buffer.MapAsync(wgpu::MapMode::Read, 0, readback.buffer.GetSize(),
                                               wgpu::CallbackMode::WaitAnyOnly,
                                               [&status](wgpu::MapAsyncStatus s, wgpu::StringView) { status = s; });
  if (g_instance.WaitAny(future, 5000000000) != wgpu::WaitStatus::Success || status != wgpu::MapAsyncStatus::Success) {
    return nullptr;
  }
  return static_cast<const uint8_t*>(readback.buffer.GetConstMappedRange(0, readback.buffer.GetSize()));
}

void compare_interp_readbacks(InterpCompareReadback& slot, InterpCompareReadback& native, uint32_t logicalFrame) {
  static uint64_t compared = 0, identical = 0, worstPermille = 0;
  const uint8_t* a = map_interp_compare_readback(slot);
  const uint8_t* b = map_interp_compare_readback(native);
  if (a == nullptr || b == nullptr || slot.width != native.width || slot.height != native.height) {
    Log.warn("[interp-compare] frame={} readback failed", logicalFrame);
    return;
  }
  uint64_t differing = 0, strongly = 0;
  for (uint32_t y = 0; y < slot.height; ++y) {
    const uint8_t* rowA = a + uint64_t{y} * slot.bytesPerRow;
    const uint8_t* rowB = b + uint64_t{y} * native.bytesPerRow;
    for (uint32_t x = 0; x < slot.width; ++x) {
      int maxDelta = 0;
      for (int c = 0; c < 3; ++c) {
        maxDelta = std::max(maxDelta, std::abs(int(rowA[x * 4 + c]) - int(rowB[x * 4 + c])));
      }
      differing += maxDelta > 2 ? 1u : 0u;
      strongly += maxDelta > 32 ? 1u : 0u;
    }
  }
  slot.buffer.Unmap();
  native.buffer.Unmap();
  const uint64_t pixels = uint64_t{slot.width} * slot.height;
  const uint64_t permille = pixels != 0 ? strongly * 1000 / pixels : 0;
  ++compared;
  identical += differing == 0 ? 1u : 0u;
  worstPermille = std::max(worstPermille, permille);
  Log.info("[interp-compare] frame={} differing_pixels={} strongly_differing={} ({} per mille) compared={} identical={} "
           "worst_per_mille={}", logicalFrame, differing, strongly, permille, compared, identical, worstPermille);
}
#endif

void publish_presentations(std::vector<PresentationJob>&& presentationJobs, bool interpolationActive);

// Phase 2: encode every presentation slot. Reads only `ctx` and the sealed passes, so it runs
// without the renderer GPU mutex while the producer records the next frame.
std::vector<PresentationJob> encode_sealed_frame(gfx::SealedFrame& sealedFrame, SealedFrameContext& ctx) {
  ZoneScopedN("Encode sealed frame");
  auto encoder = std::move(ctx.encoder);
  const auto encoderDescriptor = wgpu::CommandEncoderDescriptor{
      .label = "Redraw encoder",
  };
  const auto encodeStarted = PresentClock::now();
  // Absolute slot deadlines: pace to the boundary that just passed plus 6.5 ms; late frames
  // free-run. A slot of weight w presents (w - bias) of a period after its group's start, which
  // keeps display ticks evenly spaced across groups at any rate.
  constexpr uint64_t kNativePresentOffsetNanos = 6'500'000;
  const uint32_t presentationJobCount = ctx.interpolatedFrameCount + 1;
  const auto slotPresentDeadline = [&](uint32_t slot) -> PresentClock::time_point {
    if (ctx.scheduleBaseNanos == 0 || ctx.scheduleIntervalNanos == 0) {
      return {};
    }
    const uint64_t groupStart = ctx.scheduleBaseNanos - ctx.scheduleIntervalNanos + kNativePresentOffsetNanos;
    if (!ctx.interpolationActive) {
      return PresentClock::time_point{std::chrono::nanoseconds{groupStart}};
    }
    const double weight = slot < ctx.interpolatedFrameCount ? ctx.slotWeights[slot] : 1.0;
    const double periods = (std::max)(weight - ctx.slotBias, 0.0);
    return PresentClock::time_point{std::chrono::nanoseconds{
        groupStart + static_cast<uint64_t>(periods * static_cast<double>(ctx.scheduleIntervalNanos))}};
  };
  std::vector<PresentationJob> presentationJobs;
  presentationJobs.reserve(presentationJobCount);

  // Each slot is submitted as soon as it is encoded, so the GPU starts slot 0 while slot 1 is still
  // recording. Queue order preserves the ordering the single batched buffer gave.
  const wgpu::CommandBufferDescriptor cmdBufDescriptor{
      .label = "Presentation slot command buffer",
  };
  const auto submitEncodedSlot = [&](wgpu::CommandEncoder& target, bool releaseMetalFXOutput = true) {
    const auto buffer = target.Finish(&cmdBufDescriptor);
    std::lock_guard submitLock(g_queueSubmitMutex);
    g_queue.Submit(1, &buffer);
    if (releaseMetalFXOutput && g_metalfxPendingOutput) {
      if (!g_metalfxPendingOutput->end_output()) metalfx_failed(g_metalfxPendingOutput->error());
      g_metalfxPendingOutput = nullptr;
    }
  };

#if defined(MKW_PRIME_QA_TOOLS)
  std::optional<InterpCompareReadback> compareSlot;
  bool compareGroup = false;
  if (const char* every = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_QA_INTERP_COMPARE_EVERY")) {
    static uint64_t groups = 0;
    const uint64_t period = std::max<uint64_t>(std::strtoull(every, nullptr, 10), 1);
    compareGroup = ctx.replayInterpolatedFrames && ctx.interpolatedFrameCount != 0 && ctx.presentNative &&
                   (groups++ % period) == 0;
  }
#endif
  // Low-latency mode: the frame goes to the presenter as soon as it is encoded and its inserted images,
  // predicted past it, follow. Otherwise the inserted images lead up to the frame and are encoded first.
  const bool nativeFirst = ctx.extrapolate && ctx.replayInterpolatedFrames;
  PresentClock::time_point nativeQueuedAt{};
  const auto encodeSlots = [&](bool finalizeLast) {
    for (uint32_t interpolatedFrame = 0; interpolatedFrame < ctx.interpolatedFrameCount;
         ++interpolatedFrame) {
      const bool last = interpolatedFrame + 1 == ctx.interpolatedFrameCount;
      gfx::render(sealedFrame, encoder, static_cast<int32_t>(interpolatedFrame), finalizeLast && last);
      auto image =
          acquire_presentation_image(interpolatedFrame, ctx.snapshotWidth, ctx.snapshotHeight);
      encode_presentation_snapshot(encoder, ctx.presentSource, *image, true, ctx.metalfxEnabled);
#if defined(MKW_PRIME_QA_TOOLS)
      if (compareGroup && interpolatedFrame == 0) compareSlot = encode_interp_compare_readback(encoder, image->texture);
#endif
      PresentClock::time_point presentAt = slotPresentDeadline(interpolatedFrame);
      if (nativeFirst) {
        // A tick at weight w shows the frame carried forward, (w - 1) of a period after the frame.
        const double interval = ctx.scheduleIntervalNanos != 0 ? static_cast<double>(ctx.scheduleIntervalNanos)
                                                               : 1.0e9 / 60.0;
        const double after = (std::max)(ctx.slotWeights[interpolatedFrame] - 1.0, 0.0) * interval;
        presentAt = nativeQueuedAt + std::chrono::nanoseconds{static_cast<int64_t>(after)};
      }
      presentationJobs.push_back({
          .image = std::move(image),
          .logicalFrame = ctx.logicalFrame,
          .presentAt = presentAt,
          .interpolated = true,
      });
      submitEncodedSlot(encoder);
      encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
    }
  };
  if (ctx.replayInterpolatedFrames && !nativeFirst) {
    encodeSlots(false);
  }

  // A demanded CPU-visible EFB readback submits a prefix of the frame, so replaying the resumed
  // stream would mutate an already-rendered EFB. Render once, then duplicate into the slots.
  gfx::render(sealedFrame, encoder, -1, !nativeFirst);
  // The copy targets now hold this frame's resolves, so queue their readbacks on the same encoder;
  // completion is harvested in gfx::after_submit, never waited on here.
  gfx::efb_ram::encode_async_downloads(encoder);
  wgpu::BindGroup duplicatedMetalFXOutput;
  if (!ctx.replayInterpolatedFrames) {
    for (uint32_t interpolatedFrame = 0; interpolatedFrame < ctx.interpolatedFrameCount;
         ++interpolatedFrame) {
      auto image =
          acquire_presentation_image(interpolatedFrame, ctx.snapshotWidth, ctx.snapshotHeight);
      const auto newMetalFXOutput = encode_presentation_snapshot(
          encoder, ctx.presentSource, *image, true, ctx.metalfxEnabled,
          duplicatedMetalFXOutput ? &duplicatedMetalFXOutput : nullptr);
      if (!duplicatedMetalFXOutput && newMetalFXOutput) {
        duplicatedMetalFXOutput = newMetalFXOutput;
      }
      presentationJobs.push_back({
          .image = std::move(image),
          .logicalFrame = ctx.logicalFrame,
          .presentAt = slotPresentDeadline(interpolatedFrame),
          .interpolated = true,
          .duplicated = true,
      });
      submitEncodedSlot(encoder, false);
      encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
    }
  }
  // A screenshot wants the composed image of this frame even when no display tick lands on it.
  const bool screenshotPending = g_screenshotPending.exchange(false, std::memory_order_acq_rel);
  std::shared_ptr<PresentationImage> finalImage;
  if (ctx.presentNative || screenshotPending) {
    finalImage = acquire_presentation_image(ctx.interpolatedFrameCount, ctx.snapshotWidth, ctx.snapshotHeight);
    encode_presentation_snapshot(encoder, ctx.presentSource, *finalImage, true, ctx.metalfxEnabled,
                                 duplicatedMetalFXOutput ? &duplicatedMetalFXOutput : nullptr);
  }
#if defined(MKW_PRIME_QA_TOOLS)
  std::optional<InterpCompareReadback> compareNative;
  if ((compareSlot || (nativeFirst && compareGroup)) && finalImage) {
    compareNative = encode_interp_compare_readback(encoder, finalImage->texture);
  }
#endif
  auto captureSource = ctx.diagnosticTextureSource.texture ? ctx.diagnosticTextureSource : ctx.presentSource;
  if (screenshotPending) {
    std::lock_guard<std::mutex> lock(g_captureRequestMutex);
    g_captureOutputPath = g_screenshotPath;
    g_captureFrame.store(gfx::current_frame(), std::memory_order_release);
    captureSource = {.texture = finalImage->texture.texture, .size = finalImage->texture.size,
                     .format = finalImage->texture.format};
  }
  auto pendingFrameCapture = encode_frame_capture(encoder, captureSource, ctx.diagnosticCapture, ctx.diagnosticBase);
  if (ctx.presentNative) {
    presentationJobs.push_back({
        .image = std::move(finalImage),
        .logicalFrame = ctx.logicalFrame,
        .presentAt = nativeFirst ? PresentClock::time_point{} : slotPresentDeadline(ctx.interpolatedFrameCount),
        .interpolated = false,
    });
  }
  if (!nativeFirst) {
    gfx::gpu_timing::encode_resolve(encoder);  // diagnostic GPU timing (off unless asked for)
  }
  submitEncodedSlot(encoder);
  // Steady times for aligning to the monitor's refresh: the VI boundary this frame belongs to, plus how long
  // after it frames become ready (low latency) or groups start (scheduled). The offset follows the late
  // side: it rises within a few frames and falls slowly, so nearly every image is ready before its refresh.
  // Following the average left half of them late in heavy rooms, where each late image then shared the
  // next one's refresh (full replay at 120, 2026-11-08: more repeats aligned than unaligned there).
  static double s_alignOffsetNanos = -1.0;
  // Also while the rate controller holds one image per frame: 60 images on a 120 Hz monitor then keep exactly two
  // refreshes each instead of alternating one and three as they drift.
  const bool alignable = (ctx.interpolationActive || gx::frame_interpolation_fps() != 0) &&
                         ctx.scheduleIntervalNanos != 0 && ctx.scheduleBaseNanos != 0 &&
                         ctx.scheduleBaseNanos > ctx.scheduleIntervalNanos;
  const int64_t alignBoundary = alignable ? static_cast<int64_t>(ctx.scheduleBaseNanos - ctx.scheduleIntervalNanos) : 0;
  const auto followAlignOffset = [&](int64_t sampleNanos) {
    const double sample = static_cast<double>(sampleNanos);
    const double interval = static_cast<double>(ctx.scheduleIntervalNanos);
    const double delta = sample - s_alignOffsetNanos;
    if (s_alignOffsetNanos < 0.0 || delta < -interval) {
      s_alignOffsetNanos = sample;  // first frame, or after a pause or a mode change
    } else if (delta > 0.0) {
      // A single very late frame only nudges it; a lasting change catches up in a few frames.
      s_alignOffsetNanos += (std::min)(delta, interval * 0.25) / 4.0;
    } else {
      s_alignOffsetNanos += delta / 64.0;
    }
  };
  const int64_t alignStep =
      alignable ? static_cast<int64_t>(ctx.scheduleIntervalNanos) /
                      static_cast<int64_t>((std::max)(ctx.interpolatedFrameCount + (ctx.presentNative ? 1u : 0u), 1u))
                : 0;
  if (!alignable) {
    s_alignOffsetNanos = -1.0;
  }
  if (nativeFirst && alignable && !presentationJobs.empty()) {
    const int64_t ready = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              PresentClock::now().time_since_epoch()).count();
    followAlignOffset(ready - alignBoundary);
    auto& native = presentationJobs.back();
    native.alignAt = PresentClock::time_point{
        std::chrono::nanoseconds{alignBoundary + static_cast<int64_t>(s_alignOffsetNanos)}};
    native.alignStepNanos = alignStep;
  }
  if (nativeFirst) {
    // The frame leaves now; the predicted images are encoded while it is on its way to the screen.
    encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
    publish_presentations(std::move(presentationJobs), true);
    presentationJobs.clear();
    nativeQueuedAt = PresentClock::now();
    encodeSlots(true);
    gfx::gpu_timing::encode_resolve(encoder);
    submitEncodedSlot(encoder);
  }

  // The group starts as soon as encoding usually ends, so the real frame is held back by the smallest
  // amount the cadence allows. The offset follows encoding: up at once, down slowly, so one heavy
  // frame does not bunch the next groups together. A group that finished past its start slides
  // forward by whole display periods, never per slot. The cursor keeps groups a period apart.
  static PresentClock::time_point s_lastGroupStart{};
  static int64_t s_groupOffsetNanos = 0;
  if (ctx.scheduleIntervalNanos != 0) {
    const auto encodeNanos = std::chrono::duration<double, std::nano>(PresentClock::now() - encodeStarted).count();
    const auto presentNanos = static_cast<double>(g_presenterBusyNanos.exchange(0, std::memory_order_acq_rel));
    gx::report_frame_load(encodeNanos / static_cast<double>(ctx.scheduleIntervalNanos),
                          presentNanos / static_cast<double>(ctx.scheduleIntervalNanos));
  }
  if (!nativeFirst && ctx.interpolationActive && ctx.scheduleIntervalNanos != 0 && !presentationJobs.empty() &&
      presentationJobs.front().presentAt != PresentClock::time_point{}) {
    const std::chrono::nanoseconds interval{static_cast<int64_t>(ctx.scheduleIntervalNanos)};
    const PresentClock::time_point boundary{
        std::chrono::nanoseconds{ctx.scheduleBaseNanos - ctx.scheduleIntervalNanos}};
    const auto now = PresentClock::now();
    constexpr int64_t kEncodeMarginNanos = 500'000;
    const int64_t neededOffset =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - boundary).count() + kEncodeMarginNanos;
    if (neededOffset > s_groupOffsetNanos) {
      s_groupOffsetNanos = (std::min)(neededOffset, static_cast<int64_t>(ctx.scheduleIntervalNanos));
    } else {
      s_groupOffsetNanos -= (s_groupOffsetNanos - neededOffset) / 64;
    }
    s_groupOffsetNanos = (std::max)(s_groupOffsetNanos, static_cast<int64_t>(kNativePresentOffsetNanos));
    auto groupStart = boundary + std::chrono::nanoseconds{s_groupOffsetNanos};
    if (now > groupStart) {
      const auto behind = std::chrono::duration_cast<std::chrono::nanoseconds>(now - groupStart);
      const uint64_t periods =
          static_cast<uint64_t>(behind.count()) / ctx.scheduleIntervalNanos + 1u;
      groupStart += std::chrono::nanoseconds{static_cast<int64_t>(periods * ctx.scheduleIntervalNanos)};
    }
    if (s_lastGroupStart != PresentClock::time_point{} && groupStart < s_lastGroupStart + interval * 3 / 4) {
      groupStart = s_lastGroupStart + interval;
    }
    const PresentClock::time_point usualStart{
        boundary + std::chrono::nanoseconds{static_cast<int64_t>(kNativePresentOffsetNanos)}};
    const auto shift = std::chrono::duration_cast<std::chrono::nanoseconds>(groupStart - usualStart);
    const uint32_t slidPeriods = static_cast<uint32_t>(
        (std::max<int64_t>(shift.count() - s_groupOffsetNanos + static_cast<int64_t>(kNativePresentOffsetNanos), 0) +
         static_cast<int64_t>(ctx.scheduleIntervalNanos) - 1) /
        static_cast<int64_t>(ctx.scheduleIntervalNanos));
    for (auto& job : presentationJobs) {
      job.presentAt += shift;
      job.slidPeriods = slidPeriods;
    }
    s_lastGroupStart = groupStart;
    if (alignable) {
      // The first image of the group sits (w0 - bias) periods after the group start.
      const double firstPeriods =
          ctx.interpolatedFrameCount != 0 ? (std::max)(ctx.slotWeights[0] - ctx.slotBias, 0.0) : 1.0 - ctx.slotBias;
      const int64_t start = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                presentationJobs.front().presentAt.time_since_epoch()).count() -
                            static_cast<int64_t>(firstPeriods * static_cast<double>(ctx.scheduleIntervalNanos));
      followAlignOffset(start - alignBoundary);
    }
  } else {
    // No schedule (interpolation off, boot/black presents): the grid is gone,
    // so the cursor must not constrain the next scheduled group.
    s_lastGroupStart = {};
    s_groupOffsetNanos = 0;
    if (alignable && !nativeFirst && !presentationJobs.empty() &&
        presentationJobs.front().presentAt != PresentClock::time_point{}) {
      // One image per frame, at its scheduled time.
      followAlignOffset(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            presentationJobs.front().presentAt.time_since_epoch()).count() -
                        alignBoundary);
    }
  }

  if (alignable && s_alignOffsetNanos >= 0.0 && !presentationJobs.empty()) {
    // Low latency: these are the predicted images, (w - 1) periods after the frame. Otherwise the whole
    // group, (w - bias) periods after its start.
    const double base = static_cast<double>(alignBoundary) + s_alignOffsetNanos;
    for (size_t index = 0; index < presentationJobs.size(); ++index) {
      auto& job = presentationJobs[index];
      const bool native = !job.interpolated;
      const double weight = native ? 1.0 : static_cast<double>(ctx.slotWeights[(std::min)(index, size_t{gx::MaxInterpolatedFrames - 1})]);
      const double periods = !ctx.interpolationActive ? 0.0
                             : nativeFirst          ? (std::max)(weight - 1.0, 0.0)
                                                    : (std::max)(weight - ctx.slotBias, 0.0);
      job.alignAt = PresentClock::time_point{std::chrono::nanoseconds{
          static_cast<int64_t>(base + periods * static_cast<double>(ctx.scheduleIntervalNanos))}};
      job.alignStepNanos = alignStep;
    }
  }
  if (pendingFrameCapture.has_value()) {
    complete_frame_capture(*pendingFrameCapture);
  }
#if defined(MKW_PRIME_QA_TOOLS)
  if (compareSlot && compareNative) compare_interp_readbacks(*compareSlot, *compareNative, ctx.logicalFrame);
#endif
  for (auto& readback : gfx::draw_dump::g_copyReadbacks) {
    wgpu::MapAsyncStatus status = wgpu::MapAsyncStatus::Error;
    auto future = readback.buffer.MapAsync(wgpu::MapMode::Read, 0, readback.buffer.GetSize(),
                                           wgpu::CallbackMode::WaitAnyOnly,
                                           [&](wgpu::MapAsyncStatus s, wgpu::StringView) { status = s; });
    if (g_instance.WaitAny(future, 5000000000) == wgpu::WaitStatus::Success && status == wgpu::MapAsyncStatus::Success) {
      const auto* pixels = static_cast<const uint8_t*>(readback.buffer.GetConstMappedRange(0, readback.buffer.GetSize()));
      write_bmp(readback.path.c_str(), pixels, readback.width, readback.height, readback.bytesPerRow, readback.bgra);
      readback.buffer.Unmap();
    }
  }
  gfx::draw_dump::g_copyReadbacks.clear();
  gfx::tex_cmpr_decode::complete_verifications();
  gfx::after_submit();
  gfx::g_stats.presentedFrameCount = static_cast<uint32_t>(presentationJobs.size());
  gfx::g_stats.interpolatedFrameCount = ctx.interpolatedFrameCount;
  return presentationJobs;
}

// Phase 3: hand the encoded group to whoever owns presentation.
void publish_presentations(std::vector<PresentationJob>&& presentationJobs, bool interpolationActive) {
#if defined(__APPLE__)
  (void)interpolationActive;
  // Presenting reaches SDL/AppKit, whose window operations must stay on the
  // main thread. Interpolation normally starts the presenter worker, so keep
  // its jobs synchronous on Apple platforms.
  for (const auto& job : presentationJobs) {
    present_presentation_job(job);
  }
#else
  // Keep presentation on the presenter whenever the async frame worker runs, even with
  // interpolation off, so every mode shares one surface/resize path. RenderDoc keeps the sync path.
  if (frame_worker_requested() || interpolationActive ||
      g_presenterStarted.load(std::memory_order_acquire)) {
    enqueue_presentations(std::move(presentationJobs));
  } else {
    for (const auto& job : presentationJobs) {
      present_presentation_job(job);
    }
  }
#endif
}

void record_frame_telemetry() {
  TracyPlotConfig("aurora: lastVertSize", tracy::PlotFormatType::Memory, false, true, 0);
  TracyPlotConfig("aurora: lastUniformSize", tracy::PlotFormatType::Memory, false, true, 0);
  TracyPlotConfig("aurora: lastIndexSize", tracy::PlotFormatType::Memory, false, true, 0);
  TracyPlotConfig("aurora: lastStorageSize", tracy::PlotFormatType::Memory, false, true, 0);
  TracyPlotConfig("aurora: lastTextureUploadSize", tracy::PlotFormatType::Memory, false, true, 0);

  TracyPlot("aurora: queuedPipelines", static_cast<int64_t>(gfx::g_stats.queuedPipelines));
  TracyPlot("aurora: createdPipelines", static_cast<int64_t>(gfx::g_stats.createdPipelines));
  TracyPlot("aurora: drawCallCount", static_cast<int64_t>(gfx::g_stats.drawCallCount));
  TracyPlot("aurora: mergedDrawCallCount", static_cast<int64_t>(gfx::g_stats.mergedDrawCallCount));
  TracyPlot("aurora: lastVertSize", static_cast<int64_t>(gfx::g_stats.lastVertSize));
  TracyPlot("aurora: lastUniformSize", static_cast<int64_t>(gfx::g_stats.lastUniformSize));
  TracyPlot("aurora: lastIndexSize", static_cast<int64_t>(gfx::g_stats.lastIndexSize));
  TracyPlot("aurora: lastStorageSize", static_cast<int64_t>(gfx::g_stats.lastStorageSize));
  TracyPlot("aurora: lastTextureUploadSize", static_cast<int64_t>(gfx::g_stats.lastTextureUploadSize));
  TracyPlot("aurora: frameIndex", static_cast<int64_t>(gfx::current_frame()));
#if defined(TRACY_ENABLE) && defined(_WIN32)
  const auto fileTimeValue = [](const FILETIME& value) noexcept {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
  };
  FILETIME creation{}, exit{}, processKernel{}, processUser{}, threadKernel{}, threadUser{};
  const bool processTimesAvailable =
      GetProcessTimes(GetCurrentProcess(), &creation, &exit, &processKernel, &processUser) != FALSE;
  const bool threadTimesAvailable =
      GetThreadTimes(GetCurrentThread(), &creation, &exit, &threadKernel, &threadUser) != FALSE;
  const uint64_t processCpu100ns =
      processTimesAvailable ? fileTimeValue(processKernel) + fileTimeValue(processUser) : 0;
  const uint64_t threadCpu100ns =
      threadTimesAvailable ? fileTimeValue(threadKernel) + fileTimeValue(threadUser) : 0;
  static uint64_t previousProcessCpu100ns = processCpu100ns;
  static uint64_t previousThreadCpu100ns = threadCpu100ns;
  TracyPlot("aurora: processCpuUsPerFrame",
            static_cast<int64_t>((processCpu100ns - previousProcessCpu100ns) / 10));
  TracyPlot("aurora: mainThreadCpuUsPerFrame",
            static_cast<int64_t>((threadCpu100ns - previousThreadCpu100ns) / 10));
  previousProcessCpu100ns = processCpu100ns;
  previousThreadCpu100ns = threadCpu100ns;
#endif
  FrameMarkNamed("Aurora frame");
}

// One complete frame-worker cycle. The scene encode only leaves the renderer mutex when
// interpolation actually inserts slots; otherwise both phases publish together.
bool run_frame_worker_cycle(gfx::SealedFrame& sealedFrame) noexcept {
  ZoneScopedN("Frame worker cycle");
  webgpu::fail_if_device_lost();
  const auto cycleStarted = PresentClock::now();
  // Display stalls below the presenter's own threshold ([present-gap]): log the
  // worker's slow phases so a stall can be placed (seal/encode vs next-frame prepare).
  const auto logSlow = [](const char* phase, std::chrono::nanoseconds elapsed) {
    if (elapsed < std::chrono::milliseconds(30)) return;
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 2000)
      Log.info("[frame-worker-slow] phase={} ms={:.1f}", phase,
               std::chrono::duration<double, std::milli>(elapsed).count());
  };
  const auto noteWorker = [&] {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - cycleStarted);
    g_presentLatency.workerNs.fetch_add(elapsed.count(), std::memory_order_relaxed);
    logSlow("seal_encode", elapsed);
  };
  SealedFrameContext ctx;
  std::vector<PresentationJob> presentationJobs;
  bool overlapEncode = false;
  {
    std::lock_guard gpuLock(g_rendererGpuMutex);
    seal_frame_locked(sealedFrame, ctx);
    // Encoding the whole frame under the renderer mutex stalls a producer that
    // records the next frame on its own thread. The mutex-free phase is the
    // interpolation path's; WII_COMPILED_AURORA_OVERLAP_ENCODE=1 uses it always.
    static const bool overlapAlways = [] {
      const char* value = std::getenv("WII_COMPILED_AURORA_OVERLAP_ENCODE");
      return value && std::strcmp(value, "1") == 0;
    }();
    overlapEncode = ctx.interpolationActive || overlapAlways;
    if (!overlapEncode) {
      presentationJobs = encode_sealed_frame(sealedFrame, ctx);
    }
  }
  if (!overlapEncode) {
    noteWorker();
    publish_presentations(std::move(presentationJobs), ctx.interpolationActive);
  }

  {
    std::unique_lock lock(g_frameWorker.mutex);
    g_frameWorker.cv.wait(lock, [] { return g_frameWorker.stop || g_frameWorker.prepareAllowed; });
    if (g_frameWorker.stop) {
      return false;
    }
    g_frameWorker.prepareAllowed = false;
  }

  // Preparing the next frame belongs to the SEALED phase: without a fresh pass 0 and mapped
  // staging buffers the producer's drain has nowhere to put its commands.
  bool imguiNewFrameOwed = false;
  const auto prepareStarted = PresentClock::now();
  const bool prepared = begin_frame_impl(
      false, overlapEncode ? ImGuiFramePolicy::Deferred : ImGuiFramePolicy::Immediate,
      &imguiNewFrameOwed);
  logSlow("prepare_next", std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - prepareStarted));

  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.framePrepared = prepared;
    g_frameWorker.sealed.store(true, std::memory_order_release);
    if (!overlapEncode) {
      g_frameWorker.ready.store(true, std::memory_order_release);
    }
  }
  g_frameWorker.cv.notify_all();

  if (overlapEncode) {
    // Mutex-free: the producer drains and records the next frame in parallel, taking the renderer
    // mutex per drain, and this phase never takes it.
    presentationJobs = encode_sealed_frame(sealedFrame, ctx);
    noteWorker();
    publish_presentations(std::move(presentationJobs), ctx.interpolationActive);
    if (imguiNewFrameOwed) {
      // Safe only now: every slot has replayed this frame's ImGui draw lists.
      std::lock_guard gpuLock(g_rendererGpuMutex);
      imgui::new_frame(window::get_window_size());
    }
    {
      std::lock_guard lock(g_frameWorker.mutex);
      g_frameWorker.ready.store(true, std::memory_order_release);
    }
    g_frameWorker.cv.notify_all();
  }

  record_frame_telemetry();
  return true;
}
#endif

// Synchronous frame submission: seal, encode and present inline on the calling thread. Used when
// the frame worker is disabled (RenderDoc captures) and on the boot path.
void end_frame_impl(bool pumpEvents, bool drainFifo) {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  webgpu::fail_if_device_lost();
  if (pumpEvents) {
    window::pump_events();
  }
  gfx::SealedFrame sealedFrame;
  SealedFrameContext ctx;
  std::vector<PresentationJob> presentationJobs;
  if (drainFifo) gx::fifo::drain();
  {
    std::lock_guard gpuLock(g_rendererGpuMutex);
    seal_frame_locked(sealedFrame, ctx);
    presentationJobs = encode_sealed_frame(sealedFrame, ctx);
  }
  publish_presentations(std::move(presentationJobs), ctx.interpolationActive);
  record_frame_telemetry();
#else
  (void)pumpEvents;
  (void)drainFifo;
#endif
}

thread_local bool t_threadPumpsEvents = true;

bool begin_frame() noexcept {
#ifdef AURORA_ENABLE_GX
  // The asynchronous fast path below can return a logically prepared frame
  // without entering begin_frame_impl(), so loss must be checked before it.
  webgpu::fail_if_device_lost();
#endif
  if (!frame_worker_requested()) {
    return begin_frame_impl(t_threadPumpsEvents);
  }

  ensure_frame_worker_started();
  // SDL needs event pumping on the window-owning producer thread, and the worker passes
  // pumpEvents=false, so keep it here even when the fast path returns early. A host GX
  // consumer thread that does not own the window opts out (aurora_set_thread_event_pump).
  if (t_threadPumpsEvents) {
    window::pump_events();
  }
  bool waitForSurfacePreparation = false;
#ifdef AURORA_ENABLE_GX
  // A surface mutation can legitimately fail preparation, and optimistic success would let GX/ImGui
  // record into a frame that was never begun, so join this path and return its real result.
  waitForSurfacePreparation =
      !window::is_presentable() || !g_surface ||
      window::native_resize_pending() || window::is_paused() ||
      !window::native_window_size_matches(
          webgpu::g_graphicsConfig.surfaceConfiguration.width,
          webgpu::g_graphicsConfig.surfaceConfiguration.height);
#endif
  bool workerPreparationPending = false;
  {
    std::lock_guard lock(g_frameWorker.mutex);
    // The runtime begins right after an asynchronous end, so treat the worker's pending begin as an
    // active frame unless a resize needs the real preparation result.
    if (!g_frameWorker.ready.load(std::memory_order_acquire)) {
      g_frameWorker.prepareAllowed = true;
      g_frameWorker.cv.notify_one();
      if (!waitForSurfacePreparation) {
        return true;
      }
      workerPreparationPending = true;
    } else if (g_frameWorker.framePrepared) {
      return true;
    }
  }

  if (workerPreparationPending) {
    // DONE, not SEALED: this only runs while the window is changing and the caller is about to act on
    // the surface, so keep the resize path fully serialized.
    wait_for_frame_worker_private(FrameWorkerPhase::Done);
    std::lock_guard lock(g_frameWorker.mutex);
    return g_frameWorker.framePrepared;
  }

  {
    std::lock_guard lock(g_frameWorker.mutex);
    if (g_frameWorker.framePrepared) {
      return true;
    }
  }

  const bool prepared = begin_frame_impl(false);
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.framePrepared = prepared;
  }
  return prepared;
}

void end_frame() {
#ifdef AURORA_ENABLE_GX
  webgpu::fail_if_device_lost();
#endif
  if (!frame_worker_requested()) {
    end_frame_impl(true, true);
    return;
  }

  ensure_frame_worker_started();
  // DONE: this seals another frame, which means reusing the worker's encoder
  // state and its SealedFrame. The previous cycle must be completely finished.
  wait_for_frame_worker_private(FrameWorkerPhase::Done);

  // Seal all current GX work on the CPU while the renderer is known ready.
  // Later FIFO writes belong exclusively to the next frame.
  gx::fifo::drain();
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.framePrepared = false;
    g_frameWorker.sealed.store(false, std::memory_order_release);
    g_frameWorker.ready.store(false, std::memory_order_release);
    g_frameWorker.jobPending = true;
    g_frameWorker.prepareAllowed = false;
  }
  g_frameWorker.cv.notify_one();
}
} // namespace

void wait_for_frame_worker() noexcept { wait_for_frame_worker_private(FrameWorkerPhase::Done); }
std::chrono::nanoseconds wait_for_frame_worker_sealed() noexcept {
  if (g_frameWorker.sealed.load(std::memory_order_acquire)) {
    return std::chrono::nanoseconds::zero();
  }
  const auto started = std::chrono::steady_clock::now();
  wait_for_frame_worker_private(FrameWorkerPhase::Sealed);
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
}
bool wait_for_frame_worker_for(std::chrono::microseconds timeout) noexcept {
  return wait_for_frame_worker_private_for(FrameWorkerPhase::Done, timeout);
}
std::recursive_mutex& renderer_gpu_mutex() noexcept { return g_rendererGpuMutex; }
void submit_staging_commands(const wgpu::CommandBuffer& commands) {
  std::lock_guard submitLock(g_queueSubmitMutex);
  webgpu::g_queue.Submit(1, &commands);
}
} // namespace aurora

// C API bindings
AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {
  return aurora::initialize(argc, argv, *config);
}
void aurora_shutdown() { aurora::shutdown(); }
const AuroraEvent* aurora_update() { return aurora::update(); }
void aurora_set_efb_persistence(bool enabled) {
  aurora::g_preserveEfbAcrossFrames.store(enabled, std::memory_order_release);
}
bool aurora_begin_frame() { return aurora::begin_frame(); }
void aurora_end_frame() { aurora::end_frame(); }
void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback callback) {
  aurora::g_frameWorkerWaitCallback.store(callback, std::memory_order_release);
}
void aurora_set_thread_event_pump(bool enabled) { aurora::t_threadPumpsEvents = enabled; }
void aurora_set_async_decoder(bool enabled) { aurora::gx::fifo::set_async_enabled(enabled); }
bool aurora_async_decoder_enabled(void) { return aurora::gx::fifo::async_enabled(); }
void aurora_set_stream_callback(AuroraStreamCallback callback) { aurora::gx::fifo::set_host_callback(callback); }
void aurora_stream_host_callback(uint32_t id, const void* data, uint32_t size) {
  namespace fifo = aurora::gx::fifo;
  if (!fifo::async_enabled() || fifo::on_decoder_thread()) {
    fifo::invoke_host_callback(id, static_cast<const uint8_t*>(data), size);
    return;
  }
  fifo::write_u8(GX_LOAD_AURORA);
  fifo::write_u16(GX_LOAD_AURORA_HOST_CALLBACK);
  fifo::write_u32(id);
  fifo::write_u32(size);
  if (size != 0) fifo::write_data(data, size);
  fifo::commit();
}
void aurora_set_deferred_draw_expander(AuroraDeferredDrawExpander expander) {
  aurora::gx::fifo::set_deferred_draw_expander(expander);
}
bool aurora_stream_deferred_draw(const void* data, uint32_t size) {
  namespace fifo = aurora::gx::fifo;
  if (!fifo::async_enabled() || fifo::on_decoder_thread() || fifo::in_display_list() || fifo::in_begin() ||
      !fifo::has_deferred_draw_expander()) {
    return false;
  }
  fifo::write_u8(GX_LOAD_AURORA);
  fifo::write_u16(GX_LOAD_AURORA_DEFERRED_DRAW);
  fifo::write_u32(size);
  if (size != 0) fifo::write_data(data, size);
  fifo::commit();
  return true;
}
uint32_t aurora_stream_decoder_backlog_hint(void) { return aurora::gx::fifo::async_backlog_hint(); }
void aurora_stream_flush(void) { aurora::gx::fifo::hand_off(); }
void aurora_stream_sync(void) { aurora::gx::fifo::sync(); }
bool aurora_stream_idle(void) { return aurora::gx::fifo::async_idle(); }
bool aurora_on_decoder_thread(void) { return aurora::gx::fifo::on_decoder_thread(); }
uint32_t aurora_get_decoder_thread_id(void) { return aurora::gx::fifo::decoder_native_thread_id(); }
void aurora_stream_shutdown(void) { aurora::gx::fifo::shutdown_async(); }
int64_t aurora_last_present_steady_nanos() {
  return aurora::g_lastPresentSteadyNanos.load(std::memory_order_acquire);
}
void aurora_get_present_viewport_fraction(float* left, float* top, float* width, float* height) {
  const auto& v = aurora::g_presentViewportFraction;
  const float h = v[3].load(std::memory_order_acquire);
  if (left) *left = v[0].load(std::memory_order_relaxed);
  if (top) *top = v[1].load(std::memory_order_relaxed);
  if (width) *width = v[2].load(std::memory_order_relaxed);
  if (height) *height = h;
}
void aurora_wait_for_frame_worker() { aurora::wait_for_frame_worker(); }
void aurora_wait_for_frame_worker_sealed(void) { (void)aurora::wait_for_frame_worker_sealed(); }
bool aurora_wait_for_frame_worker_for(uint32_t timeoutMicros) {
  return aurora::wait_for_frame_worker_for(std::chrono::microseconds(timeoutMicros));
}
void aurora_set_present_schedule(uint64_t baseNanos, uint64_t intervalNanos) {
  aurora::g_presentScheduleBaseNanos.store(baseNanos, std::memory_order_release);
  aurora::g_presentScheduleIntervalNanos.store(intervalNanos, std::memory_order_release);
}
void aurora_report_producer_paced(bool paced) {
#ifdef AURORA_ENABLE_GX
  aurora::gx::report_producer_paced(paced);
#else
  (void)paced;
#endif
}
void aurora_get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics* diagnostics) {
  if (diagnostics != nullptr) {
    aurora::gx::get_frame_interpolation_diagnostics(*diagnostics);
  }
}

uint32_t aurora_get_render_thread_ids(uint32_t* ids, uint32_t capacity) {
  const uint32_t found[2] = {aurora::g_frameWorkerNativeId.load(std::memory_order_relaxed),
                             aurora::g_presenterNativeId.load(std::memory_order_relaxed)};
  uint32_t count = 0;
  for (const uint32_t id : found)
    if (id != 0 && count < capacity) ids[count++] = id;
  return count;
}
uint64_t aurora_get_present_count() {
  std::lock_guard lock(aurora::g_presentTimingMutex);
  return aurora::g_totalPresentCount;
}
void aurora_get_present_timing(AuroraPresentTiming* timing) {
  if (timing != nullptr) {
    *timing = aurora::snapshot_present_timing();
  }
}
void aurora_set_frame_interpolation_fps(uint32_t targetFps) {
#ifdef AURORA_ENABLE_GX
  aurora::gx::set_frame_interpolation_fps(targetFps);
  if (aurora::webgpu::set_non_blocking_present(aurora::gx::frame_interpolation_active())) {
    aurora::request_surface_reconfigure();
  }
#else
  (void)targetFps;
#endif
}
void aurora_set_frame_interpolation_low_latency(bool enabled) {
#ifdef AURORA_ENABLE_GX
  aurora::gx::set_frame_interpolation_low_latency(enabled);
#else
  (void)enabled;
#endif
}
void aurora_set_frame_interpolation_display_sync(bool enabled) {
  aurora::display_refresh::set_enabled(enabled);
  if (enabled) {
    aurora::display_refresh::start();
  }
}
bool aurora_get_frame_interpolation_display_sync() { return aurora::display_refresh::enabled(); }
float aurora_get_display_sync_rate() { return static_cast<float>(aurora::display_refresh::locked_rate()); }
bool aurora_get_display_sync_paused() {
  return aurora::g_displaySyncPaused.load(std::memory_order_acquire) ||
         aurora::g_displaySyncWaiting.load(std::memory_order_acquire);
}
bool aurora_get_frame_interpolation_low_latency() {
#ifdef AURORA_ENABLE_GX
  return aurora::gx::frame_interpolation_low_latency();
#else
  return false;
#endif
}
uint32_t aurora_get_frame_interpolation_fps() {
#ifdef AURORA_ENABLE_GX
  return aurora::gx::frame_interpolation_fps();
#else
  return 0;
#endif
}
bool aurora_draw_dump_active() { return aurora::gfx::draw_dump::active() != nullptr; }
void aurora_request_draw_dump(const char* basePath) {
  aurora::gfx::draw_dump::request(basePath != nullptr ? basePath : "draw_dump");
}
void aurora_request_draw_dump_burst(const char* basePath, uint32_t frames) {
  aurora::gfx::draw_dump::request(basePath != nullptr ? basePath : "draw_dump", frames);
}
// `frame` 0 means "the next frame presented" (bug reports); any other value is
// an exact frame number for diagnostics.
void aurora_request_screenshot(const char* outputPath) {
  if (outputPath == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(aurora::g_captureRequestMutex);
    aurora::g_screenshotPath = outputPath;
  }
  aurora::g_screenshotPending.store(true, std::memory_order_release);
}
void aurora_request_frame_capture(uint32_t frame, const char* outputPath) {
  std::lock_guard<std::mutex> lock(aurora::g_captureRequestMutex);
  aurora::g_captureOutputPath = outputPath != nullptr ? outputPath : "frame_capture.bmp";
  aurora::g_captureFrame.store(frame, std::memory_order_release);
}
bool aurora_flush_efb_copies_to_ram() {
#ifdef AURORA_ENABLE_GX
  if (!aurora::gfx::efb_ram::has_pending()) {
    return true;
  }
  // This finalizes the frame still being recorded, on the producer thread, so join the whole cycle
  // first: the encode phase owns the previous passes, EFB targets and image pool.
  aurora::wait_for_frame_worker();
  // The renderer is about to submit a prefix of the active frame. Its resumed
  // suffix cannot safely be replayed against the same mutable EFB resources.
  aurora::gx::mark_frame_interpolation_replay_unsafe();
  aurora::gx::fifo::drain();
  if (!aurora::gfx::efb_ram::prepare_downloads()) return false;
  const wgpu::CommandEncoderDescriptor encoderDescriptor{
      .label = "GX CPU-visible EFB copy encoder",
  };
  auto encoder = aurora::webgpu::g_device.CreateCommandEncoder(&encoderDescriptor);
  aurora::gfx::end_batch(encoder);
  aurora::gfx::render(encoder);
  aurora::gfx::efb_ram::encode_downloads(encoder);
  const wgpu::CommandBufferDescriptor commandDescriptor{
      .label = "GX CPU-visible EFB copy command buffer",
  };
  const auto commandBuffer = encoder.Finish(&commandDescriptor);
  {
    std::lock_guard submitLock(aurora::g_queueSubmitMutex);
    aurora::webgpu::g_queue.Submit(1, &commandBuffer);
  }
  const bool copied = aurora::gfx::efb_ram::complete_downloads();
  aurora::gfx::after_submit();
  const bool resumed = aurora::gfx::resume_frame();
  return copied && resumed;
#else
  return true;
#endif
}
bool aurora_flush_efb_copy_to_ram(void* dest) {
#ifdef AURORA_ENABLE_GX
  if (dest == nullptr || !aurora::gfx::efb_ram::has_pending(dest)) {
    return false;
  }

  // See aurora_flush_efb_copies_to_ram: this encodes the in-progress frame on
  // the producer thread, so the worker's overlapped encode has to be finished.
  aurora::wait_for_frame_worker();
  // Preserve the requested output cadence by duplicating the completed native
  // image instead of replaying this split frame.
  aurora::gx::mark_frame_interpolation_replay_unsafe();
  aurora::gx::fifo::drain();
  if (!aurora::gfx::efb_ram::prepare_downloads(dest)) return false;
  const wgpu::CommandEncoderDescriptor encoderDescriptor{
      .label = "GX demanded EFB copy encoder",
  };
  auto encoder = aurora::webgpu::g_device.CreateCommandEncoder(&encoderDescriptor);
  aurora::gfx::end_batch(encoder);
  aurora::gfx::render(encoder);
  aurora::gfx::efb_ram::encode_downloads(encoder, dest);
  const wgpu::CommandBufferDescriptor commandDescriptor{
      .label = "GX demanded EFB copy command buffer",
  };
  const auto commandBuffer = encoder.Finish(&commandDescriptor);
  {
    std::lock_guard submitLock(aurora::g_queueSubmitMutex);
    aurora::webgpu::g_queue.Submit(1, &commandBuffer);
  }
  const bool copied = aurora::gfx::efb_ram::complete_downloads();
  aurora::gfx::after_submit();
  const bool resumed = aurora::gfx::resume_frame();
  return copied && resumed;
#else
  (void)dest;
  return true;
#endif
}
AuroraBackend aurora_get_backend() { return aurora::g_config.desiredBackend; }
const AuroraBackend* aurora_get_available_backends(size_t* count) {
  if (count != nullptr) {
    *count = aurora::PreferredBackendOrder.size();
  }
  return aurora::PreferredBackendOrder.data();
}
void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }
void aurora_set_texture_anisotropy(uint16_t value) {
  aurora::g_config.forceTextureAnisotropy = value;
  aurora::webgpu::g_graphicsConfig.forceTextureAnisotropy = value;
}
uint16_t aurora_get_texture_anisotropy(void) { return aurora::g_config.forceTextureAnisotropy; }
void aurora_set_texture_replacement_variant(const char* name) {
  aurora::gfx::texture_replacement::request_variant(name);
}
uint32_t aurora_get_msaa_samples(void) { return aurora::webgpu::g_graphicsConfig.msaaSamples; }
void aurora_set_per_pixel_lighting(bool enabled) {
  aurora::g_config.perPixelLighting = enabled;
#ifdef AURORA_ENABLE_GX
  aurora::gx::g_perPixelLighting.store(enabled, std::memory_order_relaxed);
#endif
}
bool aurora_get_per_pixel_lighting(void) { return aurora::g_config.perPixelLighting; }
void aurora_set_enhanced_metal_params(float roughness, float metallic, float edge, float detail) {
#ifdef AURORA_ENABLE_GX
  const auto q = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  aurora::gx::g_enhancedMetalParams.store(q(roughness) | (q(metallic) << 8) | (q(edge) << 16) | (q(detail) << 24),
                                          std::memory_order_relaxed);
#endif
}
void aurora_set_enhanced_metal(bool enabled, float strength) {
  aurora::g_config.enhancedMetal = enabled;
  aurora::g_config.enhancedMetalStrength = strength;
#ifdef AURORA_ENABLE_GX
  aurora::gx::g_enhancedMetal.store(enabled, std::memory_order_relaxed);
  aurora::gx::g_enhancedMetalStrength.store(strength, std::memory_order_relaxed);
#endif
}
void aurora_set_texture_upscale(uint32_t factor, uint32_t maxSourceSize, float sharpen) {
  aurora::g_config.textureUpscaleFactor = factor;
  aurora::g_config.textureUpscaleMaxSize = maxSourceSize;
  aurora::g_config.textureUpscaleSharpen = sharpen;
#ifdef AURORA_ENABLE_GX
  aurora::gfx::texture_upscale::set_config(factor, maxSourceSize, sharpen);
#endif
}
AuroraTextureUpscaleStats aurora_get_texture_upscale_stats(void) {
  AuroraTextureUpscaleStats out{};
#ifdef AURORA_ENABLE_GX
  const auto s = aurora::gfx::texture_upscale::stats();
  out.upscaled = s.upscaled;
  out.cacheHits = s.cacheHits;
  out.skipped = s.skipped;
  out.pending = s.pending;
  out.cacheBytes = s.cacheBytes;
  out.cpuMicroseconds = s.cpuMicroseconds;
#endif
  return out;
}
void aurora_set_pause_on_focus_lost(bool value) { aurora::g_config.pauseOnFocusLost = value; }
void aurora_set_disable_copy_filter(bool disabled) { aurora::g_config.disableCopyFilter = disabled; }
void aurora_set_post_process(const AuroraPostProcess* settings) {
  if (settings == nullptr) return;
  aurora::gfx::post::Config c;
  c.sharpen = settings->sharpen; c.sharpenAmount = settings->sharpenAmount;
  c.fxaa = settings->fxaa;
  c.bloom = settings->bloom; c.bloomThreshold = settings->bloomThreshold;
  c.bloomIntensity = settings->bloomIntensity; c.bloomRadius = settings->bloomRadius;
  c.ssao = settings->ssao; c.ssaoRadius = settings->ssaoRadius; c.ssaoIntensity = settings->ssaoIntensity;
  c.ssaoSoftness = settings->ssaoSoftness > 0.0f ? settings->ssaoSoftness : 2.0f;
  c.ssaoSamples = settings->ssaoSamples > 0.0f ? settings->ssaoSamples : 12.0f;
  c.ssaoBias = settings->ssaoBias; c.ssaoPower = settings->ssaoPower > 0.0f ? settings->ssaoPower : 1.0f;
  c.ssaoFadeDistance = settings->ssaoFadeDistance; c.ssaoEdgeSharpness = settings->ssaoEdgeSharpness;
  c.bloomWorldOnly = settings->bloomWorldOnly;
  c.ssaoGlowProtect = settings->ssaoGlowProtect;
  c.ssaoSkipMarked = settings->ssaoSkipMarked;
  c.ssr = settings->ssr;
  c.ssrStrength = settings->ssrStrength;
  c.ssrRoughness = settings->ssrRoughness;
  c.ssrRelief = settings->ssrRelief;
  c.specOcclusion = settings->specOcclusion;
  c.lightShafts = settings->lightShafts;
  c.lightShaftsIntensity = settings->lightShaftsIntensity;
  c.glowFix = settings->glowFix;
  c.wallSsr = settings->wallSsr;
  c.wallSsrStrength = settings->wallSsrStrength;
  c.color = settings->color; c.brightness = settings->brightness; c.contrast = settings->contrast;
  c.saturation = settings->saturation; c.gamma = settings->gamma; c.temperature = settings->temperature;
  c.vignette = settings->vignette;
  aurora::gfx::post::set_config(c);
}
void aurora_get_post_process(AuroraPostProcess* settings) {
  if (settings == nullptr) return;
  const auto c = aurora::gfx::post::config();
  *settings = AuroraPostProcess{c.sharpen, c.sharpenAmount, c.fxaa, c.bloom, c.bloomThreshold, c.bloomIntensity,
                                c.bloomRadius, c.ssao, c.ssaoRadius, c.ssaoIntensity, c.color, c.brightness,
                                c.contrast, c.saturation, c.gamma, c.temperature, c.vignette, c.ssaoSoftness,
                                c.ssaoSamples, c.ssaoBias, c.ssaoPower, c.ssaoFadeDistance, c.ssaoEdgeSharpness,
                                c.bloomWorldOnly, c.ssaoGlowProtect, c.ssaoSkipMarked, c.ssr, c.ssrStrength,
                                c.ssrRoughness, c.ssrRelief, c.specOcclusion, c.lightShafts,
                                c.lightShaftsIntensity, c.glowFix, c.wallSsr, c.wallSsrStrength};
}
void aurora_set_ssao_exclude_textures(const uint64_t* hashes, size_t count) {
  aurora::gfx::post::set_ssao_exclude_textures(hashes, hashes != nullptr ? count : 0);
}
void aurora_set_lighting_features(const AuroraLightingFeatures* features) {
  if (features == nullptr) return;
  aurora::gx::g_litRoomMaterial.store(features->litRooms, std::memory_order_relaxed);
  aurora::gx::g_softHighlights.store(features->softHighlights, std::memory_order_relaxed);
  aurora::gx::g_linearBlend.store(features->linearBlend, std::memory_order_relaxed);
  aurora::gx::g_smoothLightmaps.store(features->smoothLightmaps, std::memory_order_relaxed);
  aurora::gx::g_areaReflections.store(features->areaReflections, std::memory_order_relaxed);
  aurora::gx::g_softShadows.store(features->softShadows, std::memory_order_relaxed);
  aurora::gfx::post::set_area_probe(features->areaReflections);
}
void aurora_set_engine_lights(const AuroraEngineLight* lights, uint32_t count) {
  aurora::gx::set_engine_lights(lights, count);
}
void aurora_set_modern_lighting(const AuroraModernLighting* settings) {
  if (settings == nullptr) return;
  aurora::gx::g_engineLightsOption.store(settings->engineLights, std::memory_order_relaxed);
  aurora::gx::g_extraDynamicLights.store(settings->extraDynamicLights, std::memory_order_relaxed);
  aurora::gx::g_lightmapDither.store(settings->lightmapDither, std::memory_order_relaxed);
  aurora::gfx::post::Modern modern{};
  modern.contact = settings->contactShadows;
  modern.contactStrength = settings->contactStrength;
  modern.contactLength = settings->contactLength;
  modern.temporal = settings->temporal;
  modern.exposure = settings->exposure;
  modern.exposureRange = settings->exposureRange;
  modern.exposureSpeed = settings->exposureSpeed;
  aurora::gfx::post::set_modern(modern);
}
void aurora_set_environment_lighting(bool material, bool borrowedLights) {
  aurora::gx::g_envMaterial.store(material, std::memory_order_relaxed);
  aurora::gx::g_envBorrowedLights.store(borrowedLights, std::memory_order_relaxed);
}
void aurora_set_enhanced_materials(bool enabled) {
  aurora::gx::g_enhancedMaterials.store(enabled, std::memory_order_relaxed);
}
void aurora_set_material_class(uint8_t cls, float strength, float roughness, float metallic, float edge, float detail) {
  if (cls == 0 || cls >= aurora::gx::g_materialClasses.size()) return;
  const auto q = [](float v) { return static_cast<uint64_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  const float s = std::clamp(strength, 0.0f, 2.0f);
  uint32_t strengthBits = 0;
  std::memcpy(&strengthBits, &s, sizeof(s));
  // A zero params word would mean "global parameters" in build_uniform: keep roughness >= 1.
  const uint64_t params = std::max<uint64_t>(q(roughness), 1) | (q(metallic) << 8) | (q(edge) << 16) | (q(detail) << 24);
  aurora::gx::g_materialClasses[cls].store((static_cast<uint64_t>(strengthBits) << 32) | params, std::memory_order_relaxed);
}
void aurora_set_material_ext(uint8_t cls, const AuroraMaterialExt* params) {
  if (cls >= aurora::gx::g_materialExt.size() || params == nullptr) return;
  constexpr size_t kFields = sizeof(AuroraMaterialExt) / sizeof(float);
  static_assert(kFields == 31 && kFields + 1 == aurora::gx::MaterialExtFloats);
  const auto* values = reinterpret_cast<const float*>(params);
  // Uniform order (shader.cpp, mat_ext): the 31 fields with the animation time at index 7.
  auto& slot = aurora::gx::g_materialExt[cls];
  for (size_t i = 0, src = 0; i < aurora::gx::MaterialExtFloats && src < kFields; ++i) {
    if (i == 7) continue;  // time, written by build_uniform
    slot[i].store(values[src++], std::memory_order_relaxed);
  }
}
void aurora_set_material_textures(uint8_t cls, const uint64_t* hashes, size_t count) {
  aurora::gfx::texture_replacement::set_texture_material_list(cls, hashes, hashes != nullptr ? count : 0);
}
void aurora_set_ssao_cover_textures(const uint64_t* hashes, size_t count) {
  aurora::gfx::post::set_ssao_cover_textures(hashes, hashes != nullptr ? count : 0);
}
void aurora_set_metal_exclude_textures(const uint64_t* hashes, size_t count) {
  aurora::gfx::texture_replacement::set_texture_tag_list(aurora::gfx::texture_replacement::TagNoMetal, hashes,
                                                         hashes != nullptr ? count : 0);
}
const char* aurora_post_process_status_note() { return aurora::gfx::post::status_note(); }
bool aurora_post_process_scene_active() { return aurora::gfx::post::scene_active(); }
bool aurora_get_disable_copy_filter() { return aurora::g_config.disableCopyFilter; }
void aurora_set_background_input(bool value) {
  aurora::g_config.allowJoystickBackgroundEvents = value;
  aurora::window::set_background_input(value);
}
void aurora_set_display_mode(AuroraDisplayMode mode) { aurora::window::set_display_mode(mode); }
AuroraDisplayMode aurora_get_display_mode() { return aurora::window::get_display_mode(); }
float aurora_get_display_refresh_rate() { return aurora::window::display_refresh_rate(); }
void aurora_set_metalfx_spatial(bool enabled) { aurora::g_metalfxRequested.store(enabled); }
bool aurora_get_metalfx_spatial() { return aurora::g_metalfxRequested.load(); }
bool aurora_is_metalfx_spatial_supported() { return aurora::g_metalfxSupported.load(); }
AuroraMetalFXStatus aurora_get_metalfx_status() { return aurora::g_metalfxStatus.load(); }
