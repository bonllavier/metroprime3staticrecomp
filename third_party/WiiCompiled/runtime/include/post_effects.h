#pragma once

#include <aurora/gfx.h>

// Aurora image effects ([effects] in Config.toml, F1 > Effects). All live.
namespace PostEffects {

// Builds the renderer configuration from the user's settings (defaults: everything off).
AuroraPostProcess FromConfig();
// Pushes the current settings to Aurora.
void Apply();

} // namespace PostEffects
