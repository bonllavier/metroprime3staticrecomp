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
#include <type_traits>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_2.h>
#elif defined(__linux__) && __has_include(<xcb/xcb.h>) && __has_include(<GL/glx.h>)
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <xcb/xcb.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <cstdlib>
#include <cstring>
#define AURORA_REFRESH_OBSERVER_X11 1
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

// With frame interpolation off, images go out at the game's own rate ([video] display_sync).
std::atomic<bool> g_nativeEnabled{false};
std::atomic<bool> g_observeAlways{false};
std::atomic<bool> g_frameLock{true};

// Whether presents are to be aligned in the current mode.
bool sync_wanted() noexcept {
#if defined(MKW_PRIME_QA_TOOLS)
  // Private QA: the unaligned presentation, for A/B runs.
  static const bool qaOff = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_QA_NO_DISPLAY_SYNC") != nullptr;
  if (qaOff) return false;
#endif
#ifdef AURORA_ENABLE_GX
  return gx::frame_interpolation_fps() != 0 ? g_enabled.load(std::memory_order_acquire)
                                            : g_nativeEnabled.load(std::memory_order_acquire);
#else
  return false;
#endif
}

// Whether the refresh is to be measured at all.
bool wanted() noexcept {
  return sync_wanted() || g_observeAlways.load(std::memory_order_acquire) || g_frameLock.load(std::memory_order_acquire);
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
#elif defined(AURORA_REFRESH_OBSERVER_X11)
// Linux under X11 (also XWayland, which is how games run on Steam Deck under gamescope). Two sources of the
// monitor's vertical blanks, both loaded at run time so the port starts without them:
// 1. The X server's Present extension: a PresentNotifyMSC request on the game's window completes at the next
//    vertical blank of the monitor that window is on, and its CompleteNotify event carries the blank's time
//    (UST, microseconds of CLOCK_MONOTONIC, the clock of steady_clock). Mesa's drivers (AMD, Intel, nouveau)
//    and XWayland fill it from the kernel. Only blanks with that time are used: NVIDIA's driver leaves it at
//    zero and its completions arrive anywhere within the refresh (measured +-4 ms at 120 Hz, 16/11/2026).
// 2. NVIDIA's own wait (GLX_SGI_video_sync), precise to ~20 us, on a hidden window of our own. It always
//    follows NVIDIA's first display device, whatever the window, so it is used only with a single monitor.
// Under native Wayland there is no X11 window and nothing is measured; then, as on any system where no source
// works, presents stay on the game's schedule and the game keeps the console's speed.
constexpr uint32_t kPresentEventMaskCompleteNotify = 2;  // XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY
constexpr uint16_t kPresentCompleteNotify = 1;           // XCB_PRESENT_EVENT_COMPLETE_NOTIFY
constexpr uint8_t kPresentCompleteKindNotifyMsc = 1;     // XCB_PRESENT_COMPLETE_KIND_NOTIFY_MSC

template <typename Function>
bool load_symbol(void* library, Function& function, const char* name) noexcept {
  function = reinterpret_cast<Function>(dlsym(library, name));
  return function != nullptr;
}

struct Xcb {
  void* core = nullptr;
  void* present = nullptr;
  xcb_connection_t* (*connect)(const char*, int*) = nullptr;
  int (*connection_has_error)(xcb_connection_t*) = nullptr;
  void (*disconnect)(xcb_connection_t*) = nullptr;
  uint32_t (*generate_id)(xcb_connection_t*) = nullptr;
  int (*flush)(xcb_connection_t*) = nullptr;
  int (*get_file_descriptor)(xcb_connection_t*) = nullptr;
  xcb_generic_event_t* (*poll_for_event)(xcb_connection_t*) = nullptr;
  const xcb_query_extension_reply_t* (*get_extension_data)(xcb_connection_t*, xcb_extension_t*) = nullptr;
  xcb_void_cookie_t (*present_select_input)(xcb_connection_t*, uint32_t, xcb_window_t, uint32_t) = nullptr;
  xcb_void_cookie_t (*present_notify_msc)(xcb_connection_t*, xcb_window_t, uint32_t, uint64_t, uint64_t,
                                          uint64_t) = nullptr;
  xcb_extension_t* present_id = nullptr;

  bool load() noexcept {
    core = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
    present = dlopen("libxcb-present.so.0", RTLD_NOW | RTLD_LOCAL);
    if (core == nullptr || present == nullptr) {
      return false;
    }
    present_id = static_cast<xcb_extension_t*>(dlsym(present, "xcb_present_id"));
    return present_id != nullptr && load_symbol(core, connect, "xcb_connect") &&
           load_symbol(core, connection_has_error, "xcb_connection_has_error") &&
           load_symbol(core, disconnect, "xcb_disconnect") && load_symbol(core, generate_id, "xcb_generate_id") &&
           load_symbol(core, flush, "xcb_flush") && load_symbol(core, get_file_descriptor, "xcb_get_file_descriptor") &&
           load_symbol(core, poll_for_event, "xcb_poll_for_event") &&
           load_symbol(core, get_extension_data, "xcb_get_extension_data") &&
           load_symbol(present, present_select_input, "xcb_present_select_input") &&
           load_symbol(present, present_notify_msc, "xcb_present_notify_msc");
  }
};

// One vertical blank reported by the X server.
struct Blank {
  uint32_t serial = 0;
  uint64_t ust = 0;
  uint64_t msc = 0;
};

// CompleteNotify, as libxcb hands it over (32 bytes of the event, its full sequence, then the rest):
// event_type at 8, kind at 10, serial at 20, ust at 24, full_sequence at 32, msc at 36.
bool read_blank(const xcb_generic_event_t* event, uint8_t presentOpcode, Blank& blank) noexcept {
  if ((event->response_type & 0x7f) != XCB_GE_GENERIC) {
    return false;
  }
  const auto* bytes = reinterpret_cast<const uint8_t*>(event);
  uint16_t type = 0;
  std::memcpy(&type, bytes + 8, sizeof(type));
  if (bytes[1] != presentOpcode || type != kPresentCompleteNotify || bytes[10] != kPresentCompleteKindNotifyMsc) {
    return false;
  }
  std::memcpy(&blank.serial, bytes + 20, sizeof(blank.serial));
  std::memcpy(&blank.ust, bytes + 24, sizeof(blank.ust));
  std::memcpy(&blank.msc, bytes + 36, sizeof(blank.msc));
  return true;
}

// The vertical blanks of the window's monitor through Present.
class PresentSource {
public:
  bool loaded() const noexcept { return m_loaded; }
  bool load() noexcept { return m_loaded = m_xcb.load(); }
  bool open() const noexcept { return m_connection != nullptr; }
  xcb_window_t window() const noexcept { return m_window; }

  bool open(xcb_window_t window) noexcept {
    close();
    m_connection = m_xcb.connect(nullptr, nullptr);
    const xcb_query_extension_reply_t* extension =
        m_connection != nullptr && m_xcb.connection_has_error(m_connection) == 0
            ? m_xcb.get_extension_data(m_connection, m_xcb.present_id)
            : nullptr;
    if (extension == nullptr || extension->present == 0) {
      close();
      return false;
    }
    m_opcode = extension->major_opcode;
    m_xcb.present_select_input(m_connection, m_xcb.generate_id(m_connection), window, kPresentEventMaskCompleteNotify);
    m_xcb.flush(m_connection);
    m_window = window;
    return true;
  }

  void close() noexcept {
    if (m_connection != nullptr) {
      m_xcb.disconnect(m_connection);
    }
    m_connection = nullptr;
    m_window = 0;
    m_pending = false;
    m_lastMsc = 0;
  }

  // Waits up to 100 ms for the next blank. Returns false on timeout; `failed` when the connection broke or
  // no blank came for a second (minimized or unmapped window).
  bool next(Blank& blank, bool& failed) noexcept {
    failed = false;
    if (!m_pending) {
      ++m_serial;
      if (m_lastMsc != 0) {
        m_xcb.present_notify_msc(m_connection, m_window, m_serial, m_lastMsc + 1, 0, 0);
      } else {
        m_xcb.present_notify_msc(m_connection, m_window, m_serial, 0, 1, 0);
      }
      m_xcb.flush(m_connection);
      m_pending = true;
      m_sentAt = steady_nanos();
    }
    pollfd descriptor{m_xcb.get_file_descriptor(m_connection), POLLIN, 0};
    poll(&descriptor, 1, 100);
    bool got = false;
    while (xcb_generic_event_t* event = m_xcb.poll_for_event(m_connection)) {
      Blank candidate;
      const bool isBlank = read_blank(event, m_opcode, candidate);
      std::free(event);
      if (!isBlank || candidate.serial != m_serial || !m_pending) {
        continue;
      }
      m_pending = false;
      if (candidate.msc <= m_lastMsc) {
        continue;  // completed at once on a blank already seen
      }
      m_lastMsc = candidate.msc;
      blank = candidate;
      got = true;
    }
    if (m_xcb.connection_has_error(m_connection) != 0) {
      failed = true;
    } else if (!got && m_pending && steady_nanos() - m_sentAt > 1'000'000'000) {
      m_pending = false;
      m_lastMsc = 0;
      failed = true;
    }
    return got;
  }

private:
  Xcb m_xcb;
  bool m_loaded = false;
  xcb_connection_t* m_connection = nullptr;
  uint8_t m_opcode = 0;
  xcb_window_t m_window = 0;
  uint32_t m_serial = 0;
  bool m_pending = false;
  int64_t m_sentAt = 0;
  uint64_t m_lastMsc = 0;
};

// NVIDIA's vertical-blank wait (GLX_SGI_video_sync) on a hidden window of our own.
class GlxSource {
public:
  bool open() const noexcept { return m_context != nullptr; }

  bool open() noexcept {
    if (m_context != nullptr) {
      return true;
    }
    if (m_failed || !load()) {
      m_failed = true;
      return false;
    }
    m_display = m_openDisplay(nullptr);
    if (m_display == nullptr) {
      m_failed = true;
      return false;
    }
    const int screen = m_defaultScreen(m_display);
    const char* extensions = m_queryExtensionsString(m_display, screen);
    if (extensions == nullptr || std::strstr(extensions, "GLX_SGI_video_sync") == nullptr) {
      close();
      m_failed = true;
      return false;
    }
    int attributes[] = {GLX_RGBA, None};
    XVisualInfo* visual = m_chooseVisual(m_display, screen, attributes);
    if (visual == nullptr) {
      close();
      m_failed = true;
      return false;
    }
    const Window root = m_rootWindow(m_display, screen);
    XSetWindowAttributes windowAttributes{};
    m_colormap = m_createColormap(m_display, root, visual->visual, AllocNone);
    windowAttributes.colormap = m_colormap;
    // Never mapped: the wait does not depend on it, and nothing appears on screen.
    m_window = m_createWindow(m_display, root, 0, 0, 1, 1, 0, visual->depth, InputOutput, visual->visual, CWColormap,
                              &windowAttributes);
    m_context = m_createContext(m_display, visual, nullptr, True);
    m_free(visual);
    if (m_context == nullptr || m_makeCurrent(m_display, m_window, m_context) == False) {
      close();
      m_failed = true;
      return false;
    }
    return true;
  }

  void close() noexcept {
    if (m_display != nullptr) {
      if (m_context != nullptr) {
        m_makeCurrent(m_display, None, nullptr);
        m_destroyContext(m_display, m_context);
      }
      if (m_window != 0) {
        m_destroyWindow(m_display, m_window);
      }
      if (m_colormap != 0) {
        m_freeColormap(m_display, m_colormap);
      }
      m_closeDisplay(m_display);
    }
    m_display = nullptr;
    m_context = nullptr;
    m_window = 0;
    m_colormap = 0;
  }

  // Blocks until the next vertical blank (at most one refresh); false when the wait fails.
  bool next() noexcept {
    // The blank after the last one: the counter's next parity.
    return m_waitVideoSync(2, static_cast<int>((m_count + 1) % 2), &m_count) == 0;
  }

private:
  bool load() noexcept {
    void* x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    void* gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (x11 == nullptr || gl == nullptr) {
      return false;
    }
    if (!load_symbol(x11, m_openDisplay, "XOpenDisplay") || !load_symbol(x11, m_closeDisplay, "XCloseDisplay") ||
        !load_symbol(x11, m_defaultScreen, "XDefaultScreen") || !load_symbol(x11, m_rootWindow, "XRootWindow") ||
        !load_symbol(x11, m_createColormap, "XCreateColormap") || !load_symbol(x11, m_freeColormap, "XFreeColormap") ||
        !load_symbol(x11, m_createWindow, "XCreateWindow") || !load_symbol(x11, m_destroyWindow, "XDestroyWindow") ||
        !load_symbol(x11, m_free, "XFree") || !load_symbol(gl, m_chooseVisual, "glXChooseVisual") ||
        !load_symbol(gl, m_createContext, "glXCreateContext") || !load_symbol(gl, m_makeCurrent, "glXMakeCurrent") ||
        !load_symbol(gl, m_destroyContext, "glXDestroyContext") ||
        !load_symbol(gl, m_queryExtensionsString, "glXQueryExtensionsString") ||
        !load_symbol(gl, m_getProcAddress, "glXGetProcAddress")) {
      return false;
    }
    m_waitVideoSync = reinterpret_cast<int (*)(int, int, unsigned int*)>(
        m_getProcAddress(reinterpret_cast<const GLubyte*>("glXWaitVideoSyncSGI")));
    return m_waitVideoSync != nullptr;
  }

  Display* (*m_openDisplay)(const char*) = nullptr;
  int (*m_closeDisplay)(Display*) = nullptr;
  int (*m_defaultScreen)(Display*) = nullptr;
  Window (*m_rootWindow)(Display*, int) = nullptr;
  Colormap (*m_createColormap)(Display*, Window, Visual*, int) = nullptr;
  int (*m_freeColormap)(Display*, Colormap) = nullptr;
  Window (*m_createWindow)(Display*, Window, int, int, unsigned int, unsigned int, unsigned int, int, unsigned int,
                           Visual*, unsigned long, XSetWindowAttributes*) = nullptr;
  int (*m_destroyWindow)(Display*, Window) = nullptr;
  int (*m_free)(void*) = nullptr;
  XVisualInfo* (*m_chooseVisual)(Display*, int, int*) = nullptr;
  GLXContext (*m_createContext)(Display*, XVisualInfo*, GLXContext, Bool) = nullptr;
  Bool (*m_makeCurrent)(Display*, GLXDrawable, GLXContext) = nullptr;
  void (*m_destroyContext)(Display*, GLXContext) = nullptr;
  const char* (*m_queryExtensionsString)(Display*, int) = nullptr;
  __GLXextFuncPtr (*m_getProcAddress)(const GLubyte*) = nullptr;
  int (*m_waitVideoSync)(int, int, unsigned int*) = nullptr;
  Display* m_display = nullptr;
  Window m_window = 0;
  Colormap m_colormap = 0;
  GLXContext m_context = nullptr;
  unsigned int m_count = 0;
  bool m_failed = false;
};

// The driver's wait follows NVIDIA's first display device, which is the window's monitor only for sure when
// there is a single one.
bool glx_allowed() noexcept {
#if defined(MKW_PRIME_QA_TOOLS)
  // Private QA: the driver's wait with several monitors, for a window on NVIDIA's first display device.
  static const bool qaForceGlx = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_QA_REFRESH_GLX") != nullptr;
  if (qaForceGlx) return true;
#endif
  return window::display_count() == 1;
}

void observer_main() noexcept {
  pthread_setname_np(pthread_self(), "refresh-sync");
  enum class Source { Off, Present, Glx };
  PresentSource present;
  GlxSource glx;
  if (!present.load()) {
    Log.info("[refresh-sync] libxcb-present is not available; trying the GPU driver's own wait");
  }
  Source source = Source::Off;
  bool presentWithoutTime = false;  // this server's Present gives no blank times (NVIDIA)
  uint32_t displayId = 0;
  xcb_window_t xWindow = 0;
  gx::policy::RefreshEstimator estimator;
  bool reportedUsable = false;
  bool reportedGivenUp = false;
  bool reportedNoWindow = false;
  bool reportedNoSource = false;
  int64_t nextWindowCheck = 0;
  uint32_t failures = 0;

  const auto restart_timeline = [&] {
    estimator = {};
    publish({}, 0.0);
    reportedUsable = false;
    reportedGivenUp = false;
  };
  const auto stop_sources = [&] {
    present.close();
    glx.close();
    source = Source::Off;
  };
  const auto observe = [&](int64_t at) {
    gx::policy::observe_refresh(estimator, at, window::display_period_nanos());
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
  };
  // Picks the source for the current window and monitor.
  const auto choose_source = [&]() -> Source {
    if (present.loaded() && !presentWithoutTime && present.open(xWindow)) {
      const int64_t mode = window::display_period_nanos();
      Log.info("[refresh-sync] watching the refresh of this window's monitor (X11 Present, mode {:.2f} Hz)",
               mode > 0 ? 1.0e9 / static_cast<double>(mode) : 0.0);
      return Source::Present;
    }
    if (glx_allowed() && glx.open()) {
      Log.info("[refresh-sync] watching the monitor's refresh through the driver's GLX_SGI_video_sync");
      return Source::Glx;
    }
    if (!reportedNoSource) {
      reportedNoSource = true;
      Log.info("[refresh-sync] no precise source of the monitor's refresh here ({}); presents stay on the game's "
               "schedule at the console's speed",
               presentWithoutTime ? "Present gives no blank times and there is more than one monitor"
                                  : "no X11 Present and no GLX_SGI_video_sync");
    }
    return Source::Off;
  };

  while (!g_stop.load(std::memory_order_acquire)) {
    if (!wanted()) {
      if (source != Source::Off) {
        stop_sources();
        restart_timeline();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    const int64_t now = steady_nanos();
    if (now >= nextWindowCheck) {
      nextWindowCheck = now + 500'000'000;
      const auto wantedWindow = static_cast<xcb_window_t>(window::x11_window());
      const uint32_t wantedDisplay = window::display_id();
      if (wantedWindow == 0) {
        if (!reportedNoWindow && window::get_sdl_window() != nullptr) {
          reportedNoWindow = true;
          Log.info("[refresh-sync] not an X11 window (native Wayland?): the monitor's refresh is not measured; "
                   "presents stay on the game's schedule at the console's speed");
        }
        if (source != Source::Off) {
          stop_sources();
          restart_timeline();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      const bool glxStillValid = source != Source::Glx || glx_allowed();
      if (wantedWindow != xWindow || wantedDisplay != displayId || !glxStillValid) {
        // Another window or monitor: a new timeline, and alignment may be tried again there.
        stop_sources();
        restart_timeline();
        xWindow = wantedWindow;
        displayId = wantedDisplay;
        reportedNoSource = false;
        source = choose_source();
      }
    }
    if (source == Source::Off) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    if (source == Source::Glx) {
      if (!glx.next()) {
        if (++failures == 20) {
          Log.warn("[refresh-sync] the driver's vertical-blank wait fails; presents stay on the game's schedule");
        }
        publish({}, 0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(failures < 20 ? 50 : 2000));
        continue;
      }
      failures = 0;
      observe(steady_nanos());
      continue;
    }
    Blank blank;
    bool failed = false;
    if (present.next(blank, failed)) {
      failures = 0;
      const int64_t at = static_cast<int64_t>(blank.ust) * 1000;
      const int64_t arrived = steady_nanos();
      if (blank.ust == 0 || at > arrived + 1'000'000 || arrived - at > 100'000'000) {
        // No blank time (NVIDIA), or not on this clock: Present cannot place the refresh.
        Log.info("[refresh-sync] X11 Present gives no usable blank times on this system (ust {}); "
                 "trying the GPU driver's own wait", blank.ust);
        presentWithoutTime = true;
        stop_sources();
        restart_timeline();
        source = choose_source();
        continue;
      }
      observe(at);
    } else if (failed) {
      if (++failures == 20) {
        Log.warn("[refresh-sync] the monitor's vertical blank is not reported; presents stay on the game's schedule");
      }
      publish({}, 0.0);
      if (!present.open() || failures % 10 == 0) {
        // A broken connection, or a window that stays hidden: start over at the next check.
        stop_sources();
        xWindow = 0;
        nextWindowCheck = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
    }
  }
  stop_sources();
  publish({}, 0.0);
  g_exited.store(true, std::memory_order_release);
}
#endif
}  // namespace

void start() noexcept {
#if defined(_WIN32) || defined(AURORA_REFRESH_OBSERVER_X11)
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

// The observer drops the timeline by itself within 200 ms once nothing wants it; grid() stops handing
// it out at once.
void set_enabled(bool enabled) noexcept { g_enabled.store(enabled, std::memory_order_release); }

bool enabled() noexcept { return g_enabled.load(std::memory_order_acquire); }

void set_native_enabled(bool enabled) noexcept { g_nativeEnabled.store(enabled, std::memory_order_release); }

bool native_enabled() noexcept { return g_nativeEnabled.load(std::memory_order_acquire); }

void set_observe_always(bool enabled) noexcept { g_observeAlways.store(enabled, std::memory_order_release); }

void set_frame_lock_enabled(bool enabled) noexcept { g_frameLock.store(enabled, std::memory_order_release); }

bool frame_lock_enabled() noexcept { return g_frameLock.load(std::memory_order_acquire); }

gx::policy::RefreshGrid grid() noexcept {
  if (!sync_wanted()) {
    return {};
  }
  std::lock_guard lock(g_gridMutex);
  return g_grid;
}

gx::policy::RefreshGrid observed_grid() noexcept {
  std::lock_guard lock(g_gridMutex);
  return g_grid;
}

double locked_rate() noexcept {
  if (!sync_wanted()) {
    return 0.0;
  }
  std::lock_guard lock(g_gridMutex);
  return g_lockedRate;
}
}  // namespace aurora::display_refresh
