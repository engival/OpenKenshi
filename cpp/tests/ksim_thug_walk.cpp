// kenshi-h2v.31: the Hub's bar thugs stay in the bar, and non-urgent moves walk. Setup as tools/ksim_hub_census (New Game at the Hub); every second of
// 5 game minutes asserts: no member of the Bar thugs squad is on PATROL_TOWN 36 / PATROL 22 / RUN_AROUND 216 (their package's day rows are the bar's; the
// patrol row only runs when no relax row is open, TOWN_LIFE_SLICE 5.38), and no resident on a non-urgent routine goal (36, 22, 216, 214, 118, 43) moves
// faster than its race's walk speed.
//
// argv[1]: the world navmesh bake directory (knav_cache).

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/routines.hpp"
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
		std::cout << "ksim_thug_walk: SKIPPED: needs the world navmesh bake (pass its directory)\n";
		return 77;
	}
	constexpr double kCx = -50986.0;
	constexpr double kCz = 3514.0;
	constexpr double kRadius = 1200.0;
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
	SpawnOptions options;
	options.seed = 1;
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
	const auto near = [&](const state::Fighter& f)
	{
		return !f.playerControlled && std::hypot(f.x - kCx, f.z - kCz) <= kRadius;
	};
	const auto count = [&]
	{
		std::size_t n = 0;
		for (const CombatArena::Member& m : sim.combat()->members())
			n += near(m.fighter) ? 1 : 0;
		return n;
	};
	for (int i = 0; i < 400 && count() == 0; ++i)
		sim.run(1);
	const auto slow = [](std::uint16_t goal)
	{
		return goal == routines::task::PatrolTown || goal == routines::task::Patrol || goal == routines::task::RunAroundTown
			|| goal == routines::task::RelaxInTownPackage || goal == routines::task::Shopping || goal == routines::task::StayInHome;
	};
	const flecs::world& ecs = sim.world().ecs();
	std::size_t thugs = 0;
	std::size_t patrolling = 0;
	std::size_t moving = 0;
	std::size_t running = 0;
	double fastest = 0.0;
	for (int second = 0; second < 300; ++second)
	{
		sim.run(static_cast<std::uint64_t>(1.0 / kTickSeconds));
		ecs.each([&](flecs::entity entity, const state::Id& id, const state::Routine& routine)
		{
			const state::Fighter* f = sim.combat()->find(id.value);
			if (f == nullptr || !near(*f))
				return;
			const flecs::entity squad = entity.target<state::MemberOf>();
			const state::SquadInstance* instance = squad.is_valid() ? squad.get<state::SquadInstance>() : nullptr;
			const kdata::Record* record = instance != nullptr ? sim.gameData()->find(instance->templateSid) : nullptr;
			if (record != nullptr && record->name == "Bar thugs")
			{
				++thugs;
				if (routine.goal == routines::task::PatrolTown || routine.goal == routines::task::Patrol || routine.goal == routines::task::RunAroundTown)
				{
					++patrolling;
					std::cout << "  " << second << " s: " << f->name << " " << id.value << " on goal " << routine.goal << " at " << f->x << " " << f->z << "\n";
				}
			}
			const RaceCombat* race = sim.combat()->data().race(f->raceSid);
			if (!slow(routine.goal) || !f->walking() || race == nullptr)
				return;
			++moving;
			const double speed = std::fabs(f->forwardSpeed);
			fastest = std::max(fastest, speed / race->walkSpeed);
			if (speed > race->walkSpeed + 0.5)
				++running;
		});
	}
	std::cout << "Bar thug member-seconds " << thugs << ", on a patrol goal " << patrolling << "; non-urgent movers " << moving << ", above walk speed " << running
		<< ", fastest " << fastest << " x walk\n";
	test::require(thugs > 0, "the Bar thugs squad is in the Hub");
	test::require(patrolling == 0, "a Bar thugs member left the bar for a patrol goal");
	test::require(moving > 0, "no resident moved on a non-urgent goal in 5 minutes");
	test::require(running == 0, "a resident on a non-urgent goal moved above the walk speed");
	std::cout << "ksim_thug_walk: OK\n";
	return 0;
}
catch (const std::exception& e)
{
	std::cerr << "ksim_thug_walk: FAIL: " << e.what() << "\n";
	return 1;
}
