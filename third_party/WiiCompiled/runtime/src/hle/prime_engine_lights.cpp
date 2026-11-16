// RM3E01 engine lights for Aurora's modern lighting (model and addresses: hle/prime_engine_lights.h).
#include "hle/prime_engine_lights.h"

#include <atomic>

#if defined(MKW_GAME_PROFILE_RM3E01_00)
#include "memory.h"
#include "runtime_log.h"

#include <aurora/gfx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>
#endif

namespace PrimeEngineLights {
namespace {
std::atomic<bool> g_send{false}, g_fade{false};
}  // namespace

void SetOptions(bool sendLights, bool actorFade) {
    const bool wasSending = g_send.exchange(sendLights);
    g_fade.store(actorFade);
#if defined(MKW_GAME_PROFILE_RM3E01_00)
    if (wasSending && !sendLights) aurora_set_engine_lights(nullptr, 0);
#else
    (void)wasSending;
#endif
}
}  // namespace PrimeEngineLights

#if defined(MKW_GAME_PROFILE_RM3E01_00)
namespace {
using PrimeEngineLights::g_fade;
using PrimeEngineLights::g_send;

constexpr uint32_t kLightSize = 0x54;
constexpr float kEpsilon = 1.1920929e-7f;      // 0x806830FC
constexpr float kRangeThreshold = 0.05882353f;  // 0x80683104: 1/17 of the light's intensity

// Game thread only from here on. The fades advance with the host clock, so they always finish even on a
// path that does not run the world lights hook (photo mode, cinematics). Each actor counts the game
// frames since its own last step, rounded: a global grid of 16.667 ms cells gave 0, 1 or 2 steps per
// game frame at random whenever the frame boundary sat next to a cell edge, which lasts for minutes
// once the game follows a 120 Hz monitor (a frame every 16.666 ms).
constexpr int64_t kStepMicros = 16667;
int64_t NowMicros() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration_cast<microseconds>(steady_clock::now() - start).count();
}

bool Valid(uint32_t address, size_t length = 4) { return address != 0 && Memory::Contains(address, length); }
uint32_t Deref(uint32_t address) { return Valid(address) ? Memory::Read32(address) : 0; }
float ReadFloat(uint32_t address) {
    const uint32_t bits = Memory::Read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The range 0x8037A580 gives a light: where its attenuated intensity falls to 1/17.
float LightRange(uint32_t light, const uint8_t color[4], uint32_t type) {
    const float k1 = ReadFloat(light + 0x2C), k2 = ReadFloat(light + 0x30);
    if (!(k1 >= kEpsilon || k2 >= kEpsilon)) return 0.0f;  // no falloff: unbounded
    const uint8_t peak = std::max(std::max(color[0], color[1]), std::max(color[2], color[3]));
    const float intensity = (type == 4 ? ReadFloat(light + 0x34) : 1.0f) * static_cast<float>(peak) / 255.0f;
    if (k2 <= kEpsilon) return k1 > kEpsilon ? intensity / (kRangeThreshold * k1) : 0.0f;
    return intensity > kEpsilon ? std::sqrt(intensity / (kRangeThreshold * k2)) : 0.0f;
}

bool ReadLight(uint32_t light, bool dynamic, AuroraEngineLight& out) {
    if (!Valid(light, kLightSize)) return false;
    out = {};
    for (int i = 0; i < 3; ++i) {
        out.pos[i] = ReadFloat(light + 4u * i);
        out.dir[i] = ReadFloat(light + 0x0C + 4u * i);
        out.distAtt[i] = ReadFloat(light + 0x28 + 4u * i);
        out.angleAtt[i] = ReadFloat(light + 0x34 + 4u * i);
    }
    for (int i = 0; i < 4; ++i) out.color[i] = Memory::Read8(light + 0x1C + i);
    out.type = Memory::Read32(light + 0x20);
    if (out.type > 4) return false;
    out.spotCutoff = ReadFloat(light + 0x24);
    const bool rangeDirty = (Memory::Read8(light + 0x50) & 0x20) != 0;
    out.radius = rangeDirty ? LightRange(light, out.color, out.type) : ReadFloat(light + 0x48);
    if (!(out.radius > 0.0f) || out.radius > 1.0e6f) out.radius = 0.0f;  // unbounded
    out.flags = dynamic ? AURORA_ENGINE_LIGHT_DYNAMIC : 0u;
    for (float v : out.pos)
        if (!std::isfinite(v)) return false;
    return true;
}

void AppendVector(uint32_t vector, bool dynamic, size_t maxCount, std::vector<AuroraEngineLight>& out) {
    if (!Valid(vector, 12)) return;
    const uint32_t count = Memory::Read32(vector), data = Memory::Read32(vector + 8);
    if (count == 0 || count > maxCount || !Valid(data, size_t(count) * kLightSize)) return;
    AuroraEngineLight light;
    for (uint32_t i = 0; i < count && out.size() < 512; ++i)
        if (ReadLight(data + i * kLightSize, dynamic, light)) out.push_back(light);
}

// ---- Character light fading ----------------------------------------------------------------------
constexpr float kFadeStep = 1.0f / 10.0f;  // a sixth of a second at 60 frames per second
struct FadeLight {
    std::array<uint8_t, kLightSize> record{};  // the light as the engine stored it
    float pos[3]{};
    float weight = 0.0f;
    bool present = false;
};
struct FadeState {
    int64_t lastMicros = -1;  // when the fade last stepped; -1 = never
    std::array<FadeLight, 8> lights{};
    uint32_t count = 0;
    float ambient[4]{};
};
std::unordered_map<uint32_t, FadeState> g_fadeStates;
int64_t g_fadeSweepMicros = 0;
// How far a light may move in one game frame and still be the same light (metres).
constexpr float kMovedLightReach = 6.0f;

struct Saved {
    uint32_t actorLights = 0;
    uint32_t count = 0;
    std::array<std::array<uint8_t, kLightSize>, 4> records{};
    uint32_t ambient = 0;
};
std::array<Saved, 4> g_savedStack{};
uint32_t g_savedDepth = 0;

void ReadRecord(uint32_t address, std::array<uint8_t, kLightSize>& record) {
    for (uint32_t i = 0; i < kLightSize; i += 4) {
        const uint32_t word = Memory::Read32(address + i);
        record[i] = uint8_t(word >> 24); record[i + 1] = uint8_t(word >> 16);
        record[i + 2] = uint8_t(word >> 8); record[i + 3] = uint8_t(word);
    }
}
void WriteRecord(uint32_t address, const std::array<uint8_t, kLightSize>& record) {
    for (uint32_t i = 0; i < kLightSize; i += 4)
        Memory::Write32(address + i, uint32_t(record[i]) << 24 | uint32_t(record[i + 1]) << 16 |
                                         uint32_t(record[i + 2]) << 8 | record[i + 3]);
}
float RecordFloat(const std::array<uint8_t, kLightSize>& record, size_t offset) {
    const uint32_t bits = uint32_t(record[offset]) << 24 | uint32_t(record[offset + 1]) << 16 |
                          uint32_t(record[offset + 2]) << 8 | record[offset + 3];
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Every call: which area lights are new, which left. `steps` game frames (0 for a further call in the
// same frame) move each one's weight.
void Advance(FadeState& state, uint32_t actorLights, uint32_t count, bool fresh, uint32_t steps) {
    const float step = kFadeStep * static_cast<float>(steps);
    const float follow = 1.0f - std::pow(0.85f, static_cast<float>(steps));
    std::array<std::array<uint8_t, kLightSize>, 4> current{};
    for (uint32_t i = 0; i < count; ++i) ReadRecord(actorLights + 4 + i * kLightSize, current[i]);
    for (uint32_t k = 0; k < state.count; ++k) state.lights[k].present = false;
    for (uint32_t i = 0; i < count; ++i) {
        const float p[3] = {RecordFloat(current[i], 0), RecordFloat(current[i], 4), RecordFloat(current[i], 8)};
        FadeLight* match = nullptr;
        for (uint32_t k = 0; k < state.count && match == nullptr; ++k) {
            const FadeLight& f = state.lights[k];
            if (!f.present && std::abs(f.pos[0] - p[0]) < 1e-3f && std::abs(f.pos[1] - p[1]) < 1e-3f &&
                std::abs(f.pos[2] - p[2]) < 1e-3f)
                match = &state.lights[k];
        }
        if (match == nullptr) {
            // A light that moves (it follows a ship or a platform) is the same light, not one that left
            // and one that arrived: taken for new every frame, it never got past the first tenth of its
            // brightness and the actor stayed dark while it moved. The nearest light of the same type
            // not matched yet, within reach, keeps its weight.
            const float reach = kMovedLightReach * static_cast<float>(std::max(steps, 1u));
            float bestD = reach * reach;
            for (uint32_t k = 0; k < state.count; ++k) {
                FadeLight& f = state.lights[k];
                if (f.present || std::memcmp(f.record.data() + 0x20, current[i].data() + 0x20, 4) != 0) continue;
                const float dx = f.pos[0] - p[0], dy = f.pos[1] - p[1], dz = f.pos[2] - p[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d < bestD) {
                    bestD = d;
                    match = &f;
                }
            }
            if (match != nullptr) std::memcpy(match->pos, p, sizeof(p));
        }
        if (match == nullptr) {
            if (state.count < state.lights.size()) {
                match = &state.lights[state.count++];
            } else {  // full: replace the most faded light that is not in this frame's list
                for (uint32_t k = 0; k < state.count; ++k) {
                    FadeLight& f = state.lights[k];
                    if (!f.present && (match == nullptr || f.weight < match->weight)) match = &f;
                }
                if (match == nullptr) continue;  // cannot happen: 4 lights at most, 8 entries
            }
            match->weight = fresh ? 1.0f : 0.0f;
            std::memcpy(match->pos, p, sizeof(p));
        }
        match->record = current[i];
        match->present = true;
    }
    // A light that left fades out in a free GX slot of the actor. With no slot for every one of them
    // (the engine gave the actor four lights and changed some), a cross-fade is not possible: the new
    // lights would rise from zero with the old ones already gone, a dip in brightness at every change
    // (an actor crossing a room changes lights all the time: it flickered darker). The engine's own
    // behaviour is kept then: the lights switch at once.
    uint32_t leaving = 0;
    for (uint32_t k = 0; k < state.count; ++k) leaving += !state.lights[k].present && state.lights[k].weight > 0.0f;
    if (leaving > 4u - std::min(count, 4u)) {
        for (uint32_t k = 0; k < state.count; ++k) state.lights[k].weight = state.lights[k].present ? 1.0f : 0.0f;
    }
    // Fade, and drop the lights that have gone.
    uint32_t kept = 0;
    for (uint32_t k = 0; k < state.count; ++k) {
        FadeLight f = state.lights[k];
        f.weight = std::clamp(f.weight + (f.present ? step : -step), 0.0f, 1.0f);
        if (!f.present && f.weight <= 0.0f) continue;
        state.lights[kept++] = f;
    }
    state.count = kept;
    // The area ambient colour (+0x2B8) follows smoothly too.
    const uint32_t ambient = Memory::Read32(actorLights + 0x2B8);
    for (int c = 0; c < 4; ++c) {
        const float target = static_cast<float>((ambient >> (24 - 8 * c)) & 0xFF);
        state.ambient[c] = fresh ? target : state.ambient[c] + (target - state.ambient[c]) * follow;
    }
}

uint32_t ScaledColor(const std::array<uint8_t, kLightSize>& record, float weight) {
    const auto scale = [&](size_t i) { return uint32_t(std::lround(record[0x1C + i] * weight)); };
    return scale(0) << 24 | scale(1) << 16 | scale(2) << 8 | record[0x1F];
}
}  // namespace

extern "C" void PrimeEngineLightsAfterWorldLights(uint32_t self) {
    if (!g_send.load(std::memory_order_relaxed)) return;
    std::vector<AuroraEngineLight> lights;
    lights.reserve(160);
    // Dynamic world lights of this frame (sorted by the engine; the world pieces take theirs from it).
    AppendVector(self + 0x5D8, true, 64, lights);
    // Static lights (light set 0) of every loaded area: baked into the lightmaps.
    const uint32_t manager = Deref(self + 4);
    const uint32_t world = Deref(Deref(manager + 0x28) + 0x2134);
    if (Valid(world + 0x30) && Valid(world + 0x38)) {
        const uint32_t areaCount = std::min<uint32_t>(Memory::Read32(world + 0x30), 64);
        const uint32_t areas = Memory::Read32(world + 0x38);
        for (uint32_t a = 0; a < areaCount && lights.size() < 384; ++a) {
            const uint32_t area = Deref(areas + a * 8 + 4);
            if (!Valid(area + 0xB4) || Memory::Read32(area + 0x98) != 0x11) continue;
            const uint32_t post = Deref(area + 0xB4);
            if (post != 0) AppendVector(post + 0xA8, false, 256, lights);
        }
    }
    aurora_set_engine_lights(lights.data(), static_cast<uint32_t>(lights.size()));
    static bool logged = false;
    if (!logged && !lights.empty()) {
        logged = true;
        size_t dynamic = 0;
        for (const auto& l : lights) dynamic += (l.flags & AURORA_ENGINE_LIGHT_DYNAMIC) != 0;
        RT_LOG(RT_TAG_RUNTIME) << "[prime-engine-lights] first list: " << lights.size() << " lights (" << dynamic
                               << " dynamic)" << std::endl;
    }
}

extern "C" uint32_t PrimeActorLightsBegin(uint32_t actorLights) {
    if (!g_fade.load(std::memory_order_relaxed) || g_savedDepth >= g_savedStack.size() ||
        !Valid(actorLights, 0x318))
        return 0;
    const uint32_t count = Memory::Read32(actorLights);
    if (count > 4) return 0;
    // A block this hook has already rewritten (a nested call before its restore) holds faded colours:
    // it is not a list to compare against.
    for (uint32_t d = 0; d < g_savedDepth; ++d)
        if (g_savedStack[d].actorLights == actorLights) return 0;
    const int64_t now = NowMicros();
    // Forget actors that have not been lit for two seconds (before taking a reference into the map).
    if (now - g_fadeSweepMicros > 10'000'000) {
        g_fadeSweepMicros = now;
        for (auto it = g_fadeStates.begin(); it != g_fadeStates.end();)
            it = now - it->second.lastMicros > 2'000'000 ? g_fadeStates.erase(it) : std::next(it);
    }
    auto& state = g_fadeStates[actorLights];
    {
        // Game frames since this actor's last step, rounded to the nearest: 0 for another call in the
        // same frame (the list is still compared, a light that is new then starts at zero).
        const bool first = state.lastMicros < 0;
        const uint32_t steps = first ? 1u : static_cast<uint32_t>(std::min<int64_t>(
                                                (now - state.lastMicros + kStepMicros / 2) / kStepMicros, 1000));
        // An actor not lit in the last frames starts at full weight (no fade-in when it appears).
        const bool fresh = first || steps > 3;
        if (fresh) state.count = 0;  // nothing left to fade out from that long ago
        Advance(state, actorLights, count, fresh, fresh ? 1u : steps);
        if (first || steps > 0) state.lastMicros = now;
    }
    bool changes = false;
    for (uint32_t k = 0; k < state.count; ++k) changes = changes || state.lights[k].weight < 1.0f || !state.lights[k].present;
    const uint32_t ambient = Memory::Read32(actorLights + 0x2B8);
    uint32_t smoothAmbient = 0;
    for (int c = 0; c < 4; ++c) smoothAmbient |= uint32_t(std::lround(std::clamp(state.ambient[c], 0.0f, 255.0f))) << (24 - 8 * c);
    changes = changes || smoothAmbient != ambient;
    if (!changes) return 0;
    Saved& saved = g_savedStack[g_savedDepth];
    saved.actorLights = actorLights;
    saved.count = count;
    saved.ambient = ambient;
    for (uint32_t i = 0; i < 4; ++i) ReadRecord(actorLights + 4 + i * kLightSize, saved.records[i]);
    // Lights still chosen: their colour scaled by their weight (fading in). Lights that left: put back
    // in a free slot with their colour scaled (fading out).
    uint32_t slots = count;
    for (uint32_t k = 0; k < state.count; ++k) {
        const FadeLight& f = state.lights[k];
        if (f.present) {
            for (uint32_t i = 0; i < count; ++i) {
                if (saved.records[i] != f.record) continue;  // the whole record: two lights can share a place
                if (f.weight < 1.0f) Memory::Write32(actorLights + 4 + i * kLightSize + 0x1C, ScaledColor(f.record, f.weight));
                break;
            }
        } else if (slots < 4) {
            const uint32_t at = actorLights + 4 + slots * kLightSize;
            WriteRecord(at, f.record);
            Memory::Write32(at + 0x1C, ScaledColor(f.record, f.weight));
            ++slots;
        }
    }
    if (slots != count) Memory::Write32(actorLights, slots);
    Memory::Write32(actorLights + 0x2B8, smoothAmbient);
    return ++g_savedDepth;
}

extern "C" void PrimeActorLightsEnd(uint32_t token) {
    if (token == 0 || token != g_savedDepth) return;
    const Saved& saved = g_savedStack[--g_savedDepth];
    for (uint32_t i = 0; i < 4; ++i) WriteRecord(saved.actorLights + 4 + i * kLightSize, saved.records[i]);
    Memory::Write32(saved.actorLights, saved.count);
    Memory::Write32(saved.actorLights + 0x2B8, saved.ambient);
}
#endif
