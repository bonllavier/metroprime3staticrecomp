#include "../gfx/post_process.hpp"
#include "../diagnostic_env.hpp"
#include "command_processor.hpp"
#include "uber.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "fifo.hpp"
#include "xf_viewport.hpp"
#include "producer_profile.hpp"

#include "../gfx/common.hpp"
#include "../gfx/draw_dump.hpp"
#include "../dolphin/gx/__gx.h"
#include "../gfx/texture_replacement.hpp"
#include "dolphin/gx/GXAurora.h"
#include "gx.hpp"
#include "gx_fmt.hpp"
#include "pipeline.hpp"
#include "shader_info.hpp"
#include "../internal.hpp"

#include <absl/container/flat_hash_map.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

// Diagnostic environment switches are fixed for the lifetime of this process.
static bool CachedWiiCompiledTracePrimeGx() {
  static const bool enabled = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_TRACE_PRIME_GX") != nullptr;
  return enabled;
}

// Diagnostic environment switches are fixed for the lifetime of this process.
static bool CachedWiiCompiledTracePrimeMii() {
  static const bool enabled = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_TRACE_PRIME_MII") != nullptr;
  return enabled;
}


// Defined in GXGeometry.cpp; see GXAuroraVtxStateEpoch.
extern "C" u64 g_gxVtxStateEpoch;

namespace aurora::gx::fifo {
static Module Log("aurora::gx::fifo");

using IndexBuffer = std::vector<u16>;

static u32 prepare_idx_template(IndexBuffer& buf, GXPrimitive prim, u16 vtxCount) {
  size_t writePos = 0;
  if (prim == GX_QUADS) {
    // GX renders a three-vertex remainder as a triangle. One/two are ignored.
    const u32 completeVertices = static_cast<u32>(vtxCount) & ~3u;
    buf.resize((completeVertices / 4u) * 6u + (vtxCount % 4u == 3u ? 3u : 0u));

    for (u32 v = 0; v < completeVertices; v += 4) {
      const u16 idx0 = v;
      const u16 idx1 = static_cast<u16>(v + 1);
      const u16 idx2 = static_cast<u16>(v + 2);
      const u16 idx3 = static_cast<u16>(v + 3);
      buf[writePos++] = idx0;
      buf[writePos++] = idx1;
      buf[writePos++] = idx2;
      buf[writePos++] = idx2;
      buf[writePos++] = idx3;
      buf[writePos++] = idx0;
    }
    if (vtxCount % 4u == 3u) {
      buf[writePos++] = static_cast<u16>(completeVertices);
      buf[writePos++] = static_cast<u16>(completeVertices + 1u);
      buf[writePos++] = static_cast<u16>(completeVertices + 2u);
    }
  } else if (prim == GX_TRIANGLES) {
    const u32 completeVertices = (static_cast<u32>(vtxCount) / 3u) * 3u;
    buf.resize(completeVertices);
    for (u32 v = 0; v < completeVertices; ++v) {
      buf[writePos++] = v;
    }
  } else if (prim == GX_TRIANGLEFAN) {
    const u32 indexCount = vtxCount < 3 ? 0u : (static_cast<u32>(vtxCount) - 2u) * 3u;
    buf.resize(indexCount);
    for (u32 v = 0; indexCount != 0 && v < vtxCount; ++v) {
      if (v < 3) {
        buf[writePos++] = v;
        continue;
      }
      buf[writePos++] = 0;
      buf[writePos++] = static_cast<u16>(v - 1);
      buf[writePos++] = v;
    }
  } else if (prim == GX_TRIANGLESTRIP) {
    const u32 indexCount = vtxCount < 3 ? 0u : (static_cast<u32>(vtxCount) - 2u) * 3u;
    buf.resize(indexCount);
    for (u32 v = 0; indexCount != 0 && v < vtxCount; ++v) {
      if (v < 3) {
        buf[writePos++] = v;
        continue;
      }
      if ((v & 1) == 0) {
        buf[writePos++] = static_cast<u16>(v - 2);
        buf[writePos++] = static_cast<u16>(v - 1);
      } else {
        buf[writePos++] = static_cast<u16>(v - 1);
        buf[writePos++] = static_cast<u16>(v - 2);
      }
      buf[writePos++] = v;
    }
  } else if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
    buf = {0, 1, 3, 3, 2, 0};
    writePos = 6;
  } else
    UNLIKELY FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  CHECK(writePos == buf.size(), "index template size mismatch ({} != {})", writePos, buf.size());
  return static_cast<u32>(writePos);
}

// Empty/incomplete draws consume FIFO bytes but cannot produce a primitive.
static bool has_complete_primitive(GXPrimitive prim, u16 count) {
  if (prim == GX_POINTS) return count >= 1;
  if (prim == GX_LINES || prim == GX_LINESTRIP) return count >= 2;
  return count >= 3;
}

// GX FIFO opcodes - use CP_ prefix to avoid clashing with GXCommandList.h macros
static constexpr u8 CP_CMD_NOP = GX_NOP;
static constexpr u8 CP_CMD_LOAD_CP_REG = GX_LOAD_CP_REG;
static constexpr u8 CP_CMD_LOAD_XF_REG = GX_LOAD_XF_REG;
static constexpr u8 CP_CMD_LOAD_INDX_A = GX_LOAD_INDX_A;
static constexpr u8 CP_CMD_LOAD_INDX_B = GX_LOAD_INDX_B;
static constexpr u8 CP_CMD_LOAD_INDX_C = GX_LOAD_INDX_C;
static constexpr u8 CP_CMD_LOAD_INDX_D = GX_LOAD_INDX_D;
static constexpr u8 CP_CMD_CALL_DL = GX_CMD_CALL_DL;
static constexpr u8 CP_CMD_INVAL_VTX = GX_CMD_INVL_VC;
static constexpr u8 CP_CMD_LOAD_BP_REG = GX_LOAD_BP_REG & GX_OPCODE_MASK;

// Primitive type mask
static constexpr u8 CP_OPCODE_MASK = GX_OPCODE_MASK;
static constexpr u8 CP_VAT_MASK = GX_VAT_MASK;
static constexpr u8 CP_PRIMITIVE_START = 0x80;
static constexpr u8 CP_PRIMITIVE_END = 0xBF;

// Read helpers for big/little endian
#if _MSC_VER
template<typename T>
__forceinline // Yes, this was necessary.
inline T unaligned_load(const T* ptr) {
  return *static_cast<const __unaligned T*>(ptr);
}
#else
template<typename T>
inline T unaligned_load(const T* ptr) {
  T copy;
  memcpy(&copy, ptr, sizeof(T));
  return copy;
}
#endif

static inline u16 read_u16(const u8* ptr, bool bigEndian) {
  const u16 val = unaligned_load(reinterpret_cast<const u16*>(ptr));
  if (bigEndian) {
    return bswap(val);
  }
  return val;
}

static inline u32 read_u32(const u8* ptr, bool bigEndian) {
  const u32 val = unaligned_load(reinterpret_cast<const u32*>(ptr));
  if (bigEndian) {
    return bswap(val);
  }
  return val;
}

static bool is_draw_cmd(u8 cmd) {
  return cmd >= CP_PRIMITIVE_START && cmd <= CP_PRIMITIVE_END;
}

static GXPrimitive primitive_from_draw_cmd(u8 cmd) {
  switch (cmd & CP_OPCODE_MASK) {
  case GX_DRAW_QUADS:
  case 0x88:
    return GX_QUADS;
  case GX_DRAW_TRIANGLES:
    return GX_TRIANGLES;
  case GX_DRAW_TRIANGLE_STRIP:
    return GX_TRIANGLESTRIP;
  case GX_DRAW_TRIANGLE_FAN:
    return GX_TRIANGLEFAN;
  case GX_DRAW_LINES:
    return GX_LINES;
  case GX_DRAW_LINE_STRIP:
    return GX_LINESTRIP;
  case GX_DRAW_POINTS:
    return GX_POINTS;
  default:
    UNLIKELY FATAL("unsupported primitive command 0x{:02X}", cmd);
  }
}




static u32 bp_get(u32 reg, u32 size, u32 shift);
static inline f32 read_f32(const u8* ptr, bool bigEndian);

static GXPixelFmt decode_pixel_fmt(u32 peCtrl, u32 cmode1) {
  switch (bp_get(peCtrl, 3, 0)) {
  case 0:
    return GX_PF_RGB8_Z24;
  case 1:
    return GX_PF_RGBA6_Z24;
  case 2:
    return GX_PF_RGB565_Z16;
  case 3:
    return GX_PF_Z24;
  case 4:
    switch (bp_get(cmode1, 2, 9)) {
    case 0:
      return GX_PF_Y8;
    case 1:
      return GX_PF_U8;
    case 2:
      return GX_PF_V8;
    default:
      Log.warn("command_processor: unsupported cmode1 pixel subtype {}", bp_get(cmode1, 2, 9));
      return GX_PF_Y8;
    }
  case 5:
    return GX_PF_YUV420;
  default:
    Log.warn("command_processor: unsupported PE pixel format {}", bp_get(peCtrl, 3, 0));
    return GX_PF_RGB8_Z24;
  }
}

static inline u64 read_u64(const u8* ptr, bool bigEndian) {
  u64 loaded;
  // Unaligned-safe load
  memcpy(&loaded, ptr, sizeof(u64));

  if (bigEndian) {
    return bswap(loaded);
  }

  return loaded;
}

struct TexBpRegMapping {
  u8 texMapId;
  enum class Kind : uint8_t { Mode0, Mode1, Image0, Image1, Image2, Image3, Tlut } kind;
};

static std::optional<TexBpRegMapping> decode_tex_bp_reg(u32 regId) {
  constexpr std::array mode0Ids{0x80u, 0x81u, 0x82u, 0x83u, 0xA0u, 0xA1u, 0xA2u, 0xA3u};
  constexpr std::array mode1Ids{0x84u, 0x85u, 0x86u, 0x87u, 0xA4u, 0xA5u, 0xA6u, 0xA7u};
  constexpr std::array image0Ids{0x88u, 0x89u, 0x8Au, 0x8Bu, 0xA8u, 0xA9u, 0xAAu, 0xABu};
  constexpr std::array image1Ids{0x8Cu, 0x8Du, 0x8Eu, 0x8Fu, 0xACu, 0xADu, 0xAEu, 0xAFu};
  constexpr std::array image2Ids{0x90u, 0x91u, 0x92u, 0x93u, 0xB0u, 0xB1u, 0xB2u, 0xB3u};
  constexpr std::array image3Ids{0x94u, 0x95u, 0x96u, 0x97u, 0xB4u, 0xB5u, 0xB6u, 0xB7u};
  constexpr std::array tlutIds{0x98u, 0x99u, 0x9Au, 0x9Bu, 0xB8u, 0xB9u, 0xBAu, 0xBBu};

  for (u8 i = 0; i < MaxTextures; ++i) {
    if (regId == mode0Ids[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Mode0};
    }
    if (regId == mode1Ids[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Mode1};
    }
    if (regId == image0Ids[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Image0};
    }
    if (regId == image1Ids[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Image1};
    }
    if (regId == image2Ids[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Image2};
    }
    if (regId == image3Ids[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Image3};
    }
    if (regId == tlutIds[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Tlut};
    }
  }
  return std::nullopt;
}

// Helper to convert packed RGBA8 to Vec4<float>
static Vec4<float> unpack_color(u32 packed) {
  return {
      static_cast<float>(packed >> 24 & 0xFF) / 255.f,
      static_cast<float>(packed >> 16 & 0xFF) / 255.f,
      static_cast<float>(packed >> 8 & 0xFF) / 255.f,
      static_cast<float>(packed & 0xFF) / 255.f,
  };
}

static inline f32 read_f32(const u8* ptr, bool bigEndian) {
  u32 bits = read_u32(ptr, bigEndian);
  f32 val;
  std::memcpy(&val, &bits, sizeof(val));
  return val;
}

// Marks draw state dirty *and* invalidates the resolved-pipeline memo.
static inline void mark_pipeline_state_dirty() noexcept {
  g_gxState.stateDirty = true;
  g_gxState.pipelineStateGeneration = next_gx_state_epoch();
}

// Rejects a malformed XF write in release builds.
#define XF_REQUIRE(cond, msg, ...)                                                                                     \
  do {                                                                                                                 \
    if (!(cond)) UNLIKELY {                                                                                            \
      CHECK(cond, msg, ##__VA_ARGS__);                                                                                  \
      Log.warn(msg, ##__VA_ARGS__);                                                                                     \
      return true;                                                                                                     \
    }                                                                                                                  \
  } while (0)

static bool copy_xf_data(u32 addr, const u8* data, u32 len, bool bigEndian) {
  if (addr < 0x78) {
    // Position matrices (0x0000 - 0x0077)
    u32 mtxIdx = addr / 12;
    u32 startOffset = addr % 12;
    // We only support full writes to matrices
    XF_REQUIRE(mtxIdx < MaxPnMtx, "XF: PosMtx copy oob; mtxIdx={}", mtxIdx);
    XF_REQUIRE(startOffset == 0 && len == 12, "XF: PosMtx sub-copy unsupported: offs={}, len={}", startOffset, len);
    auto& mtx = g_gxState.pnMtx[mtxIdx].pos;
    f32* flat = reinterpret_cast<f32*>(&mtx);
    for (u32 i = 0; i < len; i++) {
      flat[i] = read_f32(data + i * 4, bigEndian);
    }
    g_gxState.stateDirty = true;
    return true;
  } else if (addr < 0x0F0) {
    // Texture matrices (0x078-0x0EF)
    u32 texBase = addr - 0x078;
    u32 mtxIdx = texBase / 12;
    u32 startOffset = texBase % 12;
    XF_REQUIRE(mtxIdx < MaxTexMtx, "XF TexMtx copy oob; mtxIdx={}", mtxIdx);
    XF_REQUIRE(startOffset == 0 && (len == 8 || len == 12), "XF TexMtx sub-copy unsupported: offs={}, len={}",
               startOffset, len);

    // Determine if 2x4 or 3x4 from count
    auto& mtx = g_gxState.texMtxs[mtxIdx];
    f32* flat = reinterpret_cast<f32*>(&mtx);
    for (u32 i = 0; i < len; i++) {
      flat[i] = read_f32(data + i * 4, bigEndian);
    }
    g_gxState.stateDirty = true;
    return true;
  } else if (addr >= 0x400 && addr < 0x45A) {
    // Normal matrices (0x400-0x459)
    u32 nrmBase = addr - 0x400;
    u32 mtxIdx = nrmBase / 9;
    u32 startOffset = nrmBase % 9;
    // We only support full writes to matrices
    XF_REQUIRE(mtxIdx < MaxPnMtx, "XF: NrmMtx copy oob; mtxIdx={}", mtxIdx);
    XF_REQUIRE(startOffset == 0 && len == 9, "XF: NrmMtx sub-copy unsupported: offs={}, len={}", startOffset, len);
    auto& mtx = g_gxState.pnMtx[mtxIdx].nrm;
    f32* flat = reinterpret_cast<f32*>(&mtx);
    for (u32 i = 0; i < len; i++) {
      u32 xfIdx = i;
      u32 row = xfIdx / 3;
      u32 col = xfIdx % 3;
      if (row < 3) {
        flat[row * 4 + col] = read_f32(data + i * 4, bigEndian);
      }
    }
    g_gxState.stateDirty = true;
    return true;
  } else if (addr >= 0x500 && addr < 0x5F0) {
    // Post-transform texture matrices (0x500-0x5EF)
    u32 ptBase = addr - 0x500;
    u32 mtxIdx = ptBase / 12;
    u32 startOffset = ptBase % 12;
    XF_REQUIRE(mtxIdx < MaxPTTexMtx, "XF: PTTexMtx copy oob; mtxIdx={}", mtxIdx);
    XF_REQUIRE(startOffset == 0 && len == 12, "XF: PTTexMtx sub-copy unsupported: offs={}, len={}", startOffset, len);
    auto& mtx = g_gxState.ptTexMtxs[mtxIdx];
    f32* flat = reinterpret_cast<f32*>(&mtx);
    for (u32 i = 0; i < len; i++) {
      flat[startOffset + i] = read_f32(data + i * 4, bigEndian);
    }
    g_gxState.stateDirty = true;
    return true;
  } else if (addr >= 0x600 && addr < 0x680) {
    // Lights (0x600-0x67F) - 8 lights, 16 values each
    u32 lightBase = addr - 0x600;
    u32 lightIdx = lightBase / 0x10;
    u32 startOffset = lightBase % 0x10;
    XF_REQUIRE(lightIdx < GX::MaxLights, "XF: Light copy oob; lightIdx={}", lightIdx);
    XF_REQUIRE(startOffset + len <= 0x10,
               "XF: Light copy that crosses across light boundaries unsupported: offs={}, len={}", startOffset, len);
    auto& light = g_gxState.lights[lightIdx];
    for (u32 i = 0; i < len; i++) {
      u32 field = startOffset + i;
      f32 val = read_f32(data + i * 4, bigEndian);
      u32 ival = read_u32(data + i * 4, bigEndian);
      switch (field) {
      case 3: // Color (packed u32)
        light.color = unpack_color(ival);
        break;
      case 4:
        light.cosAtt[0] = val;
        break; // a0
      case 5:
        light.cosAtt[1] = val;
        break; // a1
      case 6:
        light.cosAtt[2] = val;
        break; // a2
      case 7:
        light.distAtt[0] = val;
        break; // k0
      case 8:
        light.distAtt[1] = val;
        break; // k1
      case 9:
        light.distAtt[2] = val;
        break; // k2
      case 10:
        light.pos[0] = val;
        break; // px
      case 11:
        light.pos[1] = val;
        break; // py
      case 12:
        light.pos[2] = val;
        break; // pz
      case 13:
        light.dir[0] = val;
        break; // nx
      case 14:
        light.dir[1] = val;
        break; // ny
      case 15:
        light.dir[2] = val;
        break; // nz
      default:
        break; // padding (0-2)
      }
    }
    g_gxState.preparedLightsDirty = true;
    g_gxState.stateDirty = true;
    return true;
  }
  return false;
}

static void apply_xf_viewport(u32 sourceHeader) {
  const auto& vp = g_gxState.xfViewport;
  const f32 sx = vp[0];
  const f32 sy = vp[1];
  const f32 sz = vp[2];
  const f32 ox = vp[3];
  const f32 oy = vp[4];
  const f32 oz = vp[5];
  const f32 width = sx * 2.0f;
  const f32 height = -sy * 2.0f;
  constexpr f32 z24Scale = 16777216.0f;
  constexpr f32 z24Max = 16777215.0f;

  if (CachedWiiCompiledTracePrimeMii()) {
    static unsigned observations = 0;
    static unsigned unusualObservations = 0;
    const bool unusual = std::abs(width) < 1.0f || std::abs(height) < 1.0f ||
                         width < 0 || height < 0 || !std::isfinite(width + height);
    if (observations++ < 24 || (unusual && unusualObservations++ < 16)) {
      std::array<u32, 6> bits{};
      std::memcpy(bits.data(), vp.data(), sizeof(bits));
      Log.info("[prime-xf-viewport] header={:08x} sx={} sy={} sz={} ox={} oy={} oz={} bits={:08x},{:08x},{:08x},{:08x},{:08x},{:08x}",
               sourceHeader, sx, sy, sz, ox, oy, oz, bits[0], bits[1], bits[2], bits[3], bits[4], bits[5]);
    }
  }

  set_logical_viewport({
      .left = aurora::gx::xf_viewport_edge(ox, width),
      .top = aurora::gx::xf_viewport_edge(oy, height),
      .width = width,
      .height = height,
      // EFB depth saturates at 0xFFFFFF, the value a full clear stores. Prime's
      // viewport puts the far plane at 16777216 and draws sky layers with a
      // collapsed range there under LEQUAL; unclamped they land just beyond
      // the cleared depth and fail the test that passes on hardware.
      .znear = std::min(oz - sz, z24Max) / z24Scale,
      .zfar = std::min(oz, z24Max) / z24Scale,
  });
}

static void apply_xf_projection() {
  const auto& raw = g_gxState.xfProjection;
  auto& proj = g_gxState.proj;
  proj = {};
  proj.m0[0] = raw[0];
  proj.m1[1] = raw[2];
  proj.m2[2] = raw[4];
  proj.m2[3] = raw[5];

  if (g_gxState.projType == GX_ORTHOGRAPHIC) {
    proj.m0[3] = raw[1];
    proj.m1[3] = raw[3];
    proj.m3[3] = 1.0f;
  } else {
    proj.m0[2] = raw[1];
    proj.m1[2] = raw[3];
    proj.m3[2] = -1.0f;
  }

  g_gxState.stateDirty = true;
}

// Forward declarations for register handlers
static void handle_bp(u32 value, bool bigEndian);
static bool host_callback_pending();
static void handle_cp(u8 addr, u32 value, bool bigEndian);
static void handle_xf(const u8* data, u32& pos, u32 size, bool bigEndian);
static bool handle_draw(u8 cmd, const u8* data, u32& pos, u32 size, bool bigEndian);
static bool handle_aurora(const u8* data, u32& pos, u32 size, bool bigEndian);

uint32_t process(const u8* data, u32 size, bool bigEndian) {
  ZoneScoped;
  // Everything decoded here mutates renderer state (GX state, the recorded command lists and the mapped staging buffers), so take the renderer GPU mutex once for the whole drain rather than once per draw command.
  std::lock_guard gpuLock(aurora::renderer_gpu_mutex());
  u32 pos = 0;

  while (pos < size) {
    const u32 commandStart = pos;
    u8 cmd = data[pos++];
    u8 opcode = cmd & CP_OPCODE_MASK;
    // Log.warn("Processing opcode {:02x} at pos {} (size {})", opcode, pos - 1, size);

    switch (opcode) {
    case CP_CMD_NOP:
      continue;

    case CP_CMD_LOAD_BP_REG: {
      CHECK(pos + 4 <= size, "BP reg read overrun");
      u32 value = read_u32(data + pos, bigEndian);
      pos += 4;
      handle_bp(value, bigEndian);
      break;
    }

    case CP_CMD_LOAD_CP_REG: {
      CHECK(pos + 5 <= size, "CP reg read overrun");
      u8 addr = data[pos++];
      u32 value = read_u32(data + pos, bigEndian);
      pos += 4;
      handle_cp(addr, value, bigEndian);
      break;
    }

    case CP_CMD_LOAD_XF_REG: {
      handle_xf(data, pos, size, bigEndian);
      break;
    }

    case CP_CMD_LOAD_INDX_A:
    case CP_CMD_LOAD_INDX_B:
    case CP_CMD_LOAD_INDX_C:
    case CP_CMD_LOAD_INDX_D: {
      ZoneScopedN("LOAD_INDX");
      CHECK(pos + 4 <= size, "indexed XF read overrun");
      const u32 value = read_u32(data + pos, bigEndian);
      pos += 4;

      const u32 arrayType = GX_POS_MTX_ARRAY + ((opcode - CP_CMD_LOAD_INDX_A) / 0x08);
      const u32 srcArrayIdx = value >> 16;
      const u16 len = static_cast<u16>(((value >> 12) & 0x0f) + 1);
      const u16 dstAddr = static_cast<u16>(value & 0x0fff);
      auto const& array = g_gxState.arrays[arrayType];
      const u32 byteOffset = srcArrayIdx * array.stride;
      const u32 byteCount = static_cast<u32>(len) * 4u;
      if (array.data == nullptr || array.stride == 0 || byteOffset + byteCount > array.size) {
        static u32 invalidIndexedXfLogCount = 0;
        if (invalidIndexedXfLogCount < 16) {
          Log.warn("Skipping indexed XF load with invalid source array: array={} idx={} stride={} offset={} bytes={} "
                   "size={} dst=0x{:04X}",
                   arrayType, srcArrayIdx, array.stride, byteOffset, byteCount, array.size, dstAddr);
          ++invalidIndexedXfLogCount;
        }
        break;
      }
      u8* srcData = ((u8*)array.data) + byteOffset;
      if (!copy_xf_data(dstAddr, srcData, len, bigEndian)) {
#ifndef NDEBUG
        Log.debug("Unimplemented indexed XF load (opcode 0x{:02X}, dstAddr=0x{:04X})", opcode, dstAddr);
#endif
      }
      break;
    }

    case CP_CMD_CALL_DL: {
      // Call display list: 8 bytes (address + size)
      CHECK(pos + 8 <= size, "call DL read overrun");
      Log.warn("Ignoring nested GX_CMD_CALL_DL");
      pos += 8;
      break;
    }

    case CP_CMD_INVAL_VTX: {
      // GXInvalidateVtxCache tells the GPU that CPU-written indexed vertex arrays must be observed by subsequent draws.
      for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
        g_gxState.arrays[i].cachedRange = {};
      }
      // A merged draw retains its previous array uploads. Force a new draw so
      // handle_draw_unmerged observes the invalidation and uploads fresh data.
      // Pipeline configuration itself did not change.
      g_gxState.stateDirty = true;
      break;
    }

    case GX_LOAD_AURORA: {
      try {
        if (!handle_aurora(data, pos, size, bigEndian)) {
          return size;
        }
      } catch (const gfx::StagingBatchFull&) {
        return commandStart;  // a deferred draw that does not fit: retried after the batch is submitted
      }
      // A host callback pauses decoding here; process_all runs it without the renderer lock.
      if (host_callback_pending()) return pos;
      break;
    }

    default:
      // Draw commands occupy the full 0x80-0xBF range.
      if (is_draw_cmd(cmd)) {
        try {
          if (!handle_draw(cmd, data, pos, size, bigEndian)) return size;
        } catch (const gfx::StagingBatchFull&) {
          return commandStart;
        }
      } else {
        static u32 unknownLogCount = 0;
        if (unknownLogCount < 16) {
          // Hex dump surrounding bytes for debugging
          u32 dumpStart = (pos > 17) ? pos - 17 : 0;
          u32 dumpEnd = (pos + 16 < size) ? pos + 16 : size;
          std::string hex;
          for (u32 i = dumpStart; i < dumpEnd; i++) {
            if (i == pos - 1)
              hex += fmt::format("[{:02x}]", data[i]);
            else
              hex += fmt::format(" {:02x}", data[i]);
          }
          Log.warn("  hex dump (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
          Log.warn("command_processor: unknown opcode 0x{:02X} at pos {}", cmd, pos - 1);
          ++unknownLogCount;
        }
      }
      break;
    }
  }
  return size;
}

// Helper to extract bit fields from a 32-bit register
inline static u32 bp_get(u32 reg, u32 size, u32 shift) { return reg >> shift & (1u << size) - 1; }

static u8 normal_frac_bits(GXCompType type) {
  switch (type) {
  case GX_U8:
    return 7;
  case GX_S8:
    return 6;
  case GX_U16:
    return 15;
  case GX_S16:
    return 14;
  default:
    return 0;
  }
}

static void refresh_copy_filter_flags() {
  bool aa = false;
  for (const auto& sample : g_gxState.copyFilterSamplePattern) {
    aa |= sample[0] != 6 || sample[1] != 6;
  }
  g_gxState.copyFilterAa = aa;

  static constexpr std::array<u8, 7> DefaultVFilter{0, 0, 21, 22, 21, 0, 0};
  bool vf = false;
  for (size_t i = 0; i < DefaultVFilter.size(); ++i) {
    vf |= g_gxState.copyFilterVFilter[i] != DefaultVFilter[i];
  }
  g_gxState.copyFilterVf = vf;
}

// BP register handler - decodes BP (RAS/pixel engine) register writes and updates g_gxState
static void handle_bp(u32 value, bool bigEndian) {
  u32 regId = (value >> 24) & 0xFF;
  // Mask off the register ID from the value for field extraction
  // (the regId is stored in bits 24-31, data is in bits 0-23)

  if (regId == 0xFE) {
    g_gxState.bpRegCache[regId] = value & 0x00FFFFFF;
    return;
  } else {
    const u32 ssMask = g_gxState.bpRegCache[0xFE];
    // A preceding 0xFE write is rare; the common path only has to prove the mask is already wide open.
    if (ssMask != 0x00FFFFFF) UNLIKELY {
      g_gxState.bpRegCache[0xFE] = 0x00FFFFFF;
    }
    const u32 merged = (g_gxState.bpRegCache[regId] & ~ssMask) | (value & ssMask);
    value = (regId << 24) | (merged & 0x00FFFFFF);
    // Texture invalidation is a command, not persistent state. Repeating
    // the same region invalidation must still invalidate derived bindings.
    if (regId != 0x66 && g_gxState.bpRegCache[regId] == value) return;
    g_gxState.bpRegCache[regId] = value;
  }
  // TEV color combiner stages (0xC0, 0xC2, 0xC4, ... 0xDE)
  if (regId >= 0xC0 && regId <= 0xDE && (regId & 1) == 0) {
    u32 stage = (regId - 0xC0) / 2;
    if (stage < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage];
      s.colorPass.d = static_cast<GXTevColorArg>(bp_get(value, 4, 0));
      s.colorPass.c = static_cast<GXTevColorArg>(bp_get(value, 4, 4));
      s.colorPass.b = static_cast<GXTevColorArg>(bp_get(value, 4, 8));
      s.colorPass.a = static_cast<GXTevColorArg>(bp_get(value, 4, 12));
      s.colorOp.clamp = bp_get(value, 1, 19) != 0;
      s.colorOp.outReg = static_cast<GXTevRegID>(bp_get(value, 2, 22));
      if (bp_get(value, 2, 16) == 3) {
        // Bias==3 means compare mode: reconstruct GXTevOp enum (8 + 3-bit hw value)
        u32 hwOp = bp_get(value, 1, 18) | (bp_get(value, 2, 20) << 1);
        s.colorOp.op = static_cast<GXTevOp>(hwOp + 8);
        s.colorOp.bias = GX_TB_ZERO;
        s.colorOp.scale = GX_CS_SCALE_1;
      } else {
        // Normal mode: bit18 is op (0=ADD, 1=SUB), bits16-17 is bias, bits20-21 is scale
        s.colorOp.op = static_cast<GXTevOp>(bp_get(value, 1, 18));
        s.colorOp.bias = static_cast<GXTevBias>(bp_get(value, 2, 16));
        s.colorOp.scale = static_cast<GXTevScale>(bp_get(value, 2, 20));
      }
      mark_pipeline_state_dirty();
    }
    return;
  }

  // TEV alpha combiner stages (0xC1, 0xC3, 0xC5, ... 0xDF)
  if (regId >= 0xC1 && regId <= 0xDF && (regId & 1) == 1) {
    u32 stage = (regId - 0xC1) / 2;
    if (stage < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage];
      s.tevSwapRas = static_cast<GXTevSwapSel>(bp_get(value, 2, 0));
      s.tevSwapTex = static_cast<GXTevSwapSel>(bp_get(value, 2, 2));
      s.alphaPass.d = static_cast<GXTevAlphaArg>(bp_get(value, 3, 4));
      s.alphaPass.c = static_cast<GXTevAlphaArg>(bp_get(value, 3, 7));
      s.alphaPass.b = static_cast<GXTevAlphaArg>(bp_get(value, 3, 10));
      s.alphaPass.a = static_cast<GXTevAlphaArg>(bp_get(value, 3, 13));
      s.alphaOp.clamp = bp_get(value, 1, 19) != 0;
      s.alphaOp.outReg = static_cast<GXTevRegID>(bp_get(value, 2, 22));
      if (bp_get(value, 2, 16) == 3) {
        u32 hwOp = bp_get(value, 1, 18) | (bp_get(value, 2, 20) << 1);
        s.alphaOp.op = static_cast<GXTevOp>(hwOp + 8);
        s.alphaOp.bias = GX_TB_ZERO;
        s.alphaOp.scale = GX_CS_SCALE_1;
      } else {
        s.alphaOp.op = static_cast<GXTevOp>(bp_get(value, 1, 18));
        s.alphaOp.bias = static_cast<GXTevBias>(bp_get(value, 2, 16));
        s.alphaOp.scale = static_cast<GXTevScale>(bp_get(value, 2, 20));
      }
      mark_pipeline_state_dirty();
    }
    return;
  }

  switch (regId) {
  case 0x66: {
    // Conservatively invalidate all derived native bindings for a regional
    // GX texture invalidation. Raw sources still validate complete bytes.
    invalidate_static_texture_cache();
    g_gxState.stateDirty = true;
    static uint64_t invalidations=0;
    ++invalidations;
    static const bool trace=[] {
      const char* value=AURORA_DIAGNOSTIC_ENV("WII_COMPILED_PRIME_PERFORMANCE_TRACE");
      return value && std::strcmp(value,"1")==0;
    }();
    if(trace && (invalidations<=8 || (invalidations&1023u)==0))
      Log.info("[prime-texture-invalidation] commands={} region={:06X}",invalidations,value&0x00FFFFFFu);
    break;
  }
  // genMode (0x00)
  case 0x00: {
    g_gxState.numTexGens = bp_get(value, 4, 0);
    g_gxState.numChans = bp_get(value, 3, 4);
    // genMode owns the same numTexGens/numChans that XF 0x3F/0x09 decode, so the XF cache can no longer vouch for those two slots.
    g_gxState.invalidateXfReg(0x3F);
    g_gxState.invalidateXfReg(0x09);
    g_gxState.numTevStages = bp_get(value, 4, 10) + 1;
    u32 hwCull = bp_get(value, 2, 14);
    // Swap front/back to match GX convention
    switch (hwCull) {
    case GX_CULL_FRONT:
      g_gxState.cullMode = GX_CULL_BACK;
      break;
    case GX_CULL_BACK:
      g_gxState.cullMode = GX_CULL_FRONT;
      break;
    default:
      g_gxState.cullMode = static_cast<GXCullMode>(hwCull);
      break;
    }
    g_gxState.numIndStages = bp_get(value, 3, 16);
    mark_pipeline_state_dirty();
    break;
  }

  // Display copy sample pattern (0x01-0x04), six 4-bit samples per BP reg.
  case 0x01:
  case 0x02:
  case 0x03:
  case 0x04: {
    const size_t first = static_cast<size_t>(regId - 0x01) * 6;
    for (size_t i = 0; i < 6; ++i) {
      const size_t sample = first + i;
      g_gxState.copyFilterSamplePattern[sample / 2][sample % 2] = static_cast<u8>(bp_get(value, 4, i * 4));
    }
    refresh_copy_filter_flags();
    break;
  }

  // Indirect texture mask (0x0F).
  case 0x0F:
    g_gxState.indTexMask = static_cast<u8>(value & 0xFF);
    g_gxState.stateDirty = true;
    break;

  // TEV indirect stages (0x10-0x1F)
  case 0x10:
  case 0x11:
  case 0x12:
  case 0x13:
  case 0x14:
  case 0x15:
  case 0x16:
  case 0x17:
  case 0x18:
  case 0x19:
  case 0x1A:
  case 0x1B:
  case 0x1C:
  case 0x1D:
  case 0x1E:
  case 0x1F: {
    u32 stage = regId - 0x10;
    if (stage < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage];
      s.indTexStage = static_cast<GXIndTexStageID>(bp_get(value, 2, 0));
      s.indTexFormat = static_cast<GXIndTexFormat>(bp_get(value, 2, 2));
      s.indTexBiasSel = static_cast<GXIndTexBiasSel>(bp_get(value, 3, 4));
      s.indTexAlphaSel = static_cast<GXIndTexAlphaSel>(bp_get(value, 2, 7));
      s.indTexMtxId = static_cast<GXIndTexMtxID>(bp_get(value, 4, 9));
      s.indTexWrapS = static_cast<GXIndTexWrap>(bp_get(value, 3, 13));
      s.indTexWrapT = static_cast<GXIndTexWrap>(bp_get(value, 3, 16));
      s.indTexUseOrigLOD = bp_get(value, 1, 19) != 0;
      s.indTexAddPrev = bp_get(value, 1, 20) != 0;
      mark_pipeline_state_dirty();
    }
    break;
  }

  // Scissor registers (0x20, 0x21)
  case 0x20:
  case 0x21: {
    const u32 scis0 = g_gxState.bpRegCache[0x20];
    const u32 scis1 = g_gxState.bpRegCache[0x21];
    const int32_t tp = static_cast<int32_t>(bp_get(scis0, 11, 0)) - 342;
    const int32_t lf = static_cast<int32_t>(bp_get(scis0, 11, 12)) - 342;
    const int32_t bm = static_cast<int32_t>(bp_get(scis1, 11, 0)) - 342;
    const int32_t rt = static_cast<int32_t>(bp_get(scis1, 11, 12)) - 342;
    const int32_t wd = std::max(rt - lf + 1, 0);
    const int32_t ht = std::max(bm - tp + 1, 0);
    set_logical_scissor({lf, tp, wd, ht});
    break;
  }

  // Line/point size (0x22)
  case 0x22: {
    g_gxState.lineWidth = static_cast<u8>(bp_get(value, 8, 0));
    g_gxState.pointSize = static_cast<u8>(bp_get(value, 8, 8));
    g_gxState.lineTexOffset = static_cast<GXTexOffset>(bp_get(value, 3, 16));
    g_gxState.pointTexOffset = static_cast<GXTexOffset>(bp_get(value, 3, 19));
    g_gxState.lineHalfAspect = bp_get(value, 1, 22) != 0;
    g_gxState.stateDirty = true;
    break;
  }

  // Indirect texture scale (0x25, 0x26)
  case 0x25: {
    if (MaxIndStages > 0) {
      g_gxState.indStages[0].scaleS = static_cast<GXIndTexScale>(bp_get(value, 4, 0));
      g_gxState.indStages[0].scaleT = static_cast<GXIndTexScale>(bp_get(value, 4, 4));
    }
    if (MaxIndStages > 1) {
      g_gxState.indStages[1].scaleS = static_cast<GXIndTexScale>(bp_get(value, 4, 8));
      g_gxState.indStages[1].scaleT = static_cast<GXIndTexScale>(bp_get(value, 4, 12));
    }
    mark_pipeline_state_dirty();
    break;
  }
  case 0x26: {
    if (MaxIndStages > 2) {
      g_gxState.indStages[2].scaleS = static_cast<GXIndTexScale>(bp_get(value, 4, 0));
      g_gxState.indStages[2].scaleT = static_cast<GXIndTexScale>(bp_get(value, 4, 4));
    }
    if (MaxIndStages > 3) {
      g_gxState.indStages[3].scaleS = static_cast<GXIndTexScale>(bp_get(value, 4, 8));
      g_gxState.indStages[3].scaleT = static_cast<GXIndTexScale>(bp_get(value, 4, 12));
    }
    mark_pipeline_state_dirty();
    break;
  }

  // Indirect texture reference (0x27)
  case 0x27: {
    for (u32 i = 0; i < MaxIndStages; i++) {
      g_gxState.indStages[i].texMapId = static_cast<GXTexMapID>(bp_get(value, 3, i * 6));
      g_gxState.indStages[i].texCoordId = static_cast<GXTexCoordID>(bp_get(value, 3, i * 6 + 3));
    }
    mark_pipeline_state_dirty();
    break;
  }

  // TEV order / tref (0x28-0x2F) - 2 stages per register
  case 0x28:
  case 0x29:
  case 0x2A:
  case 0x2B:
  case 0x2C:
  case 0x2D:
  case 0x2E:
  case 0x2F: {
    u32 idx = regId - 0x28;
    u32 stage0 = idx * 2;
    u32 stage1 = idx * 2 + 1;

    // Channel ID reverse mapping from hardware to GX
    static const GXChannelID r2c[] = {GX_COLOR0A0, GX_COLOR1A1,   GX_COLOR0A0,    GX_COLOR1A1,
                                      GX_COLOR0A0, GX_ALPHA_BUMP, GX_ALPHA_BUMPN, GX_COLOR_ZERO};

    if (stage0 < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage0];
      s.texMapId = static_cast<GXTexMapID>(bp_get(value, 3, 0));
      s.texCoordId = static_cast<GXTexCoordID>(bp_get(value, 3, 3));
      // bit 6 = tex enable
      if (!bp_get(value, 1, 6)) {
        s.texMapId = GX_TEXMAP_NULL;
      }
      u32 chanHw = bp_get(value, 3, 7);
      s.channelId = (chanHw < 8) ? r2c[chanHw] : GX_COLOR_NULL;
    }
    if (stage1 < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage1];
      s.texMapId = static_cast<GXTexMapID>(bp_get(value, 3, 12));
      s.texCoordId = static_cast<GXTexCoordID>(bp_get(value, 3, 15));
      if (!bp_get(value, 1, 18)) {
        s.texMapId = GX_TEXMAP_NULL;
      }
      u32 chanHw = bp_get(value, 3, 19);
      s.channelId = (chanHw < 8) ? r2c[chanHw] : GX_COLOR_NULL;
    }
    mark_pipeline_state_dirty();
    break;
  }

  // Z mode (0x40)
  case 0x40: {
    g_gxState.depthCompare = bp_get(value, 1, 0) != 0;
    g_gxState.depthFunc = static_cast<GXCompare>(bp_get(value, 3, 1));
    g_gxState.depthUpdate = bp_get(value, 1, 4) != 0;
    mark_pipeline_state_dirty();
    break;
  }

  // Blend mode / cmode0 (0x41)
  case 0x41: {
    bool blendEn = bp_get(value, 1, 0) != 0;
    bool logicEn = bp_get(value, 1, 1) != 0;
    bool dither = bp_get(value, 1, 2) != 0;
    g_gxState.colorUpdate = bp_get(value, 1, 3) != 0;
    g_gxState.alphaUpdate = bp_get(value, 1, 4) != 0;
    g_gxState.blendFacDst = static_cast<GXBlendFactor>(bp_get(value, 3, 5));
    g_gxState.blendFacSrc = static_cast<GXBlendFactor>(bp_get(value, 3, 8));
    bool subtract = bp_get(value, 1, 11) != 0;
    g_gxState.blendOp = static_cast<GXLogicOp>(bp_get(value, 4, 12));

    if (subtract) {
      g_gxState.blendMode = GX_BM_SUBTRACT;
    } else if (blendEn) {
      g_gxState.blendMode = GX_BM_BLEND;
    } else if (logicEn) {
      g_gxState.blendMode = GX_BM_LOGIC;
    } else {
      g_gxState.blendMode = GX_BM_NONE;
    }
    mark_pipeline_state_dirty();
    break;
  }

  // Dst alpha / cmode1 (0x42)
  case 0x42: {
    u8 alpha = bp_get(value, 8, 0);
    bool enabled = bp_get(value, 1, 8) != 0;
    g_gxState.dstAlpha = enabled ? alpha : UINT32_MAX;
    g_gxState.pixelFmt = decode_pixel_fmt(g_gxState.bpRegCache[0x43], value);
    mark_pipeline_state_dirty();
    break;
  }

  // PE control (0x43) - pixel format, z format, zcomp location
  case 0x43: {
    g_gxState.pixelFmt = decode_pixel_fmt(value, g_gxState.bpRegCache[0x42]);
    g_gxState.zFmt = static_cast<GXZFmt16>(bp_get(value, 3, 3));
    g_gxState.zCompLocBeforeTex = bp_get(value, 1, 6) != 0;
    mark_pipeline_state_dirty();
    break;
  }
  case 0x44:
    g_gxState.fieldMask = value & 0x3u;
    break;

  // Bounding box clear/update registers (0x55, 0x56)
  case 0x55:
  case 0x56: {
    const u32 offset = (regId & 2u);
    g_gxState.boundingBox[offset] = static_cast<u16>(value & 0x3ffu);
    g_gxState.boundingBox[offset + 1] = static_cast<u16>((value >> 10) & 0x3ffu);
    break;
  }
  case 0x58:
    g_gxState.revBits = value & 0x00FFFFFFu;
    break;

  // Scissor box offset (0x59)
  case 0x59: {
    g_gxState.scissorOffsetX = static_cast<s32>(((value & 0x3ffu) << 1) - 0x156u);
    g_gxState.scissorOffsetY = static_cast<s32>(((value & 0xffc00u) >> 9) - 0x156u);
    set_logical_scissor(g_gxState.logicalScissor);
    break;
  }

  // TLUT load address / execute (0x64, 0x65)
  case 0x64:
    break;
  case 0x65: {
    const auto idx = bp_get(value, 10, 0);
    if (idx < MaxTluts) {
      auto& slot = g_gxState.loadedTluts[idx];
      slot.loadTlut0 = g_gxState.bpRegCache[0x64];
      slot.numEntries = static_cast<u16>(bp_get(value, 10, 10) + 1);
    }
    break;
  }
  case 0x68:
    g_gxState.fieldMode = value & 0x1u;
    break;

  // Alpha compare (0xF3)
  case 0xF3: {
    g_gxState.alphaCompare.ref0 = bp_get(value, 8, 0);
    g_gxState.alphaCompare.ref1 = bp_get(value, 8, 8);
    g_gxState.alphaCompare.comp0 = static_cast<GXCompare>(bp_get(value, 3, 16));
    g_gxState.alphaCompare.comp1 = static_cast<GXCompare>(bp_get(value, 3, 19));
    g_gxState.alphaCompare.op = static_cast<GXAlphaOp>(bp_get(value, 2, 22));
    mark_pipeline_state_dirty();
    break;
  }

  // TEV K color/alpha select (0xF6-0xFD)
  case 0xF6:
  case 0xF7:
  case 0xF8:
  case 0xF9:
  case 0xFA:
  case 0xFB:
  case 0xFC:
  case 0xFD: {
    u32 kselIdx = regId - 0xF6;
    // Swap table entries (packed into pairs of ksel registers)
    if (kselIdx < MaxTevSwap * 2) {
      u32 swapIdx = kselIdx / 2;
      if (kselIdx & 1) {
        g_gxState.tevSwapTable[swapIdx].blue = static_cast<GXTevColorChan>(bp_get(value, 2, 0));
        g_gxState.tevSwapTable[swapIdx].alpha = static_cast<GXTevColorChan>(bp_get(value, 2, 2));
      } else {
        g_gxState.tevSwapTable[swapIdx].red = static_cast<GXTevColorChan>(bp_get(value, 2, 0));
        g_gxState.tevSwapTable[swapIdx].green = static_cast<GXTevColorChan>(bp_get(value, 2, 2));
      }
    }
    // K color/alpha selection for 2 stages per register
    u32 stage0 = kselIdx * 2;
    u32 stage1 = kselIdx * 2 + 1;
    if (stage0 < MaxTevStages) {
      g_gxState.tevStages[stage0].kcSel = static_cast<GXTevKColorSel>(bp_get(value, 5, 4));
      g_gxState.tevStages[stage0].kaSel = static_cast<GXTevKAlphaSel>(bp_get(value, 5, 9));
    }
    if (stage1 < MaxTevStages) {
      g_gxState.tevStages[stage1].kcSel = static_cast<GXTevKColorSel>(bp_get(value, 5, 14));
      g_gxState.tevStages[stage1].kaSel = static_cast<GXTevKAlphaSel>(bp_get(value, 5, 19));
    }
    mark_pipeline_state_dirty();
    break;
  }

  // Fog A/B parameters (0xEE-0xF0)
  // FOG0 (0xEE): A parameter - sign(1)|exp(8)|mantissa(11) partial IEEE 754 float
  case 0xEE: {
    g_gxState.fog.fog0Raw = value;
    u32 a_mant = bp_get(value, 11, 0);
    u32 a_exp = bp_get(value, 8, 11);
    u32 a_sign = bp_get(value, 1, 19);
    u32 a_bits = (a_sign << 31) | (a_exp << 23) | (a_mant << 12);
    std::memcpy(&g_gxState.fog.aRaw, &a_bits, sizeof(g_gxState.fog.aRaw));
    u32 b_s = g_gxState.fog.fog2Raw & 0x1F;
    g_gxState.fog.a = std::ldexp(g_gxState.fog.aRaw, static_cast<int>(b_s));
    g_gxState.stateDirty = true;
    break;
  }
  // FOG1 (0xEF): B mantissa (24-bit)
  case 0xEF: {
    g_gxState.fog.fog1Raw = value;
    g_gxState.fog.bMagnitude = bp_get(value, 24, 0);
    u32 b_s = g_gxState.fog.fog2Raw & 0x1F;
    float B_mant = static_cast<float>(g_gxState.fog.bMagnitude) / 8388638.0f;
    g_gxState.fog.b = std::ldexp(B_mant, static_cast<int>(b_s) - 1);
    g_gxState.stateDirty = true;
    break;
  }
  // FOG2 (0xF0): B shift/exponent (5-bit)
  case 0xF0: {
    g_gxState.fog.fog2Raw = value;
    u32 b_s = bp_get(value, 5, 0);
    g_gxState.fog.bShift = b_s;
    u32 a_mant = bp_get(g_gxState.fog.fog0Raw, 11, 0);
    u32 a_exp = bp_get(g_gxState.fog.fog0Raw, 8, 11);
    u32 a_sign = bp_get(g_gxState.fog.fog0Raw, 1, 19);
    u32 a_bits = (a_sign << 31) | (a_exp << 23) | (a_mant << 12);
    std::memcpy(&g_gxState.fog.aRaw, &a_bits, sizeof(g_gxState.fog.aRaw));
    g_gxState.fog.a = std::ldexp(g_gxState.fog.aRaw, static_cast<int>(b_s));
    g_gxState.fog.bMagnitude = bp_get(g_gxState.fog.fog1Raw, 24, 0);
    float B_mant = static_cast<float>(g_gxState.fog.bMagnitude) / 8388638.0f;
    g_gxState.fog.b = std::ldexp(B_mant, static_cast<int>(b_s) - 1);
    g_gxState.stateDirty = true;
    break;
  }

  // Fog type + C parameter from FOG3 (0xF1)
  case 0xF1: {
    const u32 fogFunc = bp_get(value, 3, 21);
    const u32 fogProj = bp_get(value, 1, 20);
    GXFogType fogType = static_cast<GXFogType>(fogFunc | (fogProj << 3));
    g_gxState.fog.type = fogType;
    // Decode C parameter (same partial float encoding as A)
    u32 c_mant = bp_get(value, 11, 0);
    u32 c_exp = bp_get(value, 8, 11);
    u32 c_sign = bp_get(value, 1, 19);
    u32 c_bits = (c_sign << 31) | (c_exp << 23) | (c_mant << 12);
    std::memcpy(&g_gxState.fog.c, &c_bits, sizeof(g_gxState.fog.c));
    mark_pipeline_state_dirty();
    break;
  }

  // Fog color from FOGCLR (0xF2)
  case 0xF2: {
    u8 b = bp_get(value, 8, 0);
    u8 g = bp_get(value, 8, 8);
    u8 r = bp_get(value, 8, 16);
    g_gxState.fog.color = {
        static_cast<float>(r) / 255.f,
        static_cast<float>(g) / 255.f,
        static_cast<float>(b) / 255.f,
        1.f,
    };
    g_gxState.stateDirty = true;
    break;
  }

  // TEV and K color registers (0xE0-0xE7): even are RA, odd are BG.
  // Bit 23 selects a K color register over a TEV color register.
  case 0xE0:
  case 0xE1:
  case 0xE2:
  case 0xE3:
  case 0xE4:
  case 0xE5:
  case 0xE6:
  case 0xE7: {
    u32 idx = (regId - 0xE0) / 2;
    bool isRA = (regId & 1) == 0;
    bool isKColor = bp_get(value, 1, 23) != 0;

    if (isKColor) {
      // K color register (8-bit components)
      if (idx < GX_MAX_KCOLOR) {
        auto& kc = g_gxState.kcolors[idx];
        if (isRA) {
          kc[0] = static_cast<float>(bp_get(value, 8, 0)) / 255.f;  // R
          kc[3] = static_cast<float>(bp_get(value, 8, 12)) / 255.f; // A
        } else {
          kc[2] = static_cast<float>(bp_get(value, 8, 0)) / 255.f;  // B
          kc[1] = static_cast<float>(bp_get(value, 8, 12)) / 255.f; // G
        }
        g_gxState.stateDirty = true;
      }
    } else {
      // TEV color register (11-bit signed components)
      if (idx < MaxTevRegs) {
        auto& cr = g_gxState.colorRegs[idx];
        if (isRA) {
          // 11-bit signed: sign-extend from 11 bits
          s32 r = bp_get(value, 11, 0);
          if (r & 0x400)
            r |= ~0x7FF; // sign extend
          s32 a = bp_get(value, 11, 12);
          if (a & 0x400)
            a |= ~0x7FF;
          cr[0] = static_cast<float>(r) / 255.f;
          cr[3] = static_cast<float>(a) / 255.f;
        } else {
          s32 b = bp_get(value, 11, 0);
          if (b & 0x400)
            b |= ~0x7FF;
          s32 g = bp_get(value, 11, 12);
          if (g & 0x400)
            g |= ~0x7FF;
          cr[2] = static_cast<float>(b) / 255.f;
          cr[1] = static_cast<float>(g) / 255.f;
        }
        g_gxState.stateDirty = true;
      }
    }
    break;
  }

  // Indirect texture matrices (0x06-0x0E), three consecutive registers per 3x2 matrix:
  // matrix 0 at 0x06, matrix 1 at 0x09, matrix 2 at 0x0C.
  case 0x06:
  case 0x07:
  case 0x08:
  case 0x09:
  case 0x0A:
  case 0x0B:
  case 0x0C:
  case 0x0D:
  case 0x0E: {
    u32 idx = (regId - 0x06) / 3;    // matrix index (0-2)
    u32 column = (regId - 0x06) % 3; // column index (0-2)
    auto& info = g_gxState.indTexMtxs[idx];

    // Decode one packed matrix column: [m[0][column], m[1][column]].
    s32 col0 = bp_get(value, 11, 0);
    if (col0 & 0x400)
      col0 |= ~0x7FF; // sign-extend from 11 bits
    s32 col1 = bp_get(value, 11, 11);
    if (col1 & 0x400)
      col1 |= ~0x7FF;

    auto& packedColumn = column == 0 ? info.mtx.m0 : (column == 1 ? info.mtx.m1 : info.mtx.m2);
    packedColumn.x = static_cast<float>(col0) / 1024.0f;
    packedColumn.y = static_cast<float>(col1) / 1024.0f;

    // Accumulate the indirect matrix scale exponent. The SDK writes two bits per column, but the
    // hardware ignores the third column's top bit, leaving 5 bits for adjScale = scaleExp + 17.
    u32 scaleBits = bp_get(value, 2, 22);
    u32 shift = column * 2;
    if (column == 2) {
      info.adjScaleRaw = (info.adjScaleRaw & ~(1u << shift)) | ((scaleBits & 1u) << shift);
    } else {
      info.adjScaleRaw = (info.adjScaleRaw & ~(3u << shift)) | (scaleBits << shift);
    }
    info.scaleExp = static_cast<s8>(info.adjScaleRaw) - 17;

    g_gxState.stateDirty = true;
    break;
  }

  // SU texture coordinate scale registers (0x30-0x3F): even (suTs0) carry S-axis scale, bias, cyl
  // wrap and line/point offset; odd (suTs1) carry the T-axis equivalents.
  case 0x30:
  case 0x31:
  case 0x32:
  case 0x33:
  case 0x34:
  case 0x35:
  case 0x36:
  case 0x37:
  case 0x38:
  case 0x39:
  case 0x3A:
  case 0x3B:
  case 0x3C:
  case 0x3D:
  case 0x3E:
  case 0x3F: {
    u32 coordIdx = (regId - 0x30) / 2;
    bool isT = (regId & 1) != 0;
    auto& tcs = g_gxState.texCoordScales[coordIdx];
    if (isT) {
      tcs.scaleT = static_cast<u16>(bp_get(value, 16, 0));
      tcs.biasT = bp_get(value, 1, 16) != 0;
      tcs.cylWrapT = bp_get(value, 1, 17) != 0;
    } else {
      tcs.scaleS = static_cast<u16>(bp_get(value, 16, 0));
      tcs.biasS = bp_get(value, 1, 16) != 0;
      tcs.cylWrapS = bp_get(value, 1, 17) != 0;
      tcs.lineOffset = bp_get(value, 1, 18) != 0;
      tcs.pointOffset = bp_get(value, 1, 19) != 0;
    }
    g_gxState.stateDirty = true;
    break;
  }

  // Copy clear color (0x4F-0x50) and depth (0x51)
  case 0x49: {
    g_gxState.dispCopySrc.x = static_cast<int32_t>(bp_get(value, 10, 0));
    g_gxState.dispCopySrc.y = static_cast<int32_t>(bp_get(value, 10, 10));
    break;
  }
  case 0x4A: {
    g_gxState.dispCopySrc.width = static_cast<int32_t>(bp_get(value, 10, 0) + 1u);
    g_gxState.dispCopySrc.height = static_cast<int32_t>(bp_get(value, 10, 10) + 1u);
    break;
  }
  case 0x4D: {
    g_gxState.dispCopyDstWidth = static_cast<u16>(bp_get(value, 10, 0) << 4);
    break;
  }
  case 0x4E: {
    const u32 iScale = bp_get(value, 9, 0);
    if (iScale != 0) {
      g_gxState.dispCopyYScale = 256.f / static_cast<float>(iScale);
    }
    break;
  }
  case 0x4F: {
    u8 r = bp_get(value, 8, 0);
    u8 a = bp_get(value, 8, 8);
    g_gxState.clearColor[0] = static_cast<float>(r) / 255.f;
    g_gxState.clearColor[3] = static_cast<float>(a) / 255.f;
    g_gxState.stateDirty = true;
    break;
  }
  case 0x50: {
    u8 b = bp_get(value, 8, 0);
    u8 g = bp_get(value, 8, 8);
    g_gxState.clearColor[2] = static_cast<float>(b) / 255.f;
    g_gxState.clearColor[1] = static_cast<float>(g) / 255.f;
    g_gxState.stateDirty = true;
    break;
  }
  case 0x51: {
    g_gxState.clearDepth = bp_get(value, 24, 0);
    g_gxState.stateDirty = true;
    break;
  }
  case 0xF4: {
    g_gxState.zTextureBias = value & 0x00FFFFFFu;
    mark_pipeline_state_dirty();
    break;
  }
  case 0xF5: {
    g_gxState.zTextureFmt = static_cast<u8>(bp_get(value, 2, 0));
    g_gxState.zTextureOp = static_cast<GXZTexOp>(bp_get(value, 2, 2));
    mark_pipeline_state_dirty();
    break;
  }
  case 0x52: {
    g_gxState.copyClamp = static_cast<GXFBClamp>(bp_get(value, 2, 0));
    g_gxState.texCopyFmt = static_cast<GXTexFmt>(bp_get(value, 4, 3));
    g_gxState.dispCopyGamma = static_cast<GXGamma>(bp_get(value, 2, 7));
    g_gxState.texCopyHalfScale = bp_get(value, 1, 9) != 0;
    g_gxState.dispCopyFrame2Field = bp_get(value, 2, 12);
    break;
  }
  case 0x53: {
    g_gxState.copyFilterVFilter[0] = static_cast<u8>(bp_get(value, 6, 0));
    g_gxState.copyFilterVFilter[1] = static_cast<u8>(bp_get(value, 6, 6));
    g_gxState.copyFilterVFilter[2] = static_cast<u8>(bp_get(value, 6, 12));
    g_gxState.copyFilterVFilter[3] = static_cast<u8>(bp_get(value, 6, 18));
    refresh_copy_filter_flags();
    break;
  }
  case 0x54: {
    g_gxState.copyFilterVFilter[4] = static_cast<u8>(bp_get(value, 6, 0));
    g_gxState.copyFilterVFilter[5] = static_cast<u8>(bp_get(value, 6, 6));
    g_gxState.copyFilterVFilter[6] = static_cast<u8>(bp_get(value, 6, 12));
    refresh_copy_filter_flags();
    break;
  }
  case 0xE8:
  case 0xE9:
  case 0xEA:
  case 0xEB:
  case 0xEC:
  case 0xED:
    g_gxState.fogRange[regId - 0xE8] = value & 0x00FFFFFFu;
    mark_pipeline_state_dirty();
    break;

  default:
    if (const auto mapping = decode_tex_bp_reg(regId); mapping.has_value()) {
      auto& slot = g_gxState.loadedTextures[mapping->texMapId];
      bool changed = false;
      switch (mapping->kind) {
      case TexBpRegMapping::Kind::Mode0:
        changed = slot.mode0 != value;
        if (changed) {
          slot.mode0 = value;
        }
        break;
      case TexBpRegMapping::Kind::Mode1:
        changed = slot.mode1 != value;
        if (changed) {
          slot.mode1 = value;
        }
        break;
      case TexBpRegMapping::Kind::Image0:
        changed = slot.image0 != value;
        if (changed) {
          slot.image0 = value;
          slot.mWidth = 0;
          slot.mHeight = 0;
          slot.mFormat = gfx::InvalidTextureFormat;
        }
        break;
      case TexBpRegMapping::Kind::Image3:
        changed = slot.image3 != value;
        if (changed) {
          slot.image3 = value;
        }
        break;
      case TexBpRegMapping::Kind::Tlut:
        // TLUT region's TMEM offset
        break;
      case TexBpRegMapping::Kind::Image1:
      case TexBpRegMapping::Kind::Image2:
        // GXTexRegion regs
        break;
      }
      if (changed) {
        g_gxState.stateDirty = true;
      }
    } else {
#ifndef NDEBUG
      Log.debug("Unhandled BP register 0x{:02X} (value 0x{:06X})", regId, value & 0xFFFFFF);
#endif
    }
    break;
  }
}

extern "C" uint32_t GXRawDrawTextureMask(GXVtxFmt fmt) {
  // The shadow and the stream buffer belong to the writer. A decoder-thread
  // caller (host callback) relies on the writer having published its dirty
  // state before the callback entered the stream (GXAuroraPublishDirtyState).
  if (!on_decoder_thread() && __gx->dirtyState != 0) __GXSetDirtyState();
  if (async_enabled() && !on_decoder_thread()) {
    // Decoder state is only current on the decoder thread; a producer caller
    // pays a full sync (the HLE issues this from a host callback instead).
    static u32 producerMaskCalls = 0;
    if (producerMaskCalls++ < 4)
      Log.warn("GXRawDrawTextureMask on the producer with the asynchronous decoder: syncing");
  }
  drain();
  std::lock_guard gpuLock(aurora::renderer_gpu_mutex());
  PipelineConfig config{};
  populate_pipeline_config(config, GX_TRIANGLES, fmt);
  const auto info = build_shader_info(config.shaderConfig);
  return static_cast<uint32_t>((info.sampledTextures | info.sampledIndTextures).to_ulong());
}

extern "C" void GXApplyBPReg(u8 reg, u32 value) {
  if (async_enabled() && !on_decoder_thread()) {
    // The same bytes the SDK writes; the decoder thread applies them in order.
    write_u8(GX_LOAD_BP_REG);
    write_u32((static_cast<u32>(reg) << 24) | (value & 0x00FFFFFFu));
    commit();
    return;
  }
  // Direct BP writes bypass the normal FIFO decoder's worker barrier and GPU
  // mutex. Even an empty producer FIFO must wait for the next pass/staging
  // preparation before mutating shared recording state.
  drain();
  std::lock_guard gpuLock(aurora::renderer_gpu_mutex());
  handle_bp((static_cast<u32>(reg) << 24) | (value & 0x00FFFFFFu), true);
}

static bool cacheable_cp_register(u8 addr) {
  return addr == 0x30 || addr == 0x40 || addr == 0x50 || addr == 0x60 || (addr >= 0x70 && addr <= 0x97);
}

static std::array<u32, 0x100> s_cpRegisterCache{};
static std::array<bool, 0x100> s_cpRegisterCacheValid{};

void reset_cp_register_cache() {
  s_cpRegisterCache.fill(0);
  s_cpRegisterCacheValid.fill(false);
}

static bool cp_register_write_unchanged(u8 addr, u32 value) {
  if (!cacheable_cp_register(addr)) return false;

  if (s_cpRegisterCacheValid[addr] && s_cpRegisterCache[addr] == value) {
    return true;
  }
  s_cpRegisterCacheValid[addr] = true;
  s_cpRegisterCache[addr] = value;
  return false;
}

// GXAuroraVtxStateEpoch belongs to the writer. Loads decoded on the
// asynchronous decoder thread were already counted at GXCallDisplayList.
static inline void bump_vtx_state_epoch() {
  if (!on_decoder_thread()) ++g_gxVtxStateEpoch;
}

// CP register handler - decodes CP register writes and updates g_gxState
static void handle_cp(u8 addr, u32 value, bool bigEndian) {
  if (cp_register_write_unchanged(addr, value)) return;

  switch (addr) {
  // VCD low (0x50)
  case 0x50: {
    auto& vd = g_gxState.vtxDesc;
    auto& svd = g_gxState.sourceVtxDesc;
    vd[GX_VA_PNMTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 0));
    vd[GX_VA_TEX0MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 1));
    vd[GX_VA_TEX1MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 2));
    vd[GX_VA_TEX2MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 3));
    vd[GX_VA_TEX3MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 4));
    vd[GX_VA_TEX4MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 5));
    vd[GX_VA_TEX5MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 6));
    vd[GX_VA_TEX6MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 7));
    vd[GX_VA_TEX7MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 8));
    vd[GX_VA_POS] = static_cast<GXAttrType>(bp_get(value, 2, 9));
    vd[GX_VA_NRM] = static_cast<GXAttrType>(bp_get(value, 2, 11));
    vd[GX_VA_CLR0] = static_cast<GXAttrType>(bp_get(value, 2, 13));
    vd[GX_VA_CLR1] = static_cast<GXAttrType>(bp_get(value, 2, 15));
    for (int attr = GX_VA_PNMTXIDX; attr <= GX_VA_CLR1; ++attr) {
      svd[attr] = vd[attr];
    }
    mark_pipeline_state_dirty();
    g_gxState.clearVtxSizeCache();
    bump_vtx_state_epoch();
    break;
  }

  // VCD high (0x60)
  case 0x60: {
    auto& vd = g_gxState.vtxDesc;
    auto& svd = g_gxState.sourceVtxDesc;
    vd[GX_VA_TEX0] = static_cast<GXAttrType>(bp_get(value, 2, 0));
    vd[GX_VA_TEX1] = static_cast<GXAttrType>(bp_get(value, 2, 2));
    vd[GX_VA_TEX2] = static_cast<GXAttrType>(bp_get(value, 2, 4));
    vd[GX_VA_TEX3] = static_cast<GXAttrType>(bp_get(value, 2, 6));
    vd[GX_VA_TEX4] = static_cast<GXAttrType>(bp_get(value, 2, 8));
    vd[GX_VA_TEX5] = static_cast<GXAttrType>(bp_get(value, 2, 10));
    vd[GX_VA_TEX6] = static_cast<GXAttrType>(bp_get(value, 2, 12));
    vd[GX_VA_TEX7] = static_cast<GXAttrType>(bp_get(value, 2, 14));
    for (int attr = GX_VA_TEX0; attr <= GX_VA_TEX7; ++attr) {
      svd[attr] = vd[attr];
    }
    mark_pipeline_state_dirty();
    g_gxState.clearVtxSizeCache();
    bump_vtx_state_epoch();
    break;
  }

  // Matrix index A (0x30)
  case 0x30: {
    g_gxState.currentPnMtx = bp_get(value, 6, 0) / 3;
    for (u32 i = 0; i < 4 && i < MaxTexCoord; i++) {
      auto texMtx = static_cast<GXTexMtx>(bp_get(value, 6, 6 + i * 6));
      assert(texMtx >= 0 && texMtx <= GXTexMtx::GX_IDENTITY);
      g_gxState.tcgs[i].mtx = texMtx;
    }
    // Same matrix indices as XF 0x18, written from a different bank.
    g_gxState.invalidateXfReg(0x18);
    mark_pipeline_state_dirty();
    break;
  }

  // Matrix index B (0x40)
  case 0x40: {
    for (u32 i = 0; i < 4 && (i + 4) < MaxTexCoord; i++) {
      auto texMtx = static_cast<GXTexMtx>(bp_get(value, 6, i * 6));
      assert(texMtx >= 0 && texMtx <= GXTexMtx::GX_IDENTITY);
      g_gxState.tcgs[i + 4].mtx = texMtx;
    }
    // Same matrix indices as XF 0x19, written from a different bank.
    g_gxState.invalidateXfReg(0x19);
    mark_pipeline_state_dirty();
    break;
  }

  default:
    // VAT A registers (0x70-0x77)
    if (addr >= 0x70 && addr <= 0x77) {
      u32 fmt = addr - 0x70;
      auto& vf = g_gxState.vtxFmts[fmt];
      vf.attrs[GX_VA_POS].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 0));
      vf.attrs[GX_VA_POS].type = static_cast<GXCompType>(bp_get(value, 3, 1));
      vf.attrs[GX_VA_POS].frac = static_cast<u8>(bp_get(value, 5, 4));
      vf.attrs[GX_VA_NRM].type = static_cast<GXCompType>(bp_get(value, 3, 10));
      if (bp_get(value, 1, 31) != 0) {
        vf.attrs[GX_VA_NRM].cnt = GX_NRM_NBT3;
      } else {
        vf.attrs[GX_VA_NRM].cnt = bp_get(value, 1, 9) != 0 ? GX_NRM_NBT : GX_NRM_XYZ;
      }
      vf.attrs[GX_VA_NRM].frac = normal_frac_bits(vf.attrs[GX_VA_NRM].type);
      vf.attrs[GX_VA_CLR0].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 13));
      vf.attrs[GX_VA_CLR0].type = static_cast<GXCompType>(bp_get(value, 3, 14));
      vf.attrs[GX_VA_CLR0].frac = 0;
      vf.attrs[GX_VA_CLR1].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 17));
      vf.attrs[GX_VA_CLR1].type = static_cast<GXCompType>(bp_get(value, 3, 18));
      vf.attrs[GX_VA_CLR1].frac = 0;
      vf.attrs[GX_VA_TEX0].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 21));
      vf.attrs[GX_VA_TEX0].type = static_cast<GXCompType>(bp_get(value, 3, 22));
      vf.attrs[GX_VA_TEX0].frac = static_cast<u8>(bp_get(value, 5, 25));
      mark_pipeline_state_dirty();
      g_gxState.clearVtxSizeCache();
      bump_vtx_state_epoch();
    }
    // VAT B registers (0x80-0x87)
    else if (addr >= 0x80 && addr <= 0x87) {
      u32 fmt = addr - 0x80;
      auto& vf = g_gxState.vtxFmts[fmt];
      vf.attrs[GX_VA_TEX1].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 0));
      vf.attrs[GX_VA_TEX1].type = static_cast<GXCompType>(bp_get(value, 3, 1));
      vf.attrs[GX_VA_TEX1].frac = static_cast<u8>(bp_get(value, 5, 4));
      vf.attrs[GX_VA_TEX2].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 9));
      vf.attrs[GX_VA_TEX2].type = static_cast<GXCompType>(bp_get(value, 3, 10));
      vf.attrs[GX_VA_TEX2].frac = static_cast<u8>(bp_get(value, 5, 13));
      vf.attrs[GX_VA_TEX3].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 18));
      vf.attrs[GX_VA_TEX3].type = static_cast<GXCompType>(bp_get(value, 3, 19));
      vf.attrs[GX_VA_TEX3].frac = static_cast<u8>(bp_get(value, 5, 22));
      vf.attrs[GX_VA_TEX4].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 27));
      vf.attrs[GX_VA_TEX4].type = static_cast<GXCompType>(bp_get(value, 3, 28));
      // TEX4 frac is in VAT C
      mark_pipeline_state_dirty();
      g_gxState.clearVtxSizeCache();
      bump_vtx_state_epoch();
    }
    // VAT C registers (0x90-0x97)
    else if (addr >= 0x90 && addr <= 0x97) {
      u32 fmt = addr - 0x90;
      auto& vf = g_gxState.vtxFmts[fmt];
      vf.attrs[GX_VA_TEX4].frac = static_cast<u8>(bp_get(value, 5, 0));
      vf.attrs[GX_VA_TEX5].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 5));
      vf.attrs[GX_VA_TEX5].type = static_cast<GXCompType>(bp_get(value, 3, 6));
      vf.attrs[GX_VA_TEX5].frac = static_cast<u8>(bp_get(value, 5, 9));
      vf.attrs[GX_VA_TEX6].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 14));
      vf.attrs[GX_VA_TEX6].type = static_cast<GXCompType>(bp_get(value, 3, 15));
      vf.attrs[GX_VA_TEX6].frac = static_cast<u8>(bp_get(value, 5, 18));
      vf.attrs[GX_VA_TEX7].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 23));
      vf.attrs[GX_VA_TEX7].type = static_cast<GXCompType>(bp_get(value, 3, 24));
      vf.attrs[GX_VA_TEX7].frac = static_cast<u8>(bp_get(value, 5, 27));
      mark_pipeline_state_dirty();
      g_gxState.clearVtxSizeCache();
      bump_vtx_state_epoch();
    }
    // Array base addresses (0xA0-0xAF)
    else if (addr >= 0xA0 && addr <= 0xAF) {
      Log.error("CP_REG_ARRAYBASE_ID is not supported on Aurora. Use GX_LOAD_AURORA_ARRAYBASE instead.");
    }
    // Array strides (0xB0-0xBF)
    else if (addr >= 0xB0 && addr <= 0xBF) {
      u32 attrIdx = addr - 0xB0 + GX_VA_POS;
      if (attrIdx < GX_VA_MAX_ATTR) {
        auto& array = g_gxState.arrays[attrIdx];
        const auto newStride = static_cast<u8>(value);
        if (array.stride != newStride) {
          array.stride = newStride;
          mark_pipeline_state_dirty();
        }
      }
    }
    break;
  }
}

// XF register handler - decodes XF (transform unit) register writes and updates g_gxState
static void handle_xf(const u8* data, u32& pos, u32 size, bool bigEndian) {
  // These bounds must hold in release too: CHECK() is a no-op under NDEBUG, so relying on it alone let a truncated guest display list read past `data`.
  if (pos > size || size - pos < 4) UNLIKELY {
      CHECK(false, "XF header read overrun");
      pos = size;
      return;
    }
  u32 header = read_u32(data + pos, bigEndian);
  pos += 4;

  u32 count = ((header >> 16) & 0xFFFF) + 1;
  u32 addr = header & 0xFFFF;
  u32 dataBytes = count * 4;
  // Log.warn("  xf: addr {:04x} count {} dataBytes {} pos {} -> {}", addr, count, dataBytes, pos, pos + dataBytes);
  if (size - pos < dataBytes) UNLIKELY {
      CHECK(false, "XF data read overrun: need {} bytes at pos {}", dataBytes, pos);
      pos = size;
      return;
    }

  const u8* xfData = data + pos;

  if (copy_xf_data(addr, xfData, count, bigEndian)) {
    // copy_xf_data handled everything.
  } else if (addr >= 0x1000) {
    // XF registers (0x1000+)
    u32 xfAddr = addr - 0x1000;
    bool viewportUpdated = false;
    bool projectionUpdated = false;
    for (u32 i = 0; i < count; i++) {
      u32 reg = xfAddr + i;
      u32 val = read_u32(xfData + i * 4, bigEndian);

      // Skip register writes that decode to state we already hold.
      const bool cacheable = reg < g_gxState.xfRegCache.size();
      const bool unchanged = cacheable && g_gxState.xfRegMatches(reg, val);
      if (cacheable) g_gxState.storeXfReg(reg, val);
      // Viewport (0x1A-0x1F) and projection (0x20-0x26) keep their unconditional apply below; only the banks that already had skip semantics and the TexGen bank drop out here.
      if (unchanged && (reg <= 0x19 || reg >= 0x3F)) continue;

      switch (reg) {
      case 0x00:
        g_gxState.xfError = val;
        break;
      case 0x08:
        // XF vertex specs (numColors, numNormals, numTexCoords) - informational
        break;
      case 0x09:
        // numChans
        g_gxState.numChans = val;
        g_gxState.stateDirty = true;
        break;
      case 0x0A:
        // Ambient color 0
        g_gxState.colorChannelState[GX_COLOR0].ambColor = unpack_color(val);
        g_gxState.colorChannelState[GX_ALPHA0].ambColor = unpack_color(val);
        g_gxState.stateDirty = true;
        break;
      case 0x0B:
        // Ambient color 1
        g_gxState.colorChannelState[GX_COLOR1].ambColor = unpack_color(val);
        g_gxState.colorChannelState[GX_ALPHA1].ambColor = unpack_color(val);
        g_gxState.stateDirty = true;
        break;
      case 0x0C:
        // Material color 0
        g_gxState.colorChannelState[GX_COLOR0].matColor = unpack_color(val);
        g_gxState.colorChannelState[GX_ALPHA0].matColor = unpack_color(val);
        g_gxState.stateDirty = true;
        break;
      case 0x0D:
        // Material color 1
        g_gxState.colorChannelState[GX_COLOR1].matColor = unpack_color(val);
        g_gxState.colorChannelState[GX_ALPHA1].matColor = unpack_color(val);
        g_gxState.stateDirty = true;
        break;
      case 0x0E:
      case 0x0F:
      case 0x10:
      case 0x11: {
        // Channel control registers
        u32 chanId = reg - 0x0E;
        if (chanId < MaxColorChannels) {
          auto& chan = g_gxState.colorChannelConfig[chanId];
          chan.matSrc = static_cast<GXColorSrc>(bp_get(val, 1, 0));
          chan.lightingEnabled = bp_get(val, 1, 1) != 0;
          u32 lightsLo = bp_get(val, 4, 2);
          chan.ambSrc = static_cast<GXColorSrc>(bp_get(val, 1, 6));
          chan.diffFn = static_cast<GXDiffuseFn>(bp_get(val, 2, 7));
          u32 lightsHi = bp_get(val, 4, 11);
          switch (bp_get(val, 2, 9)) {
          case 1:
            chan.attnFn = GX_AF_SPEC;
            break;
          case 3:
            chan.attnFn = GX_AF_SPOT;
            break;
          case 0:
          case 2:
          default:
            chan.attnFn = GX_AF_NONE;
            break;
          }
          u32 lightMask = lightsLo | (lightsHi << 4);
          g_gxState.colorChannelState[chanId].lightMask = GX::LightMask{lightMask};
          mark_pipeline_state_dirty();
        }
        break;
      }
      case 0x12:
        g_gxState.dualTex = val;
        mark_pipeline_state_dirty();
        break;
      case 0x18: {
        // Matrix index A: PnMtx + TexCoord0-3 matrix indices
        g_gxState.currentPnMtx = bp_get(val, 6, 0) / 3;
        for (u32 i = 0; i < 4 && i < MaxTexCoord; i++) {
          auto texMtx = static_cast<GXTexMtx>(bp_get(val, 6, 6 + i * 6));
          assert(texMtx >= 0 && texMtx <= GXTexMtx::GX_IDENTITY);
          g_gxState.tcgs[i].mtx = texMtx;
        }
        mark_pipeline_state_dirty();
        break;
      }
      case 0x19: {
        // Matrix index B: TexCoord4-7 matrix indices
        for (u32 i = 0; i < 4 && (i + 4) < MaxTexCoord; i++) {
          g_gxState.tcgs[i + 4].mtx = static_cast<GXTexMtx>(bp_get(val, 6, i * 6));
        }
        mark_pipeline_state_dirty();
        break;
      }
      case 0x1A:
      case 0x1B:
      case 0x1C:
      case 0x1D:
      case 0x1E:
      case 0x1F: {
        // Viewport: sx, sy, sz, ox, oy, oz at XF 0x101A-0x101F
        const u32 vpOff = reg - 0x1A;
        g_gxState.xfViewport[vpOff] = read_f32(xfData + i * 4, bigEndian);
        viewportUpdated = true;
        break;
      }
      case 0x20:
      case 0x21:
      case 0x22:
      case 0x23:
      case 0x24:
      case 0x25:
      case 0x26: {
        // Projection: 6 params + type at XF 0x1020-0x1026
        const u32 projOff = reg - 0x20;
        if (projOff < g_gxState.xfProjection.size()) {
          g_gxState.xfProjection[projOff] = read_f32(xfData + i * 4, bigEndian);
        } else {
          g_gxState.projType = static_cast<GXProjectionType>(val);
        }
        projectionUpdated = true;
        gfx::post::note_projection(g_gxState.xfProjection.data(), g_gxState.projType == GX_PERSPECTIVE);
        break;
      }
      case 0x3F:
        // numTexGens
        g_gxState.numTexGens = val;
        mark_pipeline_state_dirty();
        break;
      default:
        // TexGen config (0x40-0x4F) and post-transform (0x50-0x5F)
        if (reg >= 0x40 && reg <= 0x4F) {
          u32 tcIdx = reg - 0x40;
          if (tcIdx < MaxTexCoord) {
            auto& tcg = g_gxState.tcgs[tcIdx];
            bool proj = bp_get(val, 1, 1) != 0;
            u32 form = bp_get(val, 1, 2);
            u32 tgType = bp_get(val, 3, 4);
            u32 srcRow = bp_get(val, 5, 7);
            tcg.inputFormAB11 = form == 0;

            if (tgType == 0) {
              tcg.type = proj ? GX_TG_MTX3x4 : GX_TG_MTX2x4;
            } else if (tgType == 1) {
              // Bump mapping
              tcg.type = static_cast<GXTexGenType>(bp_get(val, 3, 15) + 2);
            } else if (tgType == 2 || tgType == 3) {
              tcg.type = GX_TG_SRTG;
            }

            // Decode source from row
            static const GXTexGenSrc rowToSrc[] = {GX_TG_POS,  GX_TG_NRM,  GX_TG_COLOR0, GX_TG_BINRM, GX_TG_TANGENT,
                                                   GX_TG_TEX0, GX_TG_TEX1, GX_TG_TEX2,   GX_TG_TEX3,  GX_TG_TEX4,
                                                   GX_TG_TEX5, GX_TG_TEX6, GX_TG_TEX7};
            if (srcRow < 13) {
              tcg.src = rowToSrc[srcRow];
            }
            mark_pipeline_state_dirty();
          }
        } else if (reg >= 0x50 && reg <= 0x5F) {
          u32 tcIdx = reg - 0x50;
          if (tcIdx < MaxTexCoord) {
            g_gxState.tcgs[tcIdx].postMtx = static_cast<GXPTTexMtx>(bp_get(val, 6, 0) + 64);
            g_gxState.tcgs[tcIdx].normalize = bp_get(val, 1, 8) != 0;
            mark_pipeline_state_dirty();
          }
        } else {
#ifndef NDEBUG
          Log.debug("Unhandled XF register 0x{:04X} (value 0x{:08X})", reg, val);
#endif
        }
        break;
      }
    }
    if (viewportUpdated) {
      apply_xf_viewport(header);
    }
    if (projectionUpdated) {
      apply_xf_projection();
    }
  }

  pos += dataBytes;
}

static void handle_draw_overrun(u8 cmd, u16 vtxCount, u32 vtxSize, u32 totalVtxBytes, const u8* data, const u32& pos,
                                u32 size) {
  static u32 truncatedDrawLogCount = 0;
  if (on_decoder_thread() && truncatedDrawLogCount < 8) {
    Log.error("asynchronous decoder: chunk ends inside a draw ({} of {} vertex bytes present); the stream after it is misaligned",
              size - pos, totalVtxBytes);
  }
  if (truncatedDrawLogCount >= 64) {
    if (truncatedDrawLogCount == 64) {
      Log.warn("suppressing further truncated draw diagnostics");
    }
    ++truncatedDrawLogCount;
    return;
  }
  ++truncatedDrawLogCount;

  // Hex dump around the draw command for debugging
  u32 cmdPos = pos - 2 - 1; // opcode byte position (before vtxCount and pos++)
  u32 dumpStart = (cmdPos > 16) ? cmdPos - 16 : 0;
  u32 dumpEnd = (cmdPos + 32 < size) ? cmdPos + 32 : size;
  std::string hex;
  for (u32 i = dumpStart; i < dumpEnd; i++) {
    if (i == cmdPos)
      hex += fmt::format("[{:02x}]", data[i]);
    else
      hex += fmt::format(" {:02x}", data[i]);
  }
  Log.warn("  hex dump around truncated draw cmd (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
  const auto fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  Log.warn("  truncated draw cmd=0x{:02X} fmt={} vtxCount={} vtxSize={} desc pn={} pos={} nrm={} clr0={} clr1={} "
            "tex0={} tex1={} tex2={} tex3={} tex4={} tex5={} tex6={} tex7={}",
            cmd, static_cast<u32>(fmt), vtxCount, vtxSize, g_gxState.vtxDesc[GX_VA_PNMTXIDX],
            g_gxState.vtxDesc[GX_VA_POS], g_gxState.vtxDesc[GX_VA_NRM], g_gxState.vtxDesc[GX_VA_CLR0],
            g_gxState.vtxDesc[GX_VA_CLR1], g_gxState.vtxDesc[GX_VA_TEX0], g_gxState.vtxDesc[GX_VA_TEX1],
            g_gxState.vtxDesc[GX_VA_TEX2], g_gxState.vtxDesc[GX_VA_TEX3], g_gxState.vtxDesc[GX_VA_TEX4],
            g_gxState.vtxDesc[GX_VA_TEX5], g_gxState.vtxDesc[GX_VA_TEX6], g_gxState.vtxDesc[GX_VA_TEX7]);
  Log.warn("  fmt {} attrs pos({},{}) nrm({},{}) clr0({},{}) tex0({},{}) tex1({},{})",
            static_cast<u32>(fmt), static_cast<u32>(vtxFmt.attrs[GX_VA_POS].cnt),
            static_cast<u32>(vtxFmt.attrs[GX_VA_POS].type), static_cast<u32>(vtxFmt.attrs[GX_VA_NRM].cnt),
            static_cast<u32>(vtxFmt.attrs[GX_VA_NRM].type), static_cast<u32>(vtxFmt.attrs[GX_VA_CLR0].cnt),
            static_cast<u32>(vtxFmt.attrs[GX_VA_CLR0].type), static_cast<u32>(vtxFmt.attrs[GX_VA_TEX0].cnt),
            static_cast<u32>(vtxFmt.attrs[GX_VA_TEX0].type), static_cast<u32>(vtxFmt.attrs[GX_VA_TEX1].cnt),
            static_cast<u32>(vtxFmt.attrs[GX_VA_TEX1].type));
  Log.warn("stopping FIFO decode at truncated draw: need {} bytes at pos {}, have {}", totalVtxBytes, pos, size);
}

// Draw command handler - parses vertices inline and caches results
static u32 calculate_last_vtx_size(GXVtxFmt fmt) {
  u32 vtxSize = 0;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto& attrFmt = vtxFmt.attrs[i];
    switch (g_gxState.vtxDesc[i]) {
    case GX_NONE:
      break;
    case GX_DIRECT: {
      vtxSize += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
      break;
    }
    case GX_INDEX8:
      vtxSize += (attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3) ? 3 : 1;
      break;
    case GX_INDEX16:
      vtxSize += (attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3) ? 6 : 2;
      break;
    }
  }

  g_gxState.lastVtxFmt = fmt;
  g_gxState.lastVtxSize = vtxSize;
  // The format is selected by the draw opcode, without a register write.
  // Even equal-stride formats may decode bytes differently, so do not merge
  // into a draw using the previous format's shader and uniform layout.
  g_gxState.stateDirty = true;

  return vtxSize;
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount,
                                 gfx::Range vertRange, uint16_t usedPnMtxMask,
                                 HashType matrixTopologySignature,
                                 HashType geometrySignature, bool interpolationIdentityActive);

// The per-draw geometry signature, matrix-usage mask and draw-identity hashes exist purely to feed frame interpolation (build_uniform consumes them only after its `frame_interpolation_fps() == 0` early-out).
static inline bool frame_interpolation_identity_needed() noexcept {
  return frame_interpolation_active() && g_gxState.projType == GX_PERSPECTIVE;
}

static uint32_t matrix_index_prefix_size(GXVtxFmt fmt) noexcept {
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  uint32_t size = 0;
  for (int i = GX_VA_PNMTXIDX; i < GX_VA_POS; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    switch (g_gxState.vtxDesc[i]) {
    case GX_NONE:
      break;
    case GX_DIRECT:
      size += comp_type_size(attr, vtxFmt.attrs[i].type) *
              comp_cnt_count(attr, vtxFmt.attrs[i].cnt);
      break;
    case GX_INDEX8:
      ++size;
      break;
    case GX_INDEX16:
      size += 2;
      break;
    }
  }
  return size;
}

static HashType draw_geometry_signature(GXVtxFmt fmt, const uint8_t* vertices,
                                        uint16_t vtxCount, uint32_t vtxStride) noexcept {
  Hasher hasher;
  hasher.update(vtxCount);
  hasher.update(vtxStride);

  // Matrix-index bytes select an instance's current XF palette slots, so they are deliberately excluded from mesh identity.
  const uint32_t matrixPrefix = std::min(matrix_index_prefix_size(fmt), vtxStride);
  if (matrixPrefix == 0) {
    // Most draws have no direct matrix-index prefix.
    hasher.update(vertices, static_cast<size_t>(vtxCount) * vtxStride);
  } else {
    for (uint16_t vertex = 0; vertex < vtxCount; ++vertex) {
      hasher.update(vertices + static_cast<size_t>(vertex) * vtxStride + matrixPrefix,
                    vtxStride - matrixPrefix);
    }
  }

  // Identical index streams can address different vertex arrays.
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (g_gxState.vtxDesc[i] != GX_INDEX8 && g_gxState.vtxDesc[i] != GX_INDEX16) {
      continue;
    }
    const auto& array = g_gxState.arrays[i];
    const auto attribute = static_cast<uint32_t>(i);
    const auto source = reinterpret_cast<uintptr_t>(array.data);
    hasher.update(attribute);
    hasher.update(source);
    hasher.update(array.stride);
  }
  return static_cast<HashType>(hasher.digest());
}

struct PnMtxUsage {
  uint16_t mask = 0;
  HashType topologySignature = 0;
};

// Slot mask only.
static uint16_t pn_mtx_mask(const uint8_t* vertices, uint16_t vtxCount,
                            uint32_t vtxStride) noexcept {
  if (g_gxState.vtxDesc[GX_VA_PNMTXIDX] != GX_DIRECT) {
    return static_cast<uint16_t>(1u << std::min<uint32_t>(g_gxState.currentPnMtx, MaxPnMtx - 1));
  }
  uint16_t mask = 0;
  for (uint16_t vertex = 0; vertex < vtxCount; ++vertex) {
    const uint32_t matrixIndex = vertices[static_cast<size_t>(vertex) * vtxStride] / 3u;
    if (matrixIndex < MaxPnMtx) {
      mask |= static_cast<uint16_t>(1u << matrixIndex);
    }
  }
  return mask;
}

static PnMtxUsage pn_mtx_usage(const uint8_t* vertices, uint16_t vtxCount,
                               uint32_t vtxStride) noexcept {
  if (g_gxState.vtxDesc[GX_VA_PNMTXIDX] != GX_DIRECT) {
    const uint32_t matrixIndex = std::min<uint32_t>(g_gxState.currentPnMtx, MaxPnMtx - 1);
    return {
        .mask = static_cast<uint16_t>(1u << matrixIndex),
        // There is no vertex matrix-index topology in this path.
        .topologySignature = 0,
    };
  }

  Hasher topologyHasher;
  topologyHasher.update(vtxCount);
  uint16_t mask = 0;
  for (uint16_t vertex = 0; vertex < vtxCount; ++vertex) {
    // GX matrix-index attributes precede every other vertex attribute and are always one byte.
    const uint8_t rawMatrixIndex = vertices[static_cast<size_t>(vertex) * vtxStride];
    topologyHasher.update(rawMatrixIndex);
    const uint32_t matrixIndex = rawMatrixIndex / 3u;
    if (matrixIndex < MaxPnMtx) {
      mask |= static_cast<uint16_t>(1u << matrixIndex);
    }
  }
  return {
      .mask = mask,
      .topologySignature = static_cast<HashType>(topologyHasher.digest()),
  };
}

// Whether the most recent unmerged draw recorded an interpolation snapshot, so a draw merged into it knows there is a snapshot to extend.
static bool s_lastDrawRecordedInterpolation = false;

struct CachedIndexTemplate {
  bool valid = false;
  GXPrimitive prim = static_cast<GXPrimitive>(0);
  u16 vtxCount = 0;
  u32 indexCount = 0;
  IndexBuffer indices{};
};

static const CachedIndexTemplate& cached_index_template(GXPrimitive prim, u16 vtxCount) {
  // Topology expansion is immutable for a primitive/count pair.
  constexpr size_t CacheSize = 256;
  static std::array<CachedIndexTemplate, CacheSize> cache{};
  const u32 key = (static_cast<u32>(underlying(prim)) << 16) | vtxCount;
  auto& entry = cache[(key ^ (key >> 9)) & (CacheSize - 1)];
  if (entry.valid && entry.prim == prim && entry.vtxCount == vtxCount) LIKELY {
    return entry;
  }

  entry.valid = true;
  entry.prim = prim;
  entry.vtxCount = vtxCount;
  entry.indexCount = prepare_idx_template(entry.indices, prim, vtxCount);
  return entry;
}

static IndexBuffer handle_draw_idx_buf;

static ArrayRef<u16> offset_index_template(const CachedIndexTemplate& indexTemplate,
                                           u16 vtxStart) {
  // Grow-only: resizing down and back up made every merge zero-fill the buffer before the transform immediately overwrote it.
  const size_t count = indexTemplate.indices.size();
  if (handle_draw_idx_buf.size() < count) {
    handle_draw_idx_buf.resize(count);
  }
  const u16* src = indexTemplate.indices.data();
  u16* dst = handle_draw_idx_buf.data();
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<u16>(src[i] + vtxStart);
  }
  return {dst, count};
}

struct CachedPipelineState {
  gfx::PipelineRef ref = 0;
  gfx::PipelineRef fallbackRef = 0;
  HashType configHash = 0;
  // Carried here so the draw can be recorded without keeping the PipelineConfig that produced it alive; it is the only field of the config the draw itself still needs.
  u32 dstAlpha = UINT32_MAX;
  ShaderInfo shaderInfo{};
  // Ubershader bridge, prepared the first time a draw of this material finds its pipeline still compiling.
  const PipelineConfig* config = nullptr;
  mutable struct UberBridge {
    bool tried = false;
    bool ok = false;
    bool specReady = false;  // the specialised pipeline compiled: no more lookups for this material
    gfx::PipelineRef ref = 0;
    ShaderInfo info{};
    UberData data{};
  } uber;
};

static const CachedPipelineState& cached_pipeline_state(const PipelineConfig& config) {
  constexpr size_t CacheSize = 1024;
  struct Entry {
    bool valid = false;
    PipelineConfig config{};
    CachedPipelineState state{};
  };
  static std::array<Entry, CacheSize> cache{};

  const HashType hash = xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX));
  auto& entry = cache[hash & (CacheSize - 1)];
  if (entry.valid && entry.state.configHash == hash &&
      std::memcmp(&entry.config, &config, sizeof(config)) == 0) LIKELY {
    return entry.state;
  }

  gfx::PipelineRef fallbackRef = 0;
  if (config.shaderConfig.perPixelLighting != 0) {
    // While the per-pixel (or enhanced metal) pipeline compiles, draw with the vertex-lit one,
    // which the shipped seed covers.
    PipelineConfig vertexLit = config;
    vertexLit.shaderConfig.perPixelLighting = 0;
    vertexLit.shaderConfig.enhancedMetal = 0;
    fallbackRef = gfx::pipeline_ref(vertexLit);
  }
  entry.valid = true;
  entry.config = config;
  entry.state = {
      .ref = gfx::pipeline_ref(config),
      .fallbackRef = fallbackRef,
      .configHash = hash,
      .dstAlpha = config.dstAlpha,
      .shaderInfo = build_shader_info(config.shaderConfig),
      .config = &entry.config,
  };
  return entry.state;
}

// Enhanced materials: the same material with the enhanced-metal variant forced on. A table of its
// own: cached_pipeline_state's table backs resolve_pipeline_state's memo, which must not be
// rewritten behind its back.
static const CachedPipelineState& material_pipeline_state(const PipelineConfig& base) {
  constexpr size_t CacheSize = 256;
  struct Entry {
    bool valid = false;
    PipelineConfig config{};
    CachedPipelineState state{};
  };
  static std::array<Entry, CacheSize> cache{};
  PipelineConfig config = base;
  config.shaderConfig.enhancedMetal = 1;
  const HashType hash = xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX));
  auto& entry = cache[hash & (CacheSize - 1)];
  if (entry.valid && entry.state.configHash == hash && std::memcmp(&entry.config, &config, sizeof(config)) == 0)
    LIKELY {
    return entry.state;
  }
  // While it compiles, draw with the vertex-lit material (covered by the shipped seed).
  PipelineConfig vertexLit = config;
  vertexLit.shaderConfig.perPixelLighting = 0;
  vertexLit.shaderConfig.enhancedMetal = 0;
  entry.valid = true;
  entry.config = config;
  entry.state = {
      .ref = gfx::pipeline_ref(config),
      .fallbackRef = gfx::pipeline_ref(vertexLit),
      .configHash = hash,
      .dstAlpha = config.dstAlpha,
      .shaderInfo = build_shader_info(config.shaderConfig),
      .config = &entry.config,
  };
  return entry.state;
}

// Environment material (level 1): the same draw with ShaderConfig::envMaterial set. While that
// pipeline compiles the draw keeps its original pipeline (already in the cache or the seed), so the
// effect appears a little later instead of a hitch.
static const CachedPipelineState& env_pipeline_state(const PipelineConfig& base) {
  constexpr size_t CacheSize = 512;
  struct Entry {
    bool valid = false;
    PipelineConfig config{};
    CachedPipelineState state{};
  };
  static std::array<Entry, CacheSize> cache{};
  PipelineConfig config = base;
  config.shaderConfig.envMaterial = 1;
  const HashType hash = xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX));
  auto& entry = cache[hash & (CacheSize - 1)];
  if (entry.valid && entry.state.configHash == hash && std::memcmp(&entry.config, &config, sizeof(config)) == 0)
    LIKELY {
    return entry.state;
  }
  entry.valid = true;
  entry.config = config;
  entry.state = {
      .ref = gfx::pipeline_ref(config),
      .fallbackRef = gfx::pipeline_ref(base),
      .configHash = hash,
      .dstAlpha = config.dstAlpha,
      .shaderInfo = build_shader_info(config.shaderConfig),
      .config = &entry.config,
  };
  return entry.state;
}

// Soft projected shadows: the draw with the soft-shadow flag (shader_soft_shadow, PCF on the projected texture); the
// original pipeline draws while the variant compiles.
static const CachedPipelineState& soft_shadow_pipeline_state(const PipelineConfig& base) {
  constexpr size_t CacheSize = 64;
  struct Entry {
    bool valid = false;
    PipelineConfig config{};
    CachedPipelineState state{};
  };
  static std::array<Entry, CacheSize> cache{};
  PipelineConfig config = base;
  set_shader_soft_shadow(config.shaderConfig);
  const HashType hash = xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX));
  auto& entry = cache[hash & (CacheSize - 1)];
  if (entry.valid && entry.state.configHash == hash && std::memcmp(&entry.config, &config, sizeof(config)) == 0)
    LIKELY {
    return entry.state;
  }
  entry.valid = true;
  entry.config = config;
  entry.state = {
      .ref = gfx::pipeline_ref(config),
      .fallbackRef = gfx::pipeline_ref(base),
      .configHash = hash,
      .dstAlpha = config.dstAlpha,
      .shaderInfo = build_shader_info(config.shaderConfig),
      .config = &entry.config,
  };
  return entry.state;
}

// The 3D world's own view: perspective, a depth band of at least half the range (not the arm cannon,
// sky layers or far backdrops) and nearly the full EFB width (not the radar or small inset views).
static bool env_world_view() noexcept {
  const auto& vp = g_gxState.logicalViewport;
  return g_gxState.projType == GX_PERSPECTIVE && vp.zfar - vp.znear >= 0.5f &&
         vp.width >= static_cast<float>(logical_fb_size().x) * 0.9f;
}

// Level 2: tally of the lights the game's lit world draws used this frame; the strongest and most
// used become the next frame's borrowed lights (view space, so one frame of camera lag at most).
namespace {
struct LightTally {
  Light light;
  float luma = 0.0f;
  uint32_t draws = 0;
};
std::array<LightTally, 16> g_lightTally{};
size_t g_lightTallyCount = 0;
uint32_t g_lightTallyFrame = UINT32_MAX;

void borrowed_lights_frame(uint32_t frame) {
  if (frame == g_lightTallyFrame) return;
  if (g_lightTallyFrame != UINT32_MAX) {
    std::array<size_t, 16> order{};
    for (size_t i = 0; i < g_lightTallyCount; ++i) order[i] = i;
    std::sort(order.begin(), order.begin() + g_lightTallyCount, [](size_t a, size_t b) {
      return g_lightTally[a].luma * static_cast<float>(g_lightTally[a].draws) >
             g_lightTally[b].luma * static_cast<float>(g_lightTally[b].draws);
    });
    g_borrowedLights.count = static_cast<uint32_t>(std::min(g_lightTallyCount, MaxBorrowedLights));
    float positions[MaxBorrowedLights][3] = {};
    float luminance[MaxBorrowedLights] = {};
    for (uint32_t i = 0; i < g_borrowedLights.count; ++i) {
      const auto& tally = g_lightTally[order[i]];
      g_borrowedLights.lights[i] = prepare_shader_light(tally.light);
      positions[i][0] = tally.light.pos[0]; positions[i][1] = tally.light.pos[1]; positions[i][2] = tally.light.pos[2];
      luminance[i] = tally.luma;
    }
    gfx::post::note_lights(g_borrowedLights.count, positions, luminance);
  }
  g_lightTallyFrame = frame;
  g_lightTallyCount = 0;
}

void tally_draw_lights() {
  const auto mask = g_gxState.colorChannelState[GX_COLOR0].lightMask;
  for (size_t i = 0; i < GX::MaxLights; ++i) {
    if (!mask.test(i)) continue;
    const Light& light = g_gxState.lights[i];
    const float luma = light.color[0] * 0.299f + light.color[1] * 0.587f + light.color[2] * 0.114f;
    if (!(luma > 0.02f)) continue;
    bool found = false;
    for (size_t k = 0; k < g_lightTallyCount; ++k) {
      if (g_lightTally[k].light == light) { ++g_lightTally[k].draws; found = true; break; }
    }
    if (!found && g_lightTallyCount < g_lightTally.size())
      g_lightTally[g_lightTallyCount++] = {light, luma, 1};
  }
}
} // namespace

// Modern lighting, level 1: the engine's own lights (RM3E01 CLight lists; docs/iluminacion_motor.md).
// The runtime sends them in world space once per game frame (game thread). Here, at the first static
// lightmapped world draw of each frame, they are moved into that draw's view (the world geometry is
// drawn with an identity model matrix, so its position matrix is the camera's view), converted to GX
// lights exactly as the engine's loader (0x803E8BD4) would, ranked, and handed to the environment
// material, the light shafts and the contact shadows. Static lights are baked into the lightmaps:
// they only ever add highlights, never diffuse light.
namespace engine_lights {
// GX producer thread only (the game thread's input lives in gx.cpp, take_engine_lights).
std::vector<AuroraEngineLight> g_world;
uint64_t g_seenGeneration = 0;
uint32_t g_freshFrame = UINT32_MAX;  // GX frame that received the latest list
constexpr uint32_t kStaleFrames = 30;  // no new list for half a second: menu, map, video
struct ViewLight {
  Light gx{};          // as the engine would load it for this view
  float radius = 0.f;  // world units (the view has no scale)
  float luma = 0.f;    // luminance at the light, with the brightness its k0 encodes
  float score = 0.f;
  uint32_t type = 0;
  bool dynamic = false;
};
std::vector<ViewLight> g_view;
std::vector<uint32_t> g_dynamicOrder;  // g_view indices of the dynamic lights, in the engine's order
uint32_t g_viewFrame = UINT32_MAX;
bool g_viewValid = false;
constexpr float kUnbounded = 1.0e6f;

Light to_gx(const AuroraEngineLight& l, const Mat3x4<float>& v) {
  const auto point = [&](const float* p) {
    return Vec4<float>{v.m0[0] * p[0] + v.m0[1] * p[1] + v.m0[2] * p[2] + v.m0[3],
                       v.m1[0] * p[0] + v.m1[1] * p[1] + v.m1[2] * p[2] + v.m1[3],
                       v.m2[0] * p[0] + v.m2[1] * p[1] + v.m2[2] * p[2] + v.m2[3], 0.f};
  };
  const auto direction = [&](const float* d) {
    return Vec4<float>{v.m0[0] * d[0] + v.m0[1] * d[1] + v.m0[2] * d[2],
                       v.m1[0] * d[0] + v.m1[1] * d[1] + v.m1[2] * d[2],
                       v.m2[0] * d[0] + v.m2[1] * d[1] + v.m2[2] * d[2], 0.f};
  };
  Light out{};
  out.color = {static_cast<float>(l.color[0]) / 255.f, static_cast<float>(l.color[1]) / 255.f,
               static_cast<float>(l.color[2]) / 255.f, static_cast<float>(l.color[3]) / 255.f};
  out.cosAtt = {1.f, 0.f, 0.f, 0.f};
  out.distAtt = {l.distAtt[0], l.distAtt[1], l.distAtt[2], 0.f};
  switch (l.type) {
  case 0: {  // spot: GXInitLightDir negates; GXInitLightSpot(cutoff, GX_SP_COS2)
    out.pos = point(l.pos);
    const auto d = direction(l.dir);
    out.dir = {-d[0], -d[1], -d[2], 0.f};
    if (l.spotCutoff > 0.f && l.spotCutoff <= 90.f) {
      const float cr = std::cos(l.spotCutoff * 3.14159265f / 180.f);
      out.cosAtt = {0.f, -cr / (1.f - cr), 1.f / (1.f - cr), 0.f};
    }
    break;
  }
  case 2: {  // directional: a point 2^20 away against the direction, no attenuation
    const auto d = direction(l.dir);
    out.pos = {-d[0] * 1048576.f, -d[1] * 1048576.f, -d[2] * 1048576.f, 0.f};
    out.distAtt = {1.f, 0.f, 0.f, 0.f};
    break;
  }
  case 4: {  // custom: position, direction and the light's own angle attenuation
    out.pos = point(l.pos);
    const auto d = direction(l.dir);
    out.dir = {-d[0], -d[1], -d[2], 0.f};
    out.cosAtt = {l.angleAtt[0], l.angleAtt[1], l.angleAtt[2], 0.f};
    break;
  }
  default:  // point (1) and local ambient (3): position only, no angle attenuation
    out.pos = point(l.pos);
    break;
  }
  return out;
}

float length3(const Vec4<float>& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

void frame(const Mat3x4<float>& view, uint32_t frame) {
  g_viewFrame = frame;
  {
    float rows[12];
    for (int c = 0; c < 4; ++c) {
      rows[c] = view.m0[c];
      rows[4 + c] = view.m1[c];
      rows[8 + c] = view.m2[c];
    }
    gfx::post::note_world_view(rows);
  }
  if (take_engine_lights(g_world, g_seenGeneration)) g_freshFrame = frame;
  g_view.clear();
  g_dynamicOrder.clear();
  g_engineEnvLights.count = 0;
  g_viewValid = false;
  const bool fresh = g_freshFrame != UINT32_MAX && frame - g_freshFrame <= kStaleFrames;
  if (!fresh || !g_engineLightsOption.load(std::memory_order_relaxed)) {
    gfx::post::note_engine_lights(0, nullptr, nullptr, nullptr, nullptr);
    return;
  }
  for (const auto& l : g_world) {
    if (l.type == 3 || l.type > 4) continue;  // local ambient: no direction, no highlight
    ViewLight vl;
    vl.gx = to_gx(l, view);
    vl.type = l.type;
    vl.dynamic = (l.flags & AURORA_ENGINE_LIGHT_DYNAMIC) != 0;
    vl.radius = l.radius > 0.f && l.type != 2 ? std::min(l.radius, kUnbounded) : kUnbounded;
    const float maxChannel = std::max({vl.gx.color[0], vl.gx.color[1], vl.gx.color[2]});
    if (!(maxChannel > 0.01f)) continue;
    // k0 < 1 is how the engine makes a light brighter than its 8-bit colour.
    const float boost = l.type == 2 ? 1.f : std::clamp(1.f / std::max(std::abs(l.distAtt[0]), 0.05f), 0.25f, 4.f);
    vl.luma = (vl.gx.color[0] * 0.299f + vl.gx.color[1] * 0.587f + vl.gx.color[2] * 0.114f) * boost;
    if (l.type == 2) {
      vl.score = vl.luma * 4.f;
    } else {
      const float d = length3(vl.gx.pos);
      const float z = -vl.gx.pos[2];  // GX view space looks down -z
      if (d - vl.radius > 60.f || z < -vl.radius) {
        vl.score = 0.f;  // out of reach of anything near the camera, or entirely behind it
      } else {
        vl.score = vl.luma * std::clamp(vl.radius / std::max(d, 0.5f), 0.f, 4.f) * (vl.dynamic ? 1.5f : 1.f);
      }
    }
    if (vl.dynamic) g_dynamicOrder.push_back(static_cast<uint32_t>(g_view.size()));
    g_view.push_back(vl);
  }
  g_viewValid = true;
  std::array<uint32_t, 16> best{};
  size_t bestCount = 0;
  for (uint32_t i = 0; i < g_view.size(); ++i) {
    if (!(g_view[i].score > 0.f)) continue;
    size_t at = bestCount < best.size() ? bestCount++ : best.size();
    if (at == best.size()) {
      if (g_view[i].score <= g_view[best[best.size() - 1]].score) continue;
      at = best.size() - 1;
    }
    while (at > 0 && g_view[best[at - 1]].score < g_view[i].score) {
      best[at] = best[at - 1];
      --at;
    }
    best[at] = i;
  }
  // Environment material: the strongest eight, range in color.a (+ static, - dynamic).
  const size_t envCount = std::min(bestCount, MaxEnvLights);
  for (size_t k = 0; k < envCount; ++k) {
    const ViewLight& vl = g_view[best[k]];
    Light light = prepare_shader_light(vl.gx);
    light.color[3] = vl.dynamic ? -std::max(vl.radius, 0.01f) : std::max(vl.radius, 0.01f);
    g_engineEnvLights.lights[k] = light;
  }
  g_engineEnvLights.count = static_cast<uint32_t>(envCount);
  g_engineEnvLights.frame = frame;
  // Light shafts and contact shadows: the four strongest; shafts only for lights in front.
  float positions[4][3] = {}, radius[4] = {}, luma[4] = {};
  uint8_t directional[4] = {};
  float shaftPos[4][3] = {}, shaftLuma[4] = {};
  uint32_t count = 0, shafts = 0;
  for (size_t k = 0; k < bestCount && (count < 4 || shafts < 4); ++k) {
    const ViewLight& vl = g_view[best[k]];
    if (count < 4) {
      for (int c = 0; c < 3; ++c) positions[count][c] = vl.gx.pos[c];
      radius[count] = vl.radius;
      luma[count] = vl.luma;
      directional[count] = vl.type == 2 ? 1 : 0;
      ++count;
    }
    if (shafts < 4 && vl.type != 2 && -vl.gx.pos[2] > 0.1f) {
      for (int c = 0; c < 3; ++c) shaftPos[shafts][c] = vl.gx.pos[c];
      shaftLuma[shafts] = std::min(vl.luma, 1.f);
      ++shafts;
    }
  }
  gfx::post::note_engine_lights(count, positions, radius, luma, directional);
  if (gfx::post::light_shafts_wanted() && shafts > 0) gfx::post::note_lights(shafts, shaftPos, shaftLuma);
}

// Lit lightmapped world pieces: the engine gives each piece at most four of the frame's dynamic lights
// (0x8047DC54) and leaves the rest out. When a piece has exactly four, the overlapping dynamic lights
// the engine left out go into the free GX slots 4-7 for this draw only; the uniform carries the light
// mask, so the pipeline does not change. Restored right after the uniform is built.
struct InjectSave {
  bool active = false;
  std::array<Light, 4> lights{};
  std::array<std::bitset<GX::MaxLights>, 4> masks{};
};

bool inject_extra(InjectSave& save, uint32_t frame) {
  if (!g_viewValid || g_viewFrame != frame || g_dynamicOrder.size() <= 4) return false;
  const auto mask0 = g_gxState.colorChannelState[GX_COLOR0].lightMask;
  if (mask0.to_ulong() != 0x0Fu) return false;
  // The four loaded lights, found again in the engine's dynamic list.
  std::array<float, 4> loadedRadius{};
  std::array<bool, 64> used{};
  int matched = 0;
  for (size_t slot = 0; slot < 4; ++slot) {
    const Light& loaded = g_gxState.lights[slot];
    loadedRadius[slot] = -1.f;
    const float tolerance = 1.0e-3f * (1.f + length3(loaded.pos));
    for (size_t k = 0; k < g_dynamicOrder.size() && k < used.size(); ++k) {
      const Light& candidate = g_view[g_dynamicOrder[k]].gx;
      if (used[k] || std::abs(candidate.pos[0] - loaded.pos[0]) > tolerance ||
          std::abs(candidate.pos[1] - loaded.pos[1]) > tolerance || std::abs(candidate.pos[2] - loaded.pos[2]) > tolerance)
        continue;
      used[k] = true;
      loadedRadius[slot] = g_view[g_dynamicOrder[k]].radius;
      ++matched;
      break;
    }
  }
  if (matched == 0) return false;
  std::array<uint32_t, 4> extra{};
  uint32_t extraCount = 0;
  for (size_t k = 0; k < g_dynamicOrder.size() && k < used.size() && extraCount < 4; ++k) {
    if (used[k]) continue;
    const ViewLight& c = g_view[g_dynamicOrder[k]];
    if (c.type == 2) continue;
    bool overlaps = false;
    for (size_t slot = 0; slot < 4 && !overlaps; ++slot) {
      if (loadedRadius[slot] <= 0.f) continue;
      const Light& loaded = g_gxState.lights[slot];
      const float dx = c.gx.pos[0] - loaded.pos[0], dy = c.gx.pos[1] - loaded.pos[1], dz = c.gx.pos[2] - loaded.pos[2];
      const float reach = c.radius + loadedRadius[slot];
      overlaps = dx * dx + dy * dy + dz * dz < reach * reach;
    }
    if (overlaps) extra[extraCount++] = g_dynamicOrder[k];
  }
  if (extraCount == 0) return false;
  save.active = true;
  for (size_t i = 0; i < 4; ++i) save.lights[i] = g_gxState.lights[4 + i];
  for (size_t ch = 0; ch < 4; ++ch) save.masks[ch] = g_gxState.colorChannelState[ch].lightMask;
  std::bitset<GX::MaxLights> added;
  for (uint32_t i = 0; i < extraCount; ++i) {
    g_gxState.lights[4 + i] = g_view[extra[i]].gx;
    added.set(4 + i);
  }
  for (size_t ch = 0; ch < 4; ++ch)
    if (g_gxState.colorChannelState[ch].lightMask == mask0) g_gxState.colorChannelState[ch].lightMask |= added;
  g_gxState.preparedLightsDirty = true;
  return true;
}

void restore(InjectSave& save) {
  if (!save.active) return;
  for (size_t i = 0; i < 4; ++i) g_gxState.lights[4 + i] = save.lights[i];
  for (size_t ch = 0; ch < 4; ++ch) g_gxState.colorChannelState[ch].lightMask = save.masks[ch];
  g_gxState.preparedLightsDirty = true;
  save.active = false;
}
} // namespace engine_lights

// Resolving a pipeline the long way costs a ~2.7KB zero-init, a full populate_pipeline_config, an XXH3 over the whole config and a memcmp against the hash-indexed slot -- roughly 11KB of memory traffic for a result that is almost always identical to the previous draw's.
static const CachedPipelineState& resolve_pipeline_state(GXPrimitive prim, GXVtxFmt fmt) {
  struct Memo {
    const CachedPipelineState* state = nullptr;
    u32 generation = 0;
    u32 sampleCount = 0;
    GXPrimitive prim = static_cast<GXPrimitive>(0);
    GXVtxFmt fmt = static_cast<GXVtxFmt>(0);
  };
  static Memo memo{};

  const u32 sampleCount = gfx::get_sample_count();
  const u32 generation = g_gxState.pipelineStateGeneration;
  if (memo.state != nullptr && memo.generation == generation && memo.sampleCount == sampleCount &&
      memo.prim == prim && memo.fmt == fmt) LIKELY {
    return *memo.state;
  }

  PipelineConfig config{};
  populate_pipeline_config(config, prim, fmt);
  static const bool primeShaderDumpEnabled = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GX_SHADER_DUMP") != nullptr;
  static bool primeShaderDumpRequested = false;
  if (!primeShaderDumpRequested && primeShaderDumpEnabled &&
      g_gxState.numTexGens == 1 && g_gxState.loadedTextures[0].width() == 640 &&
      g_gxState.loadedTextures[0].height() == 480) {
    primeShaderDumpRequested = true;
    (void)build_shader(config.shaderConfig);
  }
  static bool primeFontShaderDumpRequested = false;
  if (!primeFontShaderDumpRequested && primeShaderDumpEnabled &&
      g_gxState.numTexGens != 0 && g_gxState.loadedTextures[0].width() == 64 &&
      g_gxState.loadedTextures[0].height() == 80) {
    primeFontShaderDumpRequested = true;
    Log.info("[prime-font-shader-config] stride={} pos_offset={} uv_offset={} uniform_hash={:016X}",
             config.shaderConfig.vtxStride,config.shaderConfig.attrs[GX_VA_POS].offset,
             config.shaderConfig.attrs[GX_VA_TEX0].offset,xxh3_hash(config.shaderConfig));
    (void)build_shader(config.shaderConfig);
  }
  // cached_pipeline_state hands back a reference into a fixed direct-mapped table, so the address stays valid; the entry it points at can only be rewritten by another call to that function, and every such call goes through this miss path and replaces the memo in the same breath.
  const CachedPipelineState& state = cached_pipeline_state(config);
  memo = Memo{
      .state = &state,
      .generation = generation,
      .sampleCount = sampleCount,
      .prim = prim,
      .fmt = fmt,
  };
  return state;
}

static bool admit_draw(GXPrimitive prim, GXVtxFmt fmt, u16 count, uint32_t vertexBytes, bool merged = false) {
  const auto& indexTemplate = cached_index_template(prim, count);
  gfx::StagingSizes demand{vertexBytes, 0, indexTemplate.indices.size() * sizeof(u16), 0};
  if (merged) return gfx::staging_has_space(demand);
  const auto& info = resolve_pipeline_state(prim, fmt).shaderInfo;
  demand[1] = gfx::staging_uniform_bytes(info.uniformSize);
  if (frame_interpolation_identity_needed() && frame_interpolation_replay_safe())
    demand[1] *= 1 + active_interpolation_samples();
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if ((g_gxState.vtxDesc[i] == GX_INDEX8 || g_gxState.vtxDesc[i] == GX_INDEX16) &&
        g_gxState.arrays[i].cachedRange.size == 0)
      demand[3] += gfx::staging_storage_bytes(g_gxState.arrays[i].size);
  }
  return gfx::staging_has_space(demand);
}

bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes) {
  ZoneScoped;
  if (vertices == nullptr || vtxCount == 0 || vertexBytes == 0) {
    return false;
  }

  if (__gx->dirtyState != 0) UNLIKELY {
    __GXSetDirtyState();
  }

  if (async_enabled() && !on_decoder_thread()) {
    // The HLE producer derived vertexBytes from the VAT/VCD state it published
    // above; the decoder thread re-derives the stride from the same state.
    if (!has_complete_primitive(prim, vtxCount)) return true;
    write_u8(static_cast<u8>(fmt | prim));
    write_u16(vtxCount);
    write_data(vertices, vertexBytes);
    commit();
    return true;
  }
  // Raw bridge draws consume live decoded GX state that is also maintained by the HLE producer.
  drain();

  u32 vtxSize;
  if (g_gxState.lastVtxFmt == fmt) LIKELY {
    vtxSize = g_gxState.lastVtxSize;
  } else UNLIKELY {
    vtxSize = calculate_last_vtx_size(fmt);
  }

  const u32 expectedVertexBytes = static_cast<u32>(vtxCount) * vtxSize;
  if (expectedVertexBytes == 0 || expectedVertexBytes != vertexBytes) {
    static uint32_t rawVertexSizeMismatchCount = 0;
    if (rawVertexSizeMismatchCount++ < 64) {
      Log.warn("raw draw vertex-size mismatch prim={} fmt={} count={} cached_stride={} expected={} supplied={}",
               static_cast<uint32_t>(prim), static_cast<uint32_t>(fmt), vtxCount, vtxSize,
               expectedVertexBytes, vertexBytes);
    }
    return false;
  }

  if (!has_complete_primitive(prim, vtxCount)) return true;

  // This entry point bypasses process(), so it owns the renderer lock itself.
  std::unique_lock gpuLock(aurora::renderer_gpu_mutex());
  if (!admit_draw(prim, fmt, vtxCount, vertexBytes)) {
    gpuLock.unlock();
    gfx::split_staging_batch();
    gpuLock.lock();
    if (!admit_draw(prim, fmt, vtxCount, vertexBytes))
      throw gfx::StagingCapacityError("Raw draw does not fit after capacity submission");
  }
  static unsigned primeRawVertexTrace = 0;
  if (CachedWiiCompiledTracePrimeGx() && primeRawVertexTrace < 8 &&
      g_gxState.loadedTextures[0].width() == 640 && g_gxState.numTexGens != 0 && vertexBytes >= 24) {
    ++primeRawVertexTrace;
    Log.info("[prime-gx-raw-textured-vertex] stride={} desc={},{},{},{},{} words={:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
             vtxSize,underlying(g_gxState.vtxDesc[GX_VA_PNMTXIDX]),underlying(g_gxState.vtxDesc[GX_VA_POS]),
             underlying(g_gxState.vtxDesc[GX_VA_NRM]),underlying(g_gxState.vtxDesc[GX_VA_CLR0]),underlying(g_gxState.vtxDesc[GX_VA_TEX0]),
             read_u32(vertices,true),read_u32(vertices+4,true),read_u32(vertices+8,true),read_u32(vertices+12,true),
             read_u32(vertices+16,true),read_u32(vertices+20,true));
  }
  if (CachedWiiCompiledTracePrimeGx() && primeRawVertexTrace < 8 &&
      g_gxState.loadedTextures[0].width() == 640 && g_gxState.numTexGens != 0 && vtxCount == 4 && vtxSize == 24) {
    for (unsigned i = 0; i < 4; ++i) {
      const auto* v = vertices + i * 24;
      Log.info("[prime-gx-quad-vertex] i={} pos={},{},{} rgba={:08X} uv={},{}", i,
               read_f32(v,true),read_f32(v+4,true),read_f32(v+8,true),read_u32(v+12,true),
               read_f32(v+16,true),read_f32(v+20,true));
    }
  }
  static bool primeStrapCaptureRequested = false;
  if (!primeStrapCaptureRequested && vtxCount == 4 && vtxSize == 24 &&
      g_gxState.loadedTextures[0].width() == 640 && g_gxState.loadedTextures[0].height() == 480 &&
      g_gxState.numTexGens == 1 && vertices[15] >= 128) {
    if (const char* path = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_CAPTURE_STRAP_PATH")) {
      primeStrapCaptureRequested = true;
      aurora_request_frame_capture(gfx::current_frame() + 1, path);
      Log.info("[prime-strap-frame-capture-request] frame={} alpha={}",gfx::current_frame()+1,vertices[15]);
    }
  }
  const gfx::Range vertRange = gfx::push_verts(vertices, vertexBytes);
  const bool interpolationIdentityActive = frame_interpolation_identity_needed();
  const PnMtxUsage matrixUsage = interpolationIdentityActive
                                     ? pn_mtx_usage(vertices, vtxCount, vtxSize)
                                     : PnMtxUsage{};
  handle_draw_unmerged(prim, fmt, vtxCount, vertRange,
                       matrixUsage.mask, matrixUsage.topologySignature,
                       interpolationIdentityActive ? draw_geometry_signature(fmt, vertices, vtxCount, vtxSize) : 0,
                       interpolationIdentityActive);
  return true;
}

static bool handle_draw(u8 cmd, const u8* data, u32& pos, u32 size, bool bigEndian) {
  ZoneScoped;
  GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
  GXPrimitive prim = primitive_from_draw_cmd(cmd);

  if (pos + 2 > size) {
    if (on_decoder_thread()) Log.error("asynchronous decoder: chunk ends inside a draw header; the stream after it is misaligned");
    return false;
  }
  u16 vtxCount = read_u16(data + pos, bigEndian);
  pos += 2;

  u32 vtxSize;
  if (g_gxState.lastVtxFmt == fmt) LIKELY {
    vtxSize = g_gxState.lastVtxSize;
  } else UNLIKELY {
    vtxSize = calculate_last_vtx_size(fmt);
  }

  u32 totalVtxBytes = vtxCount * vtxSize;
  if (g_gxState.vtxDesc[GX_VA_POS] == GX_NONE) UNLIKELY {
    // Never a real draw: the stream is misaligned or the VCD load was lost.
    // A pipeline key built from it would poison the cache (and every boot).
    static u32 noPositionLogCount = 0;
    if (noPositionLogCount++ < 8) {
      Log.error("draw cmd=0x{:02X} fmt={} count={} stride={} with no position attribute (decoder thread={}): skipped",
                cmd, static_cast<u32>(fmt), vtxCount, vtxSize, on_decoder_thread());
    }
    if (pos + totalVtxBytes > size) return false;
    pos += totalVtxBytes;
    return true;
  }
  if (pos + totalVtxBytes > size) UNLIKELY {
    handle_draw_overrun(cmd, vtxCount, vtxSize, totalVtxBytes, data, pos, size);
    return false;
  }


  if (!has_complete_primitive(prim, vtxCount)) {
    pos += totalVtxBytes;
    return true;
  }

  static const bool primeTrace = CachedWiiCompiledTracePrimeGx();
  static unsigned primeTraceCount = 0;
  const auto& tex = g_gxState.loadedTextures[0];
  if (primeTrace && primeTraceCount < 8 && tex.width() == 640 && tex.height() == 480) {
    ++primeTraceCount;
    const auto& vp = g_gxState.renderViewport;
    const auto& sc = g_gxState.renderScissor;
    const auto& xp = g_gxState.xfProjection;
    Log.info("[prime-gx-draw-state] count={} stride={} be={} tex_data={} fmt={} texgens={} stages={} cull={} depth={}/{} update={} color={} alpha={}",
             vtxCount, vtxSize, bigEndian, tex.data != nullptr, tex.format(), g_gxState.numTexGens,
             g_gxState.numTevStages, underlying(g_gxState.cullMode), g_gxState.depthCompare,
             underlying(g_gxState.depthFunc), g_gxState.depthUpdate, g_gxState.colorUpdate, g_gxState.alphaUpdate);
    Log.info("[prime-gx-draw-view] viewport={},{},{},{},{},{} scissor={},{},{},{} projection_type={} projection={},{},{},{},{},{}",
             vp.left,vp.top,vp.width,vp.height,vp.znear,vp.zfar,sc.x,sc.y,sc.width,sc.height,
             underlying(g_gxState.projType),xp[0],xp[1],xp[2],xp[3],xp[4],xp[5]);
    const auto* pm = reinterpret_cast<const float*>(&g_gxState.pnMtx[g_gxState.currentPnMtx % MaxPnMtx].pos);
    Log.info("[prime-gx-draw-matrix] slot={} values={},{},{},{},{},{},{},{},{},{},{},{}",
             g_gxState.currentPnMtx,pm[0],pm[1],pm[2],pm[3],pm[4],pm[5],pm[6],pm[7],pm[8],pm[9],pm[10],pm[11]);
    if (vtxSize >= 24 && vtxCount != 0)
      Log.info("[prime-gx-first-vertex] xyz={},{},{} color=0x{:08X} uv={},{}", read_f32(data+pos,bigEndian),
               read_f32(data+pos+4,bigEndian),read_f32(data+pos+8,bigEndian),read_u32(data+pos+12,bigEndian),
               read_f32(data+pos+16,bigEndian),read_f32(data+pos+20,bigEndian));
  }

  DrawData* mergeTarget = nullptr;
  // Decide admission before allocating anything. The merged path needs only
  // vertices and indices; it must not resolve pipelines or upload arrays.
  // Try to merge with previous draw call
  if (!g_gxState.stateDirty) LIKELY {
    auto* lastDraw = gfx::get_last_draw_command<DrawData>();
    // Expanded lines/points have different vertex interpretation even with one instance.
    // Triangle-list output has no restart index; index 65535 is usable.
    // Overflow would address earlier vertices instead of the appended geometry.
    if (lastDraw != nullptr && prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS &&
        !lastDraw->expandedPrimitive && lastDraw->instanceCount == 1 &&
        uint64_t(lastDraw->vtxCount) +
            vtxCount <= 65536u) LIKELY {
      mergeTarget = lastDraw;
    }
  }
  if (!admit_draw(prim, fmt, vtxCount, totalVtxBytes, mergeTarget != nullptr)) throw gfx::StagingBatchFull{};
  const uint8_t* vertices = data + pos;
  gfx::Range vertRange = gfx::push_verts(vertices, totalVtxBytes);
  pos += totalVtxBytes;
  if (auto* lastDraw = mergeTarget) {
      const auto& indexTemplate = cached_index_template(prim, vtxCount);
      const auto indices = offset_index_template(indexTemplate, lastDraw->vtxCount);
      const u32 numIndices = indexTemplate.indexCount;
      const gfx::Range idxRange = gfx::push_indices(indices);
      CHECK(lastDraw->vertRange.offset + lastDraw->vertRange.size == vertRange.offset,
            "Non-consecutive vertex ranges ({} < {})", lastDraw->vertRange.offset + lastDraw->vertRange.size,
            vertRange.offset);
      CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == idxRange.offset,
            "Non-consecutive index ranges ({} < {})", lastDraw->idxRange.offset + lastDraw->idxRange.size,
            idxRange.offset);
      lastDraw->vertRange.size += vertRange.size;
      lastDraw->idxRange.size += idxRange.size;
      lastDraw->vtxCount += vtxCount;
      lastDraw->indexCount += numIndices;
      ++gfx::g_mergedDrawCallCount;
      // This primitive now renders through the draw we merged into, so its palette slots belong to that draw's interpolation snapshot as well.
      if (s_lastDrawRecordedInterpolation) UNLIKELY {
        extend_interpolation_draw(pn_mtx_mask(vertices, vtxCount, vtxSize));
      }
      return true;
  }

  const bool interpolationIdentityActive = frame_interpolation_identity_needed();
  const PnMtxUsage matrixUsage = interpolationIdentityActive
                                     ? pn_mtx_usage(vertices, vtxCount, vtxSize)
                                     : PnMtxUsage{};
  handle_draw_unmerged(prim, fmt, vtxCount, vertRange,
                       matrixUsage.mask, matrixUsage.topologySignature,
                       interpolationIdentityActive ? draw_geometry_signature(fmt, vertices, vtxCount, vtxSize) : 0,
                       interpolationIdentityActive);
  return true;
}

// One line per draw of the dumped frame: what decides visibility (cull, depth,
// alpha test, blend, write masks), where it lands (XF viewport/projection, the
// current position matrix) and what it samples.
// Second line of a dumped draw, once its textures are resolved: per sampled map the GX
// size/format/TLUT, the host pointer of its guest data, data version, and the host
// texture this draw binds (null = none) with its size, format, mips and replacement flag.
static void dump_draw_textures(std::FILE* f, const PipelineConfig* config) {
  const auto& s = g_gxState;
  if (config != nullptr) {
    // Material line: what decides which host material effects apply (lighting, texgen sources).
    const auto& sc = config->shaderConfig;
    std::fprintf(f, "  mat ppl=%u metal=%u lit=%d/%d lights=0x%02x/0x%02x nrm=%u stages=%u texgen=", unsigned(sc.perPixelLighting),
                 unsigned(sc.enhancedMetal), int(sc.colorChannels[0].lightingEnabled),
                 int(sc.colorChannels[1].lightingEnabled), unsigned(s.colorChannelState[0].lightMask.to_ulong()),
                 unsigned(s.colorChannelState[1].lightMask.to_ulong()), unsigned(sc.attrs[GX_VA_NRM].attrType),
                 unsigned(sc.tevStageCount));
    for (u32 i = 0; i < sc.numTexGens; ++i) std::fprintf(f, "%s%u", i ? "," : "", unsigned(sc.tcgs[i].src));
    std::fputc('\n', f);
  }
  std::fprintf(f, "  tex");
  u32 printed = 0;
  for (u32 i = 0; i < s.numTevStages; ++i) {
    const u32 map = s.tevStages[i].texMapId;
    if (map >= MaxTextures || (printed & (1u << map))) continue;
    printed |= 1u << map;
    const auto& obj = s.loadedTextures[map];
    const auto& bind = s.textures[map];
    std::fprintf(f, " t%u=%ux%u/f%u/tl%u/d%p/v%u/id%u/b%p", map, obj.width(), obj.height(), obj.format(),
                 unsigned(obj.tlut), obj.data, obj.texDataVersion, obj.texObjId,
                 static_cast<const void*>(bind.ref.get()));
    if (bind.ref)
      std::fprintf(f, "/%ux%u/hf%u/m%u/r%d", bind.ref->size.width, bind.ref->size.height,
                   unsigned(bind.ref->gxFormat), unsigned(bind.ref->mipCount), int(bind.ref->isReplacement));
    if (gfx::draw_dump::g_saveTextures.load(std::memory_order_acquire) && obj.data != nullptr) {
      const std::filesystem::path dir = gfx::draw_dump::base_path() + ".textures";
      std::fprintf(f, "/n%s", gfx::texture_replacement::dump_texture_for_inspection(obj, dir).c_str());
    }
  }
  std::fputc('\n', f);
}

static void dump_draw(std::FILE* f, GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, const gfx::Range& vertRange) {
  const auto& s = g_gxState;
  const auto& vp = s.xfViewport;
  const auto& pr = s.xfProjection;
  std::fprintf(f,
               "draw %u pass=%u prim=%u fmt=%u verts=%u cull=%u depth=%d/%u/%d color=%d alpha=%d "
               "blend=%u/%u/%u/%u dstalpha=%d acmp=%u/%u/%u/%u/%u zcomp_early=%d ztex=%u fog=%u "
               "xfvp=%g,%g,%.9g,%g,%g,%.9g lvp=%g,%g,%g,%g,%.9g,%.9g scissor=%d,%d,%d,%d "
               "proj=%u:%g,%g,%g,%g,%g,%g pnmtx=%u zclear=0x%06x",
               gfx::draw_dump::next_draw_index(), gfx::current_render_pass_index(), unsigned(prim), unsigned(fmt),
               unsigned(vtxCount), unsigned(s.cullMode), int(s.depthCompare), unsigned(s.depthFunc),
               int(s.depthUpdate), int(s.colorUpdate), int(s.alphaUpdate), unsigned(s.blendMode),
               unsigned(s.blendFacSrc), unsigned(s.blendFacDst), unsigned(s.blendOp),
               s.dstAlpha == UINT32_MAX ? -1 : int(s.dstAlpha), unsigned(s.alphaCompare.comp0),
               unsigned(s.alphaCompare.ref0), unsigned(s.alphaCompare.op), unsigned(s.alphaCompare.comp1),
               unsigned(s.alphaCompare.ref1), int(s.zCompLocBeforeTex), unsigned(s.zTextureOp),
               unsigned(s.fog.type), vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], s.logicalViewport.left,
               s.logicalViewport.top, s.logicalViewport.width, s.logicalViewport.height, s.logicalViewport.znear,
               s.logicalViewport.zfar, s.logicalScissor.x, s.logicalScissor.y, s.logicalScissor.width,
               s.logicalScissor.height, unsigned(s.projType), pr[0], pr[1], pr[2], pr[3], pr[4], pr[5],
               unsigned(s.currentPnMtx), unsigned(s.clearDepth));
  if (s.currentPnMtx < MaxPnMtx) {
    const auto& m = s.pnMtx[s.currentPnMtx].pos;
    std::fprintf(f, " mtx=%g,%g,%g,%g|%g,%g,%g,%g|%g,%g,%g,%g", m.m0.x(), m.m0.y(), m.m0.z(), m.m0.w(), m.m1.x(),
                 m.m1.y(), m.m1.z(), m.m1.w(), m.m2.x(), m.m2.y(), m.m2.z(), m.m2.w());
  }
  // Texgen matrices as the decoder holds them (late guest reads show up as repeated or skipped scroll steps),
  // and a hash of all staged vertex bytes.
  for (u32 i = 0; i < s.numTexGens && i < MaxTexCoord; ++i) {
    const auto& tcg = s.tcgs[i];
    const u32 slot = u32(tcg.mtx) / 3;
    if (tcg.mtx == GX_IDENTITY || slot >= MaxPnMtx + MaxTexMtx) continue;
    const auto& m = slot < MaxPnMtx ? s.pnMtx[slot].pos : s.texMtxs[slot - MaxPnMtx];
    std::fprintf(f, " tm%u=%u:%g,%g,%g,%g|%g,%g,%g,%g", i, slot, m.m0.x(), m.m0.y(), m.m0.z(), m.m0.w(), m.m1.x(),
                 m.m1.y(), m.m1.z(), m.m1.w());
    if (tcg.postMtx != GX_PTIDENTITY) {
      const u32 pt = (u32(tcg.postMtx) - u32(GX_PTTEXMTX0)) / 3;
      if (pt < MaxPTTexMtx) {
        const auto& p = s.ptTexMtxs[pt];
        std::fprintf(f, " pt%u=%u:%g,%g,%g,%g|%g,%g,%g,%g", i, pt, p.m0.x(), p.m0.y(), p.m0.z(), p.m0.w(), p.m1.x(),
                     p.m1.y(), p.m1.z(), p.m1.w());
      }
    }
  }
  if (const auto* staged = gfx::staged_vertex_data(vertRange); staged != nullptr) {
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < vertRange.size; ++i) h = (h ^ staged[i]) * 1099511628211ull;
    std::fprintf(f, " vh=%016llx", static_cast<unsigned long long>(h));
  }
  std::fprintf(f, " pf=%u tev=%u texgens=%u chans=%u", unsigned(s.pixelFmt), unsigned(s.numTevStages),
               unsigned(s.numTexGens), unsigned(s.numChans));
  // Per stage: color args a,b,c,d op/scale>reg | alpha args a,b,c,d op/scale>reg, texmap, konst sel.
  for (u32 i = 0; i < s.numTevStages && i < MaxTevStages; ++i) {
    const auto& t = s.tevStages[i];
    std::fprintf(f, " s%u=c%u,%u,%u,%u/%u%u>%u|a%u,%u,%u,%u/%u%u>%u,m%u,k%u/%u", i, unsigned(t.colorPass.a),
                 unsigned(t.colorPass.b), unsigned(t.colorPass.c), unsigned(t.colorPass.d), unsigned(t.colorOp.op),
                 unsigned(t.colorOp.scale), unsigned(t.colorOp.outReg), unsigned(t.alphaPass.a),
                 unsigned(t.alphaPass.b), unsigned(t.alphaPass.c), unsigned(t.alphaPass.d), unsigned(t.alphaOp.op),
                 unsigned(t.alphaOp.scale), unsigned(t.alphaOp.outReg), unsigned(t.texMapId), unsigned(t.kcSel),
                 unsigned(t.kaSel));
  }
  std::fprintf(f, " k0=%.3f,%.3f,%.3f,%.3f k1=%.3f,%.3f,%.3f,%.3f c0=%.3f,%.3f,%.3f,%.3f", s.kcolors[0].x(),
               s.kcolors[0].y(), s.kcolors[0].z(), s.kcolors[0].w(), s.kcolors[1].x(), s.kcolors[1].y(),
               s.kcolors[1].z(), s.kcolors[1].w(), s.colorRegs[1].x(), s.colorRegs[1].y(), s.colorRegs[1].z(),
               s.colorRegs[1].w());
  // Color/alpha channel 0: material source, lighting, material and ambient colors (RASC/RASA inputs).
  for (u32 ch = 0; ch < 2; ++ch) {
    const auto& cfg = s.colorChannelConfig[ch];
    const auto& st = s.colorChannelState[ch];
    std::fprintf(f, " ch%u=src%u/amb%u/light%d/df%u/af%u mat=%.3f,%.3f,%.3f,%.3f amb=%.3f,%.3f,%.3f,%.3f", ch,
                 unsigned(cfg.matSrc), unsigned(cfg.ambSrc), int(cfg.lightingEnabled), unsigned(cfg.diffFn),
                 unsigned(cfg.attnFn), st.matColor.x(), st.matColor.y(), st.matColor.z(), st.matColor.w(),
                 st.ambColor.x(), st.ambColor.y(), st.ambColor.z(), st.ambColor.w());
  }
  std::fprintf(f, " vclr0=%u", unsigned(s.vtxDesc[GX_VA_CLR0]));
  // Up to 4 direct positions as submitted (big-endian guest layout).
  if (s.vtxDesc[GX_VA_POS] == GX_DIRECT && vtxCount != 0) {
    const auto& pos = s.vtxFmts[fmt].attrs[GX_VA_POS];
    u32 offset = 0;
    for (int attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7MTXIDX; ++attr)
      if (s.vtxDesc[attr] != GX_NONE) offset += 1;
    const u32 stride = vertRange.size / vtxCount;
    const u32 comps = pos.cnt == GX_POS_XYZ ? 3 : 2;
    const auto* data = gfx::staged_vertex_data(vertRange);
    if (data != nullptr && (pos.type == GX_F32 || pos.type == GX_S16 || pos.type == GX_U16 || pos.type == GX_S8 || pos.type == GX_U8)) {
      std::fprintf(f, " pos(t%u,f%u)=", unsigned(pos.type), unsigned(pos.frac));
      for (u32 v = 0; v < std::min<u32>(vtxCount, 4); ++v) {
        const u8* p = data + v * stride + offset;
        for (u32 c = 0; c < comps; ++c) {
          float value = 0.f;
          if (pos.type == GX_F32) {
            const u32 bits = (u32(p[4 * c]) << 24) | (u32(p[4 * c + 1]) << 16) | (u32(p[4 * c + 2]) << 8) | p[4 * c + 3];
            std::memcpy(&value, &bits, 4);
          } else if (pos.type == GX_S16 || pos.type == GX_U16) {
            const u16 raw = u16((p[2 * c] << 8) | p[2 * c + 1]);
            value = (pos.type == GX_S16 ? float(s16(raw)) : float(raw)) / float(1u << pos.frac);
          } else {
            value = (pos.type == GX_S8 ? float(s8(p[c])) : float(p[c])) / float(1u << pos.frac);
          }
          std::fprintf(f, "%s%g", c ? "," : "", value);
        }
        std::fputc(v + 1 < std::min<u32>(vtxCount, 4) ? ';' : ' ', f);
      }
      std::fprintf(f, " stride=%u", stride);
      if (s.vtxDesc[GX_VA_PNMTXIDX] == GX_DIRECT) {
        std::fprintf(f, " mtxidx=");
        for (u32 v = 0; v < std::min<u32>(vtxCount, 7); ++v) std::fprintf(f, "%u%s", unsigned(data[v * stride]), v + 1 < std::min<u32>(vtxCount, 7) ? "," : "");
        for (u32 slot = 0; slot < MaxPnMtx; ++slot) {
          const auto& m = s.pnMtx[slot].pos;
          std::fprintf(f, " m%u=%g,%g,%g,%g|%g,%g,%g,%g|%g,%g,%g,%g", slot, m.m0.x(), m.m0.y(), m.m0.z(), m.m0.w(), m.m1.x(),
                       m.m1.y(), m.m1.z(), m.m1.w(), m.m2.x(), m.m2.y(), m.m2.z(), m.m2.w());
        }
      }
      if (vtxCount > 4) {
        std::fprintf(f, " pos_rest=");
        for (u32 v = 4; v < std::min<u32>(vtxCount, 8); ++v) {
          const u8* p = data + v * stride + offset;
          for (u32 c = 0; c < comps && pos.type == GX_F32; ++c) {
            const u32 bits = (u32(p[4 * c]) << 24) | (u32(p[4 * c + 1]) << 16) | (u32(p[4 * c + 2]) << 8) | p[4 * c + 3];
            float value; std::memcpy(&value, &bits, 4);
            std::fprintf(f, "%s%g", c ? "," : "", value);
          }
          std::fputc(';', f);
        }
      }
    }
  }
  std::fprintf(f, " tex=");
  u32 dumpedMaps = 0;
  for (u32 i = 0; i < s.numTevStages && i < MaxTevStages; ++i) {
    const u32 map = underlying(s.tevStages[i].texMapId);
    if (map >= MaxTextures || (dumpedMaps & (1u << map)) != 0) continue;
    dumpedMaps |= 1u << map;
    const auto& t = s.loadedTextures[map];
    std::fprintf(f, "[%u:%ux%u/f%u@%p]", map, t.width(), t.height(), t.format(), t.data);
  }
  std::fputc('\n', f);
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount,
                                 gfx::Range vertRange, uint16_t usedPnMtxMask,
                                 HashType matrixTopologySignature,
                                 HashType geometrySignature, bool interpolationIdentityActive) {
  if (auto* dump = gfx::draw_dump::active()) UNLIKELY {
    dump_draw(dump, prim, fmt, vtxCount, vertRange);
  }
  producer_profile::Scope profile(producer_profile::Draw);
  ZoneScoped;
  static unsigned primeActiveTextureTrace = 0;
  if (CachedWiiCompiledTracePrimeGx() && primeActiveTextureTrace < 16 &&
      g_gxState.loadedTextures[0].width() == 640 && g_gxState.loadedTextures[0].height() == 480 &&
      g_gxState.numTexGens != 0) {
    ++primeActiveTextureTrace;
    const auto& vp = g_gxState.renderViewport;
    const auto& sc = g_gxState.renderScissor;
    const auto& xp = g_gxState.xfProjection;
    const auto& alpha = g_gxState.alphaCompare;
    Log.info("[prime-gx-active-textured-draw] count={} fmt={} texgens={} stages={} cull={} depth={}/{} write={} blend={} src={} dst={} alpha={},{},{},{},{} host_genmode=0x{:08X}",
             vtxCount,underlying(fmt),g_gxState.numTexGens,g_gxState.numTevStages,underlying(g_gxState.cullMode),
             g_gxState.depthCompare,underlying(g_gxState.depthFunc),g_gxState.colorUpdate,underlying(g_gxState.blendMode),
             underlying(g_gxState.blendFacSrc),underlying(g_gxState.blendFacDst),underlying(alpha.comp0),alpha.ref0,
             underlying(alpha.op),underlying(alpha.comp1),alpha.ref1,__gx->genMode);
    Log.info("[prime-gx-active-textured-view] viewport={},{},{},{} scissor={},{},{},{} projection_type={} projection={},{},{},{},{},{}",
             vp.left,vp.top,vp.width,vp.height,sc.x,sc.y,sc.width,sc.height,
             underlying(g_gxState.projType),xp[0],xp[1],xp[2],xp[3],xp[4],xp[5]);
    const auto* pm = reinterpret_cast<const float*>(&g_gxState.pnMtx[g_gxState.currentPnMtx % MaxPnMtx].pos);
    Log.info("[prime-gx-active-channels] channels={} dstalpha={} fog={} filter={},{},{},{},{},{},{}",
             g_gxState.numChans,g_gxState.dstAlpha,underlying(g_gxState.fog.type),
             g_gxState.copyFilterVFilter[0],g_gxState.copyFilterVFilter[1],g_gxState.copyFilterVFilter[2],
             g_gxState.copyFilterVFilter[3],g_gxState.copyFilterVFilter[4],g_gxState.copyFilterVFilter[5],g_gxState.copyFilterVFilter[6]);
    const auto& tcg = g_gxState.tcgs[0];
    Log.info("[prime-gx-active-texgen] type={} src={} matrix={} post={} normalize={} input_ab11={}",
             underlying(tcg.type),underlying(tcg.src),underlying(tcg.mtx),underlying(tcg.postMtx),
             tcg.normalize,tcg.inputFormAB11);
    const auto& stage = g_gxState.tevStages[0];
    Log.info("[prime-gx-active-textured-matrix] slot={} values={},{},{},{},{},{},{},{},{},{},{},{}",
             g_gxState.currentPnMtx,pm[0],pm[1],pm[2],pm[3],pm[4],pm[5],pm[6],pm[7],pm[8],pm[9],pm[10],pm[11]);
    Log.info("[prime-gx-active-tev] texmap={} coord={} channel={} color={},{},{},{} alpha={},{},{},{} mat_src={}/{} lighting={}/{}",
             underlying(stage.texMapId),underlying(stage.texCoordId),underlying(stage.channelId),underlying(stage.colorPass.a),
             underlying(stage.colorPass.b),underlying(stage.colorPass.c),underlying(stage.colorPass.d),underlying(stage.alphaPass.a),
             underlying(stage.alphaPass.b),underlying(stage.alphaPass.c),underlying(stage.alphaPass.d),
             underlying(g_gxState.colorChannelConfig[GX_COLOR0].matSrc),underlying(g_gxState.colorChannelConfig[GX_ALPHA0].matSrc),
             g_gxState.colorChannelConfig[GX_COLOR0].lightingEnabled,g_gxState.colorChannelConfig[GX_ALPHA0].lightingEnabled);
  }
  static bool primeFontCaptureRequested = false;
  if (!primeFontCaptureRequested && g_gxState.numTexGens != 0 &&
      g_gxState.loadedTextures[0].width() == 64 && g_gxState.loadedTextures[0].height() == 80) {
    if (const char* path = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_CAPTURE_FONT_PATH")) {
      primeFontCaptureRequested = true;
      const auto& ts = g_gxState.tevStages[0];
      Log.info("[prime-font-state] cull={} colorwrite={} alphawrite={} depth={}/{} blend={}/{}/{} texmap={} channel={} colorargs={},{},{},{} alphaargs={},{},{},{}",
               underlying(g_gxState.cullMode),g_gxState.colorUpdate,g_gxState.alphaUpdate,g_gxState.depthCompare,
               underlying(g_gxState.depthFunc),underlying(g_gxState.blendMode),underlying(g_gxState.blendFacSrc),underlying(g_gxState.blendFacDst),
               underlying(ts.texMapId),underlying(ts.channelId),underlying(ts.colorPass.a),underlying(ts.colorPass.b),underlying(ts.colorPass.c),underlying(ts.colorPass.d),
               underlying(ts.alphaPass.a),underlying(ts.alphaPass.b),underlying(ts.alphaPass.c),underlying(ts.alphaPass.d));
      const char* delayText=AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_CAPTURE_FONT_DELAY");
      const uint32_t delay=delayText ? std::min(600u,static_cast<uint32_t>(std::strtoul(delayText,nullptr,10))) : 0u;
      aurora_request_frame_capture(gfx::current_frame()+1+delay,path);
      Log.info("[prime-font-frame-capture-request] frame={} vertex_offset={} vertex_bytes={} count={}",
               gfx::current_frame()+1,vertRange.offset,vertRange.size,vtxCount);
    }
  }
  static unsigned primeOtherDrawTrace = 0;
  if (CachedWiiCompiledTracePrimeGx() && primeOtherDrawTrace < 24 &&
      g_gxState.numTexGens != 0 && g_gxState.loadedTextures[0].width() > 8 &&
      g_gxState.loadedTextures[0].width() != 640) {
    ++primeOtherDrawTrace;
    const auto& p = g_gxState.vtxFmts[fmt].attrs[GX_VA_POS];
    const auto& t = g_gxState.vtxFmts[fmt].attrs[GX_VA_TEX0];
    Log.info("[prime-gx-other-draw] frame={} prim={} count={} fmt={} texture={}x{}/{} pos={}/{}/{} uv={}/{}/{} desc={},{},{},{} verts={}/{}",
             gfx::current_frame(),underlying(prim),vtxCount,underlying(fmt),g_gxState.loadedTextures[0].width(),
             g_gxState.loadedTextures[0].height(),g_gxState.loadedTextures[0].format(),
             underlying(p.cnt),underlying(p.type),p.frac,underlying(t.cnt),underlying(t.type),t.frac,
             underlying(g_gxState.vtxDesc[GX_VA_POS]),underlying(g_gxState.vtxDesc[GX_VA_NRM]),
             underlying(g_gxState.vtxDesc[GX_VA_CLR0]),underlying(g_gxState.vtxDesc[GX_VA_TEX0]),vertRange.offset,vertRange.size);
  }
  // GX_CULL_ALL rasterizes nothing on hardware - no color, no depth.
  if (g_gxState.cullMode == GX_CULL_ALL && prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS)
      UNLIKELY {
    // Leave stateDirty alone: the next draw re-resolving its pipeline is the safe direction, and nothing about this draw reached the GPU.
    return;
  }
  // Callers hold the renderer GPU mutex for the whole drain (process() and submit_raw_draw); taking it again per draw only cost a recursive re-entry.
  const auto& indexTemplate = cached_index_template(prim, vtxCount);
  const u32 numIndices = indexTemplate.indexCount;
  const gfx::Range idxRange = gfx::push_indices(ArrayRef<u16>{
      indexTemplate.indices.data(), indexTemplate.indices.size()});

  // Build pipeline, bind groups, and push draw command
  BindGroupRanges ranges{};
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (g_gxState.vtxDesc[i] != GX_INDEX8 && g_gxState.vtxDesc[i] != GX_INDEX16) {
      continue;
    }
    auto& array = g_gxState.arrays[i];
    if (array.cachedRange.size > 0) {
      ranges.vaRanges[i - GX_VA_POS] = array.cachedRange;
    } else {
      const auto range = gfx::push_storage(static_cast<const uint8_t*>(array.data), array.size);
      ranges.vaRanges[i - GX_VA_POS] = range;
      array.cachedRange = range;
    }
  }

  const CachedPipelineState* resolvedState = &resolve_pipeline_state(prim, fmt);
  resolve_sampled_textures(resolvedState->shaderInfo);

  // Texture tags (skin, Samus' face, material classes): hashed once per uploaded texture, cached on
  // the TextureRef.
  uint8_t drawTags = 0;
  if (gfx::texture_replacement::texture_tags_listed() != 0) UNLIKELY {
    for (u32 i = 0; i < g_gxState.numTevStages; ++i) {
      const u32 map = g_gxState.tevStages[i].texMapId;
      if (map >= MaxTextures) continue;
      const auto& ref = g_gxState.textures[map].ref;
      if (!ref) continue;
      int8_t tags = ref->tagBits.load(std::memory_order_relaxed);
      if (tags < 0) {
        tags = static_cast<int8_t>(gfx::texture_replacement::texture_tags(g_gxState.loadedTextures[map]));
        ref->tagBits.store(tags, std::memory_order_relaxed);
      }
      drawTags |= static_cast<uint8_t>(tags);
    }
  }
  g_drawMetal = {};
  // A material class wins over the skin tag: Samus' skin and eyes out of the suit have their own
  // classes (skin-like parameters), while the skin tag keeps every other listed face neutral and
  // covers them too when enhanced materials are off or the class is not set.
  if (const uint8_t cls = gfx::texture_replacement::material_class_of(drawTags);
      cls != 0 && g_enhancedMaterials.load(std::memory_order_relaxed)) UNLIKELY {
    const uint64_t packed = g_materialClasses[cls].load(std::memory_order_relaxed);
    const auto* config = resolvedState->config;
    if (packed != 0 && config != nullptr && config->shaderConfig.perPixelLighting != 0 &&
        resolvedState->shaderInfo.lightingEnabled) {
      if (config->shaderConfig.enhancedMetal == 0) resolvedState = &material_pipeline_state(*config);
      const uint32_t strengthBits = static_cast<uint32_t>(packed >> 32);
      std::memcpy(&g_drawMetal.strength, &strengthBits, sizeof(float));
      g_drawMetal.params = static_cast<uint32_t>(packed);
      g_drawMetal.extSlot = static_cast<int8_t>(cls);
      g_drawMetal.active = true;
    }
  }
  if (!g_drawMetal.active && (drawTags & gfx::texture_replacement::TagNoMetal) != 0) {
    g_drawMetal.active = true;  // skin: no metal at all
  }
  // World surfaces (experimental levels 1-2): lightmapped, unlit, opaque draws of the world view get
  // the environment material; their class is the texture's own, or the generic world class.
  const bool envMaterialOn = g_envMaterial.load(std::memory_order_relaxed);
  const bool envLightsOn = g_envBorrowedLights.load(std::memory_order_relaxed);
  const bool shaftsOn = gfx::post::light_shafts_wanted();
  const bool litRoomsOn = g_litRoomMaterial.load(std::memory_order_relaxed);
  if (envMaterialOn || envLightsOn || shaftsOn || litRoomsOn) UNLIKELY {
    const uint32_t frame = gfx::current_frame();
    borrowed_lights_frame(frame);
    const auto* config = resolvedState->config;
    const bool worldView = env_world_view();
    if ((envLightsOn || shaftsOn) && worldView && resolvedState->shaderInfo.lightingEnabled) tally_draw_lights();
    // Lit rooms: lightmapped walls that also take GX lights get the world material with those lights
    // (the enhanced-material pipeline, like a class texture). Actors never use lightmaps.
    if (litRoomsOn && !g_drawMetal.active && config != nullptr && worldView &&
        resolvedState->shaderInfo.lightingEnabled && config->shaderConfig.perPixelLighting != 0 &&
        config->shaderConfig.enhancedMetal == 0 && !resolvedState->shaderInfo.uber &&
        g_gxState.blendMode == GX_BM_NONE && g_gxState.depthUpdate &&
        env_lightmap_stage(config->shaderConfig) >= 0) {
      const uint64_t packed = g_materialClasses[EnvMaterialClass].load(std::memory_order_relaxed);
      if (packed != 0) {
        resolvedState = &material_pipeline_state(*config);
        const uint32_t strengthBits = static_cast<uint32_t>(packed >> 32);
        std::memcpy(&g_drawMetal.strength, &strengthBits, sizeof(float));
        g_drawMetal.params = static_cast<uint32_t>(packed);
        g_drawMetal.extSlot = static_cast<int8_t>(EnvMaterialClass);
        g_drawMetal.active = true;
      }
    }
    if (envMaterialOn && !g_drawMetal.active && config != nullptr && worldView &&
        !resolvedState->shaderInfo.lightingEnabled && !resolvedState->shaderInfo.uber &&
        g_gxState.blendMode == GX_BM_NONE && g_gxState.depthUpdate &&
        config->shaderConfig.attrs[GX_VA_NRM].attrType != GX_NONE && env_lightmap_stage(config->shaderConfig) >= 0) {
      uint8_t cls = gfx::texture_replacement::material_class_of(drawTags);
      uint64_t packed = 0;
      if (cls != 0 && g_enhancedMaterials.load(std::memory_order_relaxed))
        packed = g_materialClasses[cls].load(std::memory_order_relaxed);
      if (packed == 0) {
        cls = EnvMaterialClass;
        packed = g_materialClasses[cls].load(std::memory_order_relaxed);
      }
      if (packed != 0) {
        resolvedState = &env_pipeline_state(*config);
        const uint32_t strengthBits = static_cast<uint32_t>(packed >> 32);
        std::memcpy(&g_drawMetal.strength, &strengthBits, sizeof(float));
        g_drawMetal.params = static_cast<uint32_t>(packed);
        g_drawMetal.extSlot = static_cast<int8_t>(cls);
        g_drawMetal.active = true;
      }
    }
  }
  // Modern lighting: the engine's lights of this game frame, moved into the view of its first static
  // lightmapped world draw, lit or not (the world's model matrix is the identity). After the borrowed
  // lights' frame start above, so the light shafts keep the engine's lights; before build_uniform.
  {
    const uint32_t frame = gfx::current_frame();
    const auto* config = resolvedState->config;
    if (frame != engine_lights::g_viewFrame && config != nullptr &&
        config->shaderConfig.attrs[GX_VA_PNMTXIDX].attrType == GX_NONE && env_world_view() &&
        env_lightmap_stage(config->shaderConfig) >= 0) {
      engine_lights::frame(g_gxState.pnMtx[std::min<u32>(g_gxState.currentPnMtx, MaxPnMtx - 1)].pos, frame);
    }
  }
  // World up for the floor reflections (post-process): from the first static lightmapped world draw
  // of each frame, whose normal matrix is the camera's view rotation (Prime's world is z-up).
  {
    static uint32_t upFrame = UINT32_MAX;
    const uint32_t frame = gfx::current_frame();
    const auto* config = resolvedState->config;
    if (frame != upFrame && config != nullptr && !resolvedState->shaderInfo.lightingEnabled &&
        config->shaderConfig.attrs[GX_VA_PNMTXIDX].attrType == GX_NONE && env_world_view() &&
        env_lightmap_stage(config->shaderConfig) >= 0) {
      upFrame = frame;
      const auto& nrm = g_gxState.pnMtx[std::min<u32>(g_gxState.currentPnMtx, MaxPnMtx - 1)].nrm;
      gfx::post::note_world_up(nrm.m0[2], nrm.m1[2], nrm.m2[2]);
    }
  }
  // Soft projected shadows: Prime multiplies its character shadow textures into the frame
  // (dst * (1 - src)) through a position texgen.
  if (g_softShadows.load(std::memory_order_relaxed)) UNLIKELY {
    const auto* config = resolvedState->config;
    if (config != nullptr && !resolvedState->shaderInfo.uber && g_gxState.blendMode == GX_BM_BLEND &&
        g_gxState.blendFacSrc == GX_BL_ZERO && g_gxState.blendFacDst == GX_BL_INVSRCCLR &&
        shadow_projection_stage(config->shaderConfig) >= 0)
      resolvedState = &soft_shadow_pipeline_state(*config);
  }
  const auto& pipelineState = *resolvedState;
  const auto& info = pipelineState.shaderInfo;
  if (auto* dump = gfx::draw_dump::active()) UNLIKELY {
    dump_draw_textures(dump, pipelineState.config);
  }

  const auto bindGroups = build_bind_groups(info);

  const auto pipeline = pipelineState.ref;

  // Draw-identity hashing only feeds frame interpolation, and only for perspective draws: build_uniform reads the identity exclusively past its `!perspective || frame_interpolation_fps() == 0` early-out.
  FrameInterpolationDrawIdentity drawIdentity{};
  if (interpolationIdentityActive) UNLIKELY {
    const HashType drawShape = static_cast<HashType>(vtxCount) |
                               (static_cast<HashType>(underlying(prim)) << 16) |
                               (static_cast<HashType>(underlying(fmt)) << 24);
    const HashType pipelineDrawSignature = xxh3_hash(pipelineState.configHash, drawShape);
    const HashType textureSignature = xxh3_hash(bindGroups.textureBindGroup);
    const HashType materialAndTopology =
        xxh3_hash(matrixTopologySignature,
                  xxh3_hash(bindGroups.textureBindGroup, pipelineDrawSignature));
    drawIdentity = FrameInterpolationDrawIdentity{
        .combined = xxh3_hash(geometrySignature, materialAndTopology),
        .pipeline = pipelineDrawSignature,
        .texture = textureSignature,
        .matrixTopology = matrixTopologySignature,
        .geometry = xxh3_hash(geometrySignature, xxh3_hash(matrixTopologySignature, drawShape)),
    };
  }
  // Modern lighting: dynamic lights the engine left out of a lit lightmapped world piece (slots 4-7).
  engine_lights::InjectSave injected{};
  if (g_extraDynamicLights.load(std::memory_order_relaxed) && info.lightingEnabled) UNLIKELY {
    const auto* config = pipelineState.config;
    if (config != nullptr && config->shaderConfig.attrs[GX_VA_PNMTXIDX].attrType == GX_NONE && env_world_view() &&
        env_lightmap_stage(config->shaderConfig) >= 0)
      engine_lights::inject_extra(injected, gfx::current_frame());
  }
  const auto uniformRanges =
      build_uniform(info, vertRange.offset, ranges, drawIdentity, interpolationIdentityActive,
                    usedPnMtxMask);
  s_lastDrawRecordedInterpolation = interpolationIdentityActive;

  // Hybrid ubershader: while this material's specialised pipeline is still compiling, also stage the
  // ubershader's uniforms so the encoder can draw with it instead of waiting. Frame interpolation's
  // per-slot ranges are laid out for the specialised shader, so an inserted image draws such a material
  // at its final position (render() picks the ubershader's own range): better than not drawing it.
  gfx::PipelineRef uberPipeline = 0;
  gfx::Range uberUniformRange{};
  if (uber_hybrid_enabled() && pipelineState.config != nullptr) UNLIKELY {
    auto& bridge = pipelineState.uber;
    if (!bridge.specReady && !uber_force()) {
      wgpu::RenderPipeline probe;
      bridge.specReady = gfx::try_pipeline(pipeline, probe);
    }
    if (uber_force() || !bridge.specReady) {
      if (!bridge.tried) {
        bridge.tried = true;
        const UberConfig uc = make_uber_config(*pipelineState.config, info);
        bool fits = false;
        bridge.info = build_uber_shader_info(uc, &fits);
        if (fits) {
          bridge.data = make_uber_data(pipelineState.config->shaderConfig);
          bridge.ref = gfx::pipeline_ref(encode_uber_pipeline(uc));
          bridge.ok = true;
        }
      }
      if (bridge.ok) {
        uberPipeline = bridge.ref;
        uberUniformRange = build_uniform(bridge.info, vertRange.offset, ranges, drawIdentity, false, usedPnMtxMask,
                                         &bridge.data).current;
      }
    }
  }
  engine_lights::restore(injected);

  uint32_t instanceCount = 1;
  if (prim == GX_LINES) {
    instanceCount = vtxCount / 2;
  } else if (prim == GX_LINESTRIP) {
    instanceCount = vtxCount - 1;
  } else if (prim == GX_POINTS) {
    instanceCount = vtxCount;
  }
  const bool ssaoExclude = (drawTags & gfx::texture_replacement::TagNoSsao) != 0 && gfx::post::ssao_exclude_active();
  const bool ssaoCover =
      !ssaoExclude && (drawTags & gfx::texture_replacement::TagSsaoCover) != 0 && gfx::post::ssao_exclude_active();
  // Fog, explosions, particles, glows: blended over what is already drawn and without depth, so the SSAO
  // would read the surface behind them. A blend that only copies the source (One, Zero) is opaque.
  const bool ssaoEffect =
      !ssaoExclude && !ssaoCover && !g_gxState.depthUpdate &&
      (g_gxState.blendMode == GX_BM_BLEND || g_gxState.blendMode == GX_BM_SUBTRACT) &&
      !(g_gxState.blendMode == GX_BM_BLEND && g_gxState.blendFacSrc == GX_BL_ONE && g_gxState.blendFacDst == GX_BL_ZERO) &&
      gfx::post::ssao_effects_active();
  g_drawMetal = {};
  gfx::post::note_draw();
  gfx::push_draw_command(DrawData{
      .pipeline = pipeline,
      .fallbackPipeline = pipelineState.fallbackRef,
      .vertRange = vertRange,
      .idxRange = idxRange,
      .uniformRange = uniformRanges.current,
      .interpolatedUniformRanges = uniformRanges.interpolated,
      .vtxCount = vtxCount,
      .indexCount = numIndices,
      .instanceCount = instanceCount,
      .expandedPrimitive = prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS,
      .bindGroups = bindGroups,
      // The pipeline only knows that destination alpha applies; the value is this draw's.
      .dstAlpha = pipelineState.dstAlpha == UINT32_MAX ? UINT32_MAX : g_gxState.dstAlpha,
      .uberPipeline = uberPipeline,
      .uberUniformRange = uberUniformRange,
      .ssaoExclude = ssaoExclude,
      .ssaoCover = ssaoCover,
      .ssaoEffect = ssaoEffect,
      .depthWrite = g_gxState.depthUpdate,
      .depthWriteUnordered =
          g_gxState.depthUpdate &&
          (!g_gxState.depthCompare || !(g_gxState.depthFunc == GX_NEVER || g_gxState.depthFunc == GX_LESS ||
                                        g_gxState.depthFunc == GX_EQUAL || g_gxState.depthFunc == GX_LEQUAL)),
  });
  g_gxState.stateDirty = false;
}

std::string read_string(const u8* data, u32& pos, u32 size, bool bigEndian) {
  CHECK(pos + 2 <= size, "Aurora string length read overrun");
  const u16 length = read_u16(data + pos, bigEndian);
  pos += 2;

  CHECK(pos + length <= size, "Aurora string read overrun");
  std::string str(reinterpret_cast<const char*>(data) + pos, length);
  pos += length;
  return str;
}

// GX_LOAD_AURORA_HOST_CALLBACK: process() stops after the command so the
// callback runs on the decoder thread outside the renderer lock (copies and
// binds inside it take their own locks), then process_all resumes.
namespace {
struct PendingHostCallback {
  bool pending = false;
  u32 id = 0;
  const u8* data = nullptr;
  u32 size = 0;
};
PendingHostCallback s_pendingHostCallback;
std::atomic<void (*)(uint32_t, const uint8_t*, uint32_t)> g_hostCallback{nullptr};
} // namespace

std::atomic<DeferredDrawExpander> g_deferredDrawExpander{nullptr};
void set_deferred_draw_expander(DeferredDrawExpander expander) {
  g_deferredDrawExpander.store(expander, std::memory_order_release);
}
bool has_deferred_draw_expander() { return g_deferredDrawExpander.load(std::memory_order_acquire) != nullptr; }

void set_host_callback(void (*callback)(uint32_t, const uint8_t*, uint32_t)) {
  g_hostCallback.store(callback, std::memory_order_release);
}

void invoke_host_callback(uint32_t id, const uint8_t* data, uint32_t size) {
  if (const auto callback = g_hostCallback.load(std::memory_order_acquire)) callback(id, data, size);
}

static bool host_callback_pending() { return s_pendingHostCallback.pending; }

bool dispatch_pending_host_callback() {
  if (!s_pendingHostCallback.pending) return false;
  const PendingHostCallback call = s_pendingHostCallback;
  s_pendingHostCallback = {};
  invoke_host_callback(call.id, call.data, call.size);
  return true;
}

bool handle_aurora(const u8* data, u32& pos, u32 size, bool bigEndian) {
  ZoneScoped;
  if (pos + 2 > size) {
    return false;
  }
  u16 subCmd = read_u16(data + pos, bigEndian);
  pos += 2;

  // Setting of vertex array bases.
  if (subCmd == GX_LOAD_AURORA_VIEWPORT_RENDER) {
    CHECK(pos + 24 <= size, "GX_LOAD_AURORA_VIEWPORT_RENDER read overrun");
    const f32 left = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 top = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 width = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 height = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 nearZ = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 farZ = read_f32(data + pos, bigEndian);
    pos += 4;
    set_render_viewport({
        .left = left,
        .top = top,
        .width = width,
        .height = height,
        .znear = nearZ,
        .zfar = farZ,
    });
  } else if (subCmd == GX_LOAD_AURORA_SCISSOR_RENDER) {
    CHECK(pos + 16 <= size, "GX_LOAD_AURORA_SCISSOR_RENDER read overrun");
    const int32_t left = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    const int32_t top = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    const int32_t width = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    const int32_t height = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    set_render_scissor({left, top, width, height});
  } else if (subCmd >= GX_LOAD_AURORA_ARRAYBASE && subCmd <= (GX_LOAD_AURORA_ARRAYBASE | 0x0f)) {
    CHECK(pos + 13 <= size, "GX_LOAD_AURORA_ARRAYBASE read overrun");
    u32 attrIdx = subCmd - GX_LOAD_AURORA_ARRAYBASE + GX_VA_POS;

    u64 arrayAddr = read_u64(data + pos, bigEndian);
    pos += 8;
    u32 arraySize = read_u32(data + pos, bigEndian);
    pos += 4;
    bool le = data[pos] == 1;
    pos += 1;

    auto& array = g_gxState.arrays[attrIdx];
    const auto newData = reinterpret_cast<void*>(arrayAddr);
    if (array.data != newData || array.size != arraySize || array.le != le) {
      array.data = newData;
      array.size = arraySize;
      array.le = le;
      // Only drop the cached upload when the backing array actually changes.
      array.cachedRange = {};
      mark_pipeline_state_dirty();
    }
  } else if (subCmd >= GX_LOAD_AURORA_ARRAYDATA && subCmd <= (GX_LOAD_AURORA_ARRAYDATA | 0x0f)) {
    // The array by value (GXSetArrayCopy): this decoder's own copy, as the writer saw the array.
    static std::array<std::vector<u8>, 16> s_arrayCopies;
    CHECK(pos + 5 <= size, "GX_LOAD_AURORA_ARRAYDATA read overrun");
    const u32 cpIdx = subCmd - GX_LOAD_AURORA_ARRAYDATA;
    const u32 bytes = read_u32(data + pos, bigEndian);
    const bool le = data[pos + 4] == 1;
    pos += 5;
    CHECK(pos + bytes <= size, "GX_LOAD_AURORA_ARRAYDATA payload overrun");
    auto& copy = s_arrayCopies[cpIdx];
    copy.assign(data + pos, data + pos + bytes);
    pos += bytes;
    auto& array = g_gxState.arrays[cpIdx + GX_VA_POS];
    array.data = copy.data();
    array.size = bytes;
    array.le = le;
    array.cachedRange = {};
    mark_pipeline_state_dirty();
  } else if (subCmd == GX_LOAD_AURORA_TEXOBJ) {
    CHECK(pos + 34 <= size, "GX_LOAD_AURORA_TEXOBJ read overrun");
    const auto texMapId = data[pos];
    pos += 1;
    CHECK(texMapId < MaxTextures, "invalid texture map id {}", texMapId);
    auto& slot = g_gxState.loadedTextures[texMapId];
    GXTexObj_ next = slot;
    next.data = reinterpret_cast<const void*>(read_u64(data + pos, bigEndian));
    pos += 8;
    next.mWidth = read_u32(data + pos, bigEndian);
    pos += 4;
    next.mHeight = read_u32(data + pos, bigEndian);
    pos += 4;
    next.mFormat = static_cast<GXTexFmt>(read_u32(data + pos, bigEndian));
    pos += 4;
    next.tlut = static_cast<GXTlut>(read_u32(data + pos, bigEndian));
    pos += 4;
    if (data[pos] != 0) {
      next.flags |= 1u;
    } else {
      next.flags &= ~1u;
    }
    pos += 1;
    next.texObjId = read_u32(data + pos, bigEndian);
    pos += 4;
    next.texDataVersion = read_u32(data + pos, bigEndian);
    pos += 4;
    next.set_no_cache(false); // Reset no-cache flag
    const bool changed = slot.data != next.data || slot.mWidth != next.mWidth || slot.mHeight != next.mHeight ||
                         slot.mFormat != next.mFormat || slot.tlut != next.tlut || slot.flags != next.flags ||
                         slot.texObjId != next.texObjId || slot.texDataVersion != next.texDataVersion;
    slot = next;
    if (changed) {
      g_gxState.stateDirty = true;
    }
  } else if (subCmd == GX_LOAD_AURORA_TLUT) {
    CHECK(pos + 23 <= size, "GX_LOAD_AURORA_TLUT read overrun");
    const auto idx = data[pos];
    pos += 1;
    CHECK(idx < MaxTluts, "invalid tlut slot {}", idx);
    auto& slot = g_gxState.loadedTluts[idx];
    slot.data = reinterpret_cast<const void*>(read_u64(data + pos, bigEndian));
    pos += 8;
    slot.format = static_cast<GXTlutFmt>(read_u32(data + pos, bigEndian));
    pos += 4;
    slot.numEntries = read_u16(data + pos, bigEndian);
    pos += 2;
    slot.tlutObjId = read_u32(data + pos, bigEndian);
    pos += 4;
    slot.tlutDataVersion = read_u32(data + pos, bigEndian);
    pos += 4;
    slot.set_no_cache(false); // Reset no-cache flag
    g_gxState.stateDirty = true;
  } else if (subCmd == GX_LOAD_AURORA_DESTROY_TEXOBJ) {
    CHECK(pos + 4 <= size, "GX_LOAD_AURORA_DESTROY_TEXOBJ read overrun");
    evict_texture_object(read_u32(data + pos, bigEndian));
    pos += 4;
  } else if (subCmd == GX_LOAD_AURORA_DESTROY_TLUT) {
    CHECK(pos + 4 <= size, "GX_LOAD_AURORA_DESTROY_TLUT read overrun");
    evict_tlut_object(read_u32(data + pos, bigEndian));
    pos += 4;
  } else if (subCmd == GX_LOAD_AURORA_DESTROY_COPY_TEX) {
    CHECK(pos + 8 <= size, "GX_LOAD_AURORA_DESTROY_COPY_TEX read overrun");
    evict_copy_texture(reinterpret_cast<const void*>(read_u64(data + pos, bigEndian)));
    pos += 8;
  } else if (subCmd == GX_LOAD_AURORA_INVALIDATE_TEX_ALL) {
    invalidate_static_texture_cache();
  } else if (subCmd == GX_LOAD_AURORA_DEBUG_GROUP_PUSH) {
    auto label = read_string(data, pos, size, bigEndian);
    gfx::push_debug_group(std::move(label));
  } else if (subCmd == GX_LOAD_AURORA_DEBUG_GROUP_POP) {
    aurora_pop_debug_group();
  } else if (subCmd == GX_LOAD_AURORA_DEBUG_MARKER_INSERT) {
    auto label = read_string(data, pos, size, bigEndian);
    gfx::insert_debug_marker(std::move(label));
  } else if (subCmd == GX_LOAD_AURORA_HOST_CALLBACK) {
    CHECK(pos + 8 <= size, "GX_LOAD_AURORA_HOST_CALLBACK read overrun");
    const u32 id = read_u32(data + pos, bigEndian);
    const u32 bytes = read_u32(data + pos + 4, bigEndian);
    pos += 8;
    CHECK(pos + bytes <= size, "GX_LOAD_AURORA_HOST_CALLBACK payload overrun");
    s_pendingHostCallback = {true, id, data + pos, bytes};
    pos += bytes;
  } else if (subCmd == GX_LOAD_AURORA_DEFERRED_DRAW) {
    CHECK(pos + 4 <= size, "GX_LOAD_AURORA_DEFERRED_DRAW read overrun");
    const u32 bytes = read_u32(data + pos, bigEndian);
    CHECK(pos + 4 + bytes <= size, "GX_LOAD_AURORA_DEFERRED_DRAW payload overrun");
    const auto expander = g_deferredDrawExpander.load(std::memory_order_acquire);
    CHECK(expander != nullptr, "GX_LOAD_AURORA_DEFERRED_DRAW without an expander");
    u32 wireBytes = 0;
    const u8* wire = expander(data + pos + 4, bytes, &wireBytes);
    // A StagingBatchFull thrown by handle_draw leaves pos at this command: the
    // caller retries it whole (the expansion runs again; it has no side effects).
    if (wire != nullptr && wireBytes >= 3) {
      u32 wirePos = 1;
      if (!handle_draw(wire[0], wire, wirePos, wireBytes, true)) return false;
    }
    pos += 4 + bytes;
  }

  else {
    static u32 unknownAuroraLogCount = 0;
    if (unknownAuroraLogCount < 16) {
      Log.warn("Unknown Aurora subcommand: {:04X}; stopping FIFO decode", subCmd);
      ++unknownAuroraLogCount;
    }
    return false;
  }
  return true;
}

} // namespace aurora::gx::fifo
