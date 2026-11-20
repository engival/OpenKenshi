#pragma once
// KSIM_PROFILE-only route counters (kenshi-k16.7.92): routeFrom's time histogram and planPath's roundabout / clearance counts, printed to stderr as
// one cumulative line every kEvery routes. Compiled out of a normal build; docs/PROFILING.md "One search per route".
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace knav::measure
{

constexpr std::uint64_t kEvery = 2000;
constexpr int kBuckets = 512; // 8 per octave of ns

struct Counters
{
	std::atomic<std::uint64_t> hist[kBuckets] = {};
	std::atomic<std::uint64_t> routes{0}, routeNs{0};
	std::atomic<std::uint64_t> plans{0}, planNs{0};
	std::atomic<std::uint64_t> probeFitsRoundabout{0}, clearanceNoFit{0}, clearanceWon{0}, clearanceNs{0}, roundaboutNs{0};
	std::atomic<std::uint64_t> walkCalls{0}, walkNs{0}; // combat's walkOrder (kenshi-k16.7.92 addition 2)
};

inline Counters counters;

inline std::uint64_t now() noexcept
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline double quantile(double q) noexcept
{
	std::uint64_t total = 0;
	for (const auto& b : counters.hist)
		total += b.load(std::memory_order_relaxed);
	const auto want = static_cast<std::uint64_t>(q * static_cast<double>(total));
	std::uint64_t seen = 0;
	for (int i = 0; i < kBuckets; ++i)
	{
		seen += counters.hist[i].load(std::memory_order_relaxed);
		if (seen > want)
			return std::exp2((i + 1) / 8.0) / 1000.0; // us, bucket upper bound
	}
	return 0.0;
}

inline void noteRoute(std::uint64_t ns) noexcept
{
	Counters& c = counters;
	int bucket = ns > 0 ? static_cast<int>(std::log2(static_cast<double>(ns)) * 8.0) : 0;
	bucket = bucket < kBuckets ? bucket : kBuckets - 1;
	c.hist[bucket].fetch_add(1, std::memory_order_relaxed);
	c.routeNs.fetch_add(ns, std::memory_order_relaxed);
	const std::uint64_t n = c.routes.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n % kEvery == 0)
		std::fprintf(stderr, "ROUTEMEASURE routes %llu mean_us %.2f p50_us %.1f p99_us %.1f plans %llu plan_ns %llu roundabout %llu clearance_nofit %llu"
			" clearance_won %llu clearance_ns %llu roundabout_ns %llu walk_calls %llu walk_ns %llu\n", static_cast<unsigned long long>(n),
			static_cast<double>(c.routeNs.load()) / static_cast<double>(n) / 1000.0, quantile(0.5), quantile(0.99),
			static_cast<unsigned long long>(c.plans.load()), static_cast<unsigned long long>(c.planNs.load()),
			static_cast<unsigned long long>(c.probeFitsRoundabout.load()), static_cast<unsigned long long>(c.clearanceNoFit.load()),
			static_cast<unsigned long long>(c.clearanceWon.load()), static_cast<unsigned long long>(c.clearanceNs.load()), static_cast<unsigned long long>(c.roundaboutNs.load()),
			static_cast<unsigned long long>(c.walkCalls.load()), static_cast<unsigned long long>(c.walkNs.load()));
}

// Times one combat walkOrder call (roomAhead probes included); the totals ride on the ROUTEMEASURE line.
struct WalkTimer
{
	std::uint64_t began = now();
	~WalkTimer()
	{
		counters.walkCalls.fetch_add(1, std::memory_order_relaxed);
		counters.walkNs.fetch_add(now() - began, std::memory_order_relaxed);
	}
};

}
