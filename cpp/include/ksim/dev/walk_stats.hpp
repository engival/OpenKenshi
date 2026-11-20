// DEV ONLY: walkOrder re-plan counters by reason (kenshi-k16.8.39). Printed to stderr at the arena's end, and every 600 sim s, when KSIM_WALK_STATS is set.
#pragma once

#include <cstdint>

namespace ksim::dev {

struct WalkStats
{
	double walkSeconds = 0.0; // walker-seconds: dt per walkOrder tick that moved (not held)
	std::uint64_t windowReplans = 0; // off the route window for > kOffPathRepath, re-plan done
	std::uint64_t windowLost = 0; // of them: whole-route distance within the reach (the window lost its place, the walker is on the route)
	std::uint64_t windowReal = 0; // of them: beyond the reach of the whole route (really off it)
	std::uint64_t windowLostBehind = 0; // of the lost: the walker is nearest a segment before the window's start (routeNext ran ahead)
	std::uint64_t windowLostAhead = 0; // of the lost: nearest a segment past the window's end (routeNext lagging)
	std::uint64_t windowFailed = 0; // the re-plan found nothing (old route kept)
	std::uint64_t stuckReplans = 0; // walkStuck >= kStuckRepath, re-plan done
	std::uint64_t blockedReplans = 0; // edge block aimed at the waypoint, re-plan done
	std::uint64_t replanFailed = 0; // stuck / blocked re-plan found nothing or was deduped
	std::uint64_t abandoned = 0; // blocked, out of re-paths: route dropped
	std::uint64_t walks = 0; // walks that ended by arrival (routeNext past the end)
	std::uint64_t stuckArrivals = 0; // ended by the stuck rule (within 2 footprints, not within kArriveReach)
	double stuckArrivalGapSum = 0.0;
	double stuckArrivalGapMax = 0.0;
	double endGapSum = 0.0; // the distance of arrivals from the route's last point
	double endGapMax = 0.0;
	std::uint64_t endOver1 = 0; // arrivals ended with the walker > 1.0 u from the route's last point
	double nextPrint = 600.0;

	~WalkStats();
	void print(const char* when) const;
	void maybePrint(double time);
};

} // namespace ksim::dev
