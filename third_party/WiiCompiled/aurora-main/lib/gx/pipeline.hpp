#pragma once

#include "../gfx/common.hpp"
#include "shader_info.hpp"

namespace aurora::gx {
struct DrawData {
  gfx::PipelineRef pipeline;
  // Same material keyed for per-vertex lighting (0 = none). Used while the
  // per-pixel pipeline is still compiling instead of skipping the draw.
  gfx::PipelineRef fallbackPipeline;
  gfx::Range vertRange;
  gfx::Range idxRange;
  gfx::Range uniformRange;
  std::array<gfx::Range, MaxInterpolatedFrames> interpolatedUniformRanges;
  uint32_t vtxCount;
  uint32_t indexCount;
  uint32_t instanceCount;
  bool expandedPrimitive;
  GXBindGroups bindGroups;
  uint32_t dstAlpha;
  // Ubershader bridge (0 = none): recorded only while the specialised pipeline was still compiling.
  gfx::PipelineRef uberPipeline = 0;
  gfx::Range uberUniformRange{};
  // Samples a texture of the SSAO exclusion list (Samus' face): no occlusion where it stays visible.
  bool ssaoExclude = false;
  // Transparent draw of the SSAO cover list (visor glass): no occlusion where it changes the colour.
  bool ssaoCover = false;
  // GX z-update was on: the draw may write depth (SSAO depth merges skip passes that cannot).
  bool depthWrite = true;
  // ... and the write may move a texel's depth away from the camera (compare off = Always, or a
  // GREATER / GEQUAL / NEQUAL / ALWAYS test): the SSAO depth merge cannot be deferred past it.
  bool depthWriteUnordered = true;
};

// F1 > Video > Ubershader (docs/ubershader_diseno.md). Off by default.
void set_uber_hybrid(bool enabled) noexcept;
bool uber_hybrid_enabled() noexcept;
// WII_COMPILED_UBER_FORCE=1: every draw uses its ubershader when it has one (validation of the ubershader itself).
bool uber_force() noexcept;
uint64_t uber_draws_total() noexcept;

constexpr uint32_t GXPipelineConfigVersion = 22;

constexpr GXFogType effective_pipeline_fog_type(GXFogType fogType, GXZTexOp zTextureOp,
                                                bool zCompLocBeforeTex, GXBlendMode blendMode,
                                                GXLogicOp logicOp) noexcept {
  const bool usesLateZTexture = zTextureOp != GX_ZT_DISABLE && !zCompLocBeforeTex;
  const bool usesUnsupportedLogicFog =
      blendMode == GX_BM_LOGIC && logicOp != GX_LO_OR;
  return usesLateZTexture && usesUnsupportedLogicFog ? GX_FOG_NONE : fogType;
}

struct PipelineConfig {
  uint32_t version = GXPipelineConfigVersion;
  uint32_t msaaSamples = 1;
  ShaderConfig shaderConfig;
  GXCompare depthFunc;
  GXCullMode cullMode;
  GXBlendMode blendMode;
  GXBlendFactor blendFacSrc, blendFacDst;
  GXLogicOp blendOp;
  GXPixelFmt pixelFmt;
  uint32_t dstAlpha;
  bool depthCompare, depthUpdate, alphaUpdate, colorUpdate;
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

// GX destination alpha replaces the written alpha while blending still reads the
// TEV alpha. When the color blend does not read source alpha, the fragment
// writes the value itself; the blend-constant path left the alpha at 0 for
// Prime's alpha-only interaction-highlight marking (no button outline).
// Version 21: the destination alpha *value* is draw state (uniform and blend
// constant), so the config only records whether it applies: 0 = enabled,
// UINT32_MAX = none. RM3E01 animates it (0, 4, 8 ... 80), which with the value
// in the key made up to 25 pipelines of one material and new ones on every visit.
inline void normalize_dst_alpha_key(PipelineConfig& config) {
  if (config.dstAlpha != UINT32_MAX) config.dstAlpha = 0;
  if (config.shaderConfig.dstAlpha != UINT32_MAX) config.shaderConfig.dstAlpha = 0;
}

// Version 22: the TEV konst selection (fixed fraction or K0-K3 component) is draw state
// too. Shaders read each stage's konst value from a per-stage uniform (konst<N>), so the
// key holds one canonical selection. Materials differing only in kcSel/kaSel (about 2000
// near-duplicate RM3E01 pipelines) share one pipeline.
constexpr GXTevKColorSel KeyKColorSel = static_cast<GXTevKColorSel>(0x08);
constexpr GXTevKAlphaSel KeyKAlphaSel = static_cast<GXTevKAlphaSel>(0x08);
inline void normalize_konst_key(PipelineConfig& config) {
  for (auto& stage : config.shaderConfig.tevStages) {
    stage.kcSel = KeyKColorSel;
    stage.kaSel = KeyKAlphaSel;
  }
}

// Every draw-state field the key drops (see normalize_dst_alpha_key / normalize_konst_key).
inline void normalize_pipeline_key(PipelineConfig& config) {
  normalize_dst_alpha_key(config);
  normalize_konst_key(config);
}

inline uint32_t shader_dst_alpha(const PipelineConfig& config) {
  if (config.dstAlpha == UINT32_MAX) return UINT32_MAX;
  const auto readsSourceAlpha = [](GXBlendFactor factor) {
    return factor == GX_BL_SRCALPHA || factor == GX_BL_INVSRCALPHA;
  };
  const bool colorReadsSourceAlpha = config.colorUpdate && config.blendMode == GX_BM_BLEND &&
                                     (readsSourceAlpha(config.blendFacSrc) || readsSourceAlpha(config.blendFacDst));
  return colorReadsSourceAlpha ? UINT32_MAX : config.dstAlpha;
}

inline bool valid_pipeline_config(const PipelineConfig& config) noexcept {
  const auto in_range = [](auto value, auto maximum) {
    using Value = decltype(value);
    return static_cast<std::underlying_type_t<Value>>(value) >= 0 &&
           static_cast<std::underlying_type_t<Value>>(value) <=
               static_cast<std::underlying_type_t<Value>>(maximum);
  };
  const bool validSamples = config.msaaSamples == 1 || config.msaaSamples == 2 ||
                            config.msaaSamples == 4 || config.msaaSamples == 8;
  // A key decoded from a misaligned stream once reached the cache with no
  // position attribute; every later boot compiled it and hit the shader
  // generator's "unmapped vtx attr" fatal. Such a key is never a real draw.
  bool validAttrs = config.shaderConfig.attrs[GX_VA_POS].attrType != GX_NONE;
  for (const auto& attr : config.shaderConfig.attrs) {
    validAttrs = validAttrs && attr.attrType <= GX_INDEX16;
  }
  // Reserved bits are zero in a real recipe. An ubershader key (uber.hpp) sets them; it is derived from
  // the recipes again each boot and must never be loaded as one (its TEV program is empty).
  const bool reservedClear = config.shaderConfig.pad1 == 0;
  return config.version == GXPipelineConfigVersion && validSamples && validAttrs && reservedClear &&
         in_range(config.depthFunc, GX_ALWAYS) && in_range(config.cullMode, GX_CULL_ALL) &&
         in_range(config.blendMode, GX_BM_SUBTRACT) &&
         in_range(config.blendFacSrc, GX_BL_INVDSTALPHA) &&
         in_range(config.blendFacDst, GX_BL_INVDSTALPHA) &&
         in_range(config.blendOp, GX_LO_SET) && in_range(config.pixelFmt, GX_PF_YUV420);
}

wgpu::RenderPipeline create_pipeline([[maybe_unused]] const PipelineConfig& config);
void clear_shader_module_cache();

// Per-pass encoder state carried across the draws of one render pass, so the replay loop can elide Dawn calls that would re-bind what is already bound.
struct DrawEncodeState {
  gfx::PipelineRef currentPipeline = UINTPTR_MAX;
  // The bind group currently occupying slot 2.
  WGPUBindGroup boundTextureBindGroup = nullptr;
  // The pass-wide index buffer binding is established lazily by the first indexed draw; every draw then addresses it with firstIndex instead of a per-draw SetIndexBuffer.
  bool indexBufferBound = false;
};

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass, DrawEncodeState& state,
            bool requireReadyPipeline, const gfx::Range* uniformRangeOverride = nullptr);

void queue_surface(const u8* dlStart, uint32_t dlSize, bool bigEndian) noexcept;
} // namespace aurora::gx
