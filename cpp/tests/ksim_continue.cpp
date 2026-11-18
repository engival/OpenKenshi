// Continuation on real game data: a new game run N ticks, saved, loaded and
// run M more ticks reaches the same state hash as an uninterrupted N + M run,
// whatever the region worker counts before and after the load. The run
// crosses squad ticks and a strategic (game-hour) tick on both sides of the
// save, has the stand-in workload moving every squad, and saves with a
// command still queued.

#include "ksim/simulation.hpp"

#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

// New game Clock 74,182; the strategic tick is 74,188 (34 x 2182), squad
// ticks every 20.
constexpr std::uint64_t kBefore = 30;
constexpr std::uint64_t kAfter = 40;

ksim::RegionConfig regions(int workers)
{
	ksim::RegionConfig config;
	config.workerThreads = workers;
	config.standIn = true;
	config.fullDetailAll = true;
	return config;
}

struct Factions
{
	std::uint64_t first = 0;
	std::uint64_t second = 0;
};

Factions twoFactions(const ksim::Simulation& simulation)
{
	std::vector<std::uint64_t> ids;
	simulation.world().ecs().each([&ids](const ksim::state::Id& id,
		const ksim::state::FactionState&)
	{
		ids.push_back(id.value);
	});
	std::sort(ids.begin(), ids.end());
	test::require(ids.size() >= 2, "too few faction instances");
	return Factions{ids[0], ids[1]};
}

// The first N ticks, with commands before and across the save point.
void runBefore(ksim::Simulation& simulation)
{
	const Factions factions = twoFactions(simulation);
	simulation.enqueue(ksim::Command::setPlayerAreas({{0.0, 0.0}}));
	simulation.run(kBefore - 5);
	ksim::Command later = ksim::Command::setFactionRelation(
		ksim::state::Id{factions.first}, ksim::state::Id{factions.second}, -55);
	later.tick = simulation.clockTick() + 10;
	simulation.enqueue(later);
	simulation.run(5);
}

std::string bytesOf(const ksim::Simulation& simulation)
{
	std::ostringstream out;
	simulation.save(out);
	return out.str();
}

std::string fileBytes(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		ksim::SpawnOptions options;
		options.seed = 77;
		options.settleSeconds = 0; // the test pins the t0 clock against a strategic tick

		ksim::Simulation reference = ksim::Simulation::newGame(KENSHI_DATA, order,
			options, nullptr, regions(1));
		const std::uint64_t start = reference.clockTick();
		test::require(start / ksim::Simulation::kStrategicIntervalTicks
			!= (start + kBefore) / ksim::Simulation::kStrategicIntervalTicks,
			"the run does not cross a strategic tick");
		runBefore(reference);
		const std::uint64_t referenceSplit = reference.stateHash();
		test::require(reference.systems()[2].runs == 1, "no strategic run before the save");
		test::require(reference.regions().stats().entityUpdates > 0
			&& reference.regions().stats().hitsDelivered > 0,
			"the stand-in workload did not move or hit anything");
		reference.run(kAfter);
		const std::uint64_t referenceEnd = reference.stateHash();

		const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_continue_test.ksav";
		{
			ksim::Simulation split = ksim::Simulation::newGame(KENSHI_DATA, order,
				options, nullptr, regions(4));
			runBefore(split);
			test::require(split.stateHash() == referenceSplit,
				"4 workers differ from 1 before the save");
			test::require(split.pendingCommands() == 1, "no command queued at the save");
			split.saveGame(path);
		}
		const std::string saved = fileBytes(path);

		for (const int workers : {8, 1})
		{
			ksim::Simulation loaded = ksim::Simulation::loadGame(KENSHI_DATA, order, path,
				false, nullptr, regions(workers));
			test::require(loaded.stateHash() == referenceSplit,
				"the loaded game hashes differently");
			test::require(bytesOf(loaded) == saved,
				"the loaded game does not re-save to the same bytes");
			loaded.run(kAfter);
			test::require(loaded.stateHash() == referenceEnd,
				"save at 4 workers, load at " + std::to_string(workers)
				+ ": continuation differs from the uninterrupted run");
		}
		std::remove(path.c_str());

		std::cout << "ksim_continue: " << kBefore << " + " << kAfter << " ticks from "
			<< start << ", hash " << std::hex << referenceEnd << std::dec
			<< ", 1 = 4 -> 8 = 4 -> 1 workers\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_continue: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
