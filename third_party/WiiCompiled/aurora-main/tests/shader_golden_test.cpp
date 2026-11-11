// Golden test of the specialised shader generator.
//
// Input: every GX pipeline recipe of the pipeline seed the port ships
// (runtime/assets/pipeline/RM3E01_00/initial_pipeline_cache.db): real draws of the real game.
// For each one the WGSL produced by gx::build_shader_source() is hashed and compared with
// tests/golden/shader_golden.txt, so a change to the generator that alters ANY of those shaders is
// caught here instead of in a player's cache or in a room that renders wrong.
//
// A mismatch is not always a bug: after an intended change, review the listed recipes and re-record:
//     UPDATE_GOLDEN=1 gx_fifo_tests --gtest_filter='ShaderGolden.*'
// An intended change also invalidates every player's shader cache for those pipelines (Dawn keys
// are content hashes): say so in the changelog.
#include "gx_test_common.hpp"
#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"
#include "gx/uber.hpp"

#include <sqlite3.h>
#include <xxhash.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#ifndef AURORA_SEED_DB
#define AURORA_SEED_DB ""
#endif
#ifndef AURORA_GOLDEN_FILE
#define AURORA_GOLDEN_FILE ""
#endif
#ifndef AURORA_UBER_GOLDEN_FILE
#define AURORA_UBER_GOLDEN_FILE ""
#endif

namespace {

struct Recipe {
  int64_t hash = 0;
  aurora::gx::PipelineConfig config{};
};

bool load_recipes(std::vector<Recipe>& out, std::string& why) {
  const std::filesystem::path path = AURORA_SEED_DB;
  std::error_code ec;
  if (path.empty() || !std::filesystem::exists(path, ec)) {
    why = "pipeline seed not found: " + path.string();
    return false;
  }
  sqlite3* db = nullptr;
  const std::string uri = "file:" + path.generic_string() + "?mode=ro";
  if (sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
    why = "cannot open the seed";
    sqlite3_close(db);
    return false;
  }
  sqlite3_stmt* stmt = nullptr;
  const auto sql = std::string("SELECT hash, config FROM pipeline_cache WHERE type = 1 AND config_version = ") +
                   std::to_string(aurora::gx::GXPipelineConfigVersion) + " ORDER BY hash";
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    why = "seed has no pipeline_cache table";
    sqlite3_close(db);
    return false;
  }
  size_t wrongSize = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (static_cast<size_t>(sqlite3_column_bytes(stmt, 1)) != sizeof(aurora::gx::PipelineConfig)) {
      ++wrongSize;
      continue;
    }
    Recipe recipe;
    recipe.hash = sqlite3_column_int64(stmt, 0);
    std::memcpy(&recipe.config, sqlite3_column_blob(stmt, 1), sizeof(recipe.config));
    out.push_back(recipe);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  if (wrongSize != 0) {
    why = std::to_string(wrongSize) + " recipes have a different size than PipelineConfig: bump the seed (merge) after a config change";
    return false;
  }
  return true;
}

uint64_t source_hash(const std::string& source) { return XXH3_64bits(source.data(), source.size()); }

}  // namespace

TEST(ShaderGolden, GeneratorOutputIsStableForEveryShippedRecipe) {
  std::vector<Recipe> recipes;
  std::string why;
  if (!load_recipes(recipes, why)) GTEST_SKIP() << why;
  ASSERT_GT(recipes.size(), 500u) << "the seed should hold the real game's pipelines";

  struct Entry { uint64_t hash; size_t length; };
  std::map<int64_t, Entry> actual;
  size_t invalid = 0;
  for (const auto& recipe : recipes) {
    if (!aurora::gx::valid_pipeline_config(recipe.config)) {
      ++invalid;
      continue;
    }
    const auto source = aurora::gx::build_shader_source(recipe.config.shaderConfig);
    ASSERT_FALSE(source.empty()) << "recipe " << recipe.hash;
    actual[recipe.hash] = {source_hash(source), source.size()};
    // AURORA_DUMP_SHADER_DIR=<dir>: write the enhanced-metal variant of each per-pixel-lit recipe
    // there (the runtime turns enhancedMetal on; the seed stores it off), for review or validation.
    if (const char* dir = std::getenv("AURORA_DUMP_SHADER_DIR"); dir && recipe.config.shaderConfig.perPixelLighting != 0) {
      auto metal = recipe.config.shaderConfig;
      metal.enhancedMetal = 1;
      std::ofstream(std::filesystem::path(dir) / (std::to_string(recipe.hash) + ".wgsl")) << aurora::gx::build_shader_source(metal);
    }
    // Same for the environment-material variant of each lightmapped recipe (the runtime sets
    // envMaterial on unlit world draws with a lightmap and normals).
    if (const char* dir = std::getenv("AURORA_DUMP_SHADER_DIR");
        dir && aurora::gx::env_lightmap_stage(recipe.config.shaderConfig) >= 0 &&
        recipe.config.shaderConfig.attrs[GX_VA_NRM].attrType != GX_NONE) {
      auto env = recipe.config.shaderConfig;
      env.envMaterial = 1;
      if (aurora::gx::build_shader_info(env).usesEnvLights)
        std::ofstream(std::filesystem::path(dir) / ("env_" + std::to_string(recipe.hash) + ".wgsl"))
            << aurora::gx::build_shader_source(env);
    }
    // And the soft projected shadow variant of each recipe sampling through a position texgen.
    if (const char* dir = std::getenv("AURORA_DUMP_SHADER_DIR");
        dir && aurora::gx::shadow_projection_stage(recipe.config.shaderConfig) >= 0) {
      auto soft = recipe.config.shaderConfig;
      aurora::gx::set_shader_soft_shadow(soft);
      std::ofstream(std::filesystem::path(dir) / ("soft_" + std::to_string(recipe.hash) + ".wgsl"))
          << aurora::gx::build_shader_source(soft);
    }
  }
  EXPECT_EQ(invalid, 0u) << "the shipped seed must not contain recipes the loader rejects";

  const std::filesystem::path goldenPath = AURORA_GOLDEN_FILE;
  if (std::getenv("UPDATE_GOLDEN")) {
    std::ofstream out(goldenPath, std::ios::trunc);
    out << "# shader golden: <recipe hash> <xxh3-64 of the WGSL> <WGSL bytes>; GXPipelineConfigVersion "
        << aurora::gx::GXPipelineConfigVersion << ", " << actual.size() << " recipes\n";
    for (const auto& [hash, entry] : actual) {
      char line[96];
      std::snprintf(line, sizeof(line), "%lld %016llx %zu\n", static_cast<long long>(hash),
                    static_cast<unsigned long long>(entry.hash), entry.length);
      out << line;
    }
    GTEST_SKIP() << "golden file rewritten with " << actual.size() << " recipes: " << goldenPath;
  }

  std::ifstream in(goldenPath);
  ASSERT_TRUE(in.good()) << "missing " << goldenPath << " (record it with UPDATE_GOLDEN=1)";
  std::map<int64_t, Entry> golden;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    long long hash = 0;
    unsigned long long digest = 0;
    size_t length = 0;
    if (std::sscanf(line.c_str(), "%lld %llx %zu", &hash, &digest, &length) == 3) golden[hash] = {digest, length};
  }
  ASSERT_GT(golden.size(), 500u);

  std::vector<std::string> changed;
  size_t compared = 0, unrecorded = 0;
  for (const auto& [hash, entry] : actual) {
    const auto it = golden.find(hash);
    if (it == golden.end()) {
      ++unrecorded;  // a recipe merged into the seed after the last recording: not a regression
      continue;
    }
    ++compared;
    if (it->second.hash != entry.hash || it->second.length != entry.length) {
      char text[120];
      std::snprintf(text, sizeof(text), "recipe %lld: %zu bytes -> %zu bytes", static_cast<long long>(hash),
                    it->second.length, entry.length);
      changed.push_back(text);
    }
  }
  EXPECT_GT(compared, golden.size() / 2) << "most of the recorded recipes should still be in the seed";
  std::ostringstream report;
  for (size_t i = 0; i < changed.size() && i < 12; ++i) report << "\n  " << changed[i];
  EXPECT_TRUE(changed.empty()) << changed.size() << " of " << compared
                               << " shaders changed since the golden was recorded (" << unrecorded
                               << " newer recipes are not recorded yet):" << report.str()
                               << "\nIf the change is intended, re-record with UPDATE_GOLDEN=1 and note that every "
                                  "player's shader cache for these pipelines is invalidated.";
}

TEST(ShaderGolden, GenerationIsDeterministic) {
  std::vector<Recipe> recipes;
  std::string why;
  if (!load_recipes(recipes, why)) GTEST_SKIP() << why;
  ASSERT_GT(recipes.size(), 100u);
  // The same recipe must give the same source twice and must not depend on whatever draw state the process
  // happens to hold: a cache keyed on the source would otherwise miss at random.
  const auto step = std::max<size_t>(1, recipes.size() / 150);
  for (size_t i = 0; i < recipes.size(); i += step) {
    aurora::gx::g_gxState = aurora::gx::GXState{};
    const auto first = aurora::gx::build_shader_source(recipes[i].config.shaderConfig);
    aurora::gx::g_gxState.proj = {};
    aurora::gx::g_gxState.currentPnMtx = 3;
    aurora::gx::g_gxState.logicalViewport = {0, 0, 321, 123};
    aurora::gx::g_gxState.renderViewport = {0, 0, 1920, 1080};
    const auto second = aurora::gx::build_shader_source(recipes[i].config.shaderConfig);
    ASSERT_EQ(first, second) << "recipe " << recipes[i].hash << " depends on global draw state";
  }
}

TEST(ShaderGolden, EveryShippedRecipeNeedsAUniformBufferWithinTheLimit) {
  std::vector<Recipe> recipes;
  std::string why;
  if (!load_recipes(recipes, why)) GTEST_SKIP() << why;
  uint32_t largest = 0;
  for (const auto& recipe : recipes) {
    const auto info = aurora::gx::build_shader_info(recipe.config.shaderConfig);
    EXPECT_LE(info.uniformSize, aurora::gx::MaxUniformSize) << recipe.hash;
    largest = std::max(largest, static_cast<uint32_t>(info.uniformSize));
  }
  RecordProperty("largest_uniform_bytes", static_cast<int>(largest));
}

// Modern lighting (docs/iluminacion_motor.md): the environment-material variant of every shipped
// lightmapped recipe carries eight engine lights (env_lights) and must still fit the uniform limit.
TEST(ShaderGolden, EnvironmentVariantWithEightEngineLightsFitsTheUniformLimit) {
  std::vector<Recipe> recipes;
  std::string why;
  if (!load_recipes(recipes, why)) GTEST_SKIP() << why;
  uint32_t largest = 0, variants = 0;
  for (const auto& recipe : recipes) {
    if (aurora::gx::env_lightmap_stage(recipe.config.shaderConfig) < 0) continue;
    auto env = recipe.config.shaderConfig;
    env.envMaterial = 1;
    const auto info = aurora::gx::build_shader_info(env);
    if (!info.usesEnvLights) continue;
    ++variants;
    EXPECT_LE(info.uniformSize, aurora::gx::MaxUniformSize) << recipe.hash;
    largest = std::max(largest, static_cast<uint32_t>(info.uniformSize));
    const auto source = aurora::gx::build_shader_source(env);
    EXPECT_NE(source.find("env_lights: array<Light, 8>"), std::string::npos) << recipe.hash;
    EXPECT_NE(source.find("fn env_gate("), std::string::npos) << recipe.hash;
  }
  EXPECT_GT(variants, 0u);
  RecordProperty("largest_env_uniform_bytes", static_cast<int>(largest));
}

// --------------------------------------------------------------------------------------------------
// Ubershader (docs/ubershader_diseno.md)
namespace {
struct UberRecipe {
  aurora::gx::UberConfig key;
  std::string source;
};

// One entry per distinct uber key of the shipped seed, keyed by the hash of the key.
bool build_uber_set(std::map<uint64_t, UberRecipe>& out, size_t& recipeCount, std::string& why) {
  std::vector<Recipe> recipes;
  if (!load_recipes(recipes, why)) return false;
  recipeCount = recipes.size();
  for (const auto& recipe : recipes) {
    if (!aurora::gx::valid_pipeline_config(recipe.config)) continue;
    const auto info = aurora::gx::build_shader_info(recipe.config.shaderConfig);
    const auto key = aurora::gx::make_uber_config(recipe.config, info);
    const uint64_t hash = aurora::xxh3_hash(key);
    if (out.contains(hash)) continue;
    out[hash] = {key, aurora::gx::build_uber_shader_source(key)};
  }
  return true;
}
}  // namespace

TEST(UberShader, EveryShippedRecipeHasAnUberKeyThatFitsTheUniformBuffer) {
  std::map<uint64_t, UberRecipe> set;
  size_t recipes = 0;
  std::string why;
  if (!build_uber_set(set, recipes, why)) GTEST_SKIP() << why;
  ASSERT_GT(set.size(), 10u);
  size_t tooBig = 0;
  uint32_t largest = 0;
  for (const auto& [hash, recipe] : set) {
    bool ok = false;
    const auto info = aurora::gx::build_uber_shader_info(recipe.key, &ok);
    largest = std::max(largest, static_cast<uint32_t>(info.uniformSize));
    if (!ok) ++tooBig;
    EXPECT_TRUE(info.uber);
  }
  // A material whose uniform block does not fit simply has no ubershader (it waits for its specialised pipeline).
  EXPECT_LE(tooBig * 20, set.size()) << tooBig << " uber keys exceed MaxUniformSize";
  std::set<uint64_t> modules;
  for (const auto& [hash, recipe] : set) {
    struct Module { aurora::gx::ShaderConfig shader; uint8_t masks[4]; } m{};
    m.shader = recipe.key.pipeline.shaderConfig;
    m.masks[0] = recipe.key.texCoords; m.masks[1] = recipe.key.textures; m.masks[2] = recipe.key.colorChannels; m.masks[3] = recipe.key.hasIndirect;
    modules.insert(XXH3_64bits(&m, sizeof(m)));
  }
  RecordProperty("distinct_uber_modules", static_cast<int>(modules.size()));
  std::printf("[uber-stats] recipes=%zu uber_keys=%zu uber_modules=%zu too_big=%zu largest=%u\n", recipes, set.size(), modules.size(), tooBig, largest);
  RecordProperty("recipes", static_cast<int>(recipes));
  RecordProperty("distinct_uber_keys", static_cast<int>(set.size()));
  RecordProperty("largest_uber_uniform_bytes", static_cast<int>(largest));
  // The point of the design: far fewer ubershaders than specialised ones.
  EXPECT_LT(set.size() * 2, recipes) << set.size() << " uber keys for " << recipes << " recipes";
}

TEST(UberShader, SourceIsDeterministicAndStructurallySound) {
  std::map<uint64_t, UberRecipe> set;
  size_t recipes = 0;
  std::string why;
  if (!build_uber_set(set, recipes, why)) GTEST_SKIP() << why;
  for (const auto& [hash, recipe] : set) {
    const auto& src = recipe.source;
    ASSERT_FALSE(src.empty());
    EXPECT_EQ(src, aurora::gx::build_uber_shader_source(recipe.key)) << "key " << hash;
    int depth = 0;
    for (const char c : src) {
      depth += (c == '{') - (c == '}');
      ASSERT_GE(depth, 0) << "unbalanced braces, key " << hash;
    }
    EXPECT_EQ(depth, 0) << "unbalanced braces, key " << hash;
    int parens = 0;
    for (const char c : src) {
      parens += (c == '(') - (c == ')');
      ASSERT_GE(parens, 0) << "unbalanced parentheses, key " << hash;
    }
    EXPECT_EQ(parens, 0) << "key " << hash;
    EXPECT_NE(src.find("uber_stages: array<vec4u, 16>"), std::string::npos);
    EXPECT_NE(src.find("fn fs_main"), std::string::npos);
    EXPECT_EQ(src.find("TEV stage 0"), std::string::npos) << "a specialised stage leaked into the ubershader";
    EXPECT_EQ(src.find("@NOTEX@"), std::string::npos);
  }
}

TEST(UberShader, SpecialisedMaterialsWithDifferentTevShareAnUberShader) {
  std::vector<Recipe> recipes;
  std::string why;
  if (!load_recipes(recipes, why)) GTEST_SKIP() << why;
  // Two recipes that differ only in their TEV program must collapse into one key.
  std::map<uint64_t, std::set<uint64_t>> specialisedPerKey;
  for (const auto& recipe : recipes) {
    if (!aurora::gx::valid_pipeline_config(recipe.config)) continue;
    const auto info = aurora::gx::build_shader_info(recipe.config.shaderConfig);
    specialisedPerKey[aurora::xxh3_hash(aurora::gx::make_uber_config(recipe.config, info))].insert(
        aurora::xxh3_hash(recipe.config.shaderConfig));
  }
  size_t shared = 0;
  for (const auto& [key, specialised] : specialisedPerKey) shared += specialised.size() > 1;
  EXPECT_GT(shared, 0u);
}

TEST(UberShader, GoldenHashesOfEveryUberKey) {
  std::map<uint64_t, UberRecipe> set;
  size_t recipes = 0;
  std::string why;
  if (!build_uber_set(set, recipes, why)) GTEST_SKIP() << why;
  const std::filesystem::path goldenPath = AURORA_UBER_GOLDEN_FILE;
  if (std::getenv("UPDATE_GOLDEN")) {
    std::ofstream out(goldenPath, std::ios::trunc);
    out << "# uber shader golden: <uber key hash> <xxh3-64 of the WGSL> <WGSL bytes>; GXPipelineConfigVersion "
        << aurora::gx::GXPipelineConfigVersion << ", " << set.size() << " keys\n";
    for (const auto& [hash, recipe] : set) {
      char line[96];
      std::snprintf(line, sizeof(line), "%016llx %016llx %zu\n", static_cast<unsigned long long>(hash),
                    static_cast<unsigned long long>(source_hash(recipe.source)), recipe.source.size());
      out << line;
    }
    GTEST_SKIP() << "uber golden rewritten with " << set.size() << " keys";
  }
  std::ifstream in(goldenPath);
  ASSERT_TRUE(in.good()) << "missing " << goldenPath << " (record it with UPDATE_GOLDEN=1)";
  std::map<uint64_t, std::pair<uint64_t, size_t>> golden;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    unsigned long long key = 0, digest = 0;
    size_t length = 0;
    if (std::sscanf(line.c_str(), "%llx %llx %zu", &key, &digest, &length) == 3) golden[key] = {digest, length};
  }
  std::vector<std::string> changed;
  size_t compared = 0;
  for (const auto& [hash, recipe] : set) {
    const auto it = golden.find(hash);
    if (it == golden.end()) continue;
    ++compared;
    if (it->second.first != source_hash(recipe.source) || it->second.second != recipe.source.size()) {
      char text[96];
      std::snprintf(text, sizeof(text), "uber key %016llx", static_cast<unsigned long long>(hash));
      changed.push_back(text);
    }
  }
  EXPECT_GT(compared, golden.size() / 2);
  EXPECT_TRUE(changed.empty()) << changed.size() << " of " << compared << " ubershaders changed; re-record with UPDATE_GOLDEN=1";
}

// --------------------------------------------------------------------------------------------------
// Ubershader key invariants (uber.hpp)
namespace {
aurora::gx::PipelineConfig minimal_real_recipe() {
  aurora::gx::PipelineConfig config{};
  config.shaderConfig.attrs[GX_VA_POS].attrType = GX_DIRECT;
  config.shaderConfig.attrs[GX_VA_POS].cnt = 3;
  config.shaderConfig.attrs[GX_VA_POS].compType = GX_F32;
  config.shaderConfig.tevStageCount = 1;
  config.depthFunc = GX_LEQUAL;
  config.cullMode = GX_CULL_BACK;
  config.blendMode = GX_BM_NONE;
  config.blendFacSrc = GX_BL_ONE;
  config.blendFacDst = GX_BL_ZERO;
  config.blendOp = GX_LO_COPY;
  config.pixelFmt = GX_PF_RGB8_Z24;
  config.dstAlpha = UINT32_MAX;
  config.shaderConfig.dstAlpha = UINT32_MAX;
  return config;
}
}  // namespace

TEST(UberShader, KeyRoundTripsAndIsNeverAValidRecipe) {
  const auto real = minimal_real_recipe();
  ASSERT_TRUE(aurora::gx::valid_pipeline_config(real));
  const auto info = aurora::gx::build_shader_info(real.shaderConfig);
  const auto key = aurora::gx::make_uber_config(real, info);
  const auto encoded = aurora::gx::encode_uber_pipeline(key);
  EXPECT_TRUE(aurora::gx::is_uber_pipeline(encoded));
  EXPECT_FALSE(aurora::gx::is_uber_pipeline(real));
  // The loader, the seed merge and the golden tests must all refuse it: its TEV program is empty.
  EXPECT_FALSE(aurora::gx::valid_pipeline_config(encoded));
  const auto decoded = aurora::gx::decode_uber_pipeline(encoded);
  EXPECT_EQ(std::memcmp(&decoded, &key, sizeof(key)), 0);
  EXPECT_EQ(decoded.pipeline.shaderConfig.tevStageCount, 0u);
}

TEST(UberShader, DisabledAlphaCompareNeverDiscards) {
  const auto real = minimal_real_recipe();
  const auto data = aurora::gx::make_uber_data(real.shaderConfig);
  EXPECT_EQ(data.acmp[0], static_cast<uint32_t>(GX_ALWAYS));
  EXPECT_EQ(data.acmp[2], static_cast<uint32_t>(GX_ALWAYS));
  EXPECT_EQ(data.misc1[1], static_cast<uint32_t>(GX_AOP_AND));
  EXPECT_EQ(data.misc0[0], 1u);  // stage count
}

TEST(UberShader, StageWordsFollowTheMaterial) {
  auto real = minimal_real_recipe();
  auto& stage = real.shaderConfig.tevStages[0];
  stage.colorPass = {GX_CC_TEXC, GX_CC_RASC, GX_CC_ONE, GX_CC_ZERO};
  stage.alphaPass = {GX_CA_TEXA, GX_CA_RASA, GX_CA_KONST, GX_CA_ZERO};
  stage.colorOp.outReg = GX_TEVREG1;
  stage.alphaOp.outReg = GX_TEVREG2;
  stage.alphaOp.clamp = false;
  real.shaderConfig.alphaCompare = {GX_GREATER, 128, GX_AOP_OR, GX_LESS, 7};
  const auto data = aurora::gx::make_uber_data(real.shaderConfig);
  const uint32_t w0 = data.stages[0];
  EXPECT_EQ(w0 & 15u, static_cast<uint32_t>(GX_CC_TEXC));
  EXPECT_EQ((w0 >> 4u) & 15u, static_cast<uint32_t>(GX_CC_RASC));
  EXPECT_EQ((w0 >> 22u) & 7u, static_cast<uint32_t>(GX_CA_KONST));
  const uint32_t ops = data.stages[1];
  EXPECT_EQ((ops >> 9u) & 3u, static_cast<uint32_t>(GX_TEVREG1));          // colour out
  EXPECT_EQ(((ops >> 11u) >> 9u) & 3u, static_cast<uint32_t>(GX_TEVREG2));  // alpha out
  EXPECT_EQ(((ops >> 11u) >> 8u) & 1u, 0u);                                  // alpha clamp off
  // Last stage outputs: GX_TEVREG1 = 2 and GX_TEVREG2 = 3 in the GX enum (TEVPREV = 0).
  EXPECT_EQ(data.misc0[3], static_cast<uint32_t>(GX_TEVREG1) | (static_cast<uint32_t>(GX_TEVREG2) << 4));
  EXPECT_EQ(data.acmp[0], static_cast<uint32_t>(GX_GREATER));
  EXPECT_EQ(data.acmp[1], 128u);
  EXPECT_EQ(data.acmp[3], 7u);
  EXPECT_EQ(data.misc1[1], static_cast<uint32_t>(GX_AOP_OR));
}

// --------------------------------------------------------------------------------------------------
// The packed TEV program must say exactly what the material says: a C++ mirror of the WGSL decoder.
namespace {
struct DecodedStage {
  uint32_t ca, cb, cc, cd, aa, ab, ac, ad;
  uint32_t cop, cbias, cscale, cclamp, cout;
  uint32_t aop, abias, ascale, aclamp, aout;
  int texCoord, texMap;
  uint32_t kind, swapRas, swapTex;
  bool canSample, needsFixed, willSample, hasIndirect, addPrev, hasIndStage;
  uint32_t indStage, indFormat, indBias, indAlpha, indMtx, wrapS, wrapT;
};
DecodedStage decode_stage(const aurora::gx::UberData& d, uint32_t i) {
  const uint32_t w0 = d.stages[i * 4], w1 = d.stages[i * 4 + 1] & 0x7FFu, w2 = (d.stages[i * 4 + 1] >> 11) & 0x7FFu,
                 w3 = d.stages[i * 4 + 2], w4 = d.stages[i * 4 + 3];
  DecodedStage s{};
  s.ca = w0 & 15u; s.cb = (w0 >> 4) & 15u; s.cc = (w0 >> 8) & 15u; s.cd = (w0 >> 12) & 15u;
  s.aa = (w0 >> 16) & 7u; s.ab = (w0 >> 19) & 7u; s.ac = (w0 >> 22) & 7u; s.ad = (w0 >> 25) & 7u;
  s.cop = w1 & 15u; s.cbias = (w1 >> 4) & 3u; s.cscale = (w1 >> 6) & 3u; s.cclamp = (w1 >> 8) & 1u; s.cout = (w1 >> 9) & 3u;
  s.aop = w2 & 15u; s.abias = (w2 >> 4) & 3u; s.ascale = (w2 >> 6) & 3u; s.aclamp = (w2 >> 8) & 1u; s.aout = (w2 >> 9) & 3u;
  s.texCoord = int(w3 & 15u) - 1; s.texMap = int((w3 >> 4) & 15u) - 1; s.kind = (w3 >> 8) & 7u;
  s.swapRas = (w3 >> 12) & 3u; s.swapTex = (w3 >> 14) & 3u;
  s.canSample = (w3 >> 16) & 1u; s.needsFixed = (w3 >> 17) & 1u; s.willSample = (w3 >> 18) & 1u;
  s.hasIndirect = (w3 >> 19) & 1u; s.addPrev = (w3 >> 20) & 1u; s.hasIndStage = (w3 >> 21) & 1u;
  s.indStage = w4 & 3u; s.indFormat = (w4 >> 2) & 3u; s.indBias = (w4 >> 4) & 7u; s.indAlpha = (w4 >> 7) & 3u;
  s.indMtx = (w4 >> 9) & 15u; s.wrapS = (w4 >> 13) & 7u; s.wrapT = (w4 >> 16) & 7u;
  return s;
}
}  // namespace

TEST(UberShader, PackedStagesMirrorEveryShippedMaterial) {
  std::vector<Recipe> recipes;
  std::string why;
  if (!load_recipes(recipes, why)) GTEST_SKIP() << why;
  size_t stagesChecked = 0;
  for (const auto& recipe : recipes) {
    if (!aurora::gx::valid_pipeline_config(recipe.config)) continue;
    const auto& c = recipe.config.shaderConfig;
    const auto d = aurora::gx::make_uber_data(c);
    ASSERT_EQ(d.misc0[0], c.tevStageCount) << recipe.hash;
    for (uint32_t i = 0; i < c.tevStageCount; ++i) {
      const auto& st = c.tevStages[i];
      const auto dep = aurora::gx::tev_stage_texture_dependency(c, i);
      const auto s = decode_stage(d, i);
      const std::string at = "recipe " + std::to_string(recipe.hash) + " stage " + std::to_string(i);
      EXPECT_EQ(s.ca, uint32_t(st.colorPass.a)) << at; EXPECT_EQ(s.cb, uint32_t(st.colorPass.b)) << at;
      EXPECT_EQ(s.cc, uint32_t(st.colorPass.c)) << at; EXPECT_EQ(s.cd, uint32_t(st.colorPass.d)) << at;
      EXPECT_EQ(s.aa, uint32_t(st.alphaPass.a)) << at; EXPECT_EQ(s.ab, uint32_t(st.alphaPass.b)) << at;
      EXPECT_EQ(s.ac, uint32_t(st.alphaPass.c)) << at; EXPECT_EQ(s.ad, uint32_t(st.alphaPass.d)) << at;
      EXPECT_EQ(s.cop, uint32_t(st.colorOp.op)) << at; EXPECT_EQ(s.cbias, uint32_t(st.colorOp.bias)) << at;
      EXPECT_EQ(s.cscale, uint32_t(st.colorOp.scale)) << at; EXPECT_EQ(s.cclamp, st.colorOp.clamp ? 1u : 0u) << at;
      EXPECT_EQ(s.cout, uint32_t(st.colorOp.outReg)) << at;
      EXPECT_EQ(s.aop, uint32_t(st.alphaOp.op)) << at; EXPECT_EQ(s.abias, uint32_t(st.alphaOp.bias)) << at;
      EXPECT_EQ(s.ascale, uint32_t(st.alphaOp.scale)) << at; EXPECT_EQ(s.aclamp, st.alphaOp.clamp ? 1u : 0u) << at;
      EXPECT_EQ(s.aout, uint32_t(st.alphaOp.outReg)) << at;
      EXPECT_EQ(s.texCoord, dep.texCoordId) << at; EXPECT_EQ(s.texMap, dep.texMapId) << at;
      EXPECT_EQ(s.canSample, dep.canSampleTexture) << at; EXPECT_EQ(s.needsFixed, dep.needsFixedTexcoordState) << at;
      EXPECT_EQ(s.swapRas, uint32_t(st.tevSwapRas)) << at; EXPECT_EQ(s.swapTex, uint32_t(st.tevSwapTex)) << at;
      EXPECT_EQ(s.addPrev, st.indTexAddPrev) << at;
      EXPECT_EQ(s.hasIndStage, st.indTexStage < c.numIndStages) << at;
      EXPECT_EQ(s.hasIndirect, st.indTexMtxId != GX_ITM_OFF && st.indTexStage < c.numIndStages) << at;
      EXPECT_EQ(s.indStage, uint32_t(st.indTexStage) & 3u) << at; EXPECT_EQ(s.indFormat, uint32_t(st.indTexFormat)) << at;
      EXPECT_EQ(s.indBias, uint32_t(st.indTexBiasSel)) << at; EXPECT_EQ(s.indAlpha, uint32_t(st.indTexAlphaSel)) << at;
      EXPECT_EQ(s.indMtx, uint32_t(st.indTexMtxId)) << at; EXPECT_EQ(s.wrapS, uint32_t(st.indTexWrapS)) << at;
      EXPECT_EQ(s.wrapT, uint32_t(st.indTexWrapT)) << at;
      ++stagesChecked;
    }
    for (uint32_t i = 0; i < aurora::gx::MaxTevSwap; ++i) {
      const auto& sw = c.tevSwapTable[i];
      EXPECT_EQ(d.swaps[i], uint32_t(sw.red) | (uint32_t(sw.green) << 2) | (uint32_t(sw.blue) << 4) | (uint32_t(sw.alpha) << 6)) << recipe.hash;
    }
    if (c.alphaCompare) {
      EXPECT_EQ(d.acmp[0], uint32_t(c.alphaCompare.comp0)) << recipe.hash; EXPECT_EQ(d.acmp[1], c.alphaCompare.ref0 & 0xFFu) << recipe.hash;
      EXPECT_EQ(d.acmp[2], uint32_t(c.alphaCompare.comp1)) << recipe.hash; EXPECT_EQ(d.acmp[3], c.alphaCompare.ref1 & 0xFFu) << recipe.hash;
      EXPECT_EQ(d.misc1[1], uint32_t(c.alphaCompare.op)) << recipe.hash;
    } else {
      EXPECT_EQ(d.acmp[0], uint32_t(GX_ALWAYS)) << recipe.hash; EXPECT_EQ(d.acmp[2], uint32_t(GX_ALWAYS)) << recipe.hash;
    }
    EXPECT_EQ(d.misc0[2], uint32_t(aurora::gx::tev_z_texture_stage(c) + 1)) << recipe.hash;
  }
  EXPECT_GT(stagesChecked, 5000u);
}

// The ubershader shares the vertex stage and the uniform prefix with the specialised shader: every
// field of ShaderInfo that shapes them must agree, or a draw's uniform block would be read misaligned.
TEST(UberShader, UniformLayoutAgreesWithTheSpecialisedShader) {
  std::vector<Recipe> recipes;
  std::string why;
  if (!load_recipes(recipes, why)) GTEST_SKIP() << why;
  size_t checked = 0, sameTexCoords = 0;
  for (const auto& recipe : recipes) {
    if (!aurora::gx::valid_pipeline_config(recipe.config)) continue;
    const auto spec = aurora::gx::build_shader_info(recipe.config.shaderConfig);
    const auto key = aurora::gx::make_uber_config(recipe.config, spec);
    bool fits = false;
    const auto uber = aurora::gx::build_uber_shader_info(key, &fits);
    const std::string at = "recipe " + std::to_string(recipe.hash);
    EXPECT_TRUE(uber.uber) << at;
    // The uber key may declare texcoords the material's TEV never samples (fewer keys); the matrix
    // layout and post-transform matrices then follow its own, larger set, consistently on both the
    // WGSL and the uniform side. Where the sets coincide, the layouts must be identical.
    if (uber.sampledTexCoords == spec.sampledTexCoords) {
      EXPECT_EQ(std::memcmp(&uber.matrixLayout, &spec.matrixLayout, sizeof(spec.matrixLayout)), 0) << at;
      EXPECT_EQ(uber.usesPTTexMtx, spec.usesPTTexMtx) << at;
      ++sameTexCoords;
    } else {
      EXPECT_EQ((uber.usesPTTexMtx & spec.usesPTTexMtx), spec.usesPTTexMtx) << at;
    }
    EXPECT_EQ(uber.indexAttr, spec.indexAttr) << at;
    EXPECT_EQ(uber.lineMode, spec.lineMode) << at;
    EXPECT_EQ(uber.lightingEnabled, spec.lightingEnabled) << at;
    EXPECT_EQ(uber.sampledColorChannels, spec.sampledColorChannels) << at;
    EXPECT_EQ(uber.usesFog, spec.usesFog) << at;
    EXPECT_EQ(uber.usesDstAlpha, spec.usesDstAlpha) << at;
    EXPECT_EQ((uber.sampledTexCoords & spec.sampledTexCoords), spec.sampledTexCoords) << at << ": a texcoord the material samples is missing";
    EXPECT_EQ((uber.sampledTextures & spec.sampledTextures), spec.sampledTextures) << at;
    if (fits) EXPECT_LE(uber.uniformSize, aurora::gx::MaxUniformSize) << at;
    EXPECT_GE(uber.uniformSize, spec.uniformSize) << at;
    ++checked;
  }
  EXPECT_GT(checked, 500u);
  EXPECT_GT(sameTexCoords, 100u) << "most materials sample every texcoord they define";
}

TEST(UberShader, EncodedKeysAreRefusedAsRecipesForEveryShippedMaterial) {
  std::vector<Recipe> recipes;
  std::string why;
  if (!load_recipes(recipes, why)) GTEST_SKIP() << why;
  for (const auto& recipe : recipes) {
    if (!aurora::gx::valid_pipeline_config(recipe.config)) continue;
    const auto info = aurora::gx::build_shader_info(recipe.config.shaderConfig);
    const auto encoded = aurora::gx::encode_uber_pipeline(aurora::gx::make_uber_config(recipe.config, info));
    ASSERT_FALSE(aurora::gx::valid_pipeline_config(encoded)) << recipe.hash;  // what the loader and the seed merge rely on
  }
}
