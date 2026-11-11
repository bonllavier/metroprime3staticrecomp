#include "display_refresh.hpp"

#include "diagnostic_env.hpp"
#include "internal.hpp"
#include "window.hpp"
#ifdef AURORA_ENABLE_GX
#include "gx/frame_interpolation.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_2.h>
#endif

namespace aurora::display_refresh {
namespace {
Module Log("aurora::display_refresh");

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_exited{false};
std::mutex g_threadMutex;
std::thread g_thread;

// Written by the observer thread, read by the presenter once per image.
std::mutex g_gridMutex;
gx::policy::RefreshGrid g_grid;
double g_lockedRate = 0.0;

bool wanted() noexcept {
#if defined(MKW_PRIME_QA_TOOLS)
  // Private QA: the unaligned presentation, for A/B runs.
  static const bool qaOff = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_QA_NO_DISPLAY_SYNC") != nullptr;
  if (qaOff) return false;
#endif
#ifdef AURORA_ENABLE_GX
  return g_enabled.load(std::memory_order_acquire) && gx::frame_interpolation_fps() != 0;
#else
  return false;
#endif
}

void publish(const gx::policy::RefreshGrid& grid, double rate) noexcept {
  std::lock_guard lock(g_gridMutex);
  g_grid = grid;
  g_lockedRate = rate;
}

int64_t steady_nanos() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#if defined(_WIN32)
struct Output {
  IDXGIOutput* output = nullptr;
  HMONITOR monitor = nullptr;
  int64_t nominalPeriod = 0;

  void reset() noexcept {
    if (output != nullptr) {
      output->Release();
    }
    output = nullptr;
    monitor = nullptr;
    nominalPeriod = 0;
  }
};

// The refresh of the display mode. Windows rounds 59.94 Hz down to 59 (and 119.88 to 119).
int64_t nominal_period(const wchar_t* deviceName) noexcept {
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &mode) == FALSE || mode.dmDisplayFrequency <= 1) {
    return 0;
  }
  double hz = static_cast<double>(mode.dmDisplayFrequency);
  if (mode.dmDisplayFrequency >= 29 && mode.dmDisplayFrequency % 10 == 9) {
    hz = (hz + 1.0) * 1000.0 / 1001.0;
  }
  return static_cast<int64_t>(std::llround(1.0e9 / hz));
}

bool open_output(Output& out, HMONITOR monitor) noexcept {
  out.reset();
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || factory == nullptr) {
    return false;
  }
  for (UINT adapterIndex = 0; out.output == nullptr; ++adapterIndex) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    for (UINT outputIndex = 0; adapter != nullptr; ++outputIndex) {
      IDXGIOutput* output = nullptr;
      if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      DXGI_OUTPUT_DESC desc{};
      if (output != nullptr && SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
        out.output = output;
        out.monitor = monitor;
        out.nominalPeriod = nominal_period(desc.DeviceName);
        break;
      }
      if (output != nullptr) {
        output->Release();
      }
    }
    if (adapter != nullptr) {
      adapter->Release();
    }
  }
  factory->Release();
  return out.output != nullptr;
}

void observer_main() noexcept {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  Output out;
  gx::policy::RefreshEstimator estimator;
  bool reportedUsable = false;
  bool reportedGivenUp = false;
  int64_t nextMonitorCheck = 0;
  uint32_t failures = 0;
  while (!g_stop.load(std::memory_order_acquire)) {
    if (!wanted()) {
      if (out.output != nullptr) {
        out.reset();
        estimator = {};
        publish({}, 0.0);
        reportedUsable = false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    const int64_t now = steady_nanos();
    if (out.output == nullptr || now >= nextMonitorCheck) {
      nextMonitorCheck = now + 500'000'000;
      const HWND window = static_cast<HWND>(window::native_window_handle());
      const HMONITOR monitor = window != nullptr ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : nullptr;
      if (monitor == nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      if (monitor != out.monitor || out.output == nullptr) {
        // Another monitor: a new timeline, and alignment may be tried again there.
        estimator = {};
        publish({}, 0.0);
        reportedUsable = false;
        reportedGivenUp = false;
        if (!open_output(out, monitor)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
          continue;
        }
        Log.info("[refresh-sync] watching the refresh of this window's monitor (mode {:.2f} Hz)",
                 out.nominalPeriod > 0 ? 1.0e9 / static_cast<double>(out.nominalPeriod) : 0.0);
      } else {
        // The display mode can change under the same monitor.
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(out.output->GetDesc(&desc))) {
          out.nominalPeriod = nominal_period(desc.DeviceName);
        }
      }
    }
    if (FAILED(out.output->WaitForVBlank())) {
      if (++failures == 20) {
        Log.warn("[refresh-sync] the monitor's vertical blank cannot be waited on; presents stay on the game's schedule");
      }
      out.reset();
      publish({}, 0.0);
      std::this_thread::sleep_for(std::chrono::milliseconds(failures < 20 ? 50 : 2000));
      continue;
    }
    failures = 0;
    const int64_t at = steady_nanos();
    gx::policy::observe_refresh(estimator, at, out.nominalPeriod);
    const bool usable = gx::policy::refresh_usable(estimator, at);
    const double rate = estimator.period > 0.0 ? 1.0e9 / estimator.period : 0.0;
    publish(usable ? gx::policy::RefreshGrid{estimator.vblank, static_cast<int64_t>(std::llround(estimator.period))}
                   : gx::policy::RefreshGrid{},
            usable ? rate : 0.0);
    if (usable != reportedUsable) {
      reportedUsable = usable;
      if (usable) {
        Log.info("[refresh-sync] presents aligned to the monitor's refresh: {:.3f} Hz, jitter {:.0f} us", rate,
                 estimator.jitter / 1000.0);
      } else {
        Log.info("[refresh-sync] refresh timeline lost (jitter {:.0f} us, outliers {:.0f} %, losses {}); "
                 "presents follow the game's schedule",
                 estimator.jitter / 1000.0, estimator.outliers * 100.0, estimator.trips);
      }
    }
    if (!reportedGivenUp && estimator.trips > gx::policy::kRefreshMaximumTrips) {
      reportedGivenUp = true;
      Log.warn("[refresh-sync] the refresh is not regular (variable refresh such as G-Sync/FreeSync?); "
               "alignment stays off on this monitor");
    }
  }
  out.reset();
  publish({}, 0.0);
  g_exited.store(true, std::memory_order_release);
}
#endif
}  // namespace

void start() noexcept {
#if defined(_WIN32)
  std::lock_guard lock(g_threadMutex);
  if (g_thread.joinable()) {
    return;
  }
  g_stop.store(false, std::memory_order_release);
  g_exited.store(false, std::memory_order_release);
  g_thread = std::thread(observer_main);
#endif
}

void stop() noexcept {
  std::lock_guard lock(g_threadMutex);
  g_stop.store(true, std::memory_order_release);
  if (!g_thread.joinable()) {
    return;
  }
  // A vertical-blank wait returns within a refresh, but never let a display that stopped signalling
  // hold up shutdown: after half a second the thread is left to end with the process.
  const auto giveUp = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!g_exited.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < giveUp) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (g_exited.load(std::memory_order_acquire)) {
    g_thread.join();
  } else {
    g_thread.detach();
  }
}

void set_enabled(bool enabled) noexcept {
  g_enabled.store(enabled, std::memory_order_release);
  if (!enabled) {
    publish({}, 0.0);
  }
}

bool enabled() noexcept { return g_enabled.load(std::memory_order_acquire); }

gx::policy::RefreshGrid grid() noexcept {
  if (!g_enabled.load(std::memory_order_acquire)) {
    return {};
  }
  std::lock_guard lock(g_gridMutex);
  return g_grid;
}

double locked_rate() noexcept {
  std::lock_guard lock(g_gridMutex);
  return g_lockedRate;
}
}  // namespace aurora::display_refresh
