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

// GX_TF_Z16 selects the depth RA8 copy format: the encoder writes alpha (255) then the high-Z
// byte, so sampling as IA8 gives high-Z as intensity. Middle and low depth bytes are not stored.
constexpr std::array<std::uint8_t, 4> z16_ra8_as_ia8_rgba(std::uint8_t high) noexcept {
  return {high, high, high, 0xff};
}

inline constexpr std::string_view Z16FragmentShader = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let depth_bytes = sample_depth_copy(in.uv);
    let i = f32(depth_bytes.r) / 255.0;
    return vec4f(i, i, i, 1.0);
}
)";

} // namespace aurora::gfx::tex_copy_conv::detail
