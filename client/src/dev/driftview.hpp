#pragma once
// The side-by-side drift view (docs/FOLLOW_STREAM.md s4 `orig` / `align`, s5 `drift`; kenshi-cym.40.2.6). Dev only (KCLIENT_DEV_TOOLS, --debug, a follow stream
// connected). For every id the agent sends `orig` records for, the original's position is interpolated to the drawn sim time minus the align offset and drawn as a
// ghost with a line to our drawn character (green / yellow / red by distance); a `drift id= d= t=` line goes back once a second per character, with d measured
// at the SAME game time (ours at the render tick, the original's at that time minus the offset: never our current position against an old record).
// The F8 panel's toggle is the config key debug.drift_view; the colour thresholds are debug.drift_near / debug.drift_far.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class SimView;

namespace dev::drift
{
	struct Ghost
	{
		uint64_t id = 0;
		double ghost[3] = {};	// the original's position at the matched time
		double ours[3] = {};	// our drawn character (feet)
		std::string name;
		double d = 0;	// distance at the matched time, world units
		uint8_t origState = 0, ourState = 0;	// follow::State numbers: 0 up, 1 down, 2 ko, 3 dead, 4 carried
		int colour = 0;	// 0 green, 1 yellow, 2 red
	};

	bool on();	// debug.drift_view
	void toggle();	// the F8 row
	// followstream.cpp: the `align` record arrived. simHours: the sim's clock now, (day - 1) * 24 + hour.
	void onAlign(double alignT, double simHours);
	void reset();	// a new connection: the offset, the ghosts and the history go
	// Every frame once the stream is ready: recompute the ghosts; every second, send(`drift ...`) per character. active: the stream is connected and ready.
	void frame(SimView& sim, bool active, double wallNow, const std::function<void(const std::string&)>& send);
	const std::vector<Ghost>& ghosts();	// this frame's, empty while off
	std::string panelText();	// the F8 panel's rich text: the 5 worst drifts; "" while off
	double offsetHours();	// sim clock minus the original's at the quick-save instant (<= 0: the sim sits behind); for logs
}
