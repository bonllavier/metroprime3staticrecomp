#pragma once

#include <filesystem>
#include <string>

// Optional ReShade (bundled in <game folder>/reshade/). When [reshade] enabled
// is true the DLL is loaded manually before the renderer creates its Direct3D 12
// device, so ReShade hooks the swap chain and shows its own overlay (Home key)
// with every effect and parameter. Nothing is loaded when disabled.
namespace ReShadeLoader {

void LoadIfEnabled();
std::string Status();              // "disabled", "loaded 6.8.0", "not found: ..." or an error
std::filesystem::path Directory(); // <portable root>/reshade
// Called every frame: ReShade effects run only when `allowed` (a 3D scene and no port panel
// open). Effects the player switched off in ReShade (End key) stay off.
void SetEffectsAllowed(bool allowed);

} // namespace ReShadeLoader
