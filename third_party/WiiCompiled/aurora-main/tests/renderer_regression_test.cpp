#include "gx_test_common.hpp"
#include "gfx/staging_map.hpp"
#include "gx/pipeline.hpp"
#include "gfx/texture_replacement.hpp"

#include <thread>

using aurora::gx::g_gxState;

namespace {
std::vector<u8> draw(GXPrimitive primitive, u16 count, GXVtxFmt format = GX_VTXFMT0) {
  std::vector<u8> bytes{static_cast<u8>(primitive | format), static_cast<u8>(count >> 8),
                        static_cast<u8>(count)};
  bytes.resize(3 + count);
  return bytes;
}
}

TEST_F(GXFifoTest, MaximumQuadCountTerminatesWithoutOutOfRangeIndices) {
  g_gxState.lastVtxFmt = GX_VTXFMT0;
  g_gxState.lastVtxSize = 1;
  g_gxState.vtxDesc[GX_VA_POS] = GX_DIRECT;  // draws without a position are dropped (see handle_draw)
  for (const u16 count : {65532, 65533, 65534, 65535}) {
    g_gxState.stateDirty = true;
    decode_fifo(draw(GX_QUADS, count));
    const auto& indices = aurora::gfx::testing::last_pushed_indices();
    ASSERT_EQ(indices.size(), (count / 4) * 6 + (count % 4 == 3 ? 3 : 0));
    for (const auto index : indices) ASSERT_LT(index, count);
  }
}

TEST_F(GXFifoTest, IncompletePrimitivesNeverJoinAcrossDraws) {
  g_gxState.lastVtxFmt = GX_VTXFMT0;
  g_gxState.lastVtxSize = 1;
  g_gxState.vtxDesc[GX_VA_POS] = GX_DIRECT;  // draws without a position are dropped (see handle_draw)
  aurora::gfx::testing::use_draw_command_tracking(true);
  decode_fifo(draw(GX_TRIANGLES, 4));
  EXPECT_EQ(aurora::gfx::testing::last_pushed_indices(), (std::vector<u16>{0, 1, 2}));
  decode_fifo(draw(GX_TRIANGLES, 5));
  EXPECT_EQ(aurora::gfx::testing::last_pushed_indices(), (std::vector<u16>{4, 5, 6}));
  const auto before = aurora::gfx::testing::last_pushed_indices();
  decode_fifo(draw(GX_TRIANGLEFAN, 2));
  EXPECT_EQ(aurora::gfx::testing::last_pushed_indices(), before);
}

TEST_F(GXFifoTest, MergeStopsBeforeSixteenBitIndexOverflow) {
  g_gxState.lastVtxFmt = GX_VTXFMT0;
  g_gxState.lastVtxSize = 1;
  g_gxState.vtxDesc[GX_VA_POS] = GX_DIRECT;  // draws without a position are dropped (see handle_draw)
  aurora::gfx::testing::use_draw_command_tracking(true);
  decode_fifo(draw(GX_TRIANGLES, 65535));
  decode_fifo(draw(GX_TRIANGLES, 3));
  EXPECT_EQ(aurora::gfx::g_mergedDrawCallCount, 0u);
  EXPECT_EQ(aurora::gfx::testing::last_pushed_indices(), (std::vector<u16>{0, 1, 2}));
}

TEST_F(GXFifoTest, VertexCacheInvalidationBreaksDrawMerging) {
  g_gxState.lastVtxFmt = GX_VTXFMT0;
  g_gxState.lastVtxSize = 1;
  g_gxState.vtxDesc[GX_VA_POS] = GX_DIRECT;  // draws without a position are dropped (see handle_draw)
  aurora::gfx::testing::use_draw_command_tracking(true);
  decode_fifo(draw(GX_TRIANGLES, 3));
  decode_fifo({GX_CMD_INVL_VC});
  EXPECT_TRUE(g_gxState.stateDirty);
  decode_fifo(draw(GX_TRIANGLES, 3));
  EXPECT_EQ(aurora::gfx::g_mergedDrawCallCount, 0u);
}

TEST_F(GXFifoTest, EqualStrideVertexFormatChangeBreaksDrawMerging) {
  aurora::gfx::testing::use_real_vertex_format_helpers(true);
  g_gxState.vtxDesc[GX_VA_POS] = GX_DIRECT;
  for (const auto format : {GX_VTXFMT0, GX_VTXFMT1}) {
    g_gxState.vtxFmts[format].attrs[GX_VA_POS].cnt = GX_POS_XY;
    g_gxState.vtxFmts[format].attrs[GX_VA_POS].type = GX_U8;
  }
  g_gxState.vtxFmts[GX_VTXFMT1].attrs[GX_VA_POS].frac = 1;
  aurora::gfx::testing::use_draw_command_tracking(true);
  for (const auto format : {GX_VTXFMT0, GX_VTXFMT1}) {
    auto bytes = draw(GX_TRIANGLES, 3, format);
    bytes.resize(9);
    decode_fifo(bytes);
  }
  EXPECT_EQ(aurora::gfx::g_mergedDrawCallCount, 0u);
}

TEST_F(GXFifoTest, SingleExpandedPrimitiveCannotMergeWithTriangles) {
  g_gxState.lastVtxFmt = GX_VTXFMT0;
  g_gxState.lastVtxSize = 1;
  g_gxState.vtxDesc[GX_VA_POS] = GX_DIRECT;  // draws without a position are dropped (see handle_draw)
  aurora::gfx::testing::use_draw_command_tracking(true);
  decode_fifo(draw(GX_POINTS, 1));
  decode_fifo(draw(GX_TRIANGLES, 3));
  EXPECT_EQ(aurora::gfx::g_mergedDrawCallCount, 0u);
  EXPECT_EQ(aurora::gfx::testing::last_pushed_indices(), (std::vector<u16>{0, 1, 2}));
}

TEST_F(GXFifoTest, DisplayListSubmitsStagingWhenADrawDoesNotFit) {
  __GXSetDirtyState();
  aurora::gx::fifo::clear_buffer();
  g_gxState.lastVtxFmt = GX_VTXFMT0;
  g_gxState.lastVtxSize = 1;
  g_gxState.vtxDesc[GX_VA_POS] = GX_DIRECT;  // draws without a position are dropped (see handle_draw)
  // admitted=1 refuses the middle draw; admitted=2 refuses the last, so only a replay of the refused draw pushes 0x33.
  for (const u32 admitted : {1u, 2u}) for (const bool bigEndian : {true, false}) {
    std::vector<u8> list;
    for (const u8 fill : {0x11, 0x22, 0x33}) {
      auto bytes = draw(GX_TRIANGLES, 3);
      if (!bigEndian) std::swap(bytes[1], bytes[2]);
      std::fill(bytes.begin() + 3, bytes.end(), fill);
      list.insert(list.end(), bytes.begin(), bytes.end());
    }
    const auto splitsBefore = aurora::gfx::staging_split_count();
    aurora::gfx::testing::refuse_staging_admission_after(admitted);
    if (bigEndian) GXCallDisplayList(list.data(), static_cast<u32>(list.size()));
    else GXCallDisplayListLE(list.data(), static_cast<u32>(list.size()));
    EXPECT_EQ(aurora::gfx::staging_split_count() - splitsBefore, 1u)
        << "admitted=" << admitted << " bigEndian=" << bigEndian;
    EXPECT_EQ(aurora::gfx::testing::last_pushed_vertices(), (std::vector<u8>{0x33, 0x33, 0x33}))
        << "admitted=" << admitted << " bigEndian=" << bigEndian;
  }
}

TEST(StagingMapping, RetiredCallbacksCannotPublishAnotherBuffersReadiness) {
  using namespace aurora::gfx;
  StagingMapState state;
  const auto old = state.request();
  EXPECT_EQ(state.request(), 0u);
  state.reset();
  const auto current = state.request();
  EXPECT_FALSE(state.complete(old, BufferMapState::Mapped));
  EXPECT_FALSE(state.complete(old, BufferMapState::Unmapped));
  EXPECT_EQ(state.state(), BufferMapState::Mapping);
  EXPECT_TRUE(state.complete(current, BufferMapState::Mapped));
  EXPECT_FALSE(state.complete(current, BufferMapState::Unmapped));
  EXPECT_EQ(state.state(), BufferMapState::Mapped);
}

TEST(StagingMapping, AsyncCompletionWakesWaiters) {
  using namespace aurora::gfx;
  StagingMapState state;
  const auto generation = state.request();
  std::thread callback([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    state.complete(generation, BufferMapState::Mapped);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (state.state() == BufferMapState::Mapping && std::chrono::steady_clock::now() < deadline)
    state.wait_for_progress();
  callback.join();
  EXPECT_EQ(state.state(), BufferMapState::Mapped);
}

TEST(StagingCapacity, ReservesPaddingAndRejectsOverflow) {
  using namespace aurora::gfx;
  EXPECT_EQ(staging_padded(257, 256), 512u);
  EXPECT_THROW(staging_padded(UINT64_MAX, 256), StagingCapacityError);
  const StagingSizes used{0, 256, 0, 0}, demand{0, 256, 0, 0}, tail{0, 3840, 0, 0};
  EXPECT_TRUE(staging_fits(used, demand, tail, {4, 4352, 4, 4}));
  EXPECT_FALSE(staging_fits(used, demand, tail, {4, 4351, 4, 4}));
  EXPECT_FALSE(staging_fits({UINT64_MAX, 0, 0, 0}, {1, 0, 0, 0}, {},
                            {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}));
}

TEST(FrameInterpolationContract, IdenticalMeshesInDifferentViewportsDoNotShareHistory) {
  using namespace aurora;
  const auto savedViewport = gx::g_gxState.logicalViewport;
  constexpr size_t positionOffset = sizeof(Mat4x4<float>);
  constexpr size_t normalOffset = positionOffset + gx::MaxPnMtx * sizeof(Mat3x4<float>);
  constexpr size_t uniformSize = normalOffset + gx::MaxPnMtx * sizeof(Mat3x4<float>);
  const gx::FrameInterpolationDrawIdentity identity{0x1234, 0x5678, 0x9abc, 0xdef0};
  const Mat4x4<float> projection{};
  const auto record = [&](float x, std::array<uint8_t, uniformSize>& source) {
    gx::g_gxState.pnMtx[0].pos = {{1.f, 0.f, 0.f, x}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}};
    gx::g_gxState.pnMtx[0].nrm = {{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}};
    std::memcpy(source.data() + positionOffset, &gx::g_gxState.pnMtx[0].pos, sizeof(Mat3x4<float>));
    std::memcpy(source.data() + normalOffset, &gx::g_gxState.pnMtx[0].nrm, sizeof(Mat3x4<float>));
    return gx::record_interpolation_draw(identity, projection, 1, {
        .sourceUniformData = source.data(), .uniformSize = source.size(), .projectionOffset = 0,
        .positionOffset = positionOffset, .normalOffset = normalOffset, .currentMatrix = 0,
        .indexedMatrices = true});
  };
  gx::set_frame_interpolation_fps(0);
  gx::begin_frame_interpolation();
  gx::set_frame_interpolation_fps(120);
  gx::g_gxState.logicalViewport = {0.f, 0.f, 640.f, 240.f, 0.f, 1.f};
  std::array<uint8_t, uniformSize> previous{};
  gx::begin_frame_interpolation();
  record(0.f, previous);
  // The display copy that ends the game frame.
  gx::note_frame_interpolation_boundary();
  gx::finalize_frame_interpolation();
  gfx::testing::reset_uniform_allocations();
  gx::g_gxState.logicalViewport.top = 240.f;
  std::array<uint8_t, uniformSize> current{};
  gx::begin_frame_interpolation();
  const auto ranges = record(20.f, current);
  const auto expected = current;
  // The display copy that ends the game frame.
  gx::note_frame_interpolation_boundary();
  gx::finalize_frame_interpolation();
  EXPECT_EQ(current, expected);
  if (ranges[0].size) {
    const auto& duplicate = gfx::testing::uniform_allocation(ranges[0].offset);
    ASSERT_EQ(duplicate.size(), expected.size());
    EXPECT_EQ(std::memcmp(duplicate.data(), expected.data(), expected.size()), 0);
  }
  gx::g_gxState.logicalViewport = savedViewport;
  gx::set_frame_interpolation_fps(0);
  gx::begin_frame_interpolation();
}

// The decoder drops a draw whose vertex description has no position instead of building a pipeline
// key from it (such a key poisoned the pipeline cache on 2026-10-25).
TEST_F(GXFifoTest, DrawWithoutPositionAttributeIsDropped) {
  g_gxState.lastVtxFmt = GX_VTXFMT0;
  g_gxState.lastVtxSize = 1;
  g_gxState.vtxDesc[GX_VA_POS] = GX_NONE;
  aurora::gfx::testing::use_draw_command_tracking(true);
  decode_fifo(draw(GX_TRIANGLES, 3));
  EXPECT_TRUE(aurora::gfx::testing::last_pushed_indices().empty());
  g_gxState.vtxDesc[GX_VA_POS] = GX_DIRECT;
  g_gxState.stateDirty = true;
  decode_fifo(draw(GX_TRIANGLES, 3));
  EXPECT_EQ(aurora::gfx::testing::last_pushed_indices(), (std::vector<u16>{0, 1, 2}));
}

// Material classes 1..15 share the texture tag byte with the other tags (bits 2-4 and 6); every class
// must round-trip and never touch the skin, SSAO or cover bits, nor the cache's "not hashed" sign bit.
TEST(TextureTags, MaterialClassBitsRoundTripWithoutTouchingOtherTags) {
  namespace tr = aurora::gfx::texture_replacement;
  constexpr uint8_t other = tr::TagNoSsao | tr::TagNoMetal | tr::TagSsaoCover;
  for (uint8_t cls = 0; cls < tr::MaterialClassCount; ++cls) {
    const uint8_t bits = tr::material_class_bits(cls);
    EXPECT_EQ(bits & ~tr::MaterialClassMask, 0) << int(cls);
    EXPECT_EQ(bits & (other | 0x80), 0) << int(cls);
    EXPECT_EQ(tr::material_class_of(bits), cls);
    EXPECT_EQ(tr::material_class_of(static_cast<uint8_t>(bits | other)), cls);
  }
  EXPECT_EQ(tr::material_class_of(other), 0);
}

// Environment material (world lighting level 1): the lightmap is found by the TEV structure of
// Prime's lightmapped world (F7 31/10/2026, Olympus), not by the UV set.
namespace {
aurora::gx::ShaderConfig lightmap_config(GXTevColorArg laterD) {
  using namespace aurora::gx;
  ShaderConfig config{};
  config.numTexGens = 2;
  config.tcgs[0].src = GX_TG_TEX0;
  config.tcgs[1].src = GX_TG_TEX1;
  config.tevStageCount = 2;
  // Stage 0: C0 = raster + lightmap * C1 (first UV set).
  auto& s0 = config.tevStages[0];
  s0.colorPass = {GX_CC_ZERO, GX_CC_TEXC, GX_CC_C1, GX_CC_RASC};
  s0.colorOp.outReg = GX_TEVREG0;
  s0.texCoordId = GX_TEXCOORD0;
  s0.texMapId = GX_TEXMAP0;
  // Stage 1: PREV = C0 * diffuse (+ laterD) (second UV set).
  auto& s1 = config.tevStages[1];
  s1.colorPass = {GX_CC_ZERO, GX_CC_C0, GX_CC_TEXC, laterD};
  s1.colorOp.outReg = GX_TEVPREV;
  s1.texCoordId = GX_TEXCOORD1;
  s1.texMapId = GX_TEXMAP1;
  return config;
}
} // namespace

TEST(EnvironmentMaterial, FindsPrimeLightmapStage) {
  EXPECT_EQ(aurora::gx::env_lightmap_stage(lightmap_config(GX_CC_ZERO)), 0);
}

TEST(EnvironmentMaterial, AddedMaskIsNotALightmap) {
  // PREV + texture * register: an emissive or detail mask added on top, not baked light.
  EXPECT_EQ(aurora::gx::env_lightmap_stage(lightmap_config(GX_CC_CPREV)), -1);
}

TEST(EnvironmentMaterial, PlainTexturedDrawHasNoLightmap) {
  auto config = lightmap_config(GX_CC_ZERO);
  config.tevStages[0].colorOp.outReg = GX_TEVPREV;  // straight into PREV: nothing multiplies it later
  EXPECT_EQ(aurora::gx::env_lightmap_stage(config), -1);
}

// The ubershader marker (pad1) must keep its historical bit: pipeline_cache.db rows written by older
// builds rely on it, and valid_pipeline_config drops persisted rows that carry it. The host variants
// use the remaining bit and an explicit padding byte, never the marker.
TEST(ShaderConfigLayout, UberMarkerKeepsItsBit) {
  using namespace aurora::gx;
  const auto flagsByte = [](const ShaderConfig& c) { return reinterpret_cast<const uint8_t*>(&c)[2]; };
  ShaderConfig uber{};
  uber.pad1 = 1;
  EXPECT_EQ(flagsByte(uber), 0x40);
  ShaderConfig env{};
  env.envMaterial = 1;
  EXPECT_EQ(flagsByte(env), 0x80);
  ShaderConfig soft{};
  set_shader_soft_shadow(soft);
  EXPECT_EQ(flagsByte(soft), 0);
  EXPECT_TRUE(shader_soft_shadow(soft));
  EXPECT_FALSE(shader_soft_shadow(ShaderConfig{}));
}
