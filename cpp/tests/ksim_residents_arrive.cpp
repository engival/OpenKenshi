// kenshi-k16.6.31: a fresh New Game's Hub residents start where their squad token is and the scorer walks them to their sites (vanilla's start
// taskless where they were saved and assign themselves within 30 s), instead of being placed on the site at promotion (TOWN_LIFE_SLICE 5.3 keeps
// the placement pass for a loaded world and for a demote/promote cycle). Setup as tools/ksim_hub_census: New Game at the Hub, t0 is the first tick
// the residents are Fighters, t1 is 30 s later. Asserts: most residents near the Hub's bar walk >1 u in those 30 s, and some walk >200 u.
//
// argv[1]: the world navmesh bake directory (knav_cache).

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/factions.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "knav/navmesh.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "nav_source.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

int main(int argc, char** argv)
try
{
	if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
	{
		std::cout << "ksim_residents_arrive: SKIPPED: needs the world navmesh bake (pass its directory)\n";
		return 77;
	}
	constexpr double kCx = -50986.0;
	constexpr double kCz = 3514.0;
	constexpr double kRadius = 1200.0;
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
	SpawnOptions options;
	options.seed = 1;
	options.settleSeconds = 0.0; // the walk-in itself, watched from t0 (TOWN_LIFE_SLICE 5.40 settles it before the first frame)
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	sim.markPristine();
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(CombatData::build(*sim.gameData(), KENSHI_DATA), settings);
	std::vector<kworld::ZoneCell> hubZones;
	for (int dz = -1; dz <= 1; ++dz)
	{
		for (int dx = -1; dx <= 1; ++dx)
			hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
	}
	sim.enableNavigation(test::openWorldNav(argv[1], hubZones));
	AiSystem::Config ai;
	ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	ai.navigator = KnavNavigator::open(KENSHI_DATA, argv[1], 4, 7, 6, 9, 4);
	ai.factions = FactionBook::build(sim.gameData());
	sim.enableAi(std::move(ai));
	std::string sid;
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
	{
		if (record->name == "Wanderer")
			sid = record->stringID;
	}
	sim.enqueue(Command::newGame(sid, "Tester", 1));
	sim.run(3);
	const auto near = [&](const CombatArena::Member& m)
	{
		return !m.fighter.playerControlled && std::hypot(m.fighter.x - kCx, m.fighter.z - kCz) <= kRadius;
	};
	const auto count = [&]
	{
		std::size_t n = 0;
		for (const CombatArena::Member& m : sim.combat()->members())
			n += near(m) ? 1 : 0;
		return n;
	};
	for (int i = 0; i < 400 && count() == 0; ++i)
		sim.run(1);
	std::map<std::uint64_t, std::pair<double, double>> start;
	for (const CombatArena::Member& m : sim.combat()->members())
	{
		if (near(m))
			start[m.id] = {m.fighter.x, m.fighter.z};
	}
	sim.run(static_cast<std::uint64_t>(30.0 / kTickSeconds));
	std::size_t moved = 0;
	std::size_t far = 0;
	for (const CombatArena::Member& m : sim.combat()->members())
	{
		const auto it = start.find(m.id);
		if (it == start.end())
			continue;
		const double d = std::hypot(m.fighter.x - it->second.first, m.fighter.z - it->second.second);
		moved += d > 1.0 ? 1 : 0;
		far += d > 200.0 ? 1 : 0;
	}
	std::cout << "residents " << start.size() << ", moved >1 u " << moved << ", >200 u " << far << "\n";
	test::require(start.size() >= 20, "the Hub's residents are promoted at load");
	test::require(moved * 2 >= start.size(), "a fresh start's residents walk to their sites (vanilla: 23 of 26 move) and are not placed on them");
	test::require(far >= 4, "some walk a long way in 30 s (vanilla: 11 move >200 u)");
	std::cout << "ksim_residents_arrive: OK\n";
	return 0;
}
catch (const std::exception& e)
{
	std::cerr << "ksim_residents_arrive: FAIL: " << e.what() << "\n";
	return 1;
}
