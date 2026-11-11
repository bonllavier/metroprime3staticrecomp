#pragma once
// Photo mode for RM3E01 (F3): the game's per-frame update (0x802EBAF4: frame timing, the message
// queue that drives actors, animations, scripts and cinematics, input) is skipped while it is on,
// but the main loop (0x802EDC9C) still draws every frame. The world stays frozen at one instant
// and is rendered anew each frame, so material and effect settings can be tuned on it live.
// The skipped update reports success (r3 = 1) so the loop never takes its stop path.
// Design: recomp/docs/modo_foto.md.

namespace PrimePhotoMode {

bool Active();
void SetActive(bool active);
// Lets the next game update run once while frozen (frame-by-frame stepping).
void StepFrame();

}  // namespace PrimePhotoMode
