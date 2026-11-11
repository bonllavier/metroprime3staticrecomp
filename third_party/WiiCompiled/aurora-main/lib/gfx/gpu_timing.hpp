#pragma once
// GPU time per frame by category, from timestamp queries around render passes. Diagnostic only:
// on with WII_COMPILED_GPU_TIMING=1 (the device then requests the timestamp-query feature) and
// when the adapter supports it; otherwise every call is a no-op and nothing changes. Every 5 s the
// log gets "[gpu-time]" with the average and worst frame per category, in milliseconds.
#include <webgpu/webgpu_cpp.h>

#include <cstdint>

namespace aurora::gfx::gpu_timing {

enum class Category : uint8_t {
  Game,     // the game's own EFB and offscreen passes
  Copy,     // EFB copy conversions (tex_copy_conv)
  Post,     // post-process passes (SSAO, reflections, light shafts, depth merges, ...)
  Count,
};

// True when the environment asks for it (device creation requests the feature).
bool requested() noexcept;
// Timestamp writes for a render pass about to begin, or nullptr (off, or this frame is full).
// The pointer stays valid until the next call.
const wgpu::PassTimestampWrites* pass_writes(Category category) noexcept;
// At the end of a frame's encoding: resolves this frame's queries into a readback buffer.
void encode_resolve(const wgpu::CommandEncoder& encoder) noexcept;
// After the submit: maps resolved frames; their times are added from the map callback.
void after_submit() noexcept;

}  // namespace aurora::gfx::gpu_timing
