#pragma once
// Custom materials of RM3E01 (F1 > Materials). Each entry is a character or surface family whose
// textures get Aurora's enhanced-metal shading with its own parameters; an entry shows only the
// parameter groups its look uses (star dust for Gandrayda, for instance). Values are saved in
// Config.toml [prime_materials] as <entry>_<parameter>; Samus' entry is the generic enhanced metal
// of F1 > Video ([video] enhanced_metal_*) plus her colour and highlight controls.
// Design and texture sources: recomp/docs/mejoras_graficas.md.

namespace PrimeMaterials {

// Sends every entry to Aurora (startup, and after the Effects settings change).
void ApplyAll();
// The F1 tab.
void DrawTab();

}  // namespace PrimeMaterials
