// QA input recording and replay. See include/hle/prime_input_replay.h and recomp/qa/README.md.
#if MKW_PRIME_QA_TOOLS
#include "hle/prime_input_replay.h"
#include "hle/prime_input_recording.h"

#include "runtime_log.h"
#include "memory.h"
#include "hle/prime_mouse_look.h"

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include "post_effects.h"
#include "runtime_config.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <SDL3/SDL_events.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// RM3E01 object layout used by the anchors, verified live on 2026-10-26 (recomp/qa/README.md):
// CPlayer transform origin at +0x48/+0x58/+0x68 (rows of the 3x4 transform at +0x3C) with a mirror at
// +0x6C/+0x70/+0x74; CPlayerState pointer at CPlayer+0x35A0; health at CPlayerState+0xC, capacity at +0x8.
constexpr uint32_t kPlayerPosX = 0x48, kPlayerPosY = 0x58, kPlayerPosZ = 0x68, kPlayerPrevPos = 0x6C;
constexpr uint32_t kPlayerStatePtr = 0x35A0, kStateHealth = 0xC, kStateCapacity = 0x8;
constexpr uint32_t kAnchorEveryFrames = 30;
constexpr float kDriftMetres = 0.5f;

static bool GuestAddressOk(uint32_t a) { return a >= 0x80000000u && a < 0x81800000u - 0x4000u && (a & 3u) == 0; }
static float GuestFloat(uint32_t a) { const uint32_t bits = Memory::Read32(a); float f; std::memcpy(&f, &bits, 4); return f; }

// Lets recomp/tools/package_release.ps1 refuse an executable built with the QA tools.
extern "C" const char kPrimeQaToolsMarker[] = "MKW_PRIME_QA_TOOLS_ENABLED_BUILD";
bool PrimeFrontEndActive();  // prime_rso_runtime.cpp: title / file-select menus loaded

namespace PrimeInputReplay {
namespace {
namespace fs = std::filesystem;
using namespace PrimeInputRecording;

// Both sides count frames from the front end: a recording may only start while the title or the
// menus are up (what the saved game on disk reproduces), and a replay starts its frame 0 the first
// time the front end is active after launch.
// Frames are numbered on one clock shared by recorder and player: 0 is the first KPAD read with the
// front end active after launch. A recording started later carries that offset in its frame numbers,
// so a replay (which starts at 0) presses the same keys on the same front-end frame.
struct Recorder {
    std::ofstream out;
    std::string path;
    uint32_t firstFrame = 0, lastFrame = 0;
    PrimeKeyboardMouse::Sample last;
    bool haveLast = false;

    bool Begin(const std::string& target, uint32_t frame) {
        out.open(target, std::ios::out | std::ios::trunc);
        if (!out) return false;
        path = target;
        out << Header();
        firstFrame = lastFrame = frame;
        haveLast = false;
        return true;
    }
    void Observe(uint32_t frame, const PrimeKeyboardMouse::Sample& input, float dx, float dy) {
        if (!haveLast || !SameSample(input, last)) {
            out << FormatKeyframe(frame, input);
            last = input;
            haveLast = true;
        }
        if (dx != 0.0f || dy != 0.0f) out << FormatMotion(frame, dx, dy);
        if ((frame % kAnchorEveryFrames) == 0) {
            const uint32_t player = PrimeMouseLook::LastPlayer();
            if (GuestAddressOk(player)) out << FormatAnchor(frame, GuestFloat(player + kPlayerPosX), GuestFloat(player + kPlayerPosY), GuestFloat(player + kPlayerPosZ));
        }
        if ((frame % 600) == 0) out.flush();  // every 10 s, so a crash keeps most of it
        lastFrame = frame;
    }
    uint32_t Frames() const { return lastFrame + 1 - firstFrame; }
    void End() {
        out << FormatEnd(lastFrame + 1);
        out.close();
    }
};

struct State {
    bool initialized = false;
    std::unique_ptr<Recorder> recorder;
    std::unique_ptr<Player> player;
    bool frontEndSeen = false;
    uint32_t frontEndFrame = 0;   // the shared clock
    bool replayFinished = false;
    int exitDelayFrames = -1;     // >= 0: quit this many frames after the replay ends
    // Replay drift handling (recomp/qa/README.md): anchors compared, corrections applied, health kept full.
    bool correct = true, god = true;
    size_t anchorsChecked = 0, driftEvents = 0, corrections = 0;
    float maxDrift = 0;
    fs::path controlDir;
    uint32_t pollCounter = 0;
};
State g_state;
std::mutex g_mutex;

void WriteStatus(const std::string& text) {
    if (g_state.controlDir.empty()) return;
    std::ofstream status(g_state.controlDir / "status.txt", std::ios::trunc);
    status << text << '\n';
}

void StartRecording(const std::string& path) {
    if (!PrimeFrontEndActive()) {
        RT_LOG(RT_TAG_RUNTIME) << "[qa-record] refused: recordings start at the title or the menus (the saved game is the replay's starting point)" << std::endl;
        WriteStatus("refused: go back to the title or the menus, then start again");
        return;
    }
    if (g_state.recorder) { g_state.recorder->End(); g_state.recorder.reset(); }
    auto recorder = std::make_unique<Recorder>();
    if (!recorder->Begin(path, g_state.frontEndFrame)) {
        RT_LOG(RT_TAG_RUNTIME) << "[qa-record] cannot write " << path << std::endl;
        WriteStatus("error: cannot write " + path);
        return;
    }
    g_state.recorder = std::move(recorder);
    RT_LOG(RT_TAG_RUNTIME) << "[qa-record] recording inputs to " << path << std::endl;
    WriteStatus("recording " + path);
}

void StopRecording() {
    if (!g_state.recorder) { WriteStatus("idle"); return; }
    const uint32_t frames = g_state.recorder->Frames();
    g_state.recorder->End();
    RT_LOG(RT_TAG_RUNTIME) << "[qa-record] stopped after " << frames << " frames: " << g_state.recorder->path << std::endl;
    WriteStatus("stopped after " + std::to_string(frames) + " frames: " + g_state.recorder->path);
    g_state.recorder.reset();
}

// Control files written by recomp/qa/qa_record.ps1 while the game runs.
void PollControl() {
    if (g_state.controlDir.empty()) return;
    if ((++g_state.pollCounter % 30) != 0) return;  // twice a second
    std::error_code ec;
    const fs::path start = g_state.controlDir / "start.txt";
    const fs::path stop = g_state.controlDir / "stop.txt";
    if (fs::exists(start, ec)) {
        std::string path;
        { std::ifstream in(start); std::getline(in, path); }
        fs::remove(start, ec);
        if (!path.empty() && path.back() == '\r') path.pop_back();
        if (!path.empty()) StartRecording(path);
    }
    if (fs::exists(stop, ec)) {
        fs::remove(stop, ec);
        StopRecording();
    }
}

void Initialize() {
    g_state.initialized = true;
    if (const char* dir = std::getenv("WII_COMPILED_QA_CONTROL_DIR"); dir && *dir) {
        g_state.controlDir = dir;
        std::error_code ec;
        fs::create_directories(g_state.controlDir, ec);
        WriteStatus("idle");
        RT_LOG(RT_TAG_RUNTIME) << "[qa-record] control directory " << dir << std::endl;
    }
    if (const char* path = std::getenv("WII_COMPILED_QA_RECORD"); path && *path) {
        // Immediate recording from launch: frame 0 is the first front-end frame (see Apply).
        g_state.recorder = std::make_unique<Recorder>();
        if (!g_state.recorder->Begin(path, 0)) { g_state.recorder.reset(); RT_LOG(RT_TAG_RUNTIME) << "[qa-record] cannot write " << path << std::endl; }
        else RT_LOG(RT_TAG_RUNTIME) << "[qa-record] recording inputs to " << path << " from the front end on" << std::endl;
    }
    if (const char* path = std::getenv("WII_COMPILED_QA_REPLAY"); path && *path) {
        std::ifstream in(path);
        if (!in) { RT_LOG(RT_TAG_RUNTIME) << "[qa-replay] cannot open " << path << std::endl; return; }
        PrimeInputRecording::Recording rec = Parse(in);
        RT_LOG(RT_TAG_RUNTIME) << "[qa-replay] loaded " << path << ": " << rec.keyframes.size() << " keyframes, " << rec.motions.size()
                               << " motion frames, ends at frame " << rec.endFrame << (rec.badLines ? " (" + std::to_string(rec.badLines) + " unreadable lines)" : "") << std::endl;
        if (!rec.keyframes.empty()) {
            g_state.player = std::make_unique<Player>(std::move(rec));
            // WII_COMPILED_QA_REPLAY_EXIT=<seconds>: quit that long after the last recorded frame (default 3 s),
            // enough for the driver script to take its final screenshot.
            if (const char* exitAfter = std::getenv("WII_COMPILED_QA_REPLAY_EXIT")) {
                const int seconds = std::atoi(exitAfter);
                g_state.exitDelayFrames = (seconds > 0 ? seconds : 3) * 60;
            }
            g_state.correct = std::getenv("WII_COMPILED_QA_NO_CORRECT") == nullptr;
            g_state.god = std::getenv("WII_COMPILED_QA_NO_GOD") == nullptr;
            RT_LOG(RT_TAG_RUNTIME) << "[qa-replay] anchors: " << g_state.player->AnchorCount() << ", position correction "
                                   << (g_state.correct ? "on" : "off") << ", full health " << (g_state.god ? "on" : "off") << std::endl;
        }
    }
}

}  // namespace

void Apply(PrimeKeyboardMouse::Sample& input, float& lookDx, float& lookDy) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.initialized) Initialize();
    PollControl();
    if (!g_state.frontEndSeen) {
        if (!PrimeFrontEndActive()) { if (g_state.player) { input = {}; lookDx = lookDy = 0; } return; }
        g_state.frontEndSeen = true;
        RT_LOG(RT_TAG_RUNTIME) << "[qa] front end reached: frame 0" << std::endl;
    } else {
        ++g_state.frontEndFrame;
    }
    if (g_state.recorder) g_state.recorder->Observe(g_state.frontEndFrame, input, lookDx, lookDy);
    if (g_state.player && !g_state.replayFinished) {
        const uint32_t player = PrimeMouseLook::LastPlayer();
        if (GuestAddressOk(player)) {
            // Enemies push Samus around and knock her health down; a performance tour must reach the same
            // rooms regardless, so the recorded path wins: at every anchor the position is checked and, past
            // half a metre of drift, written back. Health is topped up to the capacity every frame.
            Anchor anchor;
            if (g_state.player->AnchorForCurrentFrame(anchor)) {
                const float x = GuestFloat(player + kPlayerPosX), y = GuestFloat(player + kPlayerPosY), z = GuestFloat(player + kPlayerPosZ);
                const float d = std::sqrt((x - anchor.x) * (x - anchor.x) + (y - anchor.y) * (y - anchor.y) + (z - anchor.z) * (z - anchor.z));
                ++g_state.anchorsChecked;
                if (std::isfinite(d) && d > kDriftMetres) {
                    ++g_state.driftEvents;
                    g_state.maxDrift = std::max(g_state.maxDrift, d);
                    if (g_state.driftEvents <= 20) RT_LOG(RT_TAG_RUNTIME) << "[qa-replay] drift at frame " << g_state.player->Frame() << ": " << d << " m" << (g_state.correct ? " (corrected)" : "") << std::endl;
                    if (g_state.correct) {
                        Memory::WriteFloat32(player + kPlayerPosX, anchor.x);
                        Memory::WriteFloat32(player + kPlayerPosY, anchor.y);
                        Memory::WriteFloat32(player + kPlayerPosZ, anchor.z);
                        Memory::WriteFloat32(player + kPlayerPrevPos, anchor.x);
                        Memory::WriteFloat32(player + kPlayerPrevPos + 4, anchor.y);
                        Memory::WriteFloat32(player + kPlayerPrevPos + 8, anchor.z);
                        ++g_state.corrections;
                    }
                }
            }
            if (g_state.god) {
                const uint32_t state = Memory::Read32(player + kPlayerStatePtr);
                if (GuestAddressOk(state)) {
                    const float capacity = GuestFloat(state + kStateCapacity), health = GuestFloat(state + kStateHealth);
                    if (std::isfinite(capacity) && capacity > 0.0f && capacity < 10000.0f && health < capacity) Memory::WriteFloat32(state + kStateHealth, capacity);
                }
            }
        }
        g_state.player->Step(input, lookDx, lookDy);
        if ((g_state.player->Frame() % 3600) == 0) RT_LOG(RT_TAG_RUNTIME) << "[qa-replay] frame " << g_state.player->Frame() << std::endl;
        // WII_COMPILED_QA_DUMP_EVERY=<frames> with WII_COMPILED_QA_DUMP_DIR=<dir>: an F7 draw dump at
        // exact replay frames (<dir>/frame_<n>), so two runs of one recording can be compared image by
        // image. The "request" file is polled twice a second and lands on different frames each run.
        {
            static const long every = [] { const char* v = std::getenv("WII_COMPILED_QA_DUMP_EVERY"); return v ? std::atol(v) : 0L; }();
            static const std::string dir = [] { const char* v = std::getenv("WII_COMPILED_QA_DUMP_DIR"); return std::string(v ? v : ""); }();
            const long frame = static_cast<long>(g_state.player->Frame());
            // WII_COMPILED_QA_DUMP_AB=1: the same moment twice, as configured (frame_<n>_on) and four
            // frames later with the lighting work of 13/11/2026 off (frame_<n>_off: environment and lit-room
            // materials, level 2 materials and the bounce light), then the settings are restored. The camera
            // drifts between two runs of a recording; four frames apart it barely moves.
            static const bool ab = [] { const char* v = std::getenv("WII_COMPILED_QA_DUMP_AB"); return v && *v == '1'; }();
            // WII_COMPILED_QA_BURST_AT=<f1>,<f2>,...: an F5-style burst (60 consecutive frames) at those replay
            // frames into <dir>/burst_<f>, to look for flicker in the same place every run.
            static const std::vector<long> bursts = [] {
                std::vector<long> out;
                if (const char* v = std::getenv("WII_COMPILED_QA_BURST_AT")) {
                    std::string list(v);
                    size_t at = 0;
                    while (at < list.size()) {
                        const size_t comma = list.find(',', at);
                        out.push_back(std::atol(list.substr(at, comma - at).c_str()));
                        if (comma == std::string::npos) break;
                        at = comma + 1;
                    }
                }
                return out;
            }();
            if (!dir.empty())
                for (const long b : bursts)
                    if (frame == b) {
                        std::error_code error;
                        std::filesystem::create_directories(dir, error);
                        aurora_request_draw_dump_burst((dir + "/burst_" + std::to_string(frame)).c_str(), 60);
                    }
            if (every > 0 && !dir.empty() && frame > 0) {
                const long phase = frame % every;
                const long base = frame - phase;
                std::error_code error;
                if (phase == 0) {
                    std::filesystem::create_directories(dir, error);
                    aurora_request_draw_dump((dir + "/frame_" + std::to_string(frame) + (ab ? "_on" : "")).c_str());
                } else if (ab && phase == 2) {
                    const auto& c = RuntimeConfigFile::Get();
                    aurora_set_environment_lighting(false, c.effectsEnvLights.value_or(true));
                    AuroraLightingFeatures f{};
                    f.litRooms = false;
                    f.softHighlights = c.effectsSoftHighlights.value_or(true);
                    f.linearBlend = c.effectsLinearLighting.value_or(false);
                    f.smoothLightmaps = c.effectsSmoothLightmaps.value_or(true);
                    f.areaReflections = c.effectsAreaReflections.value_or(false);
                    f.softShadows = c.effectsSoftShadows.value_or(true);
                    aurora_set_lighting_features(&f);
                    AuroraModernLighting2 off{};
                    aurora_set_modern_lighting2(&off);
                } else if (ab && phase == 4) {
                    aurora_request_draw_dump((dir + "/frame_" + std::to_string(base) + "_off").c_str());
                } else if (ab && phase == 6) {
                    PostEffects::Apply();
                }
            }
        }
        if (g_state.player->Finished()) {
            g_state.replayFinished = true;
            RT_LOG(RT_TAG_RUNTIME) << "[qa-replay] finished at frame " << g_state.player->Frame() << std::endl;
            RT_LOG(RT_TAG_RUNTIME) << "[qa-replay] anchors_checked=" << g_state.anchorsChecked << " drift_events=" << g_state.driftEvents
                                   << " max_drift_m=" << g_state.maxDrift << " corrections=" << g_state.corrections << std::endl;
        }
    } else if (g_state.player) {
        input = {};
        lookDx = lookDy = 0;
        if (g_state.exitDelayFrames >= 0 && g_state.exitDelayFrames-- == 0) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
    }
}

bool Replaying() { std::lock_guard<std::mutex> lock(g_mutex); return g_state.player != nullptr; }
bool Recording() { std::lock_guard<std::mutex> lock(g_mutex); return g_state.recorder != nullptr; }

}  // namespace PrimeInputReplay
#endif
