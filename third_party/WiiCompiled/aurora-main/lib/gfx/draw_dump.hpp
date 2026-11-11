#pragma once

// One-shot diagnostic dump of a whole frame: every GX draw (with the state that
// decides whether it reaches the image), every EFB copy, and the presented
// image, for comparing a missing object against what the game submitted.
// Armed from any thread; the file is written only by the GX producer, which
// records the frame, and closed when that frame is sealed.

#include "common.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace aurora::gfx::draw_dump {
inline std::atomic<uint32_t> g_armedFrame{UINT32_MAX};
inline std::mutex g_pathMutex;
inline std::string g_basePath;
inline std::FILE* g_file = nullptr;
inline uint32_t g_drawIndex = 0;
inline uint32_t g_copyIndex = 0;
// Burst: `count` consecutive frames, each written as <root>_f<N>.draws.txt/.bmp, so a
// one-frame flicker can be compared draw by draw against its neighbours.
inline std::string g_burstRoot;
inline uint32_t g_burstRemaining = 0;
inline uint32_t g_burstIndex = 0;
// Base path of the frame finish_frame last sealed (its image is encoded after that call).
inline std::string g_finishedBase;
// WII_COMPILED_DRAW_DUMP_COPY=<n>: the dump's image is the n-th EFB copy's
// texture (with its alpha channel) instead of the presented frame.
inline TextureHandle g_copyTexture;
// WII_COMPILED_DRAW_DUMP_ALL_COPIES: every EFB copy of the dumped frame is
// also read back right after it is encoded, as <base>.copy<N>.bmp.
struct CopyReadback {
  wgpu::Buffer buffer;
  uint32_t width = 0, height = 0, bytesPerRow = 0;
  bool bgra = false;
  std::string path;
};
inline std::vector<CopyReadback> g_copyReadbacks;
// Single-frame dumps (F7) also save every sampled texture under <base>.textures/ with its
// replacement name (content hash); bursts skip it to stay cheap.
inline std::atomic<bool> g_saveTextures{false};
inline bool all_copies_requested() noexcept {
#if !defined(MKW_PRIME_QA_TOOLS)
  return false;
#else
  static const bool value = std::getenv("WII_COMPILED_DRAW_DUMP_ALL_COPIES") != nullptr;
  return value;
#endif
}
inline uint32_t requested_copy() noexcept {
#if !defined(MKW_PRIME_QA_TOOLS)
  return UINT32_MAX;
#else
  static const uint32_t value = [] {
    const char* text = std::getenv("WII_COMPILED_DRAW_DUMP_COPY");
    return text ? static_cast<uint32_t>(std::strtoul(text, nullptr, 10)) : UINT32_MAX;
  }();
  return value;
#endif
}

// Dumps the next frame the producer starts. `basePath` gets ".draws.txt" and
// ".bmp" appended.
inline void request(std::string basePath, uint32_t count = 1) {
#if !defined(MKW_PRIME_QA_TOOLS)
  return;  // test builds only
#endif
  {
    std::lock_guard lock(g_pathMutex);
    g_burstRoot = basePath;
    g_burstIndex = 0;
    g_burstRemaining = count > 1 ? count - 1 : 0;
    g_basePath = count > 1 ? basePath + "_f0" : std::move(basePath);
  }
  g_saveTextures.store(count <= 1, std::memory_order_release);
  const uint32_t frame = current_frame();
  g_armedFrame.store(frame == UINT32_MAX ? 0 : frame + 1, std::memory_order_release);
}

inline std::string base_path() {
  std::lock_guard lock(g_pathMutex);
  return g_basePath;
}
inline std::string finished_base_path() {
  std::lock_guard lock(g_pathMutex);
  return g_finishedBase;
}

// The open dump while the producer records the armed frame, otherwise null.
inline std::FILE* active() noexcept {
#if !defined(MKW_PRIME_QA_TOOLS)
  return nullptr;  // test builds only
#endif
  const uint32_t armed = g_armedFrame.load(std::memory_order_acquire);
  if (armed == UINT32_MAX || current_frame() != armed) return nullptr;
  if (g_file == nullptr) {
    const std::string path = base_path() + ".draws.txt";
    g_file = std::fopen(path.c_str(), "w");
    g_drawIndex = 0;
    g_copyIndex = 0;
    g_copyTexture.reset();
    if (g_file != nullptr) std::fprintf(g_file, "frame %u\n", armed);
  }
  return g_file;
}

inline uint32_t next_draw_index() noexcept { return g_drawIndex++; }

// Called when `frame` is sealed. True when it was the armed frame, so its
// presented image is captured too.
inline bool finish_frame(uint32_t frame) noexcept {
  const uint32_t armed = g_armedFrame.load(std::memory_order_acquire);
  if (armed == UINT32_MAX || frame < armed) return false;
  if (g_file != nullptr) {
    std::fprintf(g_file, "end draws=%u\n", g_drawIndex);
    std::fclose(g_file);
    g_file = nullptr;
  }
  std::lock_guard lock(g_pathMutex);
  g_finishedBase = g_basePath;
  if (frame == armed && g_burstRemaining != 0) {
    --g_burstRemaining;
    g_basePath = g_burstRoot + "_f" + std::to_string(++g_burstIndex);
    g_armedFrame.store(frame + 1, std::memory_order_release);
    return true;
  }
  g_armedFrame.store(UINT32_MAX, std::memory_order_release);
  return frame == armed;
}
} // namespace aurora::gfx::draw_dump
