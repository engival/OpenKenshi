#pragma once
// KSIM_PROFILE-only route counters (kenshi-k16.7.92): routeFrom's time histogram and planPath's roundabout / clearance counts, printed to stderr as
// one cumulative line every kEvery routes. Compiled out of a normal build; docs/PROFILING.md "One search per route".
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

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

// Nav work that runs on the tick thread, charged to the tick it ran in (the slow-tick line, KSIM_PROFILE_SLOW_MS): wall ns of the
// region conversion (shipped tiles or the disk cache), the stale check (includes a rebake), the zone rebake itself and a clearance
// component label build. Nested stretches overlap on purpose: stale includes bake, convert does not include stale.
enum Load : int { kLoadConvert, kLoadStale, kLoadBake, kLoadComponents, kLoads };
inline constexpr const char* kLoadNames[kLoads] = {"nav_convert", "nav_stale", "nav_bake", "nav_components"};
inline std::atomic<std::uint64_t> loadNs[kLoads] = {};
inline std::atomic<std::uint32_t> loadCount[kLoads] = {};

struct LoadScope
{
	explicit LoadScope(Load which) noexcept : which_(which), start_(std::chrono::steady_clock::now()) {}
	~LoadScope()
	{
		loadNs[which_].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count()), std::memory_order_relaxed);
		loadCount[which_].fetch_add(1, std::memory_order_relaxed);
	}
	LoadScope(const LoadScope&) = delete;
	LoadScope& operator=(const LoadScope&) = delete;
	Load which_;
	std::chrono::steady_clock::time_point start_;
};

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

// Per-call route log (kenshi-k16.6.305): KSIM_ROUTELOG=<path> writes one CSV row per routeFrom call. The sim is one thread, so the
// call record is thread_local. `tick` is set by the Profiler at the end of each tick; `who` / `tag` by a caller that knows them
// (combat: the member id, tag 1); `lastSub` is the last profiler mark passed (the work belongs to the stretch after it).
struct Call
{
	std::uint32_t searches = 0; // planPathFrom runs (the refit plan, plus the own-polygon plan after a banded start)
	std::uint32_t findPaths = 0; // NavQuery::findPath calls (planRoute may ask for more than one)
	std::uint64_t probeNodes = 0; // Detour nodes of the plain probe findPath (or of the whole search at radius 0)
	std::uint64_t clearNodes = 0; // clearance A* nodes, first pass
	std::uint64_t againNodes = 0; // the second clearance pass (goal-exempt retry)
	std::uint32_t clearRuns = 0;
};
inline thread_local Call call;
inline thread_local std::uint64_t who = 0;
inline thread_local std::uint8_t tag = 0;
inline std::atomic<std::uint64_t> tick{0};
inline std::atomic<std::uint32_t> lastSub{0};

struct WhoScope
{
	std::uint64_t oldWho = who;
	std::uint8_t oldTag = tag;
	WhoScope(std::uint64_t id, std::uint8_t t) noexcept { who = id; tag = t; }
	~WhoScope() { who = oldWho; tag = oldTag; }
};

inline std::FILE* routeLog() noexcept
{
	static std::FILE* const file = []() -> std::FILE*
	{
		const char* path = std::getenv("KSIM_ROUTELOG");
		if (path == nullptr || *path == 0)
			return nullptr;
		std::FILE* f = std::fopen(path, "w");
		if (f != nullptr)
			std::fputs("tick,last_sub,tag,who,wall_us,found,planned,radius,fromx,fromz,tox,toz,searches,find_paths,probe_nodes,clear_nodes,again_nodes,clear_runs,approach,plain_failed\n", f);
		return f;
	}();
	return file;
}

inline void logRoute(std::uint64_t ns, bool found, bool planned, double radius, double fx, double fz, double tx, double tz, bool approach, bool plainFailed) noexcept
{
	if (std::FILE* f = routeLog())
	{
		const Call& c = call;
		std::fprintf(f, "%llu,%u,%u,%llu,%.1f,%d,%d,%.2f,%.0f,%.0f,%.0f,%.0f,%u,%u,%llu,%llu,%llu,%u,%d,%d\n", static_cast<unsigned long long>(tick.load(std::memory_order_relaxed)),
			lastSub.load(std::memory_order_relaxed), static_cast<unsigned>(tag), static_cast<unsigned long long>(who), static_cast<double>(ns) / 1000.0, found ? 1 : 0,
			planned ? 1 : 0, radius, fx, fz, tx, tz, c.searches, c.findPaths, static_cast<unsigned long long>(c.probeNodes), static_cast<unsigned long long>(c.clearNodes),
			static_cast<unsigned long long>(c.againNodes), c.clearRuns, approach ? 1 : 0, plainFailed ? 1 : 0);
		if (counters.routes.load(std::memory_order_relaxed) % 256 == 0)
			std::fflush(f);
	}
}

}
