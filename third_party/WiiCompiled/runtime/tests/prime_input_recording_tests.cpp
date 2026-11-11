#include "hle/prime_input_recording.h"

#include <iostream>
#include <sstream>

namespace {
bool Require(bool value, const char* message) {
    if (!value) std::cerr << message << std::endl;
    return value;
}
}  // namespace

int main() {
    using namespace PrimeInputRecording;
    PrimeKeyboardMouse::Sample a;
    a.hold = 0x0800u | 0x2000u;
    a.stickX = -0.25f; a.stickY = 1.0f;
    a.pointerValid = true; a.pointerX = 0.125f; a.pointerY = -0.5f; a.pointerDistance = 1.5f; a.pointerDistanceDelta = -0.0625f;
    a.roll = 0.7853982f; a.remotePitch = -0.1f; a.remoteAccelZ = 0.3f;
    a.nunchukRoll = 0.2f; a.nunchukPitch = -0.3f;
    a.remoteGesture = {1.0f, -2.0f, 0.5f};
    a.nunchukGesture = {0.0f, 0.25f, -0.125f};
    PrimeKeyboardMouse::Sample b = a;
    b.hold = 0;
    b.pointerValid = false;

    // Serialise -> parse -> identical samples, motion and end.
    std::string text = Header() + FormatKeyframe(0, a) + FormatMotion(3, 4.5f, -2.25f) + FormatKeyframe(10, b) + FormatEnd(20);
    std::istringstream in(text);
    Recording rec = Parse(in);
    if (!Require(rec.badLines == 0, "well-formed recording reported bad lines")) return 1;
    if (!Require(rec.keyframes.size() == 2 && rec.motions.size() == 1 && rec.endFrame == 20, "counts")) return 1;
    if (!Require(SameSample(rec.keyframes[0].sample, a) && rec.keyframes[0].frame == 0, "keyframe 0 did not round-trip")) return 1;
    if (!Require(SameSample(rec.keyframes[1].sample, b) && rec.keyframes[1].frame == 10, "keyframe 10 did not round-trip")) return 1;
    if (!Require(rec.motions[0].frame == 3 && rec.motions[0].dx == 4.5f && rec.motions[0].dy == -2.25f, "motion did not round-trip")) return 1;

    // Playback: a keyframe holds until the next, motion only on its frame, finished past end.
    Player player(rec);
    PrimeKeyboardMouse::Sample s; float dx = 0, dy = 0;
    for (uint32_t f = 0; f < 20; ++f) {
        player.Step(s, dx, dy);
        const bool expectA = f < 10;
        if (!Require(SameSample(s, expectA ? a : b), "held keyframe wrong")) return 1;
        if (!Require((f == 3) == (dx == 4.5f && dy == -2.25f) && (f == 3 || (dx == 0 && dy == 0)), "motion on the wrong frame")) return 1;
    }
    if (!Require(player.Finished(), "player should be finished at end")) return 1;
    player.Step(s, dx, dy);
    if (!Require(s.hold == 0 && !s.pointerValid && dx == 0, "finished player must yield neutral input")) return 1;

    // Robustness: comments, CRLF, junk and a missing end line.
    std::istringstream messy("# c\r\n" + FormatKeyframe(5, a) + "x 1 2\r\nf 7 h zz\r\nm 9 1,2\r\n");
    Recording r2 = Parse(messy);
    if (!Require(r2.badLines == 1 && r2.keyframes.size() == 2 && r2.motions.size() == 1, "junk handling")) return 1;
    if (!Require(r2.keyframes[1].sample.hold == 0 && r2.endFrame == 8, "strtoul tolerance and implied end")) return 1;

    {
        // Anchors: parsed, consumed in frame order, absent frames report nothing.
        std::istringstream in3(FormatKeyframe(0, a) + FormatAnchor(30, 1.5f, -2.25f, 72.0f) + FormatAnchor(60, 3.0f, 4.0f, 5.0f) + FormatEnd(90));
        Recording r3 = Parse(in3);
        if (!Require(r3.badLines == 0 && r3.anchors.size() == 2 && r3.anchors[0].x == 1.5f && r3.anchors[0].z == 72.0f, "anchor round trip")) return 1;
        Player p3(r3);
        size_t seen = 0;
        for (uint32_t f = 0; f < 90; ++f) {
            Anchor anchor;
            const bool got = p3.AnchorForCurrentFrame(anchor);
            if (!Require(got == (f == 30 || f == 60), "anchor on the wrong frame")) return 1;
            if (got) { ++seen; if (!Require(anchor.frame == f, "anchor frame mismatch")) return 1; }
            p3.Step(s, dx, dy);
        }
        if (!Require(seen == 2, "anchors not all consumed")) return 1;
    }
    std::cout << "PASS input recording: round trip, playback hold/motion semantics, tolerant parsing" << std::endl;
    return 0;
}
