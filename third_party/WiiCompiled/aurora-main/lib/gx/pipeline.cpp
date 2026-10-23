#include "pipeline.hpp"

#include "../webgpu/gpu.hpp"
#include "gx_fmt.hpp"
#include "shader_info.hpp"
#include "tracy/Tracy.hpp"

#include <condition_variable>
#include <memory>
#include <cstdlib>
#include <mutex>

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
};

std::mutex sShaderModuleCacheMutex;
absl::flat_hash_map<ShaderConfig, std::shared_ptr<CachedShaderModule>, ShaderConfigHash> sShaderModuleCache;

wgpu::ShaderModule cached_shader_module(const ShaderConfig& config) {
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
      return entry->module;
    }
  }

  auto module = build_shader(config);
  {
    std::lock_guard lock{sShaderModuleCacheMutex};
    entry->module = module;
    entry->compiling = false;
  }
  entry->ready.notify_all();
  return module;
}
} // namespace

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  const bool tracedFont = xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX)) == 0xF892D70D494F0487ull;
  if (tracedFont && CachedWiiCompiledTracePrimeGx())
    Log.info("[prime-font-created-pipeline] hash={:016X} shader={:016X} stride={} pos={} uv={} cull={} colorwrite={} blend={}/{}/{}",
             xxh3_hash(config,static_cast<HashType>(gfx::ShaderType::GX)),xxh3_hash(config.shaderConfig),
             config.shaderConfig.vtxStride,config.shaderConfig.attrs[GX_VA_POS].offset,
             config.shaderConfig.attrs[GX_VA_TEX0].offset,underlying(config.cullMode),config.colorUpdate,
             underlying(config.blendMode),underlying(config.blendFacSrc),underlying(config.blendFacDst));
  const auto shader = cached_shader_module(config.shaderConfig);
  return build_pipeline(config, {}, shader, "GX Pipeline");
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
  const bool bound = gfx::bind_pipeline(data.pipeline, pass, state.currentPipeline, requireReadyPipeline);
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
  const gfx::Range& uniformRange = uniformRangeOverride != nullptr ? *uniformRangeOverride : data.uniformRange;
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
