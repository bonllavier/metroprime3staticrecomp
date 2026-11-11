#pragma once
// QA input recording and replay (docs: recomp/qa/README.md). Compiled only with MKW_PRIME_QA_TOOLS=1,
// the local test build; the public build keeps the no-op inlines below and carries no trace of it.
//
// Every KPAD read (one per game frame) the keyboard/mouse sample and the mouse-look motion of that
// frame pass through Apply(): with WII_COMPILED_QA_RECORD=<file> they are written out, with
// WII_COMPILED_QA_REPLAY=<file> they are replaced by the recording. Frames are counted from the first
// KPAD read after launch, so a replay must start from the same state the recording did (same save,
// same WII_COMPILED_QA_TAP_INTRO setting).
#include "hle/prime_keyboard_mouse.h"

namespace PrimeInputReplay {
#if MKW_PRIME_QA_TOOLS
// Records or overrides one frame. `lookDx/lookDy` carry the frame's mouse-look motion in pixels:
// in, what the host produced (recorded); out, what the game must see (replayed).
void Apply(PrimeKeyboardMouse::Sample& input, float& lookDx, float& lookDy);
bool Replaying();
bool Recording();
#else
inline void Apply(PrimeKeyboardMouse::Sample&, float&, float&) {}
inline bool Replaying() { return false; }
inline bool Recording() { return false; }
#endif
}  // namespace PrimeInputReplay
