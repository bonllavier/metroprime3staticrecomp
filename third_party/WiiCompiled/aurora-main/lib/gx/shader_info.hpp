#pragma once

#include "gx.hpp"
// MaxInterpolatedFrames / FrameInterpolationDrawIdentity, and the interpolation API that build_uniform feeds.
#include "frame_interpolation.hpp"

namespace aurora::gx {
struct UniformRanges {
  gfx::Range current;
  std::array<gfx::Range, MaxInterpolatedFrames> interpolated;
};

ShaderInfo build_shader_info(const ShaderConfig& config) noexcept;
struct UberConfig;
struct UberData;
// Layout of the ubershader's uniform buffer; size_ok is false if it does not fit MaxUniformSize.
ShaderInfo build_uber_shader_info(const UberConfig& config, bool* sizeOk = nullptr) noexcept;
Light prepare_shader_light(Light light) noexcept;
UniformRanges build_uniform(const ShaderInfo& info, uint32_t vtxStart, const BindGroupRanges& ranges,
                            const FrameInterpolationDrawIdentity& drawIdentity, bool perspective,
                            uint16_t usedPnMtxMask = 1, const UberData* uber = nullptr) noexcept;
u8 color_channel(GXChannelID id) noexcept;
}; // namespace aurora::gx
