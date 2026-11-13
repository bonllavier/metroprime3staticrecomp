#ifndef AURORA_AURORA_H
#define AURORA_AURORA_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#endif

typedef enum {
  BACKEND_AUTO,
  BACKEND_D3D11,
  BACKEND_D3D12,
  BACKEND_METAL,
  BACKEND_VULKAN,
  BACKEND_OPENGL,
  BACKEND_OPENGLES,
  BACKEND_WEBGPU,
  BACKEND_NULL,
} AuroraBackend;

typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
  LOG_FATAL,
} AuroraLogLevel;

typedef enum {
  AURORA_DISPLAY_MODE_WINDOWED,
  AURORA_DISPLAY_MODE_BORDERLESS,
  AURORA_DISPLAY_MODE_EXCLUSIVE,
} AuroraDisplayMode;

typedef struct {
  int32_t x;
  int32_t y;
} AuroraWindowPos;

typedef struct {
  uint32_t width;
  uint32_t height;

  /**
   * Width of the main GX framebuffer.
   */
  uint32_t fb_width;

  /**
   * Height of the main GX framebuffer.
   */
  uint32_t fb_height;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_width if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_width;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_height if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_height;
  float scale;
} AuroraWindowSize;

typedef struct SDL_Window SDL_Window;
typedef struct AuroraEvent AuroraEvent;

typedef void (*AuroraLogCallback)(AuroraLogLevel level, const char* module, const char* message, unsigned int len);
typedef void (*AuroraImGuiInitCallback)(const AuroraWindowSize* size);

typedef struct {
  const char* appName;
  const char* userPath;
  const char* cachePath;
  // Read-only application resources. Defaults to SDL_GetBasePath(), which is
  // where release builds place initial_pipeline_cache.db.
  const char* resourcesPath;
  AuroraBackend desiredBackend;
  uint32_t msaa;
  uint16_t maxTextureAnisotropy;
  // 0: anisotropy as each GX texture object requests (capped by maxTextureAnisotropy).
  // 2..16: every eligible sampler (trilinear) uses this anisotropy instead.
  uint16_t forceTextureAnisotropy;
  // Identifies the build generation that owns the persistent shader caches (the
  // application version). When it differs from the one stored in dawn_cache.db, the
  // first launch drops every compiled blob that launch did not use again (obsolete
  // keys of the previous build) and compacts the file. NULL disables that cleanup.
  const char* cacheGeneration;
  // No vsync knob exists: the swapchain is always configured for a
  // non-blocking present mode (Immediate, else Mailbox). See best_present_mode.
  bool startFullscreen;
  bool allowJoystickBackgroundEvents;
  bool pauseOnFocusLost;
  bool allowTextureReplacements;
  bool allowTextureDumps;
  bool disableCopyFilter;
  // GX lighting evaluated per fragment (live toggle; see aurora_set_per_pixel_lighting).
  bool perPixelLighting;
  bool ubershader;  // hybrid ubershader (also settable at run time with aurora_set_ubershader)
  // Load-time Super-xBR texture upscaling: 0/1 = off, 2 or 4; textures up to
  // textureUpscaleMaxSize (width and height) are processed. See aurora_set_texture_upscale.
  uint32_t textureUpscaleFactor;
  uint32_t textureUpscaleMaxSize;
  // 0..1: unsharp mask strength applied to upscaled textures (0 = none).
  float textureUpscaleSharpen;
  // When false, Aurora centers the first window. When true, windowPosX/Y are restored verbatim,
  // including negative coordinates on monitors left of or above the primary display.
  bool hasWindowPosition;
  int32_t windowPosX;
  int32_t windowPosY;
  uint32_t windowWidth;
  uint32_t windowHeight;
  void* iconRGBA8;
  uint32_t iconWidth;
  uint32_t iconHeight;
  AuroraLogCallback logCallback;
  AuroraLogLevel logLevel;
  AuroraImGuiInitCallback imGuiInitCallback;

  /*
   * The size of the GameCube's main memory, or MEM1 on the Wii.
   * Note that it will not be allocated at the exact 0x80000000 address, as that cannot be guaranteed.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem1Size;

  /*
   * The size of the GameCube's ARAM, or MEM2 on the Wii.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem2Size;

  // Optional directory for the portable GX pipeline database. When null, the
  // database is stored in cachePath with Dawn's machine-specific cache.
  const char* pipelineCachePath;

  // Enhanced metal (needs perPixelLighting): per-pixel environment reflection, Fresnel and specular
  // highlights on Prime's metallic materials. Strength 0..2. See aurora_set_enhanced_metal.
  bool enhancedMetal;
  float enhancedMetalStrength;
} AuroraConfig;

typedef enum {
  AURORA_INITIALIZATION_SUCCESS = 0,
  AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE = 1,
} AuroraInitializationStatus;

typedef struct {
  AuroraBackend backend;
  const char* userPath;
  const char* cachePath;
  SDL_Window* window;
  AuroraWindowSize windowSize;
  AuroraInitializationStatus initializationStatus;
  // On failure, owned by SDL on the calling thread. Copy before another SDL call.
  const char* initializationError;
} AuroraInfo;

AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config);
void aurora_shutdown();
const AuroraEvent* aurora_update();
// Select before starting frames. Hardware EFB contents survive VI scanout;
// translated guests that copy and draw across host frames need this policy.
void aurora_set_efb_persistence(bool enabled);
// V-Sync ([video] vsync): present in step with the display's refresh (Fifo). Callable before
// aurora_initialize; afterwards the surface is reconfigured at the next frame.
void aurora_set_vsync(bool enabled);
bool aurora_get_vsync(void);
// Present mode of the window surface now: "Fifo", "Mailbox", "Immediate", ...
const char* aurora_get_present_mode_name(void);
bool aurora_begin_frame();
void aurora_end_frame();
typedef void (*AuroraFrameWorkerWaitCallback)();
// Called from the producer thread at bounded intervals while Aurora waits for
// the asynchronous frame worker. The callback must not enter Aurora.
void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback callback);
// Per calling thread: false keeps aurora_begin_frame from pumping window events,
// for a renderer thread that does not own the SDL window.
void aurora_set_thread_event_pump(bool enabled);
// std::chrono::steady_clock nanoseconds of the latest successful surface present, 0 before any.
int64_t aurora_last_present_steady_nanos(void);
// Rectangle of the presented game image inside the window surface, as fractions (0..1) of the
// surface size. Defaults to the whole surface before the first present.
void aurora_get_present_viewport_fraction(float* left, float* top, float* width, float* height);
void aurora_wait_for_frame_worker();
bool aurora_wait_for_frame_worker_for(uint32_t timeoutMicros);
// Until the frame worker has prepared the next frame (SEALED: ImGui's new frame
// started, staging mapped). What aurora_stream_sync also waits for, without
// draining the asynchronous decoder.
void aurora_wait_for_frame_worker_sealed(void);
// Absolute schedule for the next sealed frame, on steady_clock: baseNanos anchors the group and
// intervalNanos is the period, so slot k of N+1 fires at base + k*interval/(N+1). Zeros clear it.
void aurora_set_present_schedule(uint64_t baseNanos, uint64_t intervalNanos);
// Reports whether the frame about to be sealed met its display boundary. Interpolation sizes its
// slot group from this, backing off after misses. Only paced presents may report.
void aurora_report_producer_paced(bool paced);
void aurora_request_frame_capture(uint32_t frame, const char* outputPath);
// Screenshot of the next presented picture as the player sees it (image effects included, port
// overlay excluded), written as a 24-bit BMP to `outputPath`.
void aurora_request_screenshot(const char* outputPath);
// Dumps every GX draw and EFB copy of the next frame to <basePath>.draws.txt and
// its presented image to <basePath>.bmp.
void aurora_request_draw_dump(const char* basePath);
// The same for `frames` consecutive frames, written as <basePath>_f0 .. _f<frames-1>.
void aurora_request_draw_dump_burst(const char* basePath, uint32_t frames);
// Test builds (MKW_PRIME_QA_TOOLS) only, otherwise nothing: with frame interpolation running, saves the
// first inserted image and the real one of the next `frames` frames as half-size BMPs in `directory`
// (interp_<n>_<frame>_a_inserted.bmp / _b_real.bmp). Draw dumps and screenshots only see real frames.
void aurora_request_interpolation_capture(const char* directory, uint32_t frames);
// Asynchronous GX decoder: the calling (producer) thread only writes GX wire
// commands; a decoder thread applies them in order. Set before any GX work.
// Host code that reads or mutates decoder state must aurora_stream_sync first.
void aurora_set_async_decoder(bool enabled);
bool aurora_async_decoder_enabled(void);
typedef void (*AuroraStreamCallback)(uint32_t id, const uint8_t* data, uint32_t size);
void aurora_set_stream_callback(AuroraStreamCallback callback);
// Queues a host callback in GX stream order: the decoder thread invokes it
// outside the renderer lock after every earlier command. Without the
// asynchronous decoder it runs immediately. The payload is copied.
void aurora_stream_host_callback(uint32_t id, const void* data, uint32_t size);
// Deferred draws: a host whose vertex expansion is expensive queues the compact
// form of a draw (its own payload) and the decoder thread expands it, in stream
// order, through the registered expander. The expander returns the draw in GX
// wire form, cmd (opcode | vtxfmt), u16 big-endian count and big-endian vertex
// bytes, in a buffer it owns until its next call, or nullptr to skip the draw.
// aurora_stream_deferred_draw returns false (the caller draws itself) without
// the asynchronous decoder or an expander. The payload is copied.
typedef const uint8_t* (*AuroraDeferredDrawExpander)(const uint8_t* payload, uint32_t size, uint32_t* wireBytes);
void aurora_set_deferred_draw_expander(AuroraDeferredDrawExpander expander);
bool aurora_stream_deferred_draw(const void* payload, uint32_t size);
// Producer, lock-free: chunks the decoder has queued or in progress right now.
// For load balancing only (e.g. deferring work only while the decoder keeps up).
uint32_t aurora_stream_decoder_backlog_hint(void);
// Producer: hands the written commands to the decoder now (no wait).
void aurora_stream_flush(void);
// Producer: waits until the decoder has applied every queued command.
void aurora_stream_sync(void);
bool aurora_stream_idle(void);
bool aurora_on_decoder_thread(void);
uint32_t aurora_get_decoder_thread_id(void);
void aurora_stream_shutdown(void);
// Decode large GX textures across a worker pool (identical output); on by default.
void aurora_set_parallel_texture_decode(bool enabled);
// Decode CMPR textures on the GPU (bit-identical to the CPU decoder). Takes effect for new textures.
void aurora_set_gpu_cmpr_decode(bool enabled);
// True while the GX producer records a frame requested by aurora_request_draw_dump.
bool aurora_draw_dump_active(void);
bool aurora_flush_efb_copies_to_ram();
bool aurora_flush_efb_copy_to_ram(void* dest);

void aurora_set_log_level(AuroraLogLevel level);
// Forced texture anisotropy (0 = as the game requests). Takes effect on the
// next sampler lookup; samplers are cached by descriptor, so no flush is needed.
void aurora_set_texture_anisotropy(uint16_t value);
// Texture replacement variant: files in <texture_replacements>/variants/<name>/ take precedence while
// <name> is active ("" or NULL: none). Applied before the next texture is resolved; only the
// textures either variant replaces are re-resolved. Calling it again with the same name re-reads
// that folder.
void aurora_set_texture_replacement_variant(const char* name);
uint16_t aurora_get_texture_anisotropy(void);
uint32_t aurora_get_msaa_samples(void);
// Per-pixel GX lighting. Takes effect on the next pipeline build; existing
// pipelines of the other mode stay cached.
void aurora_set_per_pixel_lighting(bool enabled);
bool aurora_get_per_pixel_lighting(void);
// Enhanced metal on/off (new pipelines on the next build) and strength (live, 0..2).
void aurora_set_enhanced_metal(bool enabled, float strength);
// Enhanced metal material parameters, live, each 0..1: roughness, metallic, edge sheen, surface detail.
void aurora_set_enhanced_metal_params(float roughness, float metallic, float edge, float detail);
// Texture upscaling. Applies to textures uploaded from now on; already
// resident ones keep their current image until the game reloads them.
void aurora_set_texture_upscale(uint32_t factor, uint32_t maxSourceSize, float sharpen);
typedef struct {
  uint64_t upscaled;
  uint64_t cacheHits;
  uint64_t skipped;
  uint64_t pending;
  uint64_t cacheBytes;
  uint64_t cpuMicroseconds;
} AuroraTextureUpscaleStats;
AuroraTextureUpscaleStats aurora_get_texture_upscale_stats(void);
void aurora_set_pause_on_focus_lost(bool value);
void aurora_set_background_input(bool value);
void aurora_set_display_mode(AuroraDisplayMode mode);
// Refresh rate of the monitor the window is on, in Hz; 0 when unknown.
float aurora_get_display_refresh_rate();
AuroraDisplayMode aurora_get_display_mode();

typedef enum {
  AURORA_METALFX_DISABLED,
  AURORA_METALFX_UNSUPPORTED,
  AURORA_METALFX_NOT_UPSCALING,
  AURORA_METALFX_ACTIVE,
  AURORA_METALFX_ERROR,
} AuroraMetalFXStatus;

// Changes are consumed at the next sealed frame boundary. MetalFX only applies
// when both source dimensions are smaller than the aspect-fitted output.
void aurora_set_metalfx_spatial(bool enabled);
bool aurora_get_metalfx_spatial();
bool aurora_is_metalfx_spatial_supported();
AuroraMetalFXStatus aurora_get_metalfx_status();

AuroraBackend aurora_get_backend();
const AuroraBackend* aurora_get_available_backends(size_t* count);

#ifdef __cplusplus
}
#endif

#endif
