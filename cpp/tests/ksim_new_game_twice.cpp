// Two NEW_GAMEs in a row on one server process (order 84 new_world, then NEW_GAME again), the NEW_GAME after a load, and a load after a NEW_GAME
// (TOWN_LIFE_SLICE 5.14 "second NEW_GAME"). The server's setup: Hub start, streaming navmesh, squad AI, --log-world-squads. The second game must
// build the same world squads as the first and as a fresh process; the resident pass's derived state (ResidentData) used to survive Simulation::reload,
// so the second game saw its squads as already built and promoted none.
// Needs the world bake (argv[1]; exit 77 without it).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "knav/navmesh.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "nav_source.hpp"
#include "ksim/dev/build_paths.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

// One game's observations: the promotes counted from the sim, and what the log said.
struct Seen
{
	std::uint64_t promoted = 0;
	std::size_t promoteLines = 0;
	std::size_t builtMax = 0; // the largest "N built" of a "world squads at" line
};

std::size_t builtIn(const std::vector<std::string>& lines)
{
	std::size_t built = 0;
	for (const std::string& line : lines)
	{
		if (line.rfind("world squads at ", 0) == 0)
			built = std::max(built, static_cast<std::size_t>(std::stoul(line.substr(line.find(": ") + 2))));
	}
	return built;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_new_game_twice: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const std::string world = argv[1];
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = 1;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		sim.markPristine();
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combat, settings);
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		}
		sim.enableNavigation(test::openWorldNav(world, hubZones));
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim.gameData());
		sim.enableAi(std::move(ai));
		std::vector<std::string> logLines;
		sim.setWorldSquadLog(1, [&](const std::string& line) { logLines.push_back(line); });
		std::string sid;
		for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
		{
			if (record->name == "Wanderer")
				sid = record->stringID;
		}

		const std::uint64_t span = static_cast<std::uint64_t>(kTicksPerGameMinute * 1.5);
		std::uint64_t ticket = 0;
		// Starts a game on the current world, runs a game minute and a half, and reports what was built.
		const auto play = [&]() -> Seen
		{
			Seen seen;
			const std::uint64_t before = sim.residentStats().worldPromotedSquads;
			logLines.clear();
			sim.enqueue(Command::newGame(sid, "Tester", ++ticket));
			sim.run(span);
			seen.promoted = sim.residentStats().worldPromotedSquads - before;
			for (const std::string& line : logLines)
				seen.promoteLines += line.rfind("world squad promote ", 0) == 0;
			seen.builtMax = builtIn(logLines);
			return seen;
		};
		const auto describe = [](const char* what, const Seen& seen)
		{
			std::cout << "ksim_new_game_twice: " << what << ": " << seen.promoted << " world squads promoted, " << seen.promoteLines
				<< " promote lines, up to " << seen.builtMax << " built in the log\n";
		};

		const Seen first = play();
		describe("first NEW_GAME", first);
		test::require(first.promoted >= 1 && first.promoteLines == first.promoted && first.builtMax >= 1, "the first game builds world squads and logs them");
		std::ostringstream saved; // a game in progress, for the load below
		sim.save(saved);

		sim.newWorld();
		const Seen second = play();
		describe("second NEW_GAME", second);
		test::require(second.promoted == first.promoted && second.promoteLines == first.promoted, "the second game promotes the same world squads as the first");
		test::require(second.builtMax == first.builtMax, "the second game logs the same built squads (the log's clock restarts with the world)");

		// A load of the first game's save over the second game: its built world squads are known again.
		std::istringstream again(saved.str());
		sim.reload(again);
		logLines.clear();
		sim.run(span);
		const std::size_t loadedBuilt = builtIn(logLines);
		std::cout << "ksim_new_game_twice: load after NEW_GAME: up to " << loadedBuilt << " built in the log\n";
		test::require(loadedBuilt >= 1, "a load after a NEW_GAME knows the saved game's built world squads");

		// And a NEW_GAME after that load.
		sim.newWorld();
		const Seen third = play();
		describe("NEW_GAME after a load", third);
		test::require(third.promoted == first.promoted && third.builtMax == first.builtMax, "a NEW_GAME after a load promotes the same world squads as a fresh process");

		// ksim_server --money: NEW_GAME's wallet is the override, not the start's money.
		const std::int64_t startMoney = sim.playerSquads().money;
		sim.setNewGameMoney(startMoney + 4321);
		sim.newWorld();
		play();
		std::cout << "ksim_new_game_twice: --money: start " << startMoney << ", override " << sim.playerSquads().money << "\n";
		test::require(sim.playerSquads().money == startMoney + 4321, "a NEW_GAME with setNewGameMoney gives that wallet");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_new_game_twice: FAILED: " << e.what() << "\n";
		return 1;
	}
}
