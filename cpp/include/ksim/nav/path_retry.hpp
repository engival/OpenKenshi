// The retry ladder of a failed path request (HAVOKCHAR_EXE.md 2.2, CharMovement vf18 / 140661270, claims C57, C166-C168, C171-C172; LOCOMOTION.md 9, D8).
//
// The exe asks again every time the owner re-issues the move: each moveTo while the last request FAILED bumps a counter and offsets the goal
// sideways by 100 x n x (1 + 0.2 n) (sign by parity of n); the 17th is the road-network fallback; after it the character is "completely stuck"
// and is not asked for again. A new goal (or invalidate) starts over. Ours: the owners that re-issue a walk each second (world tasks, carry,
// goals) call Simulation::routeRetry, which asks this ladder what to ask the navigator. Runtime only, never saved.

#pragma once

#include <cmath>
#include <cstdint>
#include <map>

namespace ksim {

class PathRetries
{
public:
	// The exe's counts: 16 sideways retries, then the road network once, then stuck.
	static constexpr int kSideways = 16;
	static constexpr double kSameGoal = 1.0; // a goal within this of the last one (xz) is the same goal

	enum class Step { Direct, Sideways, RoadNet, Stuck };
	struct Ask
	{
		Step step = Step::Direct;
		double x = 0.0;
		double z = 0.0;
		int retries = 0; // n, 0 for the goal itself
	};

	// The sideways offset of retry n, game units (UNSURE whether the exe's 100 x n x (1 + 0.2 n) is game units or Havok units; this is game units).
	static double offset(int n) { return 100.0 * n * (1.0 + 0.2 * n); }

	// What to ask for `id` walking from (fromX, fromZ) to the goal (goalX, goalZ): the goal, or after a failed ask for the same goal the next rung.
	Ask ask(std::uint64_t id, double fromX, double fromZ, double goalX, double goalZ)
	{
		Entry& e = entries_[id];
		if (e.seen && std::hypot(goalX - e.goalX, goalZ - e.goalZ) > kSameGoal)
			e = Entry{};
		e.seen = true;
		e.goalX = goalX;
		e.goalZ = goalZ;
		Ask a{Step::Direct, goalX, goalZ, e.n};
		if (!e.failed)
			return a; // the last ask found a route (or this is the first): the goal itself
		if (e.n <= kSideways + 1)
			++e.n; // only a request after a failed one counts (C166); the count stops at "stuck"
		a.retries = e.n;
		if (e.n <= kSideways)
		{
			const double dx = goalX - fromX;
			const double dz = goalZ - fromZ;
			const double len = std::hypot(dx, dz);
			// To the side of the line from the walker to the goal; a goal on the walker's own spot offsets along +x.
			const double sx = len > 1.0e-6 ? -dz / len : 1.0;
			const double sz = len > 1.0e-6 ? dx / len : 0.0;
			const double d = (e.n % 2 == 1 ? 1.0 : -1.0) * offset(e.n);
			a.step = Step::Sideways;
			a.x = goalX + sx * d;
			a.z = goalZ + sz * d;
		} else if (e.n == kSideways + 1) {
			a.step = Step::RoadNet;
		} else {
			a.step = Step::Stuck;
		}
		return a;
	}

	// The outcome of the ask just made for `id`: a failed route makes the next ask a retry. A route found for the goal itself ends the ladder;
	// one found for a sideways goal keeps the count (the owner's next ask is for the original goal again).
	void result(std::uint64_t id, bool found)
	{
		Entry& e = entries_[id];
		if (found && e.n == 0)
			entries_.erase(id);
		else
			e.failed = !found;
	}

	// Count of retries made for `id`'s current goal (0 when none).
	int retries(std::uint64_t id) const
	{
		const auto it = entries_.find(id);
		return it == entries_.end() ? 0 : it->second.n;
	}
	// The owner's failure it can act on (the exe's vf10 "failed for good"): every rung is used up.
	bool stuck(std::uint64_t id) const { return retries(id) > kSideways + 1; }
	// Whether `id` asked for this same goal (within kSameGoal) fewer than `window` ticks before `now`: a walker the owner re-asks every tick
	// (a route found, the walk not started: a body in the way) must not cost a search per tick. A fresh or different ask is recorded and false.
	bool askedRecently(std::uint64_t id, double goalX, double goalZ, std::uint64_t now, std::uint64_t window)
	{
		Stamp& s = stamps_[id];
		if (s.seen && now - s.tick < window && std::hypot(goalX - s.goalX, goalZ - s.goalZ) <= kSameGoal)
			return true;
		s = Stamp{true, now, goalX, goalZ};
		return false;
	}
	// invalidate (C57): a navmesh change zeroes everyone's counter; a character leaving the world drops its own.
	void reset(std::uint64_t id) { entries_.erase(id); stamps_.erase(id); }
	void resetAll() { entries_.clear(); stamps_.clear(); }

private:
	struct Entry
	{
		bool seen = false;
		bool failed = false;
		int n = 0;
		double goalX = 0.0;
		double goalZ = 0.0;
	};
	struct Stamp
	{
		bool seen = false;
		std::uint64_t tick = 0;
		double goalX = 0.0;
		double goalZ = 0.0;
	};
	std::map<std::uint64_t, Entry> entries_;
	std::map<std::uint64_t, Stamp> stamps_;
};

} // namespace ksim
