#include "texture_convert.hpp"
#include "tex_cmpr_decode.hpp"

#include "../internal.hpp"
#include "../gx/gx_fmt.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aurora/host_cores.h>
#elif defined(__linux__)
#include <aurora/host_cores.h>
#endif

namespace aurora::gfx {
static Module Log("aurora::gfx");

struct RGBA8 {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
};

namespace {
constexpr float kArbMipThreshold = 15.0f;

// Texture decoding splits a large image by rows of blocks across a small
// persistent pool; every row reads and writes its own bytes, so the result is
// identical to the serial loop. Small images stay on the calling thread.
constexpr size_t kParallelDecodeMinTexels = 128 * 128;

class DecodePool {
public:
  static DecodePool& instance() {
    static DecodePool pool;
    return pool;
  }

  // Runs fn(begin, end) over [0, count) split into chunks; the caller takes
  // part and returns when every chunk is done. Queued helpers that start after
  // that find no chunk left: the counters they touch are shared-owned, and fn
  // is only called for a chunk the caller is still waiting on.
  template <typename F>
  void run(uint32_t count, F&& fn) {
    struct State {
      std::atomic<uint32_t> next{0};
      std::atomic<uint32_t> remaining{0};
      uint32_t chunks = 0;
      uint32_t count = 0;
      std::function<void(uint32_t, uint32_t)> fn;
      std::mutex doneMutex;
      std::condition_variable done;
    };
    auto state = std::make_shared<State>();
    state->chunks = static_cast<uint32_t>(std::min<size_t>(count, m_threads.size() + 1));
    state->remaining.store(state->chunks, std::memory_order_relaxed);
    state->count = count;
    state->fn = [&fn](uint32_t begin, uint32_t end) { fn(begin, end); };
    const auto work = [state] {
      for (uint32_t chunk; (chunk = state->next.fetch_add(1, std::memory_order_relaxed)) < state->chunks;) {
        state->fn(state->count * chunk / state->chunks, state->count * (chunk + 1) / state->chunks);
        if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          std::lock_guard lock{state->doneMutex};
          state->done.notify_all();
        }
      }
    };
    {
      std::lock_guard lock{m_mutex};
      for (uint32_t i = 1; i < state->chunks; ++i) m_jobs.emplace_back(work);
    }
    m_cv.notify_all();
    work();
    std::unique_lock lock{state->doneMutex};
    state->done.wait(lock, [&] { return state->remaining.load(std::memory_order_acquire) == 0; });
  }

private:
  DecodePool() {
    const unsigned hardware = std::max(std::thread::hardware_concurrency(), 2u);
    // Leave room for the game, GX and presentation threads.
    const unsigned count = std::clamp(hardware / 2, 1u, 6u);
    for (unsigned i = 0; i < count; ++i) {
      m_threads.emplace_back([this] {
#if defined(_WIN32) || defined(__linux__)
        aurora::host_cores::keep_off_game_core();
#endif
        for (;;) {
          std::function<void()> job;
          {
            std::unique_lock lock{m_mutex};
            m_cv.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop && m_jobs.empty()) return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
          }
          job();
        }
      });
    }
  }
  ~DecodePool() {
    {
      std::lock_guard lock{m_mutex};
      m_stop = true;
    }
    m_cv.notify_all();
    for (auto& thread : m_threads) thread.join();
  }

  std::vector<std::thread> m_threads;
  std::deque<std::function<void()>> m_jobs;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_stop = false;
};

std::atomic<bool> g_parallelTextureDecode{[] {
  const char* value = std::getenv("WII_COMPILED_AURORA_PARALLEL_TEXTURE_DECODE");
  return value == nullptr || std::string_view{value} != "0";
}()};

// Set while WII_COMPILED_AURORA_TEXTURE_DECODE_VERIFY re-decodes serially.
thread_local bool t_serialDecode = false;
thread_local bool t_verifyParallelOnly = false;

// Decodes block rows [0, rows) of one mip level, in parallel when it is large.
template <typename F>
void for_block_rows(uint32_t rows, size_t texels, F&& fn) {
  if (rows > 1 && texels >= kParallelDecodeMinTexels && !t_serialDecode &&
      g_parallelTextureDecode.load(std::memory_order_relaxed)) {
    DecodePool::instance().run(rows, fn);
  } else {
    fn(0u, rows);
  }
}

size_t calc_size_rgba8(uint32_t width, uint32_t height) {
  return static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(RGBA8);
}

size_t calc_offset_rgba8(uint32_t x, uint32_t y, uint32_t width) {
  return (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * sizeof(RGBA8);
}

/**
 * Downscales RGBA8 data using a simple box filter. Rows are independent, so
 * large levels are split across the decode pool with identical output.
 */
ByteBuffer downscale(const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight) {
  ByteBuffer dst{calc_size_rgba8(dstWidth, dstHeight)};
  auto* dstPixels = dst.data();
  for_block_rows(dstHeight, size_t{dstWidth} * dstHeight, [&](uint32_t firstRow, uint32_t lastRow) {
    for (uint32_t y = firstRow; y < lastRow; ++y) {
      const uint32_t srcY0 = std::min(y * 2, srcHeight - 1);
      const uint32_t srcY1 = std::min(srcY0 + 1, srcHeight - 1);
      for (uint32_t x = 0; x < dstWidth; ++x) {
        const uint32_t srcX0 = std::min(x * 2, srcWidth - 1);
        const uint32_t srcX1 = std::min(srcX0 + 1, srcWidth - 1);
        const size_t sampleOffsets[4] = {
            calc_offset_rgba8(srcX0, srcY0, srcWidth),
            calc_offset_rgba8(srcX1, srcY0, srcWidth),
            calc_offset_rgba8(srcX0, srcY1, srcWidth),
            calc_offset_rgba8(srcX1, srcY1, srcWidth),
        };
        uint8_t* out = dstPixels + calc_offset_rgba8(x, y, dstWidth);
        for (size_t channel = 0; channel < 4; ++channel) {
          uint32_t sum = 0;
          for (const size_t offset : sampleOffsets) {
            sum += src[offset + channel];
          }
          out[channel] = static_cast<uint8_t>((sum + 2) / 4);
        }
      }
    }
  });
  return dst;
}

float avg_diff(const uint8_t* lhs, const uint8_t* rhs, uint32_t width, uint32_t height) {
  // Integer squared differences: the total is exact (far below 2^53), so the
  // split sum equals the serial double accumulation bit for bit.
  std::atomic<uint64_t> diffSum{0};
  const size_t rowBytes = calc_size_rgba8(width, 1);
  for_block_rows(height, size_t{width} * height, [&](uint32_t firstRow, uint32_t lastRow) {
    uint64_t partial = 0;
    for (size_t i = firstRow * rowBytes, end = lastRow * rowBytes; i < end; ++i) {
      const int diff = static_cast<int>(lhs[i]) - static_cast<int>(rhs[i]);
      partial += static_cast<uint64_t>(diff * diff);
    }
    diffSum.fetch_add(partial, std::memory_order_relaxed);
  });
  const double sampleCount = static_cast<double>(width) * static_cast<double>(height) * 4.0;
  return static_cast<float>(std::sqrt(static_cast<double>(diffSum.load()) / sampleCount) / 2.56);
}

/**
 * Attempts to detect whether the mipmaps of a texture are manually-authored, which is sometimes used for mipmap-based
 * distance effects. This is achieved by downscaling the mipmaps using a box filter and comparing the average pixel
 * difference between the original and downscaled mipmaps.
 */
bool arb_mip_check(uint32_t width, uint32_t height, uint32_t mips, ArrayRef<uint8_t> data) {
  if (mips < 2) {
    return false;
  }

  const uint32_t maxMips = max_texture_mip_count(width, height);
  if (mips > maxMips) {
    Log.warn("arb_mip_check: mip count {} exceeds the {} levels supported by {}x{}", mips, maxMips, width, height);
    return false;
  }

  struct MipLevel {
    const uint8_t* data;
    uint32_t width;
    uint32_t height;
  };
  std::vector<MipLevel> levels;
  levels.reserve(mips);

  size_t offset = 0;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const size_t mipSize = calc_size_rgba8(width, height);
    if (offset > data.size() || mipSize > data.size() - offset) {
      Log.warn("arb_mip_check: expected at least {} bytes, got {}", offset + mipSize, data.size());
      return false;
    }
    levels.push_back({data.data() + offset, width, height});
    offset += mipSize;
    width = std::max(width >> 1, 1u);
    height = std::max(height >> 1, 1u);
  }

  ByteBuffer downscaled;
  float totalDiff = 0.0f;
  for (uint32_t mip = 0; mip + 1 < mips; ++mip) {
    const uint8_t* src = mip == 0 ? levels[mip].data : downscaled.data();
    downscaled = downscale(src, levels[mip].width, levels[mip].height, levels[mip + 1].width,
                           levels[mip + 1].height);
    totalDiff += avg_diff(levels[mip + 1].data, downscaled.data(), levels[mip + 1].width,
                          levels[mip + 1].height);
  }
  return (totalDiff / static_cast<float>(mips - 1)) > kArbMipThreshold;
}
} // namespace

// http://www.mindcontrol.org/~hplus/graphics/expand-bits.html
template <uint8_t v>
constexpr uint8_t ExpandTo8(uint8_t n) {
  if constexpr (v == 3) {
    return (n << (8 - 3)) | (n << (8 - 6)) | (n >> (9 - 8));
  } else {
    return (n << (8 - v)) | (n >> ((v * 2) - 8));
  }
}

constexpr uint8_t S3TCBlend(uint32_t a, uint32_t b) {
  return static_cast<uint8_t>((((a << 1) + a) + ((b << 2) + b)) >> 3);
}

constexpr uint8_t HalfBlend(uint8_t a, uint8_t b) {
  return static_cast<uint8_t>((static_cast<uint32_t>(a) + static_cast<uint32_t>(b)) >> 1);
}

static size_t ComputeMippedTexelCount(uint32_t w, uint32_t h, uint32_t mips) {
  size_t ret = w * h;
  for (uint32_t i = mips; i > 1; --i) {
    if (w > 1) {
      w /= 2;
    }
    if (h > 1) {
      h /= 2;
    }
    ret += w * h;
  }
  return ret;
}

static std::optional<size_t> RequiredTextureDataSize(u32 format, uint32_t width, uint32_t height, uint32_t mips) {
  if (width == 0 || height == 0 || mips == 0) {
    return std::nullopt;
  }

  uint32_t blockWidth = 1;
  uint32_t blockHeight = 1;
  uint32_t blockSize = 0;
  switch (format) {
    DEFAULT_FATAL("RequiredTextureDataSize: unknown texture format {}", format);
  case GX_TF_R8_PC:
    blockSize = 1;
    break;
  case GX_TF_RGBA8_PC:
    blockSize = 4;
    break;
  case GX_TF_I4:
  case GX_TF_C4:
  case GX_TF_CMPR:
    blockWidth = 8;
    blockHeight = 8;
    blockSize = 32;
    break;
  case GX_TF_I8:
  case GX_TF_IA4:
  case GX_TF_C8:
  case GX_TF_Z8:
    blockWidth = 8;
    blockHeight = 4;
    blockSize = 32;
    break;
  case GX_TF_IA8:
  case GX_TF_RGB565:
  case GX_TF_RGB5A3:
  case GX_TF_C14X2:
  case GX_TF_Z16:
    blockWidth = 4;
    blockHeight = 4;
    blockSize = 32;
    break;
  case GX_TF_RGBA8:
  case GX_TF_Z24X8:
    blockWidth = 4;
    blockHeight = 4;
    blockSize = 64;
    break;
  }

  size_t total = 0;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const size_t widthBlocks = (static_cast<size_t>(width) + blockWidth - 1) / blockWidth;
    const size_t heightBlocks = (static_cast<size_t>(height) + blockHeight - 1) / blockHeight;
    if (widthBlocks > std::numeric_limits<size_t>::max() / heightBlocks ||
        widthBlocks * heightBlocks > (std::numeric_limits<size_t>::max() - total) / blockSize) {
      return std::nullopt;
    }
    total += widthBlocks * heightBlocks * blockSize;
    width = std::max(width >> 1, 1u);
    height = std::max(height >> 1, 1u);
  }
  return total;
}

template <typename T>
concept TextureDecoder = requires(T) {
  typename T::Source;
  typename T::Target;
  { T::Frac } -> std::convertible_to<uint32_t>;
  { T::BlockWidth } -> std::convertible_to<uint32_t>;
  { T::BlockHeight } -> std::convertible_to<uint32_t>;
  { T::decode_texel(std::declval<typename T::Target*>(), std::declval<const typename T::Source*>(), 0u) };
};

template <TextureDecoder T>
static ByteBuffer DecodeTiled(uint32_t width, uint32_t height, uint32_t mips, ArrayRef<uint8_t> data) {
  const size_t texelCount = ComputeMippedTexelCount(width, height, mips);
  ByteBuffer buf{texelCount * sizeof(typename T::Target)};

  uint32_t w = width;
  uint32_t h = height;
  auto* targetMip = reinterpret_cast<typename T::Target*>(buf.data());
  const auto* in = reinterpret_cast<const typename T::Source*>(data.data());
  constexpr size_t blockUnits = T::BlockWidth * T::BlockHeight / T::Frac;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const uint32_t bwidth = (w + (T::BlockWidth - 1)) / T::BlockWidth;
    const uint32_t bheight = (h + (T::BlockHeight - 1)) / T::BlockHeight;
    for_block_rows(bheight, size_t{w} * h, [&, w, h, targetMip, mipIn = in](uint32_t first, uint32_t last) {
      const auto* in = mipIn + size_t{first} * bwidth * blockUnits;
      for (uint32_t by = first; by < last; ++by) {
        const uint32_t baseY = by * T::BlockHeight;
        const uint32_t numRows = std::min(h - baseY, T::BlockHeight);
        for (uint32_t bx = 0; bx < bwidth; ++bx) {
          const uint32_t baseX = bx * T::BlockWidth;
          for (uint32_t y = 0; y < numRows; ++y) {
            auto* target = targetMip + (baseY + y) * w + baseX;
            const auto n = std::min(w - baseX, T::BlockWidth);
            for (uint32_t x = 0; x < n; ++x) {
              T::decode_texel(target, in, x);
            }
            in += T::BlockWidth / T::Frac;
          }
          const uint32_t extraY = T::BlockHeight - numRows;
          in += T::BlockWidth * extraY / T::Frac;
        }
      }
    });
    in += size_t{bheight} * bwidth * blockUnits;
    targetMip += w * h;
    if (w > 1) {
      w /= 2;
    }
    if (h > 1) {
      h /= 2;
    }
  }

  return buf;
}

template <TextureDecoder T>
static ByteBuffer DecodeLinear(uint32_t width, ArrayRef<uint8_t> data) {
  ByteBuffer buf{width * sizeof(typename T::Target)};
  auto* target = reinterpret_cast<typename T::Target*>(buf.data());
  const auto* in = reinterpret_cast<const typename T::Source*>(data.data());
  for (uint32_t x = 0; x < width; ++x) {
    T::decode_texel(target, in, x);
  }
  return buf;
}

struct TextureDecoderI4 {
  using Source = uint8_t;
  using Target = RGBA8;

  static constexpr uint32_t Frac = 2;
  static constexpr uint32_t BlockWidth = 8;
  static constexpr uint32_t BlockHeight = 8;

  static void decode_texel(Target* target, const Source* in, const uint32_t x) {
    const uint8_t intensity = ExpandTo8<4>(in[x / 2] >> (x & 1 ? 0 : 4) & 0xf);
    target[x].r = intensity;
    target[x].g = intensity;
    target[x].b = intensity;
    target[x].a = intensity;
  }
};

struct TextureDecoderI8 {
  using Source = uint8_t;
  using Target = RGBA8;

  static constexpr uint32_t Frac = 1;
  static constexpr uint32_t BlockWidth = 8;
  static constexpr uint32_t BlockHeight = 4;

  static void decode_texel(Target* target, const Source* in, const uint32_t x) {
    const uint8_t intensity = in[x];
    target[x].r = intensity;
    target[x].g = intensity;
    target[x].b = intensity;
    target[x].a = intensity;
  }
};

struct TextureDecoderIA4 {
  using Source = uint8_t;
  using Target = RGBA8;

  static constexpr uint32_t Frac = 1;
  static constexpr uint32_t BlockWidth = 8;
  static constexpr uint32_t BlockHeight = 4;

  static void decode_texel(Target* target, const Source* in, const uint32_t x) {
    const uint8_t intensity = ExpandTo8<4>(in[x] & 0xf);
    target[x].r = intensity;
    target[x].g = intensity;
    target[x].b = intensity;
    target[x].a = ExpandTo8<4>(in[x] >> 4);
  }
};

struct TextureDecoderIA8 {
  using Source = uint16_t;
  using Target = RGBA8;

  static constexpr uint32_t Frac = 1;
  static constexpr uint32_t BlockWidth = 4;
  static constexpr uint32_t BlockHeight = 4;

  static void decode_texel(Target* target, const Source* in, const uint32_t x) {
    const uint8_t intensity = in[x] >> 8;
    target[x].r = intensity;
    target[x].g = intensity;
    target[x].b = intensity;
    target[x].a = in[x] & 0xff;
  }
};

struct TextureDecoderC4 {
  using Source = uint8_t;
  using Target = uint16_t;

  static constexpr uint32_t Frac = 2;
  static constexpr uint32_t BlockWidth = 8;
  static constexpr uint32_t BlockHeight = 8;

  static void decode_texel(Target* target, const Source* in, const uint32_t x) {
    target[x] = in[x / 2] >> (x & 1 ? 0 : 4) & 0xf;
  }
};

struct TextureDecoderC8 {
  using Source = uint8_t;
  using Target = uint16_t;

  static constexpr uint32_t Frac = 1;
  static constexpr uint32_t BlockWidth = 8;
  static constexpr uint32_t BlockHeight = 4;

  static void decode_texel(Target* target, const Source* in, const uint32_t x) { target[x] = in[x]; }
};

struct TextureDecoderC14X2 {
  using Source = uint16_t;
  using Target = uint16_t;

  static constexpr uint32_t Frac = 1;
  static constexpr uint32_t BlockWidth = 4;
  static constexpr uint32_t BlockHeight = 4;

  static void decode_texel(Target* target, const Source* in, const uint32_t x) { target[x] = bswap(in[x]) & 0x3fff; }
};

struct TextureDecoderRGB565 {
  using Source = uint16_t;
  using Target = RGBA8;

  static constexpr uint32_t Frac = 1;
  static constexpr uint32_t BlockWidth = 4;
  static constexpr uint32_t BlockHeight = 4;

  static void decode_texel(Target* target, const Source* in, const uint32_t x) {
    const auto texel = bswap(in[x]);
    target[x].r = ExpandTo8<5>(texel >> 11 & 0x1f);
    target[x].g = ExpandTo8<6>(texel >> 5 & 0x3f);
    target[x].b = ExpandTo8<5>(texel & 0x1f);
    target[x].a = 0xff;
  }
};

struct TextureDecoderRGB5A3 {
  using Source = uint16_t;
  using Target = RGBA8;

  static constexpr uint32_t Frac = 1;
  static constexpr uint32_t BlockWidth = 4;
  static constexpr uint32_t BlockHeight = 4;

  static void decode_texel(Target* target, const Source* in, const uint32_t x) {
    const auto texel = bswap(in[x]);
    if ((texel & 0x8000) != 0) {
      target[x].r = ExpandTo8<5>(texel >> 10 & 0x1f);
      target[x].g = ExpandTo8<5>(texel >> 5 & 0x1f);
      target[x].b = ExpandTo8<5>(texel & 0x1f);
      target[x].a = 0xff;
    } else {
      target[x].r = ExpandTo8<4>(texel >> 8 & 0xf);
      target[x].g = ExpandTo8<4>(texel >> 4 & 0xf);
      target[x].b = ExpandTo8<4>(texel & 0xf);
      target[x].a = ExpandTo8<3>(texel >> 12 & 0x7);
    }
  }
};

static ByteBuffer BuildRGBA8FromGCN(uint32_t width, uint32_t height, uint32_t mips, ArrayRef<uint8_t> data) {
  const size_t texelCount = ComputeMippedTexelCount(width, height, mips);
  ByteBuffer buf{sizeof(RGBA8) * texelCount};

  uint32_t w = width;
  uint32_t h = height;
  auto* targetMip = reinterpret_cast<RGBA8*>(buf.data());
  const uint8_t* in = data.data();
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const uint32_t bwidth = (w + 3) / 4;
    const uint32_t bheight = (h + 3) / 4;
    for_block_rows(bheight, size_t{w} * h, [&, w, h, targetMip, mipIn = in](uint32_t first, uint32_t last) {
      const uint8_t* in = mipIn + size_t{first} * bwidth * 64;
      for (uint32_t by = first; by < last; ++by) {
        const uint32_t baseY = by * 4;
        for (uint32_t bx = 0; bx < bwidth; ++bx) {
          const uint32_t baseX = bx * 4;
          for (uint32_t c = 0; c < 2; ++c) {
            for (uint32_t y = 0; y < 4; ++y) {
              for (size_t x = 0; x < 4; ++x) {
                if (baseX + x >= w || baseY + y >= h) {
                  continue;
                }
                RGBA8* target = targetMip + (baseY + y) * w + baseX;
                if (c != 0) {
                  target[x].g = in[x * 2];
                  target[x].b = in[x * 2 + 1];
                } else {
                  target[x].a = in[x * 2];
                  target[x].r = in[x * 2 + 1];
                }
              }
              in += 8;
            }
          }
        }
      }
    });
    in += size_t{bheight} * bwidth * 64;
    targetMip += w * h;
    if (w > 1) {
      w /= 2;
    }
    if (h > 1) {
      h /= 2;
    }
  }

  return buf;
}

static ByteBuffer BuildRGBA8FromCMPR(uint32_t width, uint32_t height, uint32_t mips, ArrayRef<uint8_t> data) {
  const size_t texelCount = ComputeMippedTexelCount(width, height, mips);
  ByteBuffer buf{sizeof(RGBA8) * texelCount};

  uint32_t h = height;
  uint32_t w = width;
  uint8_t* dst = buf.data();
  const uint8_t* src = data.data();
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const uint32_t bwidth = (w + 7) / 8;
    const uint32_t bheight = (h + 7) / 8;
    for_block_rows(bheight, size_t{w} * h, [&, w, h, dst, mipSrc = src](uint32_t first, uint32_t last) {
      const uint8_t* src = mipSrc + size_t{first} * bwidth * 32;
      for (uint32_t yy = first * 8; yy < last * 8 && yy < h; yy += 8) {
        for (uint32_t xx = 0; xx < w; xx += 8) {
          for (uint32_t yb = 0; yb < 8; yb += 4) {
            for (uint32_t xb = 0; xb < 8; xb += 4) {
              // CMPR difference: Big-endian color1/2
              const uint16_t color1 = bswap(*reinterpret_cast<const uint16_t*>(src));
              const uint16_t color2 = bswap(*reinterpret_cast<const uint16_t*>(src + 2));
              src += 4;

              // Fill in first two colors in color table.
              std::array<uint8_t, 16> color_table{};

              color_table[0] = ExpandTo8<5>(static_cast<uint8_t>((color1 >> 11) & 0x1F));
              color_table[1] = ExpandTo8<6>(static_cast<uint8_t>((color1 >> 5) & 0x3F));
              color_table[2] = ExpandTo8<5>(static_cast<uint8_t>(color1 & 0x1F));
              color_table[3] = 0xFF;

              color_table[4] = ExpandTo8<5>(static_cast<uint8_t>((color2 >> 11) & 0x1F));
              color_table[5] = ExpandTo8<6>(static_cast<uint8_t>((color2 >> 5) & 0x3F));
              color_table[6] = ExpandTo8<5>(static_cast<uint8_t>(color2 & 0x1F));
              color_table[7] = 0xFF;
              if (color1 > color2) {
                // Predict gradients.
                color_table[8] = S3TCBlend(color_table[4], color_table[0]);
                color_table[9] = S3TCBlend(color_table[5], color_table[1]);
                color_table[10] = S3TCBlend(color_table[6], color_table[2]);
                color_table[11] = 0xFF;

                color_table[12] = S3TCBlend(color_table[0], color_table[4]);
                color_table[13] = S3TCBlend(color_table[1], color_table[5]);
                color_table[14] = S3TCBlend(color_table[2], color_table[6]);
                color_table[15] = 0xFF;
              } else {
                color_table[8] = HalfBlend(color_table[0], color_table[4]);
                color_table[9] = HalfBlend(color_table[1], color_table[5]);
                color_table[10] = HalfBlend(color_table[2], color_table[6]);
                color_table[11] = 0xFF;

                // CMPR difference: GX fills with an alpha 0 midway point here.
                color_table[12] = color_table[8];
                color_table[13] = color_table[9];
                color_table[14] = color_table[10];
                color_table[15] = 0;
              }

              for (uint32_t y = 0; y < 4; ++y) {
                uint8_t bits = src[y];
                for (uint32_t x = 0; x < 4; ++x) {
                  if (xx + xb + x >= w || yy + yb + y >= h) {
                    continue;
                  }
                  uint8_t* dstOffs = dst + ((yy + yb + y) * w + (xx + xb + x)) * 4;
                  const uint8_t* colorTableOffs = &color_table[static_cast<size_t>((bits >> 6) & 3) * 4];
                  memcpy(dstOffs, colorTableOffs, 4);
                  bits <<= 2;
                }
              }
              src += 4;
            }
          }
        }
      }
    });
    src += size_t{bheight} * bwidth * 32;
    dst += w * h * 4;
    if (w > 1) {
      w /= 2;
    }
    if (h > 1) {
      h /= 2;
    }
  }

  return buf;
}

ConvertedTexture convert_texture(u32 format, uint32_t width, uint32_t height, uint32_t mips, ArrayRef<uint8_t> data) {
  const uint32_t maxMips = max_texture_mip_count(width, height);
  const uint32_t safeMips = std::clamp(mips, 1u, maxMips);
  if (safeMips != mips) {
    Log.warn("convert_texture: clamping mip count {} to {} for {}x{} texture", mips, safeMips, width, height);
    mips = safeMips;
  }

  const auto requiredDataSize = RequiredTextureDataSize(format, width, height, mips);
  if (!requiredDataSize) {
    Log.warn("convert_texture: invalid {}x{} texture with {} mip levels", width, height, mips);
    return {};
  }
  if (*requiredDataSize > data.size()) {
    Log.warn("convert_texture: invalid data size {} for {}x{} texture with {} mip levels (expected at least {} bytes)",
             data.size(), width, height, mips, *requiredDataSize);
    return {};
  }

  ByteBuffer converted;
  switch (format) {
    DEFAULT_FATAL("convert_texture: unknown texture format {}", format);
  case GX_TF_R8_PC:
    converted = DecodeLinear<TextureDecoderI8>(static_cast<uint32_t>(ComputeMippedTexelCount(width, height, mips)),
                                                data);
    break;
  case GX_TF_RGBA8_PC:
    return {}; // No conversion
  case GX_TF_I4:
    converted = DecodeTiled<TextureDecoderI4>(width, height, mips, data);
    break;
  case GX_TF_I8:
  case GX_TF_Z8:
    converted = DecodeTiled<TextureDecoderI8>(width, height, mips, data);
    break;
  case GX_TF_IA4:
    converted = DecodeTiled<TextureDecoderIA4>(width, height, mips, data);
    break;
  case GX_TF_IA8:
  case GX_TF_Z16:
    converted = DecodeTiled<TextureDecoderIA8>(width, height, mips, data);
    break;
  case GX_TF_C4:
    converted = DecodeTiled<TextureDecoderC4>(width, height, mips, data);
    break;
  case GX_TF_C8:
    converted = DecodeTiled<TextureDecoderC8>(width, height, mips, data);
    break;
  case GX_TF_C14X2:
    converted = DecodeTiled<TextureDecoderC14X2>(width, height, mips, data);
    break;
  case GX_TF_RGB565:
    converted = DecodeTiled<TextureDecoderRGB565>(width, height, mips, data);
    break;
  case GX_TF_RGB5A3:
    converted = DecodeTiled<TextureDecoderRGB5A3>(width, height, mips, data);
    break;
  case GX_TF_RGBA8:
  case GX_TF_Z24X8:
    converted = BuildRGBA8FromGCN(width, height, mips, data);
    break;
  case GX_TF_CMPR:
    converted = BuildRGBA8FromCMPR(width, height, mips, data);
    break;
  }
  static const bool verifyParallel = std::getenv("WII_COMPILED_AURORA_TEXTURE_DECODE_VERIFY") != nullptr;
  if (verifyParallel && !t_serialDecode && !t_verifyParallelOnly && size_t{width} * height >= kParallelDecodeMinTexels &&
      g_parallelTextureDecode.load(std::memory_order_relaxed)) {
    static std::atomic<uint32_t> checked{0}, mismatched{0};
    // Decode both ways from one snapshot: guest memory (EFB copies) can change
    // between two reads of the live source.
    const std::vector<uint8_t> snapshot(data.data(), data.data() + data.size());
    const ArrayRef<uint8_t> stable{snapshot.data(), snapshot.size()};
    t_serialDecode = true;
    const auto serial = convert_texture(format, width, height, mips, stable);
    t_serialDecode = false;
    t_verifyParallelOnly = true;
    const auto parallel = convert_texture(format, width, height, mips, stable);
    t_verifyParallelOnly = false;
    const bool same = serial.data.size() == parallel.data.size() &&
                      std::equal(parallel.data.data(), parallel.data.data() + parallel.data.size(), serial.data.data());
    if (!same) mismatched.fetch_add(1, std::memory_order_relaxed);
    const uint32_t count = checked.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!same || count % 64 == 1)
      Log.info("[texture-decode-verify] checked={} mismatched={} last={}x{} format={} mips={}", count,
               mismatched.load(std::memory_order_relaxed), width, height, format, mips);
  }
  const auto wgpuFormat = to_wgpu(format);
  bool hasArbitraryMips = false;
  if (wgpuFormat == wgpu::TextureFormat::RGBA8Unorm && mips > 1) {
    hasArbitraryMips = arb_mip_check(width, height, mips, converted);
  }
  return {
      .format = wgpuFormat,
      .width = width,
      .height = height,
      .mips = mips,
      .data = std::move(converted),
      .hasArbitraryMips = hasArbitraryMips,
  };
}

ConvertedTexture convert_tlut(u32 format, uint32_t width, ArrayRef<uint8_t> data) {
  const size_t requiredDataSize = static_cast<size_t>(width) * sizeof(uint16_t);
  if (requiredDataSize > data.size()) {
    Log.warn("convert_tlut: invalid data size {} for {} entries (expected at least {} bytes)", data.size(), width,
             requiredDataSize);
    return {};
  }

  ByteBuffer converted;
  switch (format) {
    DEFAULT_FATAL("convert_tlut: unsupported tlut format {}", format);
  case GX_TF_IA8: // GX_TL_IA8
    converted = DecodeLinear<TextureDecoderIA8>(width, data);
    break;
  case GX_TF_RGB565: // GX_TL_RGB565
    converted = DecodeLinear<TextureDecoderRGB565>(width, data);
    break;
  case GX_TF_RGB5A3: // GX_TL_RGB5A3
    converted = DecodeLinear<TextureDecoderRGB5A3>(width, data);
    break;
  }
  return {
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .width = width,
      .height = 1,
      .mips = 1,
      .data = std::move(converted),
  };
}

GXTexFmt tlut_texture_format(GXTlutFmt format) noexcept {
  switch (format) {
    DEFAULT_FATAL("tlut_texture_format: unsupported tlut format {}", format);
  case GX_TL_IA8:
    return GX_TF_IA8;
  case GX_TL_RGB565:
    return GX_TF_RGB565;
  case GX_TL_RGB5A3:
    return GX_TF_RGB5A3;
  }
}

ConvertedTexture convert_texture_palette(u32 textureFormat, uint32_t width, uint32_t height, uint32_t mips,
                                         ArrayRef<uint8_t> textureData, GXTlutFmt tlutFormat, uint16_t tlutEntries,
                                         ArrayRef<uint8_t> tlutData) {
  const auto indices = convert_texture(textureFormat, width, height, mips, textureData);
  if (indices.data.empty()) {
    return {};
  }
  mips = indices.mips;
  const auto palette = convert_tlut(tlut_texture_format(tlutFormat), tlutEntries, tlutData);
  if (palette.data.empty()) {
    return {};
  }

  ByteBuffer pixels;
  pixels.reserve_extra(indices.data.size() / sizeof(u16) * 4);

  const auto* indexData = reinterpret_cast<const u16*>(indices.data.data());
  const uint32_t baseWidth = width;
  const uint32_t baseHeight = height;
  size_t offset = 0;
  for (u32 mip = 0; mip < mips; ++mip) {
    const size_t pixelCount = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < pixelCount; ++i) {
      const u32 index = indexData[offset + i];
      if (index >= tlutEntries) {
        constexpr uint8_t transparent[4] = {0, 0, 0, 0};
        pixels.append(transparent, sizeof(transparent));
        continue;
      }
      const size_t src = static_cast<size_t>(index) * 4;
      pixels.append(palette.data.data() + src, 4);
    }
    offset += pixelCount;
    width = std::max(width >> 1, 1u);
    height = std::max(height >> 1, 1u);
  }

  bool hasArbitraryMips = arb_mip_check(baseWidth, baseHeight, mips, pixels);
  return {
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .width = baseWidth,
      .height = baseHeight,
      .mips = mips,
      .data = std::move(pixels),
      .hasArbitraryMips = hasArbitraryMips,
  };
}
} // namespace aurora::gfx

namespace aurora::gfx {
namespace {
// One low-priority worker: the check only refines LOD bias/filtering of textures
// with hand-authored mips, so it must never compete with frame work.
class ArbitraryMipChecker {
public:
  static ArbitraryMipChecker& instance() {
    static ArbitraryMipChecker checker;
    return checker;
  }
  void push(std::weak_ptr<TextureRef> texture, std::vector<uint8_t> data, uint32_t width, uint32_t height,
            uint32_t mips) {
    {
      std::lock_guard lock{m_mutex};
      m_jobs.push_back({std::move(texture), std::move(data), width, height, mips});
    }
    m_cv.notify_one();
  }

private:
  struct Job {
    std::weak_ptr<TextureRef> texture;
    std::vector<uint8_t> data;
    uint32_t width, height, mips;
  };
  ArbitraryMipChecker() {
    m_thread = std::thread([this] {
#if defined(_WIN32)
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      aurora::host_cores::keep_off_game_core();
#elif defined(__linux__)
      aurora::host_cores::set_thread_nice(5);
      aurora::host_cores::keep_off_game_core();
#endif
      t_serialDecode = true;
      for (;;) {
        Job job;
        {
          std::unique_lock lock{m_mutex};
          m_cv.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
          if (m_jobs.empty()) return;
          job = std::move(m_jobs.front());
          m_jobs.pop_front();
        }
        if (job.texture.expired()) continue;
        const auto converted =
            convert_texture(GX_TF_CMPR, job.width, job.height, job.mips, {job.data.data(), job.data.size()});
        if (auto texture = job.texture.lock()) texture->hasArbitraryMips.store(converted.hasArbitraryMips);
      }
    });
  }
  ~ArbitraryMipChecker() {
    {
      std::lock_guard lock{m_mutex};
      m_stop = true;
      m_jobs.clear();
    }
    m_cv.notify_all();
    m_thread.join();
  }
  std::thread m_thread;
  std::deque<Job> m_jobs;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_stop = false;
};
} // namespace

void schedule_arbitrary_mip_check(const TextureHandle& texture, ArrayRef<uint8_t> data) {
  ArbitraryMipChecker::instance().push(texture, std::vector<uint8_t>(data.data(), data.data() + data.size()),
                                       texture->size.width, texture->size.height, texture->mipCount);
}
} // namespace aurora::gfx

extern "C" void aurora_set_gpu_cmpr_decode(bool enabled) { aurora::gfx::tex_cmpr_decode::set_enabled(enabled); }

extern "C" void aurora_set_parallel_texture_decode(bool enabled) {
  aurora::gfx::g_parallelTextureDecode.store(enabled, std::memory_order_relaxed);
}

namespace aurora::gfx {
void parallel_for_rows(uint32_t rows, size_t texels, const std::function<void(uint32_t, uint32_t)>& fn) {
  for_block_rows(rows, texels, [&](uint32_t firstRow, uint32_t lastRow) { fn(firstRow, lastRow); });
}
} // namespace aurora::gfx
