#pragma once

#include "gx.hpp"
#include "render_policy.hpp"

#include "aurora/gfx.h"

#include <atomic>

// Frame interpolation: generates intermediate presentation frames between two consecutive 60 Hz
// guest frames by re-staging each perspective draw's uniform block with interpolated transforms.
namespace aurora::gx {
constexpr uint32_t MaxInterpolatedFrames = policy::kMaxCadenceSamples;

// Identifies a draw across frames. `combined` includes the geometry signature and is exact, while
// `pipeline`/`texture` is the material-only fallback for meshes whose vertex data changes.
struct FrameInterpolationDrawIdentity {
  HashType combined = 0;
  HashType pipeline = 0;
  HashType texture = 0;
  // Hash of the per-vertex PNMTXIDX stream. Matrix values animate, but this topology decides which
  // absolute palette slot each vertex reads, so it must match across a pair.
  HashType matrixTopology = 0;
  // Vertex data and matrix topology only: pairs a mesh whose material changed between two frames (a
  // shot lighting a wall), which the identities above lose.
  HashType geometry = 0;
};

// Where the transforms live inside a draw's staged uniform block. Offsets are
// relative to the start of the mapped range.
struct InterpolatedUniformLayout {
  const uint8_t* sourceUniformData = nullptr;
  size_t uniformSize = 0;
  size_t projectionOffset = 0;
  size_t positionOffset = 0;
  size_t normalOffset = 0;
  // Slot the live matrix occupies; a compacted position region holds it at 0.
  size_t currentMatrix = 0;
  bool indexedMatrices = false;
  // Texture matrices (3x4) the draw reads. Environment and projected maps build them from the camera, so
  // an inserted image interpolates them with the geometry when they change smoothly.
  static constexpr size_t kMaxTextureMatrices = 16;
  uint32_t textureMatrixCount = 0;
  std::array<uint32_t, kMaxTextureMatrices> textureMatrixOffsets{};
};

// Whether a texture matrix moved smoothly between two frames, so that an inserted image may take a value
// between them: a turn (the camera's rotation in an environment map) or a slow scroll. A jump (an animation
// stepping to another cell, a wrap) keeps the frame's own value.
bool texture_matrix_moves_smoothly(const Mat3x4<float>& previous, const Mat3x4<float>& current) noexcept;

namespace detail {
// Defined in frame_interpolation.cpp, exposed so the early-outs below stay inline: the shipped
// build compiles shards without LTO, so a cross-TU call would land on every draw in the frame.
extern std::atomic_uint32_t g_frameInterpolationFps;
// An F4 capture of inserted images is running (test builds): the rate controller holds the rate, since the
// readbacks slow the encoder and it would otherwise drop the very images being captured.
extern std::atomic_bool g_holdInterpolationRate;
} // namespace detail

// 0 when interpolation is disabled; otherwise the configured target in images per second, or
// AURORA_FRAME_INTERPOLATION_UNCAPPED.
inline uint32_t frame_interpolation_fps() noexcept {
  return detail::g_frameInterpolationFps.load(std::memory_order_acquire);
}
inline bool frame_interpolation_active() noexcept { return frame_interpolation_fps() != 0; }

void set_frame_interpolation_fps(uint32_t targetFps) noexcept;

// Feedback for the adaptive slot controller: whether the producer met its
// retrace boundary for the frame that just presented.
void report_producer_paced(bool paced) noexcept;
// Feedback for the same controller: the shares of a frame period (1 = all of it) that encoding this
// group and presenting the previous one took.
void report_frame_load(double encodeLoad, double presentLoad) noexcept;

void begin_frame_interpolation() noexcept;
// A display copy was recorded: the game frame being recorded ends here and the next one starts with
// its own cadence. Matching and slot weights follow game frames, which straddle sealed batches.
void note_frame_interpolation_boundary() noexcept;
// Display copies recorded since the batch began.
uint32_t frame_interpolation_boundaries_in_batch() noexcept;
// Matches and fills every staged copy of the batch, and latches what the seal presents: the cadence
// of the last game frame whose display copy the batch holds (none without a copy).
void finalize_frame_interpolation() noexcept;
// Seal outcome for the diagnostics: the batch began at a game frame start, moved its draws after the
// display copy to the next batch, and replayed inserted images.
void note_frame_interpolation_seal(bool complete, bool carried, uint32_t carriedDraws, bool replayed) noexcept;
// Fills the observability snapshot behind aurora_get_frame_interpolation_diagnostics.
void get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics& diagnostics) noexcept;
bool has_interpolated_frame() noexcept;
uint32_t interpolated_frame_count() noexcept;
// Inserted images latched for the frame being recorded, whatever the seal later decides.
uint32_t active_interpolation_samples() noexcept;
// Blend weight of an inserted image of the frame being sealed, in (0, 1).
float interpolated_frame_weight(uint32_t sample) noexcept;
// Candidate pairs the matcher has measured since start (frame worker); bounds its cost in tests.
uint64_t frame_interpolation_pair_evaluations() noexcept;
// False when no display tick lands on the frame itself, so only its inserted images are shown.
bool interpolated_frame_presents_native() noexcept;
// See policy::cadence_bias. Latched with the frame's slots.
double interpolated_frame_bias() noexcept;
// The sealed frame's inserted images come after it (weights above 1, low-latency mode): the native
// image is presented first, as soon as it is encoded.
bool interpolated_frame_extrapolates() noexcept;
void set_frame_interpolation_low_latency(bool enabled) noexcept;
bool frame_interpolation_low_latency() noexcept;
// The PC keeps the image rate with room to spare (policy::step_align_gate): display-refresh alignment applies.
bool frame_interpolation_has_headroom() noexcept;
void mark_frame_interpolation_replay_unsafe() noexcept;
// Drops the interpolation tasks staged into the currently mapped uniform range. Required of
// anything that unmaps or rotates that range before the frame is finalized.
void drop_pending_frame_interpolation_uniforms() noexcept;
bool frame_interpolation_replay_safe() noexcept;

// Records one perspective draw and maps its intermediate uniform copies, returning the mapped
// range per slot (empty when the draw has no counterpart). Called by build_uniform.
std::array<gfx::Range, MaxInterpolatedFrames> record_interpolation_draw(
    const FrameInterpolationDrawIdentity& identity, const Mat4x4<float>& projection,
    uint16_t usedPnMtxMask, const InterpolatedUniformLayout& uniformLayout) noexcept;

// Folds a merged draw back into the snapshot of the draw it joined. aurora renders merged
// primitives through the first one's uniform block, so without this the merged-in bones tear.
void extend_interpolation_draw(uint16_t usedPnMtxMask) noexcept;

bool interpolate_transform(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                           float weight, Mat3x4<float>& output) noexcept;
bool interpolate_transform_midpoint(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                                    Mat3x4<float>& output) noexcept;
// Matrix palettes are already-composed skinning transforms, so interpolating their coefficients
// keeps shared boundaries intact. Ordinary one-matrix draws keep the rigid TRS path above.
bool interpolate_indexed_transform(const Mat3x4<float>& previous,
                                   const Mat3x4<float>& current, float weight,
                                   Mat3x4<float>& output) noexcept;
float transform_match_distance_squared(const Mat3x4<float>& previous,
                                       const Mat3x4<float>& current) noexcept;
} // namespace aurora::gx
