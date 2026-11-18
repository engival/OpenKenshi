// The Goal row (task AT, SIMLINK.md "Goal names", ROADMAP 6b; kinspect findings/inspect_goal.md): the character panel reports the goal
// the sim is running -- the goal that started a fight, a routine slot, a flee, a carry, the group goal -- and the squad template's
// behaviour only when there is no goal. A KO'd or dead character has none (the original clears it) and reads "Aimless". The names are
// the exe's display strings (ORIGINAL_AI.md 5a).

#include "ksim/core/command.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <cstdio>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

const kdata::Record* recordNamed(const kdata::GameData& gd, const char* type, const char* name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
		if (r->name == name)
			return r;
	throw std::runtime_error(std::string("no ") + type + " " + name);
}

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::unique_ptr<Simulation> sim;
	std::uint64_t rng = 4242;
	std::uint32_t seq = 0;

	Rig()
	{
		gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		data = CombatData::build(*gd, KENSHI_DATA);
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91));
		sim->setGameData(gd);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(data, settings);
	}

	flecs::entity add(const char* character, std::uint64_t team, double x, double z, bool playerControlled)
	{
		state::Fighter f = data->makeFighter(*gd, recordNamed(*gd, "CHARACTER", character)->stringID, "", ++rng);
		f.x = x;
		f.y = 0.0;
		f.z = z;
		f.team = team;
		f.playerControlled = playerControlled;
		f.attention = 0.5;
		return sim->spawnFighter(std::move(f));
	}

	std::string goal(std::uint64_t id) { return sim->charStats(id).goal; }
	state::Fighter* fighter(std::uint64_t id) { return const_cast<CombatArena*>(sim->combat())->find(id); }
};

} // namespace

int main()
{
	try
	{
		// ---- a fight: the player's attack order is n_ATTACK, the enemy's own target the melee task ----
		{
			Rig rig;
			const std::uint64_t player = rig.add("Escaped Servant", 0, 0.0, 0.0, true).get<state::Id>()->value;
			const std::uint64_t enemy = rig.add("Escaped Servant", 1, 0.0, 30.0, false).get<state::Id>()->value;
			rig.sim->enqueue(Command::fighterAttack(state::Id{ player }, state::Id{ enemy }));

			bool fought = false;
			for (int n = 0; n < 400 && !fought; ++n)
			{
				rig.sim->tick();
				const state::Fighter* p = rig.sim->combat()->find(player);
				const state::Fighter* e = rig.sim->combat()->find(enemy);
				fought = p != nullptr && p->target != 0 && e != nullptr && e->target != 0;
			}
			test::require(fought, "the two fight (both hold a target)");
			test::require(rig.goal(player) == "Attacking target", "the player's attack order: \"Attacking target\" (n_ATTACK), got \"" + rig.goal(player) + "\"");
			test::require(rig.goal(enemy) == "Self preservation", "the enemy it provoked: \"Self preservation\" (goal 32), got \"" + rig.goal(enemy) + "\"");
			std::printf("fight: player \"%s\", enemy \"%s\"\n", rig.goal(player).c_str(), rig.goal(enemy).c_str());
		}

		// ---- a player's move order: "Move order" (goal 29) for the whole run, under attack too (kenshi-k16.6.343, rwh.172) ----
		{
			Rig rig;
			const std::uint64_t player = rig.add("Escaped Servant", 0, 0.0, 0.0, true).get<state::Id>()->value;
			const std::uint64_t enemy = rig.add("Escaped Servant", 1, 0.0, 30.0, false).get<state::Id>()->value;
			state::Fighter* f = rig.fighter(player);
			test::require(f != nullptr && rig.goal(player).empty(), "a player with no order: Aimless");
			f->route = { { 0.0, 0.0, 0.0 }, { 100.0, 0.0, 0.0 } };
			f->routeNext = 1;
			f->routeSource = state::Fighter::PlanSource::Order;
			test::require(rig.goal(player) == "Move order", "a player's move order: \"Move order\", got \"" + rig.goal(player) + "\"");
			f->target = enemy; // hit and answering: the order still stands
			test::require(rig.goal(player) == "Move order", "a move order under attack: \"Move order\", got \"" + rig.goal(player) + "\"");
			std::printf("move order: \"%s\"\n", rig.goal(player).c_str());
		}

		// ---- a routine slot: the running AI_TASK, named from the data; the captured bar label ----
		{
			Rig rig;
			const flecs::entity e = rig.add("Escaped Servant", 1, 100.0, 100.0, false);
			const std::uint64_t id = e.get<state::Id>()->value;
			state::Routine routine;
			routine.goal = 153; // GO_HOME_AND_GO_TO_BED, a night slot
			e.set<state::Routine>(routine);
			test::require(rig.goal(id) == "Going to bed", "a resident going to bed: the exe's \"Going to bed\", got \"" + rig.goal(id) + "\"");
			std::printf("routine 153: \"%s\"\n", rig.goal(id).c_str());

			routine.goal = 214; // RELAX_IN_TOWN_PACKAGE: a bar patron
			e.set<state::Routine>(routine);
			test::require(rig.goal(id) == "Relaxing", "a bar patron: the captured \"Relaxing\", got \"" + rig.goal(id) + "\"");

			routine.goal = 20; // Shopkeeper
			e.set<state::Routine>(routine);
			test::require(rig.goal(id) == "Shopkeeper", "a shopkeeper: the captured \"Shopkeeper\", got \"" + rig.goal(id) + "\"");

			routine.goal = 38; // STAND_AT_GUARD_NODE_HOMEBUILDING
			e.set<state::Routine>(routine);
			test::require(rig.goal(id) == "Static guard", "a guard: the exe's \"Static guard\", got \"" + rig.goal(id) + "\"");
			std::printf("routine 20 / 38: \"Shopkeeper\" / \"%s\"\n", rig.goal(id).c_str());

			// A corpse: no goal at all (the client prints Aimless).
			state::Fighter* body = rig.fighter(id);
			test::require(body != nullptr, "the body is in the arena");
			body->dead = true;
			test::require(rig.goal(id).empty(), "a corpse reports no goal, got \"" + rig.goal(id) + "\"");
		}

		// ---- the individual and group goals, and carrying ----
		{
			Rig rig;
			const flecs::entity e = rig.add("Escaped Servant", 1, 200.0, 200.0, false);
			const std::uint64_t id = e.get<state::Id>()->value;
			state::Fighter* f = rig.fighter(id);
			test::require(f != nullptr, "the fighter is in the arena");

			f->goal.kind = state::IndividualKind::Flee; // DA_RUN_AWAY: RUN_AWAY_FORCED 280
			test::require(rig.goal(id) == "Running away", "a fleeing character: the exe's \"Running away\", got \"" + rig.goal(id) + "\"");
			f->goal.kind = state::IndividualKind::None;

			f->carrying = 1; // any id: the carrier's own goal
			test::require(rig.goal(id) == "Lifting", "a carrier: the exe's \"Lifting\", got \"" + rig.goal(id) + "\"");
			f->carrying = 0;

			// The group layer's goal, on the squad entity.
			flecs::entity squad = rig.sim->world().ecs().entity("goal-label-squad");
			state::GroupAI group;
			group.goal = state::GoalKind::TravelTo;
			squad.set<state::GroupAI>(group);
			e.add<state::MemberOf>(squad);
			test::require(rig.goal(id) == "Travelling", "a squad travelling: the exe's \"Travelling\" (task 53), got \"" + rig.goal(id) + "\"");
			std::printf("group TravelTo: \"%s\"\n", rig.goal(id).c_str());

			group.goal = state::GoalKind::Patrol;
			squad.set<state::GroupAI>(group);
			test::require(rig.goal(id) == "Patrolling town", "a patrolling squad: the exe's \"Patrolling town\" (task 36)");

			group.goal = state::GoalKind::Idle;
			squad.set<state::GroupAI>(group);
			test::require(rig.goal(id).empty(), "an idle squad with no template behaviour and no routine: Aimless");
		}

		std::printf("ksim_goal_label ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_goal_label FAILED: " << e.what() << "\n";
		return 1;
	}
}
