#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace aurora::gfx::tex_copy_conv::detail {

constexpr bool preserve_a8_right_edge(uint32_t format,float x,float y,float width,float height,
                                     float framebufferWidth,float framebufferHeight) noexcept {
  return format==0x27 && width>0 && height>0 && x>=0 && x<framebufferWidth && y>=0 &&
         y+height<=framebufferHeight && x+width>framebufferWidth;
}

// GX_TF_Z16 is the SDK's RG8 channel copy of depth (GXSetTexCopyDst writes nibble 0xB for it;
// libogc GX_SetTexCopyDst, and RM3E01's scan visor copy). Sampled as IA8 that is
// I = Z[23:16] and A = Z[15:8], as in upstream aurora; the low byte is not stored.
constexpr std::array<std::uint8_t, 4> z16_rg8_as_ia8_rgba(std::uint8_t high, std::uint8_t middle) noexcept {
  return {high, high, high, middle};
}

inline constexpr std::string_view Z16FragmentShader = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let depth_bytes = sample_depth_copy(in.uv);
    let i = f32(depth_bytes.r) / 255.0;
    return vec4f(i, i, i, f32(depth_bytes.g) / 255.0);
}
)";

// Depth copies read the EFB depth buffer, which is multisampled while MSAA is on. textureLoad's third
// argument is the mip level of texture_depth_2d and the sample index of the multisampled type, so the
// same shader body reads sample 0: one real surface. Averaging across an edge would give a distance no
// surface is at, and RM3E01's scan visor picks its target from these bytes.
inline constexpr std::string_view DepthSourceBinding = "@group(0) @binding(0) var src: texture_depth_2d;\n";
inline constexpr std::string_view MultisampledDepthSourceBinding =
    "@group(0) @binding(0) var src: texture_depth_multisampled_2d;\n";

} // namespace aurora::gfx::tex_copy_conv::detail
