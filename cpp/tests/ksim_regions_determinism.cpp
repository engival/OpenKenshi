// The full state hash after K ticks must be identical for 1, 2, 4 and 8
// worker threads, and for repeated runs with the same seed. A different
// command sequence must change the hash (ARCHITECTURE.md "Threading":
// "Results must not depend on the thread count").
//
// Continuation: a run saved at tick 40 and restored (at another thread
// count, with a command still queued) must reach the same hashes as the
// uninterrupted run, and re-save to the same bytes.

#include "ksim/region.hpp"
#include "ksim/save.hpp"
#include "ksim/simulation.hpp"

#include "ksim_region_test_util.hpp"
#include "test_util.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kSeed = 20261030;
constexpr std::uint64_t kSplitTick = 40;

struct Scenario
{
	std::vector<std::uint64_t> hashes;
};

ksim::RegionConfig scenarioConfig(int workers, double regionSize)
{
	ksim::RegionConfig config = region_test::config(workers, false);
	config.regionSize = regionSize;
	config.perceiveRadius = 250.0;
	config.keepRadius = 700.0;
	config.moveSpeed = 40.0;
	config.squadSpeed = 15.0;
	return config;
}

std::string saved(const ksim::Simulation& sim)
{
	std::ostringstream out;
	sim.save(out);
	return out.str();
}

// resumeWorkers > 0: save at kSplitTick and continue in a restored
// simulation with that many workers.
Scenario run(int workers, bool extraCommand, double regionSize = 4608.0,
	int resumeWorkers = 0)
{
	ksim::Simulation sim = region_test::simulation(kSeed,
		scenarioConfig(workers, regionSize));

	const std::vector<std::pair<double, double>> positions
		= region_test::scattered(600, 777);
	for (std::size_t i = 0; i < positions.size(); ++i)
		sim.spawnMover(positions[i].first, positions[i].second,
			static_cast<std::int32_t>(i / 6));

	// Two overlapping player positions merge into one full-detail area.
	sim.enqueue(ksim::Command::setPlayerAreas({{-90000.0, -80000.0},
		{-89000.0, -80000.0}}));

	Scenario scenario;
	scenario.hashes.push_back(sim.stateHash());
	for (std::uint64_t tick = 1; tick <= 100; ++tick)
	{
		if (tick == 5 && extraCommand)
			sim.enqueue(ksim::Command::setSquadCentre(7, 0.0, 0.0));
		if (tick == kSplitTick + 1)
		{
			// Queued now, due after the split.
			ksim::Command later = ksim::Command::setSquadCentre(3, 1000.0, 2000.0);
			later.tick = kSplitTick + 5;
			sim.enqueue(later);
			if (resumeWorkers > 0)
			{
				const std::string bytes = saved(sim);
				std::istringstream in(bytes);
				sim = ksim::Simulation::restore(ksim::World::empty(), in,
					scenarioConfig(resumeWorkers, regionSize));
				test::require(sim.pendingCommands() == 1,
					"the queued command was not restored");
				test::require(saved(sim) == bytes,
					"a restored simulation does not re-save to the same bytes");
			}
		}
		sim.tick();
		if (tick == 1)
			test::require(sim.regions().areas().size() == 1,
				"overlapping areas did not merge");
		if (tick == 10 || tick == 50 || tick == 100)
			scenario.hashes.push_back(sim.stateHash());
	}
	return scenario;
}

void requireSame(const Scenario& left, const Scenario& right,
	const std::string& label)
{
	test::require(left.hashes.size() == right.hashes.size(),
		label + ": checkpoint count differs");
	for (std::size_t i = 0; i < left.hashes.size(); ++i)
	{
		test::require(left.hashes[i] == right.hashes[i],
			label + ": diverged at checkpoint " + std::to_string(i));
	}
}

} // namespace

int main()
{
	try
	{
		const Scenario one = run(1, false);
		const Scenario two = run(2, false);
		const Scenario four = run(4, false);
		const Scenario eight = run(8, false);
		requireSame(one, two, "1 vs 2 threads");
		requireSame(one, four, "1 vs 4 threads");
		requireSame(one, eight, "1 vs 8 threads");

		// Same seed and same schedule twice.
		const Scenario repeat = run(4, false);
		requireSame(four, repeat, "same seed repeat");

		// Save, load, continue: same as the uninterrupted run.
		requireSame(one, run(1, false, 4608.0, 1), "save/load 1 -> 1 threads");
		requireSame(one, run(1, false, 4608.0, 8), "save/load 1 -> 8 threads");
		requireSame(one, run(8, false, 4608.0, 2), "save/load 8 -> 2 threads");

		// The region size is a configurable multiple of 4,608 units.
		const Scenario wideSingle = run(1, false, 9216.0);
		const Scenario wideThreaded = run(8, false, 9216.0);
		requireSame(wideSingle, wideThreaded, "9216-unit regions: 1 vs 8");

		// A different command sequence changes real state, not just the seed.
		const Scenario commanded = run(4, true);
		test::require(one.hashes.front() == commanded.hashes.front(),
			"command run differs at checkpoint 0");
		bool diverged = false;
		for (std::size_t i = 1; i < one.hashes.size(); ++i)
			diverged = diverged || one.hashes[i] != commanded.hashes[i];
		test::require(diverged, "a different command did not change the hash");

		std::cout << "ksim_regions_determinism: N=1/2/4/8 and save/load "
			<< std::hex << one.hashes.back() << std::dec
			<< ", command " << std::hex << commanded.hashes.back() << std::dec
			<< "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_regions_determinism: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
