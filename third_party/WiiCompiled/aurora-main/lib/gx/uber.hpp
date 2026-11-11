#pragma once
// Ubershader support types (docs/ubershader_diseno.md). Header-only and free of GPU/global state so the
// golden and unit tests can use it.
//
// An UberConfig is a PipelineConfig whose TEV program has been erased: every TEV-derived choice the
// WGSL generator would otherwise bake in (stages, swap table, alpha compare, indirect stages) is read
// from the draw's uniform buffer instead. What stays in the key is what changes the vertex shader, the
// bind groups or the varyings: vertex format, texgens, colour channels, fog type, z texture, blend/depth
// state, and four masks taken from the real material (which texcoords, textures and colour channels
// its TEV reads), so the uniform layout and the texture bind group match the specialised shader's.
#include "pipeline.hpp"

#include <algorithm>

namespace aurora::gx {

struct UberConfig {
  PipelineConfig pipeline;
  u8 texCoords = 0;      // ShaderInfo::sampledTexCoords of the real material
  u8 textures = 0;       // ShaderInfo::sampledTextures
  u8 colorChannels = 0;  // ShaderInfo::sampledColorChannels
  u8 hasIndirect = 0;    // ShaderInfo::usedIndTexMtxs.any()
};
static_assert(std::has_unique_object_representations_v<UberConfig>);

// Everything the ubershader reads for one material, packed exactly as the WGSL decodes it
// (shader.cpp, `uber_*` uniform fields). Per-draw values (the konst colours) are added by build_uniform.
struct UberData {
  std::array<u32, MaxTevStages * 4> stages{};  // 4 words per stage, see make_uber_data
  std::array<u32, 4> misc0{};                  // stage count, uses fixed texcoord state, z-texture stage + 1, last outputs
  std::array<u32, 4> misc1{};                  // indirect stage mask, alpha compare op
  std::array<u32, 4> swaps{};                  // four TEV swap entries, 2 bits per channel
  std::array<u32, 4> acmp{};                   // comp0, ref0, comp1, ref1
  std::array<std::array<u32, 4>, MaxIndStages> ind{};  // texcoord + 1, texmap + 1, shift S, shift T
};

// Bytes the uber block adds to the uniform buffer, after the fields every shader has:
// misc0, misc1, swaps, acmp (16 each), ind[4] (16 each), konst[16] (16 each), stages (4 words each).
constexpr u32 UberUniformBytes = 16 * 4 + 16 * MaxIndStages + 16 * MaxTevStages + MaxTevStages * 4 * 4;

inline UberConfig make_uber_config(const PipelineConfig& real, const ShaderInfo& info) noexcept {
  UberConfig out{};
  out.pipeline = real;
  auto& sc = out.pipeline.shaderConfig;
  sc.tevStages = {};
  sc.tevSwapTable = {};
  sc.tevStageCount = 0;
  sc.alphaCompare = {};
  sc.indStages = {};
  sc.numIndStages = 0;
  // Over-approximate what only changes uniform size: every texcoord the material defines, and every texture
  // slot (the texture bind group always has all eight; slots the material does not use hold an empty view).
  // (Bump texgens produce no coordinate of their own: the vertex stage has nothing to emit for them.)
  u32 definedTexCoords = 0;
  for (u32 i = 0; i < real.shaderConfig.numTexGens && i < MaxTexCoord; ++i) {
    const auto type = real.shaderConfig.tcgs[i].type;
    if (type == GX_TG_MTX2x4 || type == GX_TG_MTX3x4 || type == GX_TG_SRTG) definedTexCoords |= 1u << i;
  }
  out.texCoords = static_cast<u8>(info.sampledTexCoords.to_ulong() | definedTexCoords);
  out.textures = 0xFF;
  out.colorChannels = static_cast<u8>(info.sampledColorChannels.to_ulong());
  out.hasIndirect = info.usedIndTexMtxs.any() ? 1 : 0;
  return out;
}

// An UberConfig travels through the ordinary pipeline cache as a PipelineConfig: the shader config carries
// a marker bit (pad1 = 1) and the four masks in the otherwise unused TevStage padding bytes. That lets the
// existing queue, persistence (pipeline_cache.db) and boot prewarm handle ubershaders unchanged.
inline PipelineConfig encode_uber_pipeline(const UberConfig& uber) noexcept {
  PipelineConfig out = uber.pipeline;
  auto& sc = out.shaderConfig;
  sc.pad1 = 1;
  sc.tevStages[0]._p1 = uber.texCoords;
  sc.tevStages[0]._p2 = uber.textures;
  sc.tevStages[1]._p1 = uber.colorChannels;
  sc.tevStages[1]._p2 = uber.hasIndirect;
  return out;
}

inline bool is_uber_pipeline(const PipelineConfig& config) noexcept { return config.shaderConfig.pad1 == 1; }

inline UberConfig decode_uber_pipeline(const PipelineConfig& config) noexcept {
  UberConfig out{};
  out.pipeline = config;
  auto& sc = out.pipeline.shaderConfig;
  out.texCoords = sc.tevStages[0]._p1;
  out.textures = sc.tevStages[0]._p2;
  out.colorChannels = sc.tevStages[1]._p1;
  out.hasIndirect = sc.tevStages[1]._p2;
  sc.pad1 = 0;
  sc.tevStages[0]._p1 = sc.tevStages[0]._p2 = sc.tevStages[1]._p1 = sc.tevStages[1]._p2 = 0;
  return out;
}

namespace uber_detail {
// 0 = no colour (zero/null), 1/2 = rasterised colour channel 0/1, 3 = alpha bump, 4 = normalised alpha bump
inline u32 channel_kind(GXChannelID id) noexcept {
  switch (id) {
  case GX_COLOR0:
  case GX_ALPHA0:
  case GX_COLOR0A0:
    return 1;
  case GX_COLOR1:
  case GX_ALPHA1:
  case GX_COLOR1A1:
    return 2;
  case GX_ALPHA_BUMP:
    return 3;
  case GX_ALPHA_BUMPN:
    return 4;
  default:
    return 0;
  }
}
inline u32 op_word(const TevOp& op) noexcept {
  return (static_cast<u32>(op.op) & 15u) | ((static_cast<u32>(op.bias) & 3u) << 4) |
         ((static_cast<u32>(op.scale) & 3u) << 6) | (op.clamp ? 1u << 8 : 0u) |
         ((static_cast<u32>(op.outReg) & 3u) << 9);
}
} // namespace uber_detail

// Packs the TEV program of a specialised ShaderConfig. Mirrors, field by field, the decisions the
// specialised generator and build_shader_info make, so both shaders see the same material.
inline UberData make_uber_data(const ShaderConfig& c) noexcept {
  UberData d{};
  const u32 count = std::min<u32>(c.tevStageCount, MaxTevStages);
  const int zTexStage = tev_z_texture_stage(c);
  u32 indUsed = 0;
  for (u32 i = 0; i < count; ++i) {
    const auto& st = c.tevStages[i];
    const auto dep = tev_stage_texture_dependency(c, i);
    const bool needsSample = dep.combinerUsesTexture || static_cast<int>(i) == zTexStage;
    const bool willSample = tev_texture_sample_enabled(dep, needsSample);
    const bool hasIndStage = st.indTexStage < c.numIndStages;
    const bool hasIndirect = st.indTexMtxId != GX_ITM_OFF && hasIndStage;
    const u32 kind = uber_detail::channel_kind(st.channelId);
    if (hasIndStage && (st.indTexMtxId != GX_ITM_OFF || kind == 3 || kind == 4)) {
      indUsed |= 1u << static_cast<u32>(st.indTexStage);
    }
    u32 w[5]{};
    w[0] = (static_cast<u32>(st.colorPass.a) & 15u) | ((static_cast<u32>(st.colorPass.b) & 15u) << 4) |
           ((static_cast<u32>(st.colorPass.c) & 15u) << 8) | ((static_cast<u32>(st.colorPass.d) & 15u) << 12) |
           ((static_cast<u32>(st.alphaPass.a) & 7u) << 16) | ((static_cast<u32>(st.alphaPass.b) & 7u) << 19) |
           ((static_cast<u32>(st.alphaPass.c) & 7u) << 22) | ((static_cast<u32>(st.alphaPass.d) & 7u) << 25);
    w[1] = uber_detail::op_word(st.colorOp);
    w[2] = uber_detail::op_word(st.alphaOp);
    w[3] = static_cast<u32>(dep.texCoordId + 1) | (static_cast<u32>(dep.texMapId + 1) << 4) | (kind << 8) |
           ((static_cast<u32>(st.tevSwapRas) & 3u) << 12) | ((static_cast<u32>(st.tevSwapTex) & 3u) << 14) |
           (dep.canSampleTexture ? 1u << 16 : 0u) | (dep.needsFixedTexcoordState ? 1u << 17 : 0u) |
           (willSample ? 1u << 18 : 0u) | (hasIndirect ? 1u << 19 : 0u) | (st.indTexAddPrev ? 1u << 20 : 0u) |
           (hasIndStage ? 1u << 21 : 0u);
    w[4] = (static_cast<u32>(st.indTexStage) & 3u) | ((static_cast<u32>(st.indTexFormat) & 3u) << 2) |
           ((static_cast<u32>(st.indTexBiasSel) & 7u) << 4) | ((static_cast<u32>(st.indTexAlphaSel) & 3u) << 7) |
           ((static_cast<u32>(st.indTexMtxId) & 15u) << 9) | ((static_cast<u32>(st.indTexWrapS) & 7u) << 13) |
           ((static_cast<u32>(st.indTexWrapT) & 7u) << 16);
    // Word 0: arguments. Word 1: colour op (11 bits) | alpha op (11 bits) << 11. Words 2 and 3: texture and indirect state.
    d.stages[i * 4 + 0] = w[0];
    d.stages[i * 4 + 1] = w[1] | (w[2] << 11);
    d.stages[i * 4 + 2] = w[3];
    d.stages[i * 4 + 3] = w[4];
  }
  const u32 lastColorOut = count ? static_cast<u32>(c.tevStages[count - 1].colorOp.outReg) & 3u : 0u;
  const u32 lastAlphaOut = count ? static_cast<u32>(c.tevStages[count - 1].alphaOp.outReg) & 3u : 0u;
  d.misc0 = {count, shader_uses_fixed_texcoord_state(c) ? 1u : 0u, static_cast<u32>(zTexStage + 1),
             lastColorOut | (lastAlphaOut << 4)};
  d.misc1 = {indUsed, static_cast<u32>(c.alphaCompare.op) & 3u, 0u, 0u};
  for (u32 i = 0; i < MaxTevSwap; ++i) {
    const auto& s = c.tevSwapTable[i];
    d.swaps[i] = (static_cast<u32>(s.red) & 3u) | ((static_cast<u32>(s.green) & 3u) << 2) |
                 ((static_cast<u32>(s.blue) & 3u) << 4) | ((static_cast<u32>(s.alpha) & 3u) << 6);
  }
  if (c.alphaCompare) {
    d.acmp = {static_cast<u32>(c.alphaCompare.comp0), c.alphaCompare.ref0 & 0xFFu,
              static_cast<u32>(c.alphaCompare.comp1), c.alphaCompare.ref1 & 0xFFu};
  } else {
    // Disabled in the specialised shader means no discard at all (XOR of two ALWAYS would discard).
    d.acmp = {static_cast<u32>(GX_ALWAYS), 0u, static_cast<u32>(GX_ALWAYS), 0u};
    d.misc1[1] = static_cast<u32>(GX_AOP_AND);
  }
  const auto shiftOf = [](GXIndTexScale s) { return std::min<u32>(static_cast<u32>(s), 8u); };
  for (u32 i = 0; i < MaxIndStages; ++i) {
    const auto& ind = c.indStages[i];
    const int tc = tev_effective_texcoord(c, ind.texCoordId);
    const u32 tm = static_cast<u32>(ind.texMapId);
    d.ind[i] = {static_cast<u32>(tc + 1), tm < MaxTextures ? tm + 1 : 0u, shiftOf(ind.scaleS), shiftOf(ind.scaleT)};
  }
  return d;
}

} // namespace aurora::gx
