#include "texture_upscale.hpp"

#include "../internal.hpp"
#include "../gx/gx.hpp"
#include "../webgpu/gpu.hpp"
#include "common.hpp"
#include "texture_convert.hpp"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <fmt/format.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <list>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

// Super-xBR: edge-directed 2x interpolation. Algorithm by Hyllian (2015/2016,
// MIT), see https://github.com/libretro/common-shaders (xbr/shaders/super-xbr).
// This file is an independent CPU implementation of the published method:
// pass 1 fills the centre of every 2x2 output block from its 4x4 source
// neighbourhood, choosing the diagonal along which the weighted colour
// distance is smaller; pass 2 fills the remaining pixels on the 45-degree
// rotated grid with the same rule; both clamp the result to the four nearest
// pixels (anti-ringing).

namespace aurora::gfx::texture_upscale {
namespace {
Module Log("aurora::gfx::texture_upscale");

std::atomic<uint32_t> s_factor{1};
std::atomic<uint32_t> s_maxSourceSize{256};
std::atomic<float> s_sharpen{0.0f};

std::atomic<uint64_t> s_statUpscaled{0};
std::atomic<uint64_t> s_statCacheHits{0};
std::atomic<uint64_t> s_statSkipped{0};
std::atomic<uint64_t> s_statCpuMicroseconds{0};
std::atomic<uint32_t> s_pendingJobs{0};
std::atomic<uint32_t> s_completedReady{0};

struct CacheKey {
  uint64_t contentHash = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mips = 0;
  uint32_t factor = 0;
  uint32_t format = 0;
  uint8_t wrapS = 0;
  uint8_t wrapT = 0;

  bool operator==(const CacheKey& rhs) const = default;

  template <typename H>
  friend H AbslHashValue(H h, const CacheKey& key) {
    return H::combine(std::move(h), key.contentHash, key.width, key.height, key.mips, key.factor, key.format,
                      key.wrapS, key.wrapT);
  }
};

struct CacheEntry {
  TextureHandle handle;
  uint64_t bytes = 0;
  std::list<CacheKey>::iterator lruIt;
};

// Handles are also held by the GX static texture caches; this cache only
// avoids recomputing a texture the game reloads (room changes, menus).
constexpr uint64_t kCacheBudgetBytes = 768ull * 1024 * 1024;
std::mutex s_cacheMutex;
absl::flat_hash_map<CacheKey, CacheEntry> s_cache;
absl::flat_hash_set<CacheKey> s_pendingKeys; // scheduled, not yet applied
std::list<CacheKey> s_lru;
uint64_t s_cacheBytes = 0;

// Textures the CPU (or a RAM-read EFB copy) rewrites keep the same address
// with new bytes every few frames; upscaling each version is wasted work.
// After two content changes at one address within a short window the
// address is left alone for the session.
struct DynamicSourceEntry {
  uint64_t lastHash = 0;
  uint32_t lastFrame = 0;
  uint32_t changes = 0;
  bool blacklisted = false;
};
absl::flat_hash_map<const void*, DynamicSourceEntry> s_dynamicSources;
constexpr uint32_t kDynamicWindowFrames = 600;
constexpr uint32_t kDynamicChangesToBlacklist = 2;

struct Job {
  TextureHandle handle;
  std::vector<uint8_t> source; // raw GX image or RGBA8 (rgbaInput)
  bool rgbaInput = false;
  uint32_t width = 0, height = 0, mips = 0, format = 0;
  bool wrapX = false, wrapY = false;
  uint32_t factor = 2;
  float sharpen = 0.0f;
  CacheKey key;
  std::string label;
  uint32_t scheduledFrame = 0;
};

struct Result {
  TextureHandle handle;
  ByteBuffer rgba; // all mips, factor-scaled
  uint32_t width = 0, height = 0, mips = 0, factor = 2;
  CacheKey key;
  std::string label;
  uint32_t scheduledFrame = 0;
};

std::mutex s_jobMutex;
std::condition_variable s_jobCv;
std::deque<Job> s_jobs;
std::deque<Result> s_results;
std::thread s_worker;
bool s_workerStarted = false;
bool s_workerStop = false;

// Frames to wait before swapping: a GPU-decoded CMPR source has its compute
// decode encoded on the next frame, which must target the original texture.
constexpr uint32_t kApplyFrameDelay = 3;
// Largest upscaled level (texels). The Super-xBR working set is about 48 bytes
// per output texel plus the RGBA result, so 4M texels (2048x2048) keeps one
// job under ~250 MB of host memory; a 4x pass on an 832x456 title image would
// otherwise need more than the process may be able to commit (seen as
// std::bad_alloc on 2026-10-25).
constexpr uint64_t kMaxOutputTexels = 4ull * 1024 * 1024;

struct Px {
  float r, g, b, a;
};

inline float luma(const Px& p) noexcept { return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b; }

// Weighted colour distance between two pixels: luminance plus half the alpha
// difference, so masks (decals, fonts stored as alpha) steer the edges too.
inline float df(const float* ya0, const float* ya1) noexcept {
  return std::fabs(ya0[0] - ya1[0]) + 0.5f * std::fabs(ya0[1] - ya1[1]);
}

// Weighted distance along one family of parallel lines of the 4x4 window
// (Hyllian's d_wd with the pass-1 weights).
inline float line_distance(const float* b0, const float* b1, const float* c0, const float* c1, const float* c2,
                           const float* d0, const float* d1, const float* d2, const float* d3, const float* e1,
                           const float* e2, const float* e3, const float* f2, const float* f3) noexcept {
  constexpr float wp1 = 2.0f, wp2 = 1.0f, wp3 = -1.0f, wp4 = 4.0f, wp5 = -1.0f, wp6 = 1.0f;
  return wp1 * (df(c1, c2) + df(c1, c0) + df(e2, e1) + df(e2, e3)) + wp2 * (df(d2, d3) + df(d0, d1)) +
         wp3 * (df(d1, d3) + df(d0, d2)) + wp4 * df(d1, d2) + wp5 * (df(c0, c2) + df(e1, e3)) +
         wp6 * (df(b0, b1) + df(f2, f3));
}

// P[i][j]: i along +x, j along +y, both 0..3 for offsets -1..2. The new pixel
// sits between P[1][1], P[2][1], P[1][2] and P[2][2].
inline Px xbr_center(const Px P[4][4], const float YA[4][4][2]) noexcept {
  // Lines with i - j constant ("\" through P11 and P22).
  const float wdBackslash =
      line_distance(YA[0][2], YA[1][3], YA[0][1], YA[1][2], YA[2][3], YA[0][0], YA[1][1], YA[2][2], YA[3][3],
                    YA[1][0], YA[2][1], YA[3][2], YA[2][0], YA[3][1]);
  // Lines with i + j constant ("/" through P21 and P12).
  const float wdSlash =
      line_distance(YA[1][0], YA[0][1], YA[2][0], YA[1][1], YA[0][2], YA[3][0], YA[2][1], YA[1][2], YA[0][3],
                    YA[3][1], YA[2][2], YA[1][3], YA[3][2], YA[2][3]);
  constexpr float w1 = -0.129633f;
  constexpr float w2 = 0.629633f; // w1 + w2 = 0.5
  Px out;
  if (wdBackslash <= wdSlash) {
    // Pixels vary less along "\": interpolate along it.
    out.r = w1 * (P[0][0].r + P[3][3].r) + w2 * (P[1][1].r + P[2][2].r);
    out.g = w1 * (P[0][0].g + P[3][3].g) + w2 * (P[1][1].g + P[2][2].g);
    out.b = w1 * (P[0][0].b + P[3][3].b) + w2 * (P[1][1].b + P[2][2].b);
    out.a = w1 * (P[0][0].a + P[3][3].a) + w2 * (P[1][1].a + P[2][2].a);
  } else {
    out.r = w1 * (P[3][0].r + P[0][3].r) + w2 * (P[2][1].r + P[1][2].r);
    out.g = w1 * (P[3][0].g + P[0][3].g) + w2 * (P[2][1].g + P[1][2].g);
    out.b = w1 * (P[3][0].b + P[0][3].b) + w2 * (P[2][1].b + P[1][2].b);
    out.a = w1 * (P[3][0].a + P[0][3].a) + w2 * (P[2][1].a + P[1][2].a);
  }
  // Anti-ringing: stay within the four nearest pixels.
  const auto clampTo = [](float v, float a, float b, float c, float d) noexcept {
    const float lo = std::min(std::min(a, b), std::min(c, d));
    const float hi = std::max(std::max(a, b), std::max(c, d));
    return std::clamp(v, lo, hi);
  };
  out.r = clampTo(out.r, P[1][1].r, P[2][1].r, P[1][2].r, P[2][2].r);
  out.g = clampTo(out.g, P[1][1].g, P[2][1].g, P[1][2].g, P[2][2].g);
  out.b = clampTo(out.b, P[1][1].b, P[2][1].b, P[1][2].b, P[2][2].b);
  out.a = clampTo(out.a, P[1][1].a, P[2][1].a, P[1][2].a, P[2][2].a);
  return out;
}

inline int32_t wrap_or_clamp(int32_t v, int32_t size, bool wrap) noexcept {
  if (wrap) {
    v %= size;
    return v < 0 ? v + size : v;
  }
  return std::clamp(v, 0, size - 1);
}

struct FloatImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<Px> px;
  std::vector<float> ya; // 2 per pixel

  void set(uint32_t x, uint32_t y, const Px& p) noexcept {
    const size_t i = static_cast<size_t>(y) * width + x;
    px[i] = p;
    ya[i * 2] = luma(p);
    ya[i * 2 + 1] = p.a;
  }
};

inline uint8_t to_byte(float v) noexcept {
  return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// Diagnostic BMP writer (24-bit, bottom-up). channel: 0..3 = that channel as
// gray, 4 = RGB.
void write_bmp(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height, int channel) noexcept {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t rowBytes = (width * 3 + 3) & ~3u;
  const uint32_t dataSize = rowBytes * height;
  uint8_t header[54] = {'B', 'M'};
  auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = uint8_t(v >> (8 * i)); };
  put32(2, 54 + dataSize); put32(10, 54); put32(14, 40); put32(18, width); put32(22, height);
  header[26] = 1; header[28] = 24; put32(34, dataSize);
  std::fwrite(header, 1, 54, f);
  std::vector<uint8_t> row(rowBytes, 0);
  for (uint32_t y = height; y-- > 0;) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* p = rgba + (size_t(y) * width + x) * 4;
      if (channel == 4) { row[x * 3] = p[2]; row[x * 3 + 1] = p[1]; row[x * 3 + 2] = p[0]; }
      else { row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = p[channel]; }
    }
    std::fwrite(row.data(), 1, rowBytes, f);
  }
  std::fclose(f);
}

// Unsharp mask on RGB (alpha untouched): out = p + amount * (p - blur3x3(p)), clamped.
void unsharp_rgba(uint8_t* img, uint32_t width, uint32_t height, bool wrapX, bool wrapY, float amount) noexcept {
  if (amount <= 0.0f || width < 3 || height < 3) {
    return;
  }
  const int32_t w = static_cast<int32_t>(width);
  const int32_t h = static_cast<int32_t>(height);
  std::vector<uint8_t> src(img, img + static_cast<size_t>(w) * h * 4);
  parallel_for_rows(height, static_cast<size_t>(w) * h, [&](uint32_t firstRow, uint32_t lastRow) {
    for (int32_t y = static_cast<int32_t>(firstRow); y < static_cast<int32_t>(lastRow); ++y) {
      for (int32_t x = 0; x < w; ++x) {
        float sum[3] = {0.0f, 0.0f, 0.0f};
        for (int32_t dy = -1; dy <= 1; ++dy) {
          const int32_t sy = wrap_or_clamp(y + dy, h, wrapY);
          for (int32_t dx = -1; dx <= 1; ++dx) {
            const int32_t sx = wrap_or_clamp(x + dx, w, wrapX);
            const uint8_t* p = src.data() + (static_cast<size_t>(sy) * w + sx) * 4;
            sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
          }
        }
        uint8_t* d = img + (static_cast<size_t>(y) * w + x) * 4;
        for (int c = 0; c < 3; ++c) {
          const float v = d[c] + amount * (d[c] - sum[c] / 9.0f);
          d[c] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
        }
      }
    }
  });
}
} // namespace

void super_xbr_2x(const uint8_t* src, uint32_t width, uint32_t height, bool wrapX, bool wrapY,
                  uint8_t* dst) noexcept {
  ZoneScoped;
  const int32_t w = static_cast<int32_t>(width);
  const int32_t h = static_cast<int32_t>(height);
  const int32_t w2 = w * 2;
  const int32_t h2 = h * 2;

  // Source as floats with the per-pixel distance terms.
  FloatImage in;
  in.width = width;
  in.height = height;
  in.px.resize(static_cast<size_t>(w) * h);
  in.ya.resize(static_cast<size_t>(w) * h * 2);
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      const uint8_t* s = src + (static_cast<size_t>(y) * w + x) * 4;
      in.set(x, y, Px{s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f, s[3] / 255.0f});
    }
  }

  FloatImage out;
  out.width = static_cast<uint32_t>(w2);
  out.height = static_cast<uint32_t>(h2);
  out.px.resize(static_cast<size_t>(w2) * h2);
  out.ya.resize(static_cast<size_t>(w2) * h2 * 2);

  // Pass 1: copy source pixels to (2x, 2y) and fill the block centres (2x+1, 2y+1).
  parallel_for_rows(static_cast<uint32_t>(h), static_cast<size_t>(w) * h, [&](uint32_t firstRow, uint32_t lastRow) {
    Px P[4][4];
    float YA[4][4][2];
    for (int32_t y = static_cast<int32_t>(firstRow); y < static_cast<int32_t>(lastRow); ++y) {
      for (int32_t x = 0; x < w; ++x) {
        for (int32_t j = 0; j < 4; ++j) {
          const int32_t sy = wrap_or_clamp(y - 1 + j, h, wrapY);
          for (int32_t i = 0; i < 4; ++i) {
            const int32_t sx = wrap_or_clamp(x - 1 + i, w, wrapX);
            const size_t idx = static_cast<size_t>(sy) * w + sx;
            P[i][j] = in.px[idx];
            YA[i][j][0] = in.ya[idx * 2];
            YA[i][j][1] = in.ya[idx * 2 + 1];
          }
        }
        out.set(static_cast<uint32_t>(2 * x), static_cast<uint32_t>(2 * y), P[1][1]);
        out.set(static_cast<uint32_t>(2 * x + 1), static_cast<uint32_t>(2 * y + 1), xbr_center(P, YA));
      }
    }
  });

  // Pass 2: the remaining pixels, whose four nearest neighbours form a diamond.
  // In the rotated frame P[i][j] = out(ox + i + j - 3, oy + i - j); those
  // coordinates only ever land on pixels pass 1 produced, so rows are independent.
  parallel_for_rows(static_cast<uint32_t>(h2), static_cast<size_t>(w2) * h2, [&](uint32_t firstRow, uint32_t lastRow) {
    Px P[4][4];
    float YA[4][4][2];
    for (int32_t oy = static_cast<int32_t>(firstRow); oy < static_cast<int32_t>(lastRow); ++oy) {
      for (int32_t ox = (oy & 1) ? 0 : 1; ox < w2; ox += 2) {
        for (int32_t j = 0; j < 4; ++j) {
          for (int32_t i = 0; i < 4; ++i) {
            const int32_t sx = wrap_or_clamp(ox + i + j - 3, w2, wrapX);
            const int32_t sy = wrap_or_clamp(oy + i - j, h2, wrapY);
            const size_t idx = static_cast<size_t>(sy) * w2 + sx;
            P[i][j] = out.px[idx];
            YA[i][j][0] = out.ya[idx * 2];
            YA[i][j][1] = out.ya[idx * 2 + 1];
          }
        }
        const Px c = xbr_center(P, YA);
        const size_t idx = static_cast<size_t>(oy) * w2 + ox;
        out.px[idx] = c; // ya of these pixels is never read by this pass
      }
    }
  });

  for (size_t i = 0; i < out.px.size(); ++i) {
    const Px& p = out.px[i];
    dst[i * 4 + 0] = to_byte(p.r);
    dst[i * 4 + 1] = to_byte(p.g);
    dst[i * 4 + 2] = to_byte(p.b);
    dst[i * 4 + 3] = to_byte(p.a);
  }
}

void set_config(uint32_t factor, uint32_t maxSourceSize, float sharpen) noexcept {
  if (factor != 2 && factor != 4) {
    factor = 1;
  }
  s_factor.store(factor, std::memory_order_relaxed);
  s_maxSourceSize.store(std::clamp(maxSourceSize, 16u, 1024u), std::memory_order_relaxed);
  s_sharpen.store(std::clamp(sharpen, 0.0f, 1.0f), std::memory_order_relaxed);
}

Config config() noexcept {
  return {s_factor.load(std::memory_order_relaxed), s_maxSourceSize.load(std::memory_order_relaxed),
          s_sharpen.load(std::memory_order_relaxed)};
}

bool enabled() noexcept { return s_factor.load(std::memory_order_relaxed) > 1; }

Stats stats() noexcept {
  Stats s;
  s.upscaled = s_statUpscaled.load(std::memory_order_relaxed);
  s.cacheHits = s_statCacheHits.load(std::memory_order_relaxed);
  s.skipped = s_statSkipped.load(std::memory_order_relaxed);
  s.pending = s_pendingJobs.load(std::memory_order_relaxed);
  s.cpuMicroseconds = s_statCpuMicroseconds.load(std::memory_order_relaxed);
  {
    std::lock_guard lock{s_cacheMutex};
    s.cacheBytes = s_cacheBytes;
  }
  return s;
}

namespace {
bool color_format(u32 format) noexcept {
  switch (format) {
  case GX_TF_I4:
  case GX_TF_I8:
  case GX_TF_IA4:
  case GX_TF_IA8:
  case GX_TF_RGB565:
  case GX_TF_RGB5A3:
  case GX_TF_RGBA8:
  case GX_TF_CMPR:
  case GX_TF_RGBA8_PC:
    return true;
  default:
    return false; // depth formats, palette indices (handled via the RGBA input), host-only formats
  }
}

bool qualifies(const GXTexObj_& obj, uint32_t factor, uint32_t maxSourceSize) noexcept {
  const uint32_t width = obj.width();
  const uint32_t height = obj.height();
  if (width < 8 || height < 8 || width > maxSourceSize || height > maxSourceSize) {
    return false;
  }
  const uint32_t limit = webgpu::g_graphicsConfig.maxTextureDimension2D;
  if (limit != 0 && (width * factor > limit || height * factor > limit)) {
    return false;
  }
  return true;
}

CacheKey make_key(const GXTexObj_& obj, uint64_t contentHash, uint32_t mips, uint32_t factor) noexcept {
  return CacheKey{
      .contentHash = contentHash,
      .width = obj.width(),
      .height = obj.height(),
      .mips = mips,
      .factor = factor,
      .format = obj.format(),
      .wrapS = static_cast<uint8_t>(obj.wrap_s()),
      .wrapT = static_cast<uint8_t>(obj.wrap_t()),
  };
}

TextureHandle lookup_cache(const CacheKey& key) noexcept {
  std::lock_guard lock{s_cacheMutex};
  const auto it = s_cache.find(key);
  if (it == s_cache.end()) {
    return {};
  }
  s_lru.splice(s_lru.begin(), s_lru, it->second.lruIt);
  s_statCacheHits.fetch_add(1, std::memory_order_relaxed);
  return it->second.handle;
}

void store_cached(const CacheKey& key, const TextureHandle& handle, uint64_t bytes) noexcept {
  std::lock_guard lock{s_cacheMutex};
  s_pendingKeys.erase(key);
  if (s_cache.contains(key)) {
    return;
  }
  s_lru.push_front(key);
  s_cache.emplace(key, CacheEntry{.handle = handle, .bytes = bytes, .lruIt = s_lru.begin()});
  s_cacheBytes += bytes;
  while (s_cacheBytes > kCacheBudgetBytes && !s_lru.empty()) {
    const auto victim = s_lru.back();
    if (const auto it = s_cache.find(victim); it != s_cache.end()) {
      s_cacheBytes -= it->second.bytes;
      s_cache.erase(it);
    }
    s_lru.pop_back();
  }
}

// Worker side: decode (if needed) and upscale every level. Returns false when
// the texture turned out not to qualify (arbitrary mips, decode failure).
bool run_job(const Job& job, Result& result) noexcept {
  const uint8_t* rgba = nullptr;
  size_t rgbaSize = 0;
  ConvertedTexture converted;
  if (job.rgbaInput || job.format == GX_TF_RGBA8_PC) {
    rgba = job.source.data();
    rgbaSize = job.source.size();
  } else {
    converted = convert_texture(job.format, job.width, job.height, job.mips,
                                ArrayRef<uint8_t>{job.source.data(), job.source.size()});
    if (converted.data.empty() || converted.format != wgpu::TextureFormat::RGBA8Unorm || converted.hasArbitraryMips) {
      return false;
    }
    rgba = converted.data.data();
    rgbaSize = converted.data.size();
  }

  size_t outBytes = 0;
  size_t inBytes = 0;
  for (uint32_t mip = 0; mip < job.mips; ++mip) {
    const uint32_t mw = std::max(job.width >> mip, 1u);
    const uint32_t mh = std::max(job.height >> mip, 1u);
    inBytes += static_cast<size_t>(mw) * mh * 4;
    outBytes += static_cast<size_t>(mw) * job.factor * mh * job.factor * 4;
  }
  if (inBytes > rgbaSize) {
    Log.warn("texture_upscale: {}x{} mips={} needs {} RGBA bytes, decoded {}", job.width, job.height, job.mips,
             inBytes, rgbaSize);
    return false;
  }
  if (uint64_t{job.width} * job.factor * job.height * job.factor > kMaxOutputTexels) {
    static unsigned reported = 0;
    if (reported++ < 8) {
      Log.info("texture_upscale: {}x{} at {}x exceeds the {} texel limit; kept original", job.width, job.height,
               job.factor, kMaxOutputTexels);
    }
    return false;
  }

  const auto start = std::chrono::steady_clock::now();
  ByteBuffer out{outBytes};
  size_t inOffset = 0;
  size_t outOffset = 0;
  std::vector<uint8_t> scratch;
  for (uint32_t mip = 0; mip < job.mips; ++mip) {
    const uint32_t mw = std::max(job.width >> mip, 1u);
    const uint32_t mh = std::max(job.height >> mip, 1u);
    const uint8_t* levelSrc = rgba + inOffset;
    uint8_t* levelDst = out.data() + outOffset;
    if (job.factor == 2) {
      super_xbr_2x(levelSrc, mw, mh, job.wrapX, job.wrapY, levelDst);
    } else {
      scratch.resize(static_cast<size_t>(mw) * 2 * mh * 2 * 4);
      super_xbr_2x(levelSrc, mw, mh, job.wrapX, job.wrapY, scratch.data());
      super_xbr_2x(scratch.data(), mw * 2, mh * 2, job.wrapX, job.wrapY, levelDst);
    }
    unsharp_rgba(levelDst, mw * job.factor, mh * job.factor, job.wrapX, job.wrapY, job.sharpen);
    inOffset += static_cast<size_t>(mw) * mh * 4;
    outOffset += static_cast<size_t>(mw) * job.factor * mh * job.factor * 4;
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
  s_statCpuMicroseconds.fetch_add(static_cast<uint64_t>(elapsed), std::memory_order_relaxed);

  // Diagnostic: WII_COMPILED_TEXTURE_UPSCALE_TRACE=1 reports, per job, how much
  // of level 0 is magenta-like (R,B high, G low) before and after the filter.
  static const bool trace = std::getenv("WII_COMPILED_TEXTURE_UPSCALE_TRACE") != nullptr;
  if (trace) {
    const auto magentaRatio = [](const uint8_t* px, size_t count) {
      size_t hits = 0;
      for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = px + i * 4;
        if (p[0] > 120 && p[1] < 50 && p[2] > 120) ++hits;
      }
      return count ? 100.0 * static_cast<double>(hits) / static_cast<double>(count) : 0.0;
    };
    const double srcRatio = magentaRatio(rgba, static_cast<size_t>(job.width) * job.height);
    const double dstRatio = magentaRatio(out.data(), static_cast<size_t>(job.width) * job.factor * job.height * job.factor);
    static std::atomic<unsigned> traced{0};
    const unsigned n = traced.fetch_add(1, std::memory_order_relaxed);
    if (n < 16 || srcRatio > 1.0 || dstRatio > 1.0) {
      Log.info("[upscale-trace] {}x{} fmt={} mips={} factor={} magenta_src={:.2f}% magenta_dst={:.2f}% hash={:016x}",
               job.width, job.height, job.format, job.mips, job.factor, srcRatio, dstRatio, job.key.contentHash);
    }
    static const char* dumpDir = std::getenv("WII_COMPILED_TEXTURE_UPSCALE_DUMP");
    static std::atomic<unsigned> dumped{0};
    if (dumpDir && srcRatio > 1.0 && dumped.fetch_add(1, std::memory_order_relaxed) < 12) {
      const auto base = fmt::format("{}/{:016x}_{}x{}_f{}", dumpDir, job.key.contentHash, job.width, job.height, job.format);
      write_bmp(base + "_src_rgb.bmp", rgba, job.width, job.height, 4);
      write_bmp(base + "_src_a.bmp", rgba, job.width, job.height, 3);
      write_bmp(base + "_dst_rgb.bmp", out.data(), job.width * job.factor, job.height * job.factor, 4);
      write_bmp(base + "_dst_a.bmp", out.data(), job.width * job.factor, job.height * job.factor, 3);
    }
  }

  result.handle = job.handle;
  result.rgba = std::move(out);
  result.width = job.width;
  result.height = job.height;
  result.mips = job.mips;
  result.factor = job.factor;
  result.key = job.key;
  result.label = job.label;
  result.scheduledFrame = job.scheduledFrame;
  return true;
}

void worker_main() noexcept {
  for (;;) {
    Job job;
    {
      std::unique_lock lock{s_jobMutex};
      s_jobCv.wait(lock, [] { return s_workerStop || !s_jobs.empty(); });
      if (s_workerStop) {
        return;
      }
      job = std::move(s_jobs.front());
      s_jobs.pop_front();
    }
    // Nobody references the texture any more: skip the work.
    if (job.handle.use_count() <= 1) {
      std::lock_guard lock{s_cacheMutex};
      s_pendingKeys.erase(job.key);
      s_pendingJobs.fetch_sub(1, std::memory_order_relaxed);
      continue;
    }
    Result result;
    bool ok = false;
    try {
      ok = run_job(job, result);
    } catch (const std::bad_alloc&) {
      Log.warn("texture_upscale: out of memory upscaling {}x{} at {}x; kept original", job.width, job.height,
               job.factor);
      ok = false;
    }
    if (ok) {
      std::lock_guard lock{s_jobMutex};
      s_results.push_back(std::move(result));
      s_completedReady.fetch_add(1, std::memory_order_release);
    } else {
      std::lock_guard lock{s_cacheMutex};
      s_pendingKeys.erase(job.key);
      s_statSkipped.fetch_add(1, std::memory_order_relaxed);
      s_pendingJobs.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

void ensure_worker() noexcept {
  std::lock_guard lock{s_jobMutex};
  if (!s_workerStarted) {
    s_workerStarted = true;
    s_workerStop = false;
    s_worker = std::thread(worker_main);
  }
}

bool begin_pending(const CacheKey& key) noexcept {
  std::lock_guard lock{s_cacheMutex};
  if (s_cache.contains(key) || s_pendingKeys.contains(key)) {
    return false;
  }
  s_pendingKeys.insert(key);
  return true;
}
} // namespace

TextureHandle find_cached(const GXTexObj_& obj, ArrayRef<uint8_t> data) noexcept {
  const uint32_t factor = s_factor.load(std::memory_order_relaxed);
  if (factor <= 1 || data.empty() || !color_format(obj.format()) ||
      !qualifies(obj, factor, s_maxSourceSize.load(std::memory_order_relaxed))) {
    return {};
  }
  return lookup_cache(make_key(obj, XXH64(data.data(), data.size(), 0x53585852ull), obj.mip_count(), factor));
}

TextureHandle find_cached_rgba(const GXTexObj_& obj, const ConvertedTexture& rgba) noexcept {
  const uint32_t factor = s_factor.load(std::memory_order_relaxed);
  if (factor <= 1 || rgba.data.empty() || rgba.format != wgpu::TextureFormat::RGBA8Unorm || rgba.hasArbitraryMips ||
      !qualifies(obj, factor, s_maxSourceSize.load(std::memory_order_relaxed))) {
    return {};
  }
  return lookup_cache(
      make_key(obj, XXH64(rgba.data.data(), rgba.data.size(), 0x53585850ull), obj.mip_count(), factor));
}

void schedule(const TextureHandle& handle, const GXTexObj_& obj, ArrayRef<uint8_t> data, bool rgbaInput,
              const char* label) noexcept {
  const uint32_t factor = s_factor.load(std::memory_order_relaxed);
  if (factor <= 1 || !handle || data.empty()) {
    return;
  }
  if (!rgbaInput && !color_format(obj.format())) {
    return;
  }
  if (!qualifies(obj, factor, s_maxSourceSize.load(std::memory_order_relaxed))) {
    s_statSkipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (handle->hasArbitraryMips.load(std::memory_order_relaxed)) {
    s_statSkipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ZoneScoped;
  const uint32_t mips = obj.mip_count();
  const CacheKey key =
      make_key(obj, XXH64(data.data(), data.size(), rgbaInput ? 0x53585850ull : 0x53585852ull), mips, factor);
  {
    std::lock_guard lock{s_cacheMutex};
    auto& entry = s_dynamicSources[static_cast<const void*>(obj.data)];
    const uint32_t frame = current_frame();
    if (entry.blacklisted) {
      s_statSkipped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (entry.lastHash != 0 && entry.lastHash != key.contentHash) {
      entry.changes = (frame - entry.lastFrame <= kDynamicWindowFrames) ? entry.changes + 1 : 1;
      if (entry.changes >= kDynamicChangesToBlacklist) {
        entry.blacklisted = true;
        static unsigned reported = 0;
        if (reported++ < 8) {
          Log.info("texture_upscale: {}x{} fmt={} at {} rewritten every few frames; left original for this session",
                   obj.width(), obj.height(), obj.format(), static_cast<const void*>(obj.data));
        }
        s_statSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
    entry.lastHash = key.contentHash;
    entry.lastFrame = frame;
    if (s_dynamicSources.size() > 65536) {
      s_dynamicSources.clear();
    }
  }
  if (!begin_pending(key)) {
    return;
  }
  ensure_worker();
  Job job;
  job.handle = handle;
  job.source.assign(data.data(), data.data() + data.size());
  job.rgbaInput = rgbaInput;
  job.width = obj.width();
  job.height = obj.height();
  job.mips = mips;
  job.format = obj.format();
  job.wrapX = obj.wrap_s() == GX_REPEAT;
  job.wrapY = obj.wrap_t() == GX_REPEAT;
  job.factor = factor;
  job.sharpen = s_sharpen.load(std::memory_order_relaxed);
  job.key = key;
  job.label = label != nullptr ? label : "GX Static Texture";
  job.scheduledFrame = current_frame();
  s_pendingJobs.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock{s_jobMutex};
    s_jobs.push_back(std::move(job));
  }
  s_jobCv.notify_one();
}

void apply_completed() noexcept {
  if (s_completedReady.load(std::memory_order_acquire) == 0) {
    return;
  }
  ZoneScoped;
  const uint32_t frame = current_frame();
  for (;;) {
    Result result;
    {
      std::lock_guard lock{s_jobMutex};
      if (s_results.empty()) {
        return;
      }
      if (frame < s_results.front().scheduledFrame + kApplyFrameDelay) {
        return; // the original's GPU work may still be queued; retry next frame
      }
      result = std::move(s_results.front());
      s_results.pop_front();
      s_completedReady.fetch_sub(1, std::memory_order_release);
    }
    s_pendingJobs.fetch_sub(1, std::memory_order_relaxed);
    if (result.handle.use_count() <= 1 || result.handle->isReplacement) {
      std::lock_guard lock{s_cacheMutex};
      s_pendingKeys.erase(result.key);
      continue;
    }
    const auto label = fmt::format("{} (Super-xBR {}x)", result.label, result.factor);
    auto fresh = new_dynamic_texture_2d(result.width * result.factor, result.height * result.factor, result.mips,
                                        GX_TF_RGBA8_PC, label.c_str());
    if (!fresh) {
      std::lock_guard lock{s_cacheMutex};
      s_pendingKeys.erase(result.key);
      continue;
    }
    write_texture(*fresh, ArrayRef<uint8_t>{result.rgba.data(), result.rgba.size()});
    // Swap the GPU objects inside the shared TextureRef: every cache keeps its
    // handle, bind groups made earlier keep the old texture alive, and the next
    // bind (keyed by view and mip count) picks the new one.
    TextureRef& target = *result.handle;
    // The live-texture accounting keys on size/mips: move the difference now.
    g_liveTextureBytes.fetch_add(approx_texture_bytes(fresh->size, fresh->mipCount) -
                                     approx_texture_bytes(target.size, target.mipCount),
                                 std::memory_order_relaxed);
    target.texture = std::move(fresh->texture);
    target.sampleTextureView = std::move(fresh->sampleTextureView);
    target.attachmentTextureView = wgpu::TextureView{};
    target.size = fresh->size;
    target.format = fresh->format;
    target.mipCount = fresh->mipCount;
    target.gxFormat = GX_TF_RGBA8_PC;
    target.hasArbitraryMips.store(false, std::memory_order_relaxed);
    const uint64_t bytes = result.rgba.size();
    store_cached(result.key, result.handle, bytes);
    const auto count = s_statUpscaled.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 8 || count % 64 == 0) {
      Log.info("texture_upscale: {} textures so far (pending {}), last {}x{} mips={} -> {}x{}", count,
               s_pendingJobs.load(std::memory_order_relaxed), result.width, result.height, result.mips,
               result.width * result.factor, result.height * result.factor);
    }
  }
}

void shutdown() noexcept {
  {
    std::lock_guard lock{s_jobMutex};
    s_workerStop = true;
  }
  s_jobCv.notify_all();
  if (s_workerStarted && s_worker.joinable()) {
    s_worker.join();
  }
  {
    std::lock_guard lock{s_jobMutex};
    s_workerStarted = false;
    s_jobs.clear();
    s_results.clear();
    s_completedReady.store(0, std::memory_order_relaxed);
  }
  Log.info("texture_upscale summary: upscaled={} cache_hits={} skipped={} cpu_ms={}",
           s_statUpscaled.load(std::memory_order_relaxed), s_statCacheHits.load(std::memory_order_relaxed),
           s_statSkipped.load(std::memory_order_relaxed), s_statCpuMicroseconds.load(std::memory_order_relaxed) / 1000);
  std::lock_guard lock{s_cacheMutex};
  s_cache.clear();
  s_pendingKeys.clear();
  s_dynamicSources.clear();
  s_lru.clear();
  s_cacheBytes = 0;
}
} // namespace aurora::gfx::texture_upscale
