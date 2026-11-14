#include "gx.hpp"
#include "__gx.h"

#include "../../gfx/tex_copy_conv.hpp"
#include "../../gfx/post_process.hpp"
#include "../../gfx/efb_ram_copy.hpp"
#include "../../gfx/texture.hpp"
#include "../../gx/fifo.hpp"
#include "../../gx/render_policy.hpp"
#include "../../internal.hpp"
#include "../../window.hpp"
#include "../../gfx/clear.hpp"
#include "../../webgpu/gpu.hpp"
#include "../vi/vi_internal.hpp"

#include <absl/container/flat_hash_set.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace {
struct CopyClearState {
  bool clearColor = false;
  bool clearAlpha = false;
  bool clearDepth = false;
  aurora::Vec4<float> clearColorValue{0.f, 0.f, 0.f, 1.f};
};

CopyClearState get_copy_clear_state(GXBool clear) {
  if (!clear) {
    return {};
  }

  CopyClearState state{
      .clearColor = g_gxState.colorUpdate,
      .clearAlpha = g_gxState.alphaUpdate,
      .clearDepth = g_gxState.depthUpdate,
      .clearColorValue = g_gxState.clearColor,
  };

  if (!aurora::gx::render_target_has_alpha(g_gxState.pixelFmt)) {
    // RGB and depth EFB formats do not have an alpha channel to clear.
    state.clearAlpha = false;
  }

  return state;
}

struct CopySourceRect {
  aurora::gfx::ClipRect clearRect;
  aurora::Vec4<float> sampleRect;
};

CopySourceRect map_texture_copy_source(const aurora::gfx::ClipRect& source, bool renderSpace) {
  if (renderSpace || g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return {
        .clearRect = source,
        .sampleRect = {static_cast<float>(source.x), static_cast<float>(source.y), static_cast<float>(source.width),
                       static_cast<float>(source.height)},
    };
  }

  const auto [logicalWidth, logicalHeight] = aurora::gx::logical_fb_size();
  const auto [targetWidth, targetHeight] = aurora::gfx::get_render_target_size();
  if (logicalWidth == 0 || logicalHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return {
        .clearRect = source,
        .sampleRect = {static_cast<float>(source.x), static_cast<float>(source.y), static_cast<float>(source.width),
                       static_cast<float>(source.height)},
    };
  }

  struct MappedEdge {
    float sample;
    int32_t nearest;
  };
  const auto mapEdge = [](int32_t edge, uint32_t logicalSize, uint32_t targetSize) {
    // Round scaled copy edges consistently to avoid seams and off-by-one pixels.
    const int64_t numerator = static_cast<int64_t>(edge) * static_cast<int64_t>(targetSize);
    const int64_t denominator = static_cast<int64_t>(logicalSize);
    const int64_t quotient = numerator / denominator;
    const int64_t remainder = numerator % denominator;
    const int64_t remainderMagnitude = remainder < 0 ? -remainder : remainder;
    const int64_t roundingThreshold = (denominator + 1) / 2;
    const int64_t nearestValue =
        quotient + (remainderMagnitude >= roundingThreshold ? (numerator < 0 ? -1 : 1) : 0);
    return MappedEdge{
        .sample = static_cast<float>(static_cast<double>(numerator) / static_cast<double>(denominator)),
        .nearest = static_cast<int32_t>(nearestValue),
    };
  };
  const auto left = mapEdge(source.x, logicalWidth, targetWidth);
  const auto top = mapEdge(source.y, logicalHeight, targetHeight);
  const auto right = mapEdge(source.x + source.width, logicalWidth, targetWidth);
  const auto bottom = mapEdge(source.y + source.height, logicalHeight, targetHeight);
  return {
      .clearRect =
          {
              .x = left.nearest,
              .y = top.nearest,
              .width = std::max<int32_t>(right.nearest - left.nearest, 1),
              .height = std::max<int32_t>(bottom.nearest - top.nearest, 1),
          },
      .sampleRect = {left.sample, top.sample, right.sample - left.sample, bottom.sample - top.sample},
  };
}





u32 pack_copy_filter_samples(u8 reg, const std::array<std::array<u8, 2>, 12>& samplePattern, size_t first) {
  u32 value = static_cast<u32>(reg) << 24;
  for (size_t i = 0; i < 6; ++i) {
    const size_t sample = first + i;
    const u8 component = samplePattern[sample / 2][sample % 2] & 0x0fu;
    value |= static_cast<u32>(component) << (i * 4);
  }
  return value;
}

u32 pack_copy_filter0(const std::array<u8, 7>& vfilter) {
  return 0x53000000u | (static_cast<u32>(vfilter[0] & 0x3fu) << 0) |
         (static_cast<u32>(vfilter[1] & 0x3fu) << 6) | (static_cast<u32>(vfilter[2] & 0x3fu) << 12) |
         (static_cast<u32>(vfilter[3] & 0x3fu) << 18);
}

u32 pack_copy_filter1(const std::array<u8, 7>& vfilter) {
  return 0x54000000u | (static_cast<u32>(vfilter[4] & 0x3fu) << 0) |
         (static_cast<u32>(vfilter[5] & 0x3fu) << 6) | (static_cast<u32>(vfilter[6] & 0x3fu) << 12);
}

std::array<u32, 3> combined_copy_filter_coefficients(const std::array<u8, 7>& vfilter) {
  if (!g_gxState.copyFilterVf) {
    return {0, 64, 0};
  }

  return {
      static_cast<u32>(vfilter[0]) + static_cast<u32>(vfilter[1]),
      static_cast<u32>(vfilter[2]) + static_cast<u32>(vfilter[3]) + static_cast<u32>(vfilter[4]),
      static_cast<u32>(vfilter[5]) + static_cast<u32>(vfilter[6]),
  };
}

aurora::Vec2<uint32_t> scale_copy_dst(u32 logicalWidth, u32 logicalHeight) {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return {logicalWidth, logicalHeight};
  }

  const auto [logicalFbWidth, logicalFbHeight] = aurora::gx::logical_fb_size();
  const auto [targetWidth, targetHeight] = aurora::gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return {logicalWidth, logicalHeight};
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  const auto scaledWidth = std::max<u32>(static_cast<u32>(std::lround(static_cast<float>(logicalWidth) * scaleX)), 1);
  const auto scaledHeight = std::max<u32>(static_cast<u32>(std::lround(static_cast<float>(logicalHeight) * scaleY)), 1);
  return {scaledWidth, scaledHeight};
}

aurora::gfx::TextureHandle create_copy_texture(u32 width, u32 height, GXTexFmt texCopyFmt) {
  if (aurora::gfx::tex_copy_conv::needs_conversion(texCopyFmt)) {
    return aurora::gfx::new_conv_texture(width, height, texCopyFmt, "Copy Conv Texture");
  }

  const auto fmt = texCopyFmt == GX_TF_RGB565 ? GX_TF_RGB565 : GX_TF_RGBA8;
  return aurora::gfx::new_render_texture(width, height, fmt, "Resolved Texture");
}

  // Reuse retired copy targets to avoid unbounded GPU texture allocation.
struct CopyTexturePoolEntry {
  aurora::gx::GXState::CopyTextureKey key;
  u32 scaledWidth = 0;
  u32 scaledHeight = 0;
  aurora::gfx::TextureHandle handle;
  u32 addedFrame = 0;
};
std::vector<CopyTexturePoolEntry> g_copyTexturePool;
  // Keep only a few reusable copy targets per destination.
constexpr size_t kCopyTexturePoolPerKey = 3;
  // Spares of other copy sizes kept per destination (covers a copy alternating between a few sizes).
constexpr size_t kCopyTexturePoolOtherShapesPerDest = 2;

// A destination whose copy size changes every frame (a zoom effect) left a spare for every old size until
// the 5 s budget sweep: 768 MiB in a few seconds at high internal resolution. Drop all but the newest
// spares of other sizes; targets still referenced elsewhere are never spares.
void prune_other_shape_spares(const aurora::gx::GXState::CopyTextureKey& key) {
  std::vector<aurora::gx::policy::PoolSpare> pool;
  pool.reserve(g_copyTexturePool.size());
  for (const auto& entry : g_copyTexturePool) {
    pool.push_back({entry.key.dest, entry.key == key, entry.handle.use_count() <= 1});
  }
  // Highest index first, so earlier indices stay valid while erasing.
  for (const size_t index : aurora::gx::policy::plan_other_shape_spare_drops(pool, key.dest,
                                                                             kCopyTexturePoolOtherShapesPerDest)) {
    g_copyTexturePool.erase(g_copyTexturePool.begin() + static_cast<std::ptrdiff_t>(index));
  }
}

aurora::gfx::TextureHandle acquire_copy_texture(const aurora::gx::GXState::CopyTextureKey& key, u32 width, u32 height,
                                                GXTexFmt texCopyFmt) {
  size_t sameKey = 0;
  for (auto it = g_copyTexturePool.begin(); it != g_copyTexturePool.end();) {
    if (!(it->key == key)) {
      ++it;
      continue;
    }
    // Discard retired targets when their internal resolution changes.
    if ((it->scaledWidth != width || it->scaledHeight != height) && it->handle.use_count() <= 1) {
      it = g_copyTexturePool.erase(it);
      continue;
    }
    ++sameKey;
    if (it->scaledWidth == width && it->scaledHeight == height && it->handle.use_count() == 1) {
      return it->handle;
    }
    ++it;
  }

  prune_other_shape_spares(key);
  auto handle = create_copy_texture(width, height, texCopyFmt);
  if (sameKey < kCopyTexturePoolPerKey) {
    g_copyTexturePool.push_back({key, width, height, handle, aurora::gfx::current_frame()});
  }
  return handle;
}

u16 get_num_xfb_lines_internal(u16 efbHeight, u32 iScale) {
  CHECK(efbHeight != 0, "GXGetNumXfbLines requires non-zero EFB height");
  CHECK(iScale != 0, "invalid XFB line scale");

  const u32 count = static_cast<u32>(efbHeight - 1u) * 0x100u;
  u32 realHeight = (count / iScale) + 1u;

  u32 reducedScale = iScale;
  if (reducedScale > 0x80u && reducedScale < 0x100u) {
    while ((reducedScale & 1u) == 0u) {
      reducedScale >>= 1;
    }
    if (reducedScale != 0u && (efbHeight % reducedScale) == 0u) {
      ++realHeight;
    }
  }

  return static_cast<u16>(std::min<u32>(realHeight, 0x400u));
}

u32 y_scale_to_integer(f32 yScale) {
  CHECK(yScale > 0.f, "GX display copy y-scale must be positive");
  return static_cast<u32>(256.f / yScale) & 0x1ffu;
}

} // namespace

namespace aurora::gx {
void prune_copy_texture_pool(const void* dest) noexcept {
  if (dest == nullptr) {
    g_copyTexturePool.clear();
    return;
  }
  std::erase_if(g_copyTexturePool, [dest](const CopyTexturePoolEntry& entry) { return entry.key.dest == dest; });
}

u64 copy_texture_pool_bytes(absl::flat_hash_set<const void*>& seen) noexcept {
  u64 bytes = 0;
  for (const auto& entry : g_copyTexturePool) {
    if (entry.handle && seen.insert(entry.handle.get()).second) {
      bytes += static_cast<u64>(entry.handle->size.width) * entry.handle->size.height * 4u;
    }
  }
  return bytes;
}

size_t prune_copy_texture_pool_spares(size_t keep, u32 idleFrames) noexcept {
  // A spare is a retired target only the pool still references.
  const u32 frame = aurora::gfx::current_frame();
  std::vector<size_t> spares;
  for (size_t i = 0; i < g_copyTexturePool.size(); ++i) {
    const auto& entry = g_copyTexturePool[i];
    if (entry.handle.use_count() <= 1 && frame - entry.addedFrame > idleFrames) spares.push_back(i);
  }
  if (spares.size() <= keep) return 0;
  std::sort(spares.begin(), spares.end(), [](size_t a, size_t b) {
    return g_copyTexturePool[a].addedFrame < g_copyTexturePool[b].addedFrame;
  });
  spares.resize(spares.size() - keep);  // oldest first; the newest `keep` survive
  std::sort(spares.rbegin(), spares.rend());
  for (size_t index : spares) g_copyTexturePool.erase(g_copyTexturePool.begin() + static_cast<std::ptrdiff_t>(index));
  return spares.size();
}
} // namespace aurora::gx

extern "C" {
GXRenderModeObj GXNtsc480IntDf = {
    VI_TVMODE_NTSC_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXNtsc480Int = {
    VI_TVMODE_NTSC_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {0, 0, 21, 22, 21, 0, 0},
};
GXRenderModeObj GXPal528IntDf = {
    VI_TVMODE_PAL_INT,
    704,
    528,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXMpal480IntDf = {
    VI_TVMODE_PAL_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};

void GXAdjustForOverscan(GXRenderModeObj* rmin, GXRenderModeObj* rmout, u16 hor, u16 ver) {
  *rmout = *rmin;
  const auto renderSize = aurora::gfx::get_render_target_size();
  rmout->fbWidth = static_cast<u16>(std::min<uint32_t>(renderSize.x, UINT16_MAX));
  rmout->efbHeight = static_cast<u16>(std::min<uint32_t>(renderSize.y, UINT16_MAX));
  rmout->xfbHeight = static_cast<u16>(std::min<uint32_t>(renderSize.y, UINT16_MAX));
}

void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
  g_gxState.dispCopySrc = {left, top, wd, ht};
  GX_WRITE_RAS_REG(0x49000000u | ((static_cast<u32>(top) & 0x3ffu) << 10) | (static_cast<u32>(left) & 0x3ffu));
  GX_WRITE_RAS_REG(0x4a000000u | (((static_cast<u32>(ht) - 1u) * 0x400u) & 0x000ffc00u) |
                   ((static_cast<u32>(wd) - 1u) & 0x3ffu));
}

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
  g_gxState.texCopySrc = {left, top, wd, ht};
  g_gxState.texCopySrcRenderSpace = false;
}

void GXSetDispCopyDst(u16 wd, u16 ht) {
  g_gxState.dispCopyDstWidth = wd;
  g_gxState.dispCopyDstHeight = ht;
  GX_WRITE_RAS_REG(0x4d000000u | ((((static_cast<u32>(wd) & 0x7fffu) << 1) >> 5) & 0x3ffu));
}

void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {
  g_gxState.texCopyFmt = fmt;
  g_gxState.texCopyDstWidth = wd;
  g_gxState.texCopyDstHeight = ht;
  g_gxState.texCopyHalfScale = mipmap != GX_FALSE;
}

void GXSetDispCopyFrame2Field(u32 mode) {
  g_gxState.dispCopyFrame2Field = mode & 3;
}

void GXSetCopyClamp(GXFBClamp clamp) {
  g_gxState.copyClamp = static_cast<GXFBClamp>(static_cast<u32>(clamp) & 3);
}

u32 GXSetDispCopyYScale(f32 vscale) {
  const u32 iScale = y_scale_to_integer(vscale);
  g_gxState.dispCopyYScale = vscale;
  GX_WRITE_RAS_REG(0x4e000000u | iScale);
  __gx->bpSent = 0;
  return get_num_xfb_lines_internal(static_cast<u16>(g_gxState.dispCopySrc.height), iScale);
}

void GXSetCopyClear(GXColor color, u32 depth) {
  // BP 0x4F: clear color R + A
  u32 reg0 = 0;
  SET_REG_FIELD(0, reg0, 8, 0, color.r);
  SET_REG_FIELD(0, reg0, 8, 8, color.a);
  SET_REG_FIELD(0, reg0, 8, 24, 0x4F);
  GX_WRITE_RAS_REG(reg0);

  // BP 0x50: clear color B + G
  u32 reg1 = 0;
  SET_REG_FIELD(0, reg1, 8, 0, color.b);
  SET_REG_FIELD(0, reg1, 8, 8, color.g);
  SET_REG_FIELD(0, reg1, 8, 24, 0x50);
  GX_WRITE_RAS_REG(reg1);

  // BP 0x51: clear Z (24-bit)
  u32 reg2 = 0;
  SET_REG_FIELD(0, reg2, 24, 0, depth);
  SET_REG_FIELD(0, reg2, 8, 24, 0x51);
  GX_WRITE_RAS_REG(reg2);
  __gx->bpSent = 1;
}

void GXSetCopyFilter(GXBool aa, u8 sample_pattern[12][2], GXBool vf, u8 vfilter[7]) {
  g_gxState.copyFilterAa = aa;
  g_gxState.copyFilterVf = vf;
  if (sample_pattern) {
    for (size_t i = 0; i < g_gxState.copyFilterSamplePattern.size(); ++i) {
      g_gxState.copyFilterSamplePattern[i][0] = sample_pattern[i][0];
      g_gxState.copyFilterSamplePattern[i][1] = sample_pattern[i][1];
    }
  }
  if (vfilter) {
    for (size_t i = 0; i < g_gxState.copyFilterVFilter.size(); ++i) {
      g_gxState.copyFilterVFilter[i] = vfilter[i];
    }
  }

  if (!aa) {
    for (auto& sample : g_gxState.copyFilterSamplePattern) {
      sample = {6, 6};
    }
  }
  if (!vf) {
    g_gxState.copyFilterVFilter = {0, 0, 21, 22, 21, 0, 0};
  }

  GX_WRITE_RAS_REG(pack_copy_filter_samples(0x01, g_gxState.copyFilterSamplePattern, 0));
  GX_WRITE_RAS_REG(pack_copy_filter_samples(0x02, g_gxState.copyFilterSamplePattern, 6));
  GX_WRITE_RAS_REG(pack_copy_filter_samples(0x03, g_gxState.copyFilterSamplePattern, 12));
  GX_WRITE_RAS_REG(pack_copy_filter_samples(0x04, g_gxState.copyFilterSamplePattern, 18));
  GX_WRITE_RAS_REG(pack_copy_filter0(g_gxState.copyFilterVFilter));
  GX_WRITE_RAS_REG(pack_copy_filter1(g_gxState.copyFilterVFilter));
  __gx->bpSent = 0;
}

void GXSetDispCopyGamma(GXGamma gamma) {
  g_gxState.dispCopyGamma = static_cast<GXGamma>(static_cast<u32>(gamma) & 3u);
  g_gxState.bpRegCache[0x52] = (g_gxState.bpRegCache[0x52] & ~(3u << 7)) |
                               ((static_cast<u32>(g_gxState.dispCopyGamma) & 3u) << 7);
}

// `visibleLines` (0 = all) keeps only the top source lines in the presented
// copy, as VI scans fewer lines than a taller XFB holds. The EFB clear still
// covers the whole source rectangle the game requested.
static void copy_disp(GXBool clear, u32 visibleLines) {
  // Finish queued commands before this copy reads live EFB state. On the
  // asynchronous decoder thread the stream is already in order.
  if (!aurora::gx::fifo::on_decoder_thread() && aurora::gx::fifo::get_buffer_size() != 0) {
    aurora::gx::fifo::drain();
  }
  const auto rect = aurora::gx::map_logical_scissor(g_gxState.dispCopySrc);
  const u32 srcHeight = std::max<u32>(g_gxState.dispCopySrc.height, 1);
  const u32 shownLines = visibleLines != 0 ? std::min(visibleLines, srcHeight) : srcHeight;
  const auto logicalDstWidth =
      std::max<u32>(g_gxState.dispCopyDstWidth != 0 ? g_gxState.dispCopyDstWidth : static_cast<u32>(g_gxState.dispCopySrc.width), 1);
  const auto logicalDstHeight = std::max<u32>(
      (g_gxState.dispCopyDstHeight != 0 ? g_gxState.dispCopyDstHeight : srcHeight) * shownLines / srcHeight, 1);
  const aurora::Vec4<float> sourceRect{static_cast<float>(rect.x), static_cast<float>(rect.y),
                                       static_cast<float>(rect.width),
                                       static_cast<float>(rect.height) * shownLines / srcHeight};
  const auto [dstWidth, dstHeight] = scale_copy_dst(logicalDstWidth, logicalDstHeight);
  // The presented picture is this EFB rectangle (render pixels); SSAO maps the image onto it.
  aurora::gfx::post::note_display_source(sourceRect.x(), sourceRect.y(), sourceRect.z(), sourceRect.w());

  if (!g_gxState.displayCopyTexture || g_gxState.displayCopyWidth != dstWidth ||
      g_gxState.displayCopyHeight != dstHeight) {
    g_gxState.displayCopyTexture = aurora::gfx::new_render_texture(dstWidth, dstHeight, GX_TF_RGBA8, "Display Copy");
    g_gxState.displayCopyWidth = dstWidth;
    g_gxState.displayCopyHeight = dstHeight;
  }

  const auto clearState = get_copy_clear_state(clear);
  auto copyFilter = combined_copy_filter_coefficients(g_gxState.copyFilterVFilter);
  if (aurora::g_config.disableCopyFilter) {
    copyFilter = {0, copyFilter[0] + copyFilter[1] + copyFilter[2], 0};
  }
  aurora::gfx::resolve_pass(g_gxState.displayCopyTexture, rect, clearState.clearColor, clearState.clearAlpha,
                            clearState.clearDepth, clearState.clearColorValue, aurora::gx::clear_depth_value(),
                            GX_TF_RGBA8, shownLines != srcHeight ? &sourceRect : nullptr, false, &copyFilter,
                            false, static_cast<float>(rect.height) / static_cast<float>(srcHeight),
                            (g_gxState.copyClamp & GX_CLAMP_TOP) != 0,
                            (g_gxState.copyClamp & GX_CLAMP_BOTTOM) != 0);
  aurora::gx::set_display_copy_present_source();
  aurora::gfx::note_display_copy();
}

void GXCopyDisp(void* dest, GXBool clear) {
  (void)dest;
  copy_disp(clear, 0);
}

// The raw Prime path lets the guest VI driver select between distinct XFBs.
// Retain one display-copy target per destination instead of showing the last
// copy regardless of the framebuffer selected by the original game.
struct TrackedDisplayCopy {
  aurora::gfx::TextureHandle texture;
  u32 width = 0, height = 0;
};
static std::unordered_map<const void*, TrackedDisplayCopy> s_trackedDisplayCopies;

void GXCopyDispTracked(void* dest, GXBool clear, u32 visibleLines) {
  auto& target = s_trackedDisplayCopies[dest];
  g_gxState.displayCopyTexture = target.texture;
  g_gxState.displayCopyWidth = target.width;
  g_gxState.displayCopyHeight = target.height;
  copy_disp(clear, visibleLines);
  target = {g_gxState.displayCopyTexture, g_gxState.displayCopyWidth, g_gxState.displayCopyHeight};
}

bool GXSelectTrackedDisplayCopy(const void* dest) {
  std::lock_guard gpuLock(aurora::renderer_gpu_mutex());
  const auto it = s_trackedDisplayCopies.find(dest);
  if (it == s_trackedDisplayCopies.end() || !it->second.texture) return false;
  g_gxState.displayCopyTexture = it->second.texture;
  g_gxState.displayCopyWidth = it->second.width;
  g_gxState.displayCopyHeight = it->second.height;
  aurora::gx::set_display_copy_present_source();
  return true;
}

void GXCopyTex(void* dest, GXBool clear) {
  // Texture copies must see all earlier draws and state changes.
  if (!aurora::gx::fifo::on_decoder_thread() && aurora::gx::fifo::get_buffer_size() != 0) {
    aurora::gx::fifo::drain();
  }
  const auto sourceRect = map_texture_copy_source(g_gxState.texCopySrc, g_gxState.texCopySrcRenderSpace);
  auto rect = sourceRect.clearRect;
  if (clear) {
    // A clearing copy clears every render pixel the scissor of the same rectangle lets draws reach
    // (scale_logical_scissor_rect rounds outward). Rounded to the nearest pixel, the clear missed a row
    // that RM3E01's radar (a 64x64 viewport at 0,384, copied and cleared before the world) had drawn
    // into, colour and depth: the world could not cover it and a thin white line stayed at the left
    // edge of the screen (F7 14/11/2026 01:36, rows 742 and 743 at 1815x866).
    const float l = sourceRect.sampleRect.x(), t = sourceRect.sampleRect.y();
    const float r = l + sourceRect.sampleRect.z(), b = t + sourceRect.sampleRect.w();
    const int32_t left = static_cast<int32_t>(std::floor(l)), top = static_cast<int32_t>(std::floor(t));
    rect = {.x = left, .y = top, .width = static_cast<int32_t>(std::ceil(r)) - left,
            .height = static_cast<int32_t>(std::ceil(b)) - top};
  }
  // Keep guest dimensions for cache identity while preserving scaled GPU detail.
  const auto logicalDstWidth = std::max<u32>(g_gxState.texCopyDstWidth, 1);
  const auto logicalDstHeight = std::max<u32>(g_gxState.texCopyDstHeight, 1);
  const auto [scaledDstWidth, scaledDstHeight] = scale_copy_dst(logicalDstWidth, logicalDstHeight);
  const auto texCopyFmt = g_gxState.texCopyFmt;
  const bool sourceHasAlpha = aurora::gx::render_target_has_alpha(g_gxState.pixelFmt);
  const bool forceOpaqueAlpha = !sourceHasAlpha && !aurora::gx::is_depth_format(texCopyFmt);
  const auto resolveFmt = texCopyFmt;

  const aurora::gx::GXState::CopyTextureKey key{
      .dest = dest,
      .width = logicalDstWidth,
      .height = logicalDstHeight,
      .format = texCopyFmt,
  };
  // Keep one live copy per destination and retire stale GPU textures.
  for (auto cacheIt = g_gxState.copyTextureCache.begin(); cacheIt != g_gxState.copyTextureCache.end();) {
    if (cacheIt->first.dest == dest && !(cacheIt->first == key)) {
      g_gxState.copyTextureCache.erase(cacheIt++);
    } else {
      ++cacheIt;
    }
  }
  auto it = g_gxState.copyTextureCache.find(key);
  if (it == g_gxState.copyTextureCache.end()) {
    auto handle = acquire_copy_texture(key, scaledDstWidth, scaledDstHeight, texCopyFmt);
    it = g_gxState.copyTextureCache.emplace(key, aurora::gx::GXState::CopyTextureRef{.handle = handle, .revision = 0}).first;
  }
  auto& handle = it->second;
  const u32 currentFrame = aurora::gfx::current_frame();
  const bool sampledThisFrame = handle.sampledThisFrame && handle.lastSampledFrame == currentFrame;
  const bool scaledSizeChanged = !handle.handle || handle.handle->size.width != scaledDstWidth ||
                                 handle.handle->size.height != scaledDstHeight;
  auto clearState = get_copy_clear_state(clear);
  if (sampledThisFrame || scaledSizeChanged) {
    const u32 revision = handle.revision;
    const u32 lastProducedFrame = handle.lastProducedFrame;
    handle = aurora::gx::GXState::CopyTextureRef{
        .handle = acquire_copy_texture(key, scaledDstWidth, scaledDstHeight, texCopyFmt),
        .revision = revision,
        .lastProducedFrame = lastProducedFrame,
    };
  }

  const bool alphaUpdate = g_gxState.alphaUpdate && aurora::gx::render_target_has_alpha(g_gxState.pixelFmt);
  // Destination alpha only replaces the alpha of pixels being drawn; a copy
  // reads the EFB as it is. Prime's interaction highlight copies the button
  // mask while dst alpha is still enabled, so no alpha may be written here.
  if (aurora::gx::render_target_has_alpha(g_gxState.pixelFmt)) {
    clearState.clearAlpha = clear && alphaUpdate;
  }
  const auto copyFilter = combined_copy_filter_coefficients(g_gxState.copyFilterVFilter);
  // Skip only recurring color copies so one-shot copies are never lost.
  const bool producedConsecutively = handle.revision != 0 && currentFrame - handle.lastProducedFrame <= 1;
  const bool persistentCopy = !aurora::gx::is_depth_format(texCopyFmt) && !producedConsecutively;
  aurora::gfx::resolve_pass(handle.handle, rect, clearState.clearColor, clearState.clearAlpha, clearState.clearDepth,
                            clearState.clearColorValue, aurora::gx::clear_depth_value(), resolveFmt,
                            &sourceRect.sampleRect, g_gxState.texCopyHalfScale, &copyFilter, forceOpaqueAlpha,
                            sourceRect.sampleRect.w() / std::max<float>(g_gxState.texCopySrc.height, 1.0f),
                            (g_gxState.copyClamp & GX_CLAMP_TOP) != 0,
                            (g_gxState.copyClamp & GX_CLAMP_BOTTOM) != 0, persistentCopy);
  // The game's glow source: a half-size copy of (nearly) the whole EFB. Its blur offsets are sized
  // for the Wii's texels, so the scaled copy is softened to that footprint (post::soften_copy).
  if (g_gxState.texCopyHalfScale && logicalDstWidth > 0 && logicalDstHeight > 0) {
    // Compared in render pixels (the mapped copy rectangle against the render target), whatever space
    // the game's source rectangle was given in. The EFB is taller than what Prime draws (1920 x 916 of
    // 1920 x 1080 here), so the height only has to cover most of it; the width is the full target.
    const auto [targetWidth, targetHeight] = aurora::gfx::get_render_target_size();
    const auto copyWidth = static_cast<uint64_t>(std::max<int32_t>(rect.width, 0));
    const auto copyHeight = static_cast<uint64_t>(std::max<int32_t>(rect.height, 0));
    const bool glowSource = targetWidth > 0 && targetHeight > 0 && copyWidth * 10u >= uint64_t{targetWidth} * 9u &&
                            copyHeight * 10u >= uint64_t{targetHeight} * 6u;
#if defined(MKW_PRIME_QA_TOOLS)
    static uint32_t logged = 0;
    if (logged < 6) {
      ++logged;
      std::fprintf(stderr, "[glow-soften] half-size copy %dx%d of target %ux%u, dst %ux%u (game %ux%u): %s\n",
                   rect.width, rect.height, targetWidth, targetHeight, scaledDstWidth, scaledDstHeight,
                   logicalDstWidth, logicalDstHeight, glowSource ? "glow source" : "not the whole picture");
    }
#endif
    if (glowSource) {
      aurora::gfx::note_resolve_soften(static_cast<float>(scaledDstWidth) / static_cast<float>(logicalDstWidth),
                                       static_cast<float>(scaledDstHeight) / static_cast<float>(logicalDstHeight));
    }
  }
  ++handle.revision;
  handle.lastProducedFrame = currentFrame;
  handle.width = logicalDstWidth;
  handle.height = logicalDstHeight;
  handle.format = texCopyFmt;
  handle.dataSize = GXGetTexBufferSize(static_cast<u16>(logicalDstWidth), static_cast<u16>(logicalDstHeight), texCopyFmt, GX_FALSE, 0);
  aurora::gx::notify_copy_texture_created();
  g_gxState.copyTextures[dest] = handle;
  // Keep the GPU copy and download it only if guest code reads the destination.
  aurora::gfx::efb_ram::schedule(dest, logicalDstWidth, logicalDstHeight, texCopyFmt, handle.handle);
}

// Decoder-thread entry points for host callbacks (aurora_stream_host_callback):
// the copy parameters travel with the command instead of being read from
// decoder state the producer thread set earlier.
void GXCopyTexWithParams(void* dest, GXBool clear, u16 left, u16 top, u16 width, u16 height, u16 dstWidth,
                         u16 dstHeight, u32 format, GXBool halfScale) {
  g_gxState.texCopySrc = {left, top, width, height};
  g_gxState.texCopySrcRenderSpace = false;
  g_gxState.texCopyFmt = static_cast<GXTexFmt>(format);
  g_gxState.texCopyDstWidth = dstWidth;
  g_gxState.texCopyDstHeight = dstHeight;
  g_gxState.texCopyHalfScale = halfScale != GX_FALSE;
  GXCopyTex(dest, clear);
}

void GXCopyDispTrackedWithParams(void* dest, GXBool clear, u32 visibleLines, u16 left, u16 top, u16 width,
                                 u16 height, u16 dstWidth, u16 dstHeight) {
  g_gxState.dispCopySrc = {left, top, width, height};
  g_gxState.dispCopyDstWidth = dstWidth;
  g_gxState.dispCopyDstHeight = dstHeight;
  GXCopyDispTracked(dest, clear, visibleLines);
}

void GXClearBoundingBox() {
  g_gxState.boundingBox = {1023, 0, 1023, 0};
  GX_WRITE_RAS_REG(0x550003FFu);
  GX_WRITE_RAS_REG(0x560003FFu);
  __gx->bpSent = 0;
}

void GXReadBoundingBox(u16* left, u16* right, u16* top, u16* bottom) {
  if (left) {
    *left = g_gxState.boundingBox[0];
  }
  if (right) {
    *right = g_gxState.boundingBox[1];
  }
  if (top) {
    *top = g_gxState.boundingBox[2];
  }
  if (bottom) {
    *bottom = g_gxState.boundingBox[3];
  }
}

u16 GXGetNumXfbLines(u16 efbHeight, f32 yScale) {
  return get_num_xfb_lines_internal(efbHeight, y_scale_to_integer(yScale));
}

f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) {
  CHECK(efbHeight != 0, "GXGetYScaleFactor requires non-zero EFB height");
  CHECK(xfbHeight != 0 && xfbHeight <= 1024, "GXGetYScaleFactor requires 1..1024 XFB lines");

  u32 targetHeight = xfbHeight;
  f32 yScale = static_cast<f32>(targetHeight) / static_cast<f32>(efbHeight);
  u16 realHeight = GXGetNumXfbLines(efbHeight, yScale);

  while (realHeight > xfbHeight && targetHeight > 1) {
    --targetHeight;
    yScale = static_cast<f32>(targetHeight) / static_cast<f32>(efbHeight);
    realHeight = GXGetNumXfbLines(efbHeight, yScale);
  }

  f32 resultScale = yScale;
  while (realHeight < xfbHeight && targetHeight < 1024) {
    resultScale = yScale;
    ++targetHeight;
    yScale = static_cast<f32>(targetHeight) / static_cast<f32>(efbHeight);
    realHeight = GXGetNumXfbLines(efbHeight, yScale);
  }

  return resultScale;
}

}
