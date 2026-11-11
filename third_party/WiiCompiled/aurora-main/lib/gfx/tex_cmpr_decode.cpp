#include "tex_cmpr_decode.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "texture.hpp"
#include "texture_convert.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

namespace aurora::gfx {
void push_cmpr_decode(tex_cmpr_decode::Request&& request);
bool can_push_cmpr_decode() noexcept;
} // namespace aurora::gfx

namespace aurora::gfx::tex_cmpr_decode {
static Module Log("aurora::gfx::tex_cmpr_decode");

using webgpu::g_device;
using webgpu::g_queue;

// One invocation per texel. Bytes are the guest's big-endian stream, packed four
// per u32 in buffer order. Mirrors BuildRGBA8FromCMPR line for line.
static constexpr std::string_view Shader = R"(
struct Params {
    width: u32,
    height: u32,
    tilesWide: u32,
    byteOffset: u32,
};
@group(0) @binding(0) var<storage, read> src: array<u32>;
@group(0) @binding(1) var dst: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> params: Params;

fn byte_at(i: u32) -> u32 {
    return (src[i >> 2u] >> ((i & 3u) * 8u)) & 0xFFu;
}
fn expand5(n: u32) -> u32 { return (n << 3u) | (n >> 2u); }
fn expand6(n: u32) -> u32 { return (n << 2u) | (n >> 4u); }
// S3TCBlend(a, b) = (3a + 5b) >> 3
fn s3tc_blend(a: vec3u, b: vec3u) -> vec3u { return (a * 3u + b * 5u) >> vec3u(3u); }

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= params.width || id.y >= params.height) {
        return;
    }
    let tile = (id.y >> 3u) * params.tilesWide + (id.x >> 3u);
    let sub = ((id.y >> 2u) & 1u) * 2u + ((id.x >> 2u) & 1u);
    let block = params.byteOffset + tile * 32u + sub * 8u;
    let color1 = (byte_at(block) << 8u) | byte_at(block + 1u);
    let color2 = (byte_at(block + 2u) << 8u) | byte_at(block + 3u);
    let c1 = vec3u(expand5((color1 >> 11u) & 0x1Fu), expand6((color1 >> 5u) & 0x3Fu), expand5(color1 & 0x1Fu));
    let c2 = vec3u(expand5((color2 >> 11u) & 0x1Fu), expand6((color2 >> 5u) & 0x3Fu), expand5(color2 & 0x1Fu));
    let bits = byte_at(block + 4u + (id.y & 3u));
    let index = (bits >> (6u - 2u * (id.x & 3u))) & 3u;
    var rgb: vec3u;
    var alpha = 255u;
    if (index == 0u) {
        rgb = c1;
    } else if (index == 1u) {
        rgb = c2;
    } else if (color1 > color2) {
        rgb = select(s3tc_blend(c1, c2), s3tc_blend(c2, c1), index == 2u);
    } else {
        rgb = (c1 + c2) >> vec3u(1u);
        // GX keeps the half-blend colour under alpha 0 (BC1 would use black).
        alpha = select(255u, 0u, index == 3u);
    }
    textureStore(dst, vec2i(id.xy), vec4f(vec3f(rgb), f32(alpha)) / 255.0);
}
)";

static wgpu::ComputePipeline g_pipeline;
static wgpu::BindGroupLayout g_bindGroupLayout;
static std::atomic<bool> g_enabled{[] {
  const char* value = std::getenv("WII_COMPILED_AURORA_GPU_CMPR_DECODE");
  return value == nullptr || std::string_view{value} != "0";
}()};
static const bool g_verify = std::getenv("WII_COMPILED_AURORA_CMPR_GPU_VERIFY") != nullptr;

struct Verification {
  wgpu::Buffer buffer;
  uint32_t width = 0, height = 0, bytesPerRow = 0;
  uint32_t mip = 0;
  size_t referenceOffset = 0;
  std::shared_ptr<std::vector<uint8_t>> reference;
};
static std::mutex g_verifyMutex;
static std::vector<Verification> g_verifications;
static uint32_t g_verifyQueued = 0;
static uint32_t g_verifyChecked = 0;
static uint32_t g_verifyMismatched = 0;

bool enabled() noexcept { return g_enabled.load(std::memory_order_relaxed) && g_pipeline != nullptr; }
void set_enabled(bool enabled) noexcept { g_enabled.store(enabled, std::memory_order_relaxed); }

void initialize() {
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = Shader.data();
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "CMPR decode"};
  const auto module = g_device.CreateShaderModule(&moduleDescriptor);
  const std::array entries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Compute,
          .buffer = {.type = wgpu::BufferBindingType::ReadOnlyStorage},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Compute,
          .storageTexture =
              {
                  .access = wgpu::StorageTextureAccess::WriteOnly,
                  .format = wgpu::TextureFormat::RGBA8Unorm,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          .visibility = wgpu::ShaderStage::Compute,
          .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = 16},
      },
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "CMPR decode bind group layout",
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_bindGroupLayout = g_device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "CMPR decode pipeline layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_bindGroupLayout,
  };
  const auto pipelineLayout = g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
  const wgpu::ComputePipelineDescriptor pipelineDescriptor{
      .label = "CMPR decode",
      .layout = pipelineLayout,
      .compute = {.module = module, .entryPoint = "cs_main"},
  };
  g_pipeline = g_device.CreateComputePipeline(&pipelineDescriptor);
  Log.info("GPU CMPR decode {} (verify={})", g_pipeline ? "ready" : "unavailable", g_verify);
}

void shutdown() {
  g_pipeline = {};
  g_bindGroupLayout = {};
  std::lock_guard lock(g_verifyMutex);
  g_verifications.clear();
}

bool queue(const TextureHandle& dst, ArrayRef<uint8_t> data) {
  if (!enabled() || !dst || !can_push_cmpr_decode()) {
    return false;
  }
  // Same per-mip footprint as BuildRGBA8FromCMPR: 8x8 tiles of 32 bytes.
  std::vector<std::array<uint32_t, 4>> params;
  uint32_t w = dst->size.width, h = dst->size.height;
  size_t offset = 0;
  for (uint32_t mip = 0; mip < dst->mipCount; ++mip) {
    const uint32_t tilesWide = (w + 7) / 8, tilesHigh = (h + 7) / 8;
    params.push_back({w, h, tilesWide, static_cast<uint32_t>(offset)});
    offset += size_t{tilesWide} * tilesHigh * 32;
    w = std::max(w / 2, 1u);
    h = std::max(h / 2, 1u);
  }
  if (offset > data.size()) {
    return false;
  }

  const uint64_t sourceSize = (offset + 3) & ~size_t{3};
  const wgpu::BufferDescriptor sourceDescriptor{
      .label = "CMPR source",
      .usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst,
      .size = sourceSize,
  };
  auto source = g_device.CreateBuffer(&sourceDescriptor);
  if (offset == sourceSize) {
    g_queue.WriteBuffer(source, 0, data.data(), offset);
  } else {
    std::vector<uint8_t> padded(sourceSize, 0);
    std::memcpy(padded.data(), data.data(), offset);
    g_queue.WriteBuffer(source, 0, padded.data(), padded.size());
  }
  constexpr uint64_t UniformStride = 256;
  const wgpu::BufferDescriptor uniformDescriptor{
      .label = "CMPR params",
      .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
      .size = UniformStride * params.size(),
  };
  auto uniforms = g_device.CreateBuffer(&uniformDescriptor);
  std::vector<uint8_t> uniformData(UniformStride * params.size(), 0);
  for (size_t i = 0; i < params.size(); ++i) {
    std::memcpy(uniformData.data() + i * UniformStride, params[i].data(), sizeof(params[i]));
  }
  g_queue.WriteBuffer(uniforms, 0, uniformData.data(), uniformData.size());

  Request request{.dst = dst};
  for (uint32_t mip = 0; mip < params.size(); ++mip) {
    const wgpu::TextureViewDescriptor viewDescriptor{
        .label = "CMPR decode target",
        .format = wgpu::TextureFormat::RGBA8Unorm,
        .dimension = wgpu::TextureViewDimension::e2D,
        .baseMipLevel = mip,
        .mipLevelCount = 1,
    };
    const std::array bindings{
        wgpu::BindGroupEntry{.binding = 0, .buffer = source, .size = sourceSize},
        wgpu::BindGroupEntry{.binding = 1, .textureView = dst->texture.CreateView(&viewDescriptor)},
        wgpu::BindGroupEntry{.binding = 2, .buffer = uniforms, .offset = UniformStride * mip, .size = 16},
    };
    const wgpu::BindGroupDescriptor bindGroupDescriptor{
        .label = "CMPR decode bind group",
        .layout = g_bindGroupLayout,
        .entryCount = bindings.size(),
        .entries = bindings.data(),
    };
    request.mips.push_back({g_device.CreateBindGroup(&bindGroupDescriptor), params[mip][0], params[mip][1]});
  }
  if (g_verify) {
    std::lock_guard lock(g_verifyMutex);
    if (g_verifyQueued < 256) {
      ++g_verifyQueued;
      const auto cpu = convert_texture(GX_TF_CMPR, dst->size.width, dst->size.height, dst->mipCount, data);
      request.reference = std::make_shared<std::vector<uint8_t>>(cpu.data.data(), cpu.data.data() + cpu.data.size());
    }
  }
  push_cmpr_decode(std::move(request));
  return true;
}

void run(const wgpu::CommandEncoder& cmd, const Request& req) {
  const wgpu::ComputePassDescriptor passDescriptor{.label = "CMPR decode"};
  auto pass = cmd.BeginComputePass(&passDescriptor);
  pass.SetPipeline(g_pipeline);
  for (const auto& mip : req.mips) {
    pass.SetBindGroup(0, mip.bindGroup);
    pass.DispatchWorkgroups((mip.width + 7) / 8, (mip.height + 7) / 8, 1);
  }
  pass.End();
  if (!req.reference) {
    return;
  }
  // Every mip level is read back and compared against its slice of the CPU decode.
  size_t referenceOffset = 0;
  for (uint32_t mip = 0; mip < req.mips.size(); ++mip) {
    const uint32_t width = req.mips[mip].width, height = req.mips[mip].height;
    const uint32_t bytesPerRow = (width * 4 + 255) & ~255u;
    const wgpu::BufferDescriptor readbackDescriptor{
        .label = "CMPR verify readback",
        .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
        .size = uint64_t{bytesPerRow} * height,
    };
    Verification verification{
        .buffer = g_device.CreateBuffer(&readbackDescriptor),
        .width = width,
        .height = height,
        .bytesPerRow = bytesPerRow,
        .mip = mip,
        .referenceOffset = referenceOffset,
        .reference = req.reference,
    };
    referenceOffset += size_t{width} * height * 4;
    const wgpu::TexelCopyTextureInfo source{.texture = req.dst->texture, .mipLevel = mip};
    const wgpu::TexelCopyBufferInfo destination{
        .layout = {.offset = 0, .bytesPerRow = bytesPerRow, .rowsPerImage = height},
        .buffer = verification.buffer,
    };
    const wgpu::Extent3D extent{width, height, 1};
    cmd.CopyTextureToBuffer(&source, &destination, &extent);
    std::lock_guard lock(g_verifyMutex);
    g_verifications.push_back(std::move(verification));
  }
}

void complete_verifications() {
  std::vector<Verification> pending;
  {
    std::lock_guard lock(g_verifyMutex);
    pending.swap(g_verifications);
  }
  for (auto& verification : pending) {
    wgpu::MapAsyncStatus status = wgpu::MapAsyncStatus::Error;
    auto future = verification.buffer.MapAsync(wgpu::MapMode::Read, 0, verification.buffer.GetSize(),
                                               wgpu::CallbackMode::WaitAnyOnly,
                                               [&](wgpu::MapAsyncStatus s, wgpu::StringView) { status = s; });
    if (webgpu::g_instance.WaitAny(future, 5000000000) != wgpu::WaitStatus::Success ||
        status != wgpu::MapAsyncStatus::Success) {
      Log.warn("[cmpr-gpu-verify] readback failed");
      continue;
    }
    const auto* gpu = static_cast<const uint8_t*>(verification.buffer.GetConstMappedRange(0, verification.buffer.GetSize()));
    const auto& cpu = *verification.reference;
    uint32_t badTexels = 0, firstX = 0, firstY = 0;
    for (uint32_t y = 0; y < verification.height; ++y) {
      const uint8_t* gpuRow = gpu + size_t{y} * verification.bytesPerRow;
      const uint8_t* cpuRow = cpu.data() + verification.referenceOffset + size_t{y} * verification.width * 4;
      for (uint32_t x = 0; x < verification.width; ++x) {
        if (std::memcmp(gpuRow + x * 4, cpuRow + x * 4, 4) != 0) {
          if (badTexels++ == 0) {
            firstX = x;
            firstY = y;
          }
        }
      }
    }
    verification.buffer.Unmap();
    ++g_verifyChecked;
    if (badTexels != 0) {
      ++g_verifyMismatched;
      Log.warn("[cmpr-gpu-verify] mip {} {}x{} mismatched texels={} first={},{}", verification.mip, verification.width,
               verification.height, badTexels, firstX, firstY);
    }
    if (badTexels != 0 || g_verifyChecked % 32 == 1) {
      Log.info("[cmpr-gpu-verify] mip levels checked={} mismatched={}", g_verifyChecked, g_verifyMismatched);
    }
  }
}

} // namespace aurora::gfx::tex_cmpr_decode
