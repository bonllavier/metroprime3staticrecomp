// Unit tests of the pure renderer decisions in lib/gx/render_policy.hpp.
#include <gtest/gtest.h>

#include "../lib/gx/render_policy.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../include/aurora/host_cores.h"

namespace pol = aurora::gx::policy;

TEST(RenderPolicyAnisotropy, ForcedValueOverridesWhateverTheGameAsks) {
  for (const auto request : {pol::AnisoRequest::One, pol::AnisoRequest::Two, pol::AnisoRequest::Four, pol::AnisoRequest::Max}) {
    EXPECT_EQ(pol::effective_anisotropy(16, 16, request), 16);
    EXPECT_EQ(pol::effective_anisotropy(4, 16, request), 4);
    EXPECT_EQ(pol::effective_anisotropy(2, 16, request), 2);
  }
  EXPECT_EQ(pol::effective_anisotropy(64, 16, pol::AnisoRequest::One), 16) << "never above what WebGPU accepts";
}

TEST(RenderPolicyAnisotropy, WithoutOverrideTheGamesRequestScalesTheCap) {
  EXPECT_EQ(pol::effective_anisotropy(0, 16, pol::AnisoRequest::One), 1);   // what Prime asks for nearly everywhere
  EXPECT_EQ(pol::effective_anisotropy(1, 16, pol::AnisoRequest::One), 1);
  EXPECT_EQ(pol::effective_anisotropy(0, 16, pol::AnisoRequest::Two), 8);
  EXPECT_EQ(pol::effective_anisotropy(0, 16, pol::AnisoRequest::Four), 16);
  EXPECT_EQ(pol::effective_anisotropy(0, 16, pol::AnisoRequest::Max), 1);
  EXPECT_EQ(pol::effective_anisotropy(0, 1, pol::AnisoRequest::Two), 1) << "a cap of 1 never yields 0";
  EXPECT_EQ(pol::effective_anisotropy(0, 0, pol::AnisoRequest::Four), 1);
}

TEST(RenderPolicyTexOffset, NamedValuesAreFractionsOfATexel) {
  EXPECT_EQ(pol::tex_offset_value(0), 0.f);
  EXPECT_EQ(pol::tex_offset_value(1), 1.f / 16.f);
  EXPECT_EQ(pol::tex_offset_value(2), 1.f / 8.f);
  EXPECT_EQ(pol::tex_offset_value(3), 1.f / 4.f);
  EXPECT_EQ(pol::tex_offset_value(4), 1.f / 2.f);
  EXPECT_EQ(pol::tex_offset_value(5), 1.f);
}

TEST(RenderPolicyTexOffset, UndefinedValuesNeverAbort) {
  // "invalid tex offset 6" ended a late-game session with a fatal error dialog.
  EXPECT_EQ(pol::tex_offset_value(6), 1.f);
  EXPECT_EQ(pol::tex_offset_value(7), 1.f);
  EXPECT_EQ(pol::tex_offset_value(0xFFFFFFFFu), 1.f);
}

TEST(RenderPolicySlots, RecyclesTheLeastRecentlyScheduledIdleSlot) {
  std::vector<pol::SlotCandidate> slots{{true, 50}, {true, 10}, {true, 30}};
  EXPECT_EQ(pol::pick_recycle_slot(slots), 1u);
}

TEST(RenderPolicySlots, NeverRecyclesASlotWithAReadbackInFlight) {
  std::vector<pol::SlotCandidate> slots{{false, 1}, {true, 40}, {false, 2}, {true, 90}};
  EXPECT_EQ(pol::pick_recycle_slot(slots), 1u);  // the oldest overall (index 0) is busy
}

TEST(RenderPolicySlots, ReportsWhenEverySlotIsBusyOrThereAreNone) {
  EXPECT_EQ(pol::pick_recycle_slot({}), SIZE_MAX);
  EXPECT_EQ(pol::pick_recycle_slot({{false, 5}, {false, 9}}), SIZE_MAX);
}

namespace {
std::vector<pol::CopyDestination> destinations(std::initializer_list<std::pair<uint32_t, uint64_t>> idleAndBytes) {
  std::vector<pol::CopyDestination> out;
  uintptr_t id = 0x1000;
  for (const auto& [idle, bytes] : idleAndBytes) out.push_back({reinterpret_cast<const void*>(id += 0x10), idle, bytes});
  return out;
}
constexpr uint64_t MiB = 1ull << 20;
}  // namespace

TEST(RenderPolicyCopyBudget, PlansNothingWhileTheTotalFits) {
  auto list = destinations({{900, 100 * MiB}, {10, 50 * MiB}});
  EXPECT_TRUE(pol::plan_copy_evictions(list, 150 * MiB, 512 * MiB, 300).empty());
  EXPECT_TRUE(pol::plan_copy_evictions(list, 512 * MiB, 512 * MiB, 300).empty()) << "exactly at the budget is fine";
}

TEST(RenderPolicyCopyBudget, ReleasesTheLongestIdleFirstDownToThreeQuartersOfTheBudget) {
  // budget 400 -> target 300; total 600
  auto list = destinations({{400, 100 * MiB}, {1000, 100 * MiB}, {700, 100 * MiB}, {500, 100 * MiB}, {20, 200 * MiB}});
  const auto plan = pol::plan_copy_evictions(list, 600 * MiB, 400 * MiB, 300);
  ASSERT_EQ(plan.size(), 3u);  // 600 -> 500 -> 400 -> 300
  EXPECT_EQ(plan[0], list[1].id);  // idle 1000
  EXPECT_EQ(plan[1], list[2].id);  // idle 700
  EXPECT_EQ(plan[2], list[3].id);  // idle 500
}

TEST(RenderPolicyCopyBudget, NeverTouchesDestinationsUsedRecently) {
  // Over budget, but everything is in use: leave it (a copy still being produced or sampled must not vanish).
  auto list = destinations({{5, 400 * MiB}, {100, 400 * MiB}, {300, 400 * MiB}});
  EXPECT_TRUE(pol::plan_copy_evictions(list, 1200 * MiB, 512 * MiB, 300).empty());
}

TEST(RenderPolicyCopyBudget, StopsEarlyWhenIdleDestinationsAreNotEnough) {
  auto list = destinations({{1000, 50 * MiB}, {10, 800 * MiB}});
  const auto plan = pol::plan_copy_evictions(list, 850 * MiB, 512 * MiB, 300);
  ASSERT_EQ(plan.size(), 1u);  // frees what it may; the busy one stays although the total is still over
  EXPECT_EQ(plan[0], list[0].id);
}

TEST(RenderPolicyCopyBudget, EqualIdleKeepsInputOrder) {
  auto list = destinations({{900, 100 * MiB}, {900, 100 * MiB}, {900, 100 * MiB}});
  const auto plan = pol::plan_copy_evictions(list, 300 * MiB, 200 * MiB, 300);  // target 150: two must go
  ASSERT_EQ(plan.size(), 2u);
  EXPECT_EQ(plan[0], list[0].id);
  EXPECT_EQ(plan[1], list[1].id);
}

// --- Pipeline choice (ubershader hybrid) -------------------------------------------------------
TEST(RenderPolicyPipelineChoice, ACompiledSpecializedPipelineAlwaysWins) {
  for (const auto mode : {pol::UberMode::Off, pol::UberMode::Hybrid})
    for (const bool uber : {false, true})
      EXPECT_EQ(pol::choose_pipeline(mode, {true, uber}), pol::PipelineChoice::Specialized);
}

TEST(RenderPolicyPipelineChoice, OffModeKeepsTodaysBehaviourWhileCompiling) {
  EXPECT_EQ(pol::choose_pipeline(pol::UberMode::Off, {false, true}), pol::PipelineChoice::WaitForSpecialized);
  EXPECT_EQ(pol::choose_pipeline(pol::UberMode::Off, {false, false}), pol::PipelineChoice::WaitForSpecialized);
}

TEST(RenderPolicyPipelineChoice, HybridBridgesAPendingCompileWithTheUbershader) {
  EXPECT_EQ(pol::choose_pipeline(pol::UberMode::Hybrid, {false, true}), pol::PipelineChoice::Uber);
}

TEST(RenderPolicyPipelineChoice, HybridWaitsWhenTheUbershaderIsNotReadyEither) {
  EXPECT_EQ(pol::choose_pipeline(pol::UberMode::Hybrid, {false, false}), pol::PipelineChoice::WaitForSpecialized);
}

// --- Compile slot (pipeline workers) -----------------------------------------------------------
TEST(RenderPolicyCompileSlot, UrgentWorkBeatsEverything) {
  using S = pol::CompileSlot;
  EXPECT_EQ(pol::pick_compile_slot(true, true, 2, 2, true, 0, 22), S::Urgent);
  EXPECT_EQ(pol::pick_compile_slot(true, false, 0, 2, false, 0, 22), S::Urgent);
}

TEST(RenderPolicyCompileSlot, BridgedWorkIsCappedAndThenYieldsToThePrewarm) {
  using S = pol::CompileSlot;
  EXPECT_EQ(pol::pick_compile_slot(false, true, 0, 2, true, 0, 22), S::Bridged);
  EXPECT_EQ(pol::pick_compile_slot(false, true, 1, 2, true, 0, 22), S::Bridged);
  EXPECT_EQ(pol::pick_compile_slot(false, true, 2, 2, true, 0, 22), S::Background);  // cap reached
  EXPECT_EQ(pol::pick_compile_slot(false, true, 2, 2, false, 0, 22), S::None);       // nothing else to do
}

TEST(RenderPolicyCompileSlot, InPlayCapCoversUrgentAndBridgedTogether) {
  using S = pol::CompileSlot;
  // Two compiles already running (one urgent, one bridged) with a cap of 2: nothing more in play.
  EXPECT_EQ(pol::pick_compile_slot(true, true, 1, 2, false, 0, 22, 1, 2), S::None);
  // ... but the boot prewarm keeps its own pool.
  EXPECT_EQ(pol::pick_compile_slot(true, true, 1, 2, true, 0, 22, 1, 2), S::Background);
  // Below the cap urgent work still goes first.
  EXPECT_EQ(pol::pick_compile_slot(true, true, 0, 2, true, 0, 22, 1, 2), S::Urgent);
  EXPECT_EQ(pol::pick_compile_slot(false, true, 0, 2, true, 0, 22, 1, 2), S::Bridged);
  // Urgent work alone fills the cap.
  EXPECT_EQ(pol::pick_compile_slot(true, false, 0, 2, false, 0, 22, 2, 2), S::None);
}

TEST(RenderPolicyCompileSlot, InPlayCapLeavesCoresOnSmallCpusAndIsOffWithoutSkipUnready) {
  EXPECT_EQ(pol::in_play_compile_cap(4, true), 2u);   // i7-4790: 4 cores / 8 threads
  EXPECT_EQ(pol::in_play_compile_cap(2, true), 2u);
  EXPECT_EQ(pol::in_play_compile_cap(6, true), 4u);
  EXPECT_EQ(pol::in_play_compile_cap(8, true), 6u);
  EXPECT_EQ(pol::in_play_compile_cap(4, false), static_cast<size_t>(-1));  // draws wait without limit
}

TEST(RenderPolicyCompileSlot, BackgroundRespectsItsOwnCapAndIdlesOtherwise) {
  using S = pol::CompileSlot;
  EXPECT_EQ(pol::pick_compile_slot(false, false, 0, 2, true, 22, 22), S::None);
  EXPECT_EQ(pol::pick_compile_slot(false, false, 0, 2, true, 21, 22), S::Background);
  EXPECT_EQ(pol::pick_compile_slot(false, false, 0, 2, false, 0, 22), S::None);
}

// --- Thread priority of a compile (shader stutter fixes of 2026-10-28/29) ----------------------
TEST(RenderPolicyCompilePriority, BridgedAndPrewarmCompilesNeverCompeteWithTheGame) {
  for (const bool skip : {false, true}) {
    EXPECT_EQ(pol::compile_priority(pol::CompileSlot::Bridged, skip), pol::CompilePriority::BelowNormal);
    EXPECT_EQ(pol::compile_priority(pol::CompileSlot::Background, skip), pol::CompilePriority::BelowNormal);
  }
}

TEST(RenderPolicyCompilePriority, UrgentCompilesStayAtNormalWhileWaitsAreBounded) {
  // Above normal, six workers on an 8-thread CPU took the game thread's cores during area loads.
  EXPECT_EQ(pol::compile_priority(pol::CompileSlot::Urgent, true), pol::CompilePriority::Normal);
  EXPECT_EQ(pol::compile_priority(pol::CompileSlot::Urgent, false), pol::CompilePriority::AboveNormal);
}

// --- Pipeline waits -----------------------------------------------------------------------------
TEST(RenderPolicyPipelineWait, SkipUnreadyModeNeverWaitsWithoutLimit) {
  // The 140-360 ms freezes per new material came from an unbounded wait in copy/offscreen passes.
  for (const bool requireReady : {false, true})
    EXPECT_NE(pol::pipeline_wait(true, requireReady), pol::PipelineWait::Unbounded);
  EXPECT_EQ(pol::pipeline_wait(true, false), pol::PipelineWait::FrameBudget);
  EXPECT_EQ(pol::pipeline_wait(true, true), pol::PipelineWait::PersistentPassBudget);
  EXPECT_EQ(pol::pipeline_wait(false, true), pol::PipelineWait::Unbounded);
}

TEST(RenderPolicyPipelineWait, BothBudgetsTogetherFitWellInsideAFrame) {
  // One encoded frame can spend both budgets; together they must leave most of a 16.7 ms frame.
  const uint32_t frame = pol::pipeline_wait_budget_ms(pol::PipelineWait::FrameBudget);
  const uint32_t persistent = pol::pipeline_wait_budget_ms(pol::PipelineWait::PersistentPassBudget);
  EXPECT_EQ(frame, 4u);
  EXPECT_EQ(persistent, 6u);
  EXPECT_LE(frame + persistent, 10u);
}

// --- First pipeline a draw binds ----------------------------------------------------------------
TEST(RenderPolicyFirstBind, AReadyVertexLitFallbackBridgesACompilingVariant) {
  pol::DrawPipelines d;
  d.hasFallback = true;
  d.fallbackReady = true;
  EXPECT_EQ(pol::first_bind(d), pol::FirstBind::Fallback);
}

TEST(RenderPolicyFirstBind, TheVariantWinsOnceCompiledOrAlreadyBound) {
  pol::DrawPipelines d;
  d.hasFallback = true;
  d.fallbackReady = true;
  d.primaryReady = true;
  EXPECT_EQ(pol::first_bind(d), pol::FirstBind::Primary);
  d.primaryReady = false;
  d.primaryIsCurrent = true;
  EXPECT_EQ(pol::first_bind(d), pol::FirstBind::Primary);
}

TEST(RenderPolicyFirstBind, WithoutAReadyFallbackThePrimaryWaitsItsBudget) {
  pol::DrawPipelines d;
  EXPECT_EQ(pol::first_bind(d), pol::FirstBind::Primary);
  d.hasFallback = true;
  EXPECT_EQ(pol::first_bind(d), pol::FirstBind::Primary);
}

TEST(RenderPolicyFirstBind, AChosenUbershaderComesFirst) {
  pol::DrawPipelines d;
  d.uberChosen = true;
  d.hasFallback = true;
  d.fallbackReady = true;
  EXPECT_EQ(pol::first_bind(d), pol::FirstBind::Uber);
  // The hybrid choice itself: a pending specialised compile is bridged, never waited on.
  EXPECT_EQ(pol::choose_pipeline(pol::UberMode::Hybrid, {false, true}), pol::PipelineChoice::Uber);
}

// --- Spare copy targets of other sizes ----------------------------------------------------------
TEST(RenderPolicyCopyPoolSpares, KeepsOnlyTheNewestSparesOfOtherSizesPerDestination) {
  int a = 0, b = 0;
  // Oldest first: five retired sizes of destination a, one of b, the acquired key of a.
  const std::vector<pol::PoolSpare> pool = {
      {&a, false, true}, {&a, false, true}, {&b, false, true}, {&a, false, true},
      {&a, true, true},  {&a, false, true}, {&a, false, true},
  };
  const auto drops = pol::plan_other_shape_spare_drops(pool, &a, 2);
  // Newest other-size spares of a are 6 and 5; 3, 1, 0 go, highest index first.
  EXPECT_EQ(drops, (std::vector<size_t>{3, 1, 0}));
}

TEST(RenderPolicyCopyPoolSpares, NeverDropsTargetsInUseTheCurrentKeyOrOtherDestinations) {
  int a = 0, b = 0;
  const std::vector<pol::PoolSpare> pool = {
      {&a, false, false}, {&a, false, false}, {&a, false, false},  // still referenced elsewhere
      {&a, true, true},                                            // the size being acquired
      {&b, false, true}, {&b, false, true}, {&b, false, true},     // another destination
  };
  EXPECT_TRUE(pol::plan_other_shape_spare_drops(pool, &a, 2).empty());
  EXPECT_TRUE(pol::plan_other_shape_spare_drops({}, &a, 2).empty());
}

// --- Physical core kept for the game thread (aurora/host_cores.h) -------------------------------
TEST(HostCoresGameCore, FourCoresWithSmtKeepTheLastCore) {
  namespace hc = aurora::host_cores;
  // i7-4790: 4 cores, 8 logical processors.
  const std::vector<hc::HostCore> cores = {{0x03, true}, {0x0C, true}, {0x30, true}, {0xC0, true}};
  EXPECT_EQ(hc::pick_game_core(cores, 1), 0xC0u);
}

TEST(HostCoresGameCore, OffWhenThereIsNoRoomOrNoSmt) {
  namespace hc = aurora::host_cores;
  EXPECT_EQ(hc::pick_game_core({{0x03, true}, {0x0C, true}}, 1), 0u);                      // 2 cores
  EXPECT_EQ(hc::pick_game_core({{0x1, false}, {0x2, false}, {0x4, false}, {0x8, false}}, 1), 0u);  // no SMT
  std::vector<hc::HostCore> many;
  for (unsigned i = 0; i < 12; ++i) many.push_back({uint64_t{3} << (2 * i), true});
  EXPECT_EQ(hc::pick_game_core(many, 1), 0u);                                              // > 8 cores
  EXPECT_EQ(hc::pick_game_core({{0x03, true}, {0x0C, true}, {0x30, true}}, 2), 0u);        // 2 groups
}

TEST(HostCoresGameCore, HybridCpusPickTheLastCoreThatHasSmt) {
  namespace hc = aurora::host_cores;
  // Four SMT performance cores, then four single-thread efficiency cores.
  const std::vector<hc::HostCore> cores = {{0x003, true},  {0x00C, true},  {0x030, true},  {0x0C0, true},
                                           {0x100, false}, {0x200, false}, {0x400, false}, {0x800, false}};
  EXPECT_EQ(hc::pick_game_core(cores, 1), 0x0C0u);
}

// --- Hand-off of the GX stream to the decoder thread (fifo::commit) ---------------------------
TEST(RenderPolicyHandOff, SmallBuffersStayWithTheWriter) {
  // Below the eager size nothing goes, idle decoder or not: no wake-up per draw.
  EXPECT_FALSE(pol::hand_off_now(0, true));
  EXPECT_FALSE(pol::hand_off_now(2 * 1024, true));
  EXPECT_FALSE(pol::hand_off_now(pol::kEagerHandOffBytes - 1, true));
}

TEST(RenderPolicyHandOff, IdleDecoderTakesEagerChunksBusyOneSoftChunks) {
  EXPECT_TRUE(pol::hand_off_now(pol::kEagerHandOffBytes, true));
  EXPECT_FALSE(pol::hand_off_now(pol::kEagerHandOffBytes, false));
  EXPECT_FALSE(pol::hand_off_now(pol::kSoftHandOffBytes - 1, false));
  EXPECT_TRUE(pol::hand_off_now(pol::kSoftHandOffBytes, false));
  EXPECT_TRUE(pol::hand_off_now(pol::kSoftHandOffBytes, true));
  static_assert(pol::kEagerHandOffBytes < pol::kSoftHandOffBytes);
  static_assert(pol::kEagerHandOffBytes >= 8 * 1024, "per-draw wake-ups of the decoder (2 KiB) saturated the GX thread");
}

// --- Frame interpolation cadence ---
namespace {
std::vector<double> display_ticks(double imagesPerFrame, int frames, int* nativeShown = nullptr) {
  std::vector<double> ticks;
  double next = 0.0;
  for (int frame = 0; frame < frames; ++frame) {
    const auto cadence = pol::next_frame_cadence(imagesPerFrame, next);
    for (uint32_t i = 0; i < cadence.samples; ++i) ticks.push_back(frame + cadence.weights[i]);
    if (cadence.presentNative) {
      ticks.push_back(frame + 1.0);
      if (nativeShown != nullptr) ++*nativeShown;
    }
  }
  return ticks;
}
}  // namespace

TEST(RenderPolicyCadence, WholeMultiplesShowEveryFrameWithEvenWeights) {
  double next = 0.0;
  auto c120 = pol::next_frame_cadence(2.0, next);
  EXPECT_EQ(c120.samples, 1u);
  EXPECT_TRUE(c120.presentNative);
  EXPECT_FLOAT_EQ(c120.weights[0], 0.5f);
  next = 0.0;
  auto c240 = pol::next_frame_cadence(4.0, next);
  EXPECT_EQ(c240.samples, 3u);
  EXPECT_TRUE(c240.presentNative);
  EXPECT_FLOAT_EQ(c240.weights[0], 0.25f);
  EXPECT_FLOAT_EQ(c240.weights[2], 0.75f);
  next = 0.0;
  for (int frame = 0; frame < 5000; ++frame) {
    const auto c180 = pol::next_frame_cadence(3.0, next);
    EXPECT_EQ(c180.samples, 2u) << "phase must not drift, frame " << frame;
    EXPECT_TRUE(c180.presentNative) << frame;
  }
}

TEST(RenderPolicyCadence, OneImagePerFrameOrLessInsertsNothing) {
  double next = 0.0;
  for (const double rate : {0.0, 0.5, 1.0}) {
    const auto cadence = pol::next_frame_cadence(rate, next);
    EXPECT_EQ(cadence.samples, 0u);
    EXPECT_TRUE(cadence.presentNative);
  }
  EXPECT_DOUBLE_EQ(pol::cadence_bias(1.0), 1.0);
}

TEST(RenderPolicyCadence, MonitorRatesBetweenMultiplesKeepTicksEvenlySpaced) {
  for (const double hz : {75.0, 90.0, 100.0, 144.0, 165.0, 170.0, 200.0, 280.0, 360.0}) {
    const double rate = hz / 60.0;
    const auto ticks = display_ticks(rate, 2000);
    ASSERT_GT(ticks.size(), 1000u);
    EXPECT_NEAR(static_cast<double>(ticks.size()) / 2000.0, rate, 0.01) << hz << " Hz keeps its average rate";
    for (size_t i = 1; i < ticks.size(); ++i) {
      // The snap moves a tick onto the frame by at most 5 % of the display period.
      EXPECT_NEAR(ticks[i] - ticks[i - 1], 1.0 / rate, 0.06 / rate) << hz << " Hz, tick " << i;
    }
  }
}

TEST(RenderPolicyCadence, FrameWithoutATickIsNotShown) {
  int nativeShown = 0;
  display_ticks(144.0 / 60.0, 1000, &nativeShown);
  EXPECT_NEAR(nativeShown, 200, 2) << "only every fifth frame lands on a 144 Hz tick";
  double next = 0.0;
  for (int frame = 0; frame < 2000; ++frame) {
    const auto cadence = pol::next_frame_cadence(144.0 / 60.0, next);
    EXPECT_GT(cadence.samples + (cadence.presentNative ? 1u : 0u), 0u) << "every frame shows something";
    EXPECT_LE(cadence.samples, pol::kMaxCadenceSamples);
    for (uint32_t i = 0; i < cadence.samples; ++i) {
      EXPECT_GT(cadence.weights[i], 0.f);
      EXPECT_LT(cadence.weights[i], 1.f);
    }
  }
}

TEST(RenderPolicyCadence, TheFastestRateFitsTheSlots) {
  double next = 0.0;
  for (int frame = 0; frame < 2000; ++frame) {
    const auto cadence = pol::next_frame_cadence(8.0, next);
    EXPECT_EQ(cadence.samples, pol::kMaxCadenceSamples) << frame;
    EXPECT_TRUE(cadence.presentNative) << frame;
  }
}

TEST(RenderPolicyCadence, BiasIsTheFirstTickOfTheEarliestGroup) {
  EXPECT_NEAR(pol::cadence_bias(2.0), 0.5, 1e-5);
  EXPECT_NEAR(pol::cadence_bias(4.0), 0.25, 1e-5);
  EXPECT_NEAR(pol::cadence_bias(2.4), 1.0 / 12.0, 1e-5);
}

TEST(RenderPolicyCadence, ControllerBacksOffWhenOverloaded) {
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(4.0, 8.0, 0.85, 0.1, 0.0), 3.0) << "encoding nearly filled the frame";
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(4.0, 8.0, 0.2, 1.3, 0.0), 3.0) << "presenting took longer than a frame";
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(4.0, 8.0, 0.2, 0.1, 0.15), 3.0) << "the game missed its retraces";
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(1.0, 8.0, 0.99, 2.0, 1.0), 1.0) << "never below the frame itself";
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(2.4, 2.4, 0.99, 0.0, 0.0), 1.4);
}

TEST(RenderPolicyCadence, ControllerClimbsOnlyWhenTheNextRateFits) {
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(2.0, 8.0, 0.2, 0.1, 0.0), 3.0);
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(4.0, 8.0, 0.55, 0.1, 0.0), 4.0) << "0.55 * 5/4 does not leave room";
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(2.0, 8.0, 0.2, 0.1, 0.05), 2.0) << "frames are still missing";
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(8.0, 8.0, 0.1, 0.1, 0.0), 8.0) << "the ceiling holds";
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(1.4, 2.4, 0.1, 0.1, 0.0), 2.4) << "a reduced rate returns to the target";
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(1.0, 8.0, 0.1, 0.75, 0.0), 2.0)
      << "Present blocking with a single image says nothing about the next one";
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(1.0, 2.0, 0.30, 0.04, 0.0), 2.0)
      << "pause-screen map: the first inserted image fits at 0.30 (two measured 0.43-0.59)";
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(1.0, 2.0, 0.34, 0.04, 0.0), 1.0) << "0.34 * 2 leaves no room";
  EXPECT_DOUBLE_EQ(pol::next_images_per_frame(2.0, 8.0, 0.36, 0.04, 0.0), 2.0)
      << "past the first image the linear prediction and the 0.50 room still apply";
}

TEST(RenderPolicyImageRate, LateFramesThatStayWithoutImagesDoNotHoldTheRateDown) {
  // A heavy room: the game is late about 1 frame in 6 whether or not images are inserted.
  pol::RatePacer pacer;
  double rate = 2.0;
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.35, 0.07, 10.0 / 60.0), pol::RateChange::Probe);
  EXPECT_DOUBLE_EQ(rate, 1.0);
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.13, 0.04, 9.0 / 60.0), pol::RateChange::ProbeRestored);
  EXPECT_DOUBLE_EQ(rate, 2.0) << "back at once: the images were not the cause";
  for (uint32_t window = 0; window < pol::kRateImmuneWindows; ++window) {
    pol::step_images_per_frame(pacer, rate, 2.0, 0.35, 0.07, 12.0 / 60.0);
    EXPECT_DOUBLE_EQ(rate, 2.0) << "late frames are not held against the images for a while";
  }
}

TEST(RenderPolicyImageRate, LateFramesCausedByTheImagesKeepTheLowerRate) {
  pol::RatePacer pacer;
  double rate = 2.0;
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.35, 0.07, 10.0 / 60.0), pol::RateChange::Probe);
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.13, 0.04, 0.0), pol::RateChange::ProbeConfirmed);
  EXPECT_DOUBLE_EQ(rate, 1.0);
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.13, 0.04, 0.0), pol::RateChange::None) << "held one window";
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.13, 0.04, 0.0), pol::RateChange::Grew);
  EXPECT_DOUBLE_EQ(rate, 2.0);
}

TEST(RenderPolicyImageRate, EncodeOverloadReducesWithoutAProbe) {
  pol::RatePacer pacer;
  double rate = 2.0;
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.85, 0.07, 0.0), pol::RateChange::Reduced);
  EXPECT_DOUBLE_EQ(rate, 1.0);
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.20, 0.04, 0.0), pol::RateChange::None) << "the wait applies";
  uint32_t waited = 1;
  while (pol::step_images_per_frame(pacer, rate, 2.0, 0.20, 0.04, 0.0) != pol::RateChange::Grew) {
    ASSERT_LT(++waited, 4 * pol::kRateMaximumBackoffWindows);
  }
}

TEST(RenderPolicyImageRate, AProbeDoesNotRestoreARateThatWouldNotFit) {
  // Pause-screen map, 2026-11-08 F8: one image cost 0.51 and the game stayed late; restoring two images
  // (about 1.0 of a frame) kept 43-55 real frames per second for a minute.
  pol::RatePacer pacer;
  double rate = 2.0;
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.63, 0.08, 22.0 / 60.0), pol::RateChange::Probe);
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.51, 0.06, 17.0 / 60.0), pol::RateChange::ProbeConfirmed);
  EXPECT_DOUBLE_EQ(rate, 1.0);
}

TEST(RenderPolicyImageRate, AGrowthThatFailsAtOnceIsNotRetriedInTheSameScene) {
  // The map grew at 0.28 (predicted 0.56), measured 0.69 with 34/60 late, and retried every 20-80 s.
  pol::RatePacer pacer;
  double rate = 1.0;
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.28, 0.03, 0.0), pol::RateChange::Grew);
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.69, 0.07, 34.0 / 60.0), pol::RateChange::Probe);
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.29, 0.03, 1.0 / 60.0), pol::RateChange::ProbeConfirmed);
  uint32_t windows = 0;
  while (pol::step_images_per_frame(pacer, rate, 2.0, 0.30, 0.03, 0.0) != pol::RateChange::Grew) {
    ASSERT_LT(++windows, 4 * pol::kRateFailedGrowthWindows);
  }
  EXPECT_GE(windows, pol::kRateFailedGrowthWindows) << "about a minute before one more try";
}

TEST(RenderPolicyImageRate, AnOverloadRightAfterGrowingAlsoCountsAsAFailedGrowth) {
  pol::RatePacer pacer;
  double rate = 1.0;
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.31, 0.03, 0.0), pol::RateChange::Grew);
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.72, 0.07, 0.0), pol::RateChange::Reduced);
  for (uint32_t window = 0; window < pol::kRateMaximumBackoffWindows; ++window) {
    EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.30, 0.03, 0.0), pol::RateChange::None);
  }
}

TEST(RenderPolicyImageRate, AClearlyLighterSceneForgetsTheWaitAndTheFailedGrowth) {
  // After the map, a corridor at 0.06-0.15 waited ~10 s at 60 on the map's doubled wait.
  pol::RatePacer pacer;
  double rate = 1.0;
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.28, 0.03, 0.0), pol::RateChange::Grew);
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.69, 0.07, 34.0 / 60.0), pol::RateChange::Probe);
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.29, 0.03, 1.0 / 60.0), pol::RateChange::ProbeConfirmed);
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.08, 0.03, 0.0), pol::RateChange::Grew)
      << "left the map: grows at once";
  // A short spike in the corridor, even right after growing, holds one window, not the map's wait.
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.49, 0.09, 7.0 / 60.0), pol::RateChange::Probe);
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.15, 0.04, 0.0), pol::RateChange::ProbeConfirmed);
  uint32_t windows = 0;
  while (pol::step_images_per_frame(pacer, rate, 2.0, 0.16, 0.04, 0.0) != pol::RateChange::Grew) {
    ASSERT_LT(++windows, 4u);
  }
}

TEST(RenderPolicyImageRate, TheWaitAfterAReductionIsMeasuredAtTheLowerRate) {
  // 0.85 with two images and 0.20 with one is the same scene, not a lighter one.
  pol::RatePacer pacer;
  double rate = 2.0;
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.85, 0.07, 0.0), pol::RateChange::Reduced);
  EXPECT_EQ(pol::step_images_per_frame(pacer, rate, 2.0, 0.20, 0.04, 0.0), pol::RateChange::None);
  EXPECT_EQ(pacer.holdLoad, 0.20);
}

// --- Display refresh alignment ---------------------------------------------------------------
namespace {
// A monitor with a fixed refresh: an image presented at x is finished `gpu` later and shown from the
// first refresh after that. Counts refreshes that show no new image (repeats) and images never shown.
struct ShownCadence {
  int repeats = 0;
  int dropped = 0;
  int shown = 0;
};
ShownCadence show_on_fixed_refresh(const std::vector<int64_t>& presents, int64_t firstRefresh, int64_t period,
                                   int64_t gpu, size_t skipFirst) {
  ShownCadence result;
  size_t newest = 0;
  bool any = false;
  size_t lastShown = 0;
  bool haveShown = false;
  for (int64_t refresh = firstRefresh;; refresh += period) {
    while (newest < presents.size() && presents[newest] + gpu < refresh) {
      ++newest;
      any = true;
    }
    if (newest >= presents.size()) {
      break;
    }
    if (!any) {
      continue;
    }
    const size_t onScreen = newest - 1;
    if (haveShown && onScreen >= skipFirst) {
      if (onScreen == lastShown) {
        ++result.repeats;
      } else {
        result.dropped += static_cast<int>(onScreen - lastShown - 1);
        ++result.shown;
      }
    }
    lastShown = onScreen;
    haveShown = true;
  }
  return result;
}

uint64_t next_random(uint64_t& state) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}
double uniform(uint64_t& state) { return static_cast<double>(next_random(state) >> 11) / 9007199254740992.0; }

constexpr int64_t kGameImage = 8'333'000;      // 120 images/s on the VI's 16.666 ms frames
constexpr int64_t kMonitorPeriod = 8'334'400;  // a "120 Hz" monitor at 119.98 Hz
}  // namespace

TEST(RenderPolicyRefreshAlignment, EstimatorLocksOntoAMonitorDespiteWakeupJitter) {
  pol::RefreshEstimator estimator;
  uint64_t random = 0x9E3779B97F4A7C15ull;
  const int64_t start = 1'000'000'000;
  bool usable = false;
  int64_t refresh = start;
  int64_t lastObserved = 0;
  for (int i = 0; i < 600; ++i, refresh += kMonitorPeriod) {
    // The observer thread wakes 20-320 us after each blank, now and then a whole millisecond late.
    int64_t wake = 20'000 + static_cast<int64_t>(uniform(random) * 300'000);
    if (i % 97 == 50) wake += 1'000'000;
    if (i % 211 == 100) continue;  // a blank the thread slept through
    pol::observe_refresh(estimator, refresh + wake, 8'333'333);
    usable = pol::refresh_usable(estimator, refresh + wake);
    lastObserved = refresh;
  }
  EXPECT_TRUE(usable);
  EXPECT_NEAR(estimator.period, static_cast<double>(kMonitorPeriod), 2'000.0);
  EXPECT_NEAR(static_cast<double>(estimator.vblank - lastObserved), 170'000.0, 250'000.0)
      << "phase of the latest blank";
}

TEST(RenderPolicyRefreshAlignment, IrregularRefreshNeverLocksAndRepeatedLossesStopTrying) {
  pol::RefreshEstimator estimator;
  int64_t at = 1'000'000'000;
  for (int i = 0; i < 600; ++i) {
    at += (i % 3 == 0) ? 12'500'000 : 8'333'333;  // variable refresh following uneven presents
    pol::observe_refresh(estimator, at, 8'333'333);
    EXPECT_FALSE(pol::refresh_usable(estimator, at));
  }
  // A lock that is lost while in use holds alignment off for longer each time, then for good.
  pol::RefreshEstimator vrr;
  at = 1'000'000'000;
  int locks = 0;
  for (int round = 0; round < 6; ++round) {
    bool wasUsable = false;
    for (int i = 0; i < 6000; ++i) {  // 50 s
      at += 8'333'333;
      pol::observe_refresh(vrr, at, 8'333'333);
      wasUsable = pol::refresh_usable(vrr, at) || wasUsable;
    }
    locks += wasUsable ? 1 : 0;
    for (int i = 0; i < 40; ++i) {
      at += (i % 2 == 0) ? 12'500'000 : 9'000'000;
      pol::observe_refresh(vrr, at, 8'333'333);
      pol::refresh_usable(vrr, at);
    }
  }
  EXPECT_EQ(vrr.trips, pol::kRefreshMaximumTrips + 1);
  EXPECT_EQ(locks, static_cast<int>(pol::kRefreshMaximumTrips) + 1) << "the fourth loss ends alignment";
}

TEST(RenderPolicyRefreshAlignment, JitteredImagesEachGetTheirOwnRefresh) {
  // Low latency mode: images go out when encoding ends, +-2 ms around the 120 Hz cadence.
  constexpr int kImages = 12'000;  // 100 s
  const int64_t firstRefresh = 1'000'000'000 + 3'100'000;
  const pol::RefreshGrid grid{firstRefresh + 150'000, kMonitorPeriod};  // the estimate trails the blank a little
  for (const int64_t gpu : {int64_t{300'000}, int64_t{3'000'000}}) {
    uint64_t random = 12345;
    std::vector<int64_t> unaligned, aligned;
    pol::RefreshAlignment alignment;
    for (int i = 0; i < kImages; ++i) {
      // Aligned on the steady VI-based time; the image itself is ready with the jitter.
      const int64_t steady = 1'000'000'000 + int64_t{i} * kGameImage;
      const int64_t ready = steady + static_cast<int64_t>((uniform(random) * 2.0 - 1.0) * 2'000'000);
      unaligned.push_back(ready);
      aligned.push_back((std::max)(ready, pol::align_to_refresh(alignment, grid, steady)));
    }
    std::sort(unaligned.begin(), unaligned.end());
    const auto before = show_on_fixed_refresh(unaligned, firstRefresh, kMonitorPeriod, gpu, 10);
    const auto after = show_on_fixed_refresh(aligned, firstRefresh, kMonitorPeriod, gpu, 10);
    std::printf("gpu %.1f ms: unaligned repeats %d dropped %d, aligned repeats %d dropped %d\n",
                static_cast<double>(gpu) / 1e6, before.repeats, before.dropped, after.repeats, after.dropped);
    // 120.005 images/s against 119.98 refreshes/s: about 2.5 images in 100 s cannot have a refresh.
    EXPECT_LE(after.dropped, 4);
    EXPECT_LE(after.repeats, 4);
    EXPECT_GE(before.repeats + before.dropped, 20 * (after.repeats + after.dropped + 1));
    EXPECT_LE(std::llabs(aligned.back() - unaligned.back()), 10'000'000LL) << "latency stays bounded";
  }
}

TEST(RenderPolicyRefreshAlignment, ScheduledImagesKeepAFixedOffsetAndModestLatency) {
  const pol::RefreshGrid grid{2'000'000'000, 8'333'333};
  pol::RefreshAlignment alignment;
  double added = 0.0;
  int64_t previous = 0;
  for (int i = 0; i < 1200; ++i) {
    const int64_t wanted = 2'000'000'000 + 5'000'000 + int64_t{i} * 8'333'333;
    const int64_t at = pol::align_to_refresh(alignment, grid, wanted);
    const int64_t phase = ((at - grid.vblank) % grid.period + grid.period) % grid.period;
    EXPECT_NEAR(static_cast<double>(phase), pol::kAlignPhase * 8'333'333.0, 2.0);
    if (i != 0) EXPECT_EQ(at - previous, 8'333'333) << "one refresh apart at image " << i;
    added += static_cast<double>(at - wanted);
    previous = at;
  }
  EXPECT_LE(added / 1200.0, pol::kAlignLatest * 8'333'333.0);
  EXPECT_GE(added / 1200.0, pol::kAlignEarliest * 8'333'333.0);
}

TEST(RenderPolicyRefreshAlignment, SixtyImagesOnA120HzMonitorEachHoldTwoRefreshes) {
  const pol::RefreshGrid grid{2'000'000'000, kMonitorPeriod};
  pol::RefreshAlignment alignment;
  std::vector<int64_t> presents;
  uint64_t random = 99;
  for (int i = 0; i < 3000; ++i) {
    const int64_t steady = 2'000'000'000 + int64_t{i} * 16'666'000;
    const int64_t ready = steady + static_cast<int64_t>((uniform(random) * 2.0 - 1.0) * 2'000'000);
    presents.push_back((std::max)(ready, pol::align_to_refresh(alignment, grid, steady)));
  }
  const auto shown = show_on_fixed_refresh(presents, grid.vblank, kMonitorPeriod, 500'000, 10);
  EXPECT_LE(shown.dropped, 2);
  EXPECT_NEAR(static_cast<double>(shown.repeats), static_cast<double>(shown.shown), 4.0)
      << "every image shown for exactly two refreshes";
}

TEST(RenderPolicyRefreshAlignment, NoGridOrALongGapLeavesTheScheduleAlone) {
  pol::RefreshAlignment alignment;
  EXPECT_EQ(pol::align_to_refresh(alignment, {}, 123'456'789), 123'456'789);
  EXPECT_FALSE(alignment.valid);
  const pol::RefreshGrid grid{1'000'000'000, 8'333'333};
  const int64_t first = pol::align_to_refresh(alignment, grid, 1'010'000'000);
  EXPECT_TRUE(alignment.valid);
  EXPECT_GE(first - 1'010'000'000, static_cast<int64_t>(pol::kAlignEarliest * 8'333'333.0));
  // A one-second pause: the next image is aligned afresh, not one refresh after the old one.
  const int64_t resumed = pol::align_to_refresh(alignment, grid, 2'010'000'000);
  EXPECT_LE(std::llabs(resumed - 2'010'000'000), 8'333'333LL);
}

TEST(RenderPolicyRefreshAlignment, ALateImageDoesNotShiftTheImagesAfterIt) {
  const pol::RefreshGrid grid{1'000'000'000, kMonitorPeriod};
  pol::RefreshAlignment alignment;
  std::vector<int64_t> presents;
  for (int i = 0; i < 2400; ++i) {
    const int64_t steady = 1'000'000'000 + 2'000'000 + int64_t{i} * kMonitorPeriod;  // rates match exactly
    int64_t ready = steady;
    if (i % 400 == 200) ready += 7'000'000;  // one image very late now and then
    presents.push_back((std::max)(ready, pol::align_to_refresh(alignment, grid, steady)));
  }
  const auto shown = show_on_fixed_refresh(presents, grid.vblank, kMonitorPeriod, 300'000, 10);
  // Each late image costs at most its own refresh (shown twice before it, then it is replaced at once).
  EXPECT_LE(shown.repeats, 6);
  EXPECT_LE(shown.dropped, 6);
}

TEST(RenderPolicyRefreshAlignment, ManyLateImagesSuspendAlignmentUntilTwoCalmWindows) {
  pol::AlignHealth health;
  for (uint32_t i = 0; i < pol::kAlignHealthWindow; ++i) {
    EXPECT_TRUE(pol::note_alignment_lateness(health, i % 20 == 0)) << "5 % late keeps it";
  }
  for (uint32_t i = 0; i < pol::kAlignHealthWindow - 1; ++i) {
    pol::note_alignment_lateness(health, i % 5 == 0);
  }
  EXPECT_FALSE(pol::note_alignment_lateness(health, false)) << "20 % late suspends it";
  for (uint32_t i = 0; i < pol::kAlignHealthWindow; ++i) {
    pol::note_alignment_lateness(health, i % 30 == 0);  // still 3 %: not calm enough
  }
  EXPECT_TRUE(health.suspended);
  for (uint32_t i = 0; i < pol::kAlignHealthWindow; ++i) pol::note_alignment_lateness(health, false);
  EXPECT_TRUE(health.suspended) << "one calm window is not enough";
  for (uint32_t i = 0; i < pol::kAlignHealthWindow - 1; ++i) pol::note_alignment_lateness(health, false);
  EXPECT_TRUE(pol::note_alignment_lateness(health, false)) << "back after two calm windows";
}

TEST(RenderPolicyRefreshAlignment, AlignmentWaitsForHeadroomAndBacksOffWhenItIsLost) {
  pol::AlignGate gate;
  EXPECT_TRUE(pol::pacing_is_comfortable(2.0, 2.0, 2, 60, 0.4));
  EXPECT_FALSE(pol::pacing_is_comfortable(1.0, 2.0, 0, 60, 0.2)) << "rate held below its maximum";
  EXPECT_FALSE(pol::pacing_is_comfortable(2.0, 2.0, 3, 60, 0.2)) << "game frames late";
  EXPECT_FALSE(pol::pacing_is_comfortable(2.0, 2.0, 0, 60, 0.65)) << "encoding near its limit";
  EXPECT_FALSE(pol::step_align_gate(gate, true));
  EXPECT_TRUE(pol::step_align_gate(gate, true)) << "two calm windows open it";
  EXPECT_FALSE(pol::step_align_gate(gate, false));
  int waited = 0;
  while (!pol::step_align_gate(gate, true)) ++waited;
  EXPECT_EQ(waited, 3) << "four calm windows after one loss";
  pol::step_align_gate(gate, false);
  waited = 0;
  while (!pol::step_align_gate(gate, true)) ++waited;
  EXPECT_EQ(waited, 7);
  for (int i = 0; i < 70; ++i) pol::step_align_gate(gate, true);
  pol::step_align_gate(gate, false);
  waited = 0;
  while (!pol::step_align_gate(gate, true)) ++waited;
  EXPECT_EQ(waited, 3) << "after a long calm stretch a loss starts the backoff over";
}
