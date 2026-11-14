#include "render_policy.hpp"
#include "shader_info.hpp"
#include "uber.hpp"
#include "producer_profile.hpp"
#include "../gfx/post_process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#if !defined(_LIBCPP_VERSION)
#include <execution>
#endif
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#include <tracy/Tracy.hpp>

// Diagnostic environment switches are fixed for the lifetime of this process.
static bool CachedWiiCompiledTracePrimeGx() {
  static const bool enabled = std::getenv("WII_COMPILED_TRACE_PRIME_GX") != nullptr;
  return enabled;
}


namespace aurora::gx {
// TODO: remove, just for testing
bool enableLodBias = true;

namespace {
Module Log("aurora::gx");

bool is_alpha_bump_channel(GXChannelID id) { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

bool sampled_color_channel(GXChannelID id, size_t& out) {
  switch (id) {
  case GX_COLOR0:
  case GX_ALPHA0:
  case GX_COLOR0A0:
    out = 0;
    return true;
  case GX_COLOR1:
  case GX_ALPHA1:
  case GX_COLOR1A1:
    out = 1;
    return true;
  default:
    return false;
  }
}

Fog fog_uniform() {
  const auto& state = g_gxState.fog;
  Fog fog{
      .color = state.color,
      .a = state.aRaw,
      .b = static_cast<float>(state.bMagnitude),
      .c = state.c,
      .pad = static_cast<float>(state.bShift),
  };

  const u32 rangeBase = g_gxState.fogRange[0];
  const bool rangeEnabled = (rangeBase & (1u << 10)) != 0;
  float rangeWidth = std::abs(g_gxState.xfViewport[0]) * 2.0f;
  if (rangeWidth <= 0.0f) {
    rangeWidth = 1.0f;
  }
  const int rangeCenter = static_cast<int>(rangeBase & 0x3ffu) - 342;
  const float screenSpaceCenter = rangeEnabled ? ((static_cast<float>(rangeCenter) / rangeWidth) * 2.0f) - 1.0f
                                               : 0.0f;
  fog.rangeBase = {screenSpaceCenter, rangeEnabled ? rangeWidth : 1.0f, 0.0f, 0.0f};

  std::array<float, 12> rangeK{};
  size_t kIndex = 0;
  for (size_t i = 1; i < g_gxState.fogRange.size(); ++i) {
    const u32 packed = g_gxState.fogRange[i];
    rangeK[kIndex++] = static_cast<float>((packed >> 12) & 0xfffu) / 64.0f;
    rangeK[kIndex++] = static_cast<float>(packed & 0xfffu) / 64.0f;
  }
  rangeK[10] = rangeK[9];
  rangeK[11] = rangeK[9];
  for (size_t i = 0; i < fog.rangeK.size(); ++i) {
    fog.rangeK[i] = {rangeK[i * 4 + 0], rangeK[i * 4 + 1], rangeK[i * 4 + 2], rangeK[i * 4 + 3]};
  }
  return fog;
}

float sanitize_light_dir_component(float value) noexcept {
  u32 bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  if ((bits & 0x7f800000u) != 0x7f800000u) {
    return value;
  }
  if ((bits & 0x007fffffu) != 0) {
    return 0.0f;
  }
  return (bits & 0x80000000u) != 0 ? -1.0f : 1.0f;
}
} // namespace

Light prepare_shader_light(Light light) noexcept {
  if (std::fabs(light.distAtt[0]) < 0.00001f && std::fabs(light.distAtt[1]) < 0.00001f &&
      std::fabs(light.distAtt[2]) < 0.00001f) {
    light.distAtt[0] = 0.00001f;
  }

  const double normSq = static_cast<double>(light.dir[0]) * static_cast<double>(light.dir[0]) +
                        static_cast<double>(light.dir[1]) * static_cast<double>(light.dir[1]) +
                        static_cast<double>(light.dir[2]) * static_cast<double>(light.dir[2]);
  if (normSq == 0.0) {
    light.dir[0] = 0.0f;
    light.dir[1] = 0.0f;
    light.dir[2] = 0.0f;
    return light;
  }
  const double invNorm = 1.0 / std::sqrt(normSq);
  // Aurora is built with fast-math, under which std::isnan/std::isinf may be optimized away.
  light.dir[0] = sanitize_light_dir_component(static_cast<float>(light.dir[0] * invNorm));
  light.dir[1] = sanitize_light_dir_component(static_cast<float>(light.dir[1] * invNorm));
  light.dir[2] = sanitize_light_dir_component(static_cast<float>(light.dir[2] * invNorm));
  return light;
}

namespace {
Vec4<float> texture_size_bias(const gfx::TextureBind& tex) {
  auto width = static_cast<float>(tex.texObj.width());
  auto height = static_cast<float>(tex.texObj.height());
  const auto vpBias =
      enableLodBias && tex.ref && tex.ref->hasArbitraryMips
          ? log2(std::min(g_gxState.renderViewport.width / std::max(g_gxState.logicalViewport.width, 1.f),
                          g_gxState.renderViewport.height / std::max(g_gxState.logicalViewport.height, 1.f)))
          : 0.f;
  return {width, height, tex.texObj.lod_bias() + vpBias, 0.0f};
}

Vec4<float> texcoord_scale(const TexCoordScale& scale) {
  return {static_cast<float>(scale.scaleS) + 1.0f, static_cast<float>(scale.scaleT) + 1.0f,
          scale.biasS ? 1.0f : 0.0f, scale.biasT ? 1.0f : 0.0f};
}

void mark_texture_sample(const TevStageTextureDependency& dependency, ShaderInfo& info) {
  if (!dependency.canSampleTexture) {
    return;
  }
  info.sampledTexCoords.set(static_cast<size_t>(dependency.texCoordId));
  info.sampledTextures.set(static_cast<size_t>(dependency.texMapId));
}

void mark_tev_reg_read(GXTevRegID reg, bool alpha, ShaderInfo& info) {
  auto& writes = alpha ? info.writesTevRegAlpha : info.writesTevRegRgb;
  auto& loads = alpha ? info.loadsTevRegAlpha : info.loadsTevRegRgb;
  if (!writes.test(reg)) {
    loads.set(reg);
  }
}

void color_arg_reg_info(GXTevColorArg arg, const TevStage& stage,
                        const TevStageTextureDependency& textureDependency, ShaderInfo& info) {
  switch (arg) {
  case GX_CC_CPREV:
    mark_tev_reg_read(GX_TEVPREV, false, info);
    break;
  case GX_CC_APREV:
    mark_tev_reg_read(GX_TEVPREV, true, info);
    break;
  case GX_CC_C0:
    mark_tev_reg_read(GX_TEVREG0, false, info);
    break;
  case GX_CC_A0:
    mark_tev_reg_read(GX_TEVREG0, true, info);
    break;
  case GX_CC_C1:
    mark_tev_reg_read(GX_TEVREG1, false, info);
    break;
  case GX_CC_A1:
    mark_tev_reg_read(GX_TEVREG1, true, info);
    break;
  case GX_CC_C2:
    mark_tev_reg_read(GX_TEVREG2, false, info);
    break;
  case GX_CC_A2:
    mark_tev_reg_read(GX_TEVREG2, true, info);
    break;
  case GX_CC_TEXC:
  case GX_CC_TEXA:
    mark_texture_sample(textureDependency, info);
    break;
  case GX_CC_RASC:
  case GX_CC_RASA:
    if (stage.channelId != GX_COLOR_NULL && stage.channelId != GX_COLOR_ZERO &&
        !is_alpha_bump_channel(stage.channelId)) {
      size_t channel = 0;
      if (sampled_color_channel(stage.channelId, channel)) {
        info.sampledColorChannels.set(channel);
      }
    }
    break;
  case GX_CC_KONST:
    switch (stage.kcSel) {
    case GX_TEV_KCSEL_K0:
    case GX_TEV_KCSEL_K0_R:
    case GX_TEV_KCSEL_K0_G:
    case GX_TEV_KCSEL_K0_B:
    case GX_TEV_KCSEL_K0_A:
      info.sampledKColors.set(0);
      break;
    case GX_TEV_KCSEL_K1:
    case GX_TEV_KCSEL_K1_R:
    case GX_TEV_KCSEL_K1_G:
    case GX_TEV_KCSEL_K1_B:
    case GX_TEV_KCSEL_K1_A:
      info.sampledKColors.set(1);
      break;
    case GX_TEV_KCSEL_K2:
    case GX_TEV_KCSEL_K2_R:
    case GX_TEV_KCSEL_K2_G:
    case GX_TEV_KCSEL_K2_B:
    case GX_TEV_KCSEL_K2_A:
      info.sampledKColors.set(2);
      break;
    case GX_TEV_KCSEL_K3:
    case GX_TEV_KCSEL_K3_R:
    case GX_TEV_KCSEL_K3_G:
    case GX_TEV_KCSEL_K3_B:
    case GX_TEV_KCSEL_K3_A:
      info.sampledKColors.set(3);
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
}

void alpha_arg_reg_info(GXTevAlphaArg arg, const TevStage& stage,
                        const TevStageTextureDependency& textureDependency, ShaderInfo& info) {
  switch (arg) {
  case GX_CA_APREV:
    mark_tev_reg_read(GX_TEVPREV, true, info);
    break;
  case GX_CA_A0:
    mark_tev_reg_read(GX_TEVREG0, true, info);
    break;
  case GX_CA_A1:
    mark_tev_reg_read(GX_TEVREG1, true, info);
    break;
  case GX_CA_A2:
    mark_tev_reg_read(GX_TEVREG2, true, info);
    break;
  case GX_CA_TEXA:
    mark_texture_sample(textureDependency, info);
    break;
  case GX_CA_RASA:
    if (stage.channelId != GX_COLOR_NULL && stage.channelId != GX_COLOR_ZERO &&
        !is_alpha_bump_channel(stage.channelId)) {
      size_t channel = 0;
      if (sampled_color_channel(stage.channelId, channel)) {
        info.sampledColorChannels.set(channel);
      }
    }
    break;
  case GX_CA_KONST:
    switch (stage.kaSel) {
    case GX_TEV_KASEL_K0_R:
    case GX_TEV_KASEL_K0_G:
    case GX_TEV_KASEL_K0_B:
    case GX_TEV_KASEL_K0_A:
      info.sampledKColors.set(0);
      break;
    case GX_TEV_KASEL_K1_R:
    case GX_TEV_KASEL_K1_G:
    case GX_TEV_KASEL_K1_B:
    case GX_TEV_KASEL_K1_A:
      info.sampledKColors.set(1);
      break;
    case GX_TEV_KASEL_K2_R:
    case GX_TEV_KASEL_K2_G:
    case GX_TEV_KASEL_K2_B:
    case GX_TEV_KASEL_K2_A:
      info.sampledKColors.set(2);
      break;
    case GX_TEV_KASEL_K3_R:
    case GX_TEV_KASEL_K3_G:
    case GX_TEV_KASEL_K3_B:
    case GX_TEV_KASEL_K3_A:
      info.sampledKColors.set(3);
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
}
} // namespace

static ShaderInfo init_shader_info(const ShaderConfig& config) noexcept {

  ShaderInfo info{
      // vtx_start, current_pnmtx, render/logical viewport size, array_start, pad, proj
      .uniformSize = 4 + 4 + 8 + 8 + 8 + 48 + 64,
  };

  if (config.lineMode != 0) {
    info.uniformSize += 4 + 4 + 4 + 4; // line_width, line_aspect_y, line_tex_offset, line_texcoord_mask
    info.lineMode = config.lineMode;
  }

  for (int attr = 0; attr < config.attrs.size(); attr++) {
    const auto attrType = config.attrs[attr].attrType;
    if ((attr == GX_VA_PNMTXIDX || (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX)) && attrType == GX_DIRECT) {
      info.indexAttr.set(attr);
    } else if (attrType == GX_INDEX8 || attrType == GX_INDEX16) {
      info.indexAttr.set(attr);
    }
  }
  return info;
}

// Derives the matrix layout and the uniform size from the sets a scan (or an UberConfig) filled in.
static void finish_shader_info(const ShaderConfig& config, ShaderInfo& info) noexcept {
  const auto loadsTevRegs = info.loadsTevRegRgb | info.loadsTevRegAlpha;
  info.uniformSize += loadsTevRegs.count() * sizeof(Vec4<float>);
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (info.sampledColorChannels.test(i)) {
      const auto& cc = config.colorChannels[i];
      const auto& cca = config.colorChannels[i + GX_ALPHA0];
      if (cc.lightingEnabled || cca.lightingEnabled) {
        info.lightingEnabled = true;
      }
    }
  }
  if (info.lightingEnabled) {
    // Lights + light state for all channels
    info.uniformSize += 16 + sizeof(Light) * GX::MaxLights;
  }
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (info.sampledColorChannels.test(i)) {
      const auto& cc = config.colorChannels[i];
      if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
      if (cc.matSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
      const auto& cca = config.colorChannels[i + GX_ALPHA0];
      if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
      if (cca.matSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
    }
  }
  info.uniformSize += info.sampledKColors.count() * sizeof(Vec4<float>);
  info.uniformSize += info.konstStages.count() * sizeof(Vec4<float>);
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (config.dualTexEnabled && tcg.postMtx != GX_PTIDENTITY) {
      u32 postMtxIdx = (tcg.postMtx - GX_PTTEXMTX0) / 3;
      info.usesPTTexMtx.set(postMtxIdx);
    }
  }
  if (info.usesPTTexMtx.any())
    info.uniformSize += sizeof(Mat3x4<float>) * MaxPTTexMtx;

  // Resolve the matrix slots this shader can read.
  {
    auto& layout = info.matrixLayout;
    layout.postexRemap.fill(UniformMatrixLayout::kAbsent);

    const bool dynamicPnMtx = config.attrs[GX_VA_PNMTXIDX].attrType != GX_NONE;
    bool dynamicTexMtx = false;
    std::bitset<MaxPostexMtx> literalSlots;
    for (size_t i = 0; i < info.sampledTexCoords.size(); ++i) {
      if (!info.sampledTexCoords.test(i)) {
        continue;
      }
      const auto& tcg = config.tcgs[i];
      if (tcg.type != GX_TG_MTX2x4 && tcg.type != GX_TG_MTX3x4) {
        continue;
      }
      if (info.indexAttr.test(GX_VA_TEX0MTXIDX + i)) {
        // A per-vertex texture matrix index addresses raw matrix memory and can name any row, including the position rows.
        dynamicTexMtx = true;
        continue;
      }
      if (tcg.mtx == GX_IDENTITY) {
        continue;
      }
      const u32 slot = static_cast<u32>(tcg.mtx) / 3;
      if (slot >= MaxPostexMtx) {
        // Out of range for the shader array; preserve the uncompacted layout rather than inventing a different (still invalid) index.
        dynamicTexMtx = true;
        continue;
      }
      literalSlots.set(slot);
    }

    // At most MaxPostexMtx entries can ever be pushed: the absolute layout pushes the ten position slots plus at most ten texture slots, and the compacted layout pushes the current matrix plus at most one literal slot per texgen.
    const auto pushPostex = [&](u8 slot) {
      CHECK(layout.postexCount < layout.postexSlots.size(), "postex matrix layout overflow");
      layout.postexSlots[layout.postexCount++] = slot;
    };
    const auto pushNrm = [&](u8 slot) {
      CHECK(layout.nrmCount < layout.nrmSlots.size(), "normal matrix layout overflow");
      layout.nrmSlots[layout.nrmCount++] = slot;
    };

    layout.absolutePosRegion = dynamicPnMtx || dynamicTexMtx;
    if (!layout.absolutePosRegion) {
      // Compact slot 0 always holds the matrix selected by the current matrix index; build_uniform writes 0 into `current_pnmtx` to match.
      pushPostex(UniformMatrixLayout::kCurrentPnMtx);
      pushNrm(UniformMatrixLayout::kCurrentPnMtx);
    }
    for (u32 slot = 0; slot < MaxPnMtx; ++slot) {
      if (!layout.absolutePosRegion && !literalSlots.test(slot)) {
        continue;
      }
      layout.postexRemap[slot] = layout.postexCount;
      pushPostex(static_cast<u8>(slot));
      if (layout.absolutePosRegion) {
        // The normal array is indexed by the same expression as the position array, so its compact indices have to line up with the position slots.
        pushNrm(static_cast<u8>(slot));
      }
    }
    for (u32 slot = MaxPnMtx; slot < MaxPostexMtx; ++slot) {
      if (!dynamicTexMtx && !literalSlots.test(slot)) {
        continue;
      }
      layout.postexRemap[slot] = layout.postexCount;
      pushPostex(static_cast<u8>(slot));
    }
    // `postex_mtx[in_pnmtxidx]` and `nrm_mtx[in_pnmtxidx]` are emitted unconditionally, so neither array can ever be empty.
    info.uniformSize += sizeof(Mat3x4<float>) * (layout.postexCount + layout.nrmCount);
  }
  if (config.fogType != GX_FOG_NONE) {
    info.usesFog = true;
    info.uniformSize += sizeof(Fog);
  }
  if (info.usedIndTexMtxs.any()) {
    info.uniformSize += MaxIndTexMtxs * 2 * sizeof(Vec4<s32>);
  }
  info.uniformSize += info.sampledTexCoords.count() * sizeof(Vec4<float>);
  info.uniformSize += info.sampledTextures.count() * sizeof(Vec4<float>);
  if (config.dstAlpha != UINT32_MAX) {
    info.usesDstAlpha = true;
    info.uniformSize += sizeof(Vec4<float>);
  }
  // Enhanced-metal material parameters (not in ubershaders, which do not run the metal block).
  if (!info.uber && config.enhancedMetal != 0 && config.perPixelLighting != 0 && info.lightingEnabled) {
    info.usesMaterialExt = true;
    info.uniformSize += (MaterialExtFloats + LitExtFloats) * sizeof(float);
  }
  // Environment material (unlit lightmapped world surfaces): the same material block, plus the
  // borrowed or engine lights (array<Light, MaxEnvLights> and a vec4u mask) right after mat_ext.
  if (!info.uber && config.envMaterial != 0 && !info.lightingEnabled && env_lightmap_stage(config) >= 0) {
    info.usesMaterialExt = true;
    info.uniformSize += (MaterialExtFloats + LitExtFloats) * sizeof(float);
    info.usesEnvLights = true;
    info.uniformSize += sizeof(Light) * MaxEnvLights + 16;
  }
  info.uniformSize = gfx::align_uniform(info.uniformSize);
  if (info.uniformSize > MaxUniformSize) {
    Log.fatal("Uniform size exceeds maximum: {} > {}", info.uniformSize, MaxUniformSize);
  }
}

ShaderInfo build_shader_info(const ShaderConfig& config) noexcept {
  ZoneScoped;
  ShaderInfo info = init_shader_info(config);

  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    const auto textureDependency = tev_stage_texture_dependency(config, static_cast<u32>(i));
    // Color pass
    color_arg_reg_info(stage.colorPass.a, stage, textureDependency, info);
    color_arg_reg_info(stage.colorPass.b, stage, textureDependency, info);
    color_arg_reg_info(stage.colorPass.c, stage, textureDependency, info);
    color_arg_reg_info(stage.colorPass.d, stage, textureDependency, info);
    info.writesTevRegRgb.set(stage.colorOp.outReg);

    // Alpha pass
    alpha_arg_reg_info(stage.alphaPass.a, stage, textureDependency, info);
    alpha_arg_reg_info(stage.alphaPass.b, stage, textureDependency, info);
    alpha_arg_reg_info(stage.alphaPass.c, stage, textureDependency, info);
    alpha_arg_reg_info(stage.alphaPass.d, stage, textureDependency, info);
    info.writesTevRegAlpha.set(stage.alphaOp.outReg);
    const auto& cp = stage.colorPass;
    const auto& ap = stage.alphaPass;
    if (cp.a == GX_CC_KONST || cp.b == GX_CC_KONST || cp.c == GX_CC_KONST || cp.d == GX_CC_KONST ||
        ap.a == GX_CA_KONST || ap.b == GX_CA_KONST || ap.c == GX_CA_KONST || ap.d == GX_CA_KONST) {
      info.konstStages.set(i);
    }
  }
  if (const int zTexStage = tev_z_texture_stage(config); zTexStage >= 0) {
    mark_texture_sample(tev_stage_texture_dependency(config, static_cast<u32>(zTexStage)), info);
  }
  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    const auto textureDependency = tev_stage_texture_dependency(config, static_cast<u32>(i));

    // Coordinate state is independent of whether this stage has an enabled texture map.
    if (textureDependency.needsFixedTexcoordState && textureDependency.texCoordId >= 0) {
      info.sampledTexCoords.set(static_cast<size_t>(textureDependency.texCoordId));
    }

    // Skip if not enabled
    if (stage.indTexStage >= config.numIndStages) {
      continue;
    }
    const bool usesIndStage = stage.indTexMtxId != GX_ITM_OFF || is_alpha_bump_channel(stage.channelId);
    if (!usesIndStage) {
      continue;
    }
    info.usedIndStages.set(stage.indTexStage);
    const auto& indStage = config.indStages[stage.indTexStage];
    if (indStage.texMapId < MaxTextures) {
      info.sampledTextures.set(indStage.texMapId);
      info.sampledIndTextures.set(indStage.texMapId);
    }
    const int effectiveIndTexCoord = tev_effective_texcoord(config, indStage.texCoordId);
    if (effectiveIndTexCoord >= 0) {
      info.sampledTexCoords.set(static_cast<size_t>(effectiveIndTexCoord));
    }
    // Track which indirect matrix is used
    if (stage.indTexMtxId >= GX_ITM_0 && stage.indTexMtxId <= GX_ITM_2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_0);
    } else if (stage.indTexMtxId >= GX_ITM_S0 && stage.indTexMtxId <= GX_ITM_S2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_S0);
    } else if (stage.indTexMtxId >= GX_ITM_T0 && stage.indTexMtxId <= GX_ITM_T2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_T0);
    }
  }

  // Texgens can consume a colour channel even when TEV never reads it directly
  // (upstream aurora #291); include it before lighting and uniform sizing.
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) {
      continue;
    }
    if (tcg.src == GX_TG_COLOR0) {
      info.sampledColorChannels.set(0);
    } else if (tcg.src == GX_TG_COLOR1) {
      info.sampledColorChannels.set(1);
    }
  }

  finish_shader_info(config, info);
  return info;
}

ShaderInfo build_uber_shader_info(const UberConfig& uc, bool* sizeOk) noexcept {
  ZoneScoped;
  const ShaderConfig& config = uc.pipeline.shaderConfig;
  ShaderInfo info = init_shader_info(config);
  info.uber = true;
  info.sampledTexCoords = std::bitset<MaxTexCoord>(uc.texCoords);
  info.sampledTextures = std::bitset<MaxTextures>(uc.textures);
  info.sampledColorChannels = std::bitset<MaxColorChannels / 2>(uc.colorChannels);
  // The TEV program is data, so any register may be read before the first stage writes it.
  info.loadsTevRegRgb.set();
  info.loadsTevRegAlpha.set();
  if (uc.hasIndirect) {
    info.usedIndTexMtxs.set();
  }
  finish_shader_info(config, info);
  info.uniformSize = gfx::align_uniform(info.uniformSize + UberUniformBytes);
  if (sizeOk != nullptr) {
    *sizeOk = info.uniformSize <= MaxUniformSize;
  }
  return info;
}

static f32 tex_offset(GXTexOffset offs) noexcept {
  const auto raw = static_cast<u32>(underlying(offs));
  if (raw > static_cast<u32>(GX_TO_ONE)) {
    // The BP field (SU_LPSIZE) is 3 bits wide; the hardware treats the values past GX_TO_ONE like GX_TO_ONE.
    // RM3E01 wrote 6 in late-game effects (2026-10-25); do not abort for it.
    static bool warned = false;
    if (!warned) {
      warned = true;
      Log.warn("tex offset {} outside the GX enum; using 1.0 like the hardware", raw);
    }
  }
  return policy::tex_offset_value(raw);
}

static u32 point_texcoord_mask() noexcept {
  u32 mask = 0;
  for (int i = 0; i < MaxTexCoord; ++i) {
    if (g_gxState.texCoordScales[i].pointOffset) {
      mask |= 1 << i;
    }
  }
  return mask;
}

static u32 line_texcoord_mask() noexcept {
  u32 mask = 0;
  for (int i = 0; i < MaxTexCoord; ++i) {
    if (g_gxState.texCoordScales[i].lineOffset) {
      mask |= 1 << i;
    }
  }
  return mask;
}

namespace {
// Largest possible staged prefix: scalar head (80) + line/point block (16) + projection matrix + every matrix the uncompacted layout can hold.
constexpr size_t kStagedUniformBytes =
    96 + sizeof(Mat4x4<float>) + sizeof(Mat3x4<float>) * (MaxPostexMtx + MaxPnMtx);

// The host viewport always receives the normalized GX depth window (render_pass_impl clamps to minDepth <= maxDepth).
//
// Folds the near/far depth correction the vertex shader used to apply per-vertex directly into the
// projection matrix instead (matching upstream aurora commit 1dde08fa, "Move depth correction to
// projection matrix") - valid because the correction is a linear combination of the z/w rows, so
// applying it once here to the row is equivalent to applying it once per-vertex to the dot product,
// and it must be applied exactly once: doing it here AND in the shader (the previous bug) canceled
// the negation out for `flip`, silently making "reversed" Z behave identically to forward Z.
// `flip` decides which of the two single-application forms this draw needs: true bakes in the
// reversed-Z inversion (z' = -z), false bakes in the forward-Z near/far combination (z' = z + w) -
// exactly one always applies, never both, and never neither.
static Mat4x4<float> effective_projection() noexcept {
  const auto& vp = g_gxState.renderViewport;
  const bool flip = (vp.znear <= vp.zfar) == UseReversedZ;
  Mat4x4<float> proj = g_gxState.proj;
  for (size_t i = 0; i < 4; ++i) {
    proj.m2.m[i] = flip ? -proj.m2.m[i] : (proj.m2.m[i] + proj.m3.m[i]);
  }
  return proj;
}
} // namespace

UniformRanges build_uniform(const ShaderInfo& info, u32 vtxStart, const BindGroupRanges& ranges,
                            const FrameInterpolationDrawIdentity& drawIdentity, bool perspective,
                            uint16_t usedPnMtxMask, const UberData* uber) noexcept {
  producer_profile::Scope profile(producer_profile::Uniform);
  ZoneScoped;

  auto [buf, range] = gfx::map_uniform(info.uniformSize);

  const auto& layout = info.matrixLayout;
  // `postex_mtx[current_pnmtx]` indexes raw matrix memory, so a current-matrix index of 10 or more selects a texture matrix; the normal array only covers the position rows and clamps.
  const u32 currentPostexSlot = std::min<u32>(g_gxState.currentPnMtx, MaxPostexMtx - 1);
  const u32 currentNrmSlot = std::min<u32>(g_gxState.currentPnMtx, MaxPnMtx - 1);

  // The mapped uniform range lives in write-combine memory, which wants long sequential stores.
  alignas(16) std::array<uint8_t, kStagedUniformBytes> staged;
  size_t stagedSize = 0;
  const auto stage = [&](const void* data, size_t size) noexcept {
    std::memcpy(staged.data() + stagedSize, data, size);
    stagedSize += size;
  };
  const auto stage_u32 = [&](u32 value) noexcept { stage(&value, sizeof(value)); };
  const auto stage_f32 = [&](f32 value) noexcept { stage(&value, sizeof(value)); };

  stage_u32(vtxStart);
  // With a compacted position region the live matrix is uploaded to slot 0, so every `postex_mtx[in_pnmtxidx]` / `nrm_mtx[in_pnmtxidx]` read has to resolve to 0 as well.
  stage_u32(layout.absolutePosRegion ? g_gxState.currentPnMtx : 0u);
  stage_f32(g_gxState.renderViewport.width);
  stage_f32(g_gxState.renderViewport.height);
  stage_f32(g_gxState.logicalViewport.width);
  stage_f32(g_gxState.logicalViewport.height);
  // pad.x = enhanced metal strength (f32), pad.y = packed metal parameters; read only by
  // enhanced-metal shaders.
  stage_f32(g_drawMetal.active ? g_drawMetal.strength : g_enhancedMetalStrength.load(std::memory_order_relaxed));
  stage_u32(g_drawMetal.active && g_drawMetal.params != 0 ? g_drawMetal.params
                                                          : g_enhancedMetalParams.load(std::memory_order_relaxed));
  for (const auto& vaRange : ranges.vaRanges) {
    stage_u32(vaRange.offset);
  }
  if (info.lineMode != 0) {
    if (info.lineMode == 3) { // GX_POINTS
      stage_f32(static_cast<f32>(g_gxState.pointSize) / 6.f);
      stage_f32(1.0f);
      stage_f32(tex_offset(g_gxState.pointTexOffset));
      stage_u32(point_texcoord_mask());
    } else { // GX_LINES / GX_LINESTRIP
      stage_f32(static_cast<f32>(g_gxState.lineWidth) / 6.f);
      stage_f32(g_gxState.lineHalfAspect ? 0.5f : 1.f);
      stage_f32(tex_offset(g_gxState.lineTexOffset));
      stage_u32(line_texcoord_mask());
    }
  }
  const size_t projectionOffset = stagedSize;
  const Mat4x4<float> effectiveProj = effective_projection();
  stage(&effectiveProj, sizeof(effectiveProj));
  static unsigned primeUniformTrace = 0;
  if (CachedWiiCompiledTracePrimeGx() && primeUniformTrace < 8 &&
      g_gxState.loadedTextures[0].width() == 640 && g_gxState.numTexGens != 0) {
    ++primeUniformTrace;
    Log.info("[prime-uniform-proj] vtxstart={} current={} size={} rows={},{},{},{};{},{},{},{};{},{},{},{};{},{},{},{}",
             vtxStart,currentPostexSlot,info.uniformSize,
             effectiveProj.m0[0],effectiveProj.m0[1],effectiveProj.m0[2],effectiveProj.m0[3],
             effectiveProj.m1[0],effectiveProj.m1[1],effectiveProj.m1[2],effectiveProj.m1[3],
             effectiveProj.m2[0],effectiveProj.m2[1],effectiveProj.m2[2],effectiveProj.m2[3],
             effectiveProj.m3[0],effectiveProj.m3[1],effectiveProj.m3[2],effectiveProj.m3[3]);
  }

  const size_t positionOffset = stagedSize;
  // Texture matrices the draw reads, for frame interpolation (InterpolatedUniformLayout).
  std::array<uint32_t, InterpolatedUniformLayout::kMaxTextureMatrices> textureMatrixOffsets{};
  uint32_t textureMatrixCount = 0;
  for (u32 i = 0; i < layout.postexCount; ++i) {
    const u32 slot =
        layout.postexSlots[i] == UniformMatrixLayout::kCurrentPnMtx ? currentPostexSlot
                                                                    : layout.postexSlots[i];
    if (layout.postexSlots[i] != UniformMatrixLayout::kCurrentPnMtx && slot >= MaxPnMtx &&
        textureMatrixCount < textureMatrixOffsets.size()) {
      textureMatrixOffsets[textureMatrixCount++] = static_cast<uint32_t>(stagedSize);
    }
    stage(slot < MaxPnMtx ? &g_gxState.pnMtx[slot].pos : &g_gxState.texMtxs[slot - MaxPnMtx],
          sizeof(Mat3x4<float>));
  }

  const size_t normalOffset = stagedSize;
  for (u32 i = 0; i < layout.nrmCount; ++i) {
    const u32 slot = layout.nrmSlots[i] == UniformMatrixLayout::kCurrentPnMtx ? currentNrmSlot
                                                                             : layout.nrmSlots[i];
    stage(&g_gxState.pnMtx[slot].nrm, sizeof(Mat3x4<float>));
  }
  buf.append(staged.data(), stagedSize);

  const auto loadsTevRegs = info.loadsTevRegRgb | info.loadsTevRegAlpha;
  for (int i = 0; i < loadsTevRegs.size(); ++i) {
    if (loadsTevRegs.test(i)) {
      buf.append(g_gxState.colorRegs[i]);
    }
  }
  if (info.lightingEnabled) {
    // Sanitizing and normalizing light directions is substantially more expensive than copying the uniform data, while lights normally remain unchanged across many draws.
    static_assert(sizeof(g_gxState.lights) == 80 * GX::MaxLights);
    if (g_gxState.preparedLightsDirty) {
      for (size_t i = 0; i < g_gxState.preparedLights.size(); ++i) {
        g_gxState.preparedLights[i] = prepare_shader_light(g_gxState.lights[i]);
      }
      g_gxState.preparedLightsDirty = false;
    }
    buf.append(g_gxState.preparedLights);
    // Light state for all channels
    for (int i = 0; i < 4; ++i) {
      buf.append<u32>(g_gxState.colorChannelState[i].lightMask.to_ulong());
    }
  }
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (!info.sampledColorChannels.test(i)) {
      continue;
    }
    const auto& ccc = g_gxState.colorChannelConfig[i];
    const auto& ccs = g_gxState.colorChannelState[i];
    if (ccc.lightingEnabled && ccc.ambSrc == GX_SRC_REG) {
      buf.append(ccs.ambColor);
    }
    if (ccc.matSrc == GX_SRC_REG) {
      buf.append(ccs.matColor);
    }
    const auto& ccca = g_gxState.colorChannelConfig[i + GX_ALPHA0];
    const auto& ccsa = g_gxState.colorChannelState[i + GX_ALPHA0];
    if (ccca.lightingEnabled && ccca.ambSrc == GX_SRC_REG) {
      buf.append(ccsa.ambColor);
    }
    if (ccca.matSrc == GX_SRC_REG) {
      buf.append(ccsa.matColor);
    }
  }
  for (int i = 0; i < info.sampledKColors.size(); ++i) {
    if (info.sampledKColors.test(i)) {
      buf.append(g_gxState.kcolors[i]);
    }
  }
  // This draw's konst value per stage, from its live kcSel/kaSel (the key holds a canonical one).
  for (int i = 0; i < info.konstStages.size(); ++i) {
    if (info.konstStages.test(i)) {
      const auto& stage = g_gxState.tevStages[i];
      const auto fraction = [](u32 sel) { return static_cast<float>(8 - sel) / 8.f; };
      const auto component = [](u32 sel) { return g_gxState.kcolors[sel & 3u][(sel - 0x10u) >> 2]; };
      const u32 kc = stage.kcSel, ka = stage.kaSel;
      Vec4<float> value{0.f, 0.f, 0.f, 0.f};
      if (kc <= 0x07) {
        value = {fraction(kc), fraction(kc), fraction(kc), 0.f};
      } else if (kc >= 0x0C && kc <= 0x0F) {
        const auto& k = g_gxState.kcolors[kc & 3u];
        value = {k[0], k[1], k[2], 0.f};
      } else if (kc >= 0x10) {
        const float c = component(kc);
        value = {c, c, c, 0.f};
      } // 0x08-0x0B: unsourced, zero
      value[3] = ka <= 0x07 ? fraction(ka) : ka >= 0x10 ? component(ka) : 0.f;
      buf.append(value);
    }
  }
  if (info.usesPTTexMtx.any()) {
    for (int i = 0; i < info.usesPTTexMtx.size(); ++i) {
      if (info.usesPTTexMtx.test(i) && textureMatrixCount < textureMatrixOffsets.size()) {
        textureMatrixOffsets[textureMatrixCount++] = static_cast<uint32_t>(buf.size());
      }
      buf.append(g_gxState.ptTexMtxs[i]);
    }
  }
  if (info.usesFog) {
    buf.append(fog_uniform());
  }
  if (info.usedIndTexMtxs.any()) {
    for (int i = 0; i < MaxIndTexMtxs; ++i) {
      const auto& mtx = g_gxState.indTexMtxs[i];
      buf.append(Vec4<s32>{indirect_matrix_mantissa(mtx.mtx.m0.x), indirect_matrix_mantissa(mtx.mtx.m0.y),
                           indirect_matrix_mantissa(mtx.mtx.m1.x), indirect_matrix_mantissa(mtx.mtx.m1.y)});
      buf.append(Vec4<s32>{indirect_matrix_mantissa(mtx.mtx.m2.x), indirect_matrix_mantissa(mtx.mtx.m2.y),
                           indirect_matrix_shift(mtx.scaleExp),
                           g_smoothIndirect.load(std::memory_order_relaxed) ? 1 : 0});
    }
  }
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (info.sampledTexCoords.test(i)) {
      buf.append(texcoord_scale(g_gxState.texCoordScales[i]));
    }
  }
  for (int i = 0; i < info.sampledTextures.size(); ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    const auto& tex = get_texture(static_cast<GXTexMapID>(i));
    // CHECK(tex, "unbound texture {}", i);
    buf.append(texture_size_bias(tex));
  }
  if (info.usesDstAlpha) {
    // Destination alpha of this draw (the pipeline key only records that it applies).
    buf.append(Vec4<float>{static_cast<float>(g_gxState.dstAlpha & 0xFFu) / 255.f, 0.f, 0.f, 0.f});
  }
  if (info.usesMaterialExt) {
    // Material parameters of this draw's class (slot 0 = the generic enhanced metal; skin gets the
    // neutral set) and, in float 7, the animation time in seconds (wraps every hour).
    std::array<float, MaterialExtFloats> ext{};
    const int slot = g_drawMetal.active ? g_drawMetal.extSlot : 0;
    if (slot >= 0) {
      for (size_t i = 0; i < MaterialExtFloats; ++i) ext[i] = g_materialExt[slot][i].load(std::memory_order_relaxed);
    } else {
      for (size_t i = 0; i < 7; ++i) ext[i] = 1.0f;
    }
    static const auto start = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ext[7] = static_cast<float>(std::fmod(seconds, 3600.0));
    buf.append(ext.data(), sizeof(ext));
    // lit_ext: global lighting flags, then the frame's colour above and below the horizon (w = valid).
    std::array<float, LitExtFloats> lit{};
    lit[0] = g_softHighlights.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    lit[1] = g_linearBlend.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    lit[3] = g_smoothLightmaps.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    // [1].w: level 2 flags as a small integer (bit 0 texture relief, bit 1 game material masks).
    lit[7] = (g_textureRelief.load(std::memory_order_relaxed) ? 1.0f : 0.0f) +
             (g_gameMaterialMasks.load(std::memory_order_relaxed) ? 2.0f : 0.0f);
    if (g_areaReflections.load(std::memory_order_relaxed)) {
      float up[3], down[3];
      if (gfx::post::area_colours(up, down)) {
        lit[2] = 1.0f;
        lit[4] = up[0]; lit[5] = up[1]; lit[6] = up[2];
        lit[8] = down[0]; lit[9] = down[1]; lit[10] = down[2];
      }
    }
    // [2].w: scene relighting strength, only for the environment material (lightmapped surfaces).
    lit[11] = info.usesEnvLights ? g_worldRelight.load(std::memory_order_relaxed) : 0.0f;
    buf.append(lit.data(), sizeof(lit));
  }
  if (info.usesEnvLights) {
    // x: lights in use. y: 1 = the engine's own lights (modern lighting; range in color.a), 0 = the
    // borrowed lights (level 2, exactly as before). z: unused. w: bit 0 = dither the lightmap sample.
    std::array<Light, MaxEnvLights> lights{};
    uint32_t mask = 0, mode = 0, flags = 0;
    const uint32_t frame = gfx::current_frame();
    const auto& engine = g_engineEnvLights;
    if (g_engineLightsOption.load(std::memory_order_relaxed) && engine.count > 0 &&
        (engine.frame == frame || engine.frame + 1 == frame)) {
      for (uint32_t i = 0; i < engine.count && i < MaxEnvLights; ++i) {
        lights[i] = engine.lights[i];
        mask |= 1u << i;
      }
      mode = 1;
    } else if (g_envBorrowedLights.load(std::memory_order_relaxed)) {
      for (uint32_t i = 0; i < g_borrowedLights.count && i < MaxBorrowedLights; ++i) {
        lights[i] = g_borrowedLights.lights[i];
        mask |= 1u << i;
      }
    }
    if (g_lightmapDither.load(std::memory_order_relaxed)) flags |= 1u;
    buf.append(lights);
    buf.append<u32>(mask);
    buf.append<u32>(mode);
    buf.append<u32>(0);
    buf.append<u32>(flags);
  }
  if (info.uber) {
    // Ubershader block, in the order of the uber_* fields of the generated uniform struct.
    static const UberData empty{};
    const UberData& u = uber != nullptr ? *uber : empty;
    buf.append(u.misc0);
    buf.append(u.misc1);
    buf.append(u.swaps);
    buf.append(u.acmp);
    for (const auto& ind : u.ind) {
      buf.append(ind);
    }
    // This draw's konst value per stage, from its live kcSel/kaSel (same rule as the specialised konst<N>).
    for (u32 i = 0; i < MaxTevStages; ++i) {
      Vec4<float> value{0.f, 0.f, 0.f, 0.f};
      if (i < u.misc0[0]) {
        const auto& stage = g_gxState.tevStages[i];
        const auto fraction = [](u32 sel) { return static_cast<float>(8 - sel) / 8.f; };
        const auto component = [](u32 sel) { return g_gxState.kcolors[sel & 3u][(sel - 0x10u) >> 2]; };
        const u32 kc = stage.kcSel, ka = stage.kaSel;
        if (kc <= 0x07) {
          value = {fraction(kc), fraction(kc), fraction(kc), 0.f};
        } else if (kc >= 0x0C && kc <= 0x0F) {
          const auto& k = g_gxState.kcolors[kc & 3u];
          value = {k[0], k[1], k[2], 0.f};
        } else if (kc >= 0x10) {
          const float c = component(kc);
          value = {c, c, c, 0.f};
        }
        value[3] = ka <= 0x07 ? fraction(ka) : ka >= 0x10 ? component(ka) : 0.f;
      }
      buf.append(value);
    }
    buf.append(u.stages);
  }

  if (!perspective || frame_interpolation_fps() == 0) {
    g_gxState.stateDirty = false;
    return {
        .current = range,
        .interpolated = {},
    };
  }

  const auto interpolatedRanges = record_interpolation_draw(
      drawIdentity, effectiveProj, usedPnMtxMask,
      InterpolatedUniformLayout{
          .sourceUniformData = buf.data(),
          .uniformSize = range.size,
          .projectionOffset = projectionOffset,
          .positionOffset = positionOffset,
          .normalOffset = normalOffset,
          // A compacted position region holds the current matrix at slot 0.
          .currentMatrix = layout.absolutePosRegion
                               ? std::min<size_t>(g_gxState.currentPnMtx, MaxPnMtx - 1)
                               : 0,
          .indexedMatrices = info.indexAttr.test(GX_VA_PNMTXIDX),
          .textureMatrixCount = textureMatrixCount,
          .textureMatrixOffsets = textureMatrixOffsets,
      });
  g_gxState.stateDirty = false;
  return {
      .current = range,
      .interpolated = interpolatedRanges,
  };
}
} // namespace aurora::gx
