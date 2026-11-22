#pragma once
// The real frame rate (kenshi-k16.2.437.3, FINDINGS kenshi-k16.2.205.12.112 (c)): sokol's sapp_frame_duration is an EMA of the callback interval and counts
// a repeated frame like a new one, so the fps built from it (the old app.fps) is the swap rate. This counts what the player sees change:
//   unique/s  frames that drew a new packet (render.thread=off: every callback; lockstep: a packet drawn; overlap: a redraw of the drawing packet is a repeat)
//   swaps/s   frame callbacks (sokol swaps after each)
//   present-to-present p50 / p95 / max (ms) of the unique frames
// note() is called once at the end of each frame callback on the sapp thread; the once-a-second result is published through atomics (the HUD reads
// it on the logic thread, or the sapp thread with render.thread=off). Always compiled in: it is a clock read and a vector push per callback.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>

namespace realfps
{
struct Rate { double unique = 0, swaps = 0, p50 = 0, p95 = 0, max = 0; };

inline std::atomic<double> gUnique{ 0 }, gSwaps{ 0 }, gP50{ 0 }, gP95{ 0 }, gMax{ 0 };

// Sapp thread only. A unique frame is a new packet drawn; a repeat is a kept frame shown again or a redraw.
inline void note(bool unique)
{
	using Clock = std::chrono::steady_clock;
	static Clock::time_point window{}, lastUnique{};
	static long swaps = 0, uniq = 0;
	static std::vector<double> dts;
	const auto now = Clock::now();
	if (window.time_since_epoch().count() == 0) { window = now; }
	swaps++;
	if (unique)
	{
		uniq++;
		if (lastUnique.time_since_epoch().count() != 0) { dts.push_back(std::chrono::duration<double, std::milli>(now - lastUnique).count()); }
		lastUnique = now;
	}
	const double secs = std::chrono::duration<double>(now - window).count();
	if (secs < 1.0) { return; }
	std::sort(dts.begin(), dts.end());
	auto pct = [&](double p) { return dts.empty() ? 0.0 : dts[std::min(dts.size() - 1, (size_t)(p * dts.size()))]; };
	gUnique.store(uniq / secs);
	gSwaps.store(swaps / secs);
	gP50.store(pct(0.5));
	gP95.store(pct(0.95));
	gMax.store(dts.empty() ? 0.0 : dts.back());
	swaps = uniq = 0;
	dts.clear();
	window = now;
}

inline Rate last() { return { gUnique.load(), gSwaps.load(), gP50.load(), gP95.load(), gMax.load() }; }
}
