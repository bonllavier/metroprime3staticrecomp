#pragma once
// Small pure decisions of the renderer, kept free of GPU and global state so they are unit tested
// (tests/render_policy_test.cpp). Each one was a bug or an option of the RM3E01 port; the call
// sites (gx.cpp, shader_info.cpp, efb_ram_copy.cpp) only gather their inputs and apply the answer.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aurora::gx::policy {

// --- Frame interpolation cadence --------------------------------------------------------------
// The game draws one logical frame per retrace; the display can take any rate. Every display tick
// falls somewhere inside a logical frame and shows that frame at the matching blend weight, so 144
// images/s over 60 Hz gives 0.42, 0.83 | 0.25, 0.67 | 0.08, 0.5, 0.92 | ... and the motion stays
// even. A weight of 1 is the frame itself; a frame with no tick there is still rendered, just not shown.
inline constexpr uint32_t kMaxCadenceSamples = 7;

struct FrameCadence {
  uint32_t samples = 0;  // inserted images, ordered by weight
  bool presentNative = true;
  std::array<float, kMaxCadenceSamples> weights{};
};

// `nextTick` carries the phase between frames, in logical frames from the start of the next one.
inline FrameCadence next_frame_cadence(double imagesPerFrame, double& nextTick) noexcept {
  FrameCadence cadence;
  if (!(imagesPerFrame > 1.0)) {
    nextTick = 1.0;
    return cadence;
  }
  const double step = 1.0 / imagesPerFrame;
  // A tick this close to the frame itself is the frame: no extra render for an invisible difference.
  const double snap = step * 0.05;
  if (!(nextTick > 0.0) || nextTick > step) {
    nextTick = step;
  }
  double tick = nextTick;
  for (; tick < 1.0 - snap; tick += step) {
    if (cadence.samples < kMaxCadenceSamples) {
      cadence.weights[cadence.samples++] = static_cast<float>(tick);
    }
  }
  cadence.presentNative = tick <= 1.0 + snap;
  if (cadence.presentNative) {
    tick += step;
  }
  nextTick = tick - 1.0;
  return cadence;
}

// The smallest weight the cadence ever produces. Showing weight w at (w - bias) of a frame period
// after the group's start keeps every tick evenly spaced without delaying the group needlessly.
inline double cadence_bias(double imagesPerFrame) noexcept {
  if (!(imagesPerFrame > 1.0)) {
    return 1.0;
  }
  double nextTick = 0.0;
  double smallest = 1.0;
  for (int frame = 0; frame < 600; ++frame) {
    const FrameCadence cadence = next_frame_cadence(imagesPerFrame, nextTick);
    if (cadence.samples != 0) {
      smallest = std::min(smallest, static_cast<double>(cadence.weights[0]));
    }
  }
  return smallest;
}

// Images per logical frame the controller should aim for after one pacing window. `encodeLoad` and
// `presentLoad` are the shares of a frame period that encoding and presenting took in that window;
// `misses` the share of frames (0..1) that overran their retrace. Rises only when one more image is
// predicted to fit, so a scene that can sustain N images settles there instead of flapping. Only
// encoding predicts that: Present blocks about 12 ms with one image per frame and 2 ms with two on the
// same machine, so its cost only says when to back off. Inserted images must never
// cost the game a real frame, so the limits leave room: with 8 images per frame and an average load
// of 0.66 the hangar still dropped from 60 real frames per second to 49, because single frames
// overran while the average fit.
inline double next_images_per_frame(double current, double maximum, double encodeLoad, double presentLoad,
                                     double misses) noexcept {
  constexpr double kReduceMisses = 0.10;
  constexpr double kRestoreMisses = 0.02;
  constexpr double kOverload = 0.70;
  constexpr double kRoomToGrow = 0.50;
  // The first inserted image costs well under a second native one: matching, uploads and the game's
  // own passes are not repeated. On the pause-screen map (2026-11-07 F8) one image measured 0.29-0.30
  // and two 0.43-0.59, yet 0.30 * 2 failed the 0.50 test and the rate sat at 60 FPS with the frame
  // worker 63 % idle. Still below kOverload, so a scene that grows and lands too high backs off.
  constexpr double kRoomToGrowFromNative = 0.65;
  const double ceiling = std::max(maximum, 1.0);
  const double clamped = std::clamp(current, 1.0, ceiling);
  if (misses >= kReduceMisses || encodeLoad >= kOverload || (clamped > 1.0 && presentLoad >= 1.0)) {
    return std::max(clamped - 1.0, 1.0);
  }
  const double next = std::min(clamped + 1.0, ceiling);
  const double room = clamped <= 1.0 ? kRoomToGrowFromNative : kRoomToGrow;
  if (misses <= kRestoreMisses && next > clamped && encodeLoad * next / clamped < room) {
    return next;
  }
  return clamped;
}

// One pacing window of the image-rate controller, with the state it keeps between windows. A late game
// frame already goes out without its inserted images. Many late frames in a window used to lower the
// rate as if the images caused them, but in Prime they mostly come from the game itself: in heavy rooms
// the guest CPU thread runs ~85 % busy at 60 Hz on a 4-core i7 and stays as late with no images at all
// (2026-11-07 F8: ~20 late frames per 5 s at 60 FPS). The rate then sat at 60 for minutes because each
// reduction doubled the wait before the next try. So a reduction caused by late frames alone is a probe:
// if the next window is still about as late without the images, they were not the cause, the rate goes
// straight back up and late frames are not held against it for a while. Reductions from encode or
// present load, which the images do cost, keep the doubling wait.
//
// 2026-11-08 F8 on the pause-screen map (5,271 draws, i7-4790 at 120): three more rules, each from a
// measured failure.
// - A probe only restores the higher rate when that rate is predicted to fit (kOverload). With one image
//   costing 0.51 the late frames stayed, the rate went back to two (about 1.0 of a frame) and stayed there
//   immune for a minute: 80-100 images/s on screen but only 43-55 real game frames/s.
// - Growth that fails at once (reduced or probed in the first windows) is remembered with the load it left:
//   the map grew at 0.28-0.32 predicted 0.56-0.64, measured 0.60-0.72 with 22-44/60 late, and retried every
//   20-80 s, each try a 1-2 s fall to 30-45. The same scene now stays at the lower rate until it is clearly
//   lighter or about a minute has passed.
// - A wait earned in a heavy scene is forgotten when the scene gets clearly lighter: after the map, a short
//   spike in a corridor (load 0.15) kept the rate at 60 for ~10 s on the map's doubled wait.
struct RatePacer {
  double probeMisses = -1.0;  // late share of the window that started a probe; negative: no probe
  double probeFrom = 0.0;     // the rate the probe left
  bool probeAfterGrowth = false; // the probe started right after a growth that proved costly
  uint32_t immuneWindows = 0; // windows in which late frames are blamed on the game, not the images
  uint32_t holdWindows = 0;
  uint32_t backoffWindows = 1;
  uint32_t stableWindows = 0;
  double holdLoad = -1.0;       // load at the lower rate when the current wait was earned; negative: none
  bool holdLoadPending = false; // a reduction: the next window measures holdLoad at the lower rate
  double grewFromLoad = -1.0;   // load at the lower rate when the rate last grew
  uint32_t windowsSinceGrowth = UINT32_MAX;
  double failedGrowthLoad = -1.0; // load at the lower rate of a growth that failed at once; negative: none
  uint32_t failedGrowthAge = 0;
};
enum class RateChange : uint8_t { None, Reduced, Probe, ProbeRestored, ProbeConfirmed, Grew };
inline constexpr uint32_t kRateImmuneWindows = 60;     // ~1 minute of guest frames
inline constexpr uint32_t kRateMaximumBackoffWindows = 32;
inline constexpr uint32_t kRateFailedGrowthWindows = 2 * kRateMaximumBackoffWindows; // ~1 minute
inline constexpr uint32_t kRateGrowthFailureWindows = 2;  // a failure this soon after growing is the growth's
inline constexpr double kRateLighterScene = 0.6;        // load share that makes an earned wait obsolete
inline constexpr double kRateLighterThanFailure = 0.8;  // load share that makes a failed growth worth retrying
// A growth only counts as failed when the higher rate really cost this much: the map measured 0.60-0.72,
// a corridor's one-off spike 0.49 with 7/60 late, which must not block that light scene for a minute.
inline constexpr double kRateCostlyLoad = 0.55;

inline RateChange step_images_per_frame(RatePacer& state, double& rate, double maximum, double encodeLoad,
                                        double presentLoad, double misses) noexcept {
  constexpr double kOverload = 0.70;  // as in next_images_per_frame
  const double current = rate;
  const bool loadReduces = next_images_per_frame(current, maximum, encodeLoad, presentLoad, 0.0) < current;
  if (state.windowsSinceGrowth != UINT32_MAX) {
    ++state.windowsSinceGrowth;
  }
  const auto remember_failed_growth = [&](bool failedAtOnce) {
    if (failedAtOnce && state.grewFromLoad >= 0.0) {
      state.failedGrowthLoad = state.grewFromLoad;
      state.failedGrowthAge = 0;
    }
    state.windowsSinceGrowth = UINT32_MAX;
  };
  // `lowerRateLoad` negative: measured in the next window, since one image fewer does not scale linearly
  // (map: 0.72 with two images, 0.28-0.32 with one).
  const auto earn_wait = [&](double lowerRateLoad) {
    state.holdWindows = state.backoffWindows;
    state.backoffWindows = std::min(state.backoffWindows * 2, kRateMaximumBackoffWindows);
    state.stableWindows = 0;
    state.holdLoad = lowerRateLoad;
    state.holdLoadPending = lowerRateLoad < 0.0;
  };
  if (state.probeMisses >= 0.0) {
    const double before = state.probeMisses;
    const bool afterGrowth = state.probeAfterGrowth;
    state.probeMisses = -1.0;
    state.probeAfterGrowth = false;
    const bool restoredFits = encodeLoad * state.probeFrom / std::max(current, 1.0) < kOverload;
    if (!loadReduces && misses >= before * 0.5 && restoredFits) {
      rate = state.probeFrom;
      state.immuneWindows = kRateImmuneWindows;
      return RateChange::ProbeRestored;
    }
    // The late frames went away with the images, or the images would not fit anyway: keep the lower rate
    // for a while.
    remember_failed_growth(afterGrowth);
    earn_wait(encodeLoad);
    return RateChange::ProbeConfirmed;
  }
  double countedMisses = misses;
  if (state.immuneWindows != 0) {
    --state.immuneWindows;
    countedMisses = 0.0;
  }
  const double next = next_images_per_frame(current, maximum, encodeLoad, presentLoad, countedMisses);
  const bool soonAfterGrowth =
      state.windowsSinceGrowth <= kRateGrowthFailureWindows && encodeLoad >= kRateCostlyLoad;
  if (next < current) {
    rate = next;
    if (!loadReduces) {
      state.probeMisses = misses;
      state.probeFrom = current;
      state.probeAfterGrowth = soonAfterGrowth;
      state.windowsSinceGrowth = UINT32_MAX;
      return RateChange::Probe;
    }
    remember_failed_growth(soonAfterGrowth);
    earn_wait(-1.0);
    return RateChange::Reduced;
  }
  // A clearly lighter scene: a wait or a failed growth measured in a heavier one says nothing about it.
  if (state.holdLoadPending) {
    state.holdLoad = encodeLoad;
    state.holdLoadPending = false;
  } else if (state.holdLoad >= 0.0 && encodeLoad < state.holdLoad * kRateLighterScene) {
    state.holdWindows = 0;
    state.backoffWindows = 1;
    state.holdLoad = -1.0;
  }
  if (state.failedGrowthLoad >= 0.0 && encodeLoad < state.failedGrowthLoad * kRateLighterThanFailure) {
    state.failedGrowthLoad = -1.0;
  }
  if (next > current) {
    if (state.holdWindows != 0) {
      --state.holdWindows;
      return RateChange::None;
    }
    if (state.failedGrowthLoad >= 0.0) {
      if (++state.failedGrowthAge < kRateFailedGrowthWindows) {
        return RateChange::None;
      }
      state.failedGrowthLoad = -1.0;  // one more try after a while: the machine's load may have changed
    }
    rate = next;
    state.grewFromLoad = encodeLoad;
    state.windowsSinceGrowth = 0;
    return RateChange::Grew;
  }
  if (++state.stableWindows >= 2 * kRateMaximumBackoffWindows) {
    state.backoffWindows = 1;
    state.stableWindows = 0;
  }
  return RateChange::None;
}

// --- Display refresh alignment ----------------------------------------------------------------
// Mailbox shows the newest finished image at each refresh of the monitor. Images timed by the game's
// clock (or, in low latency mode, by when encoding ends) drift against the monitor's: while their times
// sit near a refresh, a millisecond of jitter puts two images in one refresh, the first never shown, and
// none in the next, the previous one shown twice. 120 images/s on a 120 Hz monitor then looked smooth
// most of the time and uneven for stretches (user report 2026-11-08, presents with ~2 ms of jitter).
// Aligned, every image goes out a fixed fraction into its own refresh interval and has the rest of the
// interval as margin, so a small delay no longer moves it to another refresh.

// Refresh timeline measured from the display's vertical blanks (times on the presenter's clock).
struct RefreshEstimator {
  int64_t vblank = 0;        // estimated time of the latest refresh
  double period = 0.0;       // estimated refresh period
  int64_t nominal = 0;       // period of the display mode, 0 when unknown
  uint32_t locked = 0;       // consecutive refreshes that fit the estimate
  uint32_t misfits = 0;      // consecutive refreshes that did not
  double jitter = 0.0;       // average distance between a refresh and its prediction
  double outliers = 0.0;     // share of recent refreshes far from their prediction
  // Variable refresh (G-Sync/FreeSync) follows the presents, so aligning to it chases itself and the
  // timeline stops being regular. Each time that ends a usable lock, the next lock must last longer.
  uint32_t trips = 0;
  int64_t usableAfter = 0;
  bool wasUsable = false;
};
inline constexpr uint32_t kRefreshLockSamples = 120;   // one second at 120 Hz before aligning
inline constexpr uint32_t kRefreshMaximumTrips = 3;    // then the timeline is not trusted again
inline constexpr double kRefreshFitWindow = 0.2;       // of a period: a refresh this close fits
inline constexpr double kRefreshMaximumJitter = 0.1;   // of a period, averaged
inline constexpr double kRefreshNominalTolerance = 0.01;

inline void reset_refresh_lock(RefreshEstimator& e, int64_t at) noexcept {
  e.vblank = at;
  e.period = e.nominal > 0 ? static_cast<double>(e.nominal) : 0.0;
  e.locked = 0;
  e.misfits = 0;
  e.jitter = 0.0;
  e.outliers = 0.0;
}

// `at`: when a vertical blank was observed. `nominalPeriod`: from the current display mode (0 unknown).
inline void observe_refresh(RefreshEstimator& e, int64_t at, int64_t nominalPeriod) noexcept {
  if (nominalPeriod != e.nominal) {
    const bool changed = e.nominal == 0 || nominalPeriod == 0 ||
                         std::abs(static_cast<double>(nominalPeriod - e.nominal)) >
                             static_cast<double>(e.nominal) * 0.005;
    e.nominal = nominalPeriod;
    if (changed) {
      reset_refresh_lock(e, at);
      return;
    }
  }
  if (!(e.period > 0.0)) {
    // No display mode: the first two refreshes give the period.
    const int64_t gap = at - e.vblank;
    if (e.vblank != 0 && gap >= 2'000'000 && gap <= 50'000'000) {
      e.period = static_cast<double>(gap);
    }
    e.vblank = at;
    return;
  }
  const double since = static_cast<double>(at - e.vblank);
  if (since < -0.5 * e.period || since > 120.0 * e.period) {  // clock jump or a long stall
    reset_refresh_lock(e, at);
    return;
  }
  const int64_t count = std::max<int64_t>(std::llround(since / e.period), 0);
  const double error = since - static_cast<double>(count) * e.period;
  const bool fits = std::abs(error) < kRefreshFitWindow * e.period;
  e.outliers += ((fits ? 0.0 : 1.0) - e.outliers) * 0.05;
  if (!fits) {
    if (++e.misfits >= 8) {
      reset_refresh_lock(e, at);
    }
    return;
  }
  e.misfits = 0;
  // Second-order loop: the phase follows a tenth of each error, the period a small share of it.
  constexpr double kPhaseGain = 0.1;
  constexpr double kPeriodGain = kPhaseGain * kPhaseGain / 4.0;
  e.vblank += static_cast<int64_t>(static_cast<double>(count) * e.period + error * kPhaseGain);
  if (count != 0) {
    e.period += error * kPeriodGain / static_cast<double>(count);
  }
  e.jitter += (std::abs(error) - e.jitter) * 0.05;
  if (e.nominal > 0 && std::abs(e.period - static_cast<double>(e.nominal)) >
                           kRefreshNominalTolerance * 2.0 * static_cast<double>(e.nominal)) {
    reset_refresh_lock(e, at);
    return;
  }
  ++e.locked;
}

// Whether presents may be aligned to the estimate at `now`. Call once per observation or present.
inline bool refresh_usable(RefreshEstimator& e, int64_t now) noexcept {
  const bool regular = e.period > 0.0 && e.locked >= kRefreshLockSamples &&
                       e.jitter < kRefreshMaximumJitter * e.period && e.outliers < 0.15 &&
                       (e.nominal == 0 || std::abs(e.period - static_cast<double>(e.nominal)) <
                                              kRefreshNominalTolerance * static_cast<double>(e.nominal));
  if (e.wasUsable && !regular) {
    // Lost while aligned: count it, and hold off longer each time (2 s, 8 s, 32 s, then never).
    ++e.trips;
    int64_t hold = 2'000'000'000;
    for (uint32_t trip = 1; trip < e.trips; ++trip) {
      hold *= 4;
    }
    e.usableAfter = now + hold;
  }
  const bool usable = regular && e.trips <= kRefreshMaximumTrips && now >= e.usableAfter;
  e.wasUsable = usable;
  return usable;
}

struct RefreshGrid {
  int64_t vblank = 0;  // a past refresh
  int64_t period = 0;  // 0: unknown, nothing is aligned
};

// Present this far into the interval after a refresh: late enough that it cannot reach the refresh just
// past (the compositor takes the newest image shortly after it), with most of the interval left for the
// GPU to finish it before the next one.
inline constexpr double kAlignPhase = 0.2;
// The aligned time may lead the wanted one by this much of a period, or trail it by this much; leaving the
// range moves the alignment one refresh. The range is wider than a period, and is checked on an average,
// so jitter of single images never makes it flip back and forth. `wanted` must be a steady time (the VI
// grid plus a slowly following offset), not when each image happened to be ready: a late image then goes
// out late in its own refresh instead of moving every image after it. Chosen in a simulation of 120
// images/s on a 119.98 Hz monitor (tests/render_policy_test.cpp): images ready within +-2 ms of their time
// and a GPU taking up to 3 ms reach every refresh but the ~4 the clock difference forces in 200 s, against
// ~3900 repeats and drops unaligned. The cost is ~4.5 ms of extra latency on average.
inline constexpr double kAlignEarliest = -0.2;
inline constexpr double kAlignLatest = 1.05;
struct AlignTuning {
  double phase = kAlignPhase;
  double earliest = kAlignEarliest;
  double latest = kAlignLatest;
};

struct RefreshAlignment {
  int64_t lastAligned = 0;
  int64_t lastWanted = 0;
  double offset = 0.0;  // average of aligned minus wanted
  bool valid = false;
  bool shareNext = false;
};

// `wanted`: when the schedule wants the image shown (or when it became ready). Returns when to present it:
// one refresh after the previous image when the rates match, never in the same refresh as the previous one.
// A present whose time has already passed goes out at once, as before.
inline int64_t align_to_refresh(RefreshAlignment& a, const RefreshGrid& grid, int64_t wanted,
                                const AlignTuning& tuning = {}) noexcept {
  if (grid.period <= 0) {
    a.valid = false;
    return wanted;
  }
  const double period = static_cast<double>(grid.period);
  const int64_t origin = grid.vblank + static_cast<int64_t>(tuning.phase * period);
  const auto snap = [&](double at) {
    return origin + std::llround((at - static_cast<double>(origin)) / period) * grid.period;
  };
  // A pause, a load or a jump back in the schedule starts a new alignment.
  if (a.valid && (wanted < a.lastWanted - grid.period || wanted - a.lastWanted > 30 * grid.period)) {
    a.valid = false;
  }
  if (!a.valid) {
    int64_t aligned = snap(static_cast<double>(wanted));
    if (static_cast<double>(aligned - wanted) < tuning.earliest * period) {
      aligned += grid.period;
    }
    a.offset = static_cast<double>(aligned - wanted);
    a.lastAligned = aligned;
    a.lastWanted = wanted;
    a.valid = true;
    a.shareNext = false;
    return aligned;
  }
  int64_t aligned = snap(static_cast<double>(wanted) + a.offset);
  if (aligned <= a.lastAligned + grid.period / 2) {
    if (a.shareNext) {
      // Images come a little faster than refreshes: this one shares the previous image's refresh (the
      // newer is shown) instead of pushing every later image further back.
      aligned = a.lastAligned;
    } else {
      aligned = a.lastAligned + grid.period;
    }
  }
  a.shareNext = false;
  a.offset += (static_cast<double>(aligned - wanted) - a.offset) / 16.0;
  if (a.offset < tuning.earliest * period) {
    a.offset += period;
  } else if (a.offset > tuning.latest * period) {
    a.offset -= period;
    a.shareNext = true;
  }
  a.lastAligned = aligned;
  a.lastWanted = wanted;
  return aligned;
}

// Alignment pays off when images are ready before their refresh. When many arrive late (a heavy room the
// PC cannot keep at the target), a late image goes out in the next image's refresh and collides with it;
// worse, while an image waits for its refresh the GPU queue fills with the next image's work, and D3D12 only
// flips after everything submitted before the Present. Measured on the user's PC (full replay at 120, low
// latency): calm stretches 91 % -> 99 % clean refreshes aligned, heavy stretches 64 % -> 47 %. So alignment
// steps aside while more than a tenth of a window of images is late, and returns after two calm windows.
// Lateness keeps being measured against the alignment it would have used.
struct AlignHealth {
  uint32_t images = 0;
  uint32_t late = 0;
  uint32_t calmWindows = 0;
  bool suspended = false;
};
inline constexpr uint32_t kAlignHealthWindow = 120;  // about a second at 120 images/s
inline constexpr double kAlignLateAfter = 0.25;      // of a period past its aligned time

// Returns whether alignment applies to the next image.
inline bool note_alignment_lateness(AlignHealth& h, bool late) noexcept {
  ++h.images;
  h.late += late ? 1u : 0u;
  if (h.images >= kAlignHealthWindow) {
    if (!h.suspended) {
      if (h.late * 10 > h.images) {
        h.suspended = true;
        h.calmWindows = 0;
      }
    } else if (h.late * 40 <= h.images) {
      if (++h.calmWindows >= 2) {
        h.suspended = false;
      }
    } else {
      h.calmWindows = 0;
    }
    h.images = 0;
    h.late = 0;
  }
  return !h.suspended;
}

// Alignment also needs a PC with room to spare. In heavy rooms of the low latency replay the game ran late
// far more often with it (the rate controller held one image per frame in 43 of 84 windows against 7 of
// 86 without it, 83 against 103 FPS): an image held back for its refresh keeps its back buffer and leaves
// the GPU queue waiting behind its Present. So it only applies after calm pacing windows (the rate at its
// maximum, at most 2 of 60 frames late, encode load below 0.6), and when that is lost while aligned the
// next opening waits twice as long (up to 32 windows), so it cannot keep switching in a borderline room.
struct AlignGate {
  uint32_t calm = 0;
  uint32_t needed = 2;
  uint32_t openWindows = 0;
  bool open = false;
};
inline constexpr uint32_t kAlignGateMaximumWait = 32;

inline bool step_align_gate(AlignGate& g, bool comfortable) noexcept {
  if (!comfortable) {
    if (g.open) {
      g.needed = (std::min)(g.needed * 2, kAlignGateMaximumWait);
    }
    g.open = false;
    g.calm = 0;
    g.openWindows = 0;
    return false;
  }
  if (!g.open && ++g.calm >= g.needed) {
    g.open = true;
  }
  if (g.open && ++g.openWindows >= 60) {  // a minute without trouble forgives earlier losses
    g.needed = 2;
  }
  return g.open;
}

inline bool pacing_is_comfortable(double rate, double maximum, uint32_t misses, uint32_t frames,
                                  double encodeLoad) noexcept {
  return rate >= maximum && misses * 30 <= frames && encodeLoad < 0.6;
}

// --- Anisotropic filtering -------------------------------------------------------------------
// The game's texture objects ask for GX_ANISO_1 on nearly everything, so Aurora's own cap never
// engaged. `forced` (F1 > Video, 0 = off, 2..16) overrides the request for every trilinear sampler.
enum class AnisoRequest : uint8_t { One, Two, Four, Max };

constexpr uint16_t effective_anisotropy(uint16_t forced, uint16_t cap, AnisoRequest requested) noexcept {
  if (forced > 1) {
    return forced < 16 ? forced : uint16_t{16};
  }
  const uint16_t safeCap = cap < 1 ? uint16_t{1} : cap;
  switch (requested) {
  case AnisoRequest::Two:
    return std::max<uint16_t>(safeCap / 2, 1);
  case AnisoRequest::Four:
    return safeCap;
  case AnisoRequest::One:
  case AnisoRequest::Max:
  default:
    return 1;
  }
}

// --- Point/line texture offset (BP SU_LPSIZE, 3-bit field) -----------------------------------
// Values 6 and 7 do not name an offset; RM3E01 wrote 6 in late-game effects and the generator
// aborted ("invalid tex offset 6"). Hardware and Dolphin treat them as a full texel.
constexpr float tex_offset_value(uint32_t raw) noexcept {
  switch (raw) {
  case 0:
    return 0.f;  // GX_TO_ZERO
  case 1:
    return 1.f / 16.f;
  case 2:
    return 1.f / 8.f;
  case 3:
    return 1.f / 4.f;
  case 4:
    return 1.f / 2.f;
  default:
    return 1.f;  // GX_TO_ONE and the undefined 6, 7
  }
}

// --- Asynchronous EFB readback slots ---------------------------------------------------------
// The scan visor reads small EFB copies on the CPU through a fixed table of slots. A full table that
// never released anything left every new destination on the lazy path, which the game never triggers:
// scanning silently stopped until restart. A full table now recycles the least recently scheduled
// slot that is idle.
struct SlotCandidate {
  bool idle = false;           // no readback in flight
  uint64_t lastScheduled = 0;  // monotonic request counter
};

// Index of the slot to recycle, or SIZE_MAX when every slot is busy this frame.
inline size_t pick_recycle_slot(const std::vector<SlotCandidate>& slots) noexcept {
  size_t best = SIZE_MAX;
  for (size_t i = 0; i < slots.size(); ++i) {
    if (!slots[i].idle) continue;
    if (best == SIZE_MAX || slots[i].lastScheduled < slots[best].lastScheduled) best = i;
  }
  return best;
}

// --- EFB copy texture budget -----------------------------------------------------------------
// Every guest EFB-copy destination keeps GPU textures until the guest rewrites that memory; abandoned
// destinations accumulated until Direct3D 12 ran out of heap ("CreateHeap ... Copy Conv Texture").
struct CopyDestination {
  const void* id = nullptr;
  uint32_t idleFrames = 0;  // frames since it was last produced or sampled
  uint64_t bytes = 0;       // GPU bytes only this destination keeps alive
};

// Destinations to release, longest idle first, until the total is at 3/4 of the budget. Nothing is
// planned while the total fits, and nothing idle for less than `minIdleFrames` is ever touched, so
// an over-budget scene made of busy copies is left alone rather than corrupted.
inline std::vector<const void*> plan_copy_evictions(std::vector<CopyDestination> destinations, uint64_t totalBytes,
                                                    uint64_t budgetBytes, uint32_t minIdleFrames) {
  std::vector<const void*> plan;
  if (totalBytes <= budgetBytes) return plan;
  const uint64_t target = budgetBytes / 4 * 3;
  destinations.erase(std::remove_if(destinations.begin(), destinations.end(),
                                    [&](const CopyDestination& d) { return d.idleFrames <= minIdleFrames; }),
                     destinations.end());
  std::stable_sort(destinations.begin(), destinations.end(),
                   [](const CopyDestination& a, const CopyDestination& b) { return a.idleFrames > b.idleFrames; });
  uint64_t remaining = totalBytes;
  for (const auto& d : destinations) {
    if (remaining <= target) break;
    plan.push_back(d.id);
    remaining -= std::min(remaining, d.bytes);
  }
  return plan;
}

// --- Pipeline choice for a draw (ubershader hybrid mode) -------------------------------------
// Decided once per draw when it is encoded (docs/ubershader_diseno.md):
//   1. the specialised pipeline, whenever it is compiled (the on-disk Dawn/pipeline caches make that the usual case);
//   2. in hybrid mode, the ubershader bridges the compile of a specialised pipeline that is still pending. It
//      reproduces the specialised result, so it also stands in for draws that end in an EFB copy (what such a
//      pass must never do is skip a draw);
//   3. otherwise wait, as before the option existed.
enum class UberMode : uint8_t { Off, Hybrid };
enum class PipelineChoice : uint8_t { Specialized, Uber, WaitForSpecialized };

struct PipelineAvailability {
  bool specializedReady = false;
  bool uberReady = false;
};

constexpr PipelineChoice choose_pipeline(UberMode mode, const PipelineAvailability& a) noexcept {
  if (a.specializedReady) return PipelineChoice::Specialized;
  if (mode == UberMode::Hybrid && a.uberReady) return PipelineChoice::Uber;
  return PipelineChoice::WaitForSpecialized;
}

// --- Which queued compile a worker takes next ------------------------------------------------
// Urgent: a pipeline some draw is (or will be) blocked on. Bridged: a specialised pipeline met in play whose
// draws the ubershader already covers; it shares the CPU with the game, so at most `maxBridged` compile at
// once and at low thread priority. Background: the boot prewarm of cached recipes.
enum class CompileSlot : uint8_t { None, Urgent, Bridged, Background };

// `activeUrgent` / `maxInPlay`: urgent and bridged compiles together never exceed maxInPlay (see
// in_play_compile_cap); the defaults keep urgent work uncapped.
constexpr CompileSlot pick_compile_slot(bool urgentQueued, bool bridgedQueued, size_t activeBridged, size_t maxBridged,
                                        bool backgroundQueued, size_t activeBackground, size_t maxBackground,
                                        size_t activeUrgent = 0, size_t maxInPlay = static_cast<size_t>(-1)) noexcept {
  const size_t inPlay = activeUrgent + activeBridged;
  if (urgentQueued && inPlay < maxInPlay) return CompileSlot::Urgent;
  if (bridgedQueued && activeBridged < maxBridged && inPlay < maxInPlay) return CompileSlot::Bridged;
  if (backgroundQueued && activeBackground < maxBackground) return CompileSlot::Background;
  return CompileSlot::None;
}

// --- How many compiles may run at once during play ---------------------------------------------
// In skip-unready mode every draw's wait is bounded (4 / 6 ms, then the draw is skipped), and any
// such wait promotes its pipeline to urgent. At the first hypermode (PED suit test, F8 of
// 2026-10-31) dozens of new materials in EFB copy passes did that at once: all six workers compiled
// at normal priority (D3D12 compiles of 100-1500 ms) next to the driver's own threads, the CPU sat at
// 100 % and the game ran at 31-39 FPS for four seconds. Capping urgent + bridged compiles leaves the
// game, GX, decoder and frame threads their cores; a capped compile only means a skipped or
// ubershader-drawn material for a little longer. Prewarm (background) keeps its own cap. Without
// skip-unready a draw waits for an urgent compile without limit: no cap there.
constexpr size_t in_play_compile_cap(size_t physicalCores, bool skipUnreadyPipelines) noexcept {
  if (!skipUnreadyPipelines) return static_cast<size_t>(-1);
  return physicalCores > 4 ? physicalCores - 2 : 2;
}

// --- Thread priority of a compile --------------------------------------------------------------
// Bridged and prewarm compiles never take a core from the game, GX or frame threads. An urgent
// compile blocks a draw: above normal only when draws really wait for it (skip-unready off); in
// skip-unready mode every wait is bounded, and six above-normal workers on an 8-thread CPU took the
// game thread's cores during area loads (F8 captures of 2026-10-28).
enum class CompilePriority : uint8_t { BelowNormal, Normal, AboveNormal };

constexpr CompilePriority compile_priority(CompileSlot slot, bool skipUnreadyPipelines) noexcept {
  if (slot != CompileSlot::Urgent) return CompilePriority::BelowNormal;
  return skipUnreadyPipelines ? CompilePriority::Normal : CompilePriority::AboveNormal;
}

// --- How long a draw may wait for its pipeline -------------------------------------------------
// Skip-unready mode (the default) bounds every wait per encoded frame: ordinary passes 4 ms, then
// the draw is skipped this frame; passes that end in an EFB copy or an offscreen bake (whose draws
// might never be re-issued) get their own 6 ms. An unbounded wait froze Prime for 140-360 ms per new
// material (D3D12 compiles of 180-500 ms). Only skip-unready off waits without limit.
enum class PipelineWait : uint8_t { Unbounded, FrameBudget, PersistentPassBudget };
inline constexpr uint32_t kFramePipelineWaitBudgetMs = 4;
inline constexpr uint32_t kPersistentPassPipelineWaitBudgetMs = 6;

constexpr PipelineWait pipeline_wait(bool skipUnreadyPipelines, bool requireReady) noexcept {
  if (!skipUnreadyPipelines) return PipelineWait::Unbounded;
  return requireReady ? PipelineWait::PersistentPassBudget : PipelineWait::FrameBudget;
}

constexpr uint32_t pipeline_wait_budget_ms(PipelineWait wait) noexcept {
  return wait == PipelineWait::PersistentPassBudget ? kPersistentPassPipelineWaitBudgetMs : kFramePipelineWaitBudgetMs;
}

// --- Which pipeline a draw binds first ---------------------------------------------------------
// After the ubershader decision (choose_pipeline): a draw whose per-pixel lighting / enhanced
// material variant is still compiling while its vertex-lit fallback is ready binds the fallback
// right away, instead of first waiting on the variant (a 200-500 ms D3D12 compile that would block a
// persistent pass). The draw still happens, with per-vertex lighting until the compile lands. If the
// first choice fails to bind, the caller still tries the fallback.
enum class FirstBind : uint8_t { Uber, Fallback, Primary };

struct DrawPipelines {
  bool uberChosen = false;       // choose_pipeline answered Uber for this draw
  bool hasFallback = false;      // a vertex-lit fallback was recorded
  bool primaryIsCurrent = false; // the pass already has the primary bound
  bool primaryReady = false;
  bool fallbackReady = false;
};

constexpr FirstBind first_bind(const DrawPipelines& d) noexcept {
  if (d.uberChosen) return FirstBind::Uber;
  if (d.hasFallback && !d.primaryIsCurrent && !d.primaryReady && d.fallbackReady) return FirstBind::Fallback;
  return FirstBind::Primary;
}

// --- Spare EFB copy targets of other sizes -----------------------------------------------------
// The copy texture pool keeps retired targets per destination+size. A copy whose size changes every
// frame (a zoom effect) left spares of every old size until the 5 s budget sweep: 768 MiB in a few
// seconds at high internal resolution (2026-10-30). Per destination only the `keep` newest spares of
// sizes other than the one being acquired survive; targets referenced elsewhere are never spares.
// `pool` is in insertion order (oldest first); returns the indices to drop, highest first.
struct PoolSpare {
  const void* dest = nullptr;
  bool sameKey = false;  // same destination, size and format as the copy being acquired
  bool spare = false;    // only the pool still references it
};

inline std::vector<size_t> plan_other_shape_spare_drops(const std::vector<PoolSpare>& pool, const void* dest,
                                                        size_t keep) {
  std::vector<size_t> drops;
  size_t kept = 0;
  for (size_t i = pool.size(); i-- > 0;) {
    const auto& entry = pool[i];
    if (entry.dest != dest || entry.sameKey || !entry.spare) continue;
    if (++kept > keep) drops.push_back(i);
  }
  return drops;
}

// --- When the GX stream writer hands its buffer to the decoder thread ----------------------------
// fifo::commit runs after every command group (each draw). A chunk always goes once it reaches the
// soft size; an idle decoder takes a smaller one so it works in parallel. That eager size was 2 KiB:
// the decoder is idle most of the time, so nearly every draw handed off and woke it through the
// kernel, on the GX thread, which is the one that saturates in heavy scenes (the Norion cinematic,
// 2026-10-30: 91 % busy, decoder 3 %). 16 KiB keeps the decoder in parallel with about an eighth of
// the wake-ups; sync points hand off whatever is buffered regardless.
inline constexpr uint32_t kSoftHandOffBytes = 32 * 1024;
inline constexpr uint32_t kEagerHandOffBytes = 16 * 1024;

constexpr bool hand_off_now(uint32_t bufferedBytes, bool decoderIdle) noexcept {
  return bufferedBytes >= kSoftHandOffBytes || (bufferedBytes >= kEagerHandOffBytes && decoderIdle);
}

}  // namespace aurora::gx::policy
