#include "post_process.hpp"
#include "../diagnostic_env.hpp"
#include "common.hpp"
#include "draw_dump.hpp"
#include "staging_map.hpp"
#include "../gx/shader_info.hpp"

#include "clear.hpp"
#include "depth_peek.hpp"
#include "efb_ram_copy.hpp"
#include "gpu_timing.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../gx/pipeline.hpp"
#include "../gx/render_policy.hpp"
#include "pipeline_cache.hpp"
#include "tex_copy_conv.hpp"
#include "tex_copy_format_contract.hpp"
#include "tex_palette_conv.hpp"
#include "tex_cmpr_decode.hpp"
#include "texture_replacement.hpp"
#include "texture_upscale.hpp"
#include "texture.hpp"
#include "../window.hpp"

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>

#include <absl/container/flat_hash_map.h>
#include <magic_enum.hpp>

#include "tracy/Tracy.hpp"

// Diagnostic environment switches are fixed for the lifetime of this process.
static bool CachedWiiCompiledTracePrimeGx() {
  static const bool enabled = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_TRACE_PRIME_GX") != nullptr;
  return enabled;
}


namespace aurora::gx {
void enforce_copy_texture_budget() noexcept;
}

namespace aurora::gfx {
static Module Log("aurora::gfx");
static TextureHandle g_diagnosticResolveSnapshot;
TextureHandle diagnostic_resolve_snapshot() { return g_diagnosticResolveSnapshot; }


using webgpu::g_device;
using webgpu::g_instance;
using webgpu::g_queue;

struct DebugFrameData {
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> groups;
  std::vector<std::string> markers;
#endif
};
DebugFrameData g_debugFrame;

constexpr uint64_t StagingBufferSize = UniformBufferSize + VertexBufferSize + IndexBufferSize + StorageBufferSize +
                                       (UseTextureBuffer ? TextureUploadSize : 0);

struct ShaderDrawCommand {
  ShaderType type;
  union {
    clear::DrawData clear;
    gx::DrawData gx;
  };
};
enum class CommandType {
  SetViewport,
  SetScissor,
  Draw,
  DebugMarker,
};
struct Command {
  CommandType type;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> debugGroupStack;
#endif
  union Data {
    Viewport setViewport;
    ClipRect setScissor;
    ShaderDrawCommand draw;
    size_t debugMarkerIndex;
  } data;
};
} // namespace aurora::gfx

void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation,
                                  AuroraGuestWriteNotifyCallback notify) {
  aurora::g_guestWriteGenerationHook = generation;
  aurora::g_guestWriteNotifyHook = notify;
}

namespace aurora {
// For types that we can't ensure are safe to hash with has_unique_object_representations,
// we create specialized methods to handle them. Note that these are highly dependent on
// the structure definition, which could easily change with Dawn updates.
template <>
inline HashType xxh3_hash(const WGPUBindGroupDescriptor& input, HashType seed) {
  constexpr auto offset = offsetof(WGPUBindGroupDescriptor, layout); // skip nextInChain, label
  const auto hash = xxh3_hash_s(reinterpret_cast<const u8*>(&input) + offset,
                                sizeof(WGPUBindGroupDescriptor) - offset - sizeof(void*) /* skip entries */, seed);
  const size_t entryBytes = sizeof(WGPUBindGroupEntry) * input.entryCount;
  // The entries are hashed unseeded so they stay off XXH3's seeded long path;
  // the descriptor head still carries the caller's seed.
  return hash_combine(hash, xxh3_hash_s(input.entries, entryBytes));
}
template <>
inline HashType xxh3_hash(const wgpu::SamplerDescriptor& input, HashType seed) {
  constexpr auto offset = offsetof(wgpu::SamplerDescriptor, addressModeU); // skip nextInChain, label
  return xxh3_hash_s(reinterpret_cast<const u8*>(&input) + offset,
                     sizeof(wgpu::SamplerDescriptor) - offset - 2 /* skip padding */, seed);
}
} // namespace aurora

namespace aurora::gfx {
namespace {
struct CachedBindGroup {
  wgpu::BindGroup bindGroup;
  uint32_t lastUsedFrame = 0;
};

constexpr uint32_t BindGroupCacheRetainFrames = 32;
constexpr uint32_t BindGroupCacheSweepPeriod = 16;
} // namespace

static absl::flat_hash_map<BindGroupRef, CachedBindGroup> g_cachedBindGroups;
// Bind groups dropped from the cache by clear_caches() while a sealed frame's recorded draws
// still reference them. Ownership moves here and is released at the next seal.
static std::vector<CachedBindGroup> g_retiredBindGroups;
static absl::flat_hash_map<SamplerRef, wgpu::Sampler> g_cachedSamplers;

static ByteBuffer g_verts;
static ByteBuffer g_uniforms;
static ByteBuffer g_indices;
static ByteBuffer g_storage;
static ByteBuffer g_textureUpload;
wgpu::Buffer g_vertexBuffer;
wgpu::Buffer g_uniformBuffer;
wgpu::Buffer g_indexBuffer;
wgpu::Buffer g_storageBuffer;
static wgpu::Buffer g_diagnosticRasterCoverage;
wgpu::Buffer diagnostic_raster_coverage() { return g_diagnosticRasterCoverage; }
constexpr size_t FrameSlotCount = 3;
static std::array<wgpu::Buffer, FrameSlotCount> g_stagingBuffers;
static size_t currentStagingBuffer = 0;
static StagingMapState s_mappingState;
static wgpu::Limits g_cachedLimits;
// Advanced once per logical frame in the seal prologue, under the renderer GPU mutex and with the
// producer blocked, so every later reader sees a value that no longer moves.
static uint32_t g_frameIndex = UINT32_MAX;
wgpu::BindGroupLayout g_staticBindGroupLayout;
wgpu::BindGroup g_staticBindGroup;
wgpu::BindGroupLayout g_uniformBindGroupLayout;
wgpu::BindGroup g_uniformBindGroup;

// Frame interpolation renders the draws a sealed batch recorded after its display copy (the head of
// the next game frame) together with the next batch, so the GPU copy of one batch has to survive
// the next batch's upload. Two sets alternate per staging upload once the second one exists; the
// g_* buffer globals above always name the set being encoded, which only the encoding thread moves.
struct GpuBufferSet {
  wgpu::Buffer vertex;
  wgpu::Buffer uniform;
  wgpu::Buffer index;
  wgpu::Buffer storage;
  wgpu::BindGroup staticBindGroup;
  wgpu::BindGroup uniformBindGroup;
};
static constexpr uint8_t kUntaggedBufferSet = 0xFF;
static std::array<GpuBufferSet, 2> g_bufferSets;
static bool g_alternateBufferSets = false;
static uint8_t g_boundBufferSet = 0;
static uint8_t g_nextWriteBufferSet = 0;
static uint8_t g_lastWrittenBufferSet = 0;

static void bind_buffer_set(uint8_t set) noexcept {
  if (set == kUntaggedBufferSet || set == g_boundBufferSet || set >= g_bufferSets.size() ||
      (set != 0 && !g_alternateBufferSets)) {
    return;
  }
  const auto& buffers = g_bufferSets[set];
  g_vertexBuffer = buffers.vertex;
  g_uniformBuffer = buffers.uniform;
  g_indexBuffer = buffers.index;
  g_storageBuffer = buffers.storage;
  g_staticBindGroup = buffers.staticBindGroup;
  g_uniformBindGroup = buffers.uniformBindGroup;
  g_boundBufferSet = set;
}

// for imgui debug
AuroraStats g_stats{};
uint32_t g_drawCallCount = 0;
uint32_t g_mergedDrawCallCount = 0;

using CommandList = std::vector<Command>;
struct RenderPass {
  wgpu::TextureView colorView;
  wgpu::TextureView resolveView; // MSAA resolve target; null if msaaSamples == 1
  wgpu::TextureView depthView;
  wgpu::Texture depthTexture;   // texture behind depthView (EFB or offscreen), for SSAO accumulation
  wgpu::Extent3D depthSize{};
  wgpu::Texture copySourceTexture;
  wgpu::TextureView copySourceView;
  wgpu::TextureView copySourceDepthView;
  wgpu::Extent3D targetSize;
  uint32_t msaaSamples = 1;

  TextureHandle resolveTarget;
  TextureHandle resolveSourceSnapshot;
  GXTexFmt resolveFormat = GX_TF_RGBA8;
  ClipRect resolveRect;
  ClipRect resolveSnapshotRect;
  Vec4<float> resolveSourceRect;
  Range resolveUniformRange;
  std::array<u32, 3> resolveCopyFilterCoefficients{0, 64, 0};
  Vec4<float> clearColorValue{0.f, 0.f, 0.f, 0.f};
  // 1.f is the forward-Z "farthest" clear value; under UseReversedZ farthest is 0.f instead (see
  // gx::clear_depth_value(), which the main render pass explicitly overrides this default with -
  // any OTHER pass that keeps this default, e.g. an offscreen render-to-texture pass composited
  // later, needs the same reversed-Z-aware value or its depth buffer starts "already nearest",
  // failing every subsequent depth test and making whatever's drawn into it vanish).
  float clearDepthValue = gx::UseReversedZ ? 0.f : 1.f;
  CommandList commands;
  bool clearColor = true;
  bool clearDepth = true;
  // Up to four perspective cameras (xf[4], xf[5]) whose draws were counted by post::note_draw in
  // this pass: the bloom / SSAO scene snapshot is taken after the last pass of the world camera.
  std::array<std::array<float, 4>, 4> postCameras{};
  std::array<uint32_t, 4> postCameraDraws{};
  // Of those draws, the ones that may write depth (z-update on, LESS / LEQUAL / EQUAL test).
  std::array<uint32_t, 4> postCameraDepthWrites{};
  uint8_t postCameraCount = 0;
  // Depth writes the SSAO merge cannot defer past: from a draw with no tagged camera (2D, other
  // shaders, a fifth camera) or with a test that can move depth away from the camera.
  bool depthWritesForeign = false;
  // The first pass after a display copy: a new game frame starts here (post-process light shafts
  // and area colours run once per game frame).
  bool startsGameFrame = false;
  // Glow source copy to soften after this pass' resolve (post::soften_copy); 0 = none.
  float resolveSoftenX = 0.0f, resolveSoftenY = 0.0f;
  // A GX copy with the clear flag follows this pass: the previous pass' depth is still intact when
  // this pass begins (the clear is this pass' load op or its first draw). SSAO snapshots it there.
  bool captureDepthBefore = false;
  // Some draw of this pass may write depth (any non-GX draw, or a GX draw with z-update on). The SSAO
  // depth merge skips world passes after which the EFB depth cannot have changed.
  bool depthWrites = false;
  // The resolve destination outlives the frame with no re-issue path (one-shot
  // bake), so this pass may not drop draws even in skip-unready-pipelines mode.
  bool requireReadyPipelines = false;
  bool resolveHalfScale = false;
  bool resolveCopyFilterActive = false;
  bool resolveForceOpaqueAlpha = false;
  std::string resolveDumpPath; // diagnostic readback of this pass's copy (draw dump)
  bool resolveNeedsConversion = false;
  bool resolveNeedsShaderSampling = false;
  bool resolveLinearSampling = false;
  bool snapshotColorResolveSource = false;
  std::vector<tex_palette_conv::ConvRequest> paletteConvs;
  std::vector<tex_cmpr_decode::Request> cmprDecodes;
  // GPU buffer set holding this pass' vertex, index, uniform and storage ranges (see GpuBufferSet).
  uint8_t bufferSet = kUntaggedBufferSet;
};
static std::vector<RenderPass> g_renderPasses;
// Passes a seal recorded after its last display copy: the start of the next game frame, which every
// image of the next batch (inserted and native) draws first. Only while frame interpolation runs.
static std::vector<RenderPass> g_carriedPasses;
// The batch being recorded begins at a game frame start: either the previous seal moved its passes
// after the display copy here, or it ended exactly on a frame boundary.
static bool g_nextBatchStartsGameFrame = false;
// Seals in a row without any display copy; two mean a title that presents without one, where every
// seal is its own frame boundary.
static uint32_t g_sealsWithoutDisplayCopy = 0;
static bool g_boundaryAtSeal = false;
static u32 g_currentRenderPass = UINT32_MAX;

// Recycle command storage: discarding passes used to free their command lists too, so each frame
// rebuilt hundreds of KB from zero capacity. The passes themselves are cheap to recreate.
using CommandListPool = std::vector<CommandList>;
static CommandListPool g_commandListPool;
static constexpr size_t MaxPooledCommandLists = 32;
// The pool is the only recording storage both sides touch, a few pointer moves per frame, so a
// plain mutex is cheaper than the alternatives and keeps the vector's invariants.
static std::mutex g_commandListPoolMutex;

static CommandList acquire_command_list() noexcept {
  std::lock_guard lock{g_commandListPoolMutex};
  if (g_commandListPool.empty()) {
    return {};
  }
  CommandList list = std::move(g_commandListPool.back());
  g_commandListPool.pop_back();
  list.clear();
  return list;
}

static void release_command_list(CommandList&& list) noexcept {
  if (list.capacity() == 0) {
    return;
  }
  std::lock_guard lock{g_commandListPoolMutex};
  if (g_commandListPool.size() >= MaxPooledCommandLists) {
    return;
  }
  list.clear();
  g_commandListPool.emplace_back(std::move(list));
}

static void release_render_pass(RenderPass& pass) noexcept { release_command_list(std::move(pass.commands)); }

static void recycle_render_passes(std::vector<RenderPass>& passes) noexcept {
  for (auto& pass : passes) {
    release_render_pass(pass);
  }
  passes.clear();
}

struct SealedFrameData {
  depth_peek::FrameMapping depthMapping;
  DebugFrameData debug;
  std::vector<RenderPass> passes;
  // The passes begin at a game frame start (the previous seal carried its tail here).
  bool startsGameFrame = false;
  // The passes after the last display copy moved to the next batch: the list ends at that copy.
  bool endsAtDisplayCopy = false;
  uint32_t carriedOutDraws = 0;
  // GPU buffer set of this batch's own passes.
  uint8_t ownBufferSet = 0;
  // CMPR decodes write textures once; the first image of a presentation group runs them.
  bool decodesDone = false;
};

SealedFrame::SealedFrame() : m_data(std::make_unique<SealedFrameData>()) {}
SealedFrame::~SealedFrame() = default;
SealedFrame::SealedFrame(SealedFrame&&) noexcept = default;
SealedFrame& SealedFrame::operator=(SealedFrame&&) noexcept = default;

static bool g_requirePersistentEfbPipelines = false;
static RenderPass& push_render_pass(RenderPass&& pass) {
  // Skipping an uncompiled draw permanently loses contents of a persistent EFB,
  // including work after its latest display copy. Retain every hardware draw.
  pass.requireReadyPipelines |= g_requirePersistentEfbPipelines;
  auto& out = g_renderPasses.emplace_back(std::move(pass));
  if (out.commands.capacity() == 0) {
    out.commands = acquire_command_list();
  }
  return out;
}
// The producer may build the next frame's GX stream while the worker prepares the target, and
// this is the only renderer-owned property that path queries, so publish it explicitly.
static std::atomic_bool g_inOffscreen{false};
static std::optional<RenderPass> g_suspendedEfbPass;
static Viewport g_suspendedEfbViewport;
static ClipRect g_suspendedEfbScissor;
// Prefix referenced by a suspended EFB pass. Preserve its offsets across an
// offscreen split, without rendering it before the bake it may sample finishes.
static StagingSizes g_suspendedEfbBytes{};
static constexpr StagingSizes PhysicalStagingCapacity{
    VertexBufferSize, UniformBufferSize, IndexBufferSize, StorageBufferSize};
static StagingSizes g_stagingCapacity = PhysicalStagingCapacity;
static uint64_t g_stagingEpoch = 0;
static uint64_t g_stagingSplitCount = 0;
static StagingSizes g_stagingHighWater{};

StagingSizes staging_usage() noexcept {
  return {g_verts.size(), g_uniforms.size(), g_indices.size(), g_storage.size()};
}
StagingSizes staging_high_water() noexcept { return g_stagingHighWater; }
uint64_t staging_epoch() noexcept { return g_stagingEpoch; }
uint64_t staging_split_count() noexcept { return g_stagingSplitCount; }
uint64_t staging_uniform_bytes(uint64_t bytes) {
  return staging_padded(bytes, g_cachedLimits.minUniformBufferOffsetAlignment);
}
uint64_t staging_storage_bytes(uint64_t bytes) {
  return staging_padded(bytes, g_cachedLimits.minStorageBufferOffsetAlignment);
}
void set_staging_capacity_limits_for_testing(const StagingSizes& limits) {
  for (unsigned i = 0; i < limits.size(); ++i) {
    if (limits[i] > PhysicalStagingCapacity[i])
      throw StagingCapacityError("Test staging capacity exceeds physical buffer");
  }
  g_stagingCapacity = limits;
  g_stagingHighWater = {};
}
bool staging_has_space(const StagingSizes& demand) {
  // Async readback preparation runs in the worker's noexcept seal prologue.
  // Reserve all 32 slots plus the uniform binding's 3840-byte trailing window.
  const StagingSizes tail{0, gx::MaxUniformSize + efb_ram::MaxAsyncReadbackSlots * staging_uniform_bytes(48), 0, 0};
  const StagingSizes retained = g_suspendedEfbPass ? g_suspendedEfbBytes : StagingSizes{};
  if (!staging_fits(retained, demand, tail, g_stagingCapacity))
    throw StagingCapacityError("GPU operation exceeds staging capacity including retained EFB data");
  return staging_fits(staging_usage(), demand, tail, g_stagingCapacity);
}
void ensure_staging_space(const StagingSizes& demand) {
  if (staging_has_space(demand)) return;
  split_staging_batch();
  if (!staging_has_space(demand))
    throw StagingCapacityError("GPU operation still exceeds staging capacity after submission");
}

static void discard_suspended_efb_pass() noexcept {
  if (g_suspendedEfbPass) {
    release_render_pass(*g_suspendedEfbPass);
    g_suspendedEfbPass.reset();
  }
}

static bool has_current_render_pass() noexcept { return g_currentRenderPass < g_renderPasses.size(); }
static webgpu::TextureWithSampler g_offscreenColor;
static webgpu::TextureWithSampler g_offscreenDepth;

struct ResolveSourceSnapshotPool {
  TextureHandle entry;
};

// Snapshot resources live in the same safe frame slots as the mapped staging buffers, and copies
// within a frame are ordered, so one grow-only snapshot serves every resolve in that slot.
static std::array<ResolveSourceSnapshotPool, FrameSlotCount> g_resolveSourceSnapshotPools;
static size_t g_recordingSnapshotSlot = 0;

static TextureHandle new_resolve_source_snapshot(wgpu::Extent3D size, wgpu::TextureFormat format) noexcept {
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "GX Copy Source Snapshot",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = format,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto texture = g_device.CreateTexture(&textureDescriptor);
  constexpr wgpu::TextureViewDescriptor viewDescriptor{
      .label = "GX Copy Source Snapshot view",
      .dimension = wgpu::TextureViewDimension::e2D,
  };
  auto textureView = texture.CreateView(&viewDescriptor);
  wgpu::TextureView sampleTextureView = textureView;
  return std::make_shared<TextureRef>(std::move(texture), std::move(sampleTextureView), std::move(textureView), size,
                                      format, 1, GX_TF_RGBA8);
}

static TextureHandle acquire_resolve_source_snapshot(uint32_t width, uint32_t height) noexcept {
  auto& pool = g_resolveSourceSnapshotPools[g_recordingSnapshotSlot];
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;

  if (pool.entry && pool.entry->size.width >= width && pool.entry->size.height >= height &&
      pool.entry->format == format) {
    return pool.entry;
  }

  const wgpu::Extent3D size{
      std::max(width, pool.entry ? pool.entry->size.width : 0u),
      std::max(height, pool.entry ? pool.entry->size.height : 0u),
      1,
  };
  pool.entry = new_resolve_source_snapshot(size, format);
  return pool.entry;
}

struct ResolveSamplingPlan {
  bool needsConversion = false;
  bool needsShaderSampling = false;
  bool usesLinearSampling = false;
};

static ResolveSamplingPlan make_resolve_sampling_plan(const TextureHandle& target, GXTexFmt format,
                                                       const ClipRect& resolveRect, const Vec4<float>& sourceRect,
                                                       bool halfScale, bool copyFilterActive,
                                                       bool forceOpaqueAlpha) noexcept {
  const auto differs = [](float lhs, float rhs) { return std::abs(lhs - rhs) > 0.01f; };
  const auto differsFromDst = [&](uint32_t dst, float src) {
    return differs(static_cast<float>(dst), src);
  };
  const bool sourceMatchesIntegerRect =
      !differs(sourceRect.x(), static_cast<float>(resolveRect.x)) &&
      !differs(sourceRect.y(), static_cast<float>(resolveRect.y)) &&
      !differs(sourceRect.z(), static_cast<float>(resolveRect.width)) &&
      !differs(sourceRect.w(), static_cast<float>(resolveRect.height));
  const bool dstMatchesSource = target && !differsFromDst(target->size.width, sourceRect.z()) &&
                                !differsFromDst(target->size.height, sourceRect.w());
  const bool needsShaderSampling =
      halfScale || copyFilterActive || forceOpaqueAlpha || !sourceMatchesIntegerRect || !dstMatchesSource;
  return {
      .needsConversion = tex_copy_conv::needs_conversion(format),
      .needsShaderSampling = needsShaderSampling,
      .usesLinearSampling = halfScale || (needsShaderSampling && !copyFilterActive),
  };
}

static ClipRect calculate_resolve_snapshot_rect(const wgpu::Extent3D& targetSize,
                                                const Vec4<float>& sourceRect,
                                                const ResolveSamplingPlan& samplingPlan,
                                                bool copyFilterActive) noexcept {
  const ClipRect fullTarget{
      .x = 0,
      .y = 0,
      .width = static_cast<int32_t>(targetSize.width),
      .height = static_cast<int32_t>(targetSize.height),
  };
  // Shader-sampled and converted copies can feed effects that read outside the copy rectangle
  // (MKW's DOF chain), so keep those full-target. Exact copies have a closed, crop-safe footprint.
  if (samplingPlan.needsConversion ||
      samplingPlan.needsShaderSampling ||
      targetSize.width == 0 || targetSize.height == 0 ||
      !std::isfinite(sourceRect.x()) || !std::isfinite(sourceRect.y()) ||
      !std::isfinite(sourceRect.z()) || !std::isfinite(sourceRect.w()) ||
      sourceRect.z() <= 0.0f || sourceRect.w() <= 0.0f) {
    return fullTarget;
  }

  // Linear sampling can touch one neighbor on every edge. The vertical GX
  // copy filter explicitly samples the previous and next rows.
  const int32_t haloX = samplingPlan.usesLinearSampling ? 1 : 0;
  const int32_t haloY = (samplingPlan.usesLinearSampling || copyFilterActive) ? 1 : 0;
  const int32_t targetWidth = static_cast<int32_t>(targetSize.width);
  const int32_t targetHeight = static_cast<int32_t>(targetSize.height);
  const int32_t left = std::clamp(static_cast<int32_t>(std::floor(sourceRect.x())) - haloX, 0, targetWidth - 1);
  const int32_t top = std::clamp(static_cast<int32_t>(std::floor(sourceRect.y())) - haloY, 0, targetHeight - 1);
  const int32_t right = std::clamp(static_cast<int32_t>(std::ceil(sourceRect.x() + sourceRect.z())) + haloX,
                                   left + 1, targetWidth);
  const int32_t bottom = std::clamp(static_cast<int32_t>(std::ceil(sourceRect.y() + sourceRect.w())) + haloY,
                                    top + 1, targetHeight);
  return {
      .x = left,
      .y = top,
      .width = right - left,
      .height = bottom - top,
  };
}

static void set_efb_targets(RenderPass& pass) {
  pass.colorView = webgpu::g_frameBuffer.view;
  pass.resolveView = webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.view : nullptr;
  pass.depthView = webgpu::g_depthBuffer.view;
  pass.depthTexture = webgpu::g_depthBuffer.texture;
  pass.depthSize = webgpu::g_depthBuffer.size;
  pass.copySourceTexture =
      webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.texture : webgpu::g_frameBuffer.texture;
  pass.copySourceView =
      webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved.view : webgpu::g_frameBuffer.view;
  pass.copySourceDepthView = webgpu::g_depthBuffer.view;
  pass.targetSize = webgpu::g_frameBuffer.size;
  pass.msaaSamples = webgpu::g_graphicsConfig.msaaSamples;
}

struct OffscreenCacheKey {
  uint32_t width;
  uint32_t height;

  bool operator==(const OffscreenCacheKey& rhs) const { return width == rhs.width && height == rhs.height; }
  template <typename H>
  friend H AbslHashValue(H h, const OffscreenCacheKey& key) {
    return H::combine(std::move(h), key.width, key.height);
  }
};
struct OffscreenCacheEntry {
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler depth;
};
static absl::flat_hash_map<OffscreenCacheKey, OffscreenCacheEntry> g_offscreenCache;
std::vector<TextureUpload> g_textureUploads;

static inline void push_command(CommandType type, const Command::Data& data) {
  if (!has_current_render_pass())
    UNLIKELY {
      Log.warn("Dropping command {}", magic_enum::enum_name(type));
      return;
    }
  g_renderPasses[g_currentRenderPass].commands.push_back({
      .type = type,
#ifdef AURORA_GFX_DEBUG_GROUPS
      .debugGroupStack = g_debugFrame.groups,
#endif
      .data = data,
  });
}

template <>
gx::DrawData* get_last_draw_command() {
  if (g_currentRenderPass >= g_renderPasses.size()) {
    return nullptr;
  }
  auto& last = g_renderPasses[g_currentRenderPass].commands.back();
  if (last.type != CommandType::Draw || last.data.draw.type != ShaderType::GX) {
    return nullptr;
  }
  return &last.data.draw.gx;
}

void note_resolve_soften(float scaleX, float scaleY) noexcept {
  // resolve_pass has just begun the next pass; the copy belongs to the one before it.
  if (g_currentRenderPass == 0 || g_currentRenderPass > g_renderPasses.size()) return;
  auto& pass = g_renderPasses[g_currentRenderPass - 1];
  if (!pass.resolveTarget) return;
  pass.resolveSoftenX = scaleX;
  pass.resolveSoftenY = scaleY;
}

void note_display_copy() noexcept {
  if (has_current_render_pass()) g_renderPasses[g_currentRenderPass].startsGameFrame = true;
  gx::note_frame_interpolation_boundary();
}

// Camera slot of the draw being recorded (note_pass_camera, then push_draw_command on the same
// thread); -1 = no tagged camera.
static int g_drawPassCamera = -1;

void note_pass_camera(float a, float b, float znear, float zfar) noexcept {
  if (!has_current_render_pass()) return;
  auto& pass = g_renderPasses[g_currentRenderPass];
  const std::array<float, 4> key{a, b, znear, zfar};
  for (uint8_t i = 0; i < pass.postCameraCount; ++i)
    if (pass.postCameras[i] == key) { ++pass.postCameraDraws[i]; g_drawPassCamera = i; return; }
  if (pass.postCameraCount < pass.postCameras.size()) {
    pass.postCameraDraws[pass.postCameraCount] = 1;
    g_drawPassCamera = pass.postCameraCount;
    pass.postCameras[pass.postCameraCount++] = key;
  }
}

static void push_draw_command(ShaderDrawCommand data) {
  const bool gx = data.type == ShaderType::GX;
  const bool depthWrite = !gx || data.gx.depthWrite;
  const bool unordered = !gx || data.gx.depthWriteUnordered;
  const int camera = g_drawPassCamera;
  g_drawPassCamera = -1;
  push_command(CommandType::Draw, Command::Data{.draw = data});
  if (depthWrite && g_currentRenderPass < g_renderPasses.size()) {
    auto& pass = g_renderPasses[g_currentRenderPass];
    pass.depthWrites = true;
    if (camera >= 0 && camera < pass.postCameraCount && !unordered) ++pass.postCameraDepthWrites[camera];
    else pass.depthWritesForeign = true;
  }
  ++g_drawCallCount;
}

Vec2<uint32_t> get_frame_buffer_size() noexcept {
  if (webgpu::g_frameBuffer.size.width != 0 && webgpu::g_frameBuffer.size.height != 0) {
    return {webgpu::g_frameBuffer.size.width, webgpu::g_frameBuffer.size.height};
  }
  const auto windowSize = window::get_window_size();
  return {windowSize.fb_width, windowSize.fb_height};
}

// Render-thread only: the frame worker recycles g_renderPasses in end_frame, so an off-thread
// read is a use-after-free rather than a stale value. Use get_frame_buffer_size() instead.
Vec2<uint32_t> get_render_target_size() noexcept {
  if (g_currentRenderPass < g_renderPasses.size()) {
    const auto& size = g_renderPasses[g_currentRenderPass].targetSize;
    return {size.width, size.height};
  }
  return get_frame_buffer_size();
}

static Viewport g_cachedViewport;
void set_viewport(const Viewport& cmd) noexcept {
  if (cmd != g_cachedViewport) {
    push_command(CommandType::SetViewport, Command::Data{.setViewport = cmd});
    g_cachedViewport = cmd;
  }
}

static ClipRect g_cachedScissor;
void set_scissor(const ClipRect& cmd) noexcept {
  if (cmd != g_cachedScissor) {
    push_command(CommandType::SetScissor, Command::Data{.setScissor = cmd});
    g_cachedScissor = cmd;
  }
}

template <>
void push_draw_command(clear::DrawData data) {
  if (data.uniformRange.size == 0) {
    ensure_staging_space({0, staging_uniform_bytes(16), 0, 0});
    const std::array clearUniform{
        std::clamp(data.depth, 0.f, 1.f),
        0.f,
        0.f,
        0.f,
    };
    data.uniformRange = push_uniform(clearUniform);
  }
  push_draw_command(ShaderDrawCommand{.type = ShaderType::Clear, .clear = data});
}

template <>
PipelineRef pipeline_ref(const clear::PipelineConfig& config) {
  return find_pipeline(ShaderType::Clear, config, [=] { return create_pipeline(config); });
}

void resolve_pass(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                  Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                  const Vec4<float>* sourceRectPixels, bool halfScale,
                  const std::array<u32, 3>* copyFilterCoefficients, bool forceOpaqueAlpha,
                  float copyFilterRowStride, bool clampTop, bool clampBottom, bool persistentCopy) {
  // Resolve current render pass
  if (!has_current_render_pass()) {
    Log.warn("Dropping resolve pass without an active render pass");
    return;
  }
  ensure_staging_space({0, 2 * staging_uniform_bytes(48), 0, 0});
  auto& prevPass = g_renderPasses[g_currentRenderPass];
  const auto targetWidth = static_cast<int32_t>(prevPass.targetSize.width);
  const auto targetHeight = static_cast<int32_t>(prevPass.targetSize.height);
  if (targetWidth <= 0 || targetHeight <= 0) {
    Log.warn("Dropping resolve pass with invalid target size {}x{}", targetWidth, targetHeight);
    return;
  }
  Vec4<float> sourceRect = sourceRectPixels != nullptr
                               ? *sourceRectPixels
                               : Vec4<float>{static_cast<float>(rect.x), static_cast<float>(rect.y),
                                             static_cast<float>(rect.width), static_cast<float>(rect.height)};
  if (targetWidth > 0 && targetHeight > 0) {
    const int32_t left = std::clamp(rect.x, 0, targetWidth - 1);
    const int32_t top = std::clamp(rect.y, 0, targetHeight - 1);
    const int32_t right = std::clamp(rect.x + rect.width, left + 1, targetWidth);
    const int32_t bottom = std::clamp(rect.y + rect.height, top + 1, targetHeight);
    rect = {
        .x = left,
        .y = top,
        .width = right - left,
        .height = bottom - top,
    };

    const float srcW = static_cast<float>(targetWidth);
    const float srcH = static_cast<float>(targetHeight);
    const float srcLeft = std::clamp(sourceRect.x(), 0.0f, srcW);
    const float srcTop = std::clamp(sourceRect.y(), 0.0f, srcH);
    const float srcRight = std::clamp(sourceRect.x() + sourceRect.z(), srcLeft, srcW);
    const float srcBottom = std::clamp(sourceRect.y() + sourceRect.w(), srcTop, srcH);
    // A channel-copy tile can extend past the right edge. Clipping the clear
    // rectangle is necessary, but shrinking the sampling extent stretches the
    // valid pixels across the output. Preserve their original texel spacing;
    // the conversion sampler clamps the out-of-range tail to the edge texel.
    if (!tex_copy_conv::detail::preserve_a8_right_edge(static_cast<uint32_t>(resolveFormat),
          sourceRect.x(),sourceRect.y(),sourceRect.z(),sourceRect.w(),srcW,srcH)) {
      sourceRect = {srcLeft, srcTop, std::max(srcRight - srcLeft, 1.0f), std::max(srcBottom - srcTop, 1.0f)};
    }
  }
  if (auto* dump = draw_dump::active()) UNLIKELY {
    const uint32_t copyIndex = draw_dump::g_copyIndex++;
    if (copyIndex == draw_dump::requested_copy()) draw_dump::g_copyTexture = texture;
    if (draw_dump::all_copies_requested())
      prevPass.resolveDumpPath = draw_dump::base_path() + ".copy" + std::to_string(copyIndex) + ".bmp";
    std::fprintf(dump, "copy pass=%u rect=%d,%d,%d,%d source=%g,%g,%g,%g target=%ux%u format=%u clear=%d/%d/%d "
                       "half=%d dst=%ux%u pf=%u opaque_alpha=%d clear_rgba=%.3f,%.3f,%.3f,%.3f\n",
                 g_currentRenderPass, rect.x, rect.y, rect.width, rect.height, sourceRect.x(), sourceRect.y(),
                 sourceRect.z(), sourceRect.w(), prevPass.targetSize.width, prevPass.targetSize.height,
                 unsigned(resolveFormat), int(clearColor), int(clearAlpha), int(clearDepth), int(halfScale),
                 texture ? texture->size.width : 0, texture ? texture->size.height : 0,
                 unsigned(gx::g_gxState.pixelFmt), int(forceOpaqueAlpha), clearColorValue.x(), clearColorValue.y(),
                 clearColorValue.z(), clearColorValue.w());
  }
  prevPass.resolveTarget = std::move(texture);
  prevPass.requireReadyPipelines |= persistentCopy;
  prevPass.resolveRect = rect;
  prevPass.resolveSourceRect = sourceRect;
  prevPass.resolveFormat = resolveFormat;
  prevPass.resolveHalfScale = halfScale;
  prevPass.resolveForceOpaqueAlpha = forceOpaqueAlpha;
  prevPass.resolveCopyFilterCoefficients =
      copyFilterCoefficients != nullptr ? *copyFilterCoefficients : std::array<u32, 3>{0, 64, 0};
  prevPass.resolveCopyFilterActive = prevPass.resolveCopyFilterCoefficients[0] != 0 ||
                                     prevPass.resolveCopyFilterCoefficients[1] != 64 ||
                                     prevPass.resolveCopyFilterCoefficients[2] != 0;
  const auto samplingPlan = make_resolve_sampling_plan(
      prevPass.resolveTarget, resolveFormat, rect, sourceRect, halfScale,
      prevPass.resolveCopyFilterActive, forceOpaqueAlpha);
  prevPass.resolveNeedsConversion = samplingPlan.needsConversion;
  prevPass.resolveNeedsShaderSampling = samplingPlan.needsShaderSampling;
  prevPass.resolveLinearSampling = samplingPlan.usesLinearSampling;
  prevPass.snapshotColorResolveSource =
      !gx::is_depth_format(resolveFormat) && (clearColor || clearAlpha || clearDepth);
  Vec4<float> uniformSourceRect = sourceRect;
  float srcW = static_cast<float>(prevPass.targetSize.width);
  float srcH = static_cast<float>(prevPass.targetSize.height);
  if (prevPass.snapshotColorResolveSource) {
    prevPass.resolveSnapshotRect = calculate_resolve_snapshot_rect(
        prevPass.targetSize, sourceRect, samplingPlan, prevPass.resolveCopyFilterActive);
    prevPass.resolveSourceSnapshot = acquire_resolve_source_snapshot(
        static_cast<uint32_t>(prevPass.resolveSnapshotRect.width),
        static_cast<uint32_t>(prevPass.resolveSnapshotRect.height));
    if (AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_CAPTURE_EFB_SNAPSHOT"))
      g_diagnosticResolveSnapshot = prevPass.resolveSourceSnapshot;
    uniformSourceRect = {
        sourceRect.x() - static_cast<float>(prevPass.resolveSnapshotRect.x),
        sourceRect.y() - static_cast<float>(prevPass.resolveSnapshotRect.y),
        sourceRect.z(), sourceRect.w()};
    srcW = static_cast<float>(prevPass.resolveSourceSnapshot->size.width);
    srcH = static_cast<float>(prevPass.resolveSourceSnapshot->size.height);
  }
  // GX's copy-clamp bits pin every vertical filter tap to the first or last source texel; without
  // it a filtered copy at internal resolution samples an unrelated EFB row as a visible border.
  const float clampTopPixels = clampTop ? uniformSourceRect.y() : 0.0f;
  const float clampBottomPixels =
      clampBottom ? uniformSourceRect.y() + uniformSourceRect.w() : srcH;
  const float clampTopUv = (clampTopPixels + 0.5f) / srcH;
  const float clampBottomUv = (clampBottomPixels - 0.5f) / srcH;
  // Push UV transform uniform for tex_copy_conv (crop region in UV space)
  const std::array resolveUniform{
      uniformSourceRect.x() / srcW,
      uniformSourceRect.y() / srcH,
      uniformSourceRect.z() / srcW,
      uniformSourceRect.w() / srcH,
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[0]),
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[1]),
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[2]),
      prevPass.resolveCopyFilterActive ? 1.0f : 0.0f,
      prevPass.resolveForceOpaqueAlpha ? 1.0f : 0.0f,
      std::max(copyFilterRowStride, 1.0f),
      clampTopUv,
      clampBottomUv,
  };
  prevPass.resolveUniformRange = push_uniform(resolveUniform);
  const bool clearFullTarget = rect.x <= 0 && rect.y <= 0 &&
                               rect.width >= static_cast<int32_t>(prevPass.targetSize.width) &&
                               rect.height >= static_cast<int32_t>(prevPass.targetSize.height);
  const bool useAttachmentColorClear = clearFullTarget && clearColor && clearAlpha;
  const bool useAttachmentDepthClear = clearFullTarget && clearDepth;

  // Populate new render pass from previous
  const auto msaaSamples = prevPass.msaaSamples;
  RenderPass newPass{
      .colorView = prevPass.colorView,
      .resolveView = prevPass.resolveView,
      .depthView = prevPass.depthView,
      .depthTexture = prevPass.depthTexture,
      .depthSize = prevPass.depthSize,
      .copySourceTexture = prevPass.copySourceTexture,
      .copySourceView = prevPass.copySourceView,
      .copySourceDepthView = prevPass.copySourceDepthView,
      .targetSize = prevPass.targetSize,
      .msaaSamples = msaaSamples,
      .clearColorValue = clearColorValue,
      .clearDepthValue = clearDepthValue,
      .clearColor = useAttachmentColorClear,
      .clearDepth = useAttachmentDepthClear,
      .captureDepthBefore = clearDepth,
  };
  push_render_pass(std::move(newPass));
  ++g_currentRenderPass;

  if ((!useAttachmentColorClear && (clearColor || clearAlpha)) || (!useAttachmentDepthClear && clearDepth)) {
    // GX copy clears cover the copied EFB rectangle, not always the whole target, so use a scissored
    // clear draw unless the load op can clear all of it.
    push_draw_command(clear::DrawData{
        .pipeline = pipeline_ref(clear::PipelineConfig{
            .msaaSamples = msaaSamples,
            .clearColor = clearColor,
            .clearAlpha = clearAlpha,
            .clearDepth = clearDepth,
        }),
        .color =
            wgpu::Color{
                .r = clearColorValue.x(),
                .g = clearColorValue.y(),
                .b = clearColorValue.z(),
                .a = clearColorValue.w(),
            },
        .depth = clearDepthValue,
        .useScissor = !clearFullTarget,
        .scissor = rect,
    });
  }
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

bool can_push_cmpr_decode() noexcept { return has_current_render_pass(); }

// Decodes run before the pass the texture was created in, ahead of any sampling.
void push_cmpr_decode(tex_cmpr_decode::Request&& request) {
  g_renderPasses[g_currentRenderPass].cmprDecodes.push_back(std::move(request));
}

void queue_palette_conv(tex_palette_conv::ConvRequest req) {
  if (!has_current_render_pass()) {
    Log.warn("Dropping palette conversion without an active render pass");
    return;
  }
  g_renderPasses[g_currentRenderPass].paletteConvs.push_back(std::move(req));
}

bool is_offscreen() noexcept { return g_inOffscreen; }

uint32_t get_sample_count() noexcept {
  if (!has_current_render_pass()) {
    return webgpu::g_graphicsConfig.msaaSamples;
  }
  return g_renderPasses[g_currentRenderPass].msaaSamples;
}

void clear_caches() noexcept {
  // A resize replaces the EFB the carried passes target; the next batch starts without them and
  // shows its inserted images as copies of the native one.
  recycle_render_passes(g_carriedPasses);
  g_nextBatchStartsGameFrame = false;
  g_offscreenCache.clear();
  // Retire rather than free; see g_retiredBindGroups.
  g_retiredBindGroups.reserve(g_retiredBindGroups.size() + g_cachedBindGroups.size());
  for (auto& entry : g_cachedBindGroups) {
    g_retiredBindGroups.emplace_back(std::move(entry.second));
  }
  g_cachedBindGroups.clear();
}

static OffscreenCacheEntry get_offscreen_textures(uint32_t width, uint32_t height) {
  OffscreenCacheKey key{width, height};
  if (const auto it = g_offscreenCache.find(key); it != g_offscreenCache.end()) {
    return it->second;
  }
  const auto colorFormat = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  const wgpu::Extent3D size{width, height, 1};
  const wgpu::TextureDescriptor colorDesc{
      .label = "Offscreen Color",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = colorFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto colorTexture = g_device.CreateTexture(&colorDesc);
  auto colorView = colorTexture.CreateView();
  webgpu::TextureWithSampler color{
      .texture = std::move(colorTexture),
      .view = std::move(colorView),
      .size = size,
      .format = colorFormat,
  };
  const auto depthFormat = webgpu::g_graphicsConfig.depthFormat;
  const wgpu::TextureDescriptor depthDesc{
      .label = "Offscreen Depth",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = depthFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  auto depthTexture = g_device.CreateTexture(&depthDesc);
  auto depthView = depthTexture.CreateView();
  webgpu::TextureWithSampler depth{
      .texture = std::move(depthTexture),
      .view = std::move(depthView),
      .size = size,
      .format = depthFormat,
  };
  OffscreenCacheEntry entry{
      .color = std::move(color),
      .depth = std::move(depth),
  };
  auto [insertIt, _] = g_offscreenCache.emplace(key, std::move(entry));
  return insertIt->second;
}

void begin_offscreen(uint32_t width, uint32_t height) {
  ZoneScoped;
  CHECK(g_currentRenderPass != UINT32_MAX, "begin_offscreen called outside of a frame");

  // If the current EFB pass has no resolve target, its output is unobservable.
  // Suspend it so that we can resume it after the offscreen pass.
  if (!g_inOffscreen) {
    auto& currentPass = g_renderPasses[g_currentRenderPass];
    if (!currentPass.resolveTarget) {
      g_suspendedEfbBytes = staging_usage();
      g_suspendedEfbPass = std::move(currentPass);
      g_renderPasses.pop_back();
      --g_currentRenderPass;
    }
    g_suspendedEfbViewport = g_cachedViewport;
    g_suspendedEfbScissor = g_cachedScissor;
  }

  // Create offscreen textures
  auto offscreenEntry = get_offscreen_textures(width, height);
  g_offscreenColor = std::move(offscreenEntry.color);
  g_offscreenDepth = std::move(offscreenEntry.depth);

  // Start a new pass with offscreen targets
  RenderPass newPass{
      .colorView = g_offscreenColor.view,
      .depthView = g_offscreenDepth.view,
      .depthTexture = g_offscreenDepth.texture,
      .depthSize = {width, height, 1},
      .copySourceTexture = g_offscreenColor.texture,
      .copySourceView = g_offscreenColor.view,
      .copySourceDepthView = g_offscreenDepth.view,
      .targetSize = {width, height, 1},
      .msaaSamples = 1,
      .clearColorValue = {0.f, 0.f, 0.f, 0.f},
      // See the RenderPass::clearDepthValue default's comment: this offscreen pass gets its own
      // depth buffer, and the farthest clear value is 0.f, not 1.f, under UseReversedZ.
      .clearDepthValue = gx::UseReversedZ ? 0.f : 1.f,
      .clearColor = true,
      .clearDepth = true,
  };
  push_render_pass(std::move(newPass));
  ++g_currentRenderPass;

  g_inOffscreen = true;

  g_cachedViewport = {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
  g_cachedScissor = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void end_offscreen() {
  ZoneScoped;
  CHECK(g_inOffscreen, "end_offscreen called without begin_offscreen");

  g_inOffscreen = false;
  g_offscreenColor = {};
  g_offscreenDepth = {};

  // Resume suspended EFB pass, or start a new one (load existing content)
  if (g_suspendedEfbPass) {
    // Keeps its own recorded commands.
    g_renderPasses.emplace_back(std::move(*g_suspendedEfbPass));
    g_suspendedEfbPass.reset();
  } else {
    auto& pass = push_render_pass(RenderPass{});
    pass.clearColor = false;
    pass.clearDepth = false;
  }
  ++g_currentRenderPass;
  set_efb_targets(g_renderPasses[g_currentRenderPass]);

  g_cachedViewport = g_suspendedEfbViewport;
  g_cachedScissor = g_suspendedEfbScissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

template <>
void push_draw_command(gx::DrawData data) {
  push_draw_command(ShaderDrawCommand{.type = ShaderType::GX, .gx = data});
}

template <>
PipelineRef pipeline_ref(const gx::PipelineConfig& config) {
  return find_pipeline(ShaderType::GX, config, [=] { return create_pipeline(config); });
}

void initialize() {
  g_frameIndex = 0;
  depth_peek::initialize();
  tex_copy_conv::initialize();
  tex_palette_conv::initialize();
  tex_cmpr_decode::initialize();
  texture_replacement::initialize();

  // For uniform & storage buffer offset alignments
  g_device.GetLimits(&g_cachedLimits);

  const auto createBuffer = [](wgpu::Buffer& out, wgpu::BufferUsage usage, uint64_t size, const char* label) {
    if (size <= 0) {
      return;
    }
    const wgpu::BufferDescriptor descriptor{
        .label = label,
        .usage = usage | ((AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_CAPTURE_BUFFERS") || AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_CAPTURE_ALSO_BUFFERS")) ? wgpu::BufferUsage::CopySrc : wgpu::BufferUsage::None),
        .size = size,
    };
    out = g_device.CreateBuffer(&descriptor);
  };
  createBuffer(g_uniformBuffer, wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, UniformBufferSize,
               "Shared Uniform Buffer");
  createBuffer(g_vertexBuffer, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, VertexBufferSize,
               "Shared Vertex Buffer");
  createBuffer(g_indexBuffer, wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst, IndexBufferSize,
               "Shared Index Buffer");
  createBuffer(g_storageBuffer, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, StorageBufferSize,
               "Shared Storage Buffer");
  if (AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_TRACE_RASTER_COVERAGE"))
    createBuffer(g_diagnosticRasterCoverage, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc, 524288, "Diagnostic actual raster coverage");
  for (int i = 0; i < g_stagingBuffers.size(); ++i) {
    const auto label = fmt::format("Staging Buffer {}", i);
    createBuffer(g_stagingBuffers[i], wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc, StagingBufferSize,
                 label.c_str());
  }
  currentStagingBuffer = 0;
  s_mappingState.reset();
  map_staging_buffer();

  {
    std::vector<wgpu::BindGroupLayoutEntry> layoutEntries{
        // Vertex data buffer
        wgpu::BindGroupLayoutEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::ReadOnlyStorage,
                },
        },
        // Storage data buffer
        wgpu::BindGroupLayoutEntry{
            .binding = 1,
            .visibility = wgpu::ShaderStage::Vertex,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::ReadOnlyStorage,
                },
        },
    };
    if (g_diagnosticRasterCoverage)
      layoutEntries.push_back(wgpu::BindGroupLayoutEntry{.binding=2,.visibility=wgpu::ShaderStage::Fragment,.buffer={.type=wgpu::BufferBindingType::Storage}});
    const wgpu::BindGroupLayoutDescriptor layoutDesc{
        .label = "Static bind group layout",
        .entryCount = layoutEntries.size(),
        .entries = layoutEntries.data(),
    };
    g_staticBindGroupLayout = g_device.CreateBindGroupLayout(&layoutDesc);
    std::vector<wgpu::BindGroupEntry> entries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .buffer = g_vertexBuffer,
        },
        wgpu::BindGroupEntry{
            .binding = 1,
            .buffer = g_storageBuffer,
        },
    };
    if (g_diagnosticRasterCoverage)
      entries.push_back(wgpu::BindGroupEntry{.binding=2,.buffer=g_diagnosticRasterCoverage});
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .label = "Static bind group",
        .layout = g_staticBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_staticBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  }

  {
    constexpr std::array layoutEntries{
        // Uniform buffer (dynamic offset)
        wgpu::BindGroupLayoutEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::Uniform,
                    .hasDynamicOffset = true,
                },
        },
    };
    const wgpu::BindGroupLayoutDescriptor layoutDesc{
        .label = "Uniform bind group layout",
        .entryCount = layoutEntries.size(),
        .entries = layoutEntries.data(),
    };
    g_uniformBindGroupLayout = g_device.CreateBindGroupLayout(&layoutDesc);
    const std::array entries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .buffer = g_uniformBuffer,
            .size = gx::MaxUniformSize,
        },
    };
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .label = "Uniform bind group",
        .layout = g_uniformBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_uniformBindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  }
  g_bufferSets[0] = GpuBufferSet{
      .vertex = g_vertexBuffer,
      .uniform = g_uniformBuffer,
      .index = g_indexBuffer,
      .storage = g_storageBuffer,
      .staticBindGroup = g_staticBindGroup,
      .uniformBindGroup = g_uniformBindGroup,
  };
  g_bufferSets[1] = {};
  g_alternateBufferSets = false;
  g_boundBufferSet = 0;
  g_nextWriteBufferSet = 0;
  g_lastWrittenBufferSet = 0;

  gx::initialize();
  initialize_pipeline_cache();
}

// The second GPU buffer set (80 MB), made the first time a seal carries passes to the next batch.
// Runs in the seal prologue: the set the next upload writes is the one nothing references yet.
static void ensure_alternate_buffer_set() {
  if (g_alternateBufferSets) {
    return;
  }
  const auto createBuffer = [](wgpu::BufferUsage usage, uint64_t size, const char* label) {
    const wgpu::BufferDescriptor descriptor{
        .label = label,
        .usage = usage | ((AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_CAPTURE_BUFFERS") ||
                           AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_CAPTURE_ALSO_BUFFERS"))
                              ? wgpu::BufferUsage::CopySrc
                              : wgpu::BufferUsage::None),
        .size = size,
    };
    return g_device.CreateBuffer(&descriptor);
  };
  GpuBufferSet set{
      .vertex = createBuffer(wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, VertexBufferSize,
                             "Shared Vertex Buffer B"),
      .uniform = createBuffer(wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, UniformBufferSize,
                              "Shared Uniform Buffer B"),
      .index = createBuffer(wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst, IndexBufferSize,
                            "Shared Index Buffer B"),
      .storage = createBuffer(wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, StorageBufferSize,
                              "Shared Storage Buffer B"),
  };
  std::vector<wgpu::BindGroupEntry> staticEntries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = set.vertex},
      wgpu::BindGroupEntry{.binding = 1, .buffer = set.storage},
  };
  if (g_diagnosticRasterCoverage) {
    staticEntries.push_back(wgpu::BindGroupEntry{.binding = 2, .buffer = g_diagnosticRasterCoverage});
  }
  const wgpu::BindGroupDescriptor staticDescriptor{
      .label = "Static bind group B",
      .layout = g_staticBindGroupLayout,
      .entryCount = staticEntries.size(),
      .entries = staticEntries.data(),
  };
  set.staticBindGroup = g_device.CreateBindGroup(&staticDescriptor);
  const std::array uniformEntries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = set.uniform, .size = gx::MaxUniformSize},
  };
  const wgpu::BindGroupDescriptor uniformDescriptor{
      .label = "Uniform bind group B",
      .layout = g_uniformBindGroupLayout,
      .entryCount = uniformEntries.size(),
      .entries = uniformEntries.data(),
  };
  set.uniformBindGroup = g_device.CreateBindGroup(&uniformDescriptor);
  g_bufferSets[1] = std::move(set);
  g_alternateBufferSets = true;
  // Everything so far went to set 0, including the batch being sealed.
  g_nextWriteBufferSet = g_lastWrittenBufferSet ^ 1u;
  Log.info("Frame interpolation: second GPU buffer set created");
}

void shutdown() {
  shutdown_pipeline_cache();
  gx::clear_shader_module_cache();
  efb_ram::shutdown();
  depth_peek::shutdown();
  tex_copy_conv::shutdown();
  tex_palette_conv::shutdown();
  tex_cmpr_decode::shutdown();
  texture_replacement::shutdown();
  texture_upscale::shutdown();
  post::shutdown();
  gx::shutdown();

  g_textureUploads.clear();
  g_cachedBindGroups.clear();
  g_retiredBindGroups.clear();
  g_cachedSamplers.clear();
  g_vertexBuffer = {};
  g_uniformBuffer = {};
  g_indexBuffer = {};
  g_storageBuffer = {};
  g_bufferSets = {};
  g_alternateBufferSets = false;
  g_boundBufferSet = 0;
  g_carriedPasses.clear();
  g_nextBatchStartsGameFrame = false;
  g_diagnosticRasterCoverage = {};
  // Invalidate outstanding callbacks before releasing their buffers.
  s_mappingState.reset();
  g_stagingBuffers.fill({});
  for (auto& pool : g_resolveSourceSnapshotPools) {
    pool.entry.reset();
  }
  discard_suspended_efb_pass();
  g_renderPasses.clear();
  g_commandListPool.clear();
  g_currentRenderPass = UINT32_MAX;
  g_offscreenCache.clear();
  g_offscreenColor = {};
  g_offscreenDepth = {};
  g_staticBindGroup = {};
  g_staticBindGroupLayout = {};
  g_uniformBindGroup = {};
  g_uniformBindGroupLayout = {};
  g_inOffscreen = false;
  g_frameIndex = UINT32_MAX;
  currentStagingBuffer = 0;
}

void map_staging_buffer() {
  const auto generation = s_mappingState.request();
  if (generation == 0) {
    return;
  }

  g_stagingBuffers[currentStagingBuffer].MapAsync(
      wgpu::MapMode::Write, 0, StagingBufferSize, wgpu::CallbackMode::AllowSpontaneous,
      [generation](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        const auto result = status == wgpu::MapAsyncStatus::Success
            ? BufferMapState::Mapped : BufferMapState::Unmapped;
        if (!s_mappingState.complete(generation, result)) return;
        if (status == wgpu::MapAsyncStatus::CallbackCancelled || status == wgpu::MapAsyncStatus::Aborted) {
          Log.warn("Buffer mapping {}: {}", magic_enum::enum_name(status), message);
          return;
        }
        ASSERT(status == wgpu::MapAsyncStatus::Success, "Buffer mapping failed: {} {}", magic_enum::enum_name(status),
               message);
      });
}

static bool begin_frame_impl(bool clearEfb, bool capacityResume = false) {
  ZoneScoped;
  {
    ZoneScopedN("Wait for buffer map");
    map_staging_buffer();
    while (true) {
      const auto mappingState = s_mappingState.state();
      if (mappingState == BufferMapState::Mapped) {
        break;
      }
      if (mappingState == BufferMapState::Unmapped) {
        // Frame begin failed because the staging map was aborted; the caller's retry loop drops the frame
        // and any one-shot bakes recorded into it. Rate-limited.
        static uint32_t s_beginFrameMapFailCount = 0;
        if (s_beginFrameMapFailCount < 64 || (s_beginFrameMapFailCount & 255) == 0) {
          Log.warn("begin_frame aborted: staging buffer unmapped (frame={} occurrences={})", g_frameIndex,
                   s_beginFrameMapFailCount + 1);
        }
        ++s_beginFrameMapFailCount;
        return false;
      }
      g_instance.ProcessEvents();
      webgpu::fail_if_device_lost();
      s_mappingState.wait_for_progress();
    }
  }
  ++g_stagingEpoch;
  g_recordingSnapshotSlot = currentStagingBuffer;
  size_t bufferOffset = 0;
  const auto& stagingBuf = g_stagingBuffers[currentStagingBuffer];
  const auto mapBuffer = [&](ByteBuffer& buf, uint64_t size) {
    if (size <= 0) {
      return;
    }
    buf = ByteBuffer{static_cast<u8*>(stagingBuf.GetMappedRange(bufferOffset, size)), static_cast<size_t>(size)};
    bufferOffset += size;
  };
  mapBuffer(g_verts, VertexBufferSize);
  mapBuffer(g_uniforms, UniformBufferSize);
  mapBuffer(g_indices, IndexBufferSize);
  mapBuffer(g_storage, StorageBufferSize);
  if constexpr (UseTextureBuffer) {
    mapBuffer(g_textureUpload, TextureUploadSize);
  }

  g_drawCallCount = 0;
  g_mergedDrawCallCount = 0;
  if (clearEfb) {
    gx::begin_frame_interpolation();
  }
  discard_suspended_efb_pass();
  if (!capacityResume) webgpu::clear_present_source_override();

  push_render_pass(RenderPass{});
  set_efb_targets(g_renderPasses[0]);
  g_renderPasses[0].clearColorValue = gx::g_gxState.clearColor;
  g_renderPasses[0].clearDepthValue = gx::clear_depth_value();
  g_renderPasses[0].clearColor = clearEfb;
  g_renderPasses[0].clearDepth = clearEfb;
  g_currentRenderPass = 0;
  // Refresh paired render viewport/scissor from logical state in case the FB size changed.
  const auto mappedRenderState = gx::map_logical_render_state();
  const bool renderStateChanged = gx::g_gxState.renderViewport != mappedRenderState.viewport ||
                                  gx::g_gxState.renderScissor != mappedRenderState.scissor;
  gx::g_gxState.renderViewport = mappedRenderState.viewport;
  gx::g_gxState.renderScissor = mappedRenderState.scissor;
  gx::g_gxState.stateDirty = gx::g_gxState.stateDirty || renderStateChanged;
  g_cachedViewport = mappedRenderState.viewport;
  g_cachedScissor = mappedRenderState.scissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
  begin_pipeline_frame();
  return true;
}

bool begin_frame(bool clearEfb) {
  g_requirePersistentEfbPipelines = !clearEfb;
  // Starting a logical frame must reset interpolation even when the emulated
  // EFB survives the host presentation boundary. Mid-frame resume stays separate.
  if (!clearEfb && g_frameIndex != 0) gx::begin_frame_interpolation();
  // Producer thread, once per logical frame (EFB-persistent or not).
  gx::enforce_copy_texture_budget();
  return begin_frame_impl(clearEfb || g_frameIndex == 0);
}

bool resume_frame() { return begin_frame_impl(false); }

void abort_frame() noexcept {
  efb_ram::cancel();
  efb_ram::abort_async();
  g_verts.release();
  g_uniforms.release();
  g_indices.release();
  g_storage.release();
  if constexpr (UseTextureBuffer) {
    g_textureUploads.clear();
    g_textureUpload.release();
  }
  if (s_mappingState.state() == BufferMapState::Mapped) {
    // Pending interpolation tasks hold raw pointers into the mapped staging
    // range; they must be dropped before the buffer is unmapped and rotated.
    gx::drop_pending_frame_interpolation_uniforms();
    g_stagingBuffers[currentStagingBuffer].Unmap();
    s_mappingState.reset();
    currentStagingBuffer = (currentStagingBuffer + 1) % g_stagingBuffers.size();
    map_staging_buffer();
  }
  recycle_render_passes(g_renderPasses);
  g_currentRenderPass = UINT32_MAX;
  discard_suspended_efb_pass();
  g_inOffscreen = false;
  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
  webgpu::clear_present_source_override();
  end_pipeline_frame();
}

// One-shot, bounded producer-side capture. Data is copied while staging memory
// is still mapped; it never changes guest memory, draw state or user input.
static void capture_geometry_diagnostic() {
  static const std::string marker=[] {
    const char* value=AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_CAPTURE_MARKER");
    return value ? std::string(value)+".geometry" : std::string{};
  }();
  if(marker.empty())return;
  static auto nextProbe=std::chrono::steady_clock::now();
  const auto now=std::chrono::steady_clock::now();
  if(now<nextProbe)return;
  nextProbe=now+std::chrono::seconds(1);
  std::ifstream request(marker);
  if(!request)return;
  request.close();
  if(std::remove(marker.c_str())!=0)return;
  const auto dump=[&](const char* suffix,const ByteBuffer& buffer,size_t limit) {
    std::ofstream file(marker+suffix,std::ios::binary);
    const auto size=std::min(buffer.size(),limit);
    if(size)file.write(reinterpret_cast<const char*>(buffer.data()),size);
  };
  dump(".vertices.bin",g_verts,2*1024*1024);
  dump(".uniforms.bin",g_uniforms,4*1024*1024);
  dump(".indices.bin",g_indices,1024*1024);
  std::ofstream file(marker+".txt");
  file<<"frame "<<current_frame()<<" buffers "<<g_verts.size()<<' '<<g_uniforms.size()<<' '<<g_indices.size()<<'\n';
  unsigned commands=0;
  for(size_t i=0;i<g_renderPasses.size() && i<512;++i) {
    const auto& p=g_renderPasses[i];
    file<<"pass "<<i<<" target "<<p.targetSize.width<<' '<<p.targetSize.height
        <<" copy "<<bool(p.resolveTarget)<<" source "<<p.resolveSourceRect.x()<<' '<<p.resolveSourceRect.y()
        <<' '<<p.resolveSourceRect.z()<<' '<<p.resolveSourceRect.w()<<" format "<<unsigned(p.resolveFormat)<<'\n';
    for(const auto& command:p.commands) {
      if(++commands>16384) {file<<"truncated\n";return;}
      if(command.type==CommandType::SetViewport) {
        const auto& v=command.data.setViewport;
        file<<"viewport "<<v.left<<' '<<v.top<<' '<<v.width<<' '<<v.height<<'\n';
      } else if(command.type==CommandType::SetScissor) {
        const auto& s=command.data.setScissor;
        file<<"scissor "<<s.x<<' '<<s.y<<' '<<s.width<<' '<<s.height<<'\n';
      } else if(command.type==CommandType::Draw && command.data.draw.type==ShaderType::GX) {
        const auto& d=command.data.draw.gx;
        file<<"draw "<<std::hex<<d.pipeline<<std::dec<<" counts "<<d.vtxCount<<' '<<d.indexCount
            <<" vertices "<<d.vertRange.offset<<' '<<d.vertRange.size
            <<" uniforms "<<d.uniformRange.offset<<' '<<d.uniformRange.size
            <<" indices "<<d.idxRange.offset<<' '<<d.idxRange.size<<'\n';
      }
    }
  }
}

static void end_batch_impl(const wgpu::CommandEncoder& cmd, bool advanceFrame) {
  ZoneScoped;
  ASSERT(!advanceFrame || !g_inOffscreen, "end_frame called while offscreen rendering is active");
  if (advanceFrame) {
    // Game frames end at display copies. A title that presents without any (two seals in a row with
    // none) gets one boundary per seal instead, the batch being its whole frame.
    g_boundaryAtSeal = false;
    if (gx::frame_interpolation_active()) {
      if (gx::frame_interpolation_boundaries_in_batch() != 0) {
        g_sealsWithoutDisplayCopy = 0;
      } else if (++g_sealsWithoutDisplayCopy >= 2) {
        gx::note_frame_interpolation_boundary();
        g_boundaryAtSeal = true;
      }
    }
    gx::finalize_frame_interpolation();
  } else {
    // Mid-frame batch split: the staging buffer is about to be unmapped and rotated, so pending
    // interpolation tasks pointing into it would dangle. Tie the clear to the rotation itself.
    gx::drop_pending_frame_interpolation_uniforms();
  }
  g_uniforms.append_zeroes(gx::MaxUniformSize); // Pad the end of the buffer
  const auto used = staging_usage();
  for (unsigned i = 0; i < used.size(); ++i) g_stagingHighWater[i] = std::max(g_stagingHighWater[i], used[i]);
  uint64_t bufferOffset = 0;
  const auto writeBuffer = [&](ByteBuffer& buf, wgpu::Buffer& out, uint64_t size, std::string_view label) {
    const auto writeSize = buf.size(); // Only need to copy this many bytes
    if (writeSize > 0) {
      cmd.CopyBufferToBuffer(g_stagingBuffers[currentStagingBuffer], bufferOffset, out, 0, AURORA_ALIGN(writeSize, 4));
      buf.release();
    }
    bufferOffset += size;
    return writeSize;
  };
  capture_geometry_diagnostic();
  // Upload into the set no carried pass references; everything recorded since the last upload uses it.
  const uint8_t writeSet = g_alternateBufferSets ? g_nextWriteBufferSet : uint8_t{0};
  if (g_alternateBufferSets) {
    g_nextWriteBufferSet = writeSet ^ 1u;
  }
  g_lastWrittenBufferSet = writeSet;
  bind_buffer_set(writeSet);
  for (auto& pass : g_renderPasses) {
    if (pass.bufferSet == kUntaggedBufferSet) {
      pass.bufferSet = writeSet;
    }
  }
  g_stagingBuffers[currentStagingBuffer].Unmap();
  s_mappingState.reset();
  g_stats.drawCallCount = g_drawCallCount;
  g_stats.mergedDrawCallCount = g_mergedDrawCallCount;
  g_stats.lastVertSize = writeBuffer(g_verts, g_vertexBuffer, VertexBufferSize, "Vertex");
  g_stats.lastUniformSize = writeBuffer(g_uniforms, g_uniformBuffer, UniformBufferSize, "Uniform");
  g_stats.lastIndexSize = writeBuffer(g_indices, g_indexBuffer, IndexBufferSize, "Index");
  g_stats.lastStorageSize = writeBuffer(g_storage, g_storageBuffer, StorageBufferSize, "Storage");
  if constexpr (UseTextureBuffer) {
    g_stats.lastTextureUploadSize = g_textureUpload.size();
    {
      // Perform texture copies
      for (const auto& item : g_textureUploads) {
        const wgpu::TexelCopyBufferInfo buf{
            .layout =
                wgpu::TexelCopyBufferLayout{
                    .offset = item.layout.offset + bufferOffset,
                    .bytesPerRow = AURORA_ALIGN(item.layout.bytesPerRow, 256),
                    .rowsPerImage = item.layout.rowsPerImage,
                },
            .buffer = g_stagingBuffers[currentStagingBuffer],
        };
        cmd.CopyBufferToTexture(&buf, &item.tex, &item.size);
      }
      g_textureUploads.clear();
      g_textureUpload.release();
    }
  }
  currentStagingBuffer = (currentStagingBuffer + 1) % g_stagingBuffers.size();
  map_staging_buffer();
  g_currentRenderPass = UINT32_MAX;
  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
  end_pipeline_frame();
  if (advanceFrame) {
    ++g_frameIndex;
  }
}

void end_frame(const wgpu::CommandEncoder& cmd) { end_batch_impl(cmd, true); }

void end_batch(const wgpu::CommandEncoder& cmd) { end_batch_impl(cmd, false); }

void split_staging_batch() {
  // Never called under the decoder's renderer lock: the worker needs that lock
  // to reach DONE. FIFO admission yields its unconsumed command first.
  aurora::wait_for_frame_worker();
  std::lock_guard gpuLock(aurora::renderer_gpu_mutex());
  if (!has_current_render_pass())
    throw StagingCapacityError("Cannot split staging outside an active render pass");
  gx::mark_frame_interpolation_replay_unsafe();
  const bool offscreen = g_inOffscreen;
  const auto viewport = g_cachedViewport;
  const auto scissor = g_cachedScissor;
  const auto renderViewport = gx::g_gxState.renderViewport;
  const auto renderScissor = gx::g_gxState.renderScissor;
  const auto& active = g_renderPasses[g_currentRenderPass];
  RenderPass continuation{
      .colorView = active.colorView, .resolveView = active.resolveView,
      .depthView = active.depthView, .depthTexture = active.depthTexture, .depthSize = active.depthSize,
      .copySourceTexture = active.copySourceTexture,
      .copySourceView = active.copySourceView, .copySourceDepthView = active.copySourceDepthView,
      .targetSize = active.targetSize, .msaaSamples = active.msaaSamples,
      .clearColor = false, .clearDepth = false,
      .requireReadyPipelines = active.requireReadyPipelines || offscreen,
  };
  auto suspended = std::move(g_suspendedEfbPass);
  g_suspendedEfbPass.reset();
  std::array<std::vector<uint8_t>, 4> retained;
  std::array<ByteBuffer*, 4> buffers{&g_verts, &g_uniforms, &g_indices, &g_storage};
  if (suspended) {
    for (unsigned i = 0; i < buffers.size(); ++i) {
      if (g_suspendedEfbBytes[i])
        retained[i].assign(buffers[i]->data(), buffers[i]->data() + g_suspendedEfbBytes[i]);
    }
  }
  auto encoder = g_device.CreateCommandEncoder();
  end_batch(encoder);
  render(encoder);
  aurora::submit_staging_commands(encoder.Finish());
  after_submit();
  if (!begin_frame_impl(false, true))
    throw StagingCapacityError("Staging remap failed after capacity submission");
  recycle_render_passes(g_renderPasses);
  push_render_pass(std::move(continuation));
  g_currentRenderPass = 0;
  g_suspendedEfbPass = std::move(suspended);
  for (unsigned i = 0; i < buffers.size(); ++i) {
    if (!retained[i].empty()) buffers[i]->append(retained[i].data(), retained[i].size());
  }
  g_inOffscreen = offscreen;
  g_cachedViewport = viewport;
  g_cachedScissor = scissor;
  gx::g_gxState.renderViewport = renderViewport;
  gx::g_gxState.renderScissor = renderScissor;
  gx::g_gxState.stateDirty = true;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = viewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = scissor});
  ++g_stagingSplitCount;
}

uint32_t current_frame() noexcept { return g_frameIndex; }
uint32_t current_render_pass_index() noexcept { return g_currentRenderPass; }
const uint8_t* staged_vertex_data(const Range& range) noexcept {
  return range.offset + range.size <= g_verts.size() ? g_verts.data() + range.offset : nullptr;
}

// The only place that erases from g_cachedBindGroups, whose handles the frame being encoded still
// holds, so it runs in the seal prologue with the renderer mutex held and the producer excluded.
void expire_bind_group_cache() noexcept {
  if (g_cachedBindGroups.empty() || g_frameIndex == UINT32_MAX || g_frameIndex % BindGroupCacheSweepPeriod != 0) {
    return;
  }

  ZoneScoped;
  for (auto it = g_cachedBindGroups.begin(); it != g_cachedBindGroups.end();) {
    if (g_frameIndex - it->second.lastUsedFrame > BindGroupCacheRetainFrames) {
      g_cachedBindGroups.erase(it++);
    } else {
      ++it;
    }
  }
}

// Debug labels for the render passes. Formatting them per pass per slot costs a heap-allocating
// fmt::format for a name nothing reads outside a capture.
static const char* render_pass_label(u32 index) noexcept {
  static constexpr std::array<const char*, 16> kRenderPassLabels{
      "Render pass 0",  "Render pass 1",  "Render pass 2",  "Render pass 3",
      "Render pass 4",  "Render pass 5",  "Render pass 6",  "Render pass 7",
      "Render pass 8",  "Render pass 9",  "Render pass 10", "Render pass 11",
      "Render pass 12", "Render pass 13", "Render pass 14", "Render pass 15",
  };
  return index < kRenderPassLabels.size() ? kRenderPassLabels[index] : "Render pass";
}

// Optional read-only verification of the exact dumped vertex shader against
// the same GPU buffers/uniform range consumed by the first original font draw.
// This compute pass writes only its own diagnostic storage buffer.
static wgpu::Buffer g_diagnosticVertexResult;
static TextureHandle g_diagnosticIsolatedGlyph;
wgpu::Buffer diagnostic_vertex_result() { return g_diagnosticVertexResult; }
TextureHandle diagnostic_isolated_glyph() { return g_diagnosticIsolatedGlyph; }
static void raster_diagnostic_glyph(const wgpu::CommandEncoder& encoder,const gx::DrawData& draw,bool first) {
    const auto size=webgpu::g_frameBuffer.size;
    if(first) g_diagnosticIsolatedGlyph=new_render_texture(size.width,size.height,GX_TF_RGBA8,"Isolated original font glyph sequence");
    const uint32_t samples=get_sample_count();
    wgpu::Texture color=g_diagnosticIsolatedGlyph->texture;
    auto colorView=g_diagnosticIsolatedGlyph->attachmentTextureView;
    wgpu::TextureView resolveView;
    if (samples>1) {
      const wgpu::TextureDescriptor desc{.label="Isolated glyph MSAA",.usage=wgpu::TextureUsage::RenderAttachment,
          .size=size,.format=g_diagnosticIsolatedGlyph->format,.sampleCount=samples};
      color=g_device.CreateTexture(&desc);colorView=color.CreateView();resolveView=g_diagnosticIsolatedGlyph->attachmentTextureView;
    }
    const wgpu::TextureDescriptor depthDesc{.label="Isolated glyph depth",.usage=wgpu::TextureUsage::RenderAttachment,
        .size=size,.format=webgpu::g_graphicsConfig.depthFormat,.sampleCount=samples};
    auto depth=g_device.CreateTexture(&depthDesc);auto depthView=depth.CreateView();
    const wgpu::RenderPassColorAttachment attachment{.view=colorView,.resolveTarget=resolveView,
        .loadOp=first ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load,.storeOp=wgpu::StoreOp::Store,.clearValue={0,0,0,1}};
    const wgpu::RenderPassDepthStencilAttachment depthAttachment{.view=depthView,
        .depthLoadOp=wgpu::LoadOp::Clear,.depthStoreOp=wgpu::StoreOp::Store,.depthClearValue=0};
    const wgpu::RenderPassDescriptor desc{.label="Isolated original glyph raster verification",.colorAttachmentCount=1,
        .colorAttachments=&attachment,.depthStencilAttachment=&depthAttachment};
    auto pass=encoder.BeginRenderPass(&desc);
    pass.SetBindGroup(0,g_staticBindGroup);pass.SetBindGroup(2,gx::g_emptyTextureBindGroup);
    pass.SetViewport(2,2,640,448,0,1);pass.SetScissorRect(0,0,640,448);
    gx::DrawEncodeState state{};state.boundTextureBindGroup=gx::g_emptyTextureBindGroup.Get();
    gx::render(draw,pass,state,true,nullptr);pass.End();
  }

static uint32_t g_diagnosticFontFrame=UINT32_MAX;
static void encode_diagnostic_vertices(const wgpu::CommandEncoder& encoder, const gx::DrawData& draw) {
  const char* path = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_VERTEX_SHADER_PATH");
  if (!path || draw.pipeline != 0xF892D70D494F0487ull) return;
  if (g_diagnosticVertexResult) {
    if (g_diagnosticFontFrame==current_frame() && AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_CAPTURE_ISOLATED_GLYPH_SEQUENCE"))
      raster_diagnostic_glyph(encoder,draw,false);
    return;
  }
  if (draw.vtxCount != 4) return;
  g_diagnosticFontFrame=current_frame();
  std::ifstream file(path,std::ios::binary);
  if (!file) throw std::runtime_error("Missing diagnostic vertex shader source");
  std::string source{std::istreambuf_iterator<char>(file),{}};
  const auto fragment = source.find("@fragment");
  if (fragment == std::string::npos) throw std::runtime_error("Diagnostic shader has no fragment boundary");
  source.resize(fragment);
  const std::string entry = "@vertex\nfn vs_main(";
  auto pos = source.find(entry);
  if (pos == std::string::npos) throw std::runtime_error("Diagnostic shader vertex entry mismatch");
  source.replace(pos,entry.size(),"fn diagnostic_vertex(");
  const std::string vertexIndex = "@builtin(vertex_index) vidx: u32";
  pos=source.find(vertexIndex);
  if (pos == std::string::npos) throw std::runtime_error("Diagnostic shader vertex input mismatch");
  source.replace(pos,vertexIndex.size(),"vidx: u32");
  source += R"(
@group(3) @binding(0) var<storage,read_write> diagnostic_output: array<vec4f>;
@compute @workgroup_size(4)
fn diagnostic_main(@builtin(global_invocation_id) id: vec3u) {
  let v = diagnostic_vertex(id.x);
  diagnostic_output[id.x*2u] = v.pos;
  diagnostic_output[id.x*2u+1u] = vec4f(v.tex0_uv,0.0,0.0);
}
)";
  wgpu::ShaderSourceWGSL wgsl;wgsl.code=source.c_str();
  const wgpu::ShaderModuleDescriptor moduleDesc{.nextInChain=&wgsl,.label="Original font vertex diagnostic"};
  auto module=g_device.CreateShaderModule(&moduleDesc);
  const wgpu::ComputePipelineDescriptor pipelineDesc{.label="Original font vertex diagnostic",.compute={.module=module,.entryPoint="diagnostic_main"}};
  auto pipeline=g_device.CreateComputePipeline(&pipelineDesc);
  const wgpu::BufferDescriptor resultDesc{.label="Original font transformed vertices",.usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopySrc,.size=128};
  g_diagnosticVertexResult=g_device.CreateBuffer(&resultDesc);
  const wgpu::BindGroupEntry vertexEntry{.binding=0,.buffer=g_vertexBuffer};
  const wgpu::BindGroupDescriptor vertexDesc{.layout=pipeline.GetBindGroupLayout(0),.entryCount=1,.entries=&vertexEntry};
  auto vertexGroup=g_device.CreateBindGroup(&vertexDesc);
  const wgpu::BindGroupEntry uniformEntry{.binding=0,.buffer=g_uniformBuffer,.offset=draw.uniformRange.offset,.size=draw.uniformRange.size};
  const wgpu::BindGroupDescriptor uniformDesc{.layout=pipeline.GetBindGroupLayout(1),.entryCount=1,.entries=&uniformEntry};
  auto uniformGroup=g_device.CreateBindGroup(&uniformDesc);
  const wgpu::BindGroupEntry outputEntry{.binding=0,.buffer=g_diagnosticVertexResult,.size=128};
  const wgpu::BindGroupDescriptor outputDesc{.layout=pipeline.GetBindGroupLayout(3),.entryCount=1,.entries=&outputEntry};
  auto outputGroup=g_device.CreateBindGroup(&outputDesc);
  auto compute=encoder.BeginComputePass();compute.SetPipeline(pipeline);
  compute.SetBindGroup(0,vertexGroup);compute.SetBindGroup(1,uniformGroup);compute.SetBindGroup(3,outputGroup);
  compute.DispatchWorkgroups(1);compute.End();
  if (AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_CAPTURE_ISOLATED_GLYPH")) raster_diagnostic_glyph(encoder,draw,true);
  Log.info("[prime-gpu-transformed-vertices] source={} frame={} vertex_range={}/{} uniform_range={}/{}",
           path,current_frame(),draw.vertRange.offset,draw.vertRange.size,draw.uniformRange.offset,draw.uniformRange.size);
}

static void render_pass_impl(const wgpu::RenderPassEncoder& pass, const std::vector<RenderPass>& passes, u32 idx,
                             int32_t interpolatedFrame, DebugFrameData& debugFrame, size_t commandLimit=SIZE_MAX,
                             size_t commandBegin=0);

struct RenderOptions {
  // CMPR decodes write their textures once per presentation group: the first image runs them.
  bool runDecodes = true;
  // The passes hold exactly one game frame, from its start to its display copy, so every image
  // (inserted ones included) draws it whole, with its own SSAO and scene effects.
  bool selfContained = false;
  // The list ends at a display copy, its tail carried to the next batch: no world block stays open.
  bool endsAtDisplayCopy = false;
  // Buffer set left bound afterwards, for work encoded after the frame (EFB readbacks).
  uint8_t ownBufferSet = kUntaggedBufferSet;
};

static void render_impl(std::vector<RenderPass>& renderPasses, wgpu::CommandEncoder& cmd, int32_t interpolatedFrame,
                        bool finalize, DebugFrameData& debugFrame, const depth_peek::FrameMapping& depthMapping,
                        const RenderOptions& options) {
  ZoneScoped;
  // Diagnostics, draw dumps and the SSAO depth carried between batches belong to the native render.
  const bool encodeTextureBakes = interpolatedFrame < 0;
  // Scene effects (SSAO, floor reflections, light shafts) need the frame's whole world. An inserted
  // image of a self-contained frame draws all of it, so it gets them too; without that every other
  // image at 120 Hz lacked them, which flickers.
  const bool sceneEffects = encodeTextureBakes || options.selfContained;
  // EFB copies that no draw follows still run for every image of a self-contained frame: what they
  // copy differs per image.
  const bool encodeAllResolves = encodeTextureBakes || options.selfContained;
  // SSAO depth accumulation.
  // A repeated presentation has no new EFB draws: keep the previous frame's accumulated depth.
  bool efbHasDraws = false;
  for (const auto& candidate : renderPasses) {
    if (candidate.depthView.Get() == webgpu::g_depthBuffer.view.Get() && !candidate.commands.empty()) { efbHasDraws = true; break; }
  }
  const bool accumulateDepth = sceneEffects && efbHasDraws && post::wants_depth();
  // A self-contained frame never carries a world block in or out, whichever image draws it.
  if (options.selfContained) post::set_depth_carry(false, 0);
  if (accumulateDepth) post::begin_depth(cmd, webgpu::g_depthBuffer);
  const RenderPass* ssaoPrevPass = nullptr;
  // Scene points: the end of every block of EFB passes drawn with the world camera (tagged per pass
  // by post::note_draw). There the colour is snapshotted (bloom source) and SSAO is applied straight
  // into the EFB, before the visor, HUD and messages are drawn. One render call can hold two game
  // frames (two world blocks); applying only after the last one left every other frame without
  // occlusion (flicker), so each block gets its own application.
  // sceneEnd: 1 = the main world block ends after this pass (snapshot + SSAO), 2 = a small block with
  // the world camera ends here (the game's effect passes, 1-6 draws into parts of the EFB): only its
  // depth is discarded. SSAO on those small blocks produced a screen quarter that looked different
  // (a cross through the centre) and flicker.
  constexpr uint32_t kMinWorldBlockDraws = 24;
  std::vector<uint8_t> sceneEnd(renderPasses.size(), 0);
  // Passes whose depth feeds SSAO: EFB passes (main depth buffer) that drew with the world camera.
  // Merging every pass let off-screen renders (characters drawn into textures in cutscenes) and other
  // passes leak their depth, which showed up as dark silhouettes on walls where nobody stands.
  std::vector<uint8_t> worldPass(renderPasses.size(), 0);
  uint8_t sceneStart = 0;  // same codes, for a block carried from the previous batch that ends at once
  const uint32_t carriedDraws = encodeTextureBakes ? post::carried_world_draws() : 0;
  if (encodeTextureBakes) post::set_depth_carry(false, 0);
  // Passes the pending SSAO depth merge may be deferred past (see the merge loop below).
  std::vector<uint8_t> mergeDeferrable(renderPasses.size(), 0);
  // World passes of a small block (sceneEnd 2): its depth is discarded at the block end, so merging
  // it is wasted work. Rundas' fight has a dozen such blocks per frame (12-25 merges).
  std::vector<uint8_t> mergeDiscarded(renderPasses.size(), 0);
  if (sceneEffects && post::wants_scene_color()) {
    std::array<float, 4> world{};
    if (post::world_camera(world)) {
      for (size_t k = 0; k < renderPasses.size(); ++k) {
        const auto& rp = renderPasses[k];
        if (rp.depthView.Get() != webgpu::g_depthBuffer.view.Get()) {
          mergeDeferrable[k] = 1;  // its own depth target: the EFB depth is untouched
          continue;
        }
        bool onlyWorldNearer = !rp.clearDepth && !rp.depthWritesForeign;
        for (uint8_t c = 0; c < rp.postCameraCount && onlyWorldNearer; ++c)
          if (rp.postCameraDepthWrites[c] != 0 && rp.postCameras[c] != world) onlyWorldNearer = false;
        mergeDeferrable[k] = onlyWorldNearer ? 1 : 0;
      }
      size_t lastWorld = SIZE_MAX;
      uint32_t blockDraws = carriedDraws;
      bool firstEfb = true;
      size_t blockFirst = 0;
      for (size_t k = 0; k < renderPasses.size(); ++k) {
        const auto& rp = renderPasses[k];
        if (rp.colorView.Get() != webgpu::g_frameBuffer.view.Get()) continue;
        uint32_t worldDraws = 0;
        for (uint8_t c = 0; c < rp.postCameraCount; ++c)
          if (rp.postCameras[c] == world) worldDraws += rp.postCameraDraws[c];
        if (worldDraws > 0 && rp.depthView.Get() == webgpu::g_depthBuffer.view.Get()) worldPass[k] = 1;
        // A display copy ends a game frame (startsGameFrame marks the pass after it): a world block
        // never continues across it. Counting on let the head of the next frame add its draws to the
        // block of the one before: the title screen (about 17 world draws, a small block) crossed the
        // threshold only when the batch was cut between its two parts, and got the scene effects
        // (exposure) on those frames alone: the title and the movies flickered brighter.
        if (rp.startsGameFrame) {
          if (lastWorld != SIZE_MAX) {
            sceneEnd[lastWorld] = blockDraws >= kMinWorldBlockDraws ? 1 : 2;
            if (sceneEnd[lastWorld] == 2)
              for (size_t m = blockFirst; m <= lastWorld; ++m) mergeDiscarded[m] = 1;
            lastWorld = SIZE_MAX;
          } else if (firstEfb && carriedDraws > 0) {
            sceneStart = carriedDraws >= kMinWorldBlockDraws ? 1 : 2;
          }
          blockDraws = 0;
        }
        if (worldDraws > 0) {
          if (lastWorld == SIZE_MAX) blockFirst = k;
          lastWorld = k;
          blockDraws += worldDraws;
        } else if (lastWorld != SIZE_MAX) {
          sceneEnd[lastWorld] = blockDraws >= kMinWorldBlockDraws ? 1 : 2;
          if (sceneEnd[lastWorld] == 2)
            for (size_t m = blockFirst; m <= lastWorld; ++m) mergeDiscarded[m] = 1;
          lastWorld = SIZE_MAX;
          blockDraws = 0;
        } else if (firstEfb && carriedDraws > 0) {
          // The block carried from the previous batch ended with that batch.
          sceneStart = carriedDraws >= kMinWorldBlockDraws ? 1 : 2;
          blockDraws = 0;
        }
        firstEfb = false;
      }
      // A world block still open at the end of this batch continues in the next one (the game's
      // frame straddles batches at a varying point): no SSAO yet, and the depth is carried over.
      // Applying it to each half separately made the result change every frame (flicker) and left
      // visible boundaries between the halves.
      // A list cut at its display copy closes the block there instead: the passes after the copy
      // belong to the next game frame and render at the start of the next batch.
      if (lastWorld != SIZE_MAX && options.endsAtDisplayCopy) {
        sceneEnd[lastWorld] = blockDraws >= kMinWorldBlockDraws ? 1 : 2;
        if (sceneEnd[lastWorld] == 2)
          for (size_t m = blockFirst; m <= lastWorld; ++m) mergeDiscarded[m] = 1;
      } else if (lastWorld != SIZE_MAX && encodeTextureBakes) {
        post::set_depth_carry(true, blockDraws);
      }
      // One application per game frame. Rundas' fight draws its world in two to four full-size blocks
      // per frame (effect passes and EFB copies in between) and the EFB depth persists across them,
      // so every application recomputed SSAO and floor reflections over the whole screen and darkened
      // and reflected what earlier ones had already treated (post-process up to 9.5 ms of GPU per
      // frame, [gpu-time] 2026-11-01). Earlier full-size blocks of a frame now only accumulate their
      // depth (action 3); the last one of the frame applies, still before its own EFB copy (#16).
      // A frame ends at its display copy (startsGameFrame); a block still open at the end of the
      // batch is not counted, so the last closed one keeps applying as before in that case.
      size_t lastFull = SIZE_MAX;
      bool startPending = sceneStart == 1;
      for (size_t k = 0; k < renderPasses.size(); ++k) {
        if (renderPasses[k].startsGameFrame && renderPasses[k].colorView.Get() == webgpu::g_frameBuffer.view.Get()) {
          lastFull = SIZE_MAX;  // a new game frame: the previous one's last block stays an application
          startPending = false;
        }
        if (sceneEnd[k] != 1) continue;
        if (lastFull != SIZE_MAX) sceneEnd[lastFull] = 3;
        if (startPending) sceneStart = 3;
        startPending = false;
        lastFull = k;
      }
    }
  }
  static const bool sceneDebug = post::debug_depth_requested();
  if (sceneDebug && encodeTextureBakes && current_frame() % 30 == 0) {
    std::string line = "[post-scene] frame=" + std::to_string(current_frame()) + " efb:";
    for (size_t k = 0; k < renderPasses.size(); ++k)
      if (renderPasses[k].colorView.Get() == webgpu::g_frameBuffer.view.Get())
        line += " " + std::to_string(k) + "=" + std::to_string(renderPasses[k].commands.size()) + (sceneEnd[k] == 1 ? "*" : sceneEnd[k] == 2 ? "-" : "");
    line += " carried=" + std::to_string(carriedDraws) + " start=" + std::to_string(sceneStart);
    std::fprintf(stderr, "%s\n", line.c_str());
  }
  // The SSAO depth merge folds the EFB depth into the accumulation as max(previous, this depth). When
  // no draw could have written the EFB depth since the last merge, merging again gives the same
  // result: skip it. Effect-heavy frames (Rundas' ice, hypermode) split the world into dozens of
  // passes at the game's EFB copies, mostly particles without z-update; one full-screen merge per
  // pass (24 per frame) made those fights GPU-bound. A reset of the accumulation, a face run waiting
  // to be finalized and any depth-writing draw on the EFB depth mark it dirty again.
  //
  // Deferral: a world pass' depth need not be merged when the next pass starts if that pass cannot
  // lose any of it, i.e. it does not clear the EFB depth and its only depth writes are world-camera
  // draws with a LESS / LEQUAL / EQUAL test (the depth there only gets nearer; merge is a max of
  // 1/distance, monotonic in depth inside the world band). Merging once after the last of such a run
  // gives the same accumulation as merging after every pass. Rundas' fight wrote depth between
  // its 24 copies per frame, so the skip above alone never triggered there.
  bool efbDepthDirty = true;
  size_t ssaoMergesSkipped = 0, ssaoMergesDeferred = 0;
  size_t ssaoMergesDiscarded = 0, ssaoApplications = 0, ssaoSmallBlocks = 0, ssaoAccumulatedBlocks = 0;
  // A small block's passes are not merged (mergeDiscarded), so its end leaves the accumulation as it
  // was, keeping what earlier full-size blocks of the frame gathered; only a merge forced by a face
  // run puts its depth in, and then the accumulation is reset as before.
  bool smallBlockMerged = false;
  // A game frame ends with its display copy (GXCopyDisp, note_display_copy). The EFB clears were no
  // use as a marker: the frame-start clear is a scissored clear draw like the effect passes' ones. A second full-size world block of the same frame
  // (Rundas' fight: two per frame) gets SSAO and floor reflections for its own depth, but the light
  // shafts and the area colours, computed from the whole picture, were added / measured again:
  // shafts doubled on everything drawn before. Only the first application after a clear does them.
  bool firstInGameFrame = true;
  size_t ssaoFrameStarts = 0;
  const auto mergeWorldDepth = [&](const RenderPass& pass) -> bool {
    const size_t index = static_cast<size_t>(&pass - renderPasses.data());
    if (index < mergeDiscarded.size() && mergeDiscarded[index] && !post::face_depth_pending()) {
      ++ssaoMergesDiscarded;  // reset at its block end without being used
      return false;
    }
    if (!efbDepthDirty && !post::face_depth_pending()) {
      ++ssaoMergesSkipped;
      return false;
    }
    post::merge_depth(cmd, pass.depthTexture, pass.depthView, pass.depthSize);
    if (index < mergeDiscarded.size() && mergeDiscarded[index]) smallBlockMerged = true;
    efbDepthDirty = false;
    return true;
  };
  size_t ssaoApplied = 0;  // applications that actually ran (apply_ssao_in_frame returned true)
  const auto sceneAction = [&](uint8_t action) {
    if (action == 1) {
      post::capture_scene_color(cmd);
      // The area colour readback is once per presented frame: the native render takes it.
      if (post::apply_ssao_in_frame(cmd, firstInGameFrame, encodeTextureBakes)) ++ssaoApplied;
      firstInGameFrame = false;
      ++ssaoApplications;
    } else if (action == 2) {
      ++ssaoSmallBlocks;
    } else if (action == 3) {
      ++ssaoAccumulatedBlocks;  // a later block of this game frame applies with this depth too
      return;
    }
    if (action == 1 || (action == 2 && smallBlockMerged)) {
      post::reset_depth(cmd);  // the next block starts from an empty accumulation
      efbDepthDirty = true;     // the depth still in the EFB must be folded into it again
      smallBlockMerged = false;
    }
  };
  const auto sceneAfter = [&](size_t pass) {
    if (pass < sceneEnd.size()) sceneAction(sceneEnd[pass]);
  };
  sceneAction(sceneStart);
  size_t ssaoMerges = 0, ssaoEfbPasses = 0, ssaoOtherPasses = 0, ssaoEfbMax = 0, ssaoOtherMax = 0;
  uint32_t ssaoOtherW = 0, ssaoOtherH = 0;
  for (u32 i = 0; i < renderPasses.size(); ++i) {
    const auto& passInfo = renderPasses[i];
    // Carried passes read the previous upload's buffers; everything below binds through the globals.
    bind_buffer_set(passInfo.bufferSet);
    if (passInfo.colorView.Get() == webgpu::g_frameBuffer.view.Get() && passInfo.startsGameFrame) {
      firstInGameFrame = true;
      ++ssaoFrameStarts;
    }
    // Fold the previous world pass' depth into the SSAO accumulation before it is cleared or left
    // (only world passes are kept in ssaoPrevPass; the merge also keeps only the world camera's band).
    bool keepPending = false;
    if (accumulateDepth && ssaoPrevPass != nullptr && ssaoPrevPass->depthTexture) {
      if (mergeDeferrable[i] && !post::face_depth_pending()) {
        keepPending = true;  // every world pass shares the EFB depth: merged later, once
        ++ssaoMergesDeferred;
      } else if (mergeWorldDepth(*ssaoPrevPass)) {
        ++ssaoMerges;
      }
    }
    if (accumulateDepth) {
      ssaoPrevPass = worldPass[i] ? &passInfo : keepPending ? ssaoPrevPass : nullptr;
      if (passInfo.depthView.Get() == webgpu::g_depthBuffer.view.Get()) { ++ssaoEfbPasses; ssaoEfbMax = std::max(ssaoEfbMax, passInfo.commands.size()); }
      else { ++ssaoOtherPasses; ssaoOtherMax = std::max(ssaoOtherMax, passInfo.commands.size()); ssaoOtherW = passInfo.depthSize.width; ssaoOtherH = passInfo.depthSize.height; }
    }
    // Decodes fill a texture from CPU data once, before the first image of the group samples it.
    // Palette conversions run for every image: their source can be an EFB copy this image just made.
    if (options.runDecodes) {
      for (const auto& decode : passInfo.cmprDecodes) {
        tex_cmpr_decode::run(cmd, decode);
      }
    }
    for (const auto& conv : passInfo.paletteConvs) {
      tex_palette_conv::run(cmd, conv);
    }
    const bool hasRenderWork = passInfo.clearColor || passInfo.clearDepth || !passInfo.commands.empty() ||
                               !passInfo.cmprDecodes.empty();
    if (i == renderPasses.size() - 1) {
      // A list cut at its display copy ends with that copy's pass; the next frame's passes follow
      // in the next batch.
      ASSERT(!passInfo.resolveTarget || options.endsAtDisplayCopy, "Final render pass must not have resolve target");
    } else if (!(passInfo.resolveTarget && encodeAllResolves) && !hasRenderWork) {
      // Skip only empty intermediate passes: offscreen and scratch passes with resolves still have to
      // run for later samplers, and on a replay slot a resolve-only pass has nothing to encode.
      continue;
    }

    // SSAO wants the depth of the 3D world, which the game clears before drawing the HUD and
    // again at the display copy: snapshot it right after the frame's heaviest pass ends.
    const std::array attachments{
        wgpu::RenderPassColorAttachment{
            .view = passInfo.colorView,
            .resolveTarget = passInfo.resolveView,
            .loadOp = passInfo.clearColor ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load,
            .storeOp = wgpu::StoreOp::Store,
            .clearValue =
                {
                    .r = passInfo.clearColorValue.x(),
                    .g = passInfo.clearColorValue.y(),
                    .b = passInfo.clearColorValue.z(),
                    .a = passInfo.clearColorValue.w(),
                },
        },
    };
    const wgpu::RenderPassDepthStencilAttachment depthStencilAttachment{
        .view = passInfo.depthView,
        .depthLoadOp = passInfo.clearDepth ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load,
        .depthStoreOp = wgpu::StoreOp::Store,
        .depthClearValue = passInfo.clearDepthValue,
    };
    const wgpu::RenderPassDescriptor renderPassDescriptor{
        .label = render_pass_label(i),
        .colorAttachmentCount = attachments.size(),
        .colorAttachments = attachments.data(),
        .depthStencilAttachment = &depthStencilAttachment,
    };

    static unsigned primePassTrace = 0;
    if (CachedWiiCompiledTracePrimeGx() && primePassTrace++ < 64)
      Log.info("[prime-gpu-pass] index={} commands={} target={}x{} clear={}/{} resolve={} rect={},{},{},{} snapshot={}",
               i,passInfo.commands.size(),passInfo.targetSize.width,passInfo.targetSize.height,
               passInfo.clearColor,passInfo.clearDepth,static_cast<bool>(passInfo.resolveTarget),
               passInfo.resolveRect.x,passInfo.resolveRect.y,passInfo.resolveRect.width,passInfo.resolveRect.height,
               passInfo.snapshotColorResolveSource);
    if (AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_VERTEX_SHADER_PATH")) {
      for (const auto& command : passInfo.commands)
        if (command.type==CommandType::Draw && command.data.draw.type==ShaderType::GX)
          encode_diagnostic_vertices(cmd,command.data.draw.gx);
    }
    static bool diagnosticPassCaptured = false;
    if (!diagnosticPassCaptured && AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_CAPTURE_ISOLATED_PASS") &&
        g_diagnosticFontFrame==current_frame() && g_diagnosticIsolatedGlyph) {
      diagnosticPassCaptured=true;
      const auto size=passInfo.targetSize;
      const wgpu::TextureDescriptor depthDesc{.label="Isolated original pass depth",.usage=wgpu::TextureUsage::RenderAttachment,
          .size=size,.format=webgpu::g_graphicsConfig.depthFormat,.sampleCount=passInfo.msaaSamples};
      auto depth=g_device.CreateTexture(&depthDesc);auto depthView=depth.CreateView();
      wgpu::Texture color=g_diagnosticIsolatedGlyph->texture;
      auto colorView=g_diagnosticIsolatedGlyph->attachmentTextureView;
      wgpu::TextureView resolveView;
      if(passInfo.msaaSamples>1) {
        const wgpu::TextureDescriptor colorDesc{.label="Isolated original pass MSAA",.usage=wgpu::TextureUsage::RenderAttachment,
            .size=size,.format=g_diagnosticIsolatedGlyph->format,.sampleCount=passInfo.msaaSamples};
        color=g_device.CreateTexture(&colorDesc);colorView=color.CreateView();resolveView=g_diagnosticIsolatedGlyph->attachmentTextureView;
      }
      const wgpu::RenderPassColorAttachment colorAttachment{.view=colorView,.resolveTarget=resolveView,
          .loadOp=wgpu::LoadOp::Clear,.storeOp=wgpu::StoreOp::Store,.clearValue={0,0,0,1}};
      const wgpu::RenderPassDepthStencilAttachment depthAttachment{.view=depthView,
          .depthLoadOp=wgpu::LoadOp::Clear,.depthStoreOp=wgpu::StoreOp::Store,.depthClearValue=passInfo.clearDepthValue};
      const wgpu::RenderPassDescriptor desc{.label="Isolated complete original pass",.colorAttachmentCount=1,
          .colorAttachments=&colorAttachment,.depthStencilAttachment=&depthAttachment};
      auto isolatedPass=cmd.BeginRenderPass(&desc);
      const char* prefix=AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_ISOLATED_PASS_PREFIX");
      const size_t commandLimit=prefix ? std::strtoul(prefix,nullptr,10) : SIZE_MAX;
      for(size_t j=0;j<passInfo.commands.size();++j) {
        const auto& c=passInfo.commands[j];
        if(c.type==CommandType::Draw && c.data.draw.type==ShaderType::GX) {
          const auto& d=c.data.draw.gx;
          Log.info("[prime-isolated-pass-command] i={} type=gx pipeline={:016X} vertices={} indices={} v={}/{} u={}/{}",j,d.pipeline,d.vtxCount,d.indexCount,d.vertRange.offset,d.vertRange.size,d.uniformRange.offset,d.uniformRange.size);
        } else if(c.type==CommandType::SetViewport) {
          const auto& v=c.data.setViewport;
          Log.info("[prime-isolated-pass-command] i={} type=viewport values={},{},{},{},{},{}",j,v.left,v.top,v.width,v.height,v.znear,v.zfar);
        } else if(c.type==CommandType::SetScissor) {
          const auto& v=c.data.setScissor;
          Log.info("[prime-isolated-pass-command] i={} type=scissor values={},{},{},{}",j,v.x,v.y,v.width,v.height);
        } else Log.info("[prime-isolated-pass-command] i={} type={}",j,underlying(c.type));
      }
      render_pass_impl(isolatedPass,renderPasses,i,interpolatedFrame,debugFrame,commandLimit);
      isolatedPass.End();
      Log.info("[prime-isolated-complete-pass] frame={} pass={} commands={}",current_frame(),i,passInfo.commands.size());
    }
    // Runs of draws that sample a texture of the SSAO exclusion list (Samus' face): the pass is
    // encoded in segments so the depth before and after each run can be compared (post::face_*).
    // Each run is of one kind: depth (opaque face) or cover (transparent visor glass, by colour).
    // Effect runs (fog, explosions, particles: post::ssao_effects_active) are cover runs measured with a
    // graduated mask instead.
    struct ExcludeRun { size_t begin, end; bool cover; bool graduated; };
    std::vector<ExcludeRun> excludeRuns;
    if (accumulateDepth && worldPass[i] && passInfo.msaaSamples == 1 && passInfo.depthView &&
        (post::ssao_exclude_active() || post::ssao_effects_active())) {
      int open = 0;  // 0 none, 1 depth run, 2 cover run, 3 effect run
      for (size_t j = 0; j < passInfo.commands.size(); ++j) {
        const auto& c = passInfo.commands[j];
        if (c.type != CommandType::Draw) continue;  // viewport / scissor changes do not close a run
        const bool gx = c.data.draw.type == ShaderType::GX;
        const int kind = gx && c.data.draw.gx.ssaoExclude ? 1
                         : gx && c.data.draw.gx.ssaoCover ? 2
                         : gx && c.data.draw.gx.ssaoEffect ? 3
                                                           : 0;
        if (kind != 0 && kind == open) excludeRuns.back().end = j + 1;
        else if (kind != 0) excludeRuns.push_back({j, j + 1, kind >= 2, kind == 3});
        open = kind;
      }
    }
    if (excludeRuns.empty()) {
      wgpu::RenderPassDescriptor timedDescriptor = renderPassDescriptor;
      timedDescriptor.timestampWrites = gpu_timing::pass_writes(gpu_timing::Category::Game);
      auto pass = cmd.BeginRenderPass(&timedDescriptor);
      render_pass_impl(pass, renderPasses, i, interpolatedFrame, debugFrame);
      pass.End();
    } else {
      size_t at = 0;
      bool firstSegment = true;
      const auto segment = [&](size_t end) {
        std::array segmentAttachments = attachments;
        wgpu::RenderPassDepthStencilAttachment segmentDepth = depthStencilAttachment;
        if (!firstSegment) {  // later segments continue the same pass: keep colour and depth
          segmentAttachments[0].loadOp = wgpu::LoadOp::Load;
          segmentDepth.depthLoadOp = wgpu::LoadOp::Load;
        }
        wgpu::RenderPassDescriptor descriptor = renderPassDescriptor;
        descriptor.colorAttachments = segmentAttachments.data();
        descriptor.depthStencilAttachment = &segmentDepth;
        descriptor.timestampWrites = gpu_timing::pass_writes(gpu_timing::Category::Game);
        auto pass = cmd.BeginRenderPass(&descriptor);
        render_pass_impl(pass, renderPasses, i, interpolatedFrame, debugFrame, end, at);
        pass.End();
        at = end;
        firstSegment = false;
      };
      for (const auto& run : excludeRuns) {
        if (run.begin > at || firstSegment) segment(run.begin);  // the first one also performs the pass' clears
        post::face_before(cmd, passInfo.depthView, passInfo.depthSize, run.cover, run.graduated);
        segment(run.end);
        post::face_after(cmd, passInfo.depthView, passInfo.depthSize, run.cover, run.graduated);
      }
      segment(passInfo.commands.size());
    }
    if (passInfo.depthWrites && passInfo.depthView.Get() == webgpu::g_depthBuffer.view.Get()) efbDepthDirty = true;

    // End of a world block: fold this pass' depth in and apply SSAO *before* this pass' EFB copy.
    // The game copies the whole EFB right after the world (for its own glow), draws an effect into the
    // top-left quarter and restores that quarter from the copy; with SSAO applied after the copy, that
    // quarter came back without occlusion (a cross through the screen centre, flickering).
    if (i < sceneEnd.size() && sceneEnd[i] != 0) {
      if (accumulateDepth && passInfo.depthTexture && worldPass[i]) {
        if (mergeWorldDepth(passInfo)) ++ssaoMerges;
      }
      sceneAction(sceneEnd[i]);
      sceneEnd[i] = 0;
      if (accumulateDepth) ssaoPrevPass = nullptr;  // already merged; the accumulation was reset
    }


    // The native image's depth, whichever image of the group is encoded last.
    if (encodeTextureBakes && i == renderPasses.size() - 1) {
      depth_peek::encode_frame_snapshot(cmd, passInfo.copySourceDepthView, passInfo.targetSize, passInfo.msaaSamples, depthMapping);
    }

    if (passInfo.resolveTarget) {
      const bool isDepth = gx::is_depth_format(passInfo.resolveFormat);
      wgpu::Texture resolveSourceTexture = passInfo.copySourceTexture;
      wgpu::TextureView resolveSourceView = isDepth ? passInfo.copySourceDepthView : passInfo.copySourceView;
      if (passInfo.snapshotColorResolveSource && passInfo.resolveSourceSnapshot) {
        const wgpu::TexelCopyTextureInfo src{
            .texture = passInfo.copySourceTexture,
            .origin = wgpu::Origin3D{
                .x = static_cast<uint32_t>(passInfo.resolveSnapshotRect.x),
                .y = static_cast<uint32_t>(passInfo.resolveSnapshotRect.y),
            },
        };
        const wgpu::TexelCopyTextureInfo dst{
            .texture = passInfo.resolveSourceSnapshot->texture,
        };
        const wgpu::Extent3D size{
            .width = static_cast<uint32_t>(passInfo.resolveSnapshotRect.width),
            .height = static_cast<uint32_t>(passInfo.resolveSnapshotRect.height),
            .depthOrArrayLayers = 1,
        };
        cmd.CopyTextureToTexture(&src, &dst, &size);
        resolveSourceTexture = passInfo.resolveSourceSnapshot->texture;
        resolveSourceView = passInfo.resolveSourceSnapshot->sampleTextureView;
      }
      const tex_copy_conv::ConvRequest convReq{
          .fmt = passInfo.resolveFormat,
          .srcView = resolveSourceView,
          .uniformRange = passInfo.resolveUniformRange,
          .dst = passInfo.resolveTarget,
          .sampleFilter = passInfo.resolveLinearSampling
                              ? tex_copy_conv::SampleFilter::Linear
                              : tex_copy_conv::SampleFilter::Nearest,
          .forceOpaqueAlpha = passInfo.resolveForceOpaqueAlpha,
          .multisampledDepthSource = isDepth && passInfo.msaaSamples > 1,
      };
      if (passInfo.resolveNeedsConversion) {
        tex_copy_conv::run(cmd, convReq);
      } else if (passInfo.resolveNeedsShaderSampling) {
        tex_copy_conv::blit(cmd, convReq);
      } else {
        const wgpu::TexelCopyTextureInfo src{
            .texture = resolveSourceTexture,
            .origin =
                wgpu::Origin3D{
                    .x = static_cast<uint32_t>(passInfo.resolveRect.x -
                                               (passInfo.snapshotColorResolveSource
                                                    ? passInfo.resolveSnapshotRect.x : 0)),
                    .y = static_cast<uint32_t>(passInfo.resolveRect.y -
                                               (passInfo.snapshotColorResolveSource
                                                    ? passInfo.resolveSnapshotRect.y : 0)),
                },
        };
        const wgpu::TexelCopyTextureInfo dst{
            .texture = passInfo.resolveTarget->texture,
        };
        const wgpu::Extent3D size{
            .width = static_cast<uint32_t>(passInfo.resolveRect.width),
            .height = static_cast<uint32_t>(passInfo.resolveRect.height),
            .depthOrArrayLayers = 1,
        };
        cmd.CopyTextureToTexture(&src, &dst, &size);
      }
      if (passInfo.resolveSoftenX > 0.0f && passInfo.resolveTarget)
        post::soften_copy(cmd, *passInfo.resolveTarget, passInfo.resolveSoftenX, passInfo.resolveSoftenY);
      if (!passInfo.resolveDumpPath.empty() && encodeTextureBakes) {
        const auto& target = *passInfo.resolveTarget;
        const bool bgra = target.format == wgpu::TextureFormat::BGRA8Unorm;
        if (bgra || target.format == wgpu::TextureFormat::RGBA8Unorm) {
          const uint32_t bytesPerRow = (target.size.width * 4 + 255) & ~255u;
          const wgpu::BufferDescriptor desc{.label = "Draw dump copy readback",
              .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
              .size = uint64_t{bytesPerRow} * target.size.height};
          draw_dump::CopyReadback readback{.buffer = g_device.CreateBuffer(&desc), .width = target.size.width,
              .height = target.size.height, .bytesPerRow = bytesPerRow, .bgra = bgra, .path = passInfo.resolveDumpPath};
          const wgpu::TexelCopyTextureInfo source{.texture = target.texture};
          const wgpu::TexelCopyBufferInfo destination{
              .layout = {.offset = 0, .bytesPerRow = bytesPerRow, .rowsPerImage = target.size.height},
              .buffer = readback.buffer};
          const wgpu::Extent3D extent{target.size.width, target.size.height, 1};
          cmd.CopyTextureToBuffer(&source, &destination, &extent);
          draw_dump::g_copyReadbacks.push_back(std::move(readback));
        } else {
          Log.warn("[draw-dump] copy {} has format {}; not read back", passInfo.resolveDumpPath,
                   magic_enum::enum_name(target.format));
        }
      }
    }
  }
  if (accumulateDepth && ssaoPrevPass != nullptr && ssaoPrevPass->depthTexture) {
    if (mergeWorldDepth(*ssaoPrevPass)) ++ssaoMerges;
  }
  if (!renderPasses.empty()) sceneAfter(renderPasses.size() - 1);
  bind_buffer_set(options.ownBufferSet);
#if defined(MKW_PRIME_QA_TOOLS)
  // During an F5 burst: every frame's block layout and post-process outcome, to line up with the
  // captured images (pass index = draws.txt pass; * apply, - small, + accumulate, ^ display copy).
  if (encodeTextureBakes && draw_dump::g_armedFrame.load(std::memory_order_acquire) != UINT32_MAX) UNLIKELY {
    std::string line = "[post-scene-dump] frame=" + std::to_string(current_frame()) + " base=" +
                       draw_dump::finished_base_path() + " start=" + std::to_string(sceneStart) +
                       " carried=" + std::to_string(carriedDraws) + " efb:";
    for (size_t k = 0; k < renderPasses.size(); ++k) {
      if (renderPasses[k].colorView.Get() != webgpu::g_frameBuffer.view.Get()) continue;
      line += " " + std::to_string(k) + "=" + std::to_string(renderPasses[k].commands.size());
      if (worldPass[k]) line += "w";
      if (renderPasses[k].startsGameFrame) line += "^";
      line += sceneEnd[k] == 1 ? "*" : sceneEnd[k] == 2 ? "-" : sceneEnd[k] == 3 ? "+" : "";
    }
    line += " applications=" + std::to_string(ssaoApplications) + " applied=" + std::to_string(ssaoApplied) +
            " carry_out=" + std::to_string(post::carried_world_draws()) + " last=" + post::last_apply_skip();
    std::fprintf(stderr, "%s\n", line.c_str());
  }
  if (accumulateDepth && encodeTextureBakes) {
    static uint32_t ssaoStatFrames = 0;
    static uint32_t lastHeavyStat = 0;
    // Every 300 frames, and heavy frames (many blocks or merges) at most once a second.
    const bool heavy = renderPasses.size() >= 20 && ssaoStatFrames - lastHeavyStat >= 60;
    if ((++ssaoStatFrames % 300) == 1 || heavy) {
      if (heavy) lastHeavyStat = ssaoStatFrames;
      std::fprintf(stderr, "[post-ssao] frame=%u passes=%zu efb_passes=%zu (max draws %zu) other_passes=%zu (max draws %zu, %ux%u) merges=%zu skipped=%zu deferred=%zu discarded=%zu applications=%zu small_blocks=%zu accumulated_blocks=%zu game_frames=%zu\n",
                   current_frame(), renderPasses.size(), ssaoEfbPasses, ssaoEfbMax, ssaoOtherPasses, ssaoOtherMax, ssaoOtherW, ssaoOtherH, ssaoMerges,
                   ssaoMergesSkipped, ssaoMergesDeferred, ssaoMergesDiscarded, ssaoApplications, ssaoSmallBlocks, ssaoAccumulatedBlocks, ssaoFrameStarts);
    }
  }
#endif
  if (finalize) {
    recycle_render_passes(renderPasses);
  }

#if defined(AURORA_GFX_DEBUG_GROUPS)
  if (finalize && !debugFrame.groups.empty()) {
    for (auto it = debugFrame.groups.rbegin(); it != debugFrame.groups.rend(); ++it) {
      Log.warn("Debug group was not popped at end of frame: {}", *it);
    }
    debugFrame.groups.clear();
  }

  if (finalize && debugFrame.markers.size() > 0) {
    debugFrame.markers.clear();
  }
#endif
}

void seal_frame(SealedFrame& out) noexcept {
  out.data().depthMapping = depth_peek::capture_frame_mapping();
  ZoneScoped;
  // The encode that could still have been holding these has completed: the
  // producer joins the worker's DONE phase before it seals another frame.
  g_retiredBindGroups.clear();
  auto& passes = out.data().passes;
  // The previous cycle already recycled these, so this normally just hands the empty vector, its
  // capacity included, back to the producer.
  recycle_render_passes(passes);
  passes.swap(g_renderPasses);
  auto& data = out.data();
  data.decodesDone = false;
  data.ownBufferSet = g_lastWrittenBufferSet;
  data.startsGameFrame = g_nextBatchStartsGameFrame;
  data.endsAtDisplayCopy = false;
  data.carriedOutDraws = 0;
  // The previous seal's passes after its display copy open this batch's game frame.
  const size_t carriedIn = g_carriedPasses.size();
  if (carriedIn != 0) {
    passes.insert(passes.begin(), std::make_move_iterator(g_carriedPasses.begin()),
                  std::make_move_iterator(g_carriedPasses.end()));
    g_carriedPasses.clear();
  }
  // With frame interpolation every image of a batch must draw one whole game frame: the VI retrace
  // that seals it falls inside the next frame's world, and replaying that head on top of the EFB made
  // the native image mix two frames (objects missing or flickering). The passes after the last display
  // copy move to the next batch, whose images all draw them first. Their data stays in this batch's GPU
  // buffer set, which the next upload does not overwrite.
  static const bool noCarry = AURORA_DIAGNOSTIC_ENV("WII_COMPILED_QA_INTERP_NO_CARRY") != nullptr;
  size_t cut = SIZE_MAX;
  if (!noCarry && gx::frame_interpolation_active() && gx::frame_interpolation_replay_safe()) {
    for (size_t i = passes.size(); i-- > carriedIn;) {
      if (passes[i].startsGameFrame) {
        cut = i;
        break;
      }
    }
  }
  if (cut != SIZE_MAX && cut > 0) {
    ensure_alternate_buffer_set();
    uint32_t draws = 0;
    for (size_t i = cut; i < passes.size(); ++i) {
      for (const auto& command : passes[i].commands) {
        draws += command.type == CommandType::Draw ? 1u : 0u;
      }
    }
    g_carriedPasses.assign(std::make_move_iterator(passes.begin() + static_cast<std::ptrdiff_t>(cut)),
                           std::make_move_iterator(passes.end()));
    passes.erase(passes.begin() + static_cast<std::ptrdiff_t>(cut), passes.end());
    data.endsAtDisplayCopy = true;
    data.carriedOutDraws = draws;
    g_nextBatchStartsGameFrame = true;
  } else {
    g_nextBatchStartsGameFrame = g_boundaryAtSeal && gx::frame_interpolation_replay_safe();
  }
#ifdef AURORA_GFX_DEBUG_GROUPS
  // Marker indices and unmatched-group warnings belong to these detached passes.
  // The next producer frame must not modify strings still read by this encoder.
  auto& debug = out.data().debug;
  debug.groups.clear();
  debug.markers.clear();
  debug.groups.swap(g_debugFrame.groups);
  debug.markers.swap(g_debugFrame.markers);
#endif
  g_currentRenderPass = UINT32_MAX;
}

bool sealed_frame_starts_game_frame(const SealedFrame& frame) noexcept { return frame.data().startsGameFrame; }
bool sealed_frame_ends_at_display_copy(const SealedFrame& frame) noexcept {
  return frame.data().endsAtDisplayCopy;
}
uint32_t sealed_frame_carried_out_draws(const SealedFrame& frame) noexcept { return frame.data().carriedOutDraws; }

void render(SealedFrame& frame, wgpu::CommandEncoder& cmd, int32_t interpolatedFrame, bool finalize) {
  auto& data = frame.data();
  const RenderOptions options{
      .runDecodes = !data.decodesDone,
      .selfContained = data.startsGameFrame && data.endsAtDisplayCopy,
      .endsAtDisplayCopy = data.endsAtDisplayCopy,
      .ownBufferSet = data.ownBufferSet,
  };
  data.decodesDone = true;
  render_impl(data.passes, cmd, interpolatedFrame, finalize, data.debug, data.depthMapping, options);
}

void render(wgpu::CommandEncoder& cmd, int32_t interpolatedFrame, bool finalize) {
  // A prefix submitted mid-batch (EFB readback, staging split) renders natively, so it draws the
  // carried frame start first; the batch's inserted images then copy the native one.
  if (!g_carriedPasses.empty()) {
    g_renderPasses.insert(g_renderPasses.begin(), std::make_move_iterator(g_carriedPasses.begin()),
                          std::make_move_iterator(g_carriedPasses.end()));
    g_carriedPasses.clear();
    g_nextBatchStartsGameFrame = false;
  }
  const RenderOptions options{.runDecodes = true, .ownBufferSet = g_lastWrittenBufferSet};
  render_impl(g_renderPasses, cmd, interpolatedFrame, finalize, g_debugFrame, depth_peek::capture_frame_mapping(),
              options);
  if (finalize) {
    g_currentRenderPass = UINT32_MAX;
    expire_bind_group_cache();
  }
}

void after_submit() noexcept {
  depth_peek::after_submit();
  post::after_submit();
  gpu_timing::after_submit();
  efb_ram::after_submit();
  // Retire this frame's completed GPU work. Dawn only reclaims destroyed resources inside a device
  // tick, and a frame that never ticks keeps every released image and its memory for the run.
  if (g_instance) {
    g_instance.ProcessEvents();
  }
}

static void render_pass_impl(const wgpu::RenderPassEncoder& pass, const std::vector<RenderPass>& renderPasses, u32 idx,
                             int32_t interpolatedFrame, DebugFrameData& debugFrame, size_t commandLimit,
                             size_t commandBegin) {
  static uint32_t firstFontFrame=UINT32_MAX;
  const char* fontDelayText=AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_CAPTURE_FONT_DELAY");
  const uint32_t fontDelay=fontDelayText ? std::min(600u,static_cast<uint32_t>(std::strtoul(fontDelayText,nullptr,10))) : 0;
  // Per-invocation, not per-process: two encoders can be recording at once.
  gx::DrawEncodeState encodeState{};
  encodeState.boundTextureBindGroup = gx::g_emptyTextureBindGroup.Get();
  Viewport diagnosticViewport{0,0,float(renderPasses[idx].targetSize.width),float(renderPasses[idx].targetSize.height),0,1};
  ClipRect diagnosticScissor{0,0,int32_t(renderPasses[idx].targetSize.width),int32_t(renderPasses[idx].targetSize.height)};
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> lastDebugGroupStack;
#endif

  // Bind static bind group for the whole pass
  pass.SetBindGroup(0, g_staticBindGroup);
  pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);

  size_t diagnosticCommandIndex=0;
  for (const auto& cmd : renderPasses[idx].commands) {
    const size_t commandIndex = diagnosticCommandIndex;
    if(diagnosticCommandIndex++>=commandLimit) break;
    // A pass encoded in segments (SSAO exclusion runs) restarts from commandBegin: the earlier
    // viewport and scissor commands are replayed so the new render pass starts with the same state.
    if (commandIndex < commandBegin && cmd.type != CommandType::SetViewport && cmd.type != CommandType::SetScissor)
      continue;
#ifdef AURORA_GFX_DEBUG_GROUPS
    {
      size_t firstDiff = lastDebugGroupStack.size();
      for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
        if (i >= cmd.debugGroupStack.size() || cmd.debugGroupStack[i] != lastDebugGroupStack[i]) {
          firstDiff = i;
          break;
        }
      }
      for (size_t i = firstDiff; i < lastDebugGroupStack.size(); ++i) {
        pass.PopDebugGroup();
      }
      for (size_t i = firstDiff; i < cmd.debugGroupStack.size(); ++i) {
        pass.PushDebugGroup(cmd.debugGroupStack[i].c_str());
      }
      lastDebugGroupStack = cmd.debugGroupStack;
    }
#endif
    switch (cmd.type) {
    case CommandType::SetViewport: {
      const auto& vp = cmd.data.setViewport;
      diagnosticViewport = vp;
      // WebGPU requires 0 <= minDepth <= maxDepth <= 1. vp.znear/vp.zfar are in GX's own distance
      // terms (0 = near); under UseReversedZ the host depth-buffer storage direction is flipped
      // (near = 1, far = 0), so this range has to be remapped through 1-x the same way the
      // projection matrix, depth compare function, and clear value all are - a plain min/max clamp
      // (the previous code here) maps a *restricted* range (e.g. a viewport deliberately narrowed
      // to force something to draw "in front of everything") to the wrong end of the buffer: what
      // should land near the near-storage-extreme (1.0) instead lands near the far-storage-extreme
      // (0.0), so anything else drawn afterward at its true depth wins the compare test and the
      // "in front" geometry silently vanishes. A full [0,1] viewport is unaffected either way,
      // which is why this only broke specific elements, not the whole scene. Matches upstream
      // aurora's apply_viewport (lib/gfx/encoding.cpp) exactly.
      const float minDepth = gx::UseReversedZ ? 1.0f - vp.zfar : vp.znear;
      const float maxDepth = gx::UseReversedZ ? 1.0f - vp.znear : vp.zfar;
      static unsigned primeViewportTrace = 0;
      if (CachedWiiCompiledTracePrimeGx() && primeViewportTrace++ < 128)
        Log.info("[prime-gpu-viewport] values={},{},{},{},{},{}",vp.left,vp.top,vp.width,vp.height,minDepth,maxDepth);
      pass.SetViewport(vp.left, vp.top, vp.width, vp.height, minDepth, maxDepth);
    } break;
    case CommandType::SetScissor: {
      const auto& sc = cmd.data.setScissor;
      diagnosticScissor = sc;
      const auto& size = renderPasses[idx].targetSize;
      const auto left = std::clamp(sc.x, 0, static_cast<int32_t>(size.width));
      const auto top = std::clamp(sc.y, 0, static_cast<int32_t>(size.height));
      const auto right =
          std::clamp(sc.x + sc.width, left, static_cast<int32_t>(size.width));
      const auto bottom =
          std::clamp(sc.y + sc.height, top, static_cast<int32_t>(size.height));
      static unsigned primeScissorTrace = 0;
      if (CachedWiiCompiledTracePrimeGx() && primeScissorTrace++ < 128)
        Log.info("[prime-gpu-scissor] values={},{},{},{}",left,top,right-left,bottom-top);
      pass.SetScissorRect(static_cast<uint32_t>(left), static_cast<uint32_t>(top),
                          static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top));
    } break;
    case CommandType::Draw: {
      const auto& draw = cmd.data.draw;
      switch (draw.type) {
      case ShaderType::GX: {
        const gfx::Range* uniformOverride = nullptr;
        if (interpolatedFrame >= 0 &&
            static_cast<size_t>(interpolatedFrame) < draw.gx.interpolatedUniformRanges.size() &&
            draw.gx.interpolatedUniformRanges[interpolatedFrame].size != 0) {
          uniformOverride = &draw.gx.interpolatedUniformRanges[interpolatedFrame];
        }
        static unsigned fontEncodeTrace = 0;
        if (CachedWiiCompiledTracePrimeGx() && fontEncodeTrace < 48 && draw.gx.vtxCount != 0 &&
            draw.gx.vertRange.size == draw.gx.vtxCount * 20u) {
          ++fontEncodeTrace;
          Log.info("[prime-font-encode] pass={} resolve={} vertices={} indices={} vertex_range={}/{} index_range={}/{} uniform_range={}/{} pipeline={:016X} viewport={},{},{},{} scissor={},{},{},{}",
                   idx,static_cast<bool>(renderPasses[idx].resolveTarget),draw.gx.vtxCount,draw.gx.indexCount,
                   draw.gx.vertRange.offset,draw.gx.vertRange.size,draw.gx.idxRange.offset,draw.gx.idxRange.size,
                   draw.gx.uniformRange.offset,draw.gx.uniformRange.size,draw.gx.pipeline,
                   diagnosticViewport.left,diagnosticViewport.top,diagnosticViewport.width,diagnosticViewport.height,
                   diagnosticScissor.x,diagnosticScissor.y,diagnosticScissor.width,diagnosticScissor.height);
        }
        if (AURORA_DIAGNOSTIC_ENV("WII_COMPILED_GPU_TRACE_RASTER_COVERAGE") && draw.gx.pipeline==0xF892D70D494F0487ull) {
          if(firstFontFrame==UINT32_MAX) firstFontFrame=current_frame();
          if(current_frame()==firstFontFrame+fontDelay)
            Log.info("[prime-raster-coverage-input] frame={} vertices={} v={}/{} u={}/{} i={}/{}",current_frame(),draw.gx.vtxCount,draw.gx.vertRange.offset,draw.gx.vertRange.size,draw.gx.uniformRange.offset,draw.gx.uniformRange.size,draw.gx.idxRange.offset,draw.gx.idxRange.size);
        }
        gx::render(draw.gx, pass, encodeState, renderPasses[idx].requireReadyPipelines, uniformOverride);
      } break;
      case ShaderType::Clear:
        clear::render(draw.clear, pass, renderPasses[idx].targetSize, encodeState.currentPipeline);
        break;
      }
    } break;
    case CommandType::DebugMarker: {
#if defined(AURORA_GFX_DEBUG_GROUPS)
      pass.InsertDebugMarker(wgpu::StringView(debugFrame.markers[cmd.data.debugMarkerIndex]));
#endif
    } break;
    }
  }

#ifdef AURORA_GFX_DEBUG_GROUPS
  for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
    pass.PopDebugGroup();
  }
#endif
}

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass, PipelineRef& currentPipeline,
                   bool requireReady) {
  if (ref == currentPipeline) {
    return true;
  }
  wgpu::RenderPipeline pipeline;
  bool pipelineReady;
  // Which wait applies is a tested policy (render_policy.hpp, pipeline_wait).
  const auto waitPolicy = gx::policy::pipeline_wait(skip_unready_pipelines(), requireReady);
  if (waitPolicy == gx::policy::PipelineWait::Unbounded) {
    pipelineReady = wait_pipeline(ref, pipeline);
  } else if (waitPolicy == gx::policy::PipelineWait::PersistentPassBudget) {
    // The pass resolves into a texture copy that may be one-shot (the first frame of any copy target
    // counts as one, and so does an offscreen bake), so a skipped draw might never be re-issued.
    // Waiting without a limit, though, froze Prime for 140-360 ms whenever a new area brought
    // materials the cache did not have yet (D3D12 compiles of 180-500 ms each; F8 captures of
    // 2026-10-28). Skip-unready mode is the player's choice of smoothness over completeness, so these
    // passes get their own bounded budget too: up to 6 ms per frame, then the draw is skipped this
    // frame and returns once its pipeline lands. A copy the game produces again next frame (nearly
    // all of them) heals by itself.
    static thread_local uint32_t persistentBudgetFrame = UINT32_MAX;
    static thread_local std::chrono::steady_clock::time_point persistentDeadline;
    if (persistentBudgetFrame != current_frame()) {
      persistentBudgetFrame = current_frame();
      persistentDeadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(gx::policy::pipeline_wait_budget_ms(waitPolicy));
    }
    pipelineReady = wait_pipeline_until(ref, pipeline, persistentDeadline);
    if (!pipelineReady) {
      static std::atomic<uint32_t> skipped{0};
      const uint32_t n = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 8 || n % 1000 == 0)
        Log.info("[persistent-pass-skip] draws skipped in copy/offscreen passes while their pipeline compiled: {}", n);
    }
  } else {
    // Each encoded frame may wait up to 4 ms in total for new pipelines; draws
    // whose pipeline is still compiling after that are skipped for this frame.
    static thread_local uint32_t budgetFrame = UINT32_MAX;
    static thread_local std::chrono::steady_clock::time_point deadline;
    if (budgetFrame != current_frame()) {
      budgetFrame = current_frame();
      deadline = std::chrono::steady_clock::now() +
                 std::chrono::milliseconds(gx::policy::pipeline_wait_budget_ms(waitPolicy));
    }
    pipelineReady = wait_pipeline_until(ref, pipeline, deadline);
  }
  if (!pipelineReady) {
    return false;
  }
  pass.SetPipeline(pipeline);
  currentPipeline = ref;
  return true;
}

static inline Range push(ByteBuffer& target, const uint8_t* data, size_t length, size_t alignment) {
  size_t padding = 0;
  if (alignment != 0) {
    const size_t remainder = length % alignment;
    if (remainder != 0) {
      padding = alignment - remainder;
    }
  }
  auto begin = target.size();
  if (length == 0) {
    length = alignment;
    target.append_zeroes(alignment);
  } else {
    target.append(data, length);
    if (padding > 0) {
      target.append_zeroes(padding);
    }
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length + padding)};
}
static inline Range map(ByteBuffer& target, size_t length, size_t alignment) {
  size_t padding = 0;
  if (alignment != 0) {
    const size_t remainder = length % alignment;
    if (remainder != 0) {
      padding = alignment - remainder;
    }
  }
  auto begin = target.size();
  if (length == 0) {
    length = alignment;
    target.append_zeroes(length);
  } else {
    // The caller fills [0, length); only the alignment padding needs clearing,
    // otherwise it hands the next frame's readers whatever was there before.
    target.append_uninitialized(length);
    target.append_zeroes(padding);
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length + padding)};
}
Range push_verts(const uint8_t* data, size_t length) { return push(g_verts, data, length, 0); }
Range push_indices(const uint8_t* data, size_t length) { return push(g_indices, data, length, 0); }
Range push_uniform(const uint8_t* data, size_t length) {
  return push(g_uniforms, data, length, g_cachedLimits.minUniformBufferOffsetAlignment);
}
Range push_storage(const uint8_t* data, size_t length) {
  return push(g_storage, data, length, g_cachedLimits.minStorageBufferOffsetAlignment);
}
Range push_texture_data(const uint8_t* data, size_t length, u32 bytesPerRow, u32 rowsPerImage) {
  // For CopyBufferToTexture, we need an alignment of 256 per row (see Dawn kTextureBytesPerRowAlignment)
  const auto copyBytesPerRow = AURORA_ALIGN(bytesPerRow, 256);
  const auto range = map(g_textureUpload, copyBytesPerRow * rowsPerImage, 0);
  u8* dst = g_textureUpload.data() + range.offset;
  for (u32 i = 0; i < rowsPerImage; ++i) {
    memcpy(dst, data, bytesPerRow);
    data += bytesPerRow;
    dst += copyBytesPerRow;
  }
  return range;
}
std::pair<ByteBuffer, Range> map_verts(size_t length) {
  const auto range = map(g_verts, length, 4);
  return {ByteBuffer{g_verts.data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> map_indices(size_t length) {
  const auto range = map(g_indices, length, 4);
  return {ByteBuffer{g_indices.data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> map_uniform(size_t length) {
  const auto range = map(g_uniforms, length, g_cachedLimits.minUniformBufferOffsetAlignment);
  return {ByteBuffer{g_uniforms.data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> copy_uniform(Range source) {
  const auto destination = map(g_uniforms, source.size, g_cachedLimits.minUniformBufferOffsetAlignment);
  std::memcpy(g_uniforms.data() + destination.offset, g_uniforms.data() + source.offset, source.size);
  return {ByteBuffer{g_uniforms.data() + destination.offset, destination.size}, destination};
}
std::pair<ByteBuffer, Range> map_storage(size_t length) {
  const auto range = map(g_storage, length, g_cachedLimits.minStorageBufferOffsetAlignment);
  return {ByteBuffer{g_storage.data() + range.offset, range.size}, range};
}

BindGroupRef bind_group_ref(const WGPUBindGroupDescriptor& descriptor) {
  const auto id = xxh3_hash(descriptor);
  const auto it = g_cachedBindGroups.find(id);
  if (it == g_cachedBindGroups.end()) {
    auto bg = wgpu::BindGroup::Acquire(wgpuDeviceCreateBindGroup(g_device.Get(), &descriptor));
    g_cachedBindGroups.emplace(id, CachedBindGroup{
                                       .bindGroup = std::move(bg),
                                       .lastUsedFrame = g_frameIndex,
                                   });
  } else {
    it->second.lastUsedFrame = g_frameIndex;
  }
  return id;
}

wgpu::BindGroup& find_bind_group(BindGroupRef id) {
  const auto it = g_cachedBindGroups.find(id);
  CHECK(it != g_cachedBindGroups.end(), "get_bind_group: failed to locate {:x}", id);
  return it->second.bindGroup;
}

wgpu::Sampler& sampler_ref(const wgpu::SamplerDescriptor& descriptor) {
  const auto id = xxh3_hash(descriptor);
  auto it = g_cachedSamplers.find(id);
  if (it == g_cachedSamplers.end()) {
    it = g_cachedSamplers.try_emplace(id, g_device.CreateSampler(&descriptor)).first;
  }
  return it->second;
}

uint32_t align_uniform(uint32_t value) { return AURORA_ALIGN(value, g_cachedLimits.minUniformBufferOffsetAlignment); }

void insert_debug_marker(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  auto idx = g_debugFrame.markers.size();
  g_debugFrame.markers.emplace_back(std::move(label));
  push_command(CommandType::DebugMarker, {.debugMarkerIndex = idx});
#endif
}

} // namespace aurora::gfx

void aurora::gfx::push_debug_group(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  g_debugFrame.groups.push_back(std::move(label));
#endif
}
void aurora_push_debug_group(const char* label) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  aurora::gfx::g_debugFrame.groups.emplace_back(label);
#endif
}
void aurora_pop_debug_group() {
#ifdef AURORA_GFX_DEBUG_GROUPS
  if (aurora::gfx::g_debugFrame.groups.empty()) {
    aurora::gfx::Log.error("Debug group stack underflowed!");
    return;
  }

  aurora::gfx::g_debugFrame.groups.pop_back();
#endif
}

const AuroraStats* aurora_get_stats() { return &aurora::gfx::g_stats; }
