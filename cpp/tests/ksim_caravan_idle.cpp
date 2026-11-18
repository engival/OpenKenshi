// kenshi-k16.6.254: the Slaver caravan of scenarios/cage_prisoner.kscen is spawned inside Heft. A trade_circuit squad that starts in a town gets the
// same stay as one that walks in (TOWN_LIFE_SLICE 4.2), so within the arrival window its members are on a real routine (bar hangout: relax / shopping /
// patrol), not 141 FOLLOW_SQUADLEADER at score 0, and the boss decides too. Needs the world navmesh bake (argv[1], exit 77 without the Heft region).
#include "ksim/core/simulation.hpp"

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_SCENARIO
#error KSIM_SCENARIO must be defined
#endif

using namespace ksim;

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(KSIM_SCENARIO);
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "ksim_caravan_idle: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world);
		Simulation& sim = rig.sim;
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
		const std::uint64_t bossId = placed.idOf.at(3);
		const state::Fighter* boss = sim.combat()->find(bossId);
		test::require(boss != nullptr, "the Slaver boss is built");
		const double startX = boss->x;
		const double startZ = boss->z;
		const flecs::entity bossEntity = ecsOf(sim).query_builder<const state::Id>().build().find([&](const state::Id& i) { return i.value == bossId; });
		const flecs::entity squad = bossEntity.target<state::MemberOf>();

		// Two game minutes after the spawn the squad has its stay and a table of the town's bar (Heft's UC bar is 1609 u from the town point: the
		// finder's reach is the town circle), and the boss is on its way (the squad was stamped on its first step, before it could hold a table).
		sim.run(1500);
		const state::TraderTrip* trip = squad.get<state::TraderTrip>();
		test::require(trip != nullptr && trip->staying, "the caravan spawned inside Heft has its stay");
		test::require(trip->stayHours >= 5.0 && trip->stayHours <= 8.0, "the stay is U(5, 8) h");
		const state::SquadRoutine* routine = squad.get<state::SquadRoutine>();
		test::require(routine != nullptr && routine->barTable != 0, "the caravan holds a bar table");
		sim.run(1500);
		const state::Fighter* seated = sim.combat()->find(bossId);
		const double walked = std::hypot(seated->x - startX, seated->z - startZ);
		test::require(walked > 500.0, "the boss walked to the bar (" + std::to_string(walked) + " u from the Longhouse)");
		const state::GroupAI* ai = squad.get<state::GroupAI>();
		test::require(ai->goal == state::GoalKind::Idle && ai->until > sim.clockTick(), "at the table the squad idles until the departure, not before");
		std::cout << "boss walked " << walked << " u to the table, stay " << trip->stayHours << " h\n";
		std::cout << "ksim_caravan_idle ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
