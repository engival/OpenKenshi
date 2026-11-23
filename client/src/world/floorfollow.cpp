#include "world/floorfollow.hpp"

#include <algorithm>
#include <cmath>

namespace ff
{
namespace
{
constexpr double kOnFlightRises = 1.5;	// "on a flight": within about 1.5 rises of its slope (review section 3)
// The switch lines (user answer kenshi-k16.2.236.13 Q3, 2026-11-22: "the 4th stair rung" up, "2nd rung" down): a climb shows the upper floor from kUpLineRises below the top tread, a descent
// shows the lower floor from kDownLineRises below it; a flip back to the origin floor (the fired hotspot re-arming) waits until the feet are kRearmRises past the line.
constexpr double kUpLineRises = 4.0;
constexpr double kDownLineRises = 2.0;
constexpr double kRearmRises = 2.0;	// a fired hotspot is cold until the feet are about 2 steps past its line (or at the flight's end)
}

const char* reasonName(Reason r)
{
	switch (r)
	{
	case Reason::kPlayer: return "player";
	case Reason::kFlight: return "flight";
	case Reason::kRoof: return "roof";
	case Reason::kIndoors: return "indoors";
	case Reason::kOutdoors: return "outdoors";
	}
	return "?";
}

// The flight the feet are on: inside its footprint and within about 1.5 rises of its slope (the height interpolated from the foot
// tread to the top tread along the run). A character under the stairwell stands on the same footprint at a height a storey off the
// slope, and one on the deck beside it is outside the footprint: neither is on the flight.
int FloorFollow::onFlight(const Sample& s)
{
	if (!s.hasPos || !s.flights || s.flightCount <= 0) { return -1; }
	for (int i = 0; i < s.flightCount; i++)
	{
		const Flight& f = s.flights[i];
		if (s.pos[0] < f.mn[0] || s.pos[0] > f.mx[0] || s.pos[2] < f.mn[1] || s.pos[2] > f.mx[1]) { continue; }
		const double dx = (double)f.top[0] - f.foot[0], dz = (double)f.top[1] - f.foot[1];
		const double len2 = dx * dx + dz * dz;
		double t = len2 > 1e-9 ? ((s.pos[0] - f.foot[0]) * dx + (s.pos[2] - f.foot[1]) * dz) / len2 : 0.0;
		t = std::clamp(t, 0.0, 1.0);
		const double slope = (double)f.footY + ((double)f.topY - (double)f.footY) * t;
		if (std::fabs(s.pos[1] - slope) <= kOnFlightRises * (double)f.rise) { return i; }
	}
	return -1;
}

// The latch's level for a frame on the flight. Above top - 1 rise: the upper floor; below top - 2 rises: the lower; between: the
// floor that is not the origin (the one it is heading to). A flip back to the origin floor waits until the feet are two rises past
// the line it crossed, or at the flight's end (the hotspot is consumed; a stand on the line does not flicker); the flip away from it happens at once. prev == origin boards.
int FloorFollow::sideOnFlight(const Flight& f, int origin, int prev, double y)
{
	const double top = f.topY, rise = f.rise;
	const double upperLine = top - kDownLineRises * rise, lowerLine = top - kUpLineRises * rise;
	const int want = y > upperLine ? f.upper : y < lowerLine ? f.lower : (origin == f.upper ? f.lower : f.upper);
	if (prev == want) { return prev; }
	const int originSide = origin == f.upper ? f.upper : f.lower;
	if (want != originSide) { return want; }
	const double r = rearmY(f, originSide);
	return (originSide == f.upper ? y > r : y < r) ? want : prev;
}

// The hotspot is consumable (kenshi-k16.2.236.8.4.3, the user: "walk away from the hotspot before it becomes hot again"): once the latch has flipped away from
// its origin floor, it flips back only when the feet are kRearmRises (about 2 steps) past the line it crossed, or at the flight's own end if that comes first
// (the top / foot tread: the lower of the two, so a flight shorter than that still re-arms at its end).
double FloorFollow::rearmY(const Flight& f, int originSide)
{
	const double top = f.topY, rise = f.rise;
	if (originSide == f.upper) { return std::min(top - kDownLineRises * rise + kRearmRises * rise, top - 0.1 * rise); }
	return std::max(top - kUpLineRises * rise - kRearmRises * rise, (double)f.footY + 0.1 * rise);
}

void FloorFollow::resetLatch()
{
	latch_.on = false;
	latch_.building = -1;
	latch_.flight = -1;
	latch_.origin = 0;
	latch_.side = 0;
}

FloorFollow::Resolved FloorFollow::resolve(const Sample& s)
{
	// 1. On a flight of `building`: the stair latch. The building is this one, whatever the wire says.
	if (s.building >= 0)
	{
		// A frame without a position or without the flight's measurement cannot say it left: hold the latch's level.
		if (latch_.on && latch_.building == s.building && (!s.hasPos || !s.flights || s.flightCount <= 0))
		{
			return { latch_.building, latch_.side, Reason::kFlight };
		}
		const int fi = onFlight(s);
		if (fi >= 0)
		{
			const Flight& f = s.flights[fi];
			if (!latch_.on || latch_.building != s.building || latch_.flight != fi)
			{
				// Boarding. The origin is the floor the subject was settled on; first seen mid-flight: the wire floor.
				const bool fromSettled = settledValid_ && (settled_ == f.lower || settled_ == f.upper);
				const int origin = (fromSettled ? settled_ : s.wireFloor) == f.upper ? f.upper : f.lower;
				latch_.on = true;
				latch_.building = s.building;
				latch_.flight = fi;
				latch_.origin = origin;
				latch_.side = sideOnFlight(f, origin, origin, s.pos[1]);
			}
			else
			{
				// A walk that has gone kRearmRises past the line it fired on is a real walk to the other end: that floor is now the origin, so a
				// climb back up from there is a new crossing (it fires on the other line) instead of waiting for the old hotspot to re-arm.
				if (latch_.origin == f.upper && s.pos[1] < f.topY - (kDownLineRises + kRearmRises) * f.rise) { latch_.origin = f.lower; }
				else if (latch_.origin == f.lower && s.pos[1] > f.topY - (kUpLineRises - kRearmRises) * f.rise) { latch_.origin = f.upper; }
				latch_.side = sideOnFlight(f, latch_.origin, latch_.side, s.pos[1]);
			}
			settled_ = latch_.side;
			settledValid_ = true;
			info_.on = true; info_.origin = latch_.origin; info_.side = latch_.side; info_.y = s.pos[1];
			info_.upperLine = f.topY - kDownLineRises * f.rise; info_.lowerLine = f.topY - kUpLineRises * f.rise;
			info_.fired = latch_.side != latch_.origin;
			info_.rearmY = rearmY(f, latch_.origin == f.upper ? f.upper : f.lower);
			return { s.building, latch_.side, Reason::kFlight };
		}
	}
	if (latch_.on)	// it left the flight: the floor it left on is the one it is settled on now
	{
		settled_ = latch_.side;
		settledValid_ = true;
	}
	resetLatch();
	info_ = LatchInfo();
	// 2. On a roof deck: the wire's indoors bit clear and the client's roof test names a building: that building's top floor.
	if (!s.indoors && s.roof >= 0)
	{
		settled_ = s.roofFloor;
		settledValid_ = true;
		return { s.roof, s.roofFloor, Reason::kRoof };
	}
	// 3. Indoors on the wire.
	if (s.indoors && s.building >= 0)
	{
		settled_ = s.wireFloor;
		settledValid_ = true;
		return { s.building, s.wireFloor, Reason::kIndoors };
	}
	// 4. Outdoors.
	settled_ = 0;
	settledValid_ = true;
	return { -1, 0, Reason::kOutdoors };
}

View FloorFollow::update(const Sample& s)
{
	const bool follow = s.following && s.subject != 0 && s.known;
	if (!follow)
	{
		if (following_ || subject_ != 0)	// the follow ended (or the subject was lost): the player keeps the last shown level
		{
			level_ = shown_;	// the follow end keeps the last shown level (L5)
		}
		following_ = false;
		subject_ = 0;
		pairValid_ = false;
		settledValid_ = false;
		resetLatch();
		shown_ = level_;
		return { level_, Reason::kPlayer, -1 };
	}
	if (subject_ != s.subject)
	{
		// A subject change: the stair latch belongs to the character that was followed before.
		pairValid_ = false;
		settledValid_ = false;
		resetLatch();
		subject_ = s.subject;
	}
	following_ = true;
	const Resolved r = resolve(s);
	pairValid_ = true;
	pairBuilding_ = r.building;
	pairFloor_ = r.floor;
	shown_ = r.floor;
	return { shown_, r.reason, r.building };
}

void FloorFollow::step(int dir, int top)
{
	// From the floor the player sees, clamped to the building's top (the old stepFloor's rule). A following caller has ended the follow (L4/L5):
	// the stepped level is the player's, and the next sample, not following, keeps it.
	top = std::max(top, 0);
	level_ = std::clamp(std::min(shown_, top) + dir, 0, top);
	shown_ = level_;
}

void FloorFollow::load(int level, uint64_t subject)
{
	level_ = std::clamp(level, 0, 3);
	shown_ = level_;
	subject_ = subject;
	pairValid_ = false;
	settledValid_ = false;
	resetLatch();
}
}
