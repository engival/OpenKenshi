// DEV ONLY (include/ksim/dev/walk_stats.hpp).
#include "ksim/dev/walk_stats.hpp"

#include <cstdio>
#include <cstdlib>

namespace ksim::dev {

namespace {
bool enabled()
{
	static const bool on = std::getenv("KSIM_WALK_STATS") != nullptr;
	return on;
}
} // namespace

void WalkStats::print(const char* when) const
{
	const double minutes = walkSeconds / 60.0;
	const std::uint64_t all = windowReplans + stuckReplans + blockedReplans;
	std::fprintf(stderr, "WALKSTATS %s walker_min=%.2f replans=%llu (%.2f/min) window=%llu [lost=%llu (ahead=%llu behind=%llu) real=%llu failed=%llu] stuck=%llu blocked=%llu "
		"failed=%llu abandoned=%llu | ended=%llu stuckArrive=%llu gapMean=%.2f gapMax=%.2f endGapMean=%.2f endGapMax=%.2f over1=%llu\n",
		when, minutes, (unsigned long long) all, minutes > 0.0 ? all / minutes : 0.0, (unsigned long long) windowReplans, (unsigned long long) windowLost,
		(unsigned long long) windowLostAhead, (unsigned long long) windowLostBehind, (unsigned long long) windowReal, (unsigned long long) windowFailed,
		(unsigned long long) stuckReplans, (unsigned long long) blockedReplans, (unsigned long long) replanFailed, (unsigned long long) abandoned,
		(unsigned long long) walks, (unsigned long long) stuckArrivals, stuckArrivals ? stuckArrivalGapSum / stuckArrivals : 0.0, stuckArrivalGapMax,
		walks ? endGapSum / walks : 0.0, endGapMax, (unsigned long long) endOver1);
}

void WalkStats::maybePrint(double time)
{
	if (enabled() && time >= nextPrint)
	{
		nextPrint = time + 600.0;
		print("periodic");
	}
}

WalkStats::~WalkStats()
{
	if (enabled() && walkSeconds > 0.0)
		print("final");
}

} // namespace ksim::dev
