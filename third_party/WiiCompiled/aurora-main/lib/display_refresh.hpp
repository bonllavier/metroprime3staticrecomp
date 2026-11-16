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
// The same alignment at the game's own rate, with frame interpolation off ([video] display_sync).
void set_native_enabled(bool enabled) noexcept;
bool native_enabled() noexcept;
// Keeps measuring the refresh although nothing is aligned (display cadence estimate).
void set_observe_always(bool enabled) noexcept;
// The game's frame clock follows the measured refresh (aurora_set_refresh_lock): it is measured all the time.
void set_frame_lock_enabled(bool enabled) noexcept;
bool frame_lock_enabled() noexcept;

// Usable timeline (period 0 while unknown, not regular, or not wanted).
gx::policy::RefreshGrid grid() noexcept;
// The measured timeline whether or not alignment is wanted (period 0 while unknown or not regular).
gx::policy::RefreshGrid observed_grid() noexcept;
// The refresh rate the timeline is locked to, 0 when it is not.
double locked_rate() noexcept;
}  // namespace aurora::display_refresh
