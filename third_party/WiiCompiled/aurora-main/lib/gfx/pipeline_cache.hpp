#pragma once

#include <string>
#include <chrono>

#include "common.hpp"

#include <functional>

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::gfx {

using NewPipelineCallback = std::function<wgpu::RenderPipeline()>;

void initialize_pipeline_cache();
void shutdown_pipeline_cache();
void begin_pipeline_frame();
void end_pipeline_frame();
void set_skip_unready_pipelines(bool enabled) noexcept;
bool skip_unready_pipelines() noexcept;
// True from the boot prewarm's start until its queue drains.
bool pipeline_prewarm_active() noexcept;
uint32_t queued_pipeline_count() noexcept;

// Draw accounting for bug reports (discussion #2: a session that stayed dark after a cold shader
// cache left nothing in the log to tell skipped draws from fallback ones). All callable from the
// encoder threads; the summary line is logged at most every 10 s and only while something happens.
// Start of each encoded pass: its target size, named by the skip lines.
void note_encoding_pass(uint32_t width, uint32_t height) noexcept;
// A draw that was not drawn at all because no pipeline of it was ready. `persistentPass`: the pass
// ends in an EFB copy or an offscreen bake, so the draw may never be issued again.
void note_draw_skipped(PipelineRef ref, bool persistentPass) noexcept;
// A draw made with its vertex-lit fallback because the per-pixel variant was still compiling.
void note_fallback_draw() noexcept;
// Multi-line text for the F12 report (aurora_describe_render_state).
std::string describe_render_state();

template <typename Config>
PipelineRef find_pipeline(ShaderType type, const Config& config, NewPipelineCallback&& cb);

bool wait_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);
bool try_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);
bool wait_pipeline_until(PipelineRef ref, wgpu::RenderPipeline& pipeline,
                         std::chrono::steady_clock::time_point deadline);
// Waits indefinitely for a draw about to be committed to a persistent texture with no re-issue
// path. Separate from wait_pipeline so skip-unready mode never becomes blocking for other passes.
bool wait_pipeline_for_persistent_pass(PipelineRef ref, wgpu::RenderPipeline& pipeline);

} // namespace aurora::gfx
