#pragma once

#include <aurora/aurora.h>
#include <aurora/math.hpp>

#include "wgpu.hpp"

#include <array>
#include <cstdint>

struct SDL_Window;

namespace aurora::webgpu {
struct GraphicsConfig {
  wgpu::SurfaceConfiguration surfaceConfiguration;
  wgpu::TextureFormat depthFormat;
  uint32_t msaaSamples;
  uint16_t textureAnisotropy;
  uint16_t forceTextureAnisotropy; // 0 = per texture object
  uint32_t maxTextureDimension2D;
};
struct TextureWithSampler {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::Extent3D size;
  wgpu::TextureFormat format;
  wgpu::Sampler sampler;
};
struct PresentSource {
  wgpu::BindGroup bindGroup;
  wgpu::Texture texture;
  wgpu::Extent3D size;
  wgpu::TextureFormat format;
};
struct Viewport {
  float left;
  float top;
  float width;
  float height;
  float znear;
  float zfar;

  bool operator==(const Viewport& rhs) const {
    return left == rhs.left && top == rhs.top && width == rhs.width && height == rhs.height && znear == rhs.znear &&
           zfar == rhs.zfar;
  }
  bool operator!=(const Viewport& rhs) const { return !(*this == rhs); }
};

extern wgpu::Device g_device;
extern wgpu::Queue g_queue;
extern wgpu::Surface g_surface;
extern wgpu::BackendType g_backendType;
// MSAA sample counts the active backend can build pipelines for. Dawn's Vulkan backend
// (VulkanSampleCount) maps only 1 and 4, the WebGPU core counts, and aborts on any other;
// D3D12 and Metal also accept 2 and 8.
inline bool sample_count_supported(uint32_t samples) noexcept {
  if (samples == 1 || samples == 4) return true;
  return (samples == 2 || samples == 8) && g_backendType != wgpu::BackendType::Vulkan;
}
extern GraphicsConfig g_graphicsConfig;
extern TextureWithSampler g_frameBuffer;
extern TextureWithSampler g_frameBufferResolved;
extern TextureWithSampler g_depthBuffer;
extern wgpu::RenderPipeline g_CopyPipeline;
extern wgpu::BindGroup g_CopyBindGroup;
extern wgpu::Instance g_instance;
extern bool g_bcTexturesSupported;

bool initialize(AuroraBackend backend);
void shutdown();
// Device-loss callbacks may run on a Dawn-managed thread and only publish loss state; frame
// threads call this at ordered boundaries to turn it into the one user-visible fatal error.
void fail_if_device_lost() noexcept;
void release_surface() noexcept;
bool refresh_surface(bool recreate = true);
// Asks for a present mode that never blocks (see g_nonBlockingPresent). True when the surface has to
// be reconfigured for it to apply.
bool set_non_blocking_present(bool enabled) noexcept;
// [video] vsync: present with Fifo. True when the surface has to be reconfigured for the change.
bool set_vsync(bool enabled) noexcept;
bool vsync() noexcept;
// The surface's current present mode, for the settings overlay.
const char* present_mode_name() noexcept;
void resize_swapchain(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height,
                      bool force = false);
TextureWithSampler create_render_texture(uint32_t width, uint32_t height, bool multisampled);
const TextureWithSampler& present_source() noexcept;
PresentSource current_present_source() noexcept;
void set_present_source_override(wgpu::BindGroup bindGroup, wgpu::Texture texture, wgpu::Extent3D size,
                                 wgpu::TextureFormat format) noexcept;
void clear_present_source_override() noexcept;
wgpu::BindGroup create_copy_bind_group(const TextureWithSampler& source);
wgpu::BindGroup create_copy_bind_group(wgpu::TextureView sourceView, wgpu::Sampler sampler);
Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width,
                                    uint32_t content_height) noexcept;
Viewport calculate_present_viewport_for_aspect(uint32_t surface_width, uint32_t surface_height,
                                               float content_aspect) noexcept;
void draw_clear(const wgpu::RenderPassEncoder& pass, bool clearColor, bool clearAlpha, bool clearDepth,
                const Vec4<float>& clearColorValue, float clearDepthValue);

size_t load_from_cache(void const* key, size_t keySize, void* value, size_t valueSize, void* userdata);
void store_to_cache(void const* key, size_t keySize, void const* value, size_t valueSize, void* userdata);
void cache_shutdown();

struct BlobCacheStats {
  uint64_t lookups;
  uint64_t hits;
  uint64_t stores;
  uint64_t hitBytes;
};
BlobCacheStats blob_cache_stats() noexcept;
// Call once the boot prewarm has finished. On the first launch of a new cache generation
// it deletes the blobs this launch never loaded or stored (they belong to the previous
// build) and schedules a compaction for shutdown. No-op otherwise.
void blob_cache_finish_boot_sweep() noexcept;
// Persists the monolithic Vulkan pipeline cache into the blob cache (no-op elsewhere).
void serialize_pipeline_caches() noexcept;

} // namespace aurora::webgpu
