// The floor view model's table-driven traces (floorfollow.hpp, fable review kenshi-k16.2.236.9 section 5; kenshi-k16.2.236.11):
// no GL, no data, no window. Each trace is a short script of frames and events; every act's shown level (and a frame's reason) is
// asserted. The positions are the captures' own: the Storm House staircase's foot and top (the feet heights 1098-1100 and 1140.2
// from stairs_top_hotspot_timing/), its flight fitted to the measured tread 4.1 u and rise 3.9 u (11 treads over 44.5 u of run).
//   kfloor_test   exit 0 and "kfloor_test: ok" when every trace holds
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "floorfollow.hpp"

namespace
{
int gFails = 0;

void check(bool ok, const char* what)
{
	if (!ok) { fprintf(stderr, "kfloor_test: FAIL %s\n", what); gFails++; }
}

constexpr int kB = 1;		// the Storm House instance
constexpr int kH2 = 2;		// a second floorable building (no staircase)
constexpr uint64_t kSubjA = 41188;	// the shot's Wanderer
constexpr uint64_t kSubjB = 41190;	// a second followed character
constexpr float kMid = 1120.35f;	// the flight's middle: where the wire's floor turns over (WorldBuildings::stairMid)

// The Storm House staircase in the building's local mesh units (the trace frame: the instance is taken at the origin, scale 1).
// The footprint is the flight's own; the run is x, the foot at -50587.5 and the top at -50543.0, z -1276 through its 16 u width.
const ff::Flight kStairs = {
	{ -50589.7f, -1284.0f }, { -50540.8f, -1268.0f },	// mn, mx: the flight's footprint
	1100.5f, 1140.2f,									// footY, topY: the foot and top treads
	4.45f, 3.9f,										// tread, rise
	{ -50587.5f, -1276.0f }, { -50543.0f, -1276.0f },	// foot, top: the treads' centres (the slope)
	0, 1,												// lower, upper
};

struct Act
{
	enum Kind { kFrame, kStep, kEngage, kLoad } kind = kFrame;
	ff::Sample s;
	int dir = 0, top = 3;
	int level = 0;			// kLoad
	uint64_t subject = 0;	// kLoad
	int expect = 0;			// the level the act leaves shown
	ff::Reason reason = ff::Reason::kPlayer;	// kFrame: why
	const char* note = "";
};

// A following sample on the stairs (building-local metres): indoors, the wire's floor turning over at the flight's middle.
ff::Sample on(double x, double y, double z)
{
	ff::Sample s;
	s.following = true;
	s.subject = kSubjA;
	s.known = true;
	s.hasPos = true;
	s.building = kB;
	s.wireFloor = y > kMid ? 1 : 0;
	s.indoors = true;
	s.pos[0] = x;
	s.pos[1] = y;
	s.pos[2] = z;
	s.flights = &kStairs;
	s.flightCount = 1;
	return s;
}

// On the building's roof deck (the wire's indoors bit clear; the client's roof test names the building): the deck is beside the flight.
ff::Sample onDeck(double x, double y, double z)
{
	ff::Sample s = on(x, y, z);
	s.indoors = false;
	s.roof = kB;
	s.roofFloor = 1;
	s.wireFloor = 1;
	return s;
}

ff::Sample outdoors()
{
	ff::Sample s;
	s.following = true;
	s.subject = kSubjA;
	s.known = true;
	s.hasPos = true;
	s.building = -1;
	s.indoors = false;
	return s;
}

ff::Sample inOther()
{
	ff::Sample s = on(0, 1140.2, 0);
	s.building = kH2;
	s.wireFloor = 1;
	s.flights = nullptr;
	s.flightCount = 0;
	return s;
}

Act F(ff::Sample s, int expect, ff::Reason r, const char* note = "") { Act a; a.kind = Act::kFrame; a.s = s; a.expect = expect; a.reason = r; a.note = note; return a; }

Act FreeCam(int expect, const char* note = "")
{
	ff::Sample s;
	s.following = false;
	s.subject = 0;
	s.known = false;
	s.building = -1;
	return F(s, expect, ff::Reason::kPlayer, note);
}

Act Step(int dir, int top, int expect)
{
	Act a;
	a.kind = Act::kStep;
	a.dir = dir;
	a.top = top;
	a.expect = expect;
	a.note = "step";
	return a;
}

Act Engage(int expect)
{
	Act a;
	a.kind = Act::kEngage;
	a.expect = expect;
	a.note = "engage";
	return a;
}

Act Load(int level, uint64_t subject, int expect)
{
	Act a;
	a.kind = Act::kLoad;
	a.level = level;
	a.subject = subject;
	a.expect = expect;
	a.note = "load";
	return a;
}

struct Trace
{
	const char* name;
	std::vector<Act> acts;
};

std::vector<Trace> traces()
{
	std::vector<Trace> t;
	const ff::Reason kF = ff::Reason::kFlight, kR = ff::Reason::kRoof, kI = ff::Reason::kIndoors, kO = ff::Reason::kOutdoors, kM = ff::Reason::kManual;
	// The walk up the Storm House stairs, from the ground floor (0) to the upper floor (1). The points that are on the flight: the
	// foot (-50585, 1105), two treads up (-50560, 1125.2), 2 rises below the top (-50552, 1134: the switch line), the top tread
	// (-50542.8, 1137.7) and the deck.

	// order_sounds_V / _Z (review section 5): a door, then the latch: 3, 0, 0 (door), 1; ends following.
	for (const char* route : { "order_sounds_V", "order_sounds_Z" })
	{
		t.push_back({ route, {
			FreeCam(3, "free camera, the player's level 3"),
			Engage(3),
			F(outdoors(), 0, kO, "engage outdoors: the subject's floor 0"),
			F(on(-50580, 1098.3, -1276), 0, kI, "in through the door, floor 0: the pair moved, the level did not"),
			F(on(-50585, 1105, -1276), 0, kF, "the foot of the flight: origin 0"),
			F(on(-50560, 1125.2, -1276), 0, kF, "two treads up: still the origin floor"),
			F(on(-50552, 1134, -1276), 1, kF, "2 rises below the top: the switch line"),
			F(onDeck(-50546, 1140.2, -1260), 1, kR, "the deck: the building's top floor"),
			FreeCam(1, "the follow ends: the player keeps the last shown level"),
		} });
	}

	// bar_lure_hostile_V / _Z: a fight, a loose walk in and out: 0 at least once, 1 at most once.
	for (const char* route : { "bar_lure_hostile_V", "bar_lure_hostile_Z" })
	{
		t.push_back({ route, {
			FreeCam(3),
			Engage(3),
			F(outdoors(), 0, kO, "engage outdoors"),
			F(on(-50580, 1098.3, -1276), 0, kI, "the door"),
			F(on(-50560, 1125.2, -1276), 0, kF, "up the flight"),
			F(on(-50552, 1134, -1276), 1, kF, "over the line"),
			F(on(-50542.8, 1137.7, -1275.3), 1, kF, "the top tread"),
			F(on(-50552, 1129.5, -1276), 0, kF, "back down over the line (half a rise past it)"),
			F(on(-50580, 1098.3, -1276), 0, kI, "back through the door"),
		} });
	}

	// snap_{V,Z,double}_{3,0}: engage clears the manual level: a free camera at 3 or 0, then the subject's floor.
	static std::string snapNames[6];	// stable storage: Trace::name points into these
	int sn = 0;
	for (const char* route : { "snap_V", "snap_Z", "snap_double" })
	{
		for (int start : { 3, 0 })
		{
			snapNames[sn] = std::string(route) + "_" + (start == 3 ? "3" : "0");
			t.push_back({ snapNames[sn].c_str(), {
				Load(start, 0, start),
				FreeCam(start, "free camera"),
				Engage(start),
				F(on(-50560, 1140.2, -1276), 1, kI, "the engage takes the subject's floor 1"),
			} });
			sn++;
		}
	}

	// reengage_Z / reengage_double: manual 0 while following, engage clears it: 0 held, then 1.
	for (const char* route : { "reengage_Z", "reengage_double" })
	{
		t.push_back({ route, {
			F(on(-50560, 1140.2, -1276), 1, kI, "following, the subject on floor 1"),
			Step(-1, 1, 0),
			F(on(-50560, 1140.2, -1276), 0, kM, "the stepped level holds"),
			F(on(-50560, 1140.2, -1276), 0, kM),
			Engage(0),
			F(on(-50560, 1140.2, -1276), 1, kI, "the re-engage clears the manual level"),
		} });
	}

	// start_engage_snap: a start centring is an engage: 3, 0.
	t.push_back({ "start_engage_snap", {
		FreeCam(3),
		F(on(-50580, 1098.3, -1276), 0, kI, "the camera starts centred: the engage sets floor 0"),
	} });

	// stairs_walk: the latch up; the deck and the stand are off the flight: 0, 1, no further change.
	t.push_back({ "stairs_walk", {
		F(on(-50580, 1098.3, -1276), 0, kI, "on the ground floor"),
		F(on(-50585, 1105, -1276), 0, kF),
		F(on(-50560, 1125.2, -1276), 0, kF),
		F(on(-50552, 1134, -1276), 1, kF, "the switch line"),
		F(onDeck(-50546, 1140.2, -1260), 1, kR),
		F(onDeck(-50546, 1140.2, -1260), 1, kR, "a stand on the deck changes nothing"),
	} });

	// stairs_down: the latch both ways: 0, 1, 0, stays 0 to the foot.
	t.push_back({ "stairs_down", {
		F(on(-50580, 1098.3, -1276), 0, kI),
		F(on(-50552, 1134, -1276), 1, kF, "up"),
		F(onDeck(-50546, 1140.2, -1260), 1, kR),
		F(on(-50542.8, 1137.7, -1275.3), 1, kF, "the top tread: still the upper floor"),
		F(on(-50552, 1131.5, -1276), 0, kF, "1 rise below the top: down"),
		F(on(-50585, 1105, -1276), 0, kF, "the foot: stays down"),
		F(on(-50580, 1098.3, -1276), 0, kI),
	} });

	// stairs_click: the click writes nothing; the walk does: 1 held through the click, 0 on arrival.
	t.push_back({ "stairs_click", {
		F(on(-50560, 1140.2, -1276), 1, kI, "following on floor 1"),
		F(on(-50560, 1140.2, -1276), 1, kI, "the order arrives on the stairs: no level input from a click"),
		F(on(-50542.8, 1137.7, -1275.3), 1, kF, "the walk reaches the top tread: above the down line"),
		F(on(-50552, 1131.5, -1276), 0, kF, "the walk crosses the line: down"),
		F(on(-50560, 1125.2, -1276), 0, kF, "and stays down"),
	} });

	// roof_open_ground: a character on the roof of a one-floor building: unchanged; the model's roof floor is the building's top.
	t.push_back({ "roof_open_ground", {
		F(onDeck(-50540, 1140.5, -1240), 1, kR),
		F(onDeck(-50530, 1140.5, -1240), 1, kR),
	} });

	// deck_click: the 222316 run (goal -50550 1140 -1266 from the deck; the feet at 1139.4-1140.2): the view stays 1, the lead drawn.
	t.push_back({ "deck_click_222316", {
		F(onDeck(-50545.1, 1139.4, -1266.3), 1, kR, "the stand on the deck (the 222310 hidden body)"),
		F(onDeck(-50549.2, 1140.2, -1263.4), 1, kR, "the shot's stand: beside the flight, not on it"),
		F(onDeck(-50550, 1140, -1266), 1, kR, "the move goal, still the deck"),
	} });

	// deck_edge: a walk along the deck edge beside the flight; the top tread's own footprint stays at 1 too: no change.
	t.push_back({ "deck_edge", {
		F(onDeck(-50550, 1140.2, -1267), 1, kR, "just beside the flight's footprint"),
		F(onDeck(-50556, 1140.2, -1267), 1, kR),
		F(onDeck(-50543, 1140.2, -1270), 1, kF, "just inside the top tread: the upper floor, no flip"),
		F(onDeck(-50545, 1140.2, -1266), 1, kR),
	} });

	// stop_midflight_up: an order whose goal is mid-flight, a stand: one change at most (the lead drawn).
	t.push_back({ "stop_midflight_up", {
		F(on(-50580, 1098.3, -1276), 0, kI, "the order starts on floor 0"),
		F(on(-50558.9, 1125.9, -1273.1), 0, kF, "the captured 203819 stand: below the line"),
		F(on(-50551, 1135, -1276), 1, kF, "the goal above the line"),
		F(on(-50551, 1135, -1276), 1, kF, "the stand: one change"),
	} });

	// stop_midflight_down: from the upper floor, a goal mid-flight, a stand: one change at most.
	t.push_back({ "stop_midflight_down", {
		F(on(-50560, 1140.2, -1276), 1, kI, "floor 1"),
		F(on(-50542.8, 1137.7, -1275.3), 1, kF, "the top tread"),
		F(on(-50552, 1131.5, -1276), 0, kF, "mid-flight: down"),
		F(on(-50552, 1131.5, -1276), 0, kF, "the stand: one change"),
	} });

	// reverse_midflight: up past the line, then back down: 0, 1, 0.
	t.push_back({ "reverse_midflight", {
		F(on(-50580, 1098.3, -1276), 0, kI),
		F(on(-50551, 1135, -1276), 1, kF, "up past the line"),
		F(on(-50557, 1129, -1276), 0, kF, "back below it (half a rise past, the hysteresis line)"),
	} });

	// manual_hold: following on the deck, F: the view stays 0 while the lead walks the deck; the next stair crossing takes it back.
	t.push_back({ "manual_hold", {
		F(onDeck(-50546, 1140.2, -1260), 1, kR),
		Step(-1, 1, 0),
		F(onDeck(-50546, 1140.2, -1260), 0, kM, "the deck walk keeps the stepped level"),
		F(onDeck(-50540, 1140.2, -1240), 0, kM),
		F(on(-50542.8, 1137.7, -1275.3), 0, kM, "onto the top tread: the manual level holds"),
		F(on(-50552, 1131.5, -1276), 0, kF, "the down crossing takes the level back: 0"),
		F(on(-50585, 1105, -1276), 0, kF),
		F(on(-50545, 1139, -1276), 1, kF, "the climb back up: 1"),
	} });

	// manual_up_then_climb: R to 1 while the lead is on floor 0, then the climb: never a flip down.
	t.push_back({ "manual_up_then_climb", {
		F(on(-50580, 1098.3, -1276), 0, kI),
		Step(1, 1, 1),
		F(on(-50585, 1105, -1276), 1, kM, "the lead on floor 0, the view stepped to 1"),
		F(on(-50560, 1125.2, -1276), 1, kM),
		F(on(-50552, 1134, -1276), 1, kF, "the climb's own flip to 1: the pair moved, the view did not"),
		F(onDeck(-50546, 1140.2, -1260), 1, kR),
	} });

	// two_subjects: A on the deck, B on the ground; engage B, A, B: 0, 1, 0, with B standing near the stair top (the stairwell).
	t.push_back({ "two_subjects", {
		F([&] { ff::Sample s = on(-50580, 1098.3, -1280); s.subject = kSubjB; return s; }(), 0, kI, "B on the ground"),
		Engage(0),
		F(onDeck(-50546, 1140.2, -1260), 1, kR, "A on the deck"),
		F([&] { ff::Sample s = on(-50580, 1098.3, -1280); s.subject = kSubjB; return s; }(), 0, kI, "back to B: the subject change resets the latch"),
	} });

	// follow_lost: free camera while the lead is on floor 1, the lead walks down: the view stays 1.
	t.push_back({ "follow_lost", {
		F(on(-50560, 1140.2, -1276), 1, kI),
		FreeCam(1, "the follow ends on floor 1"),
		FreeCam(1, "the lead walks down; nothing writes"),
		FreeCam(1),
	} });

	// other_building: out of the Storm House and into a second floorable building: one write each way.
	t.push_back({ "other_building", {
		F(on(-50560, 1140.2, -1276), 1, kI),
		F(outdoors(), 0, kO, "out: 0"),
		F(inOther(), 1, kI, "into the second building's floor 1: 1"),
	} });

	// start_indoors: a session that starts with the lead inside (sunshafts_bar): the engage gives its floor.
	t.push_back({ "start_indoors", {
		FreeCam(3),
		F(on(-50560, 1140.2, -1276), 1, kI, "the start centring gives floor 1"),
	} });

	// The user's five shots (stairs_top_hotspot_timing/): the captured feet positions, the model's view beside the old shot's.
	// 203511: on the deck at 1140.3, the top of the stairs beside it: the view is 1 (the shot's own).
	t.push_back({ "shot_203511", {
		F(onDeck(-50547.8, 1140.3, -1244.0), 1, kR),
	} });
	// 203515: the failing follow to a waypoint on the flight: the shot's deck stand is 1, the waypoint 0 (the shot shows 0 there).
	t.push_back({ "shot_203515", {
		F(onDeck(-50544.2, 1139.9, -1253.3), 1, kR, "the deck stand (the old view 0 came from the hotspot)"),
		F(on(-50552.16, 1131.39, -1277.86), 0, kF, "the order's waypoint on the flight, below the line: 0"),
	} });
	// 203516: the mid-flight stand the old rules left at 1 while the lead was on the flight, below the line: 0 here.
	t.push_back({ "shot_203516", {
		F(onDeck(-50547.73, 1140.25, -1244.08), 1, kR),
		F(on(-50552.08, 1131.93, -1277.90), 0, kF, "the stand 1 rise below the top: 0 (the old view 1)"),
	} });
	// 203816: floor 1, down over the top tread to the mid-flight stand: 1, 0, 0 (the shot's 0).
	t.push_back({ "shot_203816", {
		F(on(-50560, 1140.2, -1276), 1, kI),
		F(on(-50542.8, 1137.7, -1275.3), 1, kF),
		F(on(-50552.16, 1131.39, -1277.86), 0, kF),
		F(on(-50559.74, 1125.87, -1273.61), 0, kF),
	} });
	// 203819: the climb from the foot (the order's goal, before the first tread) to a stand 3.2 rises below the top: the origin floor
	// (0) until the line, then 1.
	t.push_back({ "shot_203819", {
		F(on(-50594.2, 1098.32, -1281.0), 0, kI, "the order's goal at the foot, 4 u before the first tread"),
		F(on(-50559.74, 1125.87, -1273.61), 0, kF, "the stand: below the 2-rise line (the old wire floor said 1)"),
		F(on(-50550, 1135, -1276), 1, kF, "the same climb over the line: 1"),
	} });

	// The latch's own checks, outside a trace: a character under the stairwell and one on the deck beside the flight are not on it,
	// and a stand on the switch line does not flicker.
	return t;
}

void directChecks()
{
	// First seen mid-flight: the wire floor is the origin (1), so the level is the floor it is heading to (0).
	{
		ff::FloorFollow m;
		ff::View v = m.update(on(-50560, 1125.2, -1276));
		check(v.level == 0 && v.reason == ff::Reason::kFlight, "first seen mid-flight: the wire floor is the origin");
	}
	// A stand on the up line: one flip up, no flicker as the samples jitter a tenth of a metre either side.
	{
		ff::FloorFollow m;
		m.update(on(-50580, 1098.3, -1276));
		const float x = -50552.0f;	// the up line, top - 2 rises
		check(m.update(on(x, 1132.5, -1276)).level == 1, "the up line crossed: 1");
		check(m.update(on(x, 1132.3, -1276)).level == 1, "half a rise of jitter below it: still 1");
		check(m.update(on(x, 1132.3, -1276)).level == 1, "again: still 1");
		check(m.update(on(x, 1130.0, -1276)).level == 0, "more than half a rise below it: back to 0");
	}
	// The follow ends while on the flight: the player keeps the level, and no later sample writes.
	{
		ff::FloorFollow m;
		m.update(on(-50580, 1098.3, -1276));
		m.update(on(-50552, 1134, -1276));
		ff::Sample free;
		check(m.update(free).level == 1, "the follow ends on the flight: 1 kept");
		ff::Sample free2;
		free2.following = false;
		check(m.update(free2).level == 1, "a later free-camera frame writes nothing");
	}
	// The subject is lost mid-follow: the same hold.
	{
		ff::FloorFollow m;
		m.update(on(-50560, 1140.2, -1276));
		ff::Sample lost = on(-50560, 1140.2, -1276);
		lost.known = false;
		check(m.update(lost).level == 1, "the subject lost: the last shown level");
	}
	// A load: the saved level holds while following the restored subject until an engage clears it.
	{
		ff::FloorFollow m;
		m.load(0, kSubjA);
		check(m.update(on(-50560, 1140.2, -1276)).level == 0, "load: the saved level 0 holds under the restored subject");
		m.engage();
		check(m.update(on(-50560, 1140.2, -1276)).level == 1, "the engage clears it: the subject's floor");
	}
	// Two treads' slope: under the stairwell the feet are a storey off the slope and the flight is not entered.
	{
		ff::FloorFollow m;
		const ff::View v = m.update(on(-50580, 1098.3, -1276));	// inside the footprint, the ground floor
		check(v.level == 0 && v.reason == ff::Reason::kIndoors, "under the stairwell: not on the flight");
	}
}

void run()
{
	for (const Trace& tr : traces())
	{
		ff::FloorFollow m;
		for (size_t i = 0; i < tr.acts.size(); i++)
		{
			const Act& a = tr.acts[i];
			ff::View v;
			switch (a.kind)
			{
			case Act::kFrame: v = m.update(a.s); break;
			case Act::kStep: m.step(a.dir, a.top); v.level = m.shown(); v.reason = a.reason; break;
			case Act::kEngage: m.engage(); v.level = m.shown(); v.reason = a.reason; break;
			case Act::kLoad: m.load(a.level, a.subject); v.level = m.shown(); v.reason = a.reason; break;
			}
			if (v.level != a.expect)
			{
				fprintf(stderr, "kfloor_test: FAIL %s act %zu (%s): view %d, expected %d\n", tr.name, i, a.note, v.level, a.expect);
				gFails++;
			}
			if (a.kind == Act::kFrame && v.reason != a.reason)
			{
				fprintf(stderr, "kfloor_test: FAIL %s act %zu (%s): reason %s, expected %s\n", tr.name, i, a.note, ff::reasonName(v.reason),
					ff::reasonName(a.reason));
				gFails++;
			}
		}
	}
	directChecks();
}

// Every reason has a name (the log line's `floor: view A -> B (reason)`).
void reasonNames()
{
	for (ff::Reason r : { ff::Reason::kPlayer, ff::Reason::kManual, ff::Reason::kFlight, ff::Reason::kRoof, ff::Reason::kIndoors, ff::Reason::kOutdoors })
	{
		check(ff::reasonName(r) && ff::reasonName(r)[0] != '?', "every reason has a name");
	}
}
}

int main()
{
	run();
	reasonNames();
	if (gFails) { fprintf(stderr, "kfloor_test: %d failed\n", gFails); return 1; }
	printf("kfloor_test: ok\n");
	return 0;
}
