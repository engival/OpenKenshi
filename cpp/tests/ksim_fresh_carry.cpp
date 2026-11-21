// kenshi-k16.6.402: a built squad stepped at the second's first tick whatever its slice (it has no goal: AiSystem::squadStep's `fresh`) runs with
// its carry entry, as in its own slice. Without it the token walks the route and the leash and holding are ignored. Setup as ksim_fresh_carry.
// Takes a carried (built) squad off its slice tick, clears its goal before every tick of a second, and checks each step that gives it a goal had
// its carry entry.
//
// argv[1]: the world navmesh bake directory (knav_cache).

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

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
		std::cout << "ksim_fresh_carry: SKIPPED: needs the world navmesh bake (pass its directory)\n";
		return 77;
	}
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
	for (int i = 0; i < 400 && sim.ai()->carried().empty(); ++i)
		sim.run(1);
	std::uint64_t pick = 0;
	for (const auto& [id, carry] : sim.ai()->carried())
	{
		if (id % 20 != 0)
			pick = id;
	}
	test::require(pick != 0, "a built squad is carried (some slice other than 0)");
	flecs::world& ecs = const_cast<flecs::world&>(sim.world().ecs());
	const auto goalOf = [&]
	{
		state::GoalKind kind = state::GoalKind::None;
		ecs.each([&](const state::Id& id, const state::GroupAI& group)
		{
			if (id.value == pick)
				kind = group.goal;
		});
		return kind;
	};
	int steps = 0;
	int uncarried = 0;
	for (int i = 0; i < 40; ++i)
	{
		ecs.each([&](const state::Id& id, state::GroupAI& group)
		{
			if (id.value == pick)
				group.goal = state::GoalKind::None;
		});
		sim.run(1);
		if (goalOf() == state::GoalKind::None)
			continue;
		++steps;
		uncarried += sim.ai()->carried().count(pick) == 0 ? 1 : 0;
	}
	std::cout << "squad " << pick << ": steps " << steps << ", uncarried " << uncarried << "\n";
	test::require(steps >= 4, "the squad was stepped in its slice and at the second's first tick, each second");
	test::require(uncarried == 0, "every step of a built squad has its carry entry, also the fresh step outside its slice");
	std::cout << "ksim_fresh_carry: OK\n";
	return 0;
}
catch (const std::exception& e)
{
	std::cerr << "ksim_fresh_carry: FAIL: " << e.what() << "\n";
	return 1;
}
