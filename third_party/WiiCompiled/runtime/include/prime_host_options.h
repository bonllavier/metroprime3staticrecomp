#pragma once

// Metroid Prime 3 host options applied once at startup (before the renderer and
// the guest start): performance switches read by the runtime and Aurora through
// the process environment, and the Wii system configuration the game reads
// through its own SC library (TV aspect ratio, sound mode, language).
namespace PrimeHostOptions {
void ApplyAtStartup();
// After aurora_initialize: 16:9 presentation for the anamorphic widescreen
// image, or 4:3 with bars. Takes the GX lock when the consumer thread runs.
void ApplyPresentation(bool widescreen);
}
