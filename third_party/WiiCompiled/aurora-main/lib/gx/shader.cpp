#include "../gfx/common.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "gx.hpp"
#include "uber.hpp"
#include "gx_fmt.hpp"
#include "shader_info.hpp"

#include <dolphin/gx/GXEnum.h>

#include <array>
#include <set>
#include <atomic>
#include <memory>
#include <string>
#include <mutex>
#include <string_view>
#include <utility>

#include "tracy/Tracy.hpp"

namespace aurora::gx {
using namespace fmt::literals;
using namespace std::string_literals;
using namespace std::string_view_literals;

static Module Log("aurora::gfx::gx");


static inline std::string_view chan_comp(GXTevColorChan chan) noexcept {
  switch (chan) {
  case GX_CH_RED:
    return "r";
  case GX_CH_GREEN:
    return "g";
  case GX_CH_BLUE:
    return "b";
  case GX_CH_ALPHA:
    return "a";
  default:
    return "?";
  }
}

static bool is_alpha_bump_channel(GXChannelID id) noexcept { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

static std::string_view disabled_texture_color(const ShaderConfig& config) noexcept {
  return config.numTexGens == 0 ? "vec3f(0.0)"sv : "vec3f(1.0)"sv;
}

static std::string_view disabled_texture_alpha(const ShaderConfig& config) noexcept {
  return config.numTexGens == 0 ? "0.0"sv : "1.0"sv;
}

static std::string tev_mask_expr(const std::string& value, u32 mask) {
  // t_IndTexCoord is already expanded into the 0..255 indirect sample domain.
  return fmt::format("(f32(u32({}) & 0x{:X}u) / 255.0)", value, mask);
}

static std::string alpha_bump_sel(size_t stageIdx, const ShaderConfig& config, const TevStage& stage) {
  if (stage.indTexStage >= config.numIndStages || stage.indTexAlphaSel == GX_ITBA_OFF) {
    return "0.0";
  }

  std::string baseCoord;
  switch (stage.indTexAlphaSel) {
    DEFAULT_FATAL("invalid indTexAlphaSel {} for stage {}", underlying(stage.indTexAlphaSel), stageIdx);
  case GX_ITBA_S:
    baseCoord = fmt::format("t_IndTexCoord{}.x", underlying(stage.indTexStage));
    break;
  case GX_ITBA_T:
    baseCoord = fmt::format("t_IndTexCoord{}.y", underlying(stage.indTexStage));
    break;
  case GX_ITBA_U:
    baseCoord = fmt::format("t_IndTexCoord{}.z", underlying(stage.indTexStage));
    break;
  case GX_ITBA_OFF:
    return "0.0";
  }

  switch (stage.indTexFormat) {
    DEFAULT_FATAL("invalid indirect format {} for stage {}", underlying(stage.indTexFormat), stageIdx);
  case GX_ITF_8:
    return tev_mask_expr(baseCoord, 0xF8u);
  case GX_ITF_5:
    return tev_mask_expr(baseCoord, 0xE0u);
  case GX_ITF_4:
    return tev_mask_expr(baseCoord, 0xF0u);
  case GX_ITF_3:
    return tev_mask_expr(baseCoord, 0xF8u);
  }
}

u8 color_channel(GXChannelID id) noexcept {
  switch (id) {
    DEFAULT_FATAL("unimplemented color channel {}", id);
  case GX_COLOR0:
  case GX_ALPHA0:
  case GX_COLOR0A0:
    return 0;
  case GX_COLOR1:
  case GX_ALPHA1:
  case GX_COLOR1A1:
    return 1;
  }
}

static std::string color_arg_reg(GXTevColorArg arg, size_t stageIdx, const ShaderConfig& config,
                                 const TevStage& stage) {
  switch (arg) {
    DEFAULT_FATAL("invalid color arg {}", underlying(arg));
  case GX_CC_CPREV:
    return "prev.rgb";
  case GX_CC_APREV:
    return "vec3f(prev.a)";
  case GX_CC_C0:
    return "tevreg0.rgb";
  case GX_CC_A0:
    return "vec3f(tevreg0.a)";
  case GX_CC_C1:
    return "tevreg1.rgb";
  case GX_CC_A1:
    return "vec3f(tevreg1.a)";
  case GX_CC_C2:
    return "tevreg2.rgb";
  case GX_CC_A2:
    return "vec3f(tevreg2.a)";
  case GX_CC_TEXC: {
    if (!tev_stage_texture_dependency(config, static_cast<u32>(stageIdx)).canSampleTexture) {
      return std::string(disabled_texture_color(config));
    }
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("sampled{}.{}{}{}", stageIdx, chan_comp(swap.red), chan_comp(swap.green), chan_comp(swap.blue));
  }
  case GX_CC_TEXA: {
    if (!tev_stage_texture_dependency(config, static_cast<u32>(stageIdx)).canSampleTexture) {
      return fmt::format("vec3f({})", disabled_texture_alpha(config));
    }
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("vec3f(sampled{}.{})", stageIdx, chan_comp(swap.alpha));
  }
  case GX_CC_RASC: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "vec3f(0.0)";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return fmt::format("vec3f({})", alpha);
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("rast{}.{}{}{}", idx, chan_comp(swap.red), chan_comp(swap.green), chan_comp(swap.blue));
  }
  case GX_CC_RASA: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "vec3f(0.0)";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return fmt::format("vec3f({})", alpha);
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("vec3f(rast{}.{})", idx, chan_comp(swap.alpha));
  }
  case GX_CC_ONE:
    return "vec3f(1.0)";
  case GX_CC_HALF:
    return "vec3f(0.5)";
  case GX_CC_KONST: {
    // The selected value is per-draw uniform state (version 22, normalize_konst_key).
    return fmt::format("ubuf.konst{}.rgb", stageIdx);
    switch (stage.kcSel) {
      DEFAULT_FATAL("invalid kcSel {}", underlying(stage.kcSel));
    // Unsourced encodings evaluate to zero in hardware.
    case static_cast<GXTevKColorSel>(0x08):
    case static_cast<GXTevKColorSel>(0x09):
    case static_cast<GXTevKColorSel>(0x0A):
    case static_cast<GXTevKColorSel>(0x0B):
      return "vec3f(0.0)";
    case GX_TEV_KCSEL_8_8:
      return "vec3f(1.0)";
    case GX_TEV_KCSEL_7_8:
      return "vec3f(7.0/8.0)";
    case GX_TEV_KCSEL_6_8:
      return "vec3f(6.0/8.0)";
    case GX_TEV_KCSEL_5_8:
      return "vec3f(5.0/8.0)";
    case GX_TEV_KCSEL_4_8:
      return "vec3f(4.0/8.0)";
    case GX_TEV_KCSEL_3_8:
      return "vec3f(3.0/8.0)";
    case GX_TEV_KCSEL_2_8:
      return "vec3f(2.0/8.0)";
    case GX_TEV_KCSEL_1_8:
      return "vec3f(1.0/8.0)";
    case GX_TEV_KCSEL_K0:
      return "ubuf.kcolor0.rgb";
    case GX_TEV_KCSEL_K1:
      return "ubuf.kcolor1.rgb";
    case GX_TEV_KCSEL_K2:
      return "ubuf.kcolor2.rgb";
    case GX_TEV_KCSEL_K3:
      return "ubuf.kcolor3.rgb";
    case GX_TEV_KCSEL_K0_R:
      return "vec3f(ubuf.kcolor0.r)";
    case GX_TEV_KCSEL_K1_R:
      return "vec3f(ubuf.kcolor1.r)";
    case GX_TEV_KCSEL_K2_R:
      return "vec3f(ubuf.kcolor2.r)";
    case GX_TEV_KCSEL_K3_R:
      return "vec3f(ubuf.kcolor3.r)";
    case GX_TEV_KCSEL_K0_G:
      return "vec3f(ubuf.kcolor0.g)";
    case GX_TEV_KCSEL_K1_G:
      return "vec3f(ubuf.kcolor1.g)";
    case GX_TEV_KCSEL_K2_G:
      return "vec3f(ubuf.kcolor2.g)";
    case GX_TEV_KCSEL_K3_G:
      return "vec3f(ubuf.kcolor3.g)";
    case GX_TEV_KCSEL_K0_B:
      return "vec3f(ubuf.kcolor0.b)";
    case GX_TEV_KCSEL_K1_B:
      return "vec3f(ubuf.kcolor1.b)";
    case GX_TEV_KCSEL_K2_B:
      return "vec3f(ubuf.kcolor2.b)";
    case GX_TEV_KCSEL_K3_B:
      return "vec3f(ubuf.kcolor3.b)";
    case GX_TEV_KCSEL_K0_A:
      return "vec3f(ubuf.kcolor0.a)";
    case GX_TEV_KCSEL_K1_A:
      return "vec3f(ubuf.kcolor1.a)";
    case GX_TEV_KCSEL_K2_A:
      return "vec3f(ubuf.kcolor2.a)";
    case GX_TEV_KCSEL_K3_A:
      return "vec3f(ubuf.kcolor3.a)";
    }
  }
  case GX_CC_ZERO:
    return "vec3f(0.0)";
  }
}

static std::string alpha_arg_reg(GXTevAlphaArg arg, size_t stageIdx, const ShaderConfig& config,
                                 const TevStage& stage) {
  switch (arg) {
    DEFAULT_FATAL("invalid alpha arg {}", underlying(arg));
  case GX_CA_APREV:
    return "prev.a";
  case GX_CA_A0:
    return "tevreg0.a";
  case GX_CA_A1:
    return "tevreg1.a";
  case GX_CA_A2:
    return "tevreg2.a";
  case GX_CA_TEXA: {
    if (!tev_stage_texture_dependency(config, static_cast<u32>(stageIdx)).canSampleTexture) {
      return std::string(disabled_texture_alpha(config));
    }
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("sampled{}.{}", stageIdx, chan_comp(swap.alpha));
  }
  case GX_CA_RASA: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "0.0";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return alpha;
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("rast{}.{}", idx, chan_comp(swap.alpha));
  }
  case GX_CA_KONST: {
    return fmt::format("ubuf.konst{}.a", stageIdx);
    switch (stage.kaSel) {
      DEFAULT_FATAL("invalid kaSel {}", underlying(stage.kaSel));
    // Unsourced encodings evaluate to zero in hardware.
    case static_cast<GXTevKAlphaSel>(0x08):
    case static_cast<GXTevKAlphaSel>(0x09):
    case static_cast<GXTevKAlphaSel>(0x0A):
    case static_cast<GXTevKAlphaSel>(0x0B):
    case static_cast<GXTevKAlphaSel>(0x0C):
    case static_cast<GXTevKAlphaSel>(0x0D):
    case static_cast<GXTevKAlphaSel>(0x0E):
    case static_cast<GXTevKAlphaSel>(0x0F):
      return "0.0";
    case GX_TEV_KASEL_8_8:
      return "1.0";
    case GX_TEV_KASEL_7_8:
      return "(7.0/8.0)";
    case GX_TEV_KASEL_6_8:
      return "(6.0/8.0)";
    case GX_TEV_KASEL_5_8:
      return "(5.0/8.0)";
    case GX_TEV_KASEL_4_8:
      return "(4.0/8.0)";
    case GX_TEV_KASEL_3_8:
      return "(3.0/8.0)";
    case GX_TEV_KASEL_2_8:
      return "(2.0/8.0)";
    case GX_TEV_KASEL_1_8:
      return "(1.0/8.0)";
    case GX_TEV_KASEL_K0_R:
      return "ubuf.kcolor0.r";
    case GX_TEV_KASEL_K1_R:
      return "ubuf.kcolor1.r";
    case GX_TEV_KASEL_K2_R:
      return "ubuf.kcolor2.r";
    case GX_TEV_KASEL_K3_R:
      return "ubuf.kcolor3.r";
    case GX_TEV_KASEL_K0_G:
      return "ubuf.kcolor0.g";
    case GX_TEV_KASEL_K1_G:
      return "ubuf.kcolor1.g";
    case GX_TEV_KASEL_K2_G:
      return "ubuf.kcolor2.g";
    case GX_TEV_KASEL_K3_G:
      return "ubuf.kcolor3.g";
    case GX_TEV_KASEL_K0_B:
      return "ubuf.kcolor0.b";
    case GX_TEV_KASEL_K1_B:
      return "ubuf.kcolor1.b";
    case GX_TEV_KASEL_K2_B:
      return "ubuf.kcolor2.b";
    case GX_TEV_KASEL_K3_B:
      return "ubuf.kcolor3.b";
    case GX_TEV_KASEL_K0_A:
      return "ubuf.kcolor0.a";
    case GX_TEV_KASEL_K1_A:
      return "ubuf.kcolor1.a";
    case GX_TEV_KASEL_K2_A:
      return "ubuf.kcolor2.a";
    case GX_TEV_KASEL_K3_A:
      return "ubuf.kcolor3.a";
    }
  }
  case GX_CA_ZERO:
    return "0.0";
  }
}

static std::string tev_bias_i32(GXTevBias bias) {
  switch (bias) {
    DEFAULT_FATAL("invalid tev bias {}", underlying(bias));
  case GX_TB_ZERO:
    return "0";
  case GX_TB_ADDHALF:
    return "128";
  case GX_TB_SUBHALF:
    return "-128";
  }
}

static std::string tev_scale_index(GXTevScale scale) {
  switch (scale) {
    DEFAULT_FATAL("invalid tev scale {}", underlying(scale));
  case GX_CS_SCALE_1:
    return "0u";
  case GX_CS_SCALE_2:
    return "1u";
  case GX_CS_SCALE_4:
    return "2u";
  case GX_CS_DIVIDE_2:
    return "3u";
  }
}

static std::string tev_regular_op(GXTevOp op, GXTevBias bias, GXTevScale scale, std::string_view a,
                                  std::string_view b, std::string_view c, std::string_view d,
                                  std::string_view suffix) {
  CHECK(op == GX_TEV_ADD || op == GX_TEV_SUB, "invalid regular tev op {}", underlying(op));
  return fmt::format("tev_regular_{4}({0}, {1}, {2}, {3}, {5}, {6}, {7})", a, b, c, d, suffix,
                     tev_bias_i32(bias), tev_scale_index(scale), op == GX_TEV_SUB ? "true" : "false");
}

static std::string tev_op(GXTevOp op, GXTevBias bias, GXTevScale scale, std::string_view a, std::string_view b,
                          std::string_view c, std::string_view d, std::string_view zero,
                          std::string_view suffix) {
  switch (op) {
    DEFAULT_FATAL("unimplemented tev op {}", underlying(op));
  case GX_TEV_ADD:
  case GX_TEV_SUB:
    return tev_regular_op(op, bias, scale, a, b, c, d, suffix);
  case GX_TEV_COMP_R8_GT:
    return fmt::format("select({3}, {2}, round({0}.r * 255.0) > round({1}.r * 255.0)) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_R8_EQ:
    return fmt::format("select({3}, {2}, round({0}.r * 255.0) == round({1}.r * 255.0)) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_GR16_GT:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rg * 255.0, vec2(1.0, 256.0))) > round(dot({1}.rg * 255.0, vec2(1.0, 256.0))))"
        " + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_GR16_EQ:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rg * 255.0, vec2(1.0, 256.0))) == round(dot({1}.rg * 255.0, vec2(1.0, 256.0))))"
        " + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_BGR24_GT:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) > round(dot({1}.rgb * 255.0, "
        "vec3(1.0, 256.0, 65536.0)))) + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_BGR24_EQ:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) == round(dot({1}.rgb * 255.0, "
        "vec3(1.0, 256.0, 65536.0)))) + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_RGB8_GT:
    return fmt::format("select({3}, {2}, round({0} * 255.0) > round({1} * 255.0)) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_RGB8_EQ:
    return fmt::format("select({3}, {2}, round({0} * 255.0) == round({1} * 255.0)) + {4}", a, b, c, zero, d);
  }
}

static std::string tev_color_op(GXTevOp op, GXTevBias bias, GXTevScale scale, bool clamp,
                                std::string_view a, std::string_view b, std::string_view c, std::string_view d) {
  const auto overflow = [](std::string_view reg) { return fmt::format("tev_overflow_vec3f({})", reg); };
  std::string expr = tev_op(op, bias, scale, overflow(a), overflow(b), overflow(c), d, "vec3(0)"sv, "vec3f"sv);
  return clamp ? fmt::format("clamp({}, vec3f(0.0), vec3f(1.0))", expr)
               : fmt::format("clamp({}, vec3f(-1024.0 / 255.0), vec3f(1023.0 / 255.0))", expr);
}

static bool alpha_compare_uses_color_inputs(GXTevOp op) noexcept {
  switch (op) {
  case GX_TEV_COMP_R8_GT:
  case GX_TEV_COMP_R8_EQ:
  case GX_TEV_COMP_GR16_GT:
  case GX_TEV_COMP_GR16_EQ:
  case GX_TEV_COMP_BGR24_GT:
  case GX_TEV_COMP_BGR24_EQ:
    return true;
  default:
    return false;
  }
}

static std::string tev_alpha_op(GXTevOp op, GXTevBias bias, GXTevScale scale, bool clamp,
                                std::string_view a, std::string_view b, std::string_view c, std::string_view d,
                                std::string_view colorA, std::string_view colorB) {
  const auto scalarOverflow = [](std::string_view reg) { return fmt::format("tev_overflow_f32({})", reg); };
  std::string expr;
  if (alpha_compare_uses_color_inputs(op)) {
    const auto colorOverflow = [](std::string_view reg) { return fmt::format("tev_overflow_vec3f({})", reg); };
    // GX alpha compares R8/GR16/BGR24 using the color combiner's A/B inputs, while C and D remain the scalar alpha-combiner inputs.
    expr = tev_op(op, bias, scale, colorOverflow(colorA), colorOverflow(colorB), scalarOverflow(c), d, "0.0"sv,
                  "f32"sv);
  } else {
    // The numeric RGB8 op aliases are the alpha combiner's A8 compares and therefore continue to compare scalar alpha A/B inputs here.
    expr = tev_op(op, bias, scale, scalarOverflow(a), scalarOverflow(b), scalarOverflow(c), d, "0.0"sv, "f32"sv);
  }
  return clamp ? fmt::format("clamp({}, 0.0, 1.0)", expr) : fmt::format("clamp({}, -1024.0 / 255.0, 1023.0 / 255.0)", expr);
}

struct AlphaCompareExpr {
  std::string expr;
  int constant = -1;
};

static AlphaCompareExpr alpha_compare_const(bool value) { return {value ? "true"s : "false"s, value ? 1 : 0}; }

static AlphaCompareExpr alpha_compare_not(const AlphaCompareExpr& expr) {
  if (expr.constant != -1) {
    return alpha_compare_const(expr.constant == 0);
  }
  return {fmt::format("!{}", expr.expr), -1};
}

static AlphaCompareExpr alpha_compare_and(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant == 0 || rhs.constant == 0) {
    return alpha_compare_const(false);
  }
  if (lhs.constant == 1) {
    return rhs;
  }
  if (rhs.constant == 1) {
    return lhs;
  }
  return {fmt::format("({} && {})", lhs.expr, rhs.expr), -1};
}

static AlphaCompareExpr alpha_compare_or(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant == 1 || rhs.constant == 1) {
    return alpha_compare_const(true);
  }
  if (lhs.constant == 0) {
    return rhs;
  }
  if (rhs.constant == 0) {
    return lhs;
  }
  return {fmt::format("({} || {})", lhs.expr, rhs.expr), -1};
}

static AlphaCompareExpr alpha_compare_xor(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant != -1 && rhs.constant != -1) {
    return alpha_compare_const(lhs.constant != rhs.constant);
  }
  if (lhs.constant == 0) {
    return rhs;
  }
  if (rhs.constant == 0) {
    return lhs;
  }
  if (lhs.constant == 1) {
    return alpha_compare_not(rhs);
  }
  if (rhs.constant == 1) {
    return alpha_compare_not(lhs);
  }
  return {fmt::format("({} != {})", lhs.expr, rhs.expr), -1};
}

static AlphaCompareExpr alpha_compare_xnor(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant != -1 && rhs.constant != -1) {
    return alpha_compare_const(lhs.constant == rhs.constant);
  }
  if (lhs.constant == 0) {
    return alpha_compare_not(rhs);
  }
  if (rhs.constant == 0) {
    return alpha_compare_not(lhs);
  }
  if (lhs.constant == 1) {
    return rhs;
  }
  if (rhs.constant == 1) {
    return lhs;
  }
  return {fmt::format("({} == {})", lhs.expr, rhs.expr), -1};
}

static AlphaCompareExpr alpha_compare(GXCompare comp, u8 ref) {
  const auto iref = static_cast<u32>(ref);
  switch (comp) {
  default:
    return alpha_compare_const(true);
  case GX_NEVER:
    return alpha_compare_const(false);
  case GX_LESS:
    if (ref == 0) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(alphaCompare < {}u)", iref), -1};
  case GX_LEQUAL:
    if (ref == 255) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(alphaCompare <= {}u)", iref), -1};
  case GX_EQUAL:
    return {fmt::format("(alphaCompare == {}u)", iref), -1};
  case GX_NEQUAL:
    return {fmt::format("(alphaCompare != {}u)", iref), -1};
  case GX_GEQUAL:
    if (ref == 0) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(alphaCompare >= {}u)", iref), -1};
  case GX_GREATER:
    if (ref == 255) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(alphaCompare > {}u)", iref), -1};
  case GX_ALWAYS:
    return alpha_compare_const(true);
  }
}

static inline std::string vtx_attr(const ShaderConfig& config, GXAttr attr) {
  const auto type = config.attrs[attr].attrType;
  if (type == GX_NONE) {
    if (attr == GX_VA_PNMTXIDX) {
      return "ubuf.current_pnmtx";
    }
    if (attr == GX_VA_NRM) {
      // Default normal
      return "vec3f(1.0, 0.0, 0.0)"s;
    }
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
      return "vec4f(1.0)"s;
    }
    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
      return "vec2f(0.0, 0.0)"s;
    }
    UNLIKELY FATAL("unmapped vtx attr {}", underlying(attr));
  }
  if (attr == GX_VA_POS) {
    return "in_pos"s;
  }
  if (attr == GX_VA_NRM) {
    return "in_nrm"s;
  }
  if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
    const auto idx = attr - GX_VA_CLR0;
    return fmt::format("in_clr{}", idx);
  }
  if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
    const auto idx = attr - GX_VA_TEX0;
    return fmt::format("in_tex{}_uv", idx);
  }
  if (attr == GX_VA_PNMTXIDX) {
    return "in_pnmtxidx"s;
  }
  if (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX) {
    const auto idx = attr - GX_VA_TEX0MTXIDX;
    return fmt::format("in_texmtxidx{}", idx);
  }
  UNLIKELY FATAL("unhandled vtx attr {}", underlying(attr));
}

static std::string vertex_color_attr(const ShaderConfig& config, u32 channel) {
  const int attr = shader_vertex_color_attr(config, channel);
  return attr >= 0 ? vtx_attr(config, static_cast<GXAttr>(attr)) : "vec4f(1.0)"s;
}

constexpr std::array<std::string_view, GX_CC_ZERO + 1> TevColorArgNames{
    "CPREV"sv, "APREV"sv, "C0"sv,   "A0"sv,   "C1"sv,  "A1"sv,   "C2"sv,    "A2"sv,
    "TEXC"sv,  "TEXA"sv,  "RASC"sv, "RASA"sv, "ONE"sv, "HALF"sv, "KONST"sv, "ZERO"sv,
};
constexpr std::array<std::string_view, GX_CA_ZERO + 1> TevAlphaArgNames{
    "APREV"sv, "A0"sv, "A1"sv, "A2"sv, "TEXA"sv, "RASA"sv, "KONST"sv, "ZERO"sv,
};

auto fetch_attr(const AttrConfig& mapping, std::string_view buf, std::string_view offs, bool le, u8 cntOverride = 0) -> std::string {
  const u8 cnt = cntOverride != 0 ? cntOverride : mapping.cnt;
  switch (mapping.compType) {
  case GX_U8:
    return fmt::format("fetch_u8_{}(&{}, {}, {}, {})", cnt, buf, offs, mapping.frac, le);
  case GX_S8:
    return fmt::format("fetch_s8_{}(&{}, {}, {}, {})", cnt, buf, offs, mapping.frac, le);
  case GX_U16:
    return fmt::format("fetch_u16_{}(&{}, {}, {}, {})", cnt, buf, offs, mapping.frac, le);
  case GX_S16:
    return fmt::format("fetch_s16_{}(&{}, {}, {}, {})", cnt, buf, offs, mapping.frac, le);
  case GX_F32:
    return fmt::format("fetch_f32_{}(&{}, {}, {})", cnt, buf, offs, le);
  case GX_RGBA8:
    return fmt::format("unpack4x8unorm(load_u32_raw(&{}, {}))", buf, offs);
  default:
    Log.fatal("fetch_attr: Unimplemented {}", static_cast<GXCompType>(mapping.compType));
  }
}

auto fetch_color_attr(const AttrConfig& mapping, std::string_view buf, std::string_view offs, bool le) -> std::string {
  switch (mapping.compType) {
  case GX_RGB565:
    return fmt::format("fetch_rgb565(&{}, {}, {})", buf, offs, le);
  case GX_RGB8:
    return fmt::format("fetch_rgb8(&{}, {}, {})", buf, offs, le);
  case GX_RGBX8:
    return fmt::format("fetch_rgbx8(&{}, {}, {})", buf, offs, le);
  case GX_RGBA4:
    return fmt::format("fetch_rgba4(&{}, {}, {})", buf, offs, le);
  case GX_RGBA6:
    return fmt::format("fetch_rgba6(&{}, {}, {})", buf, offs, le);
  case GX_RGBA8:
    return fmt::format("fetch_rgba8(&{}, {}, {})", buf, offs, le);
  default:
    Log.fatal("fetch_color_attr: Unimplemented {}", static_cast<GXCompType>(mapping.compType));
  }
}

auto attr_load(const ShaderConfig& config, GXAttr attr, std::string_view vidx) -> std::string;

static u32 attr_comp_type_size(u8 compType) noexcept {
  switch (static_cast<GXCompType>(compType)) {
  case GX_U8:
  case GX_S8:
    return 1;
  case GX_U16:
  case GX_S16:
    return 2;
  case GX_F32:
    return 4;
  default:
    Log.fatal("attr_comp_type_size: Unimplemented {}", static_cast<GXCompType>(compType));
  }
}

static std::string offset_plus(std::string_view offs, u32 add) {
  if (add == 0) {
    return std::string(offs);
  }
  return fmt::format("({} + {}u)", offs, add);
}

auto normal_group_load(const ShaderConfig& config, u8 group, std::string_view vidx) -> std::string {
  const auto& mapping = config.attrs[GX_VA_NRM];
  if (mapping.attrType == GX_NONE || mapping.cnt != 9) {
    return attr_load(config, GX_VA_NRM, vidx);
  }

  auto buf = "vbuf"sv;
  auto offs = fmt::format("ubuf.vtx_start + {} * {}u + {}u", vidx, config.vtxStride, mapping.offset);
  auto le = false; // Vertex buffer is always big endian (for now)
  const bool indexedNbt3 =
      mapping.nrmIndexCount == 3 && (mapping.attrType == GX_INDEX8 || mapping.attrType == GX_INDEX16);
  const u32 groupOffset = indexedNbt3 ? 0 : group * 3u * attr_comp_type_size(mapping.compType);

  if (mapping.attrType == GX_INDEX8) {
    const std::string indexOffs = offset_plus(offs, mapping.nrmIndexCount == 3 ? group : 0);
    offs = fmt::format("ubuf.array_start[{}] + raw_fetch_u8_1(&{}, {}) * {}u + {}u", GX_VA_NRM - GX_VA_POS, buf,
                       indexOffs, mapping.stride, groupOffset);
    buf = "abuf"sv;
    le = mapping.le;
  } else if (mapping.attrType == GX_INDEX16) {
    const std::string indexOffs = offset_plus(offs, mapping.nrmIndexCount == 3 ? group * 2u : 0);
    offs = fmt::format("ubuf.array_start[{}] + raw_fetch_u16_1(&{}, {}, {}) * {}u + {}u", GX_VA_NRM - GX_VA_POS,
                       buf, indexOffs, le, mapping.stride, groupOffset);
    buf = "abuf"sv;
    le = mapping.le;
  } else {
    offs = offset_plus(offs, groupOffset);
  }

  return fetch_attr(mapping, buf, offs, le, 3);
}

auto attr_load(const ShaderConfig& config, GXAttr attr, std::string_view vidx) -> std::string {
  const auto& mapping = config.attrs[attr];
  if (mapping.attrType == GX_NONE) {
    return vtx_attr(config, attr);
  }
  auto buf = "vbuf"sv;
  auto offs = fmt::format("ubuf.vtx_start + {} * {}u + {}u", vidx, config.vtxStride, mapping.offset);
  auto le = false; // Vertex buffer is always big endian (for now)
  if (mapping.attrType == GX_INDEX8) {
    offs = fmt::format("ubuf.array_start[{}] + raw_fetch_u8_1(&{}, {}) * {}u", attr - GX_VA_POS, buf, offs,
                       mapping.stride);
    buf = "abuf"sv;
    le = mapping.le;
  } else if (mapping.attrType == GX_INDEX16) {
    offs = fmt::format("ubuf.array_start[{}] + raw_fetch_u16_1(&{}, {}, {}) * {}u", attr - GX_VA_POS, buf, offs, le,
                       mapping.stride);
    buf = "abuf"sv;
    le = mapping.le;
  }
  switch (attr) {
  case GX_VA_PNMTXIDX:
    return fmt::format("(raw_fetch_u8_1(&{}, {}) / 3u)", buf, offs);
  case GX_VA_TEX0MTXIDX:
  case GX_VA_TEX1MTXIDX:
  case GX_VA_TEX2MTXIDX:
  case GX_VA_TEX3MTXIDX:
  case GX_VA_TEX4MTXIDX:
  case GX_VA_TEX5MTXIDX:
  case GX_VA_TEX6MTXIDX:
  case GX_VA_TEX7MTXIDX:
    return fmt::format("raw_fetch_u8_1(&{}, {})", buf, offs);
  case GX_VA_POS: {
    const auto posLoad = fetch_attr(mapping, buf, offs, le);
    if (mapping.cnt == 2) {
      return fmt::format("vec3f({}, 0.0)", posLoad);
    }
    return posLoad;
  }
  case GX_VA_NRM:
    return fetch_attr(mapping, buf, offs, le, mapping.cnt == 9 ? 3 : 0);
  case GX_VA_CLR0:
  case GX_VA_CLR1:
    return fetch_color_attr(mapping, buf, offs, le);
  case GX_VA_TEX0:
  case GX_VA_TEX1:
  case GX_VA_TEX2:
  case GX_VA_TEX3:
  case GX_VA_TEX4:
  case GX_VA_TEX5:
  case GX_VA_TEX6:
  case GX_VA_TEX7: {
    const auto texLoad = fetch_attr(mapping, buf, offs, le);
    if (mapping.cnt == 1) {
      return fmt::format("vec2f({}, 0.0)", texLoad);
    }
    return texLoad;
  }
  default:
    Log.fatal("attr_load: Unimplemented {}", attr);
  }
}

auto lighting_func(const ShaderConfig& config, const ColorChannelConfig& cc, u8 i, bool alpha) -> std::string {
  const bool UsePerPixelLighting = config.perPixelLighting != 0;
  std::string_view swizzle = alpha ? ".a"sv : ".rgb"sv;
  std::string_view intType = alpha ? "i32"sv : "vec3i"sv;
  std::string_view floatType = alpha ? "f32"sv : "vec3f"sv;
  std::string_view intZero = alpha ? "0"sv : "vec3i(0)"sv;
  std::string_view intMax = alpha ? "255"sv : "vec3i(255)"sv;
  std::string_view shift7 = alpha ? "7u"sv : "vec3u(7u)"sv;
  std::string_view shift8 = alpha ? "8u"sv : "vec3u(8u)"sv;
  std::string outVar;
  std::string_view posVar;
  if (UsePerPixelLighting) {
    outVar = fmt::format("rast{}", i);
    posVar = "in.mv_pos"sv;
  } else {
    outVar = fmt::format("out.cc{}", i);
    posVar = "mv_pos"sv;
  }
  std::string ambSrc, matSrc;
  if (cc.ambSrc == GX_SRC_VTX) {
    if (UsePerPixelLighting) {
      ambSrc = fmt::format("in.clr{}", i);
    } else {
      ambSrc = vertex_color_attr(config, i);
    }
  } else if (cc.ambSrc == GX_SRC_REG) {
    ambSrc = fmt::format("ubuf.cc{0}{1}_amb", i, alpha ? "a"sv : ""sv);
  }
  if (cc.matSrc == GX_SRC_VTX) {
    if (UsePerPixelLighting) {
      matSrc = fmt::format("in.clr{}", i);
    } else {
      matSrc = vertex_color_attr(config, i);
    }
  } else if (cc.matSrc == GX_SRC_REG) {
    matSrc = fmt::format("ubuf.cc{0}{1}_mat", i, alpha ? "a"sv : ""sv);
  }
  if (!cc.lightingEnabled) {
    // The XF lighting block consumes and produces 8-bit channel values even when lighting is disabled.
    if (alpha) {
      return fmt::format("\n    {0}.a = f32(i32(round({1}.a * 255.0))) / 255.0;", outVar, matSrc);
    }
    return fmt::format("\n    {0} = vec4f(vec3f(vec3i(round({1}.rgb * 255.0))) / 255.0, 1.0);", outVar,
                       matSrc);
  }
  GXDiffuseFn diffFn = cc.diffFn;
  std::string lightAttnFn;
  if (cc.attnFn == GX_AF_NONE) {
    lightAttnFn = "attn = 1.0;"s;
  } else if (cc.attnFn == GX_AF_SPOT) {
    lightAttnFn = fmt::format(R"""(
          var cosine = max(0.0, dot(ldir, light.dir));
          var cos_attn = dot(light.cos_att, vec3f(1.0, cosine, cosine * cosine));
          var dist_attn = dot(light.dist_att, vec3f(1.0, dist, dist2));
          attn = max(0.0, cos_attn / dist_attn);)""");
  } else if (cc.attnFn == GX_AF_SPEC) {
    std::string_view normal = UsePerPixelLighting ? "ppl_nrm"sv : "mv_nrm"sv;
    std::string dist_attn = diffFn != GX_DF_NONE
                                ? "dot(normalize(light.dist_att), vec3f(1.0, attn, attn * attn))"
                                : "dot(light.dist_att, vec3f(1.0, attn, attn * attn))";
    lightAttnFn = fmt::format(R"""(
          attn = select(0.0, max(0.0, dot({0}, light.dir)), dot({0}, ldir) >= 0.0);
          var cos_attn = dot(light.cos_att, vec3f(1.0, attn, attn * attn));
          var dist_attn = {1};
          attn = select(select(0.0, 1.0, cos_attn > 0.0), max(0.0, cos_attn / dist_attn), dist_attn != 0.0);)""",
                              normal, dist_attn);
  }
  std::string_view normal = UsePerPixelLighting ? "ppl_nrm"sv : "mv_nrm"sv;
  std::string lightDirSetup;
  if (cc.attnFn == GX_AF_NONE) {
    lightDirSetup = fmt::format(R"""(
          var ldir = light.pos - {0};
          var dist2 = dot(ldir, ldir);
          var dist = sqrt(dist2);
          ldir = select({1}, ldir / max(dist, 1e-20), dist > 0.0);)""",
                           posVar, normal);
  } else {
    lightDirSetup = fmt::format(R"""(
          var ldir = light.pos - {0};
          var dist2 = dot(ldir, ldir);
          var dist = sqrt(dist2);
          ldir = ldir / dist;)""",
                           posVar);
  }
  std::string lightDiffFn;
  if (diffFn == GX_DF_NONE) {
    lightDiffFn = "1.0";
  } else if (diffFn == GX_DF_SIGN) {
    lightDiffFn = fmt::format("dot(ldir, {})", normal);
  } else if (diffFn == GX_DF_CLAMP) {
    lightDiffFn = fmt::format("max(0.0, dot(ldir, {}))", normal);
  }
  const auto outputTarget = alpha ? fmt::format("{}.a", outVar) : outVar;
  const auto outputValue =
      alpha
          ? fmt::format("f32((material * (lacc + (lacc >> {}))) >> {}) / 255.0", shift7, shift8)
          : fmt::format("vec4f(vec3f((material * (lacc + (lacc >> {}))) >> {}) / 255.0, 1.0)", shift7,
                        shift8);
  return fmt::format(R"""(
    {{
      var lighting = {11}(round({5}{8} * 255.0));
      for (var i = 0u; i < {1}u; i++) {{
          if ((ubuf.lightState{0}{9} & (1u << i)) == 0u) {{ continue; }}
          var light = ubuf.lights[i];{10}
          var attn: f32;{2}
          var diff = {3};
          lighting = lighting + {11}(round(attn * diff * light.color{8} * 255.0));
      }}
      let lacc = clamp(lighting, {13}, {14});
      let material = {11}(round({4}{8} * 255.0));
      {15} = {16};
    }})""",
                     i, GX::MaxLights, lightAttnFn, lightDiffFn, matSrc, ambSrc, posVar, outVar, swizzle,
                     alpha ? "a"sv : ""sv, lightDirSetup, intType, floatType, intZero, intMax, outputTarget,
                     outputValue);
}


// ---------------------------------------------------------------------------------------------
// Ubershader (docs/ubershader_diseno.md). The TEV program is read from the uniform buffer instead of
// being baked into the source. Every rule below mirrors the specialised generator above it.
static std::string uber_helper_functions(const ShaderInfo& info, const ShaderConfig& config) {
  std::string out = R"WGSL(
struct UberCtx {
    prev: vec4f,
    r0: vec4f,
    r1: vec4f,
    r2: vec4f,
    texc: vec3f,
    texa: f32,
    rasc: vec3f,
    rasa: f32,
    konst: vec4f,
};
fn uber_color_arg(sel: u32, t: UberCtx) -> vec3f {
  switch (sel) {
    case 0u: { return t.prev.rgb; }
    case 1u: { return vec3f(t.prev.a); }
    case 2u: { return t.r0.rgb; }
    case 3u: { return vec3f(t.r0.a); }
    case 4u: { return t.r1.rgb; }
    case 5u: { return vec3f(t.r1.a); }
    case 6u: { return t.r2.rgb; }
    case 7u: { return vec3f(t.r2.a); }
    case 8u: { return t.texc; }
    case 9u: { return vec3f(t.texa); }
    case 10u: { return t.rasc; }
    case 11u: { return vec3f(t.rasa); }
    case 12u: { return vec3f(1.0); }
    case 13u: { return vec3f(0.5); }
    case 14u: { return t.konst.rgb; }
    default: { return vec3f(0.0); }
  }
}
fn uber_alpha_arg(sel: u32, t: UberCtx) -> f32 {
  switch (sel) {
    case 0u: { return t.prev.a; }
    case 1u: { return t.r0.a; }
    case 2u: { return t.r1.a; }
    case 3u: { return t.r2.a; }
    case 4u: { return t.texa; }
    case 5u: { return t.rasa; }
    case 6u: { return t.konst.a; }
    default: { return 0.0; }
  }
}
fn uber_bias(b: u32) -> i32 {
  if (b == 1u) { return 128; }
  if (b == 2u) { return -128; }
  return 0;
}
fn uber_color_op(op: u32, bias: i32, scale: u32, clamp_out: bool, a: vec3f, b: vec3f, c: vec3f, d: vec3f) -> vec3f {
  let oa = tev_overflow_vec3f(a);
  let ob = tev_overflow_vec3f(b);
  let oc = tev_overflow_vec3f(c);
  var r = vec3f(0.0);
  switch (op) {
    case 0u, 1u: { r = tev_regular_vec3f(oa, ob, oc, d, bias, scale, op == 1u); }
    case 8u: { r = select(vec3f(0.0), oc, round(oa.r * 255.0) > round(ob.r * 255.0)) + d; }
    case 9u: { r = select(vec3f(0.0), oc, round(oa.r * 255.0) == round(ob.r * 255.0)) + d; }
    case 10u: { r = select(vec3f(0.0), oc, round(dot(oa.rg * 255.0, vec2(1.0, 256.0))) > round(dot(ob.rg * 255.0, vec2(1.0, 256.0)))) + d; }
    case 11u: { r = select(vec3f(0.0), oc, round(dot(oa.rg * 255.0, vec2(1.0, 256.0))) == round(dot(ob.rg * 255.0, vec2(1.0, 256.0)))) + d; }
    case 12u: { r = select(vec3f(0.0), oc, round(dot(oa.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) > round(dot(ob.rgb * 255.0, vec3(1.0, 256.0, 65536.0)))) + d; }
    case 13u: { r = select(vec3f(0.0), oc, round(dot(oa.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) == round(dot(ob.rgb * 255.0, vec3(1.0, 256.0, 65536.0)))) + d; }
    case 14u: { r = select(vec3f(0.0), oc, round(oa * 255.0) > round(ob * 255.0)) + d; }
    case 15u: { r = select(vec3f(0.0), oc, round(oa * 255.0) == round(ob * 255.0)) + d; }
    default: {}
  }
  if (clamp_out) { return clamp(r, vec3f(0.0), vec3f(1.0)); }
  return clamp(r, vec3f(-1024.0 / 255.0), vec3f(1023.0 / 255.0));
}
fn uber_alpha_op(op: u32, bias: i32, scale: u32, clamp_out: bool, a: f32, b: f32, c: f32, d: f32, ca: vec3f, cb: vec3f) -> f32 {
  let oa = tev_overflow_f32(a);
  let ob = tev_overflow_f32(b);
  let oc = tev_overflow_f32(c);
  let coa = tev_overflow_vec3f(ca);
  let cob = tev_overflow_vec3f(cb);
  var r = 0.0;
  switch (op) {
    case 0u, 1u: { r = tev_regular_f32(oa, ob, oc, d, bias, scale, op == 1u); }
    case 8u: { r = select(0.0, oc, round(coa.r * 255.0) > round(cob.r * 255.0)) + d; }
    case 9u: { r = select(0.0, oc, round(coa.r * 255.0) == round(cob.r * 255.0)) + d; }
    case 10u: { r = select(0.0, oc, round(dot(coa.rg * 255.0, vec2(1.0, 256.0))) > round(dot(cob.rg * 255.0, vec2(1.0, 256.0)))) + d; }
    case 11u: { r = select(0.0, oc, round(dot(coa.rg * 255.0, vec2(1.0, 256.0))) == round(dot(cob.rg * 255.0, vec2(1.0, 256.0)))) + d; }
    case 12u: { r = select(0.0, oc, round(dot(coa.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) > round(dot(cob.rgb * 255.0, vec3(1.0, 256.0, 65536.0)))) + d; }
    case 13u: { r = select(0.0, oc, round(dot(coa.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) == round(dot(cob.rgb * 255.0, vec3(1.0, 256.0, 65536.0)))) + d; }
    case 14u: { r = select(0.0, oc, round(oa * 255.0) > round(ob * 255.0)) + d; }
    case 15u: { r = select(0.0, oc, round(oa * 255.0) == round(ob * 255.0)) + d; }
    default: {}
  }
  if (clamp_out) { return clamp(r, 0.0, 1.0); }
  return clamp(r, -1024.0 / 255.0, 1023.0 / 255.0);
}
fn uber_swz(v: vec4f, sw: u32) -> vec4f {
  return vec4f(v[sw & 3u], v[(sw >> 2u) & 3u], v[(sw >> 4u) & 3u], v[(sw >> 6u) & 3u]);
}
fn uber_wrap_mask(code: u32) -> i32 {
  switch (code) {
    case 0u: { return -1; }
    case 1u: { return 32767; }
    case 2u: { return 16383; }
    case 3u: { return 8191; }
    case 4u: { return 4095; }
    case 5u: { return 2047; }
    default: { return 0; }
  }
}
fn uber_cmp(code: u32, a: u32, r: u32) -> bool {
  switch (code) {
    case 0u: { return false; }
    case 1u: { return a < r; }
    case 2u: { return a == r; }
    case 3u: { return a <= r; }
    case 4u: { return a > r; }
    case 5u: { return a != r; }
    case 6u: { return a >= r; }
    default: { return true; }
  }
}
)WGSL";
  out += "fn uber_sample(tm: u32, uv: vec2f) -> vec4f {\n  switch (tm) {\n";
  for (u32 i = 0; i < MaxTextures; ++i) {
    if (info.sampledTextures.test(i)) {
      out += fmt::format("    case {0}u: {{ return textureSampleBias(tex{0}, tex{0}_samp, uv, ubuf.tex{0}_size_bias.z); }}\n", i);
    }
  }
  out += "    default: { return vec4f(0.0); }\n  }\n}\n";
  out += "fn uber_tex_size(tm: u32) -> vec2f {\n  switch (tm) {\n";
  for (u32 i = 0; i < MaxTextures; ++i) {
    if (info.sampledTextures.test(i)) {
      out += fmt::format("    case {0}u: {{ return ubuf.tex{0}_size_bias.xy; }}\n", i);
    }
  }
  out += "    default: { return vec2f(1.0); }\n  }\n}\n";
  (void)config;
  return out;
}

static std::string uber_tev_body(const ShaderInfo& info, const ShaderConfig& config) {
  std::string pre = "\n    // Ubershader TEV: the material is read from the uniform buffer";
  pre += "\n    var uber_rast = array<vec4f, 2>(";
  for (int i = 0; i < 2; ++i) {
    pre += fmt::format("{}{}", i ? ", " : "", info.sampledColorChannels.test(i) ? fmt::format("rast{}", i) : "vec4f(0.0)"s);
  }
  pre += ");\n    var uber_uv = array<vec2f, 8>(";
  for (u32 i = 0; i < 8; ++i) {
    pre += fmt::format("{}{}", i ? ", " : "", info.sampledTexCoords.test(i) ? fmt::format("tex{}_uv", i) : "vec2f(0.0)"s);
  }
  pre += ");\n    var uber_scale = array<vec2f, 8>(";
  for (u32 i = 0; i < 8; ++i) {
    pre += fmt::format("{}{}", i ? ", " : "",
                       info.sampledTexCoords.test(i) ? fmt::format("ubuf.texcoord{}_scale.xy", i) : "vec2f(1.0)"s);
  }
  pre += ");";
  const std::string noTexC = config.numTexGens == 0 ? "0.0" : "1.0";
  std::string body = R"WGSL(
    var tev = array<vec4f, 4>(prev, tevreg0, tevreg1, tevreg2);
    var t_TexCoord = vec2i(0);
    var sampled_z = vec4f(0.0);
    var ind_tc = array<vec3i, 4>(vec3i(0), vec3i(0), vec3i(0), vec3i(0));
    var ind_tf = array<vec3f, 4>(vec3f(0.0), vec3f(0.0), vec3f(0.0), vec3f(0.0));
    for (var k = 0u; k < 4u; k = k + 1u) {
      if (((ubuf.uber_misc1.x >> k) & 1u) == 0u) { continue; }
      let ind = ubuf.uber_ind[k];
      var ind_base = vec2i(0);
      if (ind.x != 0u) {
        ind_base = vec2i(uber_uv[ind.x - 1u] * uber_scale[ind.x - 1u] * 128.0);
      }
      let ind_fixed = ind_base >> vec2u(ind.z, ind.w);
      if (ind.y != 0u) {
        let ind_smp = uber_sample(ind.y - 1u, vec2f(ind_fixed) / (uber_tex_size(ind.y - 1u) * 128.0));
        ind_tf[k] = ind_smp.abg * 255.0;
        ind_tc[k] = vec3i(round(ind_tf[k]));
      }
    }
    let uber_stage_count = ubuf.uber_misc0.x;
    for (var s = 0u; s < uber_stage_count; s = s + 1u) {
      let v0 = ubuf.uber_stages[s];
      let w0 = v0.x;
      let w1 = v0.y & 0x7FFu;
      let w2 = (v0.y >> 11u) & 0x7FFu;
      let w3 = v0.z;
      let w4 = v0.w;
      let tc = i32(w3 & 15u) - 1;
      let tm = i32((w3 >> 4u) & 15u) - 1;
      let kind = (w3 >> 8u) & 7u;
      let can_sample = ((w3 >> 16u) & 1u) != 0u;
      let needs_fixed = ((w3 >> 17u) & 1u) != 0u;
      let will_sample = ((w3 >> 18u) & 1u) != 0u;
      let has_ind = ((w3 >> 19u) & 1u) != 0u;
      let add_prev = ((w3 >> 20u) & 1u) != 0u;
      let has_ind_stage = ((w3 >> 21u) & 1u) != 0u;
      let ist = w4 & 3u;
      let ifmt = (w4 >> 2u) & 3u;

      var sampled = vec4f(0.0);
      if (needs_fixed || will_sample) {
        var base_fixed = vec2i(0);
        if (tc >= 0) {
          base_fixed = vec2i(uber_uv[u32(tc)] * uber_scale[u32(tc)] * 128.0);
        }
        var uv_in = vec2f(0.0);
        if (needs_fixed) {
          var ind_off = vec2i(0);
          var has_off = false;
          //@IND_BEGIN
          if (has_ind) {
            var c = ind_tc[ist];
            if (ifmt != 0u) { c = c >> vec3u(ifmt + 2u); }
            let bsel = (w4 >> 4u) & 7u;
            if (bsel != 0u) {
              let bias = select(1, -128, ifmt == 0u);
              c = c + vec3i(i32(bsel & 1u), i32((bsel >> 1u) & 1u), i32((bsel >> 2u) & 1u)) * bias;
            }
            let mtx = (w4 >> 9u) & 15u;
            var mi = 0u;
            if (mtx >= 1u && mtx <= 3u) {
              mi = mtx - 1u;
              let c0 = ubuf.ind_mtx[mi * 2u];
              let c1 = ubuf.ind_mtx[mi * 2u + 1u];
              ind_off = vec2i((c0.x * c.x + c0.z * c.y + c1.x * c.z) >> 3u,
                              (c0.y * c.x + c0.w * c.y + c1.y * c.z) >> 3u);
              has_off = true;
            } else if (mtx >= 5u && mtx <= 7u && tc >= 0) {
              mi = mtx - 5u;
              ind_off = (base_fixed * vec2i(c.x)) >> vec2u(8u);
              has_off = true;
            } else if (mtx >= 9u && mtx <= 11u && tc >= 0) {
              mi = mtx - 9u;
              ind_off = (base_fixed * vec2i(c.y)) >> vec2u(8u);
              has_off = true;
            }
            if (has_off) {
              let ind_shift = ubuf.ind_mtx[mi * 2u + 1u].z;
              if (ind_shift >= 0) {
                ind_off = ind_off >> vec2u(u32(ind_shift));
              } else {
                ind_off = ind_off << vec2u(u32(-ind_shift));
              }
              // Smooth indirect offsets (g_smoothIndirect, ITF_8 only): same as the specialised shader.
              if (ifmt == 0u && ubuf.ind_mtx[mi * 2u + 1u].w != 0) {
                let fr = ind_tf[ist] - vec3f(ind_tc[ist]);
                var fd = vec2f(0.0);
                if (mtx <= 3u) {
                  let s0 = ubuf.ind_mtx[mi * 2u];
                  let s1 = ubuf.ind_mtx[mi * 2u + 1u];
                  fd = vec2f(f32(s0.x) * fr.x + f32(s0.z) * fr.y + f32(s1.x) * fr.z,
                             f32(s0.y) * fr.x + f32(s0.w) * fr.y + f32(s1.y) * fr.z) / 8.0;
                } else if (mtx <= 7u) {
                  fd = vec2f(base_fixed) * fr.x / 256.0;
                } else {
                  fd = vec2f(base_fixed) * fr.y / 256.0;
                }
                ind_off = ind_off + vec2i(round(fd * exp2(-f32(ind_shift))));
              }
            }
          }
          //@IND_END
          var final_c = vec2i(base_fixed.x & uber_wrap_mask((w4 >> 13u) & 7u), base_fixed.y & uber_wrap_mask((w4 >> 16u) & 7u));
          if (has_off) { final_c = final_c + ind_off; }
          if (add_prev) { t_TexCoord = t_TexCoord + final_c; } else { t_TexCoord = final_c; }
          t_TexCoord = tev_s24_fixed_vec2i(t_TexCoord);
          if (will_sample) {
            uv_in = vec2f(t_TexCoord) / (uber_tex_size(u32(tm)) * 128.0);
          }
        } else {
          uv_in = vec2f(tev_s24_fixed_vec2i(base_fixed)) / (uber_tex_size(u32(tm)) * 128.0);
        }
        if (will_sample) {
          sampled = uber_sample(u32(tm), uv_in);
        }
      }
      if (s + 1u == ubuf.uber_misc0.z) { sampled_z = sampled; }

      let sw_ras = ubuf.uber_swaps[(w3 >> 12u) & 3u];
      let sw_tex = ubuf.uber_swaps[(w3 >> 14u) & 3u];
      var t: UberCtx;
      t.prev = tev[0];
      t.r0 = tev[1];
      t.r1 = tev[2];
      t.r2 = tev[3];
      if (can_sample) {
        let tex_sw = uber_swz(sampled, sw_tex);
        t.texc = tex_sw.rgb;
        t.texa = tex_sw.a;
      } else {
        t.texc = vec3f(@NOTEX@);
        t.texa = @NOTEX@;
      }
      if (kind == 1u || kind == 2u) {
        let ras_sw = uber_swz(uber_rast[kind - 1u], sw_ras);
        t.rasc = ras_sw.rgb;
        t.rasa = ras_sw.a;
      } else if (kind >= 3u) {
        var bump = 0.0;
        let asel = (w4 >> 7u) & 3u;
        if (has_ind_stage && asel != 0u) {
          var bmask = 0xF8u;
          if (ifmt == 1u) { bmask = 0xE0u; } else if (ifmt == 2u) { bmask = 0xF0u; }
          bump = f32(u32(ind_tc[ist][asel - 1u]) & bmask) / 255.0;
          if (kind == 4u) { bump = bump * (255.0 / 248.0); }
        }
        t.rasc = vec3f(bump);
        t.rasa = bump;
      }
      t.konst = ubuf.uber_konst[s];

      let ca = uber_color_arg(w0 & 15u, t);
      let cb = uber_color_arg((w0 >> 4u) & 15u, t);
      let cc = uber_color_arg((w0 >> 8u) & 15u, t);
      let cd = uber_color_arg((w0 >> 12u) & 15u, t);
      let aa = uber_alpha_arg((w0 >> 16u) & 7u, t);
      let ab = uber_alpha_arg((w0 >> 19u) & 7u, t);
      let ac = uber_alpha_arg((w0 >> 22u) & 7u, t);
      let ad = uber_alpha_arg((w0 >> 25u) & 7u, t);
      let cres = uber_color_op(w1 & 15u, uber_bias((w1 >> 4u) & 3u), (w1 >> 6u) & 3u, ((w1 >> 8u) & 1u) != 0u, ca, cb, cc, cd);
      let ares = uber_alpha_op(w2 & 15u, uber_bias((w2 >> 4u) & 3u), (w2 >> 6u) & 3u, ((w2 >> 8u) & 1u) != 0u, aa, ab, ac, ad, ca, cb);
      let cout = (w1 >> 9u) & 3u;
      let aout = (w2 >> 9u) & 3u;
      tev[cout] = vec4f(cres, tev[cout].a);
      tev[aout].a = ares;
    }
    prev = tev[0];
    let uber_last_c = ubuf.uber_misc0.w & 15u;
    let uber_last_a = (ubuf.uber_misc0.w >> 4u) & 15u;
    if (uber_last_c != 0u) { prev = vec4f(tev[uber_last_c].rgb, prev.a); }
    if (uber_last_a != 0u) { prev.a = tev[uber_last_a].a; }
)WGSL";
  if (!info.usedIndTexMtxs.any()) {
    // No indirect matrix in this material's uniform layout: drop the code that reads it.
    const auto begin = body.find("//@IND_BEGIN");
    const auto end = body.find("//@IND_END");
    if (begin != std::string::npos && end != std::string::npos && end > begin) {
      body.erase(begin, end - begin);
    }
  }
  const std::string marker = "@NOTEX@";
  for (size_t at = body.find(marker); at != std::string::npos; at = body.find(marker, at)) {
    body.replace(at, marker.size(), noTexC);
  }
  return pre + body;
}

static const char* const UberAlphaCompareWgsl = R"WGSL(
    // Alpha compare (uniform-driven)
    let alphaCompare = u32(round(clamp(prev.a, 0.0, 1.0) * 255.0));
    let acp0 = uber_cmp(ubuf.uber_acmp.x, alphaCompare, ubuf.uber_acmp.y);
    let acp1 = uber_cmp(ubuf.uber_acmp.z, alphaCompare, ubuf.uber_acmp.w);
    var acpass = acp0 && acp1;
    switch (ubuf.uber_misc1.y) {
      case 1u: { acpass = acp0 || acp1; }
      case 2u: { acpass = acp0 != acp1; }
      case 3u: { acpass = acp0 == acp1; }
      default: {}
    }
    if (!acpass) { discard; }
)WGSL";

static const char* const UberUniformFields = R"WGSL(
    uber_misc0: vec4u,
    uber_misc1: vec4u,
    uber_swaps: vec4u,
    uber_acmp: vec4u,
    uber_ind: array<vec4u, 4>,
    uber_konst: array<vec4f, 16>,
    uber_stages: array<vec4u, 16>,)WGSL";


// The WGSL of one specialised GX shader. Pure function of the config (no device, no GPU): the shader
// golden test (tests/shader_golden_test.cpp) hashes its output for a fixed set of recipes, so any
// change to the generator is visible in review before it reaches a cache or a player.
static std::string build_shader_source_impl(const ShaderConfig& config, const UberConfig* uber) noexcept {
  ZoneScoped;
  const bool UsePerPixelLighting = config.perPixelLighting != 0;
  const auto hash = xxh3_hash(config);
  const auto info = uber != nullptr ? build_uber_shader_info(*uber) : build_shader_info(config);

  std::string uniformPre;
  std::string uniBufAttrs;
  std::string texBindings;
  std::string vtxOutAttrs;
  std::string vtxInAttrs;
  std::string vtxXfrAttrsPre;
  std::string vtxXfrAttrs;
  size_t vtxOutIdx = 0;

  // Load points for line/point expansion
  std::string_view vidxAttr = "vidx"sv;
  if (config.lineMode != 0) {
    vtxInAttrs += ",\n    @builtin(instance_index) iidx: u32";
    uniBufAttrs +=
        "\n    line_width: f32,"
        "\n    line_aspect_y: f32,"
        "\n    line_tex_offset: f32,"
        "\n    line_texcoord_mask: u32,";
    if (config.lineMode == 3) {
      // GX_POINTS: each instance = one vertex, expand to quad
      vtxXfrAttrsPre += fmt::format(
          "\n    let in_vidx = iidx;"
          "\n    let in_pos = {};"
          "\n    let in_pnmtxidx = {};"
          "\n    let mv_pos = vec4f(in_pos, 1.0) * ubuf.postex_mtx[in_pnmtxidx];",
          attr_load(config, GX_VA_POS, "in_vidx"sv), attr_load(config, GX_VA_PNMTXIDX, "in_vidx"sv));
    } else {
      // GX_LINES / GX_LINESTRIP: each instance = two vertices, expand to quad
      vtxXfrAttrsPre += fmt::format(
          "\n    let use_b = vidx >= 2u;"
          "\n    let vidx_a = iidx * {}u;"
          "\n    let vidx_b = vidx_a + 1u;"
          "\n    let in_vidx = select(vidx_a, vidx_b, use_b);"
          "\n    let pos_a = {};"
          "\n    let pos_b = {};"
          "\n    let in_pos = select(pos_a, pos_b, use_b);"
          "\n    let pnmtxidx_a = {};"
          "\n    let pnmtxidx_b = {};"
          "\n    let in_pnmtxidx = select(pnmtxidx_a, pnmtxidx_b, use_b);"
          "\n    let mv_pos_a = vec4f(pos_a, 1.0) * ubuf.postex_mtx[pnmtxidx_a];"
          "\n    let mv_pos_b = vec4f(pos_b, 1.0) * ubuf.postex_mtx[pnmtxidx_b];"
          "\n    let mv_pos = select(mv_pos_a, mv_pos_b, use_b);",
          config.lineMode == 1 ? 2 : 1, attr_load(config, GX_VA_POS, "vidx_a"sv),
          attr_load(config, GX_VA_POS, "vidx_b"sv), attr_load(config, GX_VA_PNMTXIDX, "vidx_a"sv),
          attr_load(config, GX_VA_PNMTXIDX, "vidx_b"sv));
    }
    vidxAttr = "in_vidx"sv;
  } else if (config.attrs[GX_VA_PNMTXIDX].attrType == GX_NONE) {
    vtxXfrAttrsPre += "\n    let in_pnmtxidx = ubuf.current_pnmtx;";
  }

  // Load vertex attributes
  for (GXAttr attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7; attr = static_cast<GXAttr>(attr + 1)) {
    const auto attrType = config.attrs[attr].attrType;
    if (attrType == GX_NONE) {
      continue;
    }
    // in_pnmtxidx and in_pos written above for line mode
    if ((attr != GX_VA_PNMTXIDX && attr != GX_VA_POS) || config.lineMode == 0) {
      vtxXfrAttrsPre += fmt::format("\n    let {} = {};", vtx_attr(config, attr), attr_load(config, attr, vidxAttr));
    }
  }

  if (config.lineMode == 0) {
    vtxXfrAttrsPre += fmt::format(
        "\n    let mv_pos = vec4f({}, 1.0) * ubuf.postex_mtx[in_pnmtxidx];"
        "\n    out.pos = vec4f(mv_pos, 1.0) * ubuf.proj;",
        vtx_attr(config, GX_VA_POS));
  } else if (config.lineMode == 3) {
    // GX_POINTS: expand single vertex to axis-aligned screen-space square
    vtxXfrAttrsPre +=
        "\n    let clip = vec4f(mv_pos, 1.0) * ubuf.proj;"
        "\n    let viewport_scale = ubuf.render_viewport_size / max(ubuf.logical_viewport_size, vec2f(1.0));"
        "\n    let point_size = ubuf.line_width * min(viewport_scale.x, viewport_scale.y);"
        "\n    let x_sign = select(-1.0, 1.0, (vidx & 1u) != 0u);"
        "\n    let y_sign = select(-1.0, 1.0, vidx >= 2u);"
        "\n    let offset_px = vec2f(x_sign, y_sign) * (point_size / 2.0);"
        "\n    let offset_ndc = (offset_px * 2.0) / ubuf.render_viewport_size;"
        "\n    out.pos = vec4f(clip.xy + offset_ndc * clip.w, clip.zw);";
  } else {
    // GX_LINES / GX_LINESTRIP: expand line segment perpendicular to direction
    vtxXfrAttrsPre +=
        "\n    let clip_a = vec4f(mv_pos_a, 1.0) * ubuf.proj;"
        "\n    let clip_b = vec4f(mv_pos_b, 1.0) * ubuf.proj;"
        "\n    let ndc_a = clip_a.xy / clip_a.w;"
        "\n    let ndc_b = clip_b.xy / clip_b.w;"
        "\n    let viewport_scale = ubuf.render_viewport_size / max(ubuf.logical_viewport_size, vec2f(1.0));"
        "\n    let delta_px = (ndc_b - ndc_a) / 2.0 * ubuf.render_viewport_size;"
        "\n    let dir_px = select(vec2f(1.0, 0.0), normalize(delta_px), dot(delta_px, delta_px) > 1e-10);"
        "\n    let perp_px = vec2f(-dir_px.y, dir_px.x);"
        "\n    let line_width = ubuf.line_width * min(viewport_scale.x, viewport_scale.y);"
        "\n    let offset_px = perp_px * (line_width / 2.0) * select(-1.0, 1.0, (vidx & 1u) != 0u);"
        "\n    let offset_ndc = (offset_px * 2.0) / ubuf.render_viewport_size;"
        "\n    let clip_base = select(clip_a, clip_b, use_b);"
        "\n    out.pos = vec4f(clip_base.xy + offset_ndc * clip_base.w, clip_base.zw);";
  }
  // The near/far depth correction used to be applied here per-vertex (out.pos.z = -out.pos.z for
  // reversed, or += out.pos.w for forward), redundantly on top of the same correction already
  // folded into ubuf.proj by effective_projection() (shader_info.cpp) - applying it twice canceled
  // out for the common case (any draw where effective_projection() decides to flip), silently
  // making "reversed" Z behave identically to forward Z. It is now applied exactly once, in the
  // projection matrix alone (matching upstream aurora commit 1dde08fa: "Move depth correction to
  // projection matrix"), so nothing needs to happen to out.pos.z here.
  // GX rasterizes at a 7/12 pixel center when antialiasing is disabled, while WebGPU rasterizes at 1/2.
  vtxXfrAttrsPre +=
      "\n    let gx_pixel_center_correction = "
      "vec2f(-1.0, 1.0) / (6.0 * max(abs(ubuf.render_viewport_size), vec2f(1.0)));"
      "\n    out.pos = vec4f(out.pos.xy + out.pos.w * gx_pixel_center_correction, out.pos.zw);";
  vtxXfrAttrsPre += fmt::format(
      "\n    let nrm_tmp = vec4f({}, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
      "\n    let mv_nrm = select(nrm_tmp, normalize(nrm_tmp), dot(nrm_tmp, nrm_tmp) > 1e-10);",
      vtx_attr(config, GX_VA_NRM));

  uniBufAttrs += "\n    proj: mat4x4f,";
  // Only the matrix slots this shader can read are uploaded, in compacted order; see UniformMatrixLayout.
  uniBufAttrs += fmt::format("\n    postex_mtx: array<mat3x4f, {}>,", info.matrixLayout.postexCount);
  uniBufAttrs += fmt::format("\n    nrm_mtx: array<mat3x4f, {}>,", info.matrixLayout.nrmCount);
  std::string fragmentFnPre;
  std::string fragmentFn;
  const int zTexStage = tev_z_texture_stage(config);
  const bool usesZTextureDepth = tev_z_texture_enabled(config);
  std::array<bool, MaxTexCoord> emittedFixedTexCoord{};
  const auto fixed_texcoord_name = [&](u32 texCoordId) {
    if (!emittedFixedTexCoord[texCoordId]) {
      fragmentFnPre += fmt::format(
          "\n    let tex{0}_fixed_uv = vec2i(tex{0}_uv * ubuf.texcoord{0}_scale.xy * 128.0);", texCoordId);
      emittedFixedTexCoord[texCoordId] = true;
    }
    return fmt::format("tex{}_fixed_uv", texCoordId);
  };

  static std::array regName{"prev"sv, "tevreg0"sv, "tevreg1"sv, "tevreg2"sv};
  // Modern lighting, level 2 (game material masks): what the material's emissive stage adds to PREV is
  // kept apart (mat_emis_add), so the material block further down shades the surface without it.
  const bool materialBlockWanted =
      uber == nullptr &&
      ((config.enhancedMetal != 0 && config.perPixelLighting != 0 && info.lightingEnabled) || info.usesEnvLights);
  const int matEmissiveStage = materialBlockWanted ? material_emissive_stage(config) : -1;
  if (uber == nullptr) for (u32 idx = 0; idx < config.tevStageCount; ++idx) {
    const auto& stage = config.tevStages[idx];
    std::string colorA = color_arg_reg(stage.colorPass.a, idx, config, stage);
    std::string colorB = color_arg_reg(stage.colorPass.b, idx, config, stage);
    fragmentFn += fmt::format("\n    // TEV stage {}", idx);
    if (alpha_compare_uses_color_inputs(stage.alphaOp.op)) {
      const std::string colorAName = fmt::format("tev_stage{}_color_a", idx);
      const std::string colorBName = fmt::format("tev_stage{}_color_b", idx);
      // The color result is emitted before the alpha result below.
      fragmentFn += fmt::format("\n    let {0} = {1};\n    let {2} = {3};", colorAName, colorA, colorBName, colorB);
      colorA = colorAName;
      colorB = colorBName;
    }
    {
      std::string_view outReg = regName[stage.colorOp.outReg];
      std::string op = tev_color_op(
          stage.colorOp.op, stage.colorOp.bias, stage.colorOp.scale, stage.colorOp.clamp, colorA, colorB,
          color_arg_reg(stage.colorPass.c, idx, config, stage), color_arg_reg(stage.colorPass.d, idx, config, stage));
      if (static_cast<int>(idx) == matEmissiveStage) fragmentFn += "\n    let mat_emis_before = prev.rgb;";
      fragmentFn += fmt::format("\n    {0} = vec4f({1}, {0}.a);", outReg, op);
      if (static_cast<int>(idx) == matEmissiveStage)
        fragmentFn += "\n    let mat_emis_add = max(prev.rgb - mat_emis_before, vec3f(0.0));";
    }
    {
      std::string_view outReg = regName[stage.alphaOp.outReg];
      std::string op = tev_alpha_op(
          stage.alphaOp.op, stage.alphaOp.bias, stage.alphaOp.scale, stage.alphaOp.clamp,
          alpha_arg_reg(stage.alphaPass.a, idx, config, stage), alpha_arg_reg(stage.alphaPass.b, idx, config, stage),
          alpha_arg_reg(stage.alphaPass.c, idx, config, stage), alpha_arg_reg(stage.alphaPass.d, idx, config, stage),
          colorA, colorB);
      fragmentFn += fmt::format("\n    {0}.a = {1};", outReg, op);
    }
  }

  if (uber == nullptr) {
    const auto& lastStage = config.tevStages[config.tevStageCount - 1];
    if (lastStage.colorOp.outReg != 0) {
      fragmentFn += fmt::format("\n    prev = vec4f({0}.rgb, prev.a);", regName[lastStage.colorOp.outReg]);
    }
    if (lastStage.alphaOp.outReg != 0) {
      fragmentFn += fmt::format("\n    prev.a = {0}.a;", regName[lastStage.alphaOp.outReg]);
    }
  }

  if (uber != nullptr) {
    fragmentFn += uber_tev_body(info, config);
    uniformPre += uber_helper_functions(info, config);
  }
  const auto loadsTevRegs = info.loadsTevRegRgb | info.loadsTevRegAlpha;
  const auto writesTevRegs = info.writesTevRegRgb | info.writesTevRegAlpha;
  if (loadsTevRegs.test(0)) {
    uniBufAttrs += "\n    tevprev: vec4f,";
    fragmentFnPre += "\n    var prev = ubuf.tevprev;";
  } else {
    fragmentFnPre += "\n    var prev: vec4f;";
  }
  for (int i = 1 /* Skip TEVPREV */; i < loadsTevRegs.size(); ++i) {
    if (loadsTevRegs.test(i)) {
      uniBufAttrs += fmt::format("\n    tevreg{}: vec4f,", i - 1);
      fragmentFnPre += fmt::format("\n    var tevreg{0} = ubuf.tevreg{0};", i - 1);
    } else if (writesTevRegs.test(i)) {
      fragmentFnPre += fmt::format("\n    var tevreg{0}: vec4f;", i - 1);
    }
  }

  // Enhanced metal needs the per-pixel normal and position, which only exist when this shader
  // evaluates lighting itself (a lit material whose TEV never reads the colour channel does not).
  const bool metalActive = config.enhancedMetal != 0 && config.perPixelLighting != 0 && info.lightingEnabled;
  // Environment material (unlit lightmapped world surface): the enhanced-metal block with the
  // lightmap as the surroundings' light and borrowed lights (shader_info sets usesEnvLights).
  const bool envActive = info.usesEnvLights && uber == nullptr;
  const int envLightmapStage = envActive ? env_lightmap_stage(config) : -1;
  if (info.lightingEnabled) {
    uniBufAttrs += fmt::format(FMT_STRING(R"""(
    lights: array<Light, {}>,
    lightState0: u32,
    lightState1: u32,
    lightState0a: u32,
    lightState1a: u32,)"""),
                               GX::MaxLights);
    uniformPre +=
        "\n"
        "struct Light {\n"
        "    pos: vec3f,\n"
        "    dir: vec3f,\n"
        "    color: vec4f,\n"
        "    cos_att: vec3f,\n"
        "    dist_att: vec3f,\n"
        "};";
    if (UsePerPixelLighting) {
      vtxOutAttrs += fmt::format("\n    @location({}) mv_pos: vec3f,", vtxOutIdx++);
      vtxOutAttrs += fmt::format("\n    @location({}) mv_nrm: vec3f,", vtxOutIdx++);
      vtxXfrAttrs += fmt::format(FMT_STRING(R"""(
    out.mv_pos = mv_pos;
    out.mv_nrm = mv_nrm;)"""));
      // The interpolated normal shrinks across a triangle; renormalize unless it is
      // the zero vector a draw without normals produces (mirrors the vertex guard).
      fragmentFnPre += "\n    let ppl_nrm = select(in.mv_nrm, normalize(in.mv_nrm), dot(in.mv_nrm, in.mv_nrm) > 1e-10);";
      if (metalActive) {
        // Enhanced metal: strength from the uniform header pad, view vector and Fresnel term.
        fragmentFnPre +=
            "\n    let metal_k = bitcast<f32>(ubuf.pad.x);"
            "\n    let metal_pk = ubuf.pad.y;"
            "\n    let metal_edge = f32((metal_pk >> 16u) & 255u) / 255.0 * 4.0;"
            "\n    let metal_v = normalize(-in.mv_pos);"
            "\n    let metal_fres = pow(1.0 - clamp(abs(dot(ppl_nrm, metal_v)), 0.0, 1.0), 3.0);";
      }
    }
  }
  if (envActive) {
    if (!info.lightingEnabled) {
      uniformPre +=
          "\n"
          "struct Light {\n"
          "    pos: vec3f,\n"
          "    dir: vec3f,\n"
          "    color: vec4f,\n"
          "    cos_att: vec3f,\n"
          "    dist_att: vec3f,\n"
          "};";
    }
    // GX spot/distance attenuation of a borrowed light at a surface point (same terms as the XF
    // lighting above), capped so a light right next to a wall does not blow out.
    uniformPre +=
        "\nfn env_att(l: Light, p: vec3f) -> f32 {"
        "\n    let d = l.pos - p;"
        "\n    let dist2 = max(dot(d, d), 1e-8);"
        "\n    let dist = sqrt(dist2);"
        "\n    let ldir = d / dist;"
        "\n    let cosine = max(0.0, dot(ldir, l.dir));"
        "\n    let ca = dot(l.cos_att, vec3f(1.0, cosine, cosine * cosine));"
        "\n    let da = dot(l.dist_att, vec3f(1.0, dist, dist2));"
        "\n    return clamp(select(1.0, max(0.0, ca / da), abs(da) > 1e-6), 0.0, 4.0);"
        "\n}"
        // Which highlights a light may give here. Borrowed lights (env_light_mask.y = 0): only where the
        // lightmap is lit, exactly as before. Engine lights (modern lighting): color.a holds the range,
        // + for a static light (baked into the lightmap, so the same lightmap gate) and - for a dynamic
        // one (shots, explosions: not in the lightmap, no gate); both fade out smoothly at that range.
        "\nfn env_gate(l: Light, p: vec3f, shadow: f32, mode: u32) -> f32 {"
        "\n    if (mode == 0u) { return shadow; }"
        "\n    let range = max(abs(l.color.a), 1e-3);"
        "\n    let x = length(l.pos - p) / range;"
        "\n    let fall = clamp(1.0 - x * x * x * x, 0.0, 1.0);"
        "\n    return fall * fall * select(1.0, shadow, l.color.a >= 0.0);"
        "\n}";
    vtxOutAttrs += fmt::format("\n    @location({}) mv_pos: vec3f,", vtxOutIdx++);
    vtxOutAttrs += fmt::format("\n    @location({}) mv_nrm: vec3f,", vtxOutIdx++);
    vtxXfrAttrs += "\n    out.mv_pos = mv_pos;\n    out.mv_nrm = mv_nrm;";
    fragmentFnPre +=
        "\n    let ppl_nrm = select(in.mv_nrm, normalize(in.mv_nrm), dot(in.mv_nrm, in.mv_nrm) > 1e-10);"
        "\n    let metal_k = bitcast<f32>(ubuf.pad.x);"
        "\n    let metal_pk = ubuf.pad.y;"
        "\n    let metal_v = normalize(-in.mv_pos);";
  }

  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (!info.sampledColorChannels.test(i)) {
      continue;
    }

    const auto& cc = config.colorChannels[i];
    const auto& cca = config.colorChannels[i + GX_ALPHA0];
    if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}_amb: vec4f,", i);
    }
    if (cc.matSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}_mat: vec4f,", i);
    }
    if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}a_amb: vec4f,", i);
    }
    if (cca.matSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}a_mat: vec4f,", i);
    }

    // Output vertex color if necessary
    if (UsePerPixelLighting) {
      if ((cc.lightingEnabled && cc.ambSrc == GX_SRC_VTX) || cc.matSrc == GX_SRC_VTX ||
          (cca.lightingEnabled && cca.ambSrc == GX_SRC_VTX) || cca.matSrc == GX_SRC_VTX) {
        vtxOutAttrs += fmt::format("\n    @location({}) clr{}: vec4f,", vtxOutIdx++, i);
        vtxXfrAttrs += fmt::format("\n    out.clr{} = {};", i, vertex_color_attr(config, i));
      }
    }

    if (UsePerPixelLighting) {
      fragmentFnPre += fmt::format("\n    var rast{}: vec4f;", i);
      fragmentFnPre += lighting_func(config, cc, i, false);
      fragmentFnPre += lighting_func(config, cca, i, true);
    } else {
      vtxOutAttrs += fmt::format("\n    @location({}) cc{}: vec4f,", vtxOutIdx++, i);
      vtxXfrAttrs += lighting_func(config, cc, i, false);
      vtxXfrAttrs += lighting_func(config, cca, i, true);
      fragmentFnPre += fmt::format("\n    var rast{0} = in.cc{0};", i);
    }
  }
  for (int i = 0; i < info.sampledKColors.size(); ++i) {
    if (info.sampledKColors.test(i)) {
      uniBufAttrs += fmt::format("\n    kcolor{}: vec4f,", i);
    }
  }
  for (int i = 0; i < info.konstStages.size(); ++i) {
    if (info.konstStages.test(i)) {
      uniBufAttrs += fmt::format("\n    konst{}: vec4f,", i);
    }
  }
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (tcg.type == GX_TG_MTX3x4) {
      vtxOutAttrs += fmt::format("\n    @location({}) tex{}_uvw: vec3f,", vtxOutIdx++, i);
    } else {
      vtxOutAttrs += fmt::format("\n    @location({}) tex{}_uv: vec2f,", vtxOutIdx++, i);
    }
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0, 1.0);", i,
                                 vtx_attr(config, GXAttr(GX_VA_TEX0 + (tcg.src - GX_TG_TEX0))));
    } else if (tcg.src == GX_MAX_TEXGENSRC) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0, 1.0);", i,
                                 vtx_attr(config, GXAttr(GX_VA_TEX0 + i)));
    } else if (tcg.src == GX_TG_POS) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, vtx_attr(config, GX_VA_POS));
    } else if (tcg.src == GX_TG_NRM) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, vtx_attr(config, GX_VA_NRM));
    } else if (tcg.src == GX_TG_BINRM) {
      // GX source row 3 is Dolphin's tangent group in NBT normal arrays.
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, normal_group_load(config, 1, vidxAttr));
    } else if (tcg.src == GX_TG_TANGENT) {
      // GX source row 4 is Dolphin's binormal group in NBT normal arrays.
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, normal_group_load(config, 2, vidxAttr));
    } else if (tcg.src == GX_TG_COLOR0) {
      // The texgen colour source is the colour channel output, not the raw vertex
      // attribute (upstream aurora e938637c; Dolphin agrees).
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f(out.cc0.rgb, 1.0);", i);
    } else if (tcg.src == GX_TG_COLOR1) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f(out.cc1.rgb, 1.0);", i);
    } else
      UNLIKELY FATAL("unhandled tcg src {}", underlying(tcg.src));
    if (tcg.inputFormAB11) {
      vtxXfrAttrs += fmt::format("\n    tc{}.z = 1.0;", i);
    }
    if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
      if (info.indexAttr.test(GX_VA_TEX0MTXIDX + i)) {
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = tc{0} * ubuf.postex_mtx[in_texmtxidx{0} / 3u];", i);
      } else if (tcg.mtx == GX_IDENTITY) {
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = tc{0}.xyz;", i);
      } else {
        const u32 texMtxSlot = (tcg.mtx) / 3;
        const u32 texMtxIdx = texMtxSlot < MaxPostexMtx ? info.matrixLayout.postexRemap[texMtxSlot]
                                                        : texMtxSlot;
        CHECK(texMtxIdx != UniformMatrixLayout::kAbsent,
              "texgen {} matrix slot {} missing from the uniform layout", i, texMtxSlot);
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = tc{0} * ubuf.postex_mtx[{1}];", i, texMtxIdx);
      }
      if (tcg.type == GX_TG_MTX2x4) {
        vtxXfrAttrs += fmt::format("\n    tc{0}_tmp.z = 1.0f;", i);
      }
    } else if (tcg.type == GX_TG_SRTG) {
      vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = vec3f(tc{0}.xy, 1.0f);", i);
    }
    if (config.dualTexEnabled && tcg.normalize) {
      vtxXfrAttrs += fmt::format("\n    tc{0}_tmp = normalize(tc{0}_tmp);", i);
    }
    if (!config.dualTexEnabled || tcg.postMtx == GX_PTIDENTITY) {
      vtxXfrAttrs += fmt::format("\n    var tc{0}_proj = tc{0}_tmp;", i);
    } else {
      u32 postMtxIdx = (tcg.postMtx - GX_PTTEXMTX0) / 3;
      vtxXfrAttrs +=
          fmt::format("\n    var tc{0}_proj = vec4f(tc{0}_tmp.xyz, 1.0) * ubuf.postmtx[{1}];", i, postMtxIdx);
    }
    if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
      vtxXfrAttrs += fmt::format(
          "\n    if (tc{0}_proj.z == 0.0) {{"
          "\n      tc{0}_proj.x = clamp(tc{0}_proj.x / 2.0, -1.0, 1.0);"
          "\n      tc{0}_proj.y = clamp(tc{0}_proj.y / 2.0, -1.0, 1.0);"
          "\n    }}",
          i);
    }
    // Apply line/point tex offset
    if (config.lineMode == 3) {
      // GX_POINTS: offset S for right columns, T for bottom rows
      vtxXfrAttrs += fmt::format(
          "\n    if ((ubuf.line_texcoord_mask & (1u << {0})) != 0u) {{"
          "\n        if ((vidx & 1u) != 0u) {{ tc{0}_proj.x += ubuf.line_tex_offset; }}"
          "\n        if (vidx >= 2u) {{ tc{0}_proj.y += ubuf.line_tex_offset; }}"
          "\n    }}",
          i);
    } else if (config.lineMode != 0) {
      // GX_LINES / GX_LINESTRIP: offset one axis for perpendicular side
      vtxXfrAttrs += fmt::format(
          "\n    if ((ubuf.line_texcoord_mask & (1u << {0})) != 0u && (vidx & 1u) != 0u) {{"
          "\n        tc{0}_proj.y += ubuf.line_tex_offset;"
          "\n    }}",
          i);
    }
    if (tcg.type == GX_TG_MTX3x4) {
      vtxXfrAttrs += fmt::format("\n    out.tex{0}_uvw = tc{0}_proj.xyz;", i);
      fragmentFnPre += fmt::format(
          "\n    var tex{0}_uv = in.tex{0}_uvw.xy;"
          "\n    if (in.tex{0}_uvw.z != 0.0) {{"
          "\n      tex{0}_uv = in.tex{0}_uvw.xy / in.tex{0}_uvw.z;"
          "\n    }}",
          i);
    } else {
      vtxXfrAttrs += fmt::format("\n    out.tex{0}_uv = tc{0}_proj.xy;", i);
      fragmentFnPre += fmt::format("\n    var tex{0}_uv = in.tex{0}_uv.xy;", i);
    }
    // Enhanced metal: evaluate the normal-based (environment) texgen per pixel from the interpolated
    // normal instead of interpolating per-vertex texture coordinates, so reflections follow the
    // curvature instead of smearing across large polygons. Same matrices as the vertex path.
    const bool texMtxPerVertex = info.indexAttr.test(GX_VA_TEX0MTXIDX + i);
    if (metalActive && tcg.src == GX_TG_NRM && config.lineMode == 0 && !texMtxPerVertex &&
        (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4)) {
      vtxOutAttrs += fmt::format("\n    @location({}) tex{}_srcnrm: vec3f,", vtxOutIdx++, i);
      vtxXfrAttrs += fmt::format("\n    out.tex{0}_srcnrm = tc{0}.xyz;", i);
      std::string mtxExpr;
      if (tcg.mtx == GX_IDENTITY) {
        mtxExpr = fmt::format("metal_tc{0}.xyz", i);
      } else {
        const u32 texMtxSlot = (tcg.mtx) / 3;
        const u32 texMtxIdx = texMtxSlot < MaxPostexMtx ? info.matrixLayout.postexRemap[texMtxSlot] : texMtxSlot;
        mtxExpr = fmt::format("metal_tc{0} * ubuf.postex_mtx[{1}]", i, texMtxIdx);
      }
      fragmentFnPre += fmt::format(
          "\n    {{"
          "\n      let metal_n{0} = in.tex{0}_srcnrm;"
          "\n      let metal_tc{0} = vec4f(select(metal_n{0}, normalize(metal_n{0}), dot(metal_n{0}, metal_n{0}) > 1e-10), 1.0);"
          "\n      var metal_tmp{0} = {1};",
          i, mtxExpr);
      if (tcg.type == GX_TG_MTX2x4) {
        fragmentFnPre += fmt::format("\n      metal_tmp{0}.z = 1.0f;", i);
      }
      if (config.dualTexEnabled && tcg.normalize) {
        fragmentFnPre += fmt::format("\n      metal_tmp{0} = normalize(metal_tmp{0});", i);
      }
      if (!config.dualTexEnabled || tcg.postMtx == GX_PTIDENTITY) {
        fragmentFnPre += fmt::format("\n      var metal_proj{0} = metal_tmp{0};", i);
      } else {
        const u32 postMtxIdx = (tcg.postMtx - GX_PTTEXMTX0) / 3;
        fragmentFnPre += fmt::format(
            "\n      var metal_proj{0} = vec4f(metal_tmp{0}.xyz, 1.0) * ubuf.postmtx[{1}];", i, postMtxIdx);
      }
      fragmentFnPre += fmt::format(
          "\n      if (metal_proj{0}.z == 0.0) {{"
          "\n        metal_proj{0}.x = clamp(metal_proj{0}.x / 2.0, -1.0, 1.0);"
          "\n        metal_proj{0}.y = clamp(metal_proj{0}.y / 2.0, -1.0, 1.0);"
          "\n      }}",
          i);
      if (tcg.type == GX_TG_MTX3x4) {
        fragmentFnPre += fmt::format(
            "\n      tex{0}_uv = select(metal_proj{0}.xy, metal_proj{0}.xy / metal_proj{0}.z, metal_proj{0}.z != 0.0);",
            i);
      } else {
        fragmentFnPre += fmt::format("\n      tex{0}_uv = metal_proj{0}.xy;", i);
      }
      fragmentFnPre += "\n    }";
    }
  }
  if (uber == nullptr) {
  // Multiple TEV stages may reference the same indirect stage,
  // so we sample each indirect texture only once.
  const auto ind_scale_shift = [](const GXIndTexScale s) -> u32 {
    switch (s) {
    case GX_ITS_1:
      return 0;
    case GX_ITS_2:
      return 1;
    case GX_ITS_4:
      return 2;
    case GX_ITS_8:
      return 3;
    case GX_ITS_16:
      return 4;
    case GX_ITS_32:
      return 5;
    case GX_ITS_64:
      return 6;
    case GX_ITS_128:
      return 7;
    case GX_ITS_256:
      return 8;
    default:
      FATAL("unhandled indirect scale {}", underlying(s));
    }
  };
  for (int i = 0; i < info.usedIndStages.size(); ++i) {
    if (!info.usedIndStages.test(i)) {
      continue;
    }
    const auto& indStage = config.indStages[i];
    const int effectiveIndTexCoord = tev_effective_texcoord(config, indStage.texCoordId);
    const std::string indFixedUv =
        effectiveIndTexCoord >= 0 ? fixed_texcoord_name(static_cast<u32>(effectiveIndTexCoord)) : "vec2i(0)";
    fragmentFnPre += fmt::format(
        "\n    // Indirect stage {0}"
        "\n    let ind{0}_sample_fixed = {5} >> vec2u({3}u, {4}u);"
        "\n    let t_IndTexRaw{0} = textureSampleBias(tex{2}, tex{2}_samp, "
        "vec2f(ind{0}_sample_fixed) / (ubuf.tex{2}_size_bias.xy * 128.0), ubuf.tex{2}_size_bias.z).abg * 255.0;"
        "\n    let t_IndTexCoord{0} = vec3i(round(t_IndTexRaw{0}));",
        i, effectiveIndTexCoord, underlying(indStage.texMapId), ind_scale_shift(indStage.scaleS),
        ind_scale_shift(indStage.scaleT), indFixedUv);
  }
  const bool usesFixedTexcoordState = shader_uses_fixed_texcoord_state(config);
  if (usesFixedTexcoordState) {
    fragmentFnPre += "\n    var t_TexCoord = vec2i(0);";
  }
  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    const auto textureDependency = tev_stage_texture_dependency(config, static_cast<u32>(i));
    const bool needsIndirectCoord = stage.indTexMtxId != GX_ITM_OFF;
    const bool hasIndirectStage = stage.indTexStage < config.numIndStages;
    const bool needsTevTexCoord = textureDependency.needsFixedTexcoordState;
    const bool needsTextureSample = textureDependency.combinerUsesTexture || static_cast<int>(i) == zTexStage;
    const bool willSampleTexture = tev_texture_sample_enabled(textureDependency, needsTextureSample);
    if (!needsTevTexCoord && !needsTextureSample) {
      continue;
    }
    const bool hasBaseCoord = textureDependency.texCoordId >= 0;
    std::string uvIn;
    if (needsTevTexCoord) {
      fragmentFnPre += fmt::format("\n    // TEV stage {} indirect", i);

      // Apply indirect texture matrix (produces an S17.7 fixed-point offset).
      std::string indirectOffsetFixed;
      if (needsIndirectCoord && hasIndirectStage) {
        u32 fmtShift = 0;
        switch (stage.indTexFormat) {
        case GX_ITF_8:
          break;
        case GX_ITF_5:
          fmtShift = 3;
          break;
        case GX_ITF_4:
          fmtShift = 4;
          break;
        case GX_ITF_3:
          fmtShift = 5;
          break;
        default:
          FATAL("unhandled indirect format {}", underlying(stage.indTexFormat));
        }
        if (fmtShift == 0) {
          fragmentFnPre += fmt::format("\n    var ind{0}_coord = t_IndTexCoord{1};", i, underlying(stage.indTexStage));
        } else {
          fragmentFnPre += fmt::format("\n    var ind{0}_coord = t_IndTexCoord{1} >> vec3u({2}u);", i,
                                       underlying(stage.indTexStage), fmtShift);
        }

        if (stage.indTexBiasSel != GX_ITB_NONE) {
          auto bias = stage.indTexFormat == GX_ITF_8 ? "-128"sv : "1"sv;
          auto biasS = "0"sv, biasT = "0"sv, biasU = "0"sv;
          if (stage.indTexBiasSel == GX_ITB_S || stage.indTexBiasSel == GX_ITB_ST || stage.indTexBiasSel == GX_ITB_SU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasS = "1"sv;
          }
          if (stage.indTexBiasSel == GX_ITB_T || stage.indTexBiasSel == GX_ITB_ST || stage.indTexBiasSel == GX_ITB_TU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasT = "1"sv;
          }
          if (stage.indTexBiasSel == GX_ITB_U || stage.indTexBiasSel == GX_ITB_SU || stage.indTexBiasSel == GX_ITB_TU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasU = "1"sv;
          }
          fragmentFnPre += fmt::format("\n    ind{0}_coord = ind{0}_coord + vec3i({1}, {2}, {3}) * {4};", i, biasS,
                                       biasT, biasU, bias);
        }

        const auto appendScaleShift = [&](u32 mtxIdx) {
          fragmentFnPre += fmt::format(
              "\n    let ind{0}_shift = ubuf.ind_mtx[{1}].z;"
              "\n    if (ind{0}_shift >= 0) {{"
              "\n      ind{0}_offset_fixed = ind{0}_offset_fixed >> vec2u(u32(ind{0}_shift));"
              "\n    }} else {{"
              "\n      ind{0}_offset_fixed = ind{0}_offset_fixed << vec2u(u32(-ind{0}_shift));"
              "\n    }}",
              i, mtxIdx * 2 + 1);
          indirectOffsetFixed = fmt::format("ind{}_offset_fixed", i);
        };
        // Smooth indirect offsets (ind_mtx[odd].w, see g_smoothIndirect): the integer path above is the
        // hardware's 8-bit offset; this adds the sub-byte fraction of the filtered indirect sample with
        // the same matrix and scale. Exact (point-sampled) bytes give a zero fraction, so such draws keep
        // the hardware result; only the filtered in-between values move, which removes the whole-texel
        // steps distortion effects show at high resolution. ITF_8 only: smaller formats pack fields.
        const auto appendSmooth = [&](u32 mtxIdx, const std::string& fracDot) {
          if (fmtShift != 0) return;
          fragmentFnPre += fmt::format(
              "\n    if (ubuf.ind_mtx[{1}].w != 0) {{"
              "\n      let ind{0}_frac = t_IndTexRaw{2} - vec3f(t_IndTexCoord{2});"
              "\n      let ind{0}_fd = {3} * exp2(-f32(ubuf.ind_mtx[{1}].z));"
              "\n      ind{0}_offset_fixed = ind{0}_offset_fixed + vec2i(round(ind{0}_fd));"
              "\n    }}",
              i, mtxIdx * 2 + 1, underlying(stage.indTexStage), fmt::format(fmt::runtime(fracDot), i));
        };

        if (stage.indTexMtxId >= GX_ITM_0 && stage.indTexMtxId <= GX_ITM_2) {
          // Static 2x3 matrix: integer dot products, then the hardware's fixed >>3 and exponent shift sequence.
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_0;
          fragmentFnPre += fmt::format(
              "\n    let ind{0}_c0 = ubuf.ind_mtx[{1}];"
              "\n    let ind{0}_c1 = ubuf.ind_mtx[{2}];"
              "\n    var ind{0}_offset_fixed = vec2i("
              "\n      (ind{0}_c0.x * ind{0}_coord.x + ind{0}_c0.z * ind{0}_coord.y + "
              "ind{0}_c1.x * ind{0}_coord.z) >> 3u,"
              "\n      (ind{0}_c0.y * ind{0}_coord.x + ind{0}_c0.w * ind{0}_coord.y + "
              "ind{0}_c1.y * ind{0}_coord.z) >> 3u);",
              i, mtxIdx * 2, mtxIdx * 2 + 1);
          appendScaleShift(mtxIdx);
          appendSmooth(mtxIdx,
                       "vec2f(f32(ind{0}_c0.x) * ind{0}_frac.x + f32(ind{0}_c0.z) * ind{0}_frac.y + "
                       "f32(ind{0}_c1.x) * ind{0}_frac.z, f32(ind{0}_c0.y) * ind{0}_frac.x + "
                       "f32(ind{0}_c0.w) * ind{0}_frac.y + f32(ind{0}_c1.y) * ind{0}_frac.z) / 8.0");
        } else if (stage.indTexMtxId >= GX_ITM_S0 && stage.indTexMtxId <= GX_ITM_S2 && hasBaseCoord) {
          // Dynamic S: (fixed UV * indirect S) >> 8, then exponent shift.
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_S0;
          u32 regTexCoord = static_cast<u32>(textureDependency.texCoordId);
          const auto fixedUv = fixed_texcoord_name(regTexCoord);
          fragmentFnPre += fmt::format(
              "\n    var ind{0}_offset_fixed = ({1} * vec2i(ind{0}_coord.x)) >> vec2u(8u);", i, fixedUv);
          appendScaleShift(mtxIdx);
          appendSmooth(mtxIdx, "vec2f(" + fixedUv + ") * ind{0}_frac.x / 256.0");
        } else if (stage.indTexMtxId >= GX_ITM_T0 && stage.indTexMtxId <= GX_ITM_T2 && hasBaseCoord) {
          // Dynamic T: (fixed UV * indirect T) >> 8, then exponent shift.
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_T0;
          u32 regTexCoord = static_cast<u32>(textureDependency.texCoordId);
          const auto fixedUv = fixed_texcoord_name(regTexCoord);
          fragmentFnPre += fmt::format(
              "\n    var ind{0}_offset_fixed = ({1} * vec2i(ind{0}_coord.y)) >> vec2u(8u);", i, fixedUv);
          appendScaleShift(mtxIdx);
          appendSmooth(mtxIdx, "vec2f(" + fixedUv + ") * ind{0}_frac.y / 256.0");
        }
      }

      // Wrap base coord and combine with the indirect translation.
      auto wrap_comp = [](GXIndTexWrap wrap, std::string&& coord) -> std::string {
        const int mask = tev_indirect_wrap_mask(wrap);
        if (mask == -1) {
          return std::move(coord);
        }
        if (mask == 0) {
          return "0";
        }
        CHECK(mask > 0, "invalid indirect wrap {}", underlying(wrap));
        return fmt::format("({} & {})", coord, mask);
      };
      std::string baseCoordExpr = "vec2i(0)";
      if (hasBaseCoord) {
        u32 texCoordId = static_cast<u32>(textureDependency.texCoordId);
        baseCoordExpr = fixed_texcoord_name(texCoordId);
      }
      const std::string wrappedExpr =
          fmt::format("vec2i({}, {})", wrap_comp(stage.indTexWrapS, fmt::format("{}.x", baseCoordExpr)),
                      wrap_comp(stage.indTexWrapT, fmt::format("{}.y", baseCoordExpr)));
      std::string finalCoord = wrappedExpr;
      if (!indirectOffsetFixed.empty()) {
        finalCoord = fmt::format("{} + ({})", wrappedExpr, indirectOffsetFixed);
      }

      if (usesFixedTexcoordState) {
        if (stage.indTexAddPrev) {
          fragmentFnPre += fmt::format("\n    t_TexCoord += {};", finalCoord);
        } else {
          fragmentFnPre += fmt::format("\n    t_TexCoord = {};", finalCoord);
        }
        fragmentFnPre += "\n    t_TexCoord = tev_s24_fixed_vec2i(t_TexCoord);";

        // ShaderInfo only emits texN_size_bias and texture bindings for a texture that can actually be sampled.
        if (willSampleTexture) {
          u32 texMapId = static_cast<u32>(textureDependency.texMapId);
          fragmentFnPre += fmt::format(
              "\n    var ind{0}_uv = vec2f(t_TexCoord) / (ubuf.tex{1}_size_bias.xy * 128.0);", i, texMapId);
          uvIn = fmt::format("ind{0}_uv", i);
        }
      }
    }
    if (!willSampleTexture) {
      continue;
    }
    bool plainUv = false;
    if (uvIn.empty()) {
      // No indirect texturing
      const auto fixedUv = fixed_texcoord_name(static_cast<u32>(textureDependency.texCoordId));
      uvIn = fmt::format("vec2f(tev_s24_fixed_vec2i({0})) / (ubuf.tex{1}_size_bias.xy * 128.0)", fixedUv,
                         textureDependency.texMapId);
      plainUv = true;
    }
    if (plainUv && envActive && static_cast<int>(i) == envLightmapStage) {
      // Smoothed lightmap filtering (lit_ext[0].w): the texel fraction goes through smoothstep before
      // the bilinear tap, which removes the diamond and stair artefacts of the low-resolution
      // lightmaps with the same single sample (no wider footprint: atlas padding does not bleed).
      fragmentFnPre += fmt::format(
          "\n    var sampled{0}_uv = {2};"
          "\n    if (ubuf.lit_ext[0].w > 0.5) {{"
          "\n      let lm{0}_p = sampled{0}_uv * ubuf.tex{1}_size_bias.xy + vec2f(0.5);"
          "\n      let lm{0}_f = fract(lm{0}_p);"
          "\n      sampled{0}_uv = (floor(lm{0}_p) + lm{0}_f * lm{0}_f * (3.0 - 2.0 * lm{0}_f) - vec2f(0.5)) / ubuf.tex{1}_size_bias.xy;"
          "\n    }}"
          "\n    var sampled{0} = textureSampleBias(tex{1}, tex{1}_samp, sampled{0}_uv, ubuf.tex{1}_size_bias.z);"
          // Lightmap dither (modern lighting, env_light_mask.w bit 0): triangular noise of one 8-bit step
          // from two interleaved-gradient patterns, so the doubled lightmap no longer shows bands.
          "\n    if ((ubuf.env_light_mask.w & 1u) != 0u) {{"
          "\n      let lm{0}_n1 = fract(52.9829189 * fract(dot(in.pos.xy, vec2f(0.06711056, 0.00583715))));"
          "\n      let lm{0}_n2 = fract(52.9829189 * fract(dot(in.pos.xy + vec2f(47.0, 17.0), vec2f(0.06711056, 0.00583715))));"
          "\n      sampled{0} = vec4f(clamp(sampled{0}.rgb + vec3f((lm{0}_n1 + lm{0}_n2 - 1.0) / 255.0), vec3f(0.0), vec3f(1.0)), sampled{0}.a);"
          "\n    }}",
          i, textureDependency.texMapId, uvIn);
    } else if (plainUv && shader_soft_shadow(config) && uber == nullptr && static_cast<int>(i) == shadow_projection_stage(config)) {
      // Soft projected shadow: centre plus four diagonal bilinear taps one texel away (about a 3x3
      // filter), so the edge of the game's small shadow texture is no longer a hard, stepped line.
      fragmentFnPre += fmt::format(
          "\n    let sh{0}_uv = {2};"
          "\n    let sh{0}_t = vec2f(1.0) / max(ubuf.tex{1}_size_bias.xy, vec2f(1.0));"
          "\n    var sampled{0} = textureSampleBias(tex{1}, tex{1}_samp, sh{0}_uv, ubuf.tex{1}_size_bias.z) * 0.25 +"
          "\n        (textureSampleBias(tex{1}, tex{1}_samp, sh{0}_uv + sh{0}_t * vec2f(1.0, 1.0), ubuf.tex{1}_size_bias.z) +"
          "\n         textureSampleBias(tex{1}, tex{1}_samp, sh{0}_uv + sh{0}_t * vec2f(-1.0, 1.0), ubuf.tex{1}_size_bias.z) +"
          "\n         textureSampleBias(tex{1}, tex{1}_samp, sh{0}_uv + sh{0}_t * vec2f(1.0, -1.0), ubuf.tex{1}_size_bias.z) +"
          "\n         textureSampleBias(tex{1}, tex{1}_samp, sh{0}_uv + sh{0}_t * vec2f(-1.0, -1.0), ubuf.tex{1}_size_bias.z)) * 0.1875;",
          i, textureDependency.texMapId, uvIn);
    } else {
      fragmentFnPre +=
          fmt::format("\n    var sampled{0} = textureSampleBias(tex{1}, tex{1}_samp, {2}, ubuf.tex{1}_size_bias.z);", i,
                      textureDependency.texMapId, uvIn);
    }
    if (metalActive && textureDependency.texCoordId >= 0 &&
        static_cast<u32>(textureDependency.texCoordId) < MaxTexCoord &&
        config.tcgs[static_cast<u32>(textureDependency.texCoordId)].src == GX_TG_NRM) {
      // Enhanced metal: stronger environment reflection at grazing angles (Fresnel).
      fragmentFnPre += fmt::format(
          "\n    sampled{0} = vec4f(min(sampled{0}.rgb * (1.0 + metal_k * metal_edge * metal_fres), vec3f(1.0)), sampled{0}.a);",
          i);
    }
  }

  }
  // in.pos.z is the host NDC z (forward: 0=near/1=far; reversed: 1=near/0=far post-fix), but this
  // expression needs to produce GX's own native distance term (always 0=near/1=far, matching how
  // g_gxState.clearDepth/clear_depth_value() are interpreted before their own UseReversedZ
  // inversion) - forward already matches directly; reversed needs the same 1-x flip everything
  // else reversed-Z-aware uses. This was backwards (verified directly against upstream aurora's
  // identical expression in build_shader_source), which fed both fog density and the GX_ZT_ADD
  // z-texture path the wrong distance value.
  std::string fogDepthExpr = UseReversedZ ? "(1.0 - in.pos.z)" : "in.pos.z";
  std::string fogZCoordExpr =
      fmt::format("u32(round(clamp({}, 0.0, 1.0) * 16777216.0))", fogDepthExpr);
  if (usesZTextureDepth) {
    const u32 zTexBias = config.zTexture & 0x00FFFFFFu;
    const u32 zTexFmt = (config.zTexture >> 24) & 0x3u;
    const u32 zTexOp = (config.zTexture >> 26) & 0x3u;
    const std::string zTexSample = uber != nullptr ? "sampled_z"s
                                   : zTexStage >= 0 ? fmt::format("sampled{}", zTexStage) : "vec4f(0.0)"s;

    fragmentFn += fmt::format(
        "\n    // Z texture"
        "\n    let ztexSample = clamp({0}, vec4f(0.0), vec4f(1.0)) * 255.0;"
        "\n    let ztexRaw = vec4u(round(ztexSample));",
        zTexSample);
    switch (zTexFmt) {
    case 0:
      fragmentFn += fmt::format("\n    var ztexCoord = ({0}u + ztexRaw.a) & 0x00ffffffu;", zTexBias);
      break;
    case 1:
      fragmentFn +=
          fmt::format("\n    var ztexCoord = ({0}u + ((ztexRaw.a << 8u) | ztexRaw.r)) & 0x00ffffffu;", zTexBias);
      break;
    case 2:
    default:
      fragmentFn += fmt::format(
          "\n    var ztexCoord = ({0}u + ((ztexRaw.r << 16u) | (ztexRaw.g << 8u) | ztexRaw.b)) & 0x00ffffffu;",
          zTexBias);
      break;
    }
    if (zTexOp == GX_ZT_ADD) {
      fragmentFn += fmt::format(
          "\n    let oldZ = u32(round(clamp({0}, 0.0, 1.0) * 16777216.0));"
          "\n    ztexCoord = (ztexCoord + oldZ) & 0x00ffffffu;",
          UseReversedZ ? "(1.0 - in.pos.z)" : "in.pos.z");
    }
    fragmentFn += "\n    let ztexDepth = f32(ztexCoord) / 16777216.0;";
    fogZCoordExpr = "ztexCoord";
  }

  if (info.usesPTTexMtx.any())
    uniBufAttrs += fmt::format("\n    postmtx: array<mat3x4f, {}>,", MaxPTTexMtx);
  if (info.usesFog) {
    uniformPre +=
        "\n"
        "struct Fog {\n"
        "    color: vec4f,\n"
        "    a: f32,\n"
        "    b: f32,\n"
        "    c: f32,\n"
        "    pad: f32,\n"
        "    range_base: vec4f,\n"
        "    range_k: array<vec4f, 3>,\n"
        "}";
    uniBufAttrs += "\n    fog: Fog,";

    if ((config.fogType & 0x8u) == 0) {
      fragmentFn += fmt::format(
          "\n    // Fog"
          "\n    let fogZCoord = {};"
          "\n    let fogShiftedZ = fogZCoord >> u32(ubuf.fog.pad);"
          "\n    var fogZe = (ubuf.fog.a * 16777216.0) / (ubuf.fog.b - f32(fogShiftedZ));",
          fogZCoordExpr);
    } else {
      fragmentFn += fmt::format(
          "\n    // Fog"
          "\n    let fogZCoord = {};"
          "\n    var fogZe = ubuf.fog.a * f32(fogZCoord) / 16777216.0;",
          fogZCoordExpr);
    }
    if (config.fogRangeAdjust) {
      fragmentFn +=
          "\n    let fogRangeOffset = (2.0 * (in.pos.x / ubuf.fog.range_base.y)) - 1.0 - ubuf.fog.range_base.x;"
          "\n    let fogRangeFloatIndex = clamp(9.0 - abs(fogRangeOffset) * 9.0, 0.0, 9.0);"
          "\n    let fogRangeIndexLower = u32(fogRangeFloatIndex);"
          "\n    let fogRangeIndexUpper = fogRangeIndexLower + 1u;"
          "\n    let fogRangeKLower = ubuf.fog.range_k[fogRangeIndexLower >> 2u][fogRangeIndexLower & 3u];"
          "\n    let fogRangeKUpper = ubuf.fog.range_k[fogRangeIndexUpper >> 2u][fogRangeIndexUpper & 3u];"
          "\n    let fogRangeK = mix(fogRangeKLower, fogRangeKUpper, fract(fogRangeFloatIndex));"
          "\n    fogZe *= sqrt(fogRangeOffset * fogRangeOffset + fogRangeK * fogRangeK) / fogRangeK;";
    }
    fragmentFn += "\n    var fogF = clamp(fogZe - ubuf.fog.c, 0.0, 1.0);";
    switch (config.fogType) {
      DEFAULT_FATAL("invalid fog type {}", config.fogType);
    case static_cast<GXFogType>(1):
    case static_cast<GXFogType>(3):
    case GX_FOG_PERSP_LIN:
    case static_cast<GXFogType>(9):
    case static_cast<GXFogType>(11):
    case GX_FOG_ORTHO_LIN:
      fragmentFn += "\n    var fogZ = fogF;";
      break;
    case GX_FOG_PERSP_EXP:
    case GX_FOG_ORTHO_EXP:
      fragmentFn += "\n    var fogZ = 1.0 - exp2(-8.0 * fogF);";
      break;
    case GX_FOG_PERSP_EXP2:
    case GX_FOG_ORTHO_EXP2:
      fragmentFn += "\n    var fogZ = 1.0 - exp2(-8.0 * fogF * fogF);";
      break;
    case GX_FOG_PERSP_REVEXP:
    case GX_FOG_ORTHO_REVEXP:
      fragmentFn += "\n    var fogZ = exp2(-8.0 * (1.0 - fogF));";
      break;
    case GX_FOG_PERSP_REVEXP2:
    case GX_FOG_ORTHO_REVEXP2:
      fragmentFn +=
          "\n    fogF = 1.0 - fogF;"
          "\n    var fogZ = exp2(-8.0 * fogF * fogF);";
      break;
    }
    fragmentFn += "\n    prev = vec4f(mix(prev.rgb, ubuf.fog.color.rgb, clamp(fogZ, 0.0, 1.0)), prev.a);";
  }
  if (info.usedIndTexMtxs.any()) {
    uniBufAttrs += "\n    ind_mtx: array<vec4i, 6>,";
  }
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (info.sampledTexCoords.test(i)) {
      uniBufAttrs += fmt::format("\n    texcoord{}_scale: vec4f,", i);
    }
  }
  for (int i = 0; i < info.sampledTextures.size(); ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    uniBufAttrs += fmt::format("\n    tex{}_size_bias: vec4f,", i);
    texBindings += fmt::format(
        "\n@group(2) @binding({1})\n"
        "var tex{0}: texture_2d<f32>;\n"
        "@group(2) @binding({2})\n"
        "var tex{0}_samp: sampler;",
        i, i * 2, i * 2 + 1);
  }
  fragmentFn += "\n    prev = tev_overflow_vec4f(prev);";
  if (uber != nullptr) {
    fragmentFn += UberAlphaCompareWgsl;
  } else if (config.alphaCompare) {
    const auto comp0 = alpha_compare(config.alphaCompare.comp0, config.alphaCompare.ref0);
    const auto comp1 = alpha_compare(config.alphaCompare.comp1, config.alphaCompare.ref1);
    AlphaCompareExpr pass;
    switch (config.alphaCompare.op) {
    default:
      pass = alpha_compare_and(comp0, comp1);
      break;
    case GX_AOP_AND:
      pass = alpha_compare_and(comp0, comp1);
      break;
    case GX_AOP_OR:
      pass = alpha_compare_or(comp0, comp1);
      break;
    case GX_AOP_XOR:
      pass = alpha_compare_xor(comp0, comp1);
      break;
    case GX_AOP_XNOR:
      pass = alpha_compare_xnor(comp0, comp1);
      break;
    }
    const auto discard = alpha_compare_not(pass);
    if (discard.constant == 1) {
      fragmentFn += "\n    // Alpha compare\n    discard;";
    } else if (discard.constant != 0) {
      fragmentFn += "\n    // Alpha compare"
                    "\n    let alphaCompare = u32(round(clamp(prev.a, 0.0, 1.0) * 255.0));";
      fragmentFn += fmt::format("\n    if ({}) {{ discard; }}", discard.expr);
    }
  }

  if (config.dstAlpha != UINT32_MAX) {
    // Destination alpha replaces the alpha written to the EFB, after the alpha compare. The value
    // is a uniform (the last field, as build_uniform appends it) so animated values share a pipeline.
    uniBufAttrs += "\n    dst_alpha: vec4f,";
    fragmentFn += "\n    prev.a = ubuf.dst_alpha.x;";
  }
  if ((metalActive || envActive) && uber == nullptr) {
    // Material parameters (AuroraMaterialExt + time), appended after dst_alpha like build_uniform.
    uniBufAttrs += "\n    mat_ext: array<vec4f, 8>,";
    // Global lighting flags and the frame's measured colours (LitExtFloats, build_uniform).
    uniBufAttrs += "\n    lit_ext: array<vec4f, 3>,";
  }
  if (envActive) {
    uniBufAttrs += fmt::format("\n    env_lights: array<Light, {}>,\n    env_light_mask: vec4u,", MaxEnvLights);
  }
  if (uber != nullptr) {
    uniBufAttrs += UberUniformFields;
  }
  if ((metalActive || envActive) && uber == nullptr) {
    int litChannel = -1;
    for (int c = 0; c < 2 && litChannel < 0 && metalActive; ++c)
      if (config.colorChannels[c].lightingEnabled) litChannel = c;
    // Lights of the block: the draw's own GX lights (channel litChannel), or for the environment
    // material the borrowed ones, attenuated by distance and suppressed where the lightmap is dark
    // (baked shadow). The surroundings' light (metal_lc) is the draw's lights or the lightmap.
    const std::string lightsExpr = envActive ? "ubuf.env_lights"s : "ubuf.lights"s;
    const std::string maskExpr =
        envActive ? "ubuf.env_light_mask.x"s : fmt::format("ubuf.lightState{}", std::max(litChannel, 0));
    const u32 lightCount = envActive ? static_cast<u32>(MaxEnvLights) : static_cast<u32>(GX::MaxLights);
    const std::string attMetal =
        envActive ? "env_att(metal_light, in.mv_pos) * env_gate(metal_light, in.mv_pos, env_shadow, ubuf.env_light_mask.y)"s
                  : "1.0"s;
    const std::string attCoat =
        envActive ? "env_att(cc_light, in.mv_pos) * env_gate(cc_light, in.mv_pos, env_shadow, ubuf.env_light_mask.y)"s
                  : "1.0"s;
    const std::string lcExpr = envActive ? "clamp(env_lm * 1.6, vec3f(0.0), vec3f(1.5))"s
                                         : "clamp(metal_lsum, vec3f(0.0), vec3f(1.5))"s;
    if (envActive) {
      // Lightmap texel of this fragment (the TEV stage sampling the second UV set).
      fragmentFn += fmt::format("\n    let env_lm = sampled{}.rgb;"
                                "\n    let env_shadow = smoothstep(0.03, 0.35, dot(env_lm, vec3f(0.299, 0.587, 0.114)));",
                                envLightmapStage);
    }
    // Material sparkle needs texture coordinates fixed to the surface: the first texcoord generated
    // from a vertex texture attribute (environment texgens from the normal would make flakes swim).
    std::string sparkleUv;
    for (int i = 0; i < info.sampledTexCoords.size() && sparkleUv.empty(); ++i) {
      if (!info.sampledTexCoords.test(i)) continue;
      const auto& tcg = config.tcgs[i];
      const bool fromVertexUv = (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) || tcg.src == GX_MAX_TEXGENSRC;
      if (fromVertexUv && (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4)) sparkleUv = fmt::format("tex{}_uv", i);
    }
    // Modern lighting, level 2 (lit_ext[1].w: bit 0 texture relief, bit 1 game material masks). The
    // relief source is the surface's own texture sample; the reflection mask counts only when it is a
    // texture of its own (the diffuse texture as mask already tints the reflectance through f0).
    const int matDiffuseStage = material_diffuse_stage(config);
    const int matMaskStage = metalActive ? material_reflection_mask_stage(config) : -1;
    const bool matMaskOwnTexture =
        matMaskStage >= 0 && (matDiffuseStage < 0 || config.tevStages[matMaskStage].texMapId !=
                                                         config.tevStages[matDiffuseStage].texMapId);
    const std::string reliefExpr =
        matDiffuseStage >= 0 ? fmt::format("sampled{}.rgb", matDiffuseStage) : "metal_surf"s;
    const std::string maskExpr2 =
        matMaskOwnTexture
            ? fmt::format("smoothstep(0.02, 0.30, dot(sampled{}.rgb, vec3f(0.299, 0.587, 0.114)))", matMaskStage)
            : "1.0"s;
    const std::string emisExpr = matEmissiveStage >= 0 ? "mat_emis_add"s : "vec3f(0.0)"s;
    if (litChannel >= 0 || envActive) {
      fragmentFn += fmt::format(R"""(
    // Enhanced metal (PBR-style): GGX specular with Schlick Fresnel from the GX lights of channel {0},
    // metal-tinted reflectance, a procedural environment reflection lit by the scene's lights, less
    // diffuse for metal, and a detail normal derived from the colour gradient (panel lines, seams).
    {{
      var metal_rough = max(f32(metal_pk & 255u) / 255.0, 0.04);
      var metal_metallic = f32((metal_pk >> 8u) & 255u) / 255.0;
      // Clear coat and paint/bare-metal split (AuroraMaterialExt coat*, paintSplit, bare*): mat_ext[6].w
      // and mat_ext[7]. Zero for every class that does not use them, so the rest is unchanged.
      let mat_e6 = ubuf.mat_ext[6];
      let mat_e7 = ubuf.mat_ext[7];
      // lit_ext: x soft highlights, y linear blending, z area colours valid; [1] / [2] the frame's
      // average colour above / below the screen centre (a few frames late, smoothed).
      let lit_e0 = ubuf.lit_ext[0];
      let lit_area = lit_e0.z > 0.5;
      let lit_up = ubuf.lit_ext[1].rgb;
      let lit_down = ubuf.lit_ext[2].rgb;
      // Level 2 flags. metal_surf: the surface without the light its emissive stage added.
      let lit_fl = u32(ubuf.lit_ext[1].w + 0.5);
      let lit_masks = f32((lit_fl >> 1u) & 1u);
      let metal_emis = {10} * lit_masks;
      let metal_surf = max(prev.rgb - metal_emis, vec3f(0.0));
      var metal_paint = 1.0;
      if (mat_e7.y > 0.0) {{
        // Saturated colour = paint; grey or very dark texels = bare metal. Saturation does not depend
        // on how brightly the texel is lit.
        let metal_cmax = max(metal_surf.r, max(metal_surf.g, metal_surf.b));
        let metal_sat = (metal_cmax - min(metal_surf.r, min(metal_surf.g, metal_surf.b))) / max(metal_cmax, 1e-4);
        let metal_isPaint = smoothstep(0.22, 0.45, metal_sat) * smoothstep(0.02, 0.06, metal_cmax);
        metal_paint = mix(1.0, metal_isPaint, clamp(mat_e7.y, 0.0, 1.0));
        metal_rough = mix(max(mat_e7.w, 0.04), metal_rough, metal_paint);
        metal_metallic = mix(mat_e7.z, metal_metallic, metal_paint);
      }}
      let metal_detail = f32((metal_pk >> 24u) & 255u) / 255.0;
      // Detail normal: surface-gradient bump from the luminance of the shaded colour, treated as a
      // height of a few millimetres at most (Prime units are metres); larger scales turn texture noise
      // into sparkles. With texture relief (level 2) the height is the surface's own texture sample,
      // so baked shadows and the falloff of the lights are not read as relief.
      let metal_lum = dot(mix(metal_surf, {8}, f32(lit_fl & 1u)), vec3f(0.299, 0.587, 0.114));
      let metal_dpx = dpdx(in.mv_pos);
      let metal_dpy = dpdy(in.mv_pos);
      let metal_dlx = dpdx(metal_lum);
      let metal_dly = dpdy(metal_lum);
      let metal_r1 = cross(metal_dpy, ppl_nrm);
      let metal_r2 = cross(ppl_nrm, metal_dpx);
      let metal_det = dot(metal_dpx, metal_r1);
      let metal_grad = sign(metal_det) * (metal_dlx * metal_r1 + metal_dly * metal_r2);
      var metal_n = ppl_nrm;
      if (abs(metal_det) > 1e-12 && metal_detail > 0.0) {{
        // Fine, subtle relief: small amplitude, and the gradient is capped so single noisy texels
        // cannot tilt the normal enough to produce sparkles.
        let metal_gl = length(metal_grad);
        let metal_gcap = abs(metal_det) * 0.55;
        let metal_g = select(metal_grad, metal_grad * (metal_gcap / metal_gl), metal_gl > metal_gcap);
        metal_n = normalize(abs(metal_det) * ppl_nrm - metal_g * metal_detail * 0.0045);
      }}
      // Highlights use a normal only halfway towards the relief (sharp GGX peaks on tilted texels read
      // as glitter); the environment reflection uses the full relief.
      let metal_ns = normalize(mix(ppl_nrm, metal_n, 0.5));
      let metal_ndv = max(dot(metal_n, metal_v), 1e-3);
      let metal_albedo = clamp(metal_surf * 1.25, vec3f(0.0), vec3f(1.0));
      let metal_f0 = mix(vec3f(0.04), metal_albedo, metal_metallic);
      let metal_a = metal_rough * metal_rough;
      let metal_a2 = metal_a * metal_a;
      let metal_kg = (metal_rough + 1.0) * (metal_rough + 1.0) / 8.0;
      var metal_spec = vec3f(0.0);
      var metal_lsum = vec3f(0.0);
      // Level 2, scene relighting (lit_ext[2].w, only on lightmapped walls and floors): the engine's
      // lights also give diffuse light, Lambert on the relief normal, with the same reach and baked
      // shadow gate as the highlights.
      var metal_rel = vec3f(0.0);
      for (var li = 0u; li < {1}u; li++) {{
        if (({4} & (1u << li)) == 0u) {{ continue; }}
        let metal_light = {3}[li];
        metal_lsum += metal_light.color.rgb;
        let metal_ld = normalize(metal_light.pos - in.mv_pos);
        let metal_h = normalize(metal_ld + metal_v);
        let metal_ndl = max(dot(metal_ns, metal_ld), 0.0);
        let metal_ndh = max(dot(metal_ns, metal_h), 0.0);
        let metal_vdh = max(dot(metal_v, metal_h), 0.0);
        let metal_dd = metal_ndh * metal_ndh * (metal_a2 - 1.0) + 1.0;
        let metal_D = metal_a2 / (3.14159265 * metal_dd * metal_dd);
        let metal_G = (metal_ndl / (metal_ndl * (1.0 - metal_kg) + metal_kg)) *
                      (metal_ndv / (metal_ndv * (1.0 - metal_kg) + metal_kg));
        let metal_F = metal_f0 + (vec3f(1.0) - metal_f0) * pow(1.0 - metal_vdh, 5.0);
        metal_spec += metal_D * metal_G * metal_F / (4.0 * metal_ndv + 1e-4) * metal_light.color.rgb * ({5});
        metal_rel += metal_light.color.rgb * max(dot(metal_n, metal_ld), 0.0) * ({5});
      }}
      // Procedural environment: brighter above, dark below, tinted by the scene's lights; roughness
      // flattens it towards its average (a blurrier reflection).
      let metal_r = reflect(-metal_v, metal_n);
      let metal_up = clamp(0.5 + 0.5 * metal_r.y, 0.0, 1.0);
      let metal_lc = {6};
      var metal_env_sharp = mix(metal_lc * 0.12, metal_lc * 0.9, metal_up * metal_up) +
                            metal_lc * 0.35 * exp(-abs(metal_r.y) * 12.0);
      var metal_env_dull = metal_lc * 0.4;
      if (lit_area) {{
        // Reflect the area's own colours: what is above and below in this frame, keeping the lights'
        // bright horizon band.
        let area_sharp = mix(lit_down * 0.7, lit_up * 1.5, metal_up * metal_up) + metal_lc * 0.25 * exp(-abs(metal_r.y) * 12.0);
        metal_env_sharp = mix(metal_env_sharp, area_sharp, 0.7);
        metal_env_dull = mix(metal_env_dull, (lit_up + lit_down) * 0.6, 0.7);
      }}
      let metal_env = mix(metal_env_sharp, metal_env_dull, metal_rough);
      let metal_fv = metal_f0 + (vec3f(1.0) - metal_f0) * pow(1.0 - metal_ndv, 5.0) * (1.0 - metal_rough);
      // No change on glowing surfaces (Prime writes the glow amount in alpha: visor glass, energy).
      let metal_glow = 1.0 - smoothstep(0.6, 0.9, prev.a);
      // Game material masks (level 2): the game's own reflection mask scales the metal per pixel.
      let metal_w = metal_k * metal_glow * mix(1.0, {9}, lit_masks);
      // Material parameters (AuroraMaterialExt): colour and highlight multipliers.
      let mat_e0 = ubuf.mat_ext[0];
      let mat_e1 = ubuf.mat_ext[1];
      let metal_base = metal_surf * (1.0 - metal_metallic * 0.45 * min(metal_w, 1.0));
      let metal_luma = dot(metal_base, vec3f(0.299, 0.587, 0.114));
      let metal_diffuse = max(mix(vec3f(metal_luma), metal_base, mat_e0.y), vec3f(0.0)) * mat_e0.x * mat_e1.xyz;
      let metal_refl = metal_env * metal_fv * metal_metallic * 0.8 * mat_e0.w;
      var metal_spark = vec3f(0.0);
      var metal_tint = vec3f(1.0);{2}
      // Clear coat over the paint: smooth (geometric normal, not the relief), untinted f0 = 0.04.
      var metal_coat = vec3f(0.0);
      var metal_coatDim = 1.0;
      let metal_cc = clamp(mat_e6.w, 0.0, 1.0) * metal_paint * min(metal_w, 1.0);
      if (metal_cc > 0.0) {{
        let cc_rough = max(mat_e7.x, 0.03);
        let cc_a2 = cc_rough * cc_rough * cc_rough * cc_rough;
        let cc_ndv = max(dot(ppl_nrm, metal_v), 1e-3);
        var cc_spec = vec3f(0.0);
        for (var li = 0u; li < {1}u; li++) {{
          if (({4} & (1u << li)) == 0u) {{ continue; }}
          let cc_light = {3}[li];
          let cc_ld = normalize(cc_light.pos - in.mv_pos);
          let cc_h = normalize(cc_ld + metal_v);
          let cc_ndl = max(dot(ppl_nrm, cc_ld), 0.0);
          let cc_ndh = max(dot(ppl_nrm, cc_h), 0.0);
          let cc_dd = cc_ndh * cc_ndh * (cc_a2 - 1.0) + 1.0;
          let cc_D = cc_a2 / (3.14159265 * cc_dd * cc_dd);
          let cc_F = 0.04 + 0.96 * pow(1.0 - max(dot(metal_v, cc_h), 0.0), 5.0);
          // Kelemen visibility, the usual cheap term for a clear coat.
          let cc_vis = 0.25 / max(dot(cc_ld + metal_v, cc_ld + metal_v) * 0.25, 1e-3);
          cc_spec += cc_D * cc_F * cc_vis * cc_ndl * cc_light.color.rgb * ({7});
        }}
        // Same procedural environment as the metal, from the smooth normal and the coat roughness.
        let cc_r = reflect(-metal_v, ppl_nrm);
        let cc_up = clamp(0.5 + 0.5 * cc_r.y, 0.0, 1.0);
        var cc_env_sharp = mix(metal_lc * 0.12, metal_lc * 0.9, cc_up * cc_up) + metal_lc * 0.35 * exp(-abs(cc_r.y) * 12.0);
        var cc_env_dull = metal_lc * 0.4;
        if (lit_area) {{
          let cc_area = mix(lit_down * 0.7, lit_up * 1.5, cc_up * cc_up) + metal_lc * 0.25 * exp(-abs(cc_r.y) * 12.0);
          cc_env_sharp = mix(cc_env_sharp, cc_area, 0.7);
          cc_env_dull = mix(cc_env_dull, (lit_up + lit_down) * 0.6, 0.7);
        }}
        let cc_env = mix(cc_env_sharp, cc_env_dull, cc_rough);
        let cc_fv = 0.04 + 0.96 * pow(1.0 - cc_ndv, 5.0) * (1.0 - cc_rough);
        metal_coat = (min(cc_spec, vec3f(4.0)) * 0.35 * mat_e0.z + cc_env * cc_fv * mat_e0.w) * metal_cc;
        // Light the coat reflects does not reach the paint under it.
        metal_coatDim = 1.0 - cc_fv * metal_cc;
      }}
      let metal_base_out = metal_diffuse * metal_tint * metal_coatDim;
      let metal_relit = clamp({8}, vec3f(0.0), vec3f(1.0)) * min(metal_rel, vec3f(2.0)) * ubuf.lit_ext[2].w;
      let metal_add = max((metal_spec * 0.35 * mat_e0.z + metal_refl) * metal_w + metal_coat + metal_spark + metal_relit, vec3f(0.0));
      var metal_out = metal_base_out + metal_add;
      if (lit_e0.y > 0.5) {{
        // Linear blending: the added light is summed with the base in linear space.
        let lin_base = pow(max(metal_base_out, vec3f(0.0)), vec3f(2.2));
        var lin_out = lin_base + metal_add;
        if (lit_e0.x > 0.5) {{
          lin_out = lin_base + metal_add / (vec3f(1.0) + metal_add / max(vec3f(1.0) - lin_base, vec3f(0.02)));
        }}
        metal_out = pow(max(lin_out, vec3f(0.0)), vec3f(1.0 / 2.2));
      }} else if (lit_e0.x > 0.5) {{
        // Soft highlights: the added light approaches white smoothly instead of clipping flat.
        metal_out = metal_base_out + metal_add / (vec3f(1.0) + metal_add / max(vec3f(1.0) - metal_base_out, vec3f(0.02)));
      }}
      prev = vec4f(min(metal_out + metal_emis, vec3f(1.0)), prev.a);
    }})""",
                                litChannel, lightCount, sparkleUv.empty() ? std::string() : fmt::format(R"""(
      // Star dust (AuroraMaterialExt dust*/nebula*): two layers of tiny flakes fixed to the surface's
      // texture coordinates drift across it, each twinkling at its own rhythm, glowing faintly and
      // glinting when a light's half vector meets its tilted normal; a slow nebula tints the shading.
      // Flakes fade out below dustMinPixels. Time (mat_ext[1].w) is continuous wall time.
      let dust_e2 = ubuf.mat_ext[2];
      let dust_e3 = ubuf.mat_ext[3];
      let dust_e4 = ubuf.mat_ext[4];
      let dust_e5 = ubuf.mat_ext[5];
      let dust_e6 = ubuf.mat_ext[6];
      let dust_time = mat_e1.w;
      let dust_duv = max(length(dpdx({0})), length(dpdy({0})));
      if (dust_e2.x > 0.0) {{
        let dust_gate = mix(metal_glow, 1.0, clamp(dust_e6.y, 0.0, 1.0));
        let dust_up = select(vec3f(0.0, 1.0, 0.0), vec3f(1.0, 0.0, 0.0), abs(ppl_nrm.y) > 0.9);
        let dust_tu = normalize(cross(ppl_nrm, dust_up));
        let dust_tv = cross(ppl_nrm, dust_tu);
        var dust_lsum = 0.0;
        for (var li = 0u; li < {2}u; li++) {{
          if (({1} & (1u << li)) == 0u) {{ continue; }}
          dust_lsum += max(dot(ppl_nrm, normalize({3}[li].pos - in.mv_pos)), 0.0) *
                       dot({3}[li].color.rgb, vec3f(0.333));
        }}
        if (dust_e5.z > 0.0) {{
          // Nebula: smooth value noise drifting at nebulaSpeed lattice steps per second.
          let dust_np = {0} * dust_e5.w + vec2f(1.0, 0.5) * dust_time * dust_e6.x;
          let dust_ni = floor(dust_np);
          let dust_nu = fract(dust_np) * fract(dust_np) * (3.0 - 2.0 * fract(dust_np));
          var dust_corner = vec4f(0.0);
          for (var ci = 0u; ci < 4u; ci++) {{
            let dust_cc = bitcast<vec2u>(vec2i(dust_ni) + vec2i(i32(ci & 1u), i32(ci >> 1u)));
            var dust_hn = dust_cc.x * 0x27d4eb2du + dust_cc.y * 0x165667b1u + 0x3c6ef372u;
            dust_hn ^= dust_hn >> 15u; dust_hn *= 0x85ebca6bu; dust_hn ^= dust_hn >> 13u;
            dust_corner[ci] = f32(dust_hn & 1023u) / 1023.0;
          }}
          let dust_neb = mix(mix(dust_corner.x, dust_corner.y, dust_nu.x), mix(dust_corner.z, dust_corner.w, dust_nu.x), dust_nu.y);
          let dust_nh0 = clamp(abs(fract(vec3f(dust_e4.z - 0.06) + vec3f(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, vec3f(0.0), vec3f(1.0));
          let dust_nh1 = clamp(abs(fract(vec3f(dust_e4.z + 0.06) + vec3f(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, vec3f(0.0), vec3f(1.0));
          let dust_neb_tint = mix(vec3f(0.85), vec3f(1.2), mix(dust_nh0, dust_nh1, dust_neb));
          metal_tint = mix(vec3f(1.0), dust_neb_tint, clamp(dust_e5.z, 0.0, 1.0) * dust_gate);
        }}
        let dust_layers = select(1u, 2u, dust_e3.x > 0.0);
        for (var sl = 0u; sl < dust_layers; sl++) {{
          let dust_cells = dust_e2.y * select(1.0, dust_e3.x, sl == 1u);
          let dust_fw = dust_duv * dust_cells;
          let dust_px = 2.0 * 0.11 * dust_e2.z / max(dust_fw, 1e-6);
          let dust_vis = smoothstep(dust_e6.z * 0.5, max(dust_e6.z, 1e-3), dust_px);
          if (dust_vis <= 0.0) {{ continue; }}
          let dust_dir = select(vec2f(0.83, 0.55), vec2f(-0.62, 0.78) * 1.3, sl == 1u);
          let dust_p = {0} * dust_cells + dust_dir * dust_time * dust_e3.y;
          let dust_c = bitcast<vec2u>(vec2i(floor(dust_p)));
          var dust_s = dust_c.x * 1664525u + dust_c.y * 1013904223u + 0x9E3779B9u + sl * 0x51ed27u;
          dust_s ^= dust_s >> 16u; dust_s *= 0x7feb352du; dust_s ^= dust_s >> 15u; dust_s *= 0x846ca68bu; dust_s ^= dust_s >> 16u;
          var dust_t = dust_s * 0x27d4eb2du;
          dust_t ^= dust_t >> 15u; dust_t *= 0x2c1b3c6du; dust_t ^= dust_t >> 12u;
          let dust_r0 = f32(dust_s & 1023u) / 1023.0;
          let dust_r1 = f32((dust_s >> 10u) & 1023u) / 1023.0;
          let dust_r2 = f32((dust_s >> 20u) & 1023u) / 1023.0;
          let dust_r3 = f32(dust_t & 1023u) / 1023.0;
          let dust_r4 = f32((dust_t >> 10u) & 1023u) / 1023.0;
          let dust_r5 = f32((dust_t >> 20u) & 1023u) / 1023.0;
          let dust_d = length(fract(dust_p) - (vec2f(0.2) + 0.6 * vec2f(dust_r1, dust_r2)));
          let dust_rad = dust_e2.z * (0.07 + 0.08 * dust_r3);
          let dust_shape = (1.0 - smoothstep(dust_rad * 0.2, max(dust_rad, 1e-4), dust_d)) * step(dust_r0, dust_e2.w) * dust_vis;
          if (dust_shape <= 0.0) {{ continue; }}
          let dust_tw = 0.5 + 0.5 * sin(6.2831853 * (dust_time * dust_e3.z * (0.6 + 0.8 * dust_r3) + dust_r5));
          let dust_n = normalize(ppl_nrm + (dust_tu * (dust_r4 * 2.0 - 1.0) + dust_tv * (dust_r5 * 2.0 - 1.0)) * 0.6);
          let dust_exp = max(dust_e4.y, 1.0) * (0.6 + 0.8 * dust_r3);
          var dust_glint = vec3f(0.0);
          for (var li = 0u; li < {2}u; li++) {{
            if (({1} & (1u << li)) == 0u) {{ continue; }}
            let dust_ld = normalize({3}[li].pos - in.mv_pos);
            let dust_hv = normalize(dust_ld + metal_v);
            dust_glint += pow(max(dot(dust_n, dust_hv), 0.0), dust_exp) * max(dot(ppl_nrm, dust_ld), 0.0) * {3}[li].color.rgb;
          }}
          let dust_h = fract(dust_e4.z + (dust_r2 - 0.5) * dust_e4.w);
          let dust_rgb = clamp(abs(fract(vec3f(dust_h) + vec3f(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, vec3f(0.0), vec3f(1.0));
          let dust_core = smoothstep(0.55, 1.0, dust_shape);
          let dust_tint = mix(mix(vec3f(1.0), dust_rgb, clamp(dust_e5.x * (0.7 + 0.3 * dust_r4), 0.0, 1.0)), vec3f(1.0), dust_core * dust_e5.y);
          let dust_own = dust_e3.w * (0.4 + 0.6 * min(dust_lsum, 1.0)) * (0.25 + 0.75 * dust_tw * dust_tw);
          metal_spark += dust_tint * (vec3f(dust_own) + min(dust_glint, vec3f(2.0)) * dust_e4.x * (0.5 + 0.5 * dust_tw)) *
                         dust_shape * dust_e2.x * dust_gate * select(1.0, 0.7, sl == 1u);
        }}
      }})""", sparkleUv, maskExpr, lightCount, lightsExpr),
                                lightsExpr, maskExpr, attMetal, lcExpr, attCoat, reliefExpr, maskExpr2, emisExpr);
    }
  }
  std::string fragmentReturnType = "@location(0) vec4f";
  std::string fragmentReturn = "    return prev;";
  if (usesZTextureDepth) {
    uniformPre +=
        "\n"
        "struct FragmentOutput {\n"
        "    @location(0) color: vec4f,\n"
        "    @builtin(frag_depth) depth: f32,\n"
        "};";

    // ztexDepth is in GX's native distance terms (0=near/1=far, see fogDepthExpr's comment above),
    // but frag_depth must be written in the same host NDC-z convention in.pos.z itself uses -
    // forward matches directly (no change), reversed needs the same 1-x flip. This was backwards
    // the same way fogDepthExpr was (verified by the same derivation, since aurora upstream has no
    // directly equivalent line here to cross-check against - this z-texture-depth-output path
    // appears to be specific to this fork).
    fragmentFn += fmt::format("\n    let fragDepth = {}ztexDepth;", UseReversedZ ? "1.0 - " : "");
    fragmentReturnType = "FragmentOutput";
    fragmentReturn =
        "    var out: FragmentOutput;\n"
        "    out.color = prev;\n"
        "    out.depth = fragDepth;\n"
        "    return out;";
  }

  auto shaderSource = fmt::format(R"""(
fn bswap32(v: u32, le: bool) -> u32 {{
  if (le) {{
    return v;
  }}
  return ((v & 0x000000FFu) << 24u) |
         ((v & 0x0000FF00u) << 8u) |
         ((v & 0x00FF0000u) >> 8u) |
         ((v & 0xFF000000u) >> 24u);
}}

fn bswap16(v: u32, le: bool) -> u32 {{
  return select(((v & 0xFFu) << 8u) | (v >> 8u), v, le);
}}

fn load_u8(p: ptr<storage, array<u32>>, byte_off: u32) -> u32 {{
  let word = p[byte_off / 4u];
  let shift = (byte_off & 3u) * 8u;
  return (word >> shift) & 0xFFu;
}}

fn load_u32_raw(p: ptr<storage, array<u32>>, byte_off: u32) -> u32 {{
  let word_idx = byte_off >> 2u;
  let sub = byte_off & 3u;
  let lo = p[word_idx];
  if (sub == 0u) {{
    return lo;
  }}
  let hi = p[word_idx + 1u];
  let shift = sub * 8u;
  return (lo >> shift) | (hi << (32u - shift));
}}

fn load_u16(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  let word_idx = byte_off >> 2u;
  let sub = byte_off & 3u;
  let word = p[word_idx];
  if (sub <= 2u) {{
    return bswap16(extractBits(word, sub * 8u, 16u), le);
  }}
  let next = p[word_idx + 1u];
  let raw = extractBits(word, 24u, 8u) | (extractBits(next, 0u, 8u) << 8u);
  return bswap16(raw, le);
}}

fn load_u24_raw(p: ptr<storage, array<u32>>, byte_off: u32) -> u32 {{
  let word_idx = byte_off >> 2u;
  let sub = byte_off & 3u;
  let word = p[word_idx];
  // Three bytes at offsets zero or one fit entirely in this word. Do not
  // access the next word: this attribute may end at the binding boundary.
  if (sub <= 1u) {{
    return (word >> (sub * 8u)) & 0x00FFFFFFu;
  }}
  let next = p[word_idx + 1u];
  let shift = sub * 8u;
  return ((word >> shift) | (next << (32u - shift))) & 0x00FFFFFFu;
}}

fn load_u24(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  let raw = load_u24_raw(p, byte_off);
  if (le) {{
    return raw;
  }}
  return ((raw & 0x0000FFu) << 16u) |
         (raw & 0x00FF00u) |
         ((raw & 0xFF0000u) >> 16u);
}}

fn load_u32(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  return bswap32(load_u32_raw(p, byte_off), le);
}}

fn load_f32(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> f32 {{
  return bitcast<f32>(load_u32(p, byte_off, le));
}}

fn raw_fetch_u8_1(p: ptr<storage, array<u32>>, byte_off: u32) -> u32 {{
  return load_u8(p, byte_off);
}}

fn raw_fetch_u8_2(p: ptr<storage, array<u32>>, byte_off: u32) -> vec2u {{
  let word_idx = byte_off >> 2u;
  let sub = byte_off & 3u;
  let word = p[word_idx];
  if (sub <= 2u) {{
    let shift = sub * 8u;
    return vec2u(
      extractBits(word, shift + 0u, 8u),
      extractBits(word, shift + 8u, 8u),
    );
  }}
  let next = p[word_idx + 1u];
  return vec2u(
    extractBits(word, 24u, 8u),
    extractBits(next, 0u, 8u),
  );
}}

fn raw_fetch_u8_3(p: ptr<storage, array<u32>>, byte_off: u32) -> vec3u {{
  let raw = load_u24_raw(p, byte_off);
  return vec3u(
    extractBits(raw, 0u, 8u),
    extractBits(raw, 8u, 8u),
    extractBits(raw, 16u, 8u),
  );
}}

fn raw_fetch_u8_4(p: ptr<storage, array<u32>>, byte_off: u32) -> vec4u {{
  let raw = load_u32_raw(p, byte_off);
  return vec4u(
    extractBits(raw, 0u, 8u),
    extractBits(raw, 8u, 8u),
    extractBits(raw, 16u, 8u),
    extractBits(raw, 24u, 8u),
  );
}}

fn raw_fetch_u16_1(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  return load_u16(p, byte_off, le);
}}

fn raw_fetch_u16_2(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec2u {{
  return vec2u(
    load_u16(p, byte_off + 0u, le),
    load_u16(p, byte_off + 2u, le),
  );
}}

fn raw_fetch_u16_3(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec3u {{
  return vec3u(
    load_u16(p, byte_off + 0u, le),
    load_u16(p, byte_off + 2u, le),
    load_u16(p, byte_off + 4u, le),
  );
}}

fn raw_fetch_u16_4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4u {{
  return vec4u(
    load_u16(p, byte_off + 0u, le),
    load_u16(p, byte_off + 2u, le),
    load_u16(p, byte_off + 4u, le),
    load_u16(p, byte_off + 6u, le),
  );
}}

fn raw_fetch_f32_1(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> f32 {{
  return load_f32(p, byte_off, le);
}}

fn raw_fetch_f32_2(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec2f {{
  return vec2f(
    load_f32(p, byte_off + 0u, le),
    load_f32(p, byte_off + 4u, le),
  );
}}

fn raw_fetch_f32_3(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec3f {{
  return vec3f(
    load_f32(p, byte_off + 0u, le),
    load_f32(p, byte_off + 4u, le),
    load_f32(p, byte_off + 8u, le),
  );
}}

fn raw_fetch_f32_4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  return vec4f(
    load_f32(p, byte_off + 0u, le),
    load_f32(p, byte_off + 4u, le),
    load_f32(p, byte_off + 8u, le),
    load_f32(p, byte_off + 12u, le),
  );
}}

fn fetch_u8_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = raw_fetch_u8_1(p, byte_off);
  return f32(v) / f32(1u << frac);
}}

fn fetch_s8_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = (bitcast<i32>(raw_fetch_u8_1(p, byte_off)) << 24) >> 24;
  return f32(v) / f32(1u << frac);
}}

fn fetch_u8_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = raw_fetch_u8_2(p, byte_off);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_s8_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = (bitcast<vec2i>(raw_fetch_u8_2(p, byte_off)) << vec2u(24u)) >> vec2u(24u);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_u8_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = raw_fetch_u8_3(p, byte_off);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_s8_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = (bitcast<vec3i>(raw_fetch_u8_3(p, byte_off)) << vec3u(24u)) >> vec3u(24u);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_u8_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_4(p, byte_off);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_s8_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = (bitcast<vec4i>(raw_fetch_u8_4(p, byte_off)) << vec4u(24u)) >> vec4u(24u);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_u16_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = raw_fetch_u16_1(p, byte_off, le);
  return f32(v) / f32(1u << frac);
}}

fn fetch_s16_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = bitcast<i32>(raw_fetch_u16_1(p, byte_off, le) << 16u) >> 16;
  return f32(v) / f32(1u << frac);
}}

fn fetch_u16_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = raw_fetch_u16_2(p, byte_off, le);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_s16_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = (bitcast<vec2i>(raw_fetch_u16_2(p, byte_off, le)) << vec2u(16u)) >> vec2u(16u);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_u16_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = raw_fetch_u16_3(p, byte_off, le);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_s16_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = (bitcast<vec3i>(raw_fetch_u16_3(p, byte_off, le)) << vec3u(16u)) >> vec3u(16u);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_u16_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u16_4(p, byte_off, le);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_s16_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = (bitcast<vec4i>(raw_fetch_u16_4(p, byte_off, le)) << vec4u(16u)) >> vec4u(16u);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_f32_1(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> f32 {{
  return raw_fetch_f32_1(p, byte_off, le);
}}

fn fetch_f32_2(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec2f {{
  return raw_fetch_f32_2(p, byte_off, le);
}}

fn fetch_f32_3(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec3f {{
  return raw_fetch_f32_3(p, byte_off, le);
}}

fn fetch_f32_4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  return raw_fetch_f32_4(p, byte_off, le);
}}

fn fetch_rgb565(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = load_u16(p, byte_off, le);
  return vec4f(
    f32((v >> 11u) & 0x1Fu) / f32(0x1Fu),
    f32((v >>  5u) & 0x3Fu) / f32(0x3Fu),
    f32((v >>  0u) & 0x1Fu) / f32(0x1Fu),
    1.0,
  );
}}

fn fetch_rgb8(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_3(p, byte_off);
  return vec4f(f32(v.x), f32(v.y), f32(v.z), 255.0) / 255.0;
}}

fn fetch_rgbx8(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_4(p, byte_off);
  return vec4f(f32(v.x), f32(v.y), f32(v.z), 255.0) / 255.0;
}}

fn fetch_rgba4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = load_u16(p, byte_off, le);
  return vec4f(
    f32((v >> 12u) & 0x0Fu) / f32(0x0Fu),
    f32((v >>  8u) & 0x0Fu) / f32(0x0Fu),
    f32((v >>  4u) & 0x0Fu) / f32(0x0Fu),
    f32((v >>  0u) & 0x0Fu) / f32(0x0Fu),
  );
}}

fn fetch_rgba6(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = load_u24(p, byte_off, le);
  return vec4f(
    f32((v >> 18u) & 0x3Fu) / f32(0x3Fu),
    f32((v >> 12u) & 0x3Fu) / f32(0x3Fu),
    f32((v >>  6u) & 0x3Fu) / f32(0x3Fu),
    f32((v >>  0u) & 0x3Fu) / f32(0x3Fu),
  );
}}

fn fetch_rgba8(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_4(p, byte_off);
  return vec4f(v) / 255.0;
}}

fn tev_overflow_f32(in: f32) -> f32 {{
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}}

fn tev_overflow_vec3f(in: vec3f) -> vec3f {{
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}}

fn tev_overflow_vec4f(in: vec4f) -> vec4f {{
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}}

fn tev_s24_fixed_vec2i(in: vec2i) -> vec2i {{
  return (in << vec2u(8u)) >> vec2u(8u);
}}

fn tev_byte_f32(in: f32) -> i32 {{
  return i32(round(in * 255.0));
}}

fn tev_byte_vec3f(in: vec3f) -> vec3i {{
  return vec3i(round(in * 255.0));
}}

fn tev_regular_i32(a: i32, b: i32, c: i32, d: i32, bias: i32, scale: u32, is_sub: bool) -> i32 {{
  var d_part = d + bias;
  var lerp_part = (a << 8u) + (b - a) * (c + (c >> 7u));
  if (scale == 1u) {{
    d_part = d_part << 1u;
    lerp_part = lerp_part << 1u;
  }}
  if (scale == 2u) {{
    d_part = d_part << 2u;
    lerp_part = lerp_part << 2u;
  }}
  if (scale != 3u) {{
    lerp_part += select(128, 127, is_sub);
  }}
  let lerp = lerp_part >> 8u;
  var result = select(d_part + lerp, d_part - lerp, is_sub);
  if (scale == 3u) {{
    result = result >> 1u;
  }}
  return result;
}}

fn tev_regular_f32(a: f32, b: f32, c: f32, d: f32, bias: i32, scale: u32, is_sub: bool) -> f32 {{
  return f32(tev_regular_i32(tev_byte_f32(a), tev_byte_f32(b), tev_byte_f32(c), tev_byte_f32(d), bias, scale, is_sub)) /
         255.0;
}}

fn tev_regular_vec3f(a: vec3f, b: vec3f, c: vec3f, d: vec3f, bias: i32, scale: u32, is_sub: bool) -> vec3f {{
  let ai = tev_byte_vec3f(a);
  let bi = tev_byte_vec3f(b);
  let ci = tev_byte_vec3f(c);
  let di = tev_byte_vec3f(d);
  return vec3f(
    f32(tev_regular_i32(ai.x, bi.x, ci.x, di.x, bias, scale, is_sub)) / 255.0,
    f32(tev_regular_i32(ai.y, bi.y, ci.y, di.y, bias, scale, is_sub)) / 255.0,
    f32(tev_regular_i32(ai.z, bi.z, ci.z, di.z, bias, scale, is_sub)) / 255.0,
  );
}}

{8}

struct Uniform {{
    vtx_start: u32,
    current_pnmtx: u32,
    render_viewport_size: vec2f,
    logical_viewport_size: vec2f,
    pad: vec2u,
    array_start: array<u32, 12>,{0}
}};
@group(0) @binding(0)
var<storage, read> vbuf: array<u32>;
@group(0) @binding(1)
var<storage, read> abuf: array<u32>;
@group(1) @binding(0)
var<uniform> ubuf: Uniform;{1}

struct VertexOutput {{
    // Invariant: Prime redraws geometry it has just drawn (same vertices and
    // matrix, different TEV/lighting) with an LEQUAL test and relies on equal
    // depth; without it the HLSL compiler may contract the transform differently
    // per shader and the second draw fails the test by an ulp.
    @invariant @builtin(position) pos: vec4f,{2}
}};

@vertex
fn vs_main(
    @builtin(vertex_index) vidx: u32{3}
) -> VertexOutput {{
    var out: VertexOutput;{7}{4}
    return out;
}}

@fragment
fn fs_main(in: VertexOutput) -> {9} {{{6}{5}
{10}
}}
)""",
                                        uniBufAttrs, texBindings, vtxOutAttrs, vtxInAttrs, vtxXfrAttrs, fragmentFn,
                                        fragmentFnPre, vtxXfrAttrsPre, uniformPre, fragmentReturnType,
                                        fragmentReturn);
  // Opt-in observation of coverage from the actual graphics draw. Atomic
  // bounds avoid races between fragments and do not alter the returned color.
  if (hash == 0x5647F2E965F39515ull && std::getenv("WII_COMPILED_GPU_TRACE_RASTER_COVERAGE")) {
    shaderSource = "@group(0) @binding(2) var<storage, read_write> diagnostic_raster: array<atomic<u32>>;\n" + shaderSource;
    const auto end = shaderSource.find("    return prev;", shaderSource.find("fn fs_main("));
    if (end != std::string::npos)
      shaderSource.insert(end,
        "    let diagnostic_slot = (ubuf.vtx_start / 4u) * 8u;\n"
        "    atomicMax(&diagnostic_raster[diagnostic_slot], u32(in.pos.x));\n"
        "    atomicMax(&diagnostic_raster[diagnostic_slot+1u], u32(in.pos.y));\n"
        "    atomicMax(&diagnostic_raster[diagnostic_slot+2u], u32(10000.0-in.pos.x));\n"
        "    atomicMax(&diagnostic_raster[diagnostic_slot+3u], u32(10000.0-in.pos.y));\n"
        "    atomicMax(&diagnostic_raster[diagnostic_slot+4u], bitcast<u32>(in.tex0_uv.x));\n"
        "    atomicMax(&diagnostic_raster[diagnostic_slot+5u], bitcast<u32>(in.tex0_uv.y));\n"
        "    atomicMax(&diagnostic_raster[diagnostic_slot+6u], bitcast<u32>(1.0-in.tex0_uv.x));\n"
        "    atomicMax(&diagnostic_raster[diagnostic_slot+7u], bitcast<u32>(1.0-in.tex0_uv.y));\n");
  }
  if (const char* path = std::getenv("WII_COMPILED_GX_SHADER_DUMP")) {
    if ((config.vtxStride == 24 || config.vtxStride == 20) && config.tevStageCount == 1) {
      const auto filename = fmt::format("{}_{:016X}.wgsl", path, hash);
      if (auto* file = std::fopen(filename.c_str(), "wb")) {
        std::fwrite(shaderSource.data(), 1, shaderSource.size(), file);
        std::fclose(file);
      }
    }
  }
  return shaderSource;
}

std::string build_shader_source(const ShaderConfig& config) noexcept { return build_shader_source_impl(config, nullptr); }

std::string build_uber_shader_source(const UberConfig& config) noexcept {
  return build_shader_source_impl(config.pipeline.shaderConfig, &config);
}

wgpu::ShaderModule build_uber_shader(const UberConfig& config) noexcept {
  ZoneScoped;
  const auto source = build_uber_shader_source(config);
  wgpu::ShaderSourceWGSL wgslDescriptor{};
  wgslDescriptor.code = source.c_str();
  const auto label = fmt::format("GX Uber Shader {:x}", xxh3_hash(config));
  const auto shaderDescriptor = wgpu::ShaderModuleDescriptor{
      .nextInChain = &wgslDescriptor,
      .label = label.c_str(),
  };
  // A WGSL error in an ubershader must not reach Aurora's fatal uncaptured-error callback: the material
  // simply keeps waiting for its specialised pipeline. The scope captures validation errors of this module.
  webgpu::g_device.PushErrorScope(wgpu::ErrorFilter::Validation);
  auto module = webgpu::g_device.CreateShaderModule(&shaderDescriptor);
  auto message = std::make_shared<std::string>();
  auto future = webgpu::g_device.PopErrorScope(
      wgpu::CallbackMode::WaitAnyOnly, [message](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView text) {
        if (status == wgpu::PopErrorScopeStatus::Success && type != wgpu::ErrorType::NoError) {
          const std::string_view detail{text};
          *message = detail.empty() ? "unknown validation error"s : std::string(detail);
        }
      });
  webgpu::g_instance.WaitAny(future, 10'000'000'000ull);
  if (!message->empty()) {
    // One log line per distinct message (the line:column text differs per key, so strip it).
    static std::mutex reportMutex;
    static std::set<std::string> seen;
    std::string signature = *message;
    for (auto& c : signature) if (c >= '0' && c <= '9') c = '#';
    bool first = false;
    {
      std::lock_guard lock{reportMutex};
      first = seen.size() < 40 && seen.insert(signature).second;
    }
    static std::atomic<int> reported{0};
    if (first) {
      reported.fetch_add(1);
      Log.error("[uber] ubershader {} rejected by the shader compiler; the material waits for its specialised pipeline: {}",
                label, *message);
      if (const char* dump = std::getenv("WII_COMPILED_UBER_DUMP_FAILED")) {
        if (auto* file = std::fopen(fmt::format("{}_{}.wgsl", dump, reported.load()).c_str(), "wb")) {
          std::fwrite(source.data(), 1, source.size(), file);
          std::fclose(file);
        }
      }
    }
    return {};
  }
  return module;
}

wgpu::ShaderModule build_shader(const ShaderConfig& config) noexcept {
  ZoneScoped;
  const auto hash = xxh3_hash(config);
  const auto shaderSource = build_shader_source(config);
  wgpu::ShaderSourceWGSL wgslDescriptor{};
  wgslDescriptor.code = shaderSource.c_str();
  const auto label = fmt::format("GX Shader {:x}", hash);
  const auto shaderDescriptor = wgpu::ShaderModuleDescriptor{
      .nextInChain = &wgslDescriptor,
      .label = label.c_str(),
  };
  return webgpu::g_device.CreateShaderModule(&shaderDescriptor);
}
} // namespace aurora::gx
