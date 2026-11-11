#include "frame_interpolation.hpp"

#include "../internal.hpp"
#include "../diagnostic_env.hpp"
#include "aurora/gfx.h"

#include <absl/container/flat_hash_map.h>

// Guest matrices really do carry NaN/Inf, and the isfinite guards here keep them
// out of the MatchEdge sort. Needs -fno-finite-math-only (see runtime/CMakeLists.txt).
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "frame_interpolation.cpp must be compiled with -fno-finite-math-only; its NaN filtering is load-bearing for memory safety (see comment above)."
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aurora::gx {
namespace detail {
std::atomic_uint32_t g_frameInterpolationFps{0};
} // namespace detail

namespace {
static Module Log("aurora::gx::interp");

// Diagnostics for aurora_get_frame_interpolation_diagnostics. Individually atomic:
// one writer, and a torn read is harmless for an overlay.
std::atomic_uint32_t s_diagCandidates{0};
std::atomic_uint32_t s_diagMatchable{0};
std::atomic_uint32_t s_diagMatches{0};
std::atomic_bool s_diagEligible{false};
std::atomic_bool s_diagReplaySafe{true};
std::atomic_uint64_t s_diagFramesSealed{0};
std::atomic_uint64_t s_diagFramesLowMatch{0};
std::atomic_uint64_t s_diagFramesReplayUnsafe{0};
std::atomic_uint64_t s_diagSlotReductions{0};
std::atomic_uint64_t s_diagLateSealDrops{0};
std::atomic_uint64_t s_diagSealsWithBoundary{0};
std::atomic_uint64_t s_diagMultiBoundary{0};
std::atomic_uint64_t s_diagSealsCarried{0};
std::atomic_uint64_t s_diagSealsComplete{0};
std::atomic_uint64_t s_diagCarriedDraws{0};
std::atomic_uint64_t s_diagSealsReplayed{0};
std::atomic_uint64_t s_diagCameraCuts{0};
std::atomic_uint64_t s_diagBorrowedDraws{0};
std::atomic_uint64_t s_pairEvaluations{0};
std::atomic_uint64_t s_diagProbeRestores{0};
std::atomic_uint64_t s_diagCameraEstimates{0};
// Persistent worker pool for the per-sample interpolation tasks. libc++ has no
// parallel execution policies, so without it the seal loop runs serially. Leaked.
class InterpolationWorkerPool {
public:
  static InterpolationWorkerPool& instance() {
    static InterpolationWorkerPool* pool = new InterpolationWorkerPool();
    return *pool;
  }

  // Runs fn(index) for every index in [0, count). Returns once every index has
  // been processed. Not reentrant; only the producer/seal thread dispatches.
  template <typename Fn>
  void run(size_t count, const Fn& fn) {
    if (count == 0) {
      return;
    }
    if (m_workers.empty()) {
      for (size_t i = 0; i < count; ++i) {
        fn(i);
      }
      return;
    }
    // Every dispatch has its own job state. Shared counters reset per dispatch let a worker
    // woken late for the previous job claim a chunk of the next one with the previous count:
    // it subtracted more than was left, m_remaining wrapped and the dispatcher spun forever
    // (hang at 120 Hz, 08/11/2026: workers parked, the seal thread yielding in this loop).
    // A late worker now holds its own, exhausted job and never touches the new one.
    auto job = std::make_shared<Job>();
    job->invoke = [&fn](size_t index) { fn(index); };
    job->count = count;
    job->remaining.store(count, std::memory_order_relaxed);
    {
      // Job and generation bump publish under one lock, so a worker can never observe a
      // half-written job.
      std::lock_guard lock(m_mutex);
      m_job = job;
      ++m_generation;
    }
    m_wake.notify_all();
    consume(*job);
    // Wait for the workers to leave this job's consume() entirely: `fn` lives in the caller.
    while (job->remaining.load(std::memory_order_acquire) != 0 ||
           job->active.load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }
    std::lock_guard lock(m_mutex);
    if (m_job == job) m_job.reset();
  }

private:
  static constexpr size_t kChunk = 16;

  struct Job {
    std::function<void(size_t)> invoke;
    size_t count = 0;
    std::atomic_size_t next{0};
    std::atomic_size_t remaining{0};
    std::atomic_size_t active{0};
  };

  InterpolationWorkerPool() {
    const unsigned hardware = std::thread::hardware_concurrency();
    // The caller helps too. Cap the helpers: tasks are short memcpy+math, so dispatch
    // overhead and memory bandwidth dominate past a few threads.
    const unsigned helpers = hardware > 2 ? std::min(hardware - 1, 6u) : 0;
    m_workers.reserve(helpers);
    for (unsigned i = 0; i < helpers; ++i) {
      m_workers.emplace_back([this] { worker_loop(); });
    }
  }

  static void consume(Job& job) {
    while (true) {
      const size_t begin = job.next.fetch_add(kChunk, std::memory_order_relaxed);
      if (begin >= job.count) {
        return;
      }
      const size_t end = std::min(begin + kChunk, job.count);
      for (size_t index = begin; index < end; ++index) {
        job.invoke(index);
      }
      job.remaining.fetch_sub(end - begin, std::memory_order_release);
    }
  }

  void worker_loop() {
    uint64_t seenGeneration = 0;
    while (true) {
      std::shared_ptr<Job> job;
      {
        std::unique_lock lock(m_mutex);
        m_wake.wait(lock, [&] { return m_generation != seenGeneration; });
        seenGeneration = m_generation;
        job = m_job;
        // Counted under the mutex, on this job only: the dispatcher returns once no worker is
        // inside it. A worker that takes a finished job finds no chunk left and never calls it.
        if (job) job->active.fetch_add(1, std::memory_order_relaxed);
      }
      if (!job) continue;
      consume(*job);
      job->active.fetch_sub(1, std::memory_order_release);
    }
  }

  std::vector<std::thread> m_workers;
  std::mutex m_mutex;
  std::condition_variable m_wake;
  uint64_t m_generation = 0;
  std::shared_ptr<Job> m_job;  // the current dispatch (guarded by m_mutex)
};
struct FrameTransformSnapshot {
  Mat4x4<float> projection{};
  HashType viewportIdentity = 0;
  Mat3x4<float> position{};
  Mat3x4<float> normal{};
  uint16_t usedMatrixMask = 1;
  struct IndexedMatrices {
    std::array<Mat3x4<float>, MaxPnMtx> position{};
    std::array<Mat3x4<float>, MaxPnMtx> normal{};
    // Content hash of each used slot's position/normal pair. A mesh split across draws
    // repeats bone matrices byte for byte, so the seal can pair slots across draws.
    std::array<HashType, MaxPnMtx> slotHash{};
  };
  std::unique_ptr<IndexedMatrices> indexedMatrices;
};

struct FrameTransformEntry {
  FrameInterpolationDrawIdentity identity;
  FrameTransformSnapshot transform;
  // The draw's texture matrices in InterpolationFrame::textureMatrices.
  uint32_t textureMatrixBegin = 0;
  uint32_t textureMatrixCount = 0;
  // Constant-velocity prediction of a non-indexed transform, so repeated meshes match
  // on where they will be. Not for indexed draws: PNMTXIDX slot identity pairs those.
  Mat3x4<float> predictedPosition{};
  bool hasPrediction = false;
};

struct Quaternion {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

struct InterpolationFrame;

struct PendingUniformInterpolation {
  InterpolationFrame* frame = nullptr;
  size_t currentTransformIndex = 0;
  const uint8_t* sourceUniformData = nullptr;
  uint8_t* uniformData = nullptr;
  size_t uniformSize = 0;
  size_t projectionOffset = 0;
  size_t positionOffset = 0;
  size_t normalOffset = 0;
  size_t currentMatrix = 0;
  float weight = 1.f;
  bool indexedMatrices = false;
};

// One game frame: the draws between two display copies. Prime seals a batch at its VI retrace, which
// usually falls inside the next frame's world at a point that moves every frame (214 + 316 draws, then
// 28 + 457), so one game frame spans two sealed batches. Matching, cadence and slot weights follow the
// game frame; a batch only decides when the recorded draws are finalized.
struct InterpolationFrame {
  std::vector<FrameTransformEntry> transforms;
  // Texture matrices of the frame's draws: uniform offset and value (see FrameTransformEntry).
  std::vector<std::pair<uint32_t, Mat3x4<float>>> textureMatrices;
  std::unordered_map<HashType, std::vector<size_t>> exactIndices;
  std::unordered_map<HashType, std::vector<size_t>> stableIndices;
  std::unordered_map<HashType, std::vector<size_t>> geometryIndices;
  // Per transform: the previous frame's entry it interpolates from, SIZE_MAX when none.
  std::vector<size_t> toPrevious;
  // Per transform left without a partner: the partner of a matched draw of this frame with the very
  // same matrix (a wall piece beside its siblings), so it moves with them instead of standing still.
  // Not a claim and not a match: no prediction or camera statistics come from it.
  std::vector<size_t> borrowedPrevious;
  // The camera's motion since the previous frame in view space (current * inverse(previous) of the
  // static draws), found by vote; references matching for draws that have no motion history.
  Mat3x4<float> cameraDelta{};
  bool cameraDeltaValid = false;
  // Per transform: already taken by a draw of the next frame. Survives across seals, because the next
  // frame's draws are matched in two parts (before and after the seal that splits them).
  std::vector<uint8_t> claimed;
  // Transforms [0, matchedCount) went through matching at an earlier seal.
  size_t matchedCount = 0;
  bool hasIndexedMatrices = false;
  bool complete = false;
  // Low-latency mode: the frame is shown as soon as it is ready and its inserted images predict past it
  // (weights above 1) instead of leading up to it.
  bool extrapolated = false;
  policy::FrameCadence cadence{};
  double bias = 1.0;
  uint32_t candidates = 0;
  uint32_t matchable = 0;
  uint32_t matches = 0;
  // Camera cut detection over the frame's matched one-matrix draws (see camera_motion).
  uint32_t motionPairs = 0;
  uint32_t cutPairs = 0;
};

// Oldest first. Normally the last complete frame and the one being recorded; a batch that holds two
// display copies keeps one more until its seal.
std::deque<std::unique_ptr<InterpolationFrame>> s_frames;
std::vector<std::unique_ptr<InterpolationFrame>> s_framePool;
// The last frame whose display copy was recorded in the current batch: the one the seal presents.
InterpolationFrame* s_completedInBatch = nullptr;
uint32_t s_boundariesInBatch = 0;

// Free list for the indexed-matrix snapshots; per-draw heap allocation was the
// hottest cost in this path. Unused slots keep stale data, consumers mask first.
std::vector<std::unique_ptr<FrameTransformSnapshot::IndexedMatrices>> s_indexedMatricesPool;
constexpr size_t kMaximumPooledIndexedMatrices = 4096;

std::unique_ptr<FrameTransformSnapshot::IndexedMatrices> acquire_indexed_matrices() {
  if (s_indexedMatricesPool.empty()) {
    return std::make_unique<FrameTransformSnapshot::IndexedMatrices>();
  }
  auto block = std::move(s_indexedMatricesPool.back());
  s_indexedMatricesPool.pop_back();
  return block;
}

void recycle_transform_entries(std::vector<FrameTransformEntry>& entries) noexcept {
  for (auto& entry : entries) {
    if (entry.transform.indexedMatrices &&
        s_indexedMatricesPool.size() < kMaximumPooledIndexedMatrices) {
      s_indexedMatricesPool.push_back(std::move(entry.transform.indexedMatrices));
    }
  }
  entries.clear();
}

// clear() destroys every node, so empty the vectors in place and let a stable scene
// reuse them. Fall back to a real clear once the map outgrows the live set.
void clear_index_map_keep_nodes(std::unordered_map<HashType, std::vector<size_t>>& map,
                                size_t liveEntries) {
  if (map.size() > liveEntries + 256) {
    map.clear();
    return;
  }
  for (auto& entry : map) {
    entry.second.clear();
  }
}
std::vector<PendingUniformInterpolation> s_pendingUniformInterpolations;
std::atomic_bool s_frameInterpolationReplaySafe{true};
uint32_t s_previousInterpolationFps = 0;

// Adaptive image rate: the target moves at most one step per pacing window, while the cadence
// below is latched per game frame and must not move mid-frame.
std::atomic<double> s_imagesPerFrameTarget{0.0};
// Inserted images latched for the game frame being recorded (its draws stage this many copies).
std::atomic_uint32_t s_activeInterpolationSamples{0};
double s_cadenceNextTick = 0.0;
double s_cadenceBiasRate = 0.0;
double s_cadenceBias = 1.0;

// What the last seal presents: the cadence of the game frame whose display copy it holds.
std::atomic_bool s_hasInterpolatedFrame{false};
std::atomic_uint32_t s_sealSamples{0};
std::array<float, MaxInterpolatedFrames> s_sealWeights{};
std::atomic_bool s_sealPresentsNative{true};
std::atomic<double> s_sealBias{1.0};
std::atomic_bool s_sealExtrapolated{false};
// F1 "Low latency": inserted images extrapolate after the frame instead of interpolating before it.
std::atomic_bool s_lowLatency{false};

// Set when the producer already overran its retrace budget; the next seal then skips
// inserted slots for that one frame, which helps the late frame catch back up.
std::atomic_bool s_dropInterpolationAtSeal{false};

// Windowed backstop for sustained overload. The wide reduce/restore gap is the
// hysteresis, so a scene that can sustain N slots settles there instead of flapping.
std::atomic_uint32_t s_pacingWindowFrames{0};
std::atomic_uint32_t s_pacingWindowMisses{0};
std::atomic<double> s_pacingWindowLoad{0.0};
std::atomic<double> s_pacingWindowPresentLoad{0.0};
std::atomic_uint32_t s_pacingWindowLoadSamples{0};
constexpr uint32_t kPacingWindowFrames = 60; // ~1 second of guest frames
policy::RatePacer s_ratePacer;
// Display-refresh alignment only with headroom (policy::step_align_gate); read by the presenter.
policy::AlignGate s_alignGate;
std::atomic_bool s_alignHeadroom{false};

// The fixed targets are written against 60 logical frames per second, like the VI they run on.
double maximum_images_per_frame(uint32_t targetFps) noexcept {
  if (targetFps == 0) {
    return 0.0;
  }
  if (targetFps == AURORA_FRAME_INTERPOLATION_UNCAPPED) {
    return MaxInterpolatedFrames + 1.0;
  }
  return std::min(static_cast<double>(targetFps) / 60.0, MaxInterpolatedFrames + 1.0);
}

double maximum_images_per_frame() noexcept { return maximum_images_per_frame(frame_interpolation_fps()); }

uint32_t samples_for_images_per_frame(double imagesPerFrame) noexcept {
  return imagesPerFrame > 1.0 ? static_cast<uint32_t>(std::ceil(imagesPerFrame)) - 1u : 0u;
}
constexpr float kMinimumScale = 1.0e-5f;
constexpr float kMaximumTranslationPerFrame = 1500.0f;
constexpr float kMinimumQuaternionDot = 0.70710678f;
constexpr size_t kNoPreparedPair = std::numeric_limits<size_t>::max();

HashType combine_identity(HashType first, HashType second) noexcept {
  return xxh3_hash(second, first);
}

HashType stable_identity(const FrameInterpolationDrawIdentity& identity) noexcept {
  return combine_identity(combine_identity(identity.pipeline, identity.texture),
                          identity.matrixTopology);
}

float dot3(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> cross3(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
  return {
      a[1] * b[2] - a[2] * b[1],
      a[2] * b[0] - a[0] * b[2],
      a[0] * b[1] - a[1] * b[0],
  };
}

Quaternion quaternion_from_rotation(const std::array<std::array<float, 3>, 3>& m) noexcept {
  Quaternion q;
  const float trace = m[0][0] + m[1][1] + m[2][2];
  if (trace > 0.0f) {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    q.w = 0.25f * s;
    q.x = (m[2][1] - m[1][2]) / s;
    q.y = (m[0][2] - m[2][0]) / s;
    q.z = (m[1][0] - m[0][1]) / s;
  } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
    const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
    q.w = (m[2][1] - m[1][2]) / s;
    q.x = 0.25f * s;
    q.y = (m[0][1] + m[1][0]) / s;
    q.z = (m[0][2] + m[2][0]) / s;
  } else if (m[1][1] > m[2][2]) {
    const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
    q.w = (m[0][2] - m[2][0]) / s;
    q.x = (m[0][1] + m[1][0]) / s;
    q.y = 0.25f * s;
    q.z = (m[1][2] + m[2][1]) / s;
  } else {
    const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
    q.w = (m[1][0] - m[0][1]) / s;
    q.x = (m[0][2] + m[2][0]) / s;
    q.y = (m[1][2] + m[2][1]) / s;
    q.z = 0.25f * s;
  }
  return q;
}

bool decompose_affine(const Mat3x4<float>& matrix, std::array<std::array<float, 3>, 3>& rotation,
                      std::array<float, 3>& scale, std::array<float, 3>& translation,
                      Quaternion& quaternion) noexcept {
  const std::array<Vec4<float>, 3> rows{matrix.m0, matrix.m1, matrix.m2};
  // V*T*R*S puts non-uniform model scale on the columns, so scale must be read off the
  // columns; row extraction misreads R*S as shear whenever the rotation tilts an axis.
  for (size_t row = 0; row < 3; ++row) {
    translation[row] = rows[row].w();
    if (!std::isfinite(translation[row])) {
      return false;
    }
  }
  for (size_t column = 0; column < 3; ++column) {
    const float x = rows[0][column];
    const float y = rows[1][column];
    const float z = rows[2][column];
    scale[column] = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(scale[column]) || scale[column] < kMinimumScale) {
      return false;
    }
    rotation[0][column] = x / scale[column];
    rotation[1][column] = y / scale[column];
    rotation[2][column] = z / scale[column];
  }

  const auto rotationColumn = [&rotation](size_t column) noexcept {
    return std::array<float, 3>{rotation[0][column], rotation[1][column], rotation[2][column]};
  };
  if (std::abs(dot3(rotationColumn(0), rotationColumn(1))) > 0.05f ||
      std::abs(dot3(rotationColumn(0), rotationColumn(2))) > 0.05f ||
      std::abs(dot3(rotationColumn(1), rotationColumn(2))) > 0.05f) {
    return false;
  }

  const float determinant = dot3(rotation[0], cross3(rotation[1], rotation[2]));
  if (!std::isfinite(determinant) || std::abs(std::abs(determinant) - 1.0f) > 0.1f) {
    return false;
  }
  if (determinant < 0.0f) {
    scale[0] = -scale[0];
    for (size_t row = 0; row < 3; ++row) {
      rotation[row][0] = -rotation[row][0];
    }
  }

  quaternion = quaternion_from_rotation(rotation);
  return std::isfinite(quaternion.x) && std::isfinite(quaternion.y) && std::isfinite(quaternion.z) &&
         std::isfinite(quaternion.w);
}

std::array<std::array<float, 3>, 3> rotation_from_quaternion(const Quaternion& q) noexcept {
  const float xx = q.x * q.x;
  const float yy = q.y * q.y;
  const float zz = q.z * q.z;
  const float xy = q.x * q.y;
  const float xz = q.x * q.z;
  const float yz = q.y * q.z;
  const float wx = q.w * q.x;
  const float wy = q.w * q.y;
  const float wz = q.w * q.z;
  return {{
      {{1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz), 2.0f * (xz + wy)}},
      {{2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx)}},
      {{2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy)}},
  }};
}

struct PreparedAffinePair {
  Mat3x4<float> previous{};
  Mat3x4<float> current{};
  std::array<float, 3> previousScale{};
  std::array<float, 3> currentScale{};
  std::array<float, 3> previousTranslation{};
  std::array<float, 3> currentTranslation{};
  Quaternion previousQuaternion{};
  Quaternion currentQuaternion{};
  // How far the pair moves in one frame; extrapolation past the frame is only done for small motion.
  float rotationDot = 1.0f;
  float translationDeltaSquared = 0.0f;
  // Rigid relative motion (current = delta * previous with delta a rotation plus translation): the pair
  // follows delta's screw motion, which depends on delta alone. Every static piece of the scene shares
  // the camera's delta, so they all land on the same image and no seam opens between them, whatever
  // their origins. Decomposing each matrix instead moves a piece's origin on a straight chord while the
  // camera turns, a different error per origin. Not rigid (scale changing): decomposition below.
  bool screw = false;
  std::array<double, 3> screwAxis{};
  double screwAngle = 0.0;
  std::array<double, 3> screwPerpendicular{};  // part of delta's translation across the axis
  std::array<double, 3> screwAlong{};          // part along the axis
  bool linear = false;
  bool identical = false;
  bool valid = false;
};

struct PreparedTransformInterpolation {
  size_t nonIndexedPairOffset = kNoPreparedPair;
  std::array<size_t, MaxPnMtx> indexedPairOffsets{};
  // Previous-frame entry the projection interpolates from. For a draw that borrowed
  // its matrices from a sibling, that is the sibling's partner.
  size_t previousProjectionEntry = kNoPreparedPair;
  bool indexedValid = false;
  // Decided for every draw that shares bones with this one (one skeleton split across draws), so the
  // pieces never part: one piece without a valid pair keeps them all at the frame, and one bone too fast
  // to predict keeps them all from being carried past it.
  bool skeletonFrozen = false;
  bool skeletonUnpredictable = false;

  PreparedTransformInterpolation() {
    indexedPairOffsets.fill(kNoPreparedPair);
  }
};

// delta = current * inverse(previous), when it is a rotation plus a translation.
bool relative_rigid_motion(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                           std::array<std::array<double, 3>, 3>& rotation,
                           std::array<double, 3>& translation) noexcept {
  const std::array<Vec4<float>, 3> p{previous.m0, previous.m1, previous.m2};
  const std::array<Vec4<float>, 3> c{current.m0, current.m1, current.m2};
  double a[3][3];
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 4; ++k) {
      if (!std::isfinite(p[r][k]) || !std::isfinite(c[r][k])) {
        return false;
      }
    }
    for (int k = 0; k < 3; ++k) {
      a[r][k] = p[r][k];
    }
  }
  const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                     a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                     a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (!std::isfinite(det) || std::abs(det) < 1.0e-15) {
    return false;
  }
  double inv[3][3];
  inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
  inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
  inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
  inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
  inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
  inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
  inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
  inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
  inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) {
      rotation[r][k] = c[r][0] * inv[0][k] + c[r][1] * inv[1][k] + c[r][2] * inv[2][k];
    }
  }
  // Rigid within float noise; a scale that changes between the frames is not.
  constexpr double kRigidTolerance = 1.0e-3;
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) {
      const double product = rotation[r][0] * rotation[k][0] + rotation[r][1] * rotation[k][1] +
                             rotation[r][2] * rotation[k][2];
      if (!(std::abs(product - (r == k ? 1.0 : 0.0)) <= kRigidTolerance)) {
        return false;
      }
    }
  }
  const double rotationDet =
      rotation[0][0] * (rotation[1][1] * rotation[2][2] - rotation[1][2] * rotation[2][1]) -
      rotation[0][1] * (rotation[1][0] * rotation[2][2] - rotation[1][2] * rotation[2][0]) +
      rotation[0][2] * (rotation[1][0] * rotation[2][1] - rotation[1][1] * rotation[2][0]);
  if (!(rotationDet > 0.0)) {
    return false;
  }
  for (int r = 0; r < 3; ++r) {
    translation[r] = c[r][3] - (rotation[r][0] * p[0][3] + rotation[r][1] * p[1][3] +
                                rotation[r][2] * p[2][3]);
  }
  return true;
}

bool prepare_screw(PreparedAffinePair& pair) noexcept {
  std::array<std::array<double, 3>, 3> rotation{};
  std::array<double, 3> translation{};
  if (!relative_rigid_motion(pair.previous, pair.current, rotation, translation)) {
    return false;
  }
  // Axis and angle (0..pi) from the rotation's quaternion.
  const double trace = rotation[0][0] + rotation[1][1] + rotation[2][2];
  double qw = 0.0;
  double qx = 0.0;
  double qy = 0.0;
  double qz = 0.0;
  if (trace > 0.0) {
    const double s = std::sqrt(trace + 1.0) * 2.0;
    qw = 0.25 * s;
    qx = (rotation[2][1] - rotation[1][2]) / s;
    qy = (rotation[0][2] - rotation[2][0]) / s;
    qz = (rotation[1][0] - rotation[0][1]) / s;
  } else if (rotation[0][0] > rotation[1][1] && rotation[0][0] > rotation[2][2]) {
    const double s = std::sqrt(1.0 + rotation[0][0] - rotation[1][1] - rotation[2][2]) * 2.0;
    qw = (rotation[2][1] - rotation[1][2]) / s;
    qx = 0.25 * s;
    qy = (rotation[0][1] + rotation[1][0]) / s;
    qz = (rotation[0][2] + rotation[2][0]) / s;
  } else if (rotation[1][1] > rotation[2][2]) {
    const double s = std::sqrt(1.0 + rotation[1][1] - rotation[0][0] - rotation[2][2]) * 2.0;
    qw = (rotation[0][2] - rotation[2][0]) / s;
    qx = (rotation[0][1] + rotation[1][0]) / s;
    qy = 0.25 * s;
    qz = (rotation[1][2] + rotation[2][1]) / s;
  } else {
    const double s = std::sqrt(1.0 + rotation[2][2] - rotation[0][0] - rotation[1][1]) * 2.0;
    qw = (rotation[1][0] - rotation[0][1]) / s;
    qx = (rotation[0][2] + rotation[2][0]) / s;
    qy = (rotation[1][2] + rotation[2][1]) / s;
    qz = 0.25 * s;
  }
  const double length = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
  if (!std::isfinite(length) || length < 1.0e-9) {
    return false;
  }
  qw /= length;
  qx /= length;
  qy /= length;
  qz /= length;
  if (qw < 0.0) {
    qw = -qw;
    qx = -qx;
    qy = -qy;
    qz = -qz;
  }
  const double sinHalf = std::sqrt(qx * qx + qy * qy + qz * qz);
  const double angle = 2.0 * std::atan2(sinHalf, qw);
  std::array<double, 3> axis{0.0, 0.0, 1.0};
  if (sinHalf > 1.0e-12) {
    axis = {qx / sinHalf, qy / sinHalf, qz / sinHalf};
  }
  const double alongLength =
      axis[0] * translation[0] + axis[1] * translation[1] + axis[2] * translation[2];
  const double translationSquared = translation[0] * translation[0] +
                                    translation[1] * translation[1] +
                                    translation[2] * translation[2];
  if (!std::isfinite(angle) || !std::isfinite(translationSquared)) {
    return false;
  }
  // Same limits as the decomposition: 90 degrees and kMaximumTranslationPerFrame per frame.
  if (static_cast<float>(qw) < kMinimumQuaternionDot ||
      translationSquared > double{kMaximumTranslationPerFrame} * kMaximumTranslationPerFrame) {
    pair.valid = false;
    return true;
  }
  pair.screw = true;
  pair.screwAxis = axis;
  pair.screwAngle = angle;
  for (int i = 0; i < 3; ++i) {
    pair.screwAlong[i] = axis[i] * alongLength;
    pair.screwPerpendicular[i] = translation[i] - pair.screwAlong[i];
  }
  pair.rotationDot = static_cast<float>(qw);
  pair.translationDeltaSquared = static_cast<float>(translationSquared);
  pair.valid = true;
  return true;
}

// delta^s * previous along delta's screw: rotation by s * angle about an axis through the pivot, plus s
// times the part of the translation along it. s = 1 gives current exactly; s above 1 continues the motion.
void evaluate_screw(const PreparedAffinePair& pair, double s, Mat3x4<float>& output) noexcept {
  const auto& k = pair.screwAxis;
  const double angle = pair.screwAngle * s;
  const double sine = std::sin(angle);
  const double halfSine = std::sin(angle * 0.5);
  const double oneMinusCosine = 2.0 * halfSine * halfSine;
  std::array<std::array<double, 3>, 3> r{};
  // Rodrigues: R = I + sin * K + (1 - cos) * K^2.
  const double kxx = k[0] * k[0], kyy = k[1] * k[1], kzz = k[2] * k[2];
  const double kxy = k[0] * k[1], kxz = k[0] * k[2], kyz = k[1] * k[2];
  r[0] = {1.0 - oneMinusCosine * (kyy + kzz), oneMinusCosine * kxy - sine * k[2],
          oneMinusCosine * kxz + sine * k[1]};
  r[1] = {oneMinusCosine * kxy + sine * k[2], 1.0 - oneMinusCosine * (kxx + kzz),
          oneMinusCosine * kyz - sine * k[0]};
  r[2] = {oneMinusCosine * kxz - sine * k[1], oneMinusCosine * kyz + sine * k[0],
          1.0 - oneMinusCosine * (kxx + kyy)};
  // (I - R_s) * pivot written without the pivot, which is far away for small angles:
  // pivot = (t_perp + cot(angle/2) * (k x t_perp)) / 2.
  const double fullHalf = pair.screwAngle * 0.5;
  double cotSine = 2.0 * s;   // cot(full/2) * sin(s * full), limit for a vanishing angle
  double cotOneMinus = 0.0;   // cot(full/2) * (1 - cos(s * full))
  if (fullHalf > 1.0e-9) {
    const double cotangent = std::cos(fullHalf) / std::sin(fullHalf);
    cotSine = cotangent * sine;
    cotOneMinus = cotangent * oneMinusCosine;
  }
  const double perpendicularWeight = 0.5 * (oneMinusCosine + cotSine);
  const double crossWeight = 0.5 * (cotOneMinus - sine);
  const auto& tp = pair.screwPerpendicular;
  const std::array<double, 3> cross{k[1] * tp[2] - k[2] * tp[1], k[2] * tp[0] - k[0] * tp[2],
                                    k[0] * tp[1] - k[1] * tp[0]};
  std::array<double, 3> t{};
  for (int i = 0; i < 3; ++i) {
    t[i] = perpendicularWeight * tp[i] + crossWeight * cross[i] + s * pair.screwAlong[i];
  }
  const std::array<const Vec4<float>*, 3> previousRows{&pair.previous.m0, &pair.previous.m1,
                                                       &pair.previous.m2};
  const std::array<Vec4<float>*, 3> outputRows{&output.m0, &output.m1, &output.m2};
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 4; ++column) {
      double value = r[row][0] * (*previousRows[0])[column] + r[row][1] * (*previousRows[1])[column] +
                     r[row][2] * (*previousRows[2])[column];
      if (column == 3) {
        value += t[row];
      }
      (*outputRows[row])[column] = static_cast<float>(value);
    }
  }
}

PreparedAffinePair prepare_affine_pair(const Mat3x4<float>& previous,
                                       const Mat3x4<float>& current) noexcept {
  PreparedAffinePair pair{.previous = previous, .current = current};
  if (std::memcmp(&previous, &current, sizeof(current)) == 0) {
    pair.identical = true;
    pair.valid = true;
    return pair;
  }
  if (prepare_screw(pair)) {
    return pair;
  }

  std::array<std::array<float, 3>, 3> previousRotation{};
  std::array<std::array<float, 3>, 3> currentRotation{};
  if (!decompose_affine(previous, previousRotation, pair.previousScale,
                        pair.previousTranslation, pair.previousQuaternion) ||
      !decompose_affine(current, currentRotation, pair.currentScale,
                        pair.currentTranslation, pair.currentQuaternion)) {
    return pair;
  }

  float translationDeltaSquared = 0.0f;
  for (size_t i = 0; i < 3; ++i) {
    const float delta = pair.currentTranslation[i] - pair.previousTranslation[i];
    translationDeltaSquared += delta * delta;
  }
  if (!std::isfinite(translationDeltaSquared) ||
      translationDeltaSquared > kMaximumTranslationPerFrame * kMaximumTranslationPerFrame) {
    return pair;
  }

  float quaternionDot = pair.previousQuaternion.x * pair.currentQuaternion.x +
                        pair.previousQuaternion.y * pair.currentQuaternion.y +
                        pair.previousQuaternion.z * pair.currentQuaternion.z +
                        pair.previousQuaternion.w * pair.currentQuaternion.w;
  if (quaternionDot < 0.0f) {
    pair.currentQuaternion.x = -pair.currentQuaternion.x;
    pair.currentQuaternion.y = -pair.currentQuaternion.y;
    pair.currentQuaternion.z = -pair.currentQuaternion.z;
    pair.currentQuaternion.w = -pair.currentQuaternion.w;
    quaternionDot = -quaternionDot;
  }
  if (!std::isfinite(quaternionDot) || quaternionDot < kMinimumQuaternionDot) {
    return pair;
  }
  pair.rotationDot = quaternionDot;
  pair.translationDeltaSquared = translationDeltaSquared;
  pair.valid = true;
  return pair;
}

// Indexed matrices are final palette transforms, so interpolate coefficients instead
// of decomposing: ((1-t)M0 + tM1)v keeps a shared mesh boundary shared.
PreparedAffinePair prepare_indexed_pair(const Mat3x4<float>& previous,
                                        const Mat3x4<float>& current) noexcept {
  PreparedAffinePair pair{
      .previous = previous,
      .current = current,
      .linear = true,
  };
  if (std::memcmp(&previous, &current, sizeof(current)) == 0) {
    pair.identical = true;
    pair.valid = true;
    return pair;
  }

  const std::array<Vec4<float>, 3> previousRows{previous.m0, previous.m1, previous.m2};
  const std::array<Vec4<float>, 3> currentRows{current.m0, current.m1, current.m2};
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      if (!std::isfinite(previousRows[row][column]) ||
          !std::isfinite(currentRows[row][column])) {
        return pair;
      }
    }
  }

  const float dx = current.m0.w() - previous.m0.w();
  const float dy = current.m1.w() - previous.m1.w();
  const float dz = current.m2.w() - previous.m2.w();
  const float translationDeltaSquared = dx * dx + dy * dy + dz * dz;
  if (!(translationDeltaSquared <=
        kMaximumTranslationPerFrame * kMaximumTranslationPerFrame)) {
    return pair;
  }
  pair.translationDeltaSquared = translationDeltaSquared;
  // How far the bone turns in one frame, from its three axes, for the low-latency guard. Without it a
  // palette pair kept rotationDot at 1 and any bone was carried past the frame, however fast it turned:
  // in close cinematic shots (blinks, the suit pieces snapping onto Zero Suit Samus) the predicted pose
  // overshot and black holes flickered in the mesh (user report 2026-11-10, 120 Hz low latency). A
  // sudden change of an axis length (squash, a part popping in) is not predicted either.
  float smallestCosine = 1.0f;
  bool steadyScale = true;
  for (size_t column = 0; column < 3; ++column) {
    const float px = previous.m0[column], py = previous.m1[column], pz = previous.m2[column];
    const float cx = current.m0[column], cy = current.m1[column], cz = current.m2[column];
    const float previousLength = std::sqrt(px * px + py * py + pz * pz);
    const float currentLength = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (!(previousLength > 1.0e-6f) || !(currentLength > 1.0e-6f)) {
      steadyScale = false;
      continue;
    }
    const float ratio = currentLength / previousLength;
    if (ratio < 0.8f || ratio > 1.25f) {
      steadyScale = false;
    }
    smallestCosine = (std::min)(smallestCosine, (px * cx + py * cy + pz * cz) / (previousLength * currentLength));
  }
  // Same measure as a rigid pair's quaternion dot: cos(angle / 2).
  pair.rotationDot = steadyScale ? std::sqrt((std::max)(0.0f, (1.0f + smallestCosine) * 0.5f)) : 0.0f;
  pair.valid = true;
  return pair;
}

} // namespace

bool texture_matrix_moves_smoothly(const Mat3x4<float>& previous, const Mat3x4<float>& current) noexcept {
  const std::array<const Vec4<float>*, 3> before{&previous.m0, &previous.m1, &previous.m2};
  const std::array<const Vec4<float>*, 3> after{&current.m0, &current.m1, &current.m2};
  float linear = 0.0f;
  float translation = 0.0f;
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      const float a = (*before[row])[column];
      const float b = (*after[row])[column];
      if (!std::isfinite(a) || !std::isfinite(b)) {
        return false;
      }
      const float delta = std::abs(b - a);
      (column < 3 ? linear : translation) = (std::max)(column < 3 ? linear : translation, delta);
    }
  }
  if (linear == 0.0f && translation == 0.0f) {
    return false;  // nothing to interpolate
  }
  constexpr float kLargestLinearStep = 0.2f;        // about an 11 degree turn of a 0.5-scaled map per frame
  constexpr float kLargestFollowingShift = 0.25f;   // its offset moving with it
  constexpr float kLargestScroll = 0.02f;           // a pure offset change: a slow scroll, not a cell jump
  if (linear > kLargestLinearStep) {
    return false;
  }
  if (linear > 1.0e-6f) {
    return translation <= kLargestFollowingShift;
  }
  return translation <= kLargestScroll;
}

namespace {
// Past the frame (low-latency mode) the prediction holds only for small, steady motion: up to 30
// degrees and 30 units per frame. A larger step shows the frame itself instead of a guess. For a rigid
// pair both measures come from its relative motion, which every static piece shares with the camera,
// so the whole scene takes the same decision.
bool extrapolation_allowed(const PreparedAffinePair& pair) noexcept {
  constexpr float kMaximumExtrapolatedRotationDot = 0.96592583f;  // cos(15 degrees): half of 30
  constexpr float kMaximumExtrapolatedTranslation = 30.0f;
  return !(pair.rotationDot < kMaximumExtrapolatedRotationDot) &&
         pair.translationDeltaSquared <= kMaximumExtrapolatedTranslation * kMaximumExtrapolatedTranslation;
}

bool evaluate_affine_pair(const PreparedAffinePair& pair, float weight,
                          Mat3x4<float>& output) noexcept {
  if (!pair.valid || pair.identical) {
    output = pair.current;
    return pair.valid;
  }
  if (weight > 1.0f && !extrapolation_allowed(pair)) {
    output = pair.current;
    return true;
  }

  if (pair.screw) {
    evaluate_screw(pair, weight, output);
    return true;
  }

  if (pair.linear) {
    const std::array<const Vec4<float>*, 3> previousRows{
        &pair.previous.m0, &pair.previous.m1, &pair.previous.m2};
    const std::array<const Vec4<float>*, 3> currentRows{
        &pair.current.m0, &pair.current.m1, &pair.current.m2};
    const std::array<Vec4<float>*, 3> outputRows{&output.m0, &output.m1, &output.m2};
    for (size_t row = 0; row < 3; ++row) {
      for (size_t column = 0; column < 4; ++column) {
        const float before = (*previousRows[row])[column];
        (*outputRows[row])[column] =
            before + ((*currentRows[row])[column] - before) * weight;
      }
    }
    return true;
  }

  // Consecutive 60 Hz transforms stay in one hemisphere and within 90 degrees, so
  // normalized lerp is stable and skips three transcendentals per matrix.
  const float previousWeight = 1.0f - weight;
  const float currentWeight = weight;
  Quaternion interpolated{
      pair.previousQuaternion.x * previousWeight + pair.currentQuaternion.x * currentWeight,
      pair.previousQuaternion.y * previousWeight + pair.currentQuaternion.y * currentWeight,
      pair.previousQuaternion.z * previousWeight + pair.currentQuaternion.z * currentWeight,
      pair.previousQuaternion.w * previousWeight + pair.currentQuaternion.w * currentWeight,
  };
  const float quaternionLength = std::sqrt(interpolated.x * interpolated.x +
                                           interpolated.y * interpolated.y +
                                           interpolated.z * interpolated.z +
                                           interpolated.w * interpolated.w);
  if (!std::isfinite(quaternionLength) || quaternionLength < kMinimumScale) {
    output = pair.current;
    return false;
  }
  interpolated.x /= quaternionLength;
  interpolated.y /= quaternionLength;
  interpolated.z /= quaternionLength;
  interpolated.w /= quaternionLength;

  const auto interpolatedRotation = rotation_from_quaternion(interpolated);
  std::array<float, 3> interpolatedScale{};
  std::array<float, 3> interpolatedTranslation{};
  for (size_t i = 0; i < 3; ++i) {
    interpolatedScale[i] = pair.previousScale[i] +
                           (pair.currentScale[i] - pair.previousScale[i]) * weight;
    interpolatedTranslation[i] =
        pair.previousTranslation[i] +
        (pair.currentTranslation[i] - pair.previousTranslation[i]) * weight;
  }
  // Column scale mirrors decompose_affine: the reconstruction is R*S, with
  // each scale component applied down its column.
  output = {
      {interpolatedRotation[0][0] * interpolatedScale[0],
       interpolatedRotation[0][1] * interpolatedScale[1],
       interpolatedRotation[0][2] * interpolatedScale[2], interpolatedTranslation[0]},
      {interpolatedRotation[1][0] * interpolatedScale[0],
       interpolatedRotation[1][1] * interpolatedScale[1],
       interpolatedRotation[1][2] * interpolatedScale[2], interpolatedTranslation[1]},
      {interpolatedRotation[2][0] * interpolatedScale[0],
       interpolatedRotation[2][1] * interpolatedScale[1],
       interpolatedRotation[2][2] * interpolatedScale[2], interpolatedTranslation[2]},
  };
  return true;
}

bool interpolate_affine_impl(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                             float weight, Mat3x4<float>& output) noexcept {
  return evaluate_affine_pair(prepare_affine_pair(previous, current), weight, output);
}

Mat4x4<float> interpolate_projection(const Mat4x4<float>& previous, const Mat4x4<float>& current,
                                     float weight) noexcept {
  Mat4x4<float> result = current;
  for (size_t row = 0; row < 4; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      const float before = previous[row][column];
      const float after = current[row][column];
      if (std::isfinite(before) && std::isfinite(after)) {
        result[row][column] = before + (after - before) * weight;
      }
    }
  }
  return result;
}
} // namespace

bool interpolate_transform(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                           float weight, Mat3x4<float>& output) noexcept {
  return interpolate_affine_impl(previous, current, std::clamp(weight, 0.0f, 1.0f), output);
}

bool interpolate_transform_midpoint(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                                    Mat3x4<float>& output) noexcept {
  return interpolate_transform(previous, current, 0.5f, output);
}

bool interpolate_indexed_transform(const Mat3x4<float>& previous,
                                   const Mat3x4<float>& current, float weight,
                                   Mat3x4<float>& output) noexcept {
  return evaluate_affine_pair(prepare_indexed_pair(previous, current),
                              std::clamp(weight, 0.0f, 1.0f), output);
}

float transform_match_distance_squared(const Mat3x4<float>& previous,
                                       const Mat3x4<float>& current) noexcept {
  // Translation dominates in game units; the 3x3 part only breaks ties between
  // repeated meshes at the same origin.
  // Float throughout (the matcher measures millions of pairs on crowded screens): any non-finite
  // element, or an overflow, makes the sum non-finite.
  float distance = 0.0f;
  const std::array<Vec4<float>, 3> previousRows{previous.m0, previous.m1, previous.m2};
  const std::array<Vec4<float>, 3> currentRows{current.m0, current.m1, current.m2};
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      const float delta = currentRows[row][column] - previousRows[row][column];
      distance += delta * delta;
    }
  }
  return std::isfinite(distance) ? distance : std::numeric_limits<float>::infinity();
}

namespace {
// Translation-only delta, matching the kMaximumTranslationPerFrame gate. Non-finite
// inputs propagate NaN; callers compare with `<=` so NaN fails the gate.
float translation_delta_squared(const Mat3x4<float>& previous,
                                const Mat3x4<float>& current) noexcept {
  const float dx = current.m0.w() - previous.m0.w();
  const float dy = current.m1.w() - previous.m1.w();
  const float dz = current.m2.w() - previous.m2.w();
  return dx * dx + dy * dy + dz * dz;
}

// 2*current - previous. Not a valid rigid transform, but it is only a matching
// reference and its one-frame error stays far below instance spacing.
Mat3x4<float> extrapolate_transform(const Mat3x4<float>& previous,
                                    const Mat3x4<float>& current) noexcept {
  Mat3x4<float> predicted{};
  const std::array<const Vec4<float>*, 3> previousRows{&previous.m0, &previous.m1, &previous.m2};
  const std::array<const Vec4<float>*, 3> currentRows{&current.m0, &current.m1, &current.m2};
  const std::array<Vec4<float>*, 3> predictedRows{&predicted.m0, &predicted.m1, &predicted.m2};
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      (*predictedRows[row])[column] =
          2.0f * (*currentRows[row])[column] - (*previousRows[row])[column];
    }
  }
  return predicted;
}
} // namespace

float snapshot_match_distance_squared(const FrameTransformEntry& previousEntry,
                                      const FrameTransformSnapshot& current) noexcept {
  const FrameTransformSnapshot& previous = previousEntry.transform;
  if (static_cast<bool>(previous.indexedMatrices) != static_cast<bool>(current.indexedMatrices)) {
    return std::numeric_limits<float>::infinity();
  }
  if (!current.indexedMatrices) {
    // Match against the predicted position, falling back to the last known one for
    // entries with no motion history.
    const Mat3x4<float>& reference =
        previousEntry.hasPrediction ? previousEntry.predictedPosition : previous.position;
    return transform_match_distance_squared(reference, current.position);
  }

  if (previous.usedMatrixMask == 0 || current.usedMatrixMask == 0) {
    return std::numeric_limits<float>::infinity();
  }

  // PNMTXIDX is an absolute palette slot baked into the vertices, so nearest matching
  // swaps joint histories. A changed mask has no safe correspondence: leave the draw.
  if (previous.usedMatrixMask != current.usedMatrixMask) {
    return std::numeric_limits<float>::infinity();
  }

  double distance = 0.0;
  size_t matchedMatrices = 0;
  for (size_t currentIndex = 0; currentIndex < MaxPnMtx; ++currentIndex) {
    if ((current.usedMatrixMask & (1u << currentIndex)) == 0) {
      continue;
    }
    const float matrixDistance = transform_match_distance_squared(
        previous.indexedMatrices->position[currentIndex],
        current.indexedMatrices->position[currentIndex]);
    if (!std::isfinite(matrixDistance)) {
      return std::numeric_limits<float>::infinity();
    }
    distance += matrixDistance;
    ++matchedMatrices;
  }
  if (matchedMatrices == 0 || !std::isfinite(distance) ||
      distance > std::numeric_limits<float>::max()) {
    return std::numeric_limits<float>::infinity();
  }
  return static_cast<float>(distance / static_cast<double>(matchedMatrices));
}

namespace {
std::unique_ptr<InterpolationFrame> acquire_frame() {
  if (!s_framePool.empty()) {
    auto frame = std::move(s_framePool.back());
    s_framePool.pop_back();
    return frame;
  }
  return std::make_unique<InterpolationFrame>();
}

void recycle_frame(std::unique_ptr<InterpolationFrame> frame) noexcept {
  if (!frame) {
    return;
  }
  const size_t live = frame->transforms.size();
  clear_index_map_keep_nodes(frame->exactIndices, live);
  clear_index_map_keep_nodes(frame->stableIndices, live);
  clear_index_map_keep_nodes(frame->geometryIndices, live);
  recycle_transform_entries(frame->transforms);
  frame->textureMatrices.clear();
  frame->toPrevious.clear();
  frame->borrowedPrevious.clear();
  frame->cameraDeltaValid = false;
  frame->claimed.clear();
  frame->matchedCount = 0;
  frame->hasIndexedMatrices = false;
  frame->complete = false;
  frame->extrapolated = false;
  frame->cadence = {};
  frame->bias = 1.0;
  frame->candidates = 0;
  frame->matchable = 0;
  frame->matches = 0;
  frame->motionPairs = 0;
  frame->cutPairs = 0;
  if (s_framePool.size() < 4) {
    s_framePool.push_back(std::move(frame));
  }
}

void reset_frames() noexcept {
  s_pendingUniformInterpolations.clear();
  while (!s_frames.empty()) {
    recycle_frame(std::move(s_frames.front()));
    s_frames.pop_front();
  }
  s_completedInBatch = nullptr;
  s_boundariesInBatch = 0;
}

void latch_cadence(InterpolationFrame& frame) noexcept {
  const double imagesPerFrame =
      std::min(s_imagesPerFrameTarget.load(std::memory_order_acquire), maximum_images_per_frame());
  if (imagesPerFrame != s_cadenceBiasRate) {
    s_cadenceBiasRate = imagesPerFrame;
    s_cadenceBias = policy::cadence_bias(imagesPerFrame);
    s_cadenceNextTick = 0.0;
  }
  frame.cadence = policy::next_frame_cadence(imagesPerFrame, s_cadenceNextTick);
  frame.bias = s_cadenceBias;
  frame.extrapolated = s_lowLatency.load(std::memory_order_acquire) && frame.cadence.samples != 0;
  if (frame.extrapolated) {
    // The same display ticks, one frame later: the frame itself goes out first, then each tick shows
    // it carried forward along its last motion.
    for (uint32_t sample = 0; sample < frame.cadence.samples; ++sample) {
      frame.cadence.weights[sample] += 1.f;
    }
    frame.cadence.presentNative = true;
  }
  // Private QA: inserted images at weight 1 must equal the native image pixel for pixel (see
  // WII_COMPILED_QA_INTERP_COMPARE_EVERY in aurora.cpp).
  if (AURORA_DIAGNOSTIC_ENV("WII_COMPILED_QA_INTERP_WEIGHT_ONE") != nullptr) {
    for (uint32_t sample = 0; sample < frame.cadence.samples; ++sample) {
      frame.cadence.weights[sample] = 1.f;
    }
  }
  s_activeInterpolationSamples.store(frame.cadence.samples, std::memory_order_release);
}

InterpolationFrame& recording_frame() noexcept {
  if (s_frames.empty()) {
    auto frame = acquire_frame();
    latch_cadence(*frame);
    s_frames.push_back(std::move(frame));
  }
  return *s_frames.back();
}

// The complete frame the recording one interpolates from; null for the first frame.
InterpolationFrame* recording_previous_frame() noexcept {
  return s_frames.size() >= 2 ? s_frames[s_frames.size() - 2].get() : nullptr;
}

// How far the camera moved between two draws of one static object: their modelviews are V*W, so
// current * inverse(previous) is the camera's own motion in view space. False for a singular matrix.
bool camera_motion(const Mat3x4<float>& previous, const Mat3x4<float>& current, float& angleDegrees,
                   float& distance) noexcept {
  const std::array<Vec4<float>, 3> p{previous.m0, previous.m1, previous.m2};
  const std::array<Vec4<float>, 3> c{current.m0, current.m1, current.m2};
  double a[3][3];
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) {
      a[r][k] = p[r][k];
    }
  }
  const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                     a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                     a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (!std::isfinite(det) || std::abs(det) < 1.0e-12) {
    return false;
  }
  double inv[3][3];
  inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
  inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
  inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
  inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
  inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
  inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
  inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
  inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
  inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
  double d[3][3];
  double trace = 0.0;
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) {
      d[r][k] = c[r][0] * inv[0][k] + c[r][1] * inv[1][k] + c[r][2] * inv[2][k];
    }
    trace += d[r][r];
  }
  double moved = 0.0;
  for (int r = 0; r < 3; ++r) {
    const double t = c[r][3] - (d[r][0] * p[0][3] + d[r][1] * p[1][3] + d[r][2] * p[2][3]);
    moved += t * t;
  }
  const double cosine = std::clamp((trace - 1.0) * 0.5, -1.0, 1.0);
  angleDegrees = static_cast<float>(std::acos(cosine) * 57.29577951308232);
  distance = static_cast<float>(std::sqrt(moved));
  return std::isfinite(angleDegrees) && std::isfinite(distance);
}

// A cut between two shots: most static draws report a camera jump no game camera makes in 1/60 s
// (Prime units are metres). Interpolating it would show a made-up shot halfway between the two.
constexpr float kCutAngleDegrees = 35.f;
constexpr float kCutDistance = 5.f;
constexpr uint32_t kCutMinimumPairs = 16;
bool frame_is_camera_cut(const InterpolationFrame& frame) noexcept {
  return frame.motionPairs >= kCutMinimumPairs && frame.cutPairs * 5u > frame.motionPairs * 2u;
}

// The camera's motion in view space between `prev` and `cur`: current * inverse(previous) of the draws
// whose exact identity occurs once in both frames, taking the value most of them agree on (static
// scenery; moving actors disagree). Kept for the frame once found, so its later segment reuses it.
void estimate_camera_delta(InterpolationFrame& cur, const InterpolationFrame& prev, size_t begin) noexcept {
  if (cur.cameraDeltaValid) {
    return;
  }
  struct Candidate {
    std::array<std::array<double, 3>, 3> rotation{};
    std::array<double, 3> translation{};
  };
  static std::vector<Candidate> candidates;
  candidates.clear();
  constexpr size_t kMaximumCandidates = 256;
  for (size_t i = begin; i < cur.transforms.size() && candidates.size() < kMaximumCandidates; ++i) {
    const auto& entry = cur.transforms[i];
    if (entry.transform.indexedMatrices) {
      continue;
    }
    const auto currentGroup = cur.exactIndices.find(entry.identity.combined);
    if (currentGroup == cur.exactIndices.end() || currentGroup->second.size() != 1) {
      continue;
    }
    const auto previousGroup = prev.exactIndices.find(entry.identity.combined);
    if (previousGroup == prev.exactIndices.end() || previousGroup->second.size() != 1) {
      continue;
    }
    const auto& previous = prev.transforms[previousGroup->second.front()].transform;
    if (previous.indexedMatrices) {
      continue;
    }
    Candidate candidate;
    if (relative_rigid_motion(previous.position, entry.transform.position, candidate.rotation,
                              candidate.translation)) {
      candidates.push_back(candidate);
    }
  }
  constexpr size_t kMinimumSupport = 4;
  if (candidates.size() < kMinimumSupport) {
    return;
  }
  const auto agrees = [](const Candidate& a, const Candidate& b) noexcept {
    for (int r = 0; r < 3; ++r) {
      for (int k = 0; k < 3; ++k) {
        if (!(std::abs(a.rotation[r][k] - b.rotation[r][k]) <= 2.0e-3)) {
          return false;
        }
      }
      if (!(std::abs(a.translation[r] - b.translation[r]) <= 0.02 + 1.0e-3 * std::abs(a.translation[r]))) {
        return false;
      }
    }
    return true;
  };
  size_t best = 0;
  size_t bestSupport = 0;
  const size_t probes = std::min<size_t>(candidates.size(), 24);
  for (size_t probe = 0; probe < probes; ++probe) {
    const size_t index = probe * candidates.size() / probes;
    size_t support = 0;
    for (const auto& candidate : candidates) {
      support += agrees(candidates[index], candidate) ? 1 : 0;
    }
    if (support > bestSupport) {
      bestSupport = support;
      best = index;
    }
  }
  if (bestSupport < kMinimumSupport || bestSupport * 4 < candidates.size()) {
    return;
  }
  const auto& chosen = candidates[best];
  std::array<Vec4<float>*, 3> rows{&cur.cameraDelta.m0, &cur.cameraDelta.m1, &cur.cameraDelta.m2};
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) {
      (*rows[r])[k] = static_cast<float>(chosen.rotation[r][k]);
    }
    (*rows[r])[3] = static_cast<float>(chosen.translation[r]);
  }
  cur.cameraDeltaValid = true;
  s_diagCameraEstimates.fetch_add(1, std::memory_order_relaxed);
}

// How far a one-matrix draw may be from a partner in one frame (view-space units; Prime units are metres,
// so 150 m/s). Measured from the nearest of three references: where the partner was on screen (attached
// to the camera), where the camera's motion takes it (static in the world) and where its own last motion
// takes it. Fire and spark particles appear and vanish every frame; without this a leftover sprite paired
// with one of another flame metres away and the inserted image drew it in between, a flickering clone.
// Such a draw is better shown where it is.
constexpr float kMaximumObjectStep = 2.5f;
using ReachPoint = std::array<float, 3>;

// The translations a one-matrix partner may be measured from (see above); within_reach and the
// matching grid both use these, so the grid never hides a pair the gate would accept.
size_t reach_references(const InterpolationFrame& cur, const FrameTransformEntry& previousEntry,
                        std::array<ReachPoint, 3>& out) noexcept {
  const auto& previous = previousEntry.transform.position;
  size_t count = 0;
  out[count++] = {previous.m0.w(), previous.m1.w(), previous.m2.w()};
  if (previousEntry.hasPrediction) {
    const auto& predicted = previousEntry.predictedPosition;
    out[count++] = {predicted.m0.w(), predicted.m1.w(), predicted.m2.w()};
  }
  if (cur.cameraDeltaValid) {
    const auto& d = cur.cameraDelta;
    const float px = previous.m0.w();
    const float py = previous.m1.w();
    const float pz = previous.m2.w();
    out[count++] = {d.m0.x() * px + d.m0.y() * py + d.m0.z() * pz + d.m0.w(),
                    d.m1.x() * px + d.m1.y() * py + d.m1.z() * pz + d.m1.w(),
                    d.m2.x() * px + d.m2.y() * py + d.m2.z() * pz + d.m2.w()};
  }
  return count;
}

bool references_reach(const std::array<ReachPoint, 3>& references, size_t count,
                      const FrameTransformSnapshot& current) noexcept {
  constexpr float limit = kMaximumObjectStep * kMaximumObjectStep;
  const float cx = current.position.m0.w();
  const float cy = current.position.m1.w();
  const float cz = current.position.m2.w();
  for (size_t i = 0; i < count; ++i) {
    const float x = references[i][0] - cx;
    const float y = references[i][1] - cy;
    const float z = references[i][2] - cz;
    if (x * x + y * y + z * z <= limit) {  // `<=` so NaN fails
      return true;
    }
  }
  return false;
}

bool within_reach(const InterpolationFrame& cur, const FrameTransformEntry& previousEntry,
                  const FrameTransformSnapshot& current) noexcept {
  if (previousEntry.transform.indexedMatrices || current.indexedMatrices) {
    return true;
  }
  std::array<ReachPoint, 3> references;
  const size_t count = reach_references(cur, previousEntry, references);
  return references_reach(references, count, current);
}

// Matches the transforms [begin, size) of `cur` against `prev` and fills the uniform copies of the given
// tasks. Earlier transforms of `cur` were matched at a previous seal and keep their partners, which stay
// claimed in `prev`, so a frame split across two batches is matched as one.
void process_segment(InterpolationFrame& cur, InterpolationFrame& prev, size_t begin,
                     const std::vector<size_t>& taskIndices) noexcept {
  const auto& currentTransforms = cur.transforms;
  const auto& previousTransforms = prev.transforms;
  // Below this bound a direct all-pairs build is cheaper than setting up the
  // spatial grid; the produced edge set is identical either way.
  constexpr size_t kAllPairsEdgeLimit = 1024;
  // Bounds the all-pairs cost for indexed groups, which have no single translation to
  // bucket on. Non-indexed groups of any size use the grid instead.
  constexpr size_t kMaximumAssignmentEdges = 16384;
  // Hard bound on the pairs the grid path may evaluate, checked before evaluating any: only a swarm
  // packed within reach of itself gets there (CPU-deformed shapes, the pause-screen map's thousands of
  // lookalike pieces), and the ordered fallback pairs those in submission order instead.
  constexpr size_t kMaximumGridEdges = 4096;
  // A spread-out crowd offers each draw a handful of neighbors; allow that much on top for big groups.
  constexpr size_t kGridEdgesPerDraw = 4;
  // The same bound for the whole segment, so many medium groups packed within reach of each other
  // (the pause-screen map while it turns or zooms) cannot add up to a quadratic cost either. A group
  // that no longer fits is paired in submission order, inside its own identity: the same mesh drawn in
  // the same order is the same piece. At 120 FPS the map's ~5000 draws left the frame worker ~80 % busy
  // (2026-11-07 F8) and a fifth of that was measuring pairs, so the bound is kept tight.
  const size_t segmentPairBudget =
      kMaximumGridEdges + kGridEdgesPerDraw * (currentTransforms.size() - std::min(begin, currentTransforms.size()));
  size_t segmentPairs = 0;
  const auto fits_budget = [&](size_t pairs) noexcept { return segmentPairs + pairs <= segmentPairBudget; };

  // 12 bytes, sorted on the bit pattern of a finite non-negative distance (monotonic as an unsigned
  // integer): a group's edges sort in half the time of the former 24-byte float-keyed records.
  struct MatchEdge {
    uint32_t distanceBits = 0;
    uint32_t previous = 0;
    uint32_t current = 0;
  };

  // Frame-persistent matching scratch: sized to the frame's draw count, so a
  // stable scene performs no matching allocations at all after warm-up.
  static std::vector<uint8_t> currentMatched;
  static std::vector<size_t> groupCurrentIndices;
  static std::vector<size_t> groupPreviousIndices;
  static std::vector<MatchEdge> edges;
  // (cell, previous index) for each reach reference, sorted by cell: a flat grid with no per-cell
  // allocations. `identical` is (matrix hash, previous index), also sorted.
  static std::vector<std::pair<uint64_t, uint32_t>> gridEntries;
  // Cell -> [first, last) in gridEntries, so a lookup is one probe instead of a binary search.
  static absl::flat_hash_map<uint64_t, std::pair<uint32_t, uint32_t>> cellRanges;
  static std::vector<std::pair<uint64_t, uint32_t>> identical;
  // Per previous entry: the last visit that already offered it (one current draw can reach it
  // through several cells and references).
  static std::vector<uint32_t> visitStamp;
  static uint32_t visitCounter = 0;
  auto& currentToPrevious = cur.toPrevious;
  auto& previousMatched = prev.claimed;
  currentToPrevious.resize(currentTransforms.size(), SIZE_MAX);
  for (size_t i = begin; i < currentTransforms.size(); ++i) {
    currentToPrevious[i] = SIZE_MAX;
  }
  previousMatched.resize(previousTransforms.size(), 0);
  // Transforms finalized at an earlier seal are out of reach: their copies are already on the GPU.
  currentMatched.assign(currentTransforms.size(), 0);
  std::fill(currentMatched.begin(), currentMatched.begin() + static_cast<std::ptrdiff_t>(begin), uint8_t{1});
  estimate_camera_delta(cur, prev, begin);

  // The matching grid. A cell is a little over twice the reach, so every point within reach of a
  // current draw lies in the 2x2x2 block of cells nearest to it (the margin absorbs float rounding at
  // cell edges). Previous entries are filed under each of their reach references.
  constexpr float kGridCell = 2.0f * kMaximumObjectStep * 1.01f;
  constexpr float kInverseGridCell = 1.0f / kGridCell;
  constexpr int64_t kCellBias = int64_t{1} << 20;
  constexpr uint64_t kInvalidCell = std::numeric_limits<uint64_t>::max();
  const auto pack_cell = [](int64_t cx, int64_t cy, int64_t cz) noexcept -> uint64_t {
    // One cell of margin at each field edge so the neighbor cells cannot borrow into the next
    // packed coordinate.
    cx += kCellBias;
    cy += kCellBias;
    cz += kCellBias;
    if (cx < 1 || cy < 1 || cz < 1 || cx >= (kCellBias << 1) - 1 || cy >= (kCellBias << 1) - 1 ||
        cz >= (kCellBias << 1) - 1) {
      return kInvalidCell;
    }
    return (static_cast<uint64_t>(cx) << 42) | (static_cast<uint64_t>(cy) << 21) |
           static_cast<uint64_t>(cz);
  };
  const auto point_cell = [&](const ReachPoint& p) noexcept -> uint64_t {
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
      return kInvalidCell;
    }
    return pack_cell(static_cast<int64_t>(std::floor(p[0] * kInverseGridCell)),
                     static_cast<int64_t>(std::floor(p[1] * kInverseGridCell)),
                     static_cast<int64_t>(std::floor(p[2] * kInverseGridCell)));
  };
  // Calls visit(first, last) for the grid entries of the 2x2x2 cells around a current draw.
  const auto for_each_nearby_cell = [&](const Mat3x4<float>& position, auto&& visit) {
    const ReachPoint p{position.m0.w(), position.m1.w(), position.m2.w()};
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
      return;
    }
    std::array<int64_t, 3> base{};
    std::array<int64_t, 3> side{};
    for (size_t axis = 0; axis < 3; ++axis) {
      const float scaled = p[axis] * kInverseGridCell;
      const float cell = std::floor(scaled);
      base[axis] = static_cast<int64_t>(cell);
      side[axis] = scaled - cell < 0.5f ? -1 : 1;
    }
    for (int corner = 0; corner < 8; ++corner) {
      const uint64_t key = pack_cell(base[0] + ((corner & 1) ? side[0] : 0),
                                     base[1] + ((corner & 2) ? side[1] : 0),
                                     base[2] + ((corner & 4) ? side[2] : 0));
      if (key == kInvalidCell) {
        continue;
      }
      const auto range = cellRanges.find(key);
      if (range != cellRanges.end()) {
        visit(gridEntries.begin() + range->second.first, gridEntries.begin() + range->second.second);
      }
    }
  };
  const auto matrix_hash = [](const Mat3x4<float>& m) noexcept {
    return static_cast<uint64_t>(xxh3_hash_s(&m, sizeof(m), 0));
  };

  // Reach references of each previous entry, computed once per segment rather than once per pair
  // (the camera reference costs a matrix product). 0 = not computed yet, otherwise count + 1.
  static std::vector<std::array<ReachPoint, 3>> reachCache;
  static std::vector<uint8_t> reachCount;
  if (reachCache.size() < previousTransforms.size()) {
    reachCache.resize(previousTransforms.size());
  }
  reachCount.assign(previousTransforms.size(), 0);
  const auto cached_references = [&](size_t previousIndex) noexcept -> size_t {
    uint8_t& stored = reachCount[previousIndex];
    if (stored == 0) {
      stored = static_cast<uint8_t>(
          reach_references(cur, previousTransforms[previousIndex], reachCache[previousIndex]) + 1);
    }
    return stored - 1u;
  };
  const auto reaches = [&](size_t previousIndex, const FrameTransformSnapshot& current) noexcept {
    if (previousTransforms[previousIndex].transform.indexedMatrices || current.indexedMatrices) {
      return true;
    }
    return references_reach(reachCache[previousIndex], cached_references(previousIndex), current);
  };

  const auto matchGroups =
      [&](const auto& currentGroups, const auto& previousGroups,
          bool allowOrderedFallback, bool trustUniqueExact) {
    for (const auto& [signature, allCurrentIndices] : currentGroups) {
      if (allCurrentIndices.empty()) {
        continue;
      }
      // Leftovers first: after the exact pass nearly every draw is paired, and the lookup below is a
      // cache miss per group.
      groupCurrentIndices.clear();
      for (const size_t index : allCurrentIndices) {
        if (!currentMatched[index]) {
          groupCurrentIndices.push_back(index);
        }
      }
      if (groupCurrentIndices.empty()) {
        continue;
      }
      const auto previousIt = previousGroups.find(signature);
      if (previousIt == previousGroups.end() || previousIt->second.empty()) {
        continue;
      }
      groupPreviousIndices.clear();
      for (const size_t index : previousIt->second) {
        if (!previousMatched[index]) {
          groupPreviousIndices.push_back(index);
        }
      }
      if (groupCurrentIndices.empty() || groupPreviousIndices.empty()) {
        continue;
      }

      // A unique draw has no identity ambiguity. Keep the conservative
      // interpolation fallback for malformed/non-finite matrices.
      if (groupPreviousIndices.size() == 1 && groupCurrentIndices.size() == 1) {
        // The one draw of a mesh and material in both frames is the same object; without a camera
        // estimate to measure it against, trust that. Otherwise it must be within reach like any pair.
        if (!(trustUniqueExact && !cur.cameraDeltaValid) &&
            !reaches(groupPreviousIndices.front(), currentTransforms[groupCurrentIndices.front()].transform)) {
          continue;
        }
        currentToPrevious[groupCurrentIndices.front()] = groupPreviousIndices.front();
        previousMatched[groupPreviousIndices.front()] = 1;
        currentMatched[groupCurrentIndices.front()] = 1;
        continue;
      }

      // Drop pairs past the translation gate (`<=` also drops a NaN delta): they can only
      // render as a snap, and matching them steals a neighbor's true partner.
      const auto appendEdge = [&](size_t previousIndex, size_t currentIndex) {
        s_pairEvaluations.fetch_add(1, std::memory_order_relaxed);
        ++segmentPairs;
        const auto& previousEntry = previousTransforms[previousIndex];
        const auto& currentSnapshot = currentTransforms[currentIndex].transform;
        if (!reaches(previousIndex, currentSnapshot)) {
          return;
        }
        const float distance = snapshot_match_distance_squared(previousEntry, currentSnapshot);
        if (std::isfinite(distance)) {
          uint32_t distanceBits = 0;
          std::memcpy(&distanceBits, &distance, sizeof(distanceBits));
          edges.push_back({
              .distanceBits = distanceBits,
              .previous = static_cast<uint32_t>(previousIndex),
              .current = static_cast<uint32_t>(currentIndex),
          });
        }
      };

      // `outOfBudget`: the group is paired in order because measuring it would cost too much, in any
      // pass. Otherwise only the stable pass pairs leftovers in order.
      const auto orderedFallback = [&](bool outOfBudget = false) {
        // GX preserves submission order for these draws, so pair the leftovers in order
        // once exact identities have claimed their instances.
        if (!allowOrderedFallback && !outOfBudget) {
          return;
        }
        const size_t pairCount = std::min(groupPreviousIndices.size(), groupCurrentIndices.size());
        for (size_t i = 0; i < pairCount; ++i) {
          // Submission order shifts when the transparent sort reorders or culling drops an
          // instance; leave over-gate pairs unmatched so they snap instead of sweeping.
          const auto& currentEntry = currentTransforms[groupCurrentIndices[i]];
          if (!reaches(groupPreviousIndices[i], currentEntry.transform)) {
            continue;
          }
          currentToPrevious[groupCurrentIndices[i]] = groupPreviousIndices[i];
          previousMatched[groupPreviousIndices[i]] = 1;
          currentMatched[groupCurrentIndices[i]] = 1;
        }
      };

      // Exact repeats and grid buckets need one translation per entry, which mixed or skinned groups lack.
      bool gridable = true;
      for (const size_t index : groupCurrentIndices) {
        if (currentTransforms[index].transform.indexedMatrices) {
          gridable = false;
          break;
        }
      }
      if (gridable) {
        for (const size_t index : groupPreviousIndices) {
          if (previousTransforms[index].transform.indexedMatrices) {
            gridable = false;
            break;
          }
        }
      }
      if (gridable) {
        // A piece that kept its matrix sits exactly where it was predicted: distance zero, the pair the
        // greedy assignment below would take first anyway. Pairing those keeps a still crowd linear (the
        // pause-screen map: ~5000 lookalike pieces in groups of all sizes, packed within reach). Submission
        // order usually holds, so compare in order first; hash only what that leaves, and only when
        // something was still (a moving crowd has no identical pair and skips the hashing entirely).
        const auto reference_of = [&](size_t previousIndex) -> const Mat3x4<float>& {
          const auto& previousEntry = previousTransforms[previousIndex];
          return previousEntry.hasPrediction ? previousEntry.predictedPosition : previousEntry.transform.position;
        };
        const auto drop_claimed = [&] {
          const auto claimedCurrent = [&](size_t index) { return currentMatched[index] != 0; };
          const auto claimedPrevious = [&](size_t index) { return previousMatched[index] != 0; };
          groupCurrentIndices.erase(
              std::remove_if(groupCurrentIndices.begin(), groupCurrentIndices.end(), claimedCurrent),
              groupCurrentIndices.end());
          groupPreviousIndices.erase(
              std::remove_if(groupPreviousIndices.begin(), groupPreviousIndices.end(), claimedPrevious),
              groupPreviousIndices.end());
        };
        size_t pairedInOrder = 0;
        const size_t orderedCount = std::min(groupPreviousIndices.size(), groupCurrentIndices.size());
        for (size_t i = 0; i < orderedCount; ++i) {
          const size_t previousIndex = groupPreviousIndices[i];
          const size_t currentIndex = groupCurrentIndices[i];
          const auto& snapshot = currentTransforms[currentIndex].transform;
          if (std::memcmp(&reference_of(previousIndex), &snapshot.position, sizeof(snapshot.position)) != 0 ||
              !reaches(previousIndex, snapshot)) {
            continue;
          }
          currentToPrevious[currentIndex] = previousIndex;
          previousMatched[previousIndex] = 1;
          currentMatched[currentIndex] = 1;
          ++pairedInOrder;
        }
        if (pairedInOrder != 0) {
          drop_claimed();
          if (groupCurrentIndices.empty() || groupPreviousIndices.empty()) {
            continue;
          }
        }
        identical.clear();
        const bool hashLeftovers = pairedInOrder != 0 || groupCurrentIndices.size() * groupPreviousIndices.size() <= 64;
        for (const size_t previousIndex : groupPreviousIndices) {
          if (!hashLeftovers) {
            break;
          }
          identical.emplace_back(matrix_hash(reference_of(previousIndex)), static_cast<uint32_t>(previousIndex));
        }
        std::sort(identical.begin(), identical.end());
        size_t pairedIdentical = 0;
        for (const size_t currentIndex : groupCurrentIndices) {
          if (identical.empty()) {
            break;
          }
          const auto& position = currentTransforms[currentIndex].transform.position;
          const uint64_t hash = matrix_hash(position);
          for (auto it = std::lower_bound(identical.begin(), identical.end(),
                                          std::pair<uint64_t, uint32_t>{hash, 0});
               it != identical.end() && it->first == hash; ++it) {
            if (previousMatched[it->second] ||
                std::memcmp(&reference_of(it->second), &position, sizeof(position)) != 0 ||
                !reaches(it->second, currentTransforms[currentIndex].transform)) {
              continue;
            }
            currentToPrevious[currentIndex] = it->second;
            previousMatched[it->second] = 1;
            currentMatched[currentIndex] = 1;
            ++pairedIdentical;
            break;
          }
        }
        if (pairedIdentical != 0) {
          drop_claimed();
          if (groupCurrentIndices.empty() || groupPreviousIndices.empty()) {
            continue;
          }
        }
      }

      edges.clear();
      const size_t allPairsEdges = groupPreviousIndices.size() * groupCurrentIndices.size();
      if (allPairsEdges <= kAllPairsEdgeLimit) {
        if (!fits_budget(allPairsEdges)) {
          orderedFallback(true);
          continue;
        }
        for (const size_t currentIndex : groupCurrentIndices) {
          for (const size_t previousIndex : groupPreviousIndices) {
            appendEdge(previousIndex, currentIndex);
          }
        }
      } else {
        if (!gridable) {
          if (groupPreviousIndices.size() > kMaximumAssignmentEdges / groupCurrentIndices.size() ||
              !fits_budget(allPairsEdges)) {
            orderedFallback(!fits_budget(allPairsEdges));
            continue;
          }
          for (const size_t currentIndex : groupCurrentIndices) {
            for (const size_t previousIndex : groupPreviousIndices) {
              appendEdge(previousIndex, currentIndex);
            }
          }
        } else {
          // Estimate the pairs the grid would offer from the group's extent, as if spread evenly: clustering
          // only adds pairs, so an estimate far past the budget means building and sorting the grid would be
          // wasted. A packed crowd goes straight to submission order.
          {
            std::array<float, 3> low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                     std::numeric_limits<float>::max()};
            std::array<float, 3> high{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                                      std::numeric_limits<float>::lowest()};
            for (const size_t currentIndex : groupCurrentIndices) {
              const auto& position = currentTransforms[currentIndex].transform.position;
              const std::array<float, 3> p{position.m0.w(), position.m1.w(), position.m2.w()};
              for (size_t axis = 0; axis < 3; ++axis) {
                if (std::isfinite(p[axis])) {
                  low[axis] = std::min(low[axis], p[axis]);
                  high[axis] = std::max(high[axis], p[axis]);
                }
              }
            }
            double cells = 1.0;
            for (size_t axis = 0; axis < 3; ++axis) {
              const double extent = high[axis] >= low[axis] ? double{high[axis]} - double{low[axis]} : 0.0;
              cells *= std::floor(extent / kGridCell) + 1.0;
            }
            const double share = std::min(1.0, 8.0 / cells);
            const double estimate = share * static_cast<double>(groupCurrentIndices.size()) *
                                    static_cast<double>(groupPreviousIndices.size());
            const double budget = static_cast<double>(
                std::min(kMaximumGridEdges + kGridEdgesPerDraw * groupCurrentIndices.size(),
                         segmentPairBudget - std::min(segmentPairs, segmentPairBudget)));
            if (estimate > 4.0 * budget) {
              orderedFallback(true);
              continue;
            }
          }
          gridEntries.clear();
          for (const size_t previousIndex : groupPreviousIndices) {
            const size_t count = cached_references(previousIndex);
            const auto& references = reachCache[previousIndex];
            std::array<uint64_t, 3> filed{};
            size_t filedCount = 0;
            for (size_t i = 0; i < count; ++i) {
              const uint64_t cell = point_cell(references[i]);
              if (cell == kInvalidCell ||
                  std::find(filed.begin(), filed.begin() + static_cast<std::ptrdiff_t>(filedCount), cell) !=
                      filed.begin() + static_cast<std::ptrdiff_t>(filedCount)) {
                continue;
              }
              filed[filedCount++] = cell;
              gridEntries.emplace_back(cell, static_cast<uint32_t>(previousIndex));
            }
          }
          std::sort(gridEntries.begin(), gridEntries.end());
          cellRanges.clear();
          for (uint32_t first = 0; first < gridEntries.size();) {
            uint32_t last = first + 1;
            while (last < gridEntries.size() && gridEntries[last].first == gridEntries[first].first) {
              ++last;
            }
            cellRanges.emplace(gridEntries[first].first, std::pair<uint32_t, uint32_t>{first, last});
            first = last;
          }

          // Count the work before doing any of it.
          const size_t pairBudget = kMaximumGridEdges + kGridEdgesPerDraw * groupCurrentIndices.size();
          size_t candidatePairs = 0;
          for (const size_t currentIndex : groupCurrentIndices) {
            for_each_nearby_cell(currentTransforms[currentIndex].transform.position,
                                 [&](auto first, auto last) {
                                   candidatePairs += static_cast<size_t>(last - first);
                                 });
            if (candidatePairs > pairBudget) {
              break;
            }
          }
          if (candidatePairs > pairBudget || !fits_budget(candidatePairs)) {
            orderedFallback(true);
            continue;
          }

          if (visitStamp.size() < previousTransforms.size()) {
            visitStamp.resize(previousTransforms.size(), 0);
          }
          for (const size_t currentIndex : groupCurrentIndices) {
            if (++visitCounter == 0) {
              std::fill(visitStamp.begin(), visitStamp.end(), 0u);
              visitCounter = 1;
            }
            for_each_nearby_cell(currentTransforms[currentIndex].transform.position,
                                 [&](auto first, auto last) {
                                   for (auto it = first; it != last; ++it) {
                                     if (visitStamp[it->second] == visitCounter) {
                                       continue;
                                     }
                                     visitStamp[it->second] = visitCounter;
                                     appendEdge(it->second, currentIndex);
                                   }
                                 });
          }
        }
      }

      std::sort(edges.begin(), edges.end(), [](const MatchEdge& lhs, const MatchEdge& rhs) {
        return lhs.distanceBits < rhs.distanceBits;
      });
      for (const auto& edge : edges) {
        if (previousMatched[edge.previous] || currentMatched[edge.current]) {
          continue;
        }
        currentToPrevious[edge.current] = edge.previous;
        previousMatched[edge.previous] = 1;
        currentMatched[edge.current] = 1;
      }
    }
  };
  matchGroups(cur.exactIndices, prev.exactIndices, false, true);
  matchGroups(cur.stableIndices, prev.stableIndices, true, false);
  matchGroups(cur.geometryIndices, prev.geometryIndices, false, false);

  for (size_t i = begin; i < currentTransforms.size(); ++i) {
    if (currentToPrevious[i] != SIZE_MAX) {
      ++cur.matches;
    }
  }

  // A one-matrix draw still without a partner moves like a matched draw with the very same matrix
  // (scenery pieces share the camera's matrix), instead of standing at its final place among them.
  auto& borrowedPrevious = cur.borrowedPrevious;
  borrowedPrevious.resize(currentTransforms.size(), SIZE_MAX);
  for (size_t i = begin; i < currentTransforms.size(); ++i) {
    borrowedPrevious[i] = SIZE_MAX;
  }
  {
    static std::unordered_map<HashType, size_t> matchedByMatrix;
    matchedByMatrix.clear();
    const auto matrixKey = [](const FrameTransformSnapshot& snapshot) noexcept {
      return xxh3_hash_s(&snapshot.position, sizeof(snapshot.position), snapshot.viewportIdentity);
    };
    bool anyUnmatched = false;
    for (size_t i = begin; i < currentTransforms.size() && !anyUnmatched; ++i) {
      anyUnmatched = currentToPrevious[i] == SIZE_MAX && !currentTransforms[i].transform.indexedMatrices;
    }
    if (anyUnmatched) {
      for (size_t i = 0; i < currentTransforms.size(); ++i) {
        const auto& snapshot = currentTransforms[i].transform;
        if (currentToPrevious[i] != SIZE_MAX && !snapshot.indexedMatrices &&
            !previousTransforms[currentToPrevious[i]].transform.indexedMatrices) {
          matchedByMatrix.emplace(matrixKey(snapshot), i);
        }
      }
      for (size_t i = begin; i < currentTransforms.size(); ++i) {
        const auto& snapshot = currentTransforms[i].transform;
        if (currentToPrevious[i] != SIZE_MAX || snapshot.indexedMatrices) {
          continue;
        }
        const auto sibling = matchedByMatrix.find(matrixKey(snapshot));
        if (sibling != matchedByMatrix.end()) {
          borrowedPrevious[i] = currentToPrevious[sibling->second];
          s_diagBorrowedDraws.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  }

  // Only ordinary one-matrix draws use motion prediction. Indexed draws keep the slot
  // identity encoded by PNMTXIDX and are prepared as one coherent draw below.
  constexpr float kMaximumPredictionSeedDeltaSquared =
      kMaximumTranslationPerFrame * kMaximumTranslationPerFrame;
  for (size_t currentIndex = begin; currentIndex < currentTransforms.size(); ++currentIndex) {
    const size_t previousIndex = currentToPrevious[currentIndex];
    if (previousIndex == SIZE_MAX) {
      continue;
    }
    auto& currentEntry = cur.transforms[currentIndex];
    const auto& previousEntry = previousTransforms[previousIndex];
    if (currentEntry.transform.indexedMatrices || previousEntry.transform.indexedMatrices) {
      continue;
    }
    // Seed the next frame's matching with a constant-velocity reference, but never from
    // a pair the interpolator would reject as a teleport (`<=` so NaN fails too).
    if (translation_delta_squared(previousEntry.transform.position,
                                  currentEntry.transform.position) <=
        kMaximumPredictionSeedDeltaSquared) {
      currentEntry.predictedPosition = extrapolate_transform(previousEntry.transform.position,
                                                             currentEntry.transform.position);
      currentEntry.hasPrediction = true;
    }
  }

  // A cut is a property of the whole shot, so an even sample of the segment decides it as well as every
  // pair does; each measure inverts a matrix in double, which added up on 5000-draw screens.
  constexpr size_t kCutSamples = 256;
  const size_t cutStride = std::max<size_t>(1, (currentTransforms.size() - begin) / kCutSamples);
  for (size_t currentIndex = begin; currentIndex < currentTransforms.size(); currentIndex += cutStride) {
    const size_t previousIndex = currentToPrevious[currentIndex];
    if (previousIndex == SIZE_MAX) {
      continue;
    }
    const auto& current = currentTransforms[currentIndex].transform;
    const auto& previous = previousTransforms[previousIndex].transform;
    if (current.indexedMatrices || previous.indexedMatrices) {
      continue;
    }
    float angle = 0.f;
    float distance = 0.f;
    if (!camera_motion(previous.position, current.position, angle, distance)) {
      continue;
    }
    ++cur.motionPairs;
    if (angle > kCutAngleDegrees || distance > kCutDistance) {
      ++cur.cutPairs;
    }
  }

  if (taskIndices.empty()) {
    return;
  }

  // Prepare each matched pair once: every sample of a draw shares the same
  // previous/current matrices. A flat vector keeps the sample tasks parallel.
  // Kept across segments: with ~5000 draws (the pause-screen map) these are megabytes, and allocating
  // them for every frame showed up as allocator time on the frame worker. An entry is reset when its
  // draw is first prepared below; only prepared entries are read.
  static std::vector<PreparedTransformInterpolation> preparedTransforms;
  static std::vector<uint8_t> preparedTransformState;
  static std::vector<PreparedAffinePair> preparedPairs;
  if (preparedTransforms.size() < currentTransforms.size()) {
    preparedTransforms.resize(currentTransforms.size());
  }
  preparedTransformState.assign(currentTransforms.size(), 0);
  preparedPairs.clear();
  preparedPairs.reserve(taskIndices.size() * 2);

  // One-matrix pairs are independent of each other: their slots are reserved here and filled on the
  // worker pool after the serial pass below (~0.35 us each; 5000 per frame on the map screen).
  struct DeferredPair {
    size_t offset;
    const Mat3x4<float>* previousPosition;
    const Mat3x4<float>* currentPosition;
    const Mat3x4<float>* previousNormal;
    const Mat3x4<float>* currentNormal;
  };
  static std::vector<DeferredPair> deferredPairs;
  deferredPairs.clear();

  const auto appendPreparedPair = [&](const Mat3x4<float>& previousPosition,
                                      const Mat3x4<float>& currentPosition,
                                      const Mat3x4<float>& previousNormal,
                                      const Mat3x4<float>& currentNormal,
                                      bool indexed) {
    const size_t pairOffset = preparedPairs.size();
    preparedPairs.push_back(indexed ? prepare_indexed_pair(previousPosition, currentPosition)
                                    : prepare_affine_pair(previousPosition, currentPosition));
    preparedPairs.push_back(indexed ? prepare_indexed_pair(previousNormal, currentNormal)
                                    : prepare_affine_pair(previousNormal, currentNormal));
    return pairOffset;
  };

  // Content-keyed palette pairing: a mesh split across draws repeats bone matrices byte
  // for byte, so a chunk with no partner of its own can borrow a sibling's.
  static std::vector<int32_t> paletteDrawIndex; // draw -> compact palette index
  static std::vector<uint32_t> paletteDraws;    // compact palette index -> draw
  paletteDrawIndex.assign(currentTransforms.size(), -1);
  paletteDraws.clear();
  for (size_t drawIndex = begin; drawIndex < currentTransforms.size(); ++drawIndex) {
    const auto& transform = currentTransforms[drawIndex].transform;
    if (!transform.indexedMatrices || transform.usedMatrixMask == 0) {
      continue;
    }
    paletteDrawIndex[drawIndex] = static_cast<int32_t>(paletteDraws.size());
    paletteDraws.push_back(static_cast<uint32_t>(drawIndex));
  }

  struct PaletteSlotKey {
    HashType hash = 0;
    uint32_t palette = 0;
    uint32_t slot = 0;
  };
  struct ResolvedSlot {
    const Mat3x4<float>* position = nullptr;
    const Mat3x4<float>* normal = nullptr;
  };
  static std::vector<PaletteSlotKey> paletteSlotKeys;
  static std::vector<ResolvedSlot> resolvedSlots;
  static std::vector<size_t> resolvedProjectionEntry;
  paletteSlotKeys.clear();
  resolvedSlots.assign(paletteDraws.size() * MaxPnMtx, ResolvedSlot{});
  resolvedProjectionEntry.assign(paletteDraws.size(), kNoPreparedPair);

  for (uint32_t palette = 0; palette < paletteDraws.size(); ++palette) {
    const auto& transform = currentTransforms[paletteDraws[palette]].transform;
    for (uint32_t slot = 0; slot < MaxPnMtx; ++slot) {
      if ((transform.usedMatrixMask & (1u << slot)) == 0) {
        continue;
      }
      paletteSlotKeys.push_back({combine_identity(transform.indexedMatrices->slotHash[slot],
                                                 transform.viewportIdentity), palette, slot});
    }
  }
  std::sort(paletteSlotKeys.begin(), paletteSlotKeys.end(),
            [](const PaletteSlotKey& lhs, const PaletteSlotKey& rhs) {
              if (lhs.hash != rhs.hash) {
                return lhs.hash < rhs.hash;
              }
              if (lhs.palette != rhs.palette) {
                return lhs.palette < rhs.palette;
              }
              return lhs.slot < rhs.slot;
            });

  // Draws whose palettes hold the same bone byte for byte are pieces of one skeleton.
  static std::vector<uint32_t> skeletonOf;
  skeletonOf.resize(paletteDraws.size());
  for (uint32_t palette = 0; palette < paletteDraws.size(); ++palette) {
    skeletonOf[palette] = palette;
  }
  const auto skeletonRoot = [&](uint32_t palette) {
    while (skeletonOf[palette] != palette) {
      skeletonOf[palette] = skeletonOf[skeletonOf[palette]];
      palette = skeletonOf[palette];
    }
    return palette;
  };
  for (size_t runStart = 0; runStart < paletteSlotKeys.size();) {
    size_t runEnd = runStart + 1;
    while (runEnd < paletteSlotKeys.size() &&
           paletteSlotKeys[runEnd].hash == paletteSlotKeys[runStart].hash) {
      ++runEnd;
    }
    for (size_t keyIndex = runStart + 1; keyIndex < runEnd; ++keyIndex) {
      const uint32_t a = skeletonRoot(paletteSlotKeys[runStart].palette);
      const uint32_t b = skeletonRoot(paletteSlotKeys[keyIndex].palette);
      if (a != b) {
        skeletonOf[b] = a;
      }
    }
    // Where this matrix was last frame, per the draws that did match. Partners that
    // disagree mean no single previous pose, so the coupled unit duplicates.
    const Mat3x4<float>* sourcePosition = nullptr;
    const Mat3x4<float>* sourceNormal = nullptr;
    size_t sourceEntry = kNoPreparedPair;
    HashType sourceHash = 0;
    bool ambiguous = false;
    for (size_t keyIndex = runStart; keyIndex < runEnd && !ambiguous; ++keyIndex) {
      const uint32_t drawIndex = paletteDraws[paletteSlotKeys[keyIndex].palette];
      const size_t previousIndex = currentToPrevious[drawIndex];
      if (previousIndex >= previousTransforms.size()) {
        continue;
      }
      const auto& current = currentTransforms[drawIndex].transform;
      const auto& previous = previousTransforms[previousIndex].transform;
      // A slot index is an absolute palette address, which is why it pairs matched draws
      // and why a changed layout invalidates every slot as a source.
      if (!previous.indexedMatrices || previous.usedMatrixMask != current.usedMatrixMask) {
        continue;
      }
      const uint32_t slot = paletteSlotKeys[keyIndex].slot;
      const HashType candidateHash = previous.indexedMatrices->slotHash[slot];
      if (sourcePosition == nullptr) {
        sourcePosition = &previous.indexedMatrices->position[slot];
        sourceNormal = &previous.indexedMatrices->normal[slot];
        sourceHash = candidateHash;
        sourceEntry = previousIndex;
      } else if (candidateHash != sourceHash) {
        ambiguous = true;
      }
    }
    if (ambiguous || sourcePosition == nullptr) {
      runStart = runEnd;
      continue;
    }
    for (size_t keyIndex = runStart; keyIndex < runEnd; ++keyIndex) {
      const auto& key = paletteSlotKeys[keyIndex];
      resolvedSlots[static_cast<size_t>(key.palette) * MaxPnMtx + key.slot] = {sourcePosition,
                                                                              sourceNormal};
      if (resolvedProjectionEntry[key.palette] == kNoPreparedPair) {
        resolvedProjectionEntry[key.palette] = sourceEntry;
      }
    }
    runStart = runEnd;
  }

  for (const size_t taskIndex : taskIndices) {
    const auto& task = s_pendingUniformInterpolations[taskIndex];
    if (task.currentTransformIndex >= preparedTransformState.size() ||
        preparedTransformState[task.currentTransformIndex] != 0) {
      continue;
    }
    preparedTransformState[task.currentTransformIndex] = 1;

    const auto& current = currentTransforms[task.currentTransformIndex].transform;
    auto& prepared = preparedTransforms[task.currentTransformIndex];
    prepared = PreparedTransformInterpolation{};
    if (task.indexedMatrices) {
      const int32_t palette = paletteDrawIndex[task.currentTransformIndex];
      if (palette < 0) {
        continue;
      }
      prepared.previousProjectionEntry = resolvedProjectionEntry[palette];
      // A palette is one deformation unit: interpolating only the resolved slots cracks
      // the mesh, so any unresolved slot duplicates the whole current draw.
      bool allSlotsValid = true;
      for (size_t slot = 0; slot < MaxPnMtx; ++slot) {
        if ((current.usedMatrixMask & (1u << slot)) == 0) {
          continue;
        }
        const auto& resolved = resolvedSlots[static_cast<size_t>(palette) * MaxPnMtx + slot];
        if (resolved.position == nullptr) {
          allSlotsValid = false;
          break;
        }
        const size_t pairOffset =
            appendPreparedPair(*resolved.position, current.indexedMatrices->position[slot],
                               *resolved.normal, current.indexedMatrices->normal[slot], true);
        prepared.indexedPairOffsets[slot] = pairOffset;
        if (!preparedPairs[pairOffset].valid || !preparedPairs[pairOffset + 1].valid) {
          allSlotsValid = false;
          break;
        }
      }
      prepared.indexedValid = allSlotsValid;
    } else {
      size_t previousTransformIndex = currentToPrevious[task.currentTransformIndex];
      if (previousTransformIndex >= previousTransforms.size()) {
        previousTransformIndex = borrowedPrevious[task.currentTransformIndex];
      }
      if (previousTransformIndex >= previousTransforms.size()) {
        continue;
      }
      const auto& previous = previousTransforms[previousTransformIndex].transform;
      prepared.previousProjectionEntry = previousTransformIndex;
      prepared.nonIndexedPairOffset = preparedPairs.size();
      deferredPairs.push_back({preparedPairs.size(), &previous.position, &current.position, &previous.normal,
                               &current.normal});
      preparedPairs.emplace_back();
      preparedPairs.emplace_back();
    }
  }
  // One decision per skeleton (see PreparedTransformInterpolation::skeletonFrozen).
  static std::vector<uint8_t> skeletonFlags;
  skeletonFlags.assign(paletteDraws.size(), 0);
  constexpr uint8_t kSkeletonFrozen = 1;
  constexpr uint8_t kSkeletonUnpredictable = 2;
  for (uint32_t palette = 0; palette < paletteDraws.size(); ++palette) {
    const uint32_t drawIndex = paletteDraws[palette];
    if (drawIndex >= preparedTransformState.size() || preparedTransformState[drawIndex] == 0) {
      continue;
    }
    const auto& prepared = preparedTransforms[drawIndex];
    auto& flags = skeletonFlags[skeletonRoot(palette)];
    if (!prepared.indexedValid) {
      flags |= kSkeletonFrozen;
      continue;
    }
    const auto mask = currentTransforms[drawIndex].transform.usedMatrixMask;
    for (size_t slot = 0; slot < MaxPnMtx; ++slot) {
      const size_t pairOffset = prepared.indexedPairOffsets[slot];
      if ((mask & (1u << slot)) != 0 && pairOffset != kNoPreparedPair &&
          (!extrapolation_allowed(preparedPairs[pairOffset]) ||
           !extrapolation_allowed(preparedPairs[pairOffset + 1]))) {
        flags |= kSkeletonUnpredictable;
        break;
      }
    }
  }
  for (uint32_t palette = 0; palette < paletteDraws.size(); ++palette) {
    const uint32_t drawIndex = paletteDraws[palette];
    if (drawIndex >= preparedTransformState.size() || preparedTransformState[drawIndex] == 0) {
      continue;
    }
    const uint8_t flags = skeletonFlags[skeletonRoot(palette)];
    preparedTransforms[drawIndex].skeletonFrozen = (flags & kSkeletonFrozen) != 0;
    preparedTransforms[drawIndex].skeletonUnpredictable = (flags & kSkeletonUnpredictable) != 0;
  }

  // preparedPairs no longer grows from here, so the pool can fill the reserved slots in place.
  const auto prepareDeferred = [&](size_t index) {
    const auto& job = deferredPairs[index];
    preparedPairs[job.offset] = prepare_affine_pair(*job.previousPosition, *job.currentPosition);
    preparedPairs[job.offset + 1] = prepare_affine_pair(*job.previousNormal, *job.currentNormal);
  };

  const auto interpolatePendingUniform = [&](const PendingUniformInterpolation& task) {
    // Every staged copy starts as the draw's own uniforms, so a draw left unmatched renders
    // its end state instead of whatever the staging buffer held.
    std::memcpy(task.uniformData, task.sourceUniformData, task.uniformSize);
    // Weight 1 is the frame itself: its own uniforms, not a decomposed and rebuilt copy. Above 1 is a
    // low-latency prediction past the frame.
    if (task.weight == 1.f || task.currentTransformIndex >= currentTransforms.size()) {
      return;
    }
    const auto& current = currentTransforms[task.currentTransformIndex].transform;
    const auto& prepared = preparedTransforms[task.currentTransformIndex];
    if (task.indexedMatrices &&
        (!prepared.indexedValid || prepared.skeletonFrozen ||
         (task.weight > 1.f && prepared.skeletonUnpredictable))) {
      return;
    }
    // The projection comes from whichever previous entry supplied the transforms, which
    // for a borrowed palette is a sibling's partner.
    const size_t previousTransformIndex = prepared.previousProjectionEntry;
    if (previousTransformIndex >= previousTransforms.size()) {
      return;
    }
    const auto& previous = previousTransforms[previousTransformIndex].transform;
    const float weight = task.weight;
    const auto interpolatedProjection =
        interpolate_projection(previous.projection, current.projection, weight);
    std::memcpy(task.uniformData + task.projectionOffset, &interpolatedProjection,
                sizeof(interpolatedProjection));

    const auto interpolateMatrixSlot = [&](size_t currentIndex, size_t pairOffset) {
      if (pairOffset == kNoPreparedPair) {
        return;
      }
      Mat3x4<float> interpolatedPosition{};
      Mat3x4<float> interpolatedNormal{};
      evaluate_affine_pair(preparedPairs[pairOffset], weight, interpolatedPosition);
      evaluate_affine_pair(preparedPairs[pairOffset + 1], weight, interpolatedNormal);
      std::memcpy(task.uniformData + task.positionOffset + currentIndex * sizeof(Mat3x4<float>),
                  &interpolatedPosition, sizeof(interpolatedPosition));
      std::memcpy(task.uniformData + task.normalOffset + currentIndex * sizeof(Mat3x4<float>),
                  &interpolatedNormal, sizeof(interpolatedNormal));
    };
    // Texture matrices that follow the camera (environment and projected maps) or scroll slowly move with
    // the geometry. Left at the frame's value, the glossy layer of the ship's console screens ran half a
    // frame ahead of the panels at every inserted image while the camera turned and the screens flickered
    // (user report 2026-11-10, 120 Hz); at 60 Hz nothing is shown in between.
    // Applied after the geometry, and only where the geometry moved too.
    const auto blendTextureMatrices = [&] {
      const auto& currentEntry = currentTransforms[task.currentTransformIndex];
      if (currentEntry.textureMatrixCount == 0) {
        return;
      }
      const auto& previousEntry = previousTransforms[previousTransformIndex];
      if (previousEntry.textureMatrixCount == currentEntry.textureMatrixCount) {
        for (uint32_t i = 0; i < currentEntry.textureMatrixCount; ++i) {
          const auto& [offset, now] = cur.textureMatrices[currentEntry.textureMatrixBegin + i];
          const auto& [previousOffset, before] = prev.textureMatrices[previousEntry.textureMatrixBegin + i];
          if (offset != previousOffset || offset + sizeof(Mat3x4<float>) > task.uniformSize ||
              !texture_matrix_moves_smoothly(before, now)) {
            continue;
          }
          Mat3x4<float> blended{};
          const std::array<const Vec4<float>*, 3> beforeRows{&before.m0, &before.m1, &before.m2};
          const std::array<const Vec4<float>*, 3> nowRows{&now.m0, &now.m1, &now.m2};
          const std::array<Vec4<float>*, 3> outRows{&blended.m0, &blended.m1, &blended.m2};
          for (size_t row = 0; row < 3; ++row) {
            for (size_t column = 0; column < 4; ++column) {
              const float a = (*beforeRows[row])[column];
              (*outRows[row])[column] = a + ((*nowRows[row])[column] - a) * weight;
            }
          }
          std::memcpy(task.uniformData + offset, &blended, sizeof(blended));
        }
      }
    };
    if (task.indexedMatrices) {
      if (!current.indexedMatrices) {
        return;
      }
      // A palette is one deformation unit: past the frame, one bone too fast to predict keeps the
      // whole draw at the frame, or that bone would tear away from the others.
      if (weight > 1.f) {
        for (size_t currentIndex = 0; currentIndex < MaxPnMtx; ++currentIndex) {
          const size_t pairOffset = prepared.indexedPairOffsets[currentIndex];
          if ((current.usedMatrixMask & (1u << currentIndex)) != 0 && pairOffset != kNoPreparedPair &&
              (!extrapolation_allowed(preparedPairs[pairOffset]) ||
               !extrapolation_allowed(preparedPairs[pairOffset + 1]))) {
            return;
          }
        }
      }
      for (size_t currentIndex = 0; currentIndex < MaxPnMtx; ++currentIndex) {
        if ((current.usedMatrixMask & (1u << currentIndex)) == 0) {
          continue;
        }
        interpolateMatrixSlot(currentIndex, prepared.indexedPairOffsets[currentIndex]);
      }
      blendTextureMatrices();
    } else {
      interpolateMatrixSlot(task.currentMatrix, prepared.nonIndexedPairOffset);
      const size_t pairOffset = prepared.nonIndexedPairOffset;
      // Past the frame, a draw whose motion was too fast to predict stays at the frame, its maps too.
      if (weight <= 1.f || (pairOffset != kNoPreparedPair && extrapolation_allowed(preparedPairs[pairOffset]) &&
                            extrapolation_allowed(preparedPairs[pairOffset + 1]))) {
        blendTextureMatrices();
      }
    }
  };
  // A handful of tasks costs more to schedule than to run. The pool stands in for
  // std::execution::par, which libc++ does not provide at all.
  constexpr size_t kMinimumParallelInterpolationTasks = 64;
  if (deferredPairs.size() < kMinimumParallelInterpolationTasks) {
    for (size_t index = 0; index < deferredPairs.size(); ++index) {
      prepareDeferred(index);
    }
  } else {
    InterpolationWorkerPool::instance().run(deferredPairs.size(), prepareDeferred);
  }
  if (taskIndices.size() < kMinimumParallelInterpolationTasks) {
    for (const size_t taskIndex : taskIndices) {
      interpolatePendingUniform(s_pendingUniformInterpolations[taskIndex]);
    }
  } else {
    InterpolationWorkerPool::instance().run(taskIndices.size(), [&](size_t index) {
      interpolatePendingUniform(s_pendingUniformInterpolations[taskIndices[index]]);
    });
  }
}
} // namespace

void set_frame_interpolation_fps(uint32_t targetFps) noexcept {
  if (targetFps != AURORA_FRAME_INTERPOLATION_UNCAPPED &&
      (targetFps <= AURORA_FRAME_INTERPOLATION_MIN_FPS || targetFps > AURORA_FRAME_INTERPOLATION_MAX_FPS)) {
    targetFps = 0;
  }
  detail::g_frameInterpolationFps.store(targetFps, std::memory_order_release);
  // Fixed targets start at the configured quality and back off from there. Uncapped starts low and
  // climbs while the machine has room, since nothing says how far this one can go.
  s_imagesPerFrameTarget.store(targetFps == AURORA_FRAME_INTERPOLATION_UNCAPPED ? 2.0
                                                                               : maximum_images_per_frame(targetFps),
                               std::memory_order_release);
  s_dropInterpolationAtSeal.store(false, std::memory_order_release);
  s_pacingWindowFrames.store(0, std::memory_order_release);
  s_pacingWindowMisses.store(0, std::memory_order_release);
  s_pacingWindowLoad.store(0.0, std::memory_order_release);
  s_pacingWindowPresentLoad.store(0.0, std::memory_order_release);
  s_pacingWindowLoadSamples.store(0, std::memory_order_release);
  s_ratePacer = {};
  // A reconfiguration starts a fresh diagnostic window.
  s_diagFramesSealed.store(0, std::memory_order_relaxed);
  s_diagFramesLowMatch.store(0, std::memory_order_relaxed);
  s_diagFramesReplayUnsafe.store(0, std::memory_order_relaxed);
  s_diagSlotReductions.store(0, std::memory_order_relaxed);
  s_diagLateSealDrops.store(0, std::memory_order_relaxed);
  s_diagSealsWithBoundary.store(0, std::memory_order_relaxed);
  s_diagMultiBoundary.store(0, std::memory_order_relaxed);
  s_diagSealsCarried.store(0, std::memory_order_relaxed);
  s_diagSealsComplete.store(0, std::memory_order_relaxed);
  s_diagCarriedDraws.store(0, std::memory_order_relaxed);
  s_diagSealsReplayed.store(0, std::memory_order_relaxed);
  s_diagCameraCuts.store(0, std::memory_order_relaxed);
  s_diagBorrowedDraws.store(0, std::memory_order_relaxed);
  s_diagProbeRestores.store(0, std::memory_order_relaxed);
  s_diagCameraEstimates.store(0, std::memory_order_relaxed);
}

void report_frame_load(double encodeLoad, double presentLoad) noexcept {
  // Averaged over the window: a single load or shader compile is not a rate this machine can't hold,
  // and such a frame already seals without inserted images by itself.
  double seen = s_pacingWindowLoad.load(std::memory_order_acquire);
  while (!s_pacingWindowLoad.compare_exchange_weak(seen, seen + encodeLoad, std::memory_order_acq_rel)) {
  }
  seen = s_pacingWindowPresentLoad.load(std::memory_order_acquire);
  while (!s_pacingWindowPresentLoad.compare_exchange_weak(seen, seen + presentLoad, std::memory_order_acq_rel)) {
  }
  s_pacingWindowLoadSamples.fetch_add(1, std::memory_order_acq_rel);
}

void report_producer_paced(bool paced) noexcept {
  const double maximumImages = maximum_images_per_frame();
  if (maximumImages == 0.0) {
    s_dropInterpolationAtSeal.store(false, std::memory_order_release);
    s_pacingWindowFrames.store(0, std::memory_order_release);
    s_pacingWindowMisses.store(0, std::memory_order_release);
    s_pacingWindowLoad.store(0.0, std::memory_order_release);
    s_pacingWindowPresentLoad.store(0.0, std::memory_order_release);
    s_pacingWindowLoadSamples.store(0, std::memory_order_release);
    s_imagesPerFrameTarget.store(0.0, std::memory_order_release);
    s_alignGate = {};
    s_alignHeadroom.store(false, std::memory_order_release);
    return;
  }

  // Per-frame decision, no streaks: a late frame seals without its inserted slots and
  // the next one is back at full count. See s_dropInterpolationAtSeal.
  s_dropInterpolationAtSeal.store(!paced, std::memory_order_release);

  const uint32_t frames = s_pacingWindowFrames.load(std::memory_order_acquire) + 1;
  const uint32_t misses =
      s_pacingWindowMisses.load(std::memory_order_acquire) + (paced ? 0u : 1u);
  if (frames < kPacingWindowFrames) {
    s_pacingWindowFrames.store(frames, std::memory_order_release);
    s_pacingWindowMisses.store(misses, std::memory_order_release);
    return;
  }
  // Window complete: adjust the target one step at most, then start over. At
  // one decision per window this cannot spam the log or flap within a second.
  // Private QA: keeps the configured rate whatever happens, to exercise it on a slower machine.
  if (AURORA_DIAGNOSTIC_ENV("WII_COMPILED_QA_INTERP_HOLD_RATE") != nullptr) {
    s_pacingWindowFrames.store(0, std::memory_order_release);
    s_pacingWindowMisses.store(0, std::memory_order_release);
    return;
  }
  const double current =
      std::min(s_imagesPerFrameTarget.load(std::memory_order_acquire), maximumImages);
  const uint32_t loadSamples = s_pacingWindowLoadSamples.exchange(0, std::memory_order_acq_rel);
  const double load = s_pacingWindowLoad.exchange(0.0, std::memory_order_acq_rel) /
                      static_cast<double>(std::max(loadSamples, 1u));
  const double presentLoad = s_pacingWindowPresentLoad.exchange(0.0, std::memory_order_acq_rel) /
                             static_cast<double>(std::max(loadSamples, 1u));
  // See policy::step_images_per_frame: late frames alone only probe a lower rate, and the rate comes
  // straight back when the game stays late without the images.
  double target = current;
  const auto change = policy::step_images_per_frame(s_ratePacer, target, maximumImages, load, presentLoad,
                                                    static_cast<double>(misses) / static_cast<double>(frames));
  if (change == policy::RateChange::Reduced || change == policy::RateChange::Probe) {
    s_diagSlotReductions.fetch_add(1, std::memory_order_relaxed);
  }
  if (change == policy::RateChange::ProbeRestored) {
    s_diagProbeRestores.fetch_add(1, std::memory_order_relaxed);
  }
  if (change != policy::RateChange::None) {
    static constexpr const char* kChangeNames[] = {"", "reduced", "probe", "restored: the game stays late without the images",
                                                   "kept: late frames stopped without the images", "raised"};
    Log.info("interpolation rate {:.1f} -> {:.1f} images per frame ({}): {}/{} frames overran their retrace, "
             "encode {:.2f}, present {:.2f}", current, target, kChangeNames[static_cast<int>(change)], misses,
             frames, load, presentLoad);
  }
  s_alignHeadroom.store(policy::step_align_gate(s_alignGate, change == policy::RateChange::None &&
                                                                 policy::pacing_is_comfortable(
                                                                     target, maximumImages, misses, frames, load)),
                        std::memory_order_release);
  s_imagesPerFrameTarget.store(target, std::memory_order_release);
  s_pacingWindowFrames.store(0, std::memory_order_release);
  s_pacingWindowMisses.store(0, std::memory_order_release);
}

bool frame_interpolation_has_headroom() noexcept { return s_alignHeadroom.load(std::memory_order_acquire); }

void begin_frame_interpolation() noexcept {
  const uint32_t targetFps = frame_interpolation_fps();
  if (targetFps != s_previousInterpolationFps) {
    reset_frames();
    s_previousInterpolationFps = targetFps;
  }
  // Normally empty (the seal consumed them); an aborted batch can leave tasks behind, whose staging
  // pointers are already gone.
  s_pendingUniformInterpolations.clear();
  s_completedInBatch = nullptr;
  s_boundariesInBatch = 0;
  s_frameInterpolationReplaySafe.store(true, std::memory_order_release);
  if (targetFps != 0) {
    recording_frame();
  }
}

void note_frame_interpolation_boundary() noexcept {
  if (!frame_interpolation_active()) {
    return;
  }
  InterpolationFrame& completed = recording_frame();
  completed.complete = true;
  s_completedInBatch = &completed;
  ++s_boundariesInBatch;
  // A batch only holds a few display copies; many without a seal (a stall in the presenter) would
  // keep every frame alive, so give up interpolating that batch and keep the newest two.
  constexpr size_t kMaximumFramesBetweenSeals = 6;
  if (s_frames.size() >= kMaximumFramesBetweenSeals) {
    mark_frame_interpolation_replay_unsafe();
    while (s_frames.size() > 1) {
      recycle_frame(std::move(s_frames.front()));
      s_frames.pop_front();
    }
    s_completedInBatch = s_frames.back().get();
  }
  auto next = acquire_frame();
  latch_cadence(*next);
  s_frames.push_back(std::move(next));
}

uint32_t frame_interpolation_boundaries_in_batch() noexcept { return s_boundariesInBatch; }

void finalize_frame_interpolation() noexcept {
  const bool dropSlots = s_dropInterpolationAtSeal.exchange(false, std::memory_order_acq_rel);
  if (!frame_interpolation_active()) {
    reset_frames();
    s_hasInterpolatedFrame.store(false, std::memory_order_release);
    s_sealSamples.store(0, std::memory_order_release);
    s_sealPresentsNative.store(true, std::memory_order_release);
    return;
  }
  // Every frame that recorded draws since the last seal, oldest first: the tail of the frame whose
  // copy this batch holds is matched against its predecessor before the next frame's head is matched
  // against it, which needs the predictions seeded by the former.
  static std::vector<size_t> frameTasks;
  static std::vector<uint8_t> taskDone;
  taskDone.assign(s_pendingUniformInterpolations.size(), 0);
  for (size_t frameIndex = 0; frameIndex < s_frames.size(); ++frameIndex) {
    InterpolationFrame& frame = *s_frames[frameIndex];
    InterpolationFrame* previous = frameIndex > 0 ? s_frames[frameIndex - 1].get() : nullptr;
    frameTasks.clear();
    for (size_t taskIndex = 0; taskIndex < s_pendingUniformInterpolations.size(); ++taskIndex) {
      if (s_pendingUniformInterpolations[taskIndex].frame == &frame) {
        frameTasks.push_back(taskIndex);
        taskDone[taskIndex] = 1;
      }
    }
    const size_t begin = frame.matchedCount;
    if (previous != nullptr && begin < frame.transforms.size()) {
      process_segment(frame, *previous, begin, frameTasks);
    } else {
      for (const size_t taskIndex : frameTasks) {
        const auto& task = s_pendingUniformInterpolations[taskIndex];
        std::memcpy(task.uniformData, task.sourceUniformData, task.uniformSize);
      }
    }
    frame.matchedCount = frame.transforms.size();
  }
  // A task whose frame is gone (only after an explicit reset) still owns a staged range.
  for (size_t taskIndex = 0; taskIndex < s_pendingUniformInterpolations.size(); ++taskIndex) {
    if (!taskDone[taskIndex]) {
      const auto& task = s_pendingUniformInterpolations[taskIndex];
      std::memcpy(task.uniformData, task.sourceUniformData, task.uniformSize);
    }
  }
  s_pendingUniformInterpolations.clear();

  // The seal presents the last frame whose display copy it holds. Without one there is nothing new
  // to insert images for.
  const InterpolationFrame* presented = s_completedInBatch;
  const bool replaySafe = frame_interpolation_replay_safe();
  const bool cameraCut = presented != nullptr && frame_is_camera_cut(*presented);
  if (cameraCut) {
    s_diagCameraCuts.fetch_add(1, std::memory_order_relaxed);
  }
  const bool eligible = presented != nullptr && !dropSlots && !cameraCut && presented->cadence.samples != 0;
  if (dropSlots && presented != nullptr) {
    s_diagLateSealDrops.fetch_add(1, std::memory_order_relaxed);
  }
  const uint32_t samples = eligible ? presented->cadence.samples : 0u;
  for (uint32_t sample = 0; sample < MaxInterpolatedFrames; ++sample) {
    s_sealWeights[sample] = sample < samples ? presented->cadence.weights[sample] : 1.f;
  }
  s_sealSamples.store(samples, std::memory_order_release);
  s_sealPresentsNative.store(!eligible || presented->cadence.presentNative, std::memory_order_release);
  s_sealBias.store(presented != nullptr ? presented->bias : 1.0, std::memory_order_release);
  s_sealExtrapolated.store(eligible && presented->extrapolated, std::memory_order_release);
  s_hasInterpolatedFrame.store(eligible, std::memory_order_release);

  // Overlay observability: the live match ratio, and how often the scene sits in
  // low-match territory where inserted slots mostly duplicate draws.
  {
    constexpr uint32_t kLowMatchPercent = 55;
    s_diagEligible.store(eligible, std::memory_order_relaxed);
    s_diagReplaySafe.store(replaySafe, std::memory_order_relaxed);
    if (s_boundariesInBatch != 0) {
      s_diagSealsWithBoundary.fetch_add(1, std::memory_order_relaxed);
    }
    if (s_boundariesInBatch > 1) {
      s_diagMultiBoundary.fetch_add(1, std::memory_order_relaxed);
    }
    if (presented != nullptr) {
      s_diagCandidates.store(presented->candidates, std::memory_order_relaxed);
      s_diagMatchable.store(presented->matchable, std::memory_order_relaxed);
      s_diagMatches.store(presented->matches, std::memory_order_relaxed);
      s_diagFramesSealed.fetch_add(1, std::memory_order_relaxed);
      if (presented->matchable != 0 && presented->matches * 100 < presented->matchable * kLowMatchPercent) {
        s_diagFramesLowMatch.fetch_add(1, std::memory_order_relaxed);
      }
      if (!replaySafe) {
        s_diagFramesReplayUnsafe.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  // Keep the newest complete frame (the recording frame's remaining draws match against it) and
  // everything after it.
  size_t keepFrom = 0;
  for (size_t frameIndex = 0; frameIndex + 1 < s_frames.size(); ++frameIndex) {
    if (s_frames[frameIndex]->complete) {
      keepFrom = frameIndex;
    }
  }
  for (size_t dropped = 0; dropped < keepFrom; ++dropped) {
    recycle_frame(std::move(s_frames.front()));
    s_frames.pop_front();
  }
  s_completedInBatch = nullptr;
  s_boundariesInBatch = 0;
}

void note_frame_interpolation_seal(bool complete, bool carried, uint32_t carriedDraws, bool replayed) noexcept {
  if (complete) {
    s_diagSealsComplete.fetch_add(1, std::memory_order_relaxed);
  }
  if (carried) {
    s_diagSealsCarried.fetch_add(1, std::memory_order_relaxed);
    s_diagCarriedDraws.fetch_add(carriedDraws, std::memory_order_relaxed);
  }
  if (replayed) {
    s_diagSealsReplayed.fetch_add(1, std::memory_order_relaxed);
  }
}

void get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics& diagnostics) noexcept {
  diagnostics.targetFps = frame_interpolation_fps();
  diagnostics.targetSamples = samples_for_images_per_frame(
      std::min(s_imagesPerFrameTarget.load(std::memory_order_acquire), maximum_images_per_frame()));
  diagnostics.activeSamples = s_sealSamples.load(std::memory_order_acquire);
  diagnostics.candidates = s_diagCandidates.load(std::memory_order_relaxed);
  diagnostics.matchable = s_diagMatchable.load(std::memory_order_relaxed);
  diagnostics.matches = s_diagMatches.load(std::memory_order_relaxed);
  diagnostics.eligible = s_diagEligible.load(std::memory_order_relaxed) ? 1 : 0;
  diagnostics.replaySafe = s_diagReplaySafe.load(std::memory_order_relaxed) ? 1 : 0;
  diagnostics.framesSealed = s_diagFramesSealed.load(std::memory_order_relaxed);
  diagnostics.framesLowMatch = s_diagFramesLowMatch.load(std::memory_order_relaxed);
  diagnostics.framesReplayUnsafe = s_diagFramesReplayUnsafe.load(std::memory_order_relaxed);
  diagnostics.slotReductions = s_diagSlotReductions.load(std::memory_order_relaxed);
  diagnostics.lateSealDrops = s_diagLateSealDrops.load(std::memory_order_relaxed);
  diagnostics.sealsWithBoundary = s_diagSealsWithBoundary.load(std::memory_order_relaxed);
  diagnostics.multiBoundary = s_diagMultiBoundary.load(std::memory_order_relaxed);
  diagnostics.sealsCarried = s_diagSealsCarried.load(std::memory_order_relaxed);
  diagnostics.sealsComplete = s_diagSealsComplete.load(std::memory_order_relaxed);
  diagnostics.carriedDraws = s_diagCarriedDraws.load(std::memory_order_relaxed);
  diagnostics.sealsReplayed = s_diagSealsReplayed.load(std::memory_order_relaxed);
  diagnostics.cameraCuts = s_diagCameraCuts.load(std::memory_order_relaxed);
  diagnostics.borrowedDraws = s_diagBorrowedDraws.load(std::memory_order_relaxed);
  diagnostics.probeRestores = s_diagProbeRestores.load(std::memory_order_relaxed);
  diagnostics.cameraEstimates = s_diagCameraEstimates.load(std::memory_order_relaxed);
}

bool has_interpolated_frame() noexcept {
  return s_hasInterpolatedFrame.load(std::memory_order_acquire);
}

uint32_t interpolated_frame_count() noexcept {
  if (!has_interpolated_frame()) {
    return 0;
  }
  // The count latched when the presented game frame began recording: its draws staged exactly this
  // many ranges.
  return s_sealSamples.load(std::memory_order_acquire);
}

uint32_t active_interpolation_samples() noexcept {
  return s_activeInterpolationSamples.load(std::memory_order_acquire);
}

uint64_t frame_interpolation_pair_evaluations() noexcept {
  return s_pairEvaluations.load(std::memory_order_relaxed);
}

float interpolated_frame_weight(uint32_t sample) noexcept {
  return sample < MaxInterpolatedFrames ? s_sealWeights[sample] : 1.f;
}

bool interpolated_frame_presents_native() noexcept {
  return !has_interpolated_frame() || s_sealPresentsNative.load(std::memory_order_acquire);
}

double interpolated_frame_bias() noexcept { return s_sealBias.load(std::memory_order_acquire); }

bool interpolated_frame_extrapolates() noexcept { return s_sealExtrapolated.load(std::memory_order_acquire); }

void set_frame_interpolation_low_latency(bool enabled) noexcept {
  s_lowLatency.store(enabled, std::memory_order_release);
}

bool frame_interpolation_low_latency() noexcept { return s_lowLatency.load(std::memory_order_acquire); }

void drop_pending_frame_interpolation_uniforms() noexcept {
  // Pending tasks hold raw pointers into the mapped uniform staging range, so anything
  // that unmaps or rotates that buffer first has to drop the tasks.
  s_pendingUniformInterpolations.clear();
}

void mark_frame_interpolation_replay_unsafe() noexcept {
  s_frameInterpolationReplaySafe.store(false, std::memory_order_release);
  // Marking the frame unsafe rotates the staging buffer mid-frame, and the seal then
  // duplicates slots instead of replaying, so drop the copies staged so far.
  drop_pending_frame_interpolation_uniforms();
}

bool frame_interpolation_replay_safe() noexcept {
  return s_frameInterpolationReplaySafe.load(std::memory_order_acquire);
}

void extend_interpolation_draw(uint16_t usedPnMtxMask) noexcept {
  if (s_frames.empty() || s_frames.back()->transforms.empty()) {
    return;
  }
  auto& snapshot = s_frames.back()->transforms.back().transform;
  if (!snapshot.indexedMatrices) {
    // One-matrix draws all read the current matrix index, which a merge cannot
    // have changed either.
    return;
  }
  const uint16_t addedSlots = static_cast<uint16_t>(usedPnMtxMask & ~snapshot.usedMatrixMask);
  if (addedSlots == 0) {
    return;
  }
  for (size_t slot = 0; slot < MaxPnMtx; ++slot) {
    if ((addedSlots & (1u << slot)) == 0) {
      continue;
    }
    snapshot.indexedMatrices->position[slot] = g_gxState.pnMtx[slot].pos;
    snapshot.indexedMatrices->normal[slot] = g_gxState.pnMtx[slot].nrm;
    snapshot.indexedMatrices->slotHash[slot] =
        xxh3_hash_s(&g_gxState.pnMtx[slot].pos, sizeof(Mat3x4<float>),
                    xxh3_hash_s(&g_gxState.pnMtx[slot].nrm, sizeof(Mat3x4<float>)));
  }
  snapshot.usedMatrixMask |= addedSlots;
}

std::array<gfx::Range, MaxInterpolatedFrames> record_interpolation_draw(
    const FrameInterpolationDrawIdentity& drawIdentity, const Mat4x4<float>& projection,
    uint16_t usedPnMtxMask, const InterpolatedUniformLayout& uniformLayout) noexcept {
  // Split-screen cameras can draw identical meshes in unrelated view spaces.
  // Scope exact, material-only and sibling-palette history to the guest viewport.
  // Logical coordinates keep render-scale changes out of the camera identity.
  const auto& viewport = g_gxState.logicalViewport;
  const std::array viewportValues{viewport.left, viewport.top, viewport.width,
                                  viewport.height, viewport.znear, viewport.zfar};
  const HashType viewportIdentity = xxh3_hash_s(viewportValues.data(), sizeof(viewportValues));
  auto identity = drawIdentity;
  identity.combined = combine_identity(identity.combined, viewportIdentity);
  identity.pipeline = combine_identity(identity.pipeline, viewportIdentity);
  identity.geometry = combine_identity(identity.geometry, viewportIdentity);
  FrameTransformSnapshot snapshot{
      .projection = projection,
      .viewportIdentity = viewportIdentity,
      .usedMatrixMask = usedPnMtxMask,
  };
  if (uniformLayout.indexedMatrices) {
    snapshot.indexedMatrices = acquire_indexed_matrices();
    // Only the used slots are copied; consumers mask with usedMatrixMask, so the stale
    // pool contents of unused slots are never read.
    for (size_t i = 0; i < MaxPnMtx; ++i) {
      if ((usedPnMtxMask & (1u << i)) == 0) {
        continue;
      }
      snapshot.indexedMatrices->position[i] = g_gxState.pnMtx[i].pos;
      snapshot.indexedMatrices->normal[i] = g_gxState.pnMtx[i].nrm;
      snapshot.indexedMatrices->slotHash[i] =
          xxh3_hash_s(&g_gxState.pnMtx[i].pos, sizeof(Mat3x4<float>),
                      xxh3_hash_s(&g_gxState.pnMtx[i].nrm, sizeof(Mat3x4<float>)));
    }
  } else {
    const size_t currentMatrix = std::min<size_t>(g_gxState.currentPnMtx, MaxPnMtx - 1);
    snapshot.position = g_gxState.pnMtx[currentMatrix].pos;
    snapshot.normal = g_gxState.pnMtx[currentMatrix].nrm;
  }

  InterpolationFrame& frame = recording_frame();
  const InterpolationFrame* previous = recording_previous_frame();
  if (uniformLayout.indexedMatrices) {
    frame.hasIndexedMatrices = true;
  }
  const size_t currentTransformIndex = frame.transforms.size();
  const uint32_t textureMatrixBegin = static_cast<uint32_t>(frame.textureMatrices.size());
  const uint32_t textureMatrixCount = static_cast<uint32_t>(
      std::min<size_t>(uniformLayout.textureMatrixCount, InterpolatedUniformLayout::kMaxTextureMatrices));
  for (uint32_t i = 0; i < textureMatrixCount; ++i) {
    const uint32_t offset = uniformLayout.textureMatrixOffsets[i];
    Mat3x4<float> value{};
    if (uniformLayout.sourceUniformData != nullptr && offset + sizeof(value) <= uniformLayout.uniformSize) {
      std::memcpy(static_cast<void*>(&value), uniformLayout.sourceUniformData + offset, sizeof(value));
    }
    frame.textureMatrices.emplace_back(offset, value);
  }
  frame.transforms.emplace_back(FrameTransformEntry{
      .identity = identity,
      .transform = std::move(snapshot),
      .textureMatrixBegin = textureMatrixBegin,
      .textureMatrixCount = textureMatrixCount,
  });
  frame.exactIndices[identity.combined].push_back(currentTransformIndex);
  const HashType stableIdentity = stable_identity(identity);
  frame.stableIndices[stableIdentity].push_back(currentTransformIndex);
  frame.geometryIndices[identity.geometry].push_back(currentTransformIndex);

  std::array<gfx::Range, MaxInterpolatedFrames> interpolatedRanges{};
  ++frame.candidates;
  bool hasPreviousPartner = false;
  if (previous != nullptr) {
    const auto exactPrevious = previous->exactIndices.find(identity.combined);
    const auto stablePrevious = previous->stableIndices.find(stableIdentity);
    const auto geometryPrevious = previous->geometryIndices.find(identity.geometry);
    hasPreviousPartner =
        (exactPrevious != previous->exactIndices.end() && !exactPrevious->second.empty()) ||
        (stablePrevious != previous->stableIndices.end() && !stablePrevious->second.empty()) ||
        (geometryPrevious != previous->geometryIndices.end() && !geometryPrevious->second.empty());
  }
  if (hasPreviousPartner) {
    ++frame.matchable;
  }
  // A draw with no identity partner can still borrow at seal time: a skinned one its sibling's palette,
  // a one-matrix one the partner of a matched draw with the same matrix. So stage copies for all.
  const bool stageInterpolation =
      previous != nullptr &&
      (hasPreviousPartner || !uniformLayout.indexedMatrices || previous->hasIndexedMatrices);
  // A frame already split by a submitted prefix duplicates its slots instead of
  // replaying, so staging copies for the resumed suffix would only waste space.
  if (frame_interpolation_replay_safe() && stageInterpolation) {
    const uint32_t sampleCount = std::min<uint32_t>(frame.cadence.samples, MaxInterpolatedFrames);
    for (uint32_t sample = 0; sample < sampleCount; ++sample) {
      auto [interpolatedBuffer, interpolatedRange] = gfx::map_uniform(uniformLayout.uniformSize);
      s_pendingUniformInterpolations.push_back({
          .frame = &frame,
          .currentTransformIndex = currentTransformIndex,
          .sourceUniformData = uniformLayout.sourceUniformData,
          .uniformData = interpolatedBuffer.data(),
          .uniformSize = uniformLayout.uniformSize,
          .projectionOffset = uniformLayout.projectionOffset,
          .positionOffset = uniformLayout.positionOffset,
          .normalOffset = uniformLayout.normalOffset,
          .currentMatrix = uniformLayout.currentMatrix,
          .weight = frame.cadence.weights[sample],
          .indexedMatrices = uniformLayout.indexedMatrices,
      });
      interpolatedRanges[sample] = interpolatedRange;
    }
  }
  return interpolatedRanges;
}
} // namespace aurora::gx
