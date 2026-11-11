#pragma once

#include "texture.hpp"
#include "texture_convert.hpp"

// Load-time texture upscaling (Super-xBR, CPU) for static game textures.
//
// The original texture is uploaded first, exactly as without the option, and a
// worker thread decodes the source to RGBA8 and enlarges every mip level by the
// configured factor with an edge-directed interpolation. When the result is
// ready, apply_completed() (GX thread) swaps the GPU texture inside the same
// TextureRef, so every cache that holds the handle sees the upscaled image
// without re-resolving. The GX texture object (filters, wrap modes, LOD range)
// is untouched: with the same level count the GPU picks one level further down
// the chain at distance (identical texel density to the original) and only
// magnified texels change. Results are cached per source content so a texture
// reloaded in another room is served upscaled at once.
//
// Not applied to: depth formats, textures whose mip levels carry different
// content (hasArbitraryMips), video frames (YUVA), EFB copies and anything
// above the configured source size.
namespace aurora::gfx::texture_upscale {
struct Config {
  uint32_t factor = 1;          // 1 = off, 2 or 4
  uint32_t maxSourceSize = 256; // largest source width/height that is upscaled
  float sharpen = 0.0f;         // 0 = none .. 1 = strong unsharp mask after the upscale
};

void set_config(uint32_t factor, uint32_t maxSourceSize, float sharpen) noexcept;
Config config() noexcept;
bool enabled() noexcept;

// Already-upscaled texture for this exact source content, or null.
TextureHandle find_cached(const GXTexObj_& obj, ArrayRef<uint8_t> data) noexcept;
TextureHandle find_cached_rgba(const GXTexObj_& obj, const ConvertedTexture& rgba) noexcept;
// Queue the upscale of `handle` (just created from `obj`). `data` is the raw GX
// image (all mips) or, for palette textures, the converted RGBA8 image. Bytes
// are copied; the caller keeps using the handle as is.
void schedule(const TextureHandle& handle, const GXTexObj_& obj, ArrayRef<uint8_t> data, bool rgbaInput,
              const char* label) noexcept;
// GX thread: swap finished results into their TextureRefs. Cheap when idle.
void apply_completed() noexcept;

// Super-xBR 2x of one RGBA8 level. Exposed for tests.
void super_xbr_2x(const uint8_t* src, uint32_t width, uint32_t height, bool wrapX, bool wrapY,
                  uint8_t* dst) noexcept;

struct Stats {
  uint64_t upscaled = 0;
  uint64_t cacheHits = 0;
  uint64_t skipped = 0;
  uint64_t pending = 0;
  uint64_t cacheBytes = 0;
  uint64_t cpuMicroseconds = 0;
};
Stats stats() noexcept;
void shutdown() noexcept;
} // namespace aurora::gfx::texture_upscale
