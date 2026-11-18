// Same seed and same command sequence must produce the same state hash at
// every checkpoint, at any region worker count; a different seed must
// diverge, and a different command only from the tick it applies
// (ARCHITECTURE.md "Simulation": seeded RNG, deterministic tick).

#include "ksim/simulation.hpp"

#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

struct ScenarioResult
{
	std::vector<std::uint64_t> hashes;
	std::uint64_t relationFrom = 0;
	std::uint64_t relationTo = 0;
};

ksim::Simulation newGame(std::uint64_t seed, int workers)
{
	ksim::SpawnOptions options;
	options.seed = seed;
	ksim::RegionConfig regions;
	regions.workerThreads = workers;
	return ksim::Simulation::newGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA),
		options, nullptr, regions);
}

ScenarioResult runScenario(ksim::Simulation& simulation, std::int32_t freshValue = 33)
{
	ScenarioResult result;

	std::vector<std::uint64_t> factions;
	simulation.world().ecs().each([&factions](const ksim::state::Id& id,
		const ksim::state::FactionState&)
	{
		factions.push_back(id.value);
	});
	std::sort(factions.begin(), factions.end());
	test::require(factions.size() > 2, "too few faction instances");
	result.relationFrom = factions[0];
	result.relationTo = factions[1];

	ksim::state::GroundPoint player;
	simulation.world().ecs().each([&player](const ksim::state::Position& position)
	{
		player = ksim::state::GroundPoint{position.x, position.z};
	});

	result.hashes.push_back(simulation.stateHash()); // before the first tick
	for (std::uint64_t tick = 1; tick <= 100; ++tick)
	{
		if (tick == 1)
			simulation.enqueue(ksim::Command::setPlayerAreas({player}));
		if (tick == 5)
		{
			simulation.enqueue(ksim::Command::setFactionRelation(
				ksim::state::Id{factions[0]}, ksim::state::Id{factions[1]}, -77));
		}
		if (tick == 20)
		{
			simulation.enqueue(ksim::Command::setFactionRelation(
				ksim::state::Id{factions[2]}, ksim::state::Id{factions[0]}, freshValue));
		}
		if (tick == 50)
			simulation.enqueue(ksim::Command::setPlayerAreas({}));
		simulation.tick();
		if (tick == 10 || tick == 50 || tick == 100)
			result.hashes.push_back(simulation.stateHash());
	}
	return result;
}

void requireSameHashes(const ScenarioResult& left, const ScenarioResult& right,
	const std::string& label)
{
	test::require(left.hashes.size() == right.hashes.size(),
		label + ": checkpoint count differs");
	for (std::size_t i = 0; i < left.hashes.size(); ++i)
	{
		test::require(left.hashes[i] == right.hashes[i],
			label + ": diverged at checkpoint " + std::to_string(i) + ": "
			+ std::to_string(left.hashes[i]) + " != "
			+ std::to_string(right.hashes[i]));
	}
}

} // namespace

int main()
{
	try
	{
		ksim::Simulation first = newGame(20261030, 1);
		const ScenarioResult firstResult = runScenario(first);

		ksim::Simulation second = newGame(20261030, 4);
		const ScenarioResult secondResult = runScenario(second);

		ksim::Simulation other = newGame(424242, 1);
		const ScenarioResult otherResult = runScenario(other);

		// Same seed, one command differs: state is identical until the command
		// applies, then the hash diverges (a different seed alone proves
		// nothing).
		ksim::Simulation changed = newGame(20261030, 2);
		const ScenarioResult changedResult = runScenario(changed, 34);

		test::require(firstResult.relationFrom == secondResult.relationFrom
			&& firstResult.relationTo == secondResult.relationTo,
			"relation targets differ between same-seed games");

		requireSameHashes(firstResult, secondResult, "same seed, 1 vs 4 workers");

		test::require(firstResult.hashes[0] == changedResult.hashes[0]
			&& firstResult.hashes[1] == changedResult.hashes[1],
			"a later command changed state before it was applied");
		test::require(firstResult.hashes[2] != changedResult.hashes[2]
			&& firstResult.hashes[3] != changedResult.hashes[3],
			"a different command did not change the state hash");

		test::require(firstResult.hashes.front() != otherResult.hashes.front(),
			"different seeds produced the same initial hash");
		test::require(firstResult.hashes.back() != otherResult.hashes.back(),
			"different seeds produced the same final hash");

		std::cout << "ksim_determinism: same seed "
			<< std::hex << firstResult.hashes.back()
			<< ", other seed " << otherResult.hashes.back()
			<< ", different command " << changedResult.hashes.back()
			<< std::dec << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_determinism: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
