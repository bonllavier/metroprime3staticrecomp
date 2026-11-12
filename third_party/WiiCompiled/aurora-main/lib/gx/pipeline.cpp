#include "pipeline.hpp"
#include <aurora/gfx.h>
#include "render_policy.hpp"
#include "uber.hpp"

#include "../webgpu/gpu.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "gx_fmt.hpp"
#include "shader_info.hpp"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

// Diagnostic environment switches are fixed for the lifetime of this process.
static bool CachedWiiCompiledTracePrimeGx() {
  static const bool enabled = std::getenv("WII_COMPILED_TRACE_PRIME_GX") != nullptr;
  return enabled;
}

// Diagnostic environment switches are fixed for the lifetime of this process.
static bool CachedWiiCompiledDiagnosticRebindGx() {
  static const bool enabled = std::getenv("WII_COMPILED_DIAGNOSTIC_REBIND_GX") != nullptr;
  return enabled;
}

// Diagnostic environment switches are fixed for the lifetime of this process.
static bool CachedWiiCompiledDiagnosticUniformGroups() {
  static const bool enabled = std::getenv("WII_COMPILED_DIAGNOSTIC_UNIFORM_GROUPS") != nullptr;
  return enabled;
}


namespace aurora::gx {
static Module Log("aurora::gx");

namespace {
struct ShaderConfigHash {
  size_t operator()(const ShaderConfig& config) const noexcept { return static_cast<size_t>(xxh3_hash(config)); }
};

struct CachedShaderModule {
  std::condition_variable ready;
  wgpu::ShaderModule module;
  bool compiling = true;
  uint64_t lastUse = 0;
};

std::mutex sShaderModuleCacheMutex;
absl::flat_hash_map<ShaderConfig, std::shared_ptr<CachedShaderModule>, ShaderConfigHash> sShaderModuleCache;
uint64_t sShaderModuleUseClock = 0;

// Dawn keeps the parsed Tint program (AST + semantic info, ~1 MB) of a shader module for as long as the
// application holds a reference to it; it is dropped when the last external reference goes (pipelines keep
// internal ones only). Holding every module forever therefore kept ~11k programs alive on a cold-cache launch
// (7+ GB, the desktop froze, 2026-11-11). The cache only serves pipelines of one shader built close together
// (blend/depth variants), so it keeps the most recently used modules; an evicted one is rebuilt on demand,
// which is cheap once Dawn's blob cache holds its parse.
constexpr size_t MaxCachedShaderModules = 256;

// Caller holds sShaderModuleCacheMutex. Trims to the cap in batches so the scan stays rare.
void evict_shader_modules_locked() {
  if (sShaderModuleCache.size() <= MaxCachedShaderModules + MaxCachedShaderModules / 4) {
    return;
  }
  std::vector<std::pair<uint64_t, ShaderConfig>> finished;
  finished.reserve(sShaderModuleCache.size());
  for (const auto& [config, entry] : sShaderModuleCache) {
    if (!entry->compiling) finished.emplace_back(entry->lastUse, config);
  }
  const size_t excess = sShaderModuleCache.size() - MaxCachedShaderModules;
  const size_t count = std::min(excess, finished.size());
  std::partial_sort(finished.begin(), finished.begin() + count, finished.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });
  for (size_t i = 0; i < count; ++i) sShaderModuleCache.erase(finished[i].second);
}

// Pipeline compile telemetry: how long each new GX pipeline takes, split into the shader
// module (WGSL generation + Dawn parse) and the render pipeline (Tint -> HLSL -> DXC -> PSO).
std::atomic<uint64_t> sCompiledPipelines{0}, sNewModules{0}, sModuleMicros{0}, sPipelineMicros{0}, sSlowPipelines{0};
std::atomic<uint64_t> sMaxPipelineMicros{0};
thread_local bool tLastModuleWasNew = false;

wgpu::ShaderModule cached_uber_shader_module(const PipelineConfig& config);
wgpu::ShaderModule cached_shader_module(const ShaderConfig& config) {
  tLastModuleWasNew = false;
  std::shared_ptr<CachedShaderModule> entry;
  {
    std::unique_lock lock{sShaderModuleCacheMutex};
    const auto it = sShaderModuleCache.find(config);
    if (it == sShaderModuleCache.end()) {
      entry = std::make_shared<CachedShaderModule>();
      sShaderModuleCache.emplace(config, entry);
    } else {
      entry = it->second;
      entry->ready.wait(lock, [&] { return !entry->compiling; });
      entry->lastUse = ++sShaderModuleUseClock;
      return entry->module;
    }
  }

  tLastModuleWasNew = true;
  auto module = build_shader(config);
  {
    std::lock_guard lock{sShaderModuleCacheMutex};
    entry->module = module;
    entry->compiling = false;
    entry->lastUse = ++sShaderModuleUseClock;
    evict_shader_modules_locked();
  }
  entry->ready.notify_all();
  return module;
}
wgpu::ShaderModule cached_uber_shader_module(const PipelineConfig& config) {
  // Keyed like a specialised module: the encoded shader config is unique (marker bit and masks).
  tLastModuleWasNew = false;
  std::shared_ptr<CachedShaderModule> entry;
  {
    std::unique_lock lock{sShaderModuleCacheMutex};
    const auto it = sShaderModuleCache.find(config.shaderConfig);
    if (it == sShaderModuleCache.end()) {
      entry = std::make_shared<CachedShaderModule>();
      sShaderModuleCache.emplace(config.shaderConfig, entry);
    } else {
      entry = it->second;
      entry->ready.wait(lock, [&] { return !entry->compiling; });
      entry->lastUse = ++sShaderModuleUseClock;
      return entry->module;
    }
  }
  tLastModuleWasNew = true;
  auto module = build_uber_shader(decode_uber_pipeline(config));
  {
    std::lock_guard lock{sShaderModuleCacheMutex};
    entry->module = module;
    entry->compiling = false;
    entry->lastUse = ++sShaderModuleUseClock;
    evict_shader_modules_locked();
  }
  entry->ready.notify_all();
  return module;
}

std::atomic<bool> sUberHybrid{false};
std::atomic<uint64_t> sUberDrawsTotal{0};
} // namespace

void set_uber_hybrid(bool enabled) noexcept { sUberHybrid.store(enabled, std::memory_order_relaxed); }
bool uber_hybrid_enabled() noexcept { return sUberHybrid.load(std::memory_order_relaxed); }
uint64_t uber_draws_total() noexcept { return sUberDrawsTotal.load(std::memory_order_relaxed); }
bool uber_force() noexcept {
  static const bool force = std::getenv("WII_COMPILED_UBER_FORCE") != nullptr;
  return force;
}

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  const bool tracedFont = xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX)) == 0xF892D70D494F0487ull;
  if (tracedFont && CachedWiiCompiledTracePrimeGx())
    Log.info("[prime-font-created-pipeline] hash={:016X} shader={:016X} stride={} pos={} uv={} cull={} colorwrite={} blend={}/{}/{}",
             xxh3_hash(config,static_cast<HashType>(gfx::ShaderType::GX)),xxh3_hash(config.shaderConfig),
             config.shaderConfig.vtxStride,config.shaderConfig.attrs[GX_VA_POS].offset,
             config.shaderConfig.attrs[GX_VA_TEX0].offset,underlying(config.cullMode),config.colorUpdate,
             underlying(config.blendMode),underlying(config.blendFacSrc),underlying(config.blendFacDst));
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  const auto shader = is_uber_pipeline(config) ? cached_uber_shader_module(config) : cached_shader_module(config.shaderConfig);
  const bool newModule = tLastModuleWasNew;
  if (!shader) return {};  // an ubershader the compiler rejected: the draw keeps waiting for the specialised pipeline
  const auto moduleDone = Clock::now();
  auto pipeline = build_pipeline(config, {}, shader, "GX Pipeline");
  const auto done = Clock::now();
  const auto micros = [](auto d) { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(d).count()); };
  const uint64_t moduleUs = micros(moduleDone - start), pipelineUs = micros(done - moduleDone);
  sCompiledPipelines.fetch_add(1, std::memory_order_relaxed);
  sNewModules.fetch_add(newModule, std::memory_order_relaxed);
  sModuleMicros.fetch_add(moduleUs, std::memory_order_relaxed);
  sPipelineMicros.fetch_add(pipelineUs, std::memory_order_relaxed);
  for (uint64_t seen = sMaxPipelineMicros.load(); moduleUs + pipelineUs > seen &&
       !sMaxPipelineMicros.compare_exchange_weak(seen, moduleUs + pipelineUs);) {}
  // Detail lines only for compiles met in play: the boot prewarm would use up the budget.
  const bool inGame = !gfx::pipeline_prewarm_active();
  if (inGame && moduleUs + pipelineUs >= 20000 && sSlowPipelines.fetch_add(1, std::memory_order_relaxed) < 600) {
    const auto& sc = config.shaderConfig;
    Log.info("[pipeline-compile] ms={:.1f} module_ms={:.1f} pipeline_ms={:.1f} new_module={} tev={} texgens={} ind={} "
             "chans={} fog={} ppl={} metal={} blend={}/{}/{} depth={}/{}/{} dstalpha={} hash={:016X}",
             (moduleUs + pipelineUs) / 1000.0, moduleUs / 1000.0, pipelineUs / 1000.0, newModule, sc.tevStageCount,
             sc.numTexGens, sc.numIndStages, unsigned(sc.colorChannels[0].lightingEnabled) + unsigned(sc.colorChannels[1].lightingEnabled),
             unsigned(sc.fogType), unsigned(sc.perPixelLighting), unsigned(sc.enhancedMetal), unsigned(config.blendMode), unsigned(config.blendFacSrc), unsigned(config.blendFacDst),
             config.depthCompare, unsigned(config.depthFunc), config.depthUpdate, sc.dstAlpha,
             xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX)));
  }
  static std::atomic<int64_t> nextReport{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(done.time_since_epoch()).count();
  int64_t due = nextReport.load(std::memory_order_relaxed);
  if (now >= due && nextReport.compare_exchange_strong(due, now + 10)) {
    const uint64_t n = sCompiledPipelines.load(), mods = sNewModules.load();
    Log.info("[pipeline-compile-summary] pipelines={} new_modules={} avg_module_ms={:.2f} avg_pipeline_ms={:.2f} "
             "max_ms={:.1f} slow_ge20ms={}",
             n, mods, n ? sModuleMicros.load() / 1000.0 / n : 0.0, n ? sPipelineMicros.load() / 1000.0 / n : 0.0,
             sMaxPipelineMicros.load() / 1000.0, sSlowPipelines.load());
  }
  return pipeline;
}

void clear_shader_module_cache() {
  std::lock_guard lock{sShaderModuleCacheMutex};
  sShaderModuleCache.clear();
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass, DrawEncodeState& state,
            bool requireReadyPipeline, const gfx::Range* uniformRangeOverride) {
  if (CachedWiiCompiledDiagnosticRebindGx()) {
    state.currentPipeline=UINTPTR_MAX;
    state.boundTextureBindGroup=nullptr;
    state.indexBufferBound=false;
  }
  bool bound = false;
  bool usingUber = false;
  // WII_COMPILED_UBER_STATS=1: every 5 s, why draws whose specialised pipeline was not ready did or did not
  // get the ubershader. Costs a pipeline-map lookup per draw, so it is diagnostic only.
  static const bool uberStats = std::getenv("WII_COMPILED_UBER_STATS") != nullptr;
  struct UberStats {
    std::atomic<uint64_t> waitedNoBridge{0}, used{0}, uberNotReady{0}, specReadyAtEncode{0};
  };
  static UberStats stats;
  if (uberStats) {
    if (data.uberPipeline == 0) {
      wgpu::RenderPipeline probe;
      if (!gfx::try_pipeline(data.pipeline, probe)) stats.waitedNoBridge.fetch_add(1, std::memory_order_relaxed);
    }
    static std::atomic<int64_t> nextReport{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t due = nextReport.load(std::memory_order_relaxed);
    if (now >= due && nextReport.compare_exchange_strong(due, now + 5)) {
      Log.info("[uber-stats] not-ready draws: bridged_by_uber={} uber_not_ready={} spec_ready_at_encode={} "
               "no_bridge_recorded={}",
               stats.used.exchange(0), stats.uberNotReady.exchange(0), stats.specReadyAtEncode.exchange(0),
               stats.waitedNoBridge.exchange(0));
    }
  }
  if (data.uberPipeline != 0) {
    // The specialised pipeline was still compiling when this draw was recorded (hybrid ubershader mode).
    wgpu::RenderPipeline probe;
    policy::PipelineAvailability availability;
    availability.specializedReady = gfx::try_pipeline(data.pipeline, probe) && !uber_force();
    // A rejected ubershader module leaves a null pipeline in the map: never "ready".
    availability.uberReady = gfx::try_pipeline(data.uberPipeline, probe) && probe.Get() != nullptr;
    if (uberStats) {
      if (availability.specializedReady) stats.specReadyAtEncode.fetch_add(1, std::memory_order_relaxed);
      else if (!availability.uberReady) stats.uberNotReady.fetch_add(1, std::memory_order_relaxed);
      else stats.used.fetch_add(1, std::memory_order_relaxed);
    }
    if (policy::choose_pipeline(policy::UberMode::Hybrid, availability) == policy::PipelineChoice::Uber) {
      bound = gfx::bind_pipeline(data.uberPipeline, pass, state.currentPipeline, false);
      usingUber = bound;
      if (usingUber) {
        const uint64_t n = sUberDrawsTotal.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || n == 100 || n % 100000 == 0) Log.info("[uber] draws encoded with an ubershader so far: {}", n);
      }
    }
  }
  if (!bound && data.fallbackPipeline != 0 && data.pipeline != state.currentPipeline) {
    // Per-pixel lighting / enhanced material still compiling (a 200-500 ms D3D12 compile) while its
    // vertex-lit fallback is ready: draw with the fallback now instead of waiting (policy::first_bind).
    wgpu::RenderPipeline probe;
    policy::DrawPipelines pipelines;
    pipelines.hasFallback = true;
    pipelines.primaryReady = gfx::try_pipeline(data.pipeline, probe);
    pipelines.fallbackReady = !pipelines.primaryReady && gfx::try_pipeline(data.fallbackPipeline, probe);
    if (policy::first_bind(pipelines) == policy::FirstBind::Fallback) {
      bound = gfx::bind_pipeline(data.fallbackPipeline, pass, state.currentPipeline, requireReadyPipeline);
    }
  }
  if (!bound) bound = gfx::bind_pipeline(data.pipeline, pass, state.currentPipeline, requireReadyPipeline);
  if (!bound && data.fallbackPipeline != 0) {
    // Per-pixel variant still compiling: draw with the per-vertex pipeline this frame.
    bound = gfx::bind_pipeline(data.fallbackPipeline, pass, state.currentPipeline, requireReadyPipeline);
  }
  static unsigned primeRenderTrace = 0;
  if (CachedWiiCompiledTracePrimeGx() && primeRenderTrace++ < 128)
    Log.info("[prime-gx-render-encoded] indices={} instances={} pipeline_bound={} required={} uniform={}/{}",
             data.indexCount,data.instanceCount,bound,requireReadyPipeline,data.uniformRange.offset,data.uniformRange.size);
  if (!bound) {
    static unsigned skippedTrace = 0;
    if (CachedWiiCompiledTracePrimeGx() && skippedTrace++ < 16)
      Log.warn("[prime-gx-draw-skipped-pipeline] indices={} required={}",data.indexCount,requireReadyPipeline);
    return;
  }

  // An interpolated presentation slot re-encodes the identical draw with only this range replaced; overriding here avoids copying the whole DrawData per draw per slot.
  const gfx::Range& uniformRange = usingUber ? data.uberUniformRange
                                  : uniformRangeOverride != nullptr ? *uniformRangeOverride : data.uniformRange;
  const std::array offsets{uniformRange.offset};
  if(CachedWiiCompiledDiagnosticUniformGroups()) {
    const wgpu::BindGroupEntry entry{.binding=0,.buffer=gfx::g_uniformBuffer,.offset=uniformRange.offset,.size=MaxUniformSize};
    const wgpu::BindGroupDescriptor desc{.label="Diagnostic fixed uniform range",.layout=gfx::g_uniformBindGroupLayout,.entryCount=1,.entries=&entry};
    auto group=webgpu::g_device.CreateBindGroup(&desc);
    const uint32_t zero=0;
    pass.SetBindGroup(1,group,1,&zero);
  } else pass.SetBindGroup(1, gfx::g_uniformBindGroup, offsets.size(), offsets.data());
  // Resolved when the draw was recorded; see GXBindGroups.
  if (data.bindGroups.resolvedTextureBindGroup != nullptr &&
      data.bindGroups.resolvedTextureBindGroup != state.boundTextureBindGroup) {
    wgpuRenderPassEncoderSetBindGroup(pass.Get(), 2, data.bindGroups.resolvedTextureBindGroup, 0, nullptr);
    state.boundTextureBindGroup = data.bindGroups.resolvedTextureBindGroup;
  }
  if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  if (!state.indexBufferBound) {
    // Bound once for the pass; draws select their range with firstIndex below.
    pass.SetIndexBuffer(gfx::g_indexBuffer, wgpu::IndexFormat::Uint16, 0, wgpu::kWholeSize);
    state.indexBufferBound = true;
  }
  pass.DrawIndexed(data.indexCount, data.instanceCount,
                   static_cast<uint32_t>(data.idxRange.offset / sizeof(uint16_t)));
}
} // namespace aurora::gx

void aurora_set_ubershader(const bool enabled) { aurora::gx::set_uber_hybrid(enabled); }

bool aurora_get_ubershader() { return aurora::gx::uber_hybrid_enabled(); }

uint64_t aurora_get_ubershader_draws() { return aurora::gx::uber_draws_total(); }
