#pragma once
// Text format of a QA input recording (recomp/qa/README.md). Pure: no SDL, no logging, no globals, so
// tests/prime_input_recording_tests.cpp covers it and the replay module only adds the plumbing.
//
//   # comment
//   f <frame> h <hold hex> s <stickX,stickY> p <valid,x,y,dist,ddist> r <roll,pitch,accelZ> n <roll,pitch> g <6 gesture values>
//   m <frame> <dx,dy>        mouse-look motion of that frame, pixels
//   a <frame> <x,y,z>        Samus's world position on that frame (CPlayer transform origin)
//   end <frame>              one past the last recorded frame
// A keyframe holds from its frame until the next keyframe; frames are counted from the moment the
// recording started (see the replay module for how both sides align on the front end).
#include "hle/prime_keyboard_mouse.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

namespace PrimeInputRecording {

struct Keyframe {
    uint32_t frame = 0;
    PrimeKeyboardMouse::Sample sample;
};
struct Motion {
    uint32_t frame = 0;
    float dx = 0, dy = 0;
};
struct Anchor {
    uint32_t frame = 0;
    float x = 0, y = 0, z = 0;
};
struct Recording {
    std::vector<Keyframe> keyframes;
    std::vector<Motion> motions;
    std::vector<Anchor> anchors;
    uint32_t endFrame = 0;
    size_t badLines = 0;
};

inline bool SameSample(const PrimeKeyboardMouse::Sample& a, const PrimeKeyboardMouse::Sample& b) {
    return a.hold == b.hold && a.stickX == b.stickX && a.stickY == b.stickY && a.pointerX == b.pointerX &&
           a.pointerY == b.pointerY && a.remoteAccelZ == b.remoteAccelZ && a.pointerDistance == b.pointerDistance &&
           a.pointerDistanceDelta == b.pointerDistanceDelta && a.roll == b.roll && a.remotePitch == b.remotePitch &&
           a.nunchukRoll == b.nunchukRoll && a.nunchukPitch == b.nunchukPitch && a.remoteGesture == b.remoteGesture &&
           a.nunchukGesture == b.nunchukGesture && a.pointerValid == b.pointerValid;
}

inline std::string Header() {
    return "# prime-input-recording v1\n"
           "# f <frame> h <hold> s <stickX,stickY> p <valid,x,y,dist,ddist> r <roll,pitch,accelZ> n <roll,pitch> g <6 gesture g>\n"
           "# m <frame> <mouse-look dx,dy pixels>   end <frame>\n";
}

inline std::string FormatKeyframe(uint32_t frame, const PrimeKeyboardMouse::Sample& s) {
    std::ostringstream out;
    out.precision(9);  // floats survive the round trip exactly
    out << "f " << frame << " h 0x" << std::hex << s.hold << std::dec
        << " s " << s.stickX << ',' << s.stickY
        << " p " << int(s.pointerValid) << ',' << s.pointerX << ',' << s.pointerY << ',' << s.pointerDistance << ',' << s.pointerDistanceDelta
        << " r " << s.roll << ',' << s.remotePitch << ',' << s.remoteAccelZ
        << " n " << s.nunchukRoll << ',' << s.nunchukPitch
        << " g " << s.remoteGesture[0] << ',' << s.remoteGesture[1] << ',' << s.remoteGesture[2] << ','
        << s.nunchukGesture[0] << ',' << s.nunchukGesture[1] << ',' << s.nunchukGesture[2] << '\n';
    return out.str();
}

inline std::string FormatMotion(uint32_t frame, float dx, float dy) {
    std::ostringstream out;
    out.precision(9);
    out << "m " << frame << ' ' << dx << ',' << dy << '\n';
    return out.str();
}

inline std::string FormatAnchor(uint32_t frame, float x, float y, float z) {
    std::ostringstream out;
    out.precision(9);
    out << "a " << frame << ' ' << x << ',' << y << ',' << z << '\n';
    return out.str();
}

inline std::string FormatEnd(uint32_t frame) { return "end " + std::to_string(frame) + "\n"; }

namespace detail {
inline bool ParseFloats(const std::string& text, float* out, size_t count) {
    std::stringstream ss(text);
    for (size_t i = 0; i < count; ++i) {
        if (!(ss >> out[i])) return false;
        if (i + 1 < count) {
            char comma = 0;
            if (!(ss >> comma) || comma != ',') return false;
        }
    }
    return true;
}
}  // namespace detail

// Adds one line to `rec`; comments and blank lines are ignored, unreadable ones counted.
inline void ParseLine(const std::string& line, Recording& rec) {
    if (line.empty() || line[0] == '#') return;
    std::stringstream ss(line);
    std::string kind;
    uint32_t frame = 0;
    if (!(ss >> kind >> frame)) { ++rec.badLines; return; }
    if (kind == "end") { rec.endFrame = frame; return; }
    if (kind == "m") {
        std::string v;
        float xy[2]{};
        if (ss >> v && detail::ParseFloats(v, xy, 2)) rec.motions.push_back({frame, xy[0], xy[1]});
        else ++rec.badLines;
        return;
    }
    if (kind == "a") {
        std::string v;
        float xyz[3]{};
        if (ss >> v && detail::ParseFloats(v, xyz, 3)) rec.anchors.push_back({frame, xyz[0], xyz[1], xyz[2]});
        else ++rec.badLines;
        return;
    }
    if (kind != "f") { ++rec.badLines; return; }
    Keyframe k;
    k.frame = frame;
    auto& s = k.sample;
    std::string tag, v;
    bool ok = true;
    while (ss >> tag >> v) {
        float f[6]{};
        if (tag == "h") s.hold = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
        else if (tag == "s" && detail::ParseFloats(v, f, 2)) { s.stickX = f[0]; s.stickY = f[1]; }
        else if (tag == "p" && detail::ParseFloats(v, f, 5)) { s.pointerValid = f[0] != 0; s.pointerX = f[1]; s.pointerY = f[2]; s.pointerDistance = f[3]; s.pointerDistanceDelta = f[4]; }
        else if (tag == "r" && detail::ParseFloats(v, f, 3)) { s.roll = f[0]; s.remotePitch = f[1]; s.remoteAccelZ = f[2]; }
        else if (tag == "n" && detail::ParseFloats(v, f, 2)) { s.nunchukRoll = f[0]; s.nunchukPitch = f[1]; }
        else if (tag == "g" && detail::ParseFloats(v, f, 6)) { s.remoteGesture = {f[0], f[1], f[2]}; s.nunchukGesture = {f[3], f[4], f[5]}; }
        else { ok = false; break; }
    }
    if (ok) rec.keyframes.push_back(k); else ++rec.badLines;
}

inline Recording Parse(std::istream& in) {
    Recording rec;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ParseLine(line, rec);
    }
    if (rec.endFrame == 0 && !rec.keyframes.empty()) rec.endFrame = rec.keyframes.back().frame + 1;
    return rec;
}

// Plays a recording back one frame at a time.
class Player {
public:
    explicit Player(Recording rec) : m_rec(std::move(rec)) {}
    bool Finished() const { return m_frame >= m_rec.endFrame; }
    uint32_t Frame() const { return m_frame; }
    size_t AnchorCount() const { return m_rec.anchors.size(); }
    // The anchor recorded on the frame Step() is about to play, if any (consumed in order).
    bool AnchorForCurrentFrame(Anchor& out) {
        while (m_nextAnchor < m_rec.anchors.size() && m_rec.anchors[m_nextAnchor].frame < m_frame) ++m_nextAnchor;
        if (m_nextAnchor < m_rec.anchors.size() && m_rec.anchors[m_nextAnchor].frame == m_frame) {
            out = m_rec.anchors[m_nextAnchor++];
            return true;
        }
        return false;
    }
    // Input and motion for the current frame, then advances.
    void Step(PrimeKeyboardMouse::Sample& input, float& dx, float& dy) {
        dx = dy = 0;
        if (Finished()) { input = {}; return; }
        while (m_nextKeyframe < m_rec.keyframes.size() && m_rec.keyframes[m_nextKeyframe].frame <= m_frame) {
            m_current = m_rec.keyframes[m_nextKeyframe].sample;
            ++m_nextKeyframe;
        }
        input = m_current;
        while (m_nextMotion < m_rec.motions.size() && m_rec.motions[m_nextMotion].frame <= m_frame) {
            if (m_rec.motions[m_nextMotion].frame == m_frame) { dx += m_rec.motions[m_nextMotion].dx; dy += m_rec.motions[m_nextMotion].dy; }
            ++m_nextMotion;
        }
        ++m_frame;
    }

private:
    Recording m_rec;
    PrimeKeyboardMouse::Sample m_current;
    size_t m_nextKeyframe = 0, m_nextMotion = 0, m_nextAnchor = 0;
    uint32_t m_frame = 0;
};

}  // namespace PrimeInputRecording
