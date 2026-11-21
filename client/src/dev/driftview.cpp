// The side-by-side drift view: see driftview.hpp. Dev only.
#include "dev/driftview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "app/config.hpp"
#include "bridge/simview.hpp"
#include "dev/origstore.hpp"

namespace dev::drift
{
namespace
{
const cfg::Bool kDriftView{ cfg::Group{ "Drift view" }, "debug.drift_view", false, "Debug (kenshi-cym.40.2.6; with --follow-stream): the original's characters as ghosts at the matched game time, with a line to our drawn character (green under drift_near, yellow under drift_far, red beyond), and a `drift` record per character back to the agent once a second. Also a toggle in the F8 panel" };
const cfg::Float kNear{ cfg::Group{ "Drift view" }, "debug.drift_near", 1.0, 0.0, 1000.0, "Drift view: a ghost line is green under this distance (world units)" };
const cfg::Float kFar{ cfg::Group{ "Drift view" }, "debug.drift_far", 10.0, 0.0, 10000.0, "Drift view: a ghost line is yellow under this distance and red beyond; the F8 table counts how long a character has been above it" };

constexpr double kTickSeconds = 0.05;	// ksim/core/clock.hpp
constexpr double kHourSeconds = 2618.1819 / 24.0;	// wall seconds of one game hour at 1x (kGameHourSeconds)
constexpr double kHoursPerTick = kTickSeconds / kHourSeconds;

struct Track
{
	double d = 0;
	double aboveSince = -1;	// sim seconds (ticks x 0.05) at which d first exceeded kFar; < 0 not above
	double aboveFor = 0;	// sim seconds above it so far
	bool seen = false;
};

struct S
{
	bool haveOffset = false;
	double offset = 0;	// sim hours minus the original's hours at one instant
	std::vector<Ghost> ghosts;
	std::map<uint64_t, Track> tracks;
	double lastSend = 0;
	bool loggedFirst = false;
};
S& st() { static S s; return s; }

// the sim's game hours at a (fractional) tick, anchored at the newest CLOCK record
double simHoursAt(const SimView& sim, double tick)
{
	const auto& c = sim.clock();
	return ((double)c.day - 1.0) * 24.0 + c.hour + (tick - (double)c.tick) * kHoursPerTick;
}

uint8_t ourStateOf(const SimView& sim, uint64_t id)
{
	simlink::Dynamic d;
	if (!sim.stateAt(id, d)) { return 0; }
	if (d.carriedBy) { return 4; }
	if (d.flags & simlink::kDead) { return 3; }
	if (d.flags & simlink::kKO) { return 2; }
	if (d.flags & simlink::kProne) { return 1; }
	return 0;
}

const char* stateName(uint8_t s)
{
	static const char* const n[] = { "up", "down", "ko", "dead", "carried" };
	return s < 5 ? n[s] : "?";
}
}

bool on() { return kDriftView.get(); }
void toggle() { cfg::setUser("debug.drift_view", on() ? "off" : "on"); }

void onAlign(double alignT, double simHours)
{
	S& s = st();
	s.offset = simHours - alignT;
	s.haveOffset = true;
	printf("drift: align offset %.6f h (%.3f game s): the sim clock %.6f h at the import, the original's %.6f h\n", s.offset, s.offset * kHourSeconds, simHours, alignT);
	fflush(stdout);
}

void reset()
{
	S& s = st();
	s.haveOffset = false;
	s.ghosts.clear();
	s.tracks.clear();
	s.lastSend = 0;
}

double offsetHours() { return st().offset; }
const std::vector<Ghost>& ghosts() { return st().ghosts; }

void frame(SimView& sim, bool active, double wallNow, const std::function<void(const std::string&)>& send)
{
	S& s = st();
	s.ghosts.clear();
	static const bool logAll = std::getenv("KCLIENT_DRIFT_LOG") != nullptr;	// one trace line per frame per selected character: tools/drift_track.py's input
	if (logAll && active && s.haveOffset && sim.clock().valid && sim.tick() >= 0)
	{
		const double since = (simHoursAt(sim, sim.tick()) - s.offset - follow::origStore().alignT()) * 3600.0;	// game seconds since the quick-save instant, at the drawn tick
		for (const uint64_t id : sim.selected())
		{
			double p[3];
			if (sim.simPos(id, p)) { printf("drift-trace: id %llu t %.4f x %.3f z %.3f\n", (unsigned long long)id, since, p[0], p[2]); }
		}
	}
	if (!active || !on() || !s.haveOffset || !sim.clock().valid || sim.tick() < 0) { return; }
	const follow::OrigStore& store = follow::origStore();
	const double tick = sim.tick();
	const double simT = simHoursAt(sim, tick);	// the drawn sim time
	const double origT = simT - s.offset;	// the original's clock at the same game time
	const bool due = wallNow - s.lastSend >= 1.0;
	if (due) { s.lastSend = wallNow; }
	const double near = kNear.get(), far = kFar.get();
	for (const uint64_t id : store.ids())
	{
		follow::OrigSample o;
		double at[3], ours[3];
		float hull[3];
		if (!store.at(id, origT, o)) { continue; }
		if (!sim.simPos(id, ours)) { continue; }	// ours at the render tick: the same game time as origT
		Ghost g;
		g.id = id;
		g.name = sim.charName(id);
		std::copy(o.pos, o.pos + 3, g.ghost);
		g.origState = o.state;
		g.ourState = ourStateOf(sim, id);
		std::copy(ours, ours + 3, g.ours);
		// horizontal: the wire carries no height for a character until the standing ray gives one (simPos's y is 0 before it), and the original's logical y is its own
		g.d = std::hypot(o.pos[0] - ours[0], o.pos[2] - ours[2]);
		if (sim.placed(id, at, hull)) { std::copy(at, at + 3, g.ours); }	// the line ends on the drawn feet; d stays the sim-to-record distance
		if (logAll) { printf("drift-d: id %llu t %.4f d %.3f\n", (unsigned long long)id, (origT - store.alignT()) * 3600.0, g.d); }
		g.colour = g.d < near ? 0 : g.d < far ? 1 : 2;
		Track& t = s.tracks[id];
		t.d = g.d;
		t.seen = true;
		const double simSec = tick * kTickSeconds;
		if (g.d > far) { if (t.aboveSince < 0) { t.aboveSince = simSec; } t.aboveFor = simSec - t.aboveSince; }
		else { t.aboveSince = -1; t.aboveFor = 0; }
		if (due && send)
		{
			char b[128];
			snprintf(b, sizeof b, "drift id=%llu d=%.3f t=%.6f", (unsigned long long)id, g.d, simT);
			send(b);
		}
		s.ghosts.push_back(g);
	}
	if (!s.loggedFirst && !s.ghosts.empty()) { s.loggedFirst = true; printf("drift: first ghosts (%zu), d %.3f for id %llu at sim t %.6f h, original t %.6f h\n", s.ghosts.size(), s.ghosts[0].d, (unsigned long long)s.ghosts[0].id, simT, origT); fflush(stdout); }
}

std::string panelText()
{
	S& s = st();
	if (!on()) { return ""; }
	if (!s.haveOffset) { return "Drift view: waiting for the agent's align record\n"; }
	std::vector<const Ghost*> w;
	for (const Ghost& g : s.ghosts) { w.push_back(&g); }
	std::sort(w.begin(), w.end(), [](const Ghost* a, const Ghost* b) { return a->d > b->d; });
	std::string out = "Worst drifts (" + std::to_string(s.ghosts.size()) + " tracked)\n";
	char b[200];
	for (size_t i = 0; i < w.size() && i < 5; i++)
	{
		const Ghost& g = *w[i];
		const Track& t = s.tracks[g.id];
		const char* col = g.colour == 0 ? "55ff66" : g.colour == 1 ? "ffdd44" : "ff5544";
		snprintf(b, sizeof b, "{#%s}# %llu %.24s  %.1f u  >%.0f u for %.0f s%s%s\n", col, (unsigned long long)g.id, g.name.empty() ? "?" : g.name.c_str(), g.d, kFar.get(), t.aboveFor, g.origState != g.ourState ? "  orig " : "", g.origState != g.ourState ? stateName(g.origState) : "");
		out += b;
	}
	return out;
}
}
