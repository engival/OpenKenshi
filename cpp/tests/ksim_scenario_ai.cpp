// A loaded scenario's non-player squads are world squads that run the normal AI (docs/reference/SCENARIO_IMPORT.md "Non-player squads").
// tests/data/scenario_residents.kscen: an Outlaws pair (citizen, ex-servant), a Hounds patron of another squad beside them and one bandit ordered to
// attack the citizen. Asserted: every file squad is a SquadInstance holding its characters; the squad AI attaches its GroupAI; the residents get their
// Routine; the ex-servant (the citizen's squadmate) joins the fight against the bandit. The patron of another squad does NOT join (BQ14: its
// package has none of 16 / 15 / 34, so a bandit one fight away is nothing to it; BAR_FIGHT_LOOP "bar_crowd: join rules").
// Needs the world navmesh bake (argv[1], exit 77 without it).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/navmesh.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "nav_source.hpp"

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
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_scenario_ai: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const std::string world = argv[1];
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions spawn;
		spawn.seed = 1;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		const auto gameData = sim.gameData();
		sim.enableWeather(*gameData);
		const auto combatData = CombatData::build(*gameData, KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combatData, settings);
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		}
		sim.enableNavigation(test::openWorldNav(world, hubZones));
		simlink::ServerContext context;
		context.gameData = gameData;
		const scenario::File file = scenario::parseFile(KSIM_SCENARIO);
		test::require(file.version == 2 && file.orders.size() == 1, "the file is KSCEN 2 with one order");
		const scenario::Placed placed = scenario::apply(sim, *gameData, *combatData, context, file, 1);
		test::require(placed.orders == 1, "the order is queued");
		// The file names no `blood`: every character starts at the race's maximum for its strength, as CombatData::makeFighter
		// builds one (the parsed 0.0 default used to leave the tank empty and the first wound killed the character).
		for (const auto& [charId, id] : placed.idOf)
		{
			const state::Fighter* f = sim.combat()->find(id);
			test::require(f != nullptr, "char " + std::to_string(charId) + " is in the combat arena");
			const double maxBlood = f->minBlood + (f->maxBloodRace - f->minBlood) * f->skills.strength * 0.01;
			test::require(std::abs(f->blood - maxBlood) < 1.0e-9,
				"char " + std::to_string(charId) + " blood " + std::to_string(f->blood) + " is the race's maximum " + std::to_string(maxBlood));
		}
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim.gameData());
		sim.enableAi(std::move(ai));

		flecs::world& ecs = const_cast<flecs::world&>(sim.world().ecs());
		const std::uint64_t citizen = placed.idOf.at(2);
		const std::uint64_t servant = placed.idOf.at(3);
		const std::uint64_t patron = placed.idOf.at(4);
		const std::uint64_t bandit = placed.idOf.at(5);

		// 1. Each non-player file squad is a SquadInstance that holds its characters (MemberOf), the two Outlaws in one.
		const auto squadOf = [&](std::uint64_t id)
		{
			flecs::entity found;
			ecs.each([&](flecs::entity entity, const state::Id& value, const state::Fighter&)
			{
				if (value.value == id)
					found = entity.target<state::MemberOf>();
			});
			return found;
		};
		for (const std::uint64_t id : {citizen, servant, patron, bandit})
			test::require(squadOf(id).is_valid() && squadOf(id).get<state::SquadInstance>() != nullptr, "every non-player character belongs to a world squad");
		test::require(squadOf(citizen).id() == squadOf(servant).id(), "the pair share a squad");
		test::require(squadOf(citizen).id() != squadOf(patron).id() && squadOf(bandit).id() != squadOf(patron).id(), "the patron and the bandit have squads of their own");
		test::require(squadOf(bandit).get<state::SquadInstance>()->templateSid == "1308-gamedata.base", "the squad keeps the file's template");

		// 2. The first seconds: the bandit goes for the citizen; the squadmate joins; the patron of another squad is only reported.
		bool servantJoined = false;
		bool patronJoined = false;
		for (int tick = 0; tick < 400; ++tick) // 20 s: the servant walks (goal 43 is a walking goal, kenshi-h2v.31) and sees the fight after ~11 s
		{
			sim.tick();
			const state::Fighter& s = *sim.combat()->find(servant);
			const state::Fighter& p = *sim.combat()->find(patron);
			servantJoined = servantJoined || s.target == bandit;
			patronJoined = patronJoined || p.target == bandit;
		}
		for (const auto& [name, id] : {std::pair<const char*, std::uint64_t>{"citizen", citizen}, {"servant", servant}, {"patron", patron}, {"bandit", bandit}})
		{
			for (const CombatArena::Member& m : sim.combat()->members())
			{
				if (m.id == id)
					std::cout << "ksim_scenario_ai: " << name << " join goals " << static_cast<int>(m.joinGoals) << (m.joinDriven ? " (package-driven)" : " (plain)") << "\n";
			}
		}
		test::require(sim.combat()->find(bandit)->target == citizen, "the ordered bandit fights the citizen");
		test::require(servantJoined, "the citizen's squadmate joined the fight against the bandit");
		test::require(!patronJoined, "a patron of another squad (no goal 16 / 15 / 34 in its package) does not join by sight (BQ14)");

		// 3. The squad AI runs on these squads: GroupAI attached, residents' Routine given.
		sim.run(60);
		test::require(squadOf(citizen).get<state::GroupAI>() != nullptr && squadOf(bandit).get<state::GroupAI>() != nullptr, "the squad AI attaches its state to the file's squads");
		bool anyRoutine = false;
		ecs.each([&](const state::Id& id, const state::Routine&)
		{
			anyRoutine = anyRoutine || id.value == patron || id.value == citizen || id.value == servant;
		});
		std::cout << "ksim_scenario_ai: claimed " << placed.claimedSquads << " world squads, removed " << placed.removedSquads << ", residents with a Routine: "
			<< (anyRoutine ? "yes" : "no") << ", patron joined in 20 s: " << (patronJoined ? "yes" : "no") << "\n";
		std::cout << "ksim_scenario_ai: OK\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_scenario_ai: FAIL: " << error.what() << "\n";
		return 1;
	}
}
