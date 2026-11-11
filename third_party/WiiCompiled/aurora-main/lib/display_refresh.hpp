#pragma once
// The monitor's refresh timeline, measured from its vertical blanks, for aligning presents to it
// (policy::align_to_refresh, F1 > Video "Sync images to the monitor's refresh"). Windows only; elsewhere
// nothing is measured and presents stay on the game's schedule.
#include "gx/render_policy.hpp"

namespace aurora::display_refresh {
// Starts the observer thread once. It only waits for vertical blanks while alignment is wanted.
void start() noexcept;
void stop() noexcept;

void set_enabled(bool enabled) noexcept;
bool enabled() noexcept;

// Usable timeline (period 0 while unknown, not regular, or not wanted).
gx::policy::RefreshGrid grid() noexcept;
// The refresh rate the timeline is locked to, 0 when it is not.
double locked_rate() noexcept;
}  // namespace aurora::display_refresh
