#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <atomic>
#include <aurora/gfx.h>

namespace aurora::gx::producer_profile {
inline std::mutex snapshotMutex;
inline std::atomic<bool> overlayEnabled{false};
inline AuroraProducerTiming snapshot{};
inline AuroraProducerTiming read_snapshot() {
  std::lock_guard lock(snapshotMutex);return snapshot;
}
enum Stage { Draw, Textures, StaticTexture, BindGroups, Uniform, DuplicateTexture, Count };
struct Counter {uint64_t calls=0,samples=0;double milliseconds=0;};
struct State {
  std::array<Counter,Count> counters{};
  uint32_t random=0x9e3779b9u;
  std::chrono::steady_clock::time_point windowStart=std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point next=windowStart+std::chrono::seconds(5);
};
inline State& state() {static thread_local State value;return value;}
inline bool enabled() {
#if !defined(MKW_PRIME_QA_TOOLS)
  return false;  // test builds only
#else
  static const bool value=[] {
    const auto* text=std::getenv("WII_COMPILED_PRIME_PERFORMANCE_TRACE");
    return text && std::strcmp(text,"1")==0;
  }();
  return value || overlayEnabled.load(std::memory_order_relaxed);
#endif
}
class Scope {
  Stage stage;
  bool sampled=false;
  std::chrono::steady_clock::time_point begin;
public:
  explicit Scope(Stage value):stage(value) {
    if(!enabled())return;
    auto& s=state();auto& c=s.counters[stage];
    ++c.calls;
    // Avoid locking the sample cadence to a repeated sequence of materials.
    s.random^=s.random<<13;s.random^=s.random>>17;s.random^=s.random<<5;
    sampled=(s.random&63u)==0;
    if(sampled)begin=std::chrono::steady_clock::now();
  }
  ~Scope() {
    if(!sampled)return;
    const auto now=std::chrono::steady_clock::now();
    auto& s=state();auto& c=s.counters[stage];
    ++c.samples;c.milliseconds+=std::chrono::duration<double,std::milli>(now-begin).count();
    if(stage!=Draw || now<s.next)return;
    AuroraProducerTiming published{};
    published.windowMs=std::chrono::duration<double,std::milli>(now-s.windowStart).count();
    for(unsigned i=0;i<5;++i) {
      const auto& v=s.counters[i];
      published.calls[i]=v.calls;published.samples[i]=v.samples;
      published.estimatedCpuMs[i]=v.samples ? v.milliseconds*double(v.calls)/double(v.samples) : 0.0;
    }
    {std::lock_guard lock(snapshotMutex);snapshot=published;}
    s.windowStart=now;
    s.next=now+std::chrono::seconds(5);
    std::fprintf(stderr,"[prime-gx-producer] sampled=1 inclusive=1");
    constexpr const char* names[]{"draw","textures","static_texture","bind_groups","uniform","duplicate_texture"};
    for(unsigned i=0;i<Count;++i) {
      const auto& v=s.counters[i];
      std::fprintf(stderr," %s_calls=%llu %s_samples=%llu %s_estimated_ms=%.3f",names[i],
          static_cast<unsigned long long>(v.calls),names[i],static_cast<unsigned long long>(v.samples),
          names[i],v.samples ? v.milliseconds*double(v.calls)/double(v.samples) : 0.0);
    }
    std::fprintf(stderr,"\n");s.counters={};
  }
};
}
