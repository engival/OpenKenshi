#pragma once
#include <algorithm>
#include <cstdint>

// A progress value from the wire and the one extrapolation between resends (SIMLINK TASK_PROGRESS 39, and INTERACTABLE kind 4's producer tail which
// sends the same numbers): fill = clamp(progress + rate x sim-seconds since tick, 0, 1), the sim clock at 20 ticks per sim-second (SIMLINK CLOCK).
// The server resends when the value or the rate changes, so nothing here polls. Both the over-head bars (progress_frame) and the production
// building's window (machine_state) read through this; do not keep a second copy of the formula.
struct ProgressFeed
{
	float progress = 0;	// 0..1 at `tick`
	float rate = 0;		// progress per sim-second; 0 when stalled or not advancing
	uint64_t tick = 0;	// the sim tick `progress` is valid at (`progressTick` on the producer tail)
	float at(double nowTick) const { return std::clamp(progress + rate * (float)((nowTick - (double)tick) / 20.0), 0.0f, 1.0f); }
};
