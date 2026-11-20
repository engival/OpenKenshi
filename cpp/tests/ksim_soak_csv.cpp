// The living-world CSV writer of the soak (docs/reference/LIVING_WORLD.md): a short soak with no player writes the header and one row
// per game-hour to both files, every row has the header's column count, the counts are plausible, and the writer is a pure read (the
// same state hash with and without it).
// argv[1]: the world bake (`knav_bake --world DIR`, the world_sample fixture). Exit 77 without it.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/soak.hpp"
#include "ksim/world/spawn.hpp"

#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr int kWindow[4] = {4, 7, 6, 9};
constexpr double kDays = 3.0 / 24.0; // three game-hours

std::uint64_t soakHash(const std::string& bake, std::ostream* csv, std::ostream* factionCsv, std::ostream* playerCsv = nullptr)
{
	SpawnOptions options;
	options.seed = 5;
	options.wildernessByDensity = false;
	RegionConfig regions;
	regions.workerThreads = 1;
	Simulation simulation = Simulation::newGame(KENSHI_DATA, kworld::resolveLoadOrder(KENSHI_DATA), options, nullptr, regions);
	AiSystem::Config config;
	config.registry = std::make_shared<const BehaviourRegistry>(simulation.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = KnavNavigator::open(KENSHI_DATA, bake, kWindow[0], kWindow[1], kWindow[2], kWindow[3], 4);
	simulation.enableAi(std::move(config));
	SoakOptions soak;
	soak.days = kDays;
	soak.csv = csv;
	soak.factionCsv = factionCsv;
	soak.playerCsv = playerCsv;
	runSoak(simulation, soak);
	return simulation.stateHash();
}

std::vector<std::string> lines(const std::string& text)
{
	std::vector<std::string> out;
	std::istringstream stream(text);
	for (std::string line; std::getline(stream, line);)
		out.push_back(line);
	return out;
}

std::size_t columns(const std::string& line)
{
	return static_cast<std::size_t>(std::count(line.begin(), line.end(), ',')) + 1;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2)
		{
			std::fprintf(stderr, "usage: ksim_soak_csv BAKE\n");
			return 2;
		}
		const std::string bake = argv[1];
		if (!std::filesystem::exists(bake))
		{
			std::fprintf(stderr, "ksim_soak_csv: no bake at %s (knav_bake --world DIR)\n", bake.c_str());
			return 77;
		}
		std::ostringstream world;
		std::ostringstream factions;
		std::ostringstream players;
		const std::uint64_t withCsv = soakHash(bake, &world, &factions, &players);
		const std::uint64_t without = soakHash(bake, nullptr, nullptr);
		test::require(withCsv == without, "writing the CSV changed the state hash");

		const std::vector<std::string> rows = lines(world.str());
		test::require(rows.size() == 4, "expected a header and 3 hourly rows, got " + std::to_string(rows.size()));
		test::require(rows[0] == kSoakCsvHeader, "the world CSV header is not kSoakCsvHeader");
		for (std::size_t i = 1; i < rows.size(); ++i)
		{
			test::require(columns(rows[i]) == columns(rows[0]), "row " + std::to_string(i) + " has the wrong column count");
			test::require(rows[i].rfind(std::to_string(i) + ",0,", 0) == 0, "row " + std::to_string(i) + " is not hour " + std::to_string(i));
		}
		// Squads and members exist, and the off-screen death columns are the fixed zeros of this baseline.
		test::require(rows[1].rfind("1,0,0,", 0) != 0, "no squads in the first row");

		const std::vector<std::string> factionRows = lines(factions.str());
		test::require(factionRows.size() > 3, "the faction CSV has no rows");
		test::require(factionRows[0] == kSoakFactionCsvHeader, "the faction CSV header is not kSoakFactionCsvHeader");
		for (std::size_t i = 1; i < factionRows.size(); ++i)
			test::require(columns(factionRows[i]) == 4, "faction row " + std::to_string(i) + " has the wrong column count");

		// No player faction in a new game's soak: the player file is its header alone (a scenario with a player squad writes its members).
		test::require(players.str() == std::string(kSoakPlayerCsvHeader) + "\n", "the player CSV is not its header alone");

		std::printf("soak csv: %zu hourly rows, %zu faction rows, hash %llx with and without\n", rows.size() - 1,
			factionRows.size() - 1, static_cast<unsigned long long>(withCsv));
		std::cout << "ksim_soak_csv: ok\n";
	} catch (const test::Failure& failure) {
		std::fprintf(stderr, "ksim_soak_csv: %s\n", failure.what());
		return 1;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "ksim_soak_csv: %s\n", error.what());
		return 1;
	}
	return 0;
}
