// 138 FIND_A_DEAD_GUY (task ds/dead-guy; dt30 + its correction):
//   - the resolver, pure: a dead occupant of a built cage is picked over a nearer loose corpse; an unbuilt cage (or an inedible or
//     claimed occupant) is ignored; with no cage corpse the nearest sensed corpse is the pick;
//   - the registry: 138 is a goal on IS_CARRYING_TARGET that effects isCarriedDudeDead, planned [67, 68, 138];
//   - live, over the real Heft world: a world squad whose package carries 138 walks to a corpse and lifts it (beginInteract Carry).

#include "ksim/law/crime.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/ai/sensor.hpp"

#include "../src/ksim/world/world_resolvers.hpp"

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "knav/navmesh.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;

class StraightNavigator final : public Navigator
{
public:
	bool covers(double, double) const override { return true; }
	Route route(double fromX, double fromZ, double toX, double toZ) const override
	{
		Route route;
		route.found = true;
		route.points = {{fromX, fromZ}, {toX, toZ}};
		return route;
	}
};

std::uint64_t idOf(flecs::entity e)
{
	return e.get<state::Id>()->value;
}

void runUntil(Simulation& sim, const std::function<bool()>& done, std::uint64_t maxTicks, const std::string& what)
{
	for (std::uint64_t i = 0; i < maxTicks; ++i)
	{
		if (done())
			return;
		sim.run(1);
	}
	test::require(done(), what);
}

void centreOn(Simulation& sim, const std::vector<std::array<double, 2>>& at)
{
	std::vector<state::GroundPoint> areas;
	for (const auto& p : at)
		areas.push_back({p[0], p[1]});
	sim.enqueue(Command::setPlayerAreas(areas));
	sim.run(1);
}

state::Fighter& fighterOf(Simulation& sim, std::uint64_t id)
{
	return const_cast<state::Fighter&>(*sim.combat()->find(id));
}

// A squad template whose Squad list carries 106 and which spawns a few people (the kidnap-style world squads).
std::vector<std::string> templatesWith(Simulation& sim, const routines::PackageTable& packages, std::uint16_t task)
{
	std::vector<std::string> found;
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
	{
		const routines::CompiledPackage* package = packages.find(sim.ai()->registry().squad(record->stringID).package);
		if (package == nullptr)
			continue;
		for (const routines::GoalRow& row : package->list(routines::Role::Squad))
			if (row.task == task && routines::windowHolds(row, 2.0))
			{
				found.push_back(record->stringID);
				break;
			}
	}
	return found;
}

// Heft's grid cell (the baked world's region_11_4.knav) and its town centre (the cases run inside its 2000 u cage finder).
constexpr int kHeftCellX = 46;
constexpr int kHeftCellZ = 16;
constexpr double kHeftX = 68772.98;
constexpr double kHeftZ = -71496.61;
struct Game
{
	std::shared_ptr<const CombatData> combat;
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;
	std::vector<std::string> taskLog;

	explicit Game(const std::string& bake)
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({kHeftCellX + dx, kHeftCellZ + dz});
		mesh = test::openWorldNav(bake, zones);
		SpawnOptions options;
		options.seed = 1;
		options.wildernessPopulation = 0.0;
		options.nestMultiplier = 0.0;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->markPristine();
		combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(combat, settings);
		sim->enableNavigation(mesh);
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, bake, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim->gameData());
		sim->enableAi(std::move(ai));
		sim->setTaskLog([this](const std::string& line) { taskLog.push_back(line); });
		std::string sid;
		for (const kdata::Record* record : sim->gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			if (record->name == "Wanderer")
				sid = record->stringID;
		sim->enqueue(Command::newGame(sid, "Tester", 1));
		sim->run(3);
		for (const CombatArena::Member& member : sim->combat()->members())
			if (member.fighter.playerControlled)
				player = member.id;
	}

	// The player's camera is the squad's spot (the area streams to Full and the world squads build Fighters); the player CHARACTER
	// stands well away, so the placed squad has nobody to fight.
	void bringPlayer(double x, double y, double z)
	{
		if (state::Fighter* me = &fighterOf(*sim, player))
		{
			me->x = x + 400.0;
			me->y = y;
			me->z = z + 400.0;
			me->clearRoute();
			me->down = false;
			me->unconscious = false;
		}
	}

	flecs::entity place(const char* templateSid, double x, double z)
	{
		const flecs::entity squad = sim->placeWildernessSquad(templateSid, x, z);
		bringPlayer(x, sim->groundHeight(x, z).value_or(0.0), z);
		sim->enqueue(Command::setPlayerAreas({{x, z}}));
		sim->run(1);
		for (int i = 0; i < 20 * 20; ++i)
		{
			sim->run(1);
			const std::vector<flecs::entity> members = membersOf(*sim, squad);
			if (!members.empty() && std::all_of(members.begin(), members.end(),
				[&](flecs::entity member)
				{
					const state::Id* id = member.get<state::Id>();
					return id != nullptr && sim->combat()->find(id->value) != nullptr;
				}))
				break;
		}
		return squad;
	}

	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->run(1);
	}
};



// ---- the resolver, pure ----
struct Bodies
{
	sensor::Context ctx;
	state::Fighter self;
	std::map<std::uint64_t, state::Fighter> bodies;

	Bodies()
	{
		ctx.playerFaction = 200;
		ctx.relation = [](std::uint64_t, std::uint64_t) { return 0.0; };
		self.faction = 100;
		self.squad = 1u << 20 | 1;
	}
	std::uint64_t add(std::uint64_t id, double z, bool dead)
	{
		state::Fighter f;
		f.faction = 400;
		f.squad = 1u << 20 | id;
		f.z = z;
		f.dead = dead;
		bodies.emplace(id, f);
		state::SightRecord record;
		record.target = id;
		record.seen = true;
		record.sinceSeen = 0.0;
		record.sinceSensed = 0.0;
		self.sight.push_back(record);
		return id;
	}
	const state::Fighter* lookup(std::uint64_t id) const
	{
		const auto found = bodies.find(id);
		return found != bodies.end() ? &found->second : nullptr;
	}
};

void resolver()
{
	Bodies world;
	const std::uint64_t caged = world.add(2, 500.0, true); // a corpse in a cage, far away
	world.bodies.at(2).cageBuilding = 77;
	const std::uint64_t near = world.add(3, 20.0, true); // the nearest sensed corpse
	const std::uint64_t far = world.add(4, 300.0, true);
	world.add(5, 5.0, false); // alive: no target
	const auto lookup = [&](std::uint64_t id) { return world.lookup(id); };
	const auto free = [](std::uint64_t) { return false; };
	const auto edible = [](const state::Fighter& body) { return !worldres::inedibleRace(body); };

	// 1. a dead occupant of a built cage is picked over a nearer loose corpse.
	std::vector<worldres::DeadGuyCage> cages{{caged, true}};
	test::require(worldres::findCorpse138(world.self, world.ctx, cages, lookup, free, edible) == caged,
		"138: a built cage's dead occupant before the nearer loose corpse");
	// 2. an unbuilt cage is ignored (the nearest loose corpse wins), and so is an occupant that is alive, carried, claimed or inedible.
	cages[0].built = false;
	test::require(worldres::findCorpse138(world.self, world.ctx, cages, lookup, free, edible) == near, "138: an unbuilt cage is ignored");
	cages[0].built = true;
	world.bodies.at(2).carriedBy = 9;
	test::require(worldres::findCorpse138(world.self, world.ctx, cages, lookup, free, edible) == near, "138: a carried cage corpse is skipped");
	world.bodies.at(2).carriedBy = 0;
	test::require(worldres::findCorpse138(world.self, world.ctx, cages, lookup, [&](std::uint64_t id) { return id == caged; }, edible) == near,
		"138: a cage corpse a squad-mate goes for is skipped");
	state::FighterPart robot;
	robot.robotic = true;
	world.bodies.at(2).parts.push_back(robot);
	test::require(worldres::findCorpse138(world.self, world.ctx, cages, lookup, free, edible) == near, "138: an inedible (robot) occupant is skipped");
	world.bodies.at(2).parts.clear();
	world.bodies.at(2).dead = false;
	test::require(worldres::findCorpse138(world.self, world.ctx, cages, lookup, free, edible) == near, "138: a live cage occupant is no corpse");
	world.bodies.at(2).dead = true;
	// 3. no cage corpse: the nearest sensed corpse; a claimed one falls to the next; none left, none.
	test::require(worldres::findCorpse138(world.self, world.ctx, {}, lookup, free, edible) == near, "138: the nearest sensed corpse");
	test::require(worldres::findCorpse138(world.self, world.ctx, {}, lookup, [&](std::uint64_t id) { return id == near; }, edible) != near,
		"138: a claimed corpse falls to the next");
	(void)far;
	test::require(worldres::findCorpse138(world.self, world.ctx, {}, lookup, [](std::uint64_t) { return true; }, edible) == 0,
		"138: every corpse claimed, none picked");
	// 4. a caged body is never a lift target (goal 68's finders, kenshi-k16.6.144): a corpse outside the cage list, a KO'd occupant for 132.
	Bodies jail;
	const std::uint64_t cagedCorpse = jail.add(2, 5.0, true);
	jail.bodies.at(2).cageBuilding = 77;
	const std::uint64_t cagedKo = jail.add(3, 6.0, false);
	jail.bodies.at(3).unconscious = true;
	jail.bodies.at(3).cageBuilding = 77;
	const auto jailLookup = [&](std::uint64_t id) { return jail.lookup(id); };
	test::require(worldres::findCorpse138(jail.self, jail.ctx, {}, jailLookup, free, edible) != cagedCorpse,
		"138: a caged corpse outside the cage list is no sensed target");
	test::require(worldres::kidnap132(jail.self, jail.ctx, jailLookup, free) != cagedKo, "132: a caged KO'd body is not lifted");
	jail.bodies.at(3).cageBuilding = 0;
	test::require(worldres::kidnap132(jail.self, jail.ctx, jailLookup, free) == cagedKo, "132: the same body out of the cage is the target");
	std::cout << "resolver ok\n";
}

void registry()
{
	const goap::TaskRow* row = goap::rowOf(routines::task::FindCorpses);
	test::require(row != nullptr && row->hasGoalState && row->goalState.sym == goap::Sym::IsCarryingTarget && row->preCount == 0
			&& row->effCount == 1 && row->eff[0].sym == goap::Sym::CarriedDudeIsDead,
		"138: no preconditions, goal IS_CARRYING_TARGET, effect isCarriedDudeDead");
	std::cout << "registry ok\n";
}

// ---- live: a world squad carrying the row lifts a corpse ----
void lift(const char* bake)
{
	Game game(bake);
	Simulation& sim = *game.sim;
	sim.setClockTick(tickAt(2, 1, 0));
	const routines::PackageTable packages(*sim.gameData());
	const std::vector<std::string> templates = templatesWith(sim, packages, routines::task::FindCorpses);
	test::require(!templates.empty(), "a squad template whose Squad list carries 138");
	const double bx = kHeftX + 300.0;
	const double bz = kHeftZ + 300.0;
	flecs::entity squad;
	for (const std::string& sid : templates)
	{
		try
		{
			squad = sim.placeWildernessSquad(sid, bx, bz, 2); // two people: the first is the follow leader, which never scores
		} catch (const std::exception&) {
			squad = flecs::entity();
		}
		if (!squad.is_valid())
			continue;
		bool buildable = !membersOf(sim, squad).empty();
		for (const flecs::entity member : membersOf(sim, squad))
		{
			std::uint64_t dice = 1;
			try
			{
				game.combat->makeFighter(*sim.gameData(), member.get<state::CharacterInstance>()->templateSid, "", dice, sid);
			} catch (const std::exception&) {
				buildable = false;
			}
		}
		if (!buildable)
		{
			squad = flecs::entity();
			continue;
		}
		game.bringPlayer(bx + 900.0, sim.groundHeight(bx, bz).value_or(0.0), bz + 900.0);
		centreOn(sim, {{bx, bz}});
		bool built = false;
		for (int i = 0; i < 400 && !built; ++i)
		{
			sim.run(1);
			const std::vector<flecs::entity> now = membersOf(sim, squad);
			built = !now.empty() && std::all_of(now.begin(), now.end(), [&](flecs::entity m) { return sim.combat()->find(idOf(m)) != nullptr; });
		}
		if (built)
			break;
		squad = flecs::entity();
	}
	test::require(squad.is_valid(), "a 138 squad was built");
	const std::vector<flecs::entity> members = membersOf(sim, squad);
	const std::uint64_t carrierId = idOf(members.back()); // not the follow leader
	// The corpse: a dead copy of a member, 12 u from the carrier. No cage corpse exists, so this is the sensed-corpse fallback.
	std::uint64_t bodyId = 0;
	{
		state::Fighter body = fighterOf(sim, carrierId);
		body.playerControlled = false;
		body.dead = true;
		body.carrying = 0;
		body.carriedBy = 0;
		body.target = 0;
		body.orderTarget = 0;
		body.homeBuilding = 0;
		body.faction = 0;
		body.team = 0;
		body.x += 12.0;
		body.clearRoute();
		const flecs::entity entity = sim.spawnFighter(std::move(body));
		entity.set<state::Allegiance>(state::Allegiance{"test-corpse"});
		bodyId = idOf(entity);
	}
	bool planned = false;
	runUntil(sim, [&]
	{
		for (const flecs::entity member : members)
		{
			const state::Routine* r = member.get<state::Routine>();
			if (r != nullptr && r->goal == routines::task::FindCorpses && !planned)
			{
				planned = true;
				test::require(r->chainTarget == bodyId, "138's target is the corpse");
				test::require(std::find(r->plan.begin(), r->plan.end(), goap::kLiftPerson) != r->plan.end(), "138's plan lifts (68)");
			}
		}
		return fighterOf(sim, bodyId).carriedBy != 0;
	}, 3000, "the corpse was never lifted");
	test::require(planned, "138 held a routine before the lift");
	const std::uint64_t by = fighterOf(sim, bodyId).carriedBy;
	test::require(fighterOf(sim, by).carrying == bodyId, "the lifter carries the corpse");
	std::cout << "lift ok (member " << by << " lifted " << bodyId << ")\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_dead_guy KSIM_NAVWORLD");
		resolver();
		registry();
		lift(argv[1]);
		std::cout << "ksim_dead_guy: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_dead_guy: FAILED: " << e.what() << "\n";
		return 1;
	}
}
