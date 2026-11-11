#pragma once

#include "common.hpp"

#include <memory>
#include <vector>

// GPU decode of GX_TF_CMPR static textures. The raw guest blocks (8x fewer bytes
// than RGBA8) are uploaded and a compute shader writes every mip level with the
// same arithmetic as BuildRGBA8FromCMPR: 5/6-bit expansion, GX's 3/8-5/8 blend
// and the half-blend colour with alpha 0 in three-colour blocks.
namespace aurora::gfx::tex_cmpr_decode {

struct MipDispatch {
  wgpu::BindGroup bindGroup;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct Request {
  TextureHandle dst;
  std::vector<MipDispatch> mips;
  // Verification mode only: CPU-decoded mip 0, compared after readback.
  std::shared_ptr<std::vector<uint8_t>> reference;
};

void initialize();
void shutdown();
bool enabled() noexcept;
void set_enabled(bool enabled) noexcept;

// GX thread: records the decode of `data` into `dst` (created with StorageBinding)
// before the current render pass. Returns false when the caller must decode on
// the CPU instead (no pass, short data, or the feature is off).
bool queue(const TextureHandle& dst, ArrayRef<uint8_t> data);
void run(const wgpu::CommandEncoder& cmd, const Request& req);
// Frame worker, after submit: compares verification readbacks (no-op otherwise).
void complete_verifications();

} // namespace aurora::gfx::tex_cmpr_decode
