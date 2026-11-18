// Performance smoke test: 20,000 stand-in entities at 1 and 8 worker
// threads. Not a hard threshold; it reports ms per tick for NOTES.md and
// checks that the threaded result still matches the single-threaded one.

#include "ksim/world/region.hpp"
#include "ksim/core/simulation.hpp"

#include "ksim_region_test_util.hpp"
#include "test_util.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kEntities = 20000;
constexpr std::uint64_t kWarmupTicks = 2;
constexpr std::uint64_t kTimedTicks = 10;

struct Run
{
	std::uint64_t hash = 0;
	double msPerTick = 0.0;
	std::uint64_t writes = 0;
	std::uint64_t violations = 0;
	std::uint64_t activeWorkers = 0;
	std::string phases;
};

Run run(int workers)
{
	ksim::RegionConfig config = region_test::config(workers, true);
	config.perceiveRadius = 300.0;
	config.keepRadius = 900.0;
	config.moveSpeed = 6.0;
	config.squadSpeed = 3.0;
	ksim::Simulation sim = region_test::simulation(8080, config);
	const ksim::RegionEngine& engine = sim.regions();

	const std::vector<std::pair<double, double>> positions
		= region_test::scattered(kEntities, 1001, 5000.0);
	for (std::size_t i = 0; i < positions.size(); ++i)
		sim.spawnMover(positions[i].first, positions[i].second,
			static_cast<std::int32_t>(i / 10));

	sim.run(kWarmupTicks);
	const auto start = std::chrono::steady_clock::now();
	sim.run(kTimedTicks);
	const auto elapsed = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - start).count();

	Run result;
	result.msPerTick = elapsed * 1000.0
		/ static_cast<double>(kTimedTicks);
	result.hash = sim.stateHash();
	result.writes = engine.stats().writes;
	result.violations = engine.stats().writeViolations;
	result.activeWorkers = engine.stats().maxActiveWorkers;
	const ksim::RegionStats& stats = engine.stats();
	const double ticks = static_cast<double>(stats.ticks);
	const auto ms = [ticks](std::uint64_t ns) { return static_cast<double>(ns) / 1e6 / ticks; };
	result.phases = "prepare " + std::to_string(ms(stats.nsPrepare))
		+ " phase1 " + std::to_string(ms(stats.nsPhase1))
		+ " sort " + std::to_string(ms(stats.nsSort))
		+ " phase2 " + std::to_string(ms(stats.nsPhase2))
		+ " boundary " + std::to_string(ms(stats.nsBoundary));
	return result;
}

} // namespace

int main()
{
	try
	{
		const Run single = run(1);
		const Run threaded = run(8);

		test::require(threaded.hash == single.hash,
			"threaded state differs from single-threaded");
		test::require(single.violations == 0 && threaded.violations == 0,
			"ownership violation during the performance run");
		test::require(threaded.activeWorkers > 1,
			"the 8-worker run never used more than one thread");
		test::require(single.writes > 0 && threaded.writes > 0,
			"no writes were audited");

		std::cout << "ksim_regions_perf: " << kEntities << " entities, "
			<< kTimedTicks << " ticks; N=1 " << single.msPerTick
			<< " ms/tick, N=8 " << threaded.msPerTick << " ms/tick\n  N=1 ms/tick by segment: " << single.phases
			<< "\n  N=8 ms/tick by segment: " << threaded.phases << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_regions_perf: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
