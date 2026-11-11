#pragma once
// RetroAchievements overlay (hle/prime_achievements.h): the F2 window
// (account, game, achievement list) and the in-game pop-ups drawn in the
// style of Prime 3's visor HUD.
struct ImFont;

namespace PrimeAchievementsUi {
// The F2 window; `open` follows its close button.
void DrawPanel(bool* open);
// Unlock pop-ups (right edge), progress and challenge indicators. `hudFont` is
// the overlay's HUD face (Bahnschrift), or null for ImGui's default.
void DrawHud(ImFont* hudFont);
}  // namespace PrimeAchievementsUi
