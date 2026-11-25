// World squads follow their Fighters (docs/reference/TOWN_LIFE_SLICE.md 5.14, follow): with the squad AI on, a built world squad's
// Fighters carry its movement.
//   - the group step decides the destination (here a Roam goal is set by hand); the leader walks to it on the navmesh, the others follow;
//   - the token is the leader's position, not advanced by the strategic mover;
//   - the members stay within a follow distance of the leader;
//   - demote and re-promote puts the members at the token;
//   - a fight replaces the squad's walk goal (the route progress waits, the goal is Fight; rwh.202 s12);
//   - no member stays at its build point for a game day while the goal moves the squad.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/ai/decision_trace.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"
#include "ksim/dev/build_paths.hpp"
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

namespace {

using namespace ksim;

std::shared_ptr<MeshNavigator> gNav; // the Hub navmesh (argv[1]): NEW_GAME places the player on it, members are snapped to it

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

void enable(Simulation& sim, const std::shared_ptr<const CombatData>& combat, bool withAi)
{
	if (withAi)
	{
		AiSystem::Config config;
		config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		config.navigator = std::make_shared<StraightNavigator>();
		sim.enableAi(std::move(config));
	}
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
}

// A NEW_GAME at the Hub (the "Wanderer" start-off: one character, the user's acceptance path).
Simulation newGame(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, bool withAi, std::uint64_t seed)
{
	SpawnOptions options;
	options.seed = seed;
	options.wildernessByDensity = false;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	enable(sim, combat, withAi);
	sim.enableNavigation(gNav);
	std::string sid;
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
	{
		if (record->name == "Wanderer")
			sid = record->stringID;
	}
	test::require(!sid.empty(), "the data has the Wanderer start-off");
	sim.enqueue(Command::newGame(sid, "Tester", 1));
	sim.run(3);
	return sim;
}

std::vector<std::uint64_t> playerIds(const Simulation& sim)
{
	std::vector<std::uint64_t> ids;
	for (const CombatArena::Member& member : sim.combat()->members())
	{
		if (member.fighter.playerControlled)
			ids.push_back(member.id);
	}
	return ids;
}

void teleport(Simulation& sim, std::uint64_t id, double x, double z)
{
	state::Fighter& f = const_cast<state::Fighter&>(*sim.combat()->find(id));
	f.x = x;
	f.z = z;
	f.y = sim.groundHeight(x, z).value_or(f.y);
	f.clearRoute();
	f.building = 0;
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

flecs::entity playerFaction(Simulation& sim)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::PlayerFaction&, const state::FactionState&) { found = e; });
	return found;
}

std::size_t inArena(const Simulation& sim, const std::vector<flecs::entity>& members)
{
	return static_cast<std::size_t>(std::count_if(members.begin(), members.end(), [&](flecs::entity m) { return sim.combat()->find(idOf(m)) != nullptr; }));
}

bool anyTargets(const Simulation& sim, const std::vector<flecs::entity>& members, std::uint64_t victim)
{
	for (const flecs::entity member : members)
	{
		const state::Fighter* f = sim.combat()->find(idOf(member));
		if (f != nullptr && !f->dead && (victim == 0 ? f->target != 0 : f->target == victim))
			return true;
	}
	return false;
}

struct Start
{
	std::uint64_t player = 0;
	double x = 0.0;
	double z = 0.0;
};

Start startOf(Simulation& sim)
{
	const std::vector<std::uint64_t> ids = playerIds(sim);
	test::require(!ids.empty(), "the new game has a player character");
	const state::Fighter* f = sim.combat()->find(ids.front());
	return {ids.front(), f->x, f->z};
}

// Walks the player onto the squad and waits until the squad has been built and (for a hostile one) attacks.

// A world squad that is built right now: some member is an alive, non-player Fighter with no Routine (a resident has one).
struct World
{
	flecs::entity squad;
	std::uint64_t id = 0;
	flecs::entity faction;
	double relation = 0.0; // the faction's opinion of the player faction
	std::vector<flecs::entity> members;
};

std::vector<World> builtWorldSquads(Simulation& sim)
{
	const flecs::entity player = playerFaction(sim);
	std::vector<World> found;
	ecsOf(sim).each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance)
	{
		if (instance.source == state::SpawnSource::Dismissed)
			return;
		World world;
		world.squad = squad;
		world.id = id.value;
		world.members = membersOf(sim, squad);
		bool built = false;
		for (const flecs::entity member : world.members)
		{
			const state::Fighter* f = sim.combat()->find(idOf(member));
			built = built || (f != nullptr && !f->dead && !f->playerControlled && !member.has<state::Routine>());
		}
		if (!built)
			return;
		if (instance.faction.id != 0 && ecsOf(sim).is_alive(instance.faction.id))
		{
			world.faction = ecsOf(sim).entity(instance.faction.id);
			world.relation = factionRelation(*sim.gameData(), world.faction, player);
		}
		found.push_back(std::move(world));
	});
	std::sort(found.begin(), found.end(), [](const World& a, const World& b) { return a.id < b.id; });
	return found;
}
// Where the player goes to leave the Hub: the nearest squad position at least 14000 u (three regions of 4608) from the start.
void farPoint(Simulation& sim, const Start& start, double& x, double& z)
{
	double best = 1.0e18;
	ecsOf(sim).each([&](const state::SquadInstance&, const state::Position& at)
	{
		const double d = std::hypot(at.x - start.x, at.z - start.z);
		if (d >= 14000.0 && d < best)
		{
			best = d;
			x = at.x;
			z = at.z;
		}
	});
	test::require(best < 1.0e17, "a far point to leave to");
}

// Nobody is left standing in the arena: bodies (dead or knocked out) stay, as they do for residents.
bool allOut(const Simulation& sim, const std::vector<flecs::entity>& members)
{
	return std::none_of(members.begin(), members.end(), [&](flecs::entity m)
	{
		const state::Fighter* f = sim.combat()->find(idOf(m));
		return f != nullptr && !f->dead && !f->unconscious && !f->down;
	});
}


// Sets the squad's goal by hand: a Roam to (tx, tz) along a straight two-point route from the token (what the group layer would set).
void sendSquad(Simulation& sim, flecs::entity squad, double tx, double tz)
{
	const state::Position at = *squad.get<state::Position>();
	state::GroupAI& ai = *squad.get_mut<state::GroupAI>();
	ai.goal = state::GoalKind::Roam;
	ai.fallback = state::AiReason::None;
	ai.route = {{at.x, at.z}, {tx, tz}};
	ai.length = std::hypot(tx - at.x, tz - at.z);
	ai.along = 0.0;
	ai.goalX = tx;
	ai.goalZ = tz;
	ai.since = sim.clockTick();
	ai.lastStep = sim.clockTick();
	ai.progressTick = sim.clockTick();
	ai.until = 0;
}

const state::Fighter* leaderOf(const Simulation& sim, const World& world)
{
	for (const flecs::entity member : world.members)
	{
		const state::Fighter* f = sim.combat()->find(idOf(member));
		if (f != nullptr && !f->dead && !f->unconscious && !f->playerControlled)
			return f;
	}
	return nullptr;
}

double reach(const Simulation& sim, const World& world, double x, double z)
{
	double worst = 0.0;
	for (const flecs::entity member : world.members)
	{
		const state::Fighter* f = sim.combat()->find(idOf(member));
		if (f != nullptr && !f->dead && !f->unconscious)
			worst = std::max(worst, std::hypot(f->x - x, f->z - z));
	}
	return worst;
}

// Where a squad can be sent from its leader: about 250 u away, the navmesh finds a route to it.
bool destinationFor(const Simulation& sim, const state::Fighter& leader, double& tx, double& tz)
{
	for (int k = 0; k < 8; ++k)
	{
		const double angle = 0.7853981633974483 * k;
		const Route route = sim.navigation()->routeFrom({leader.x, leader.y, leader.z}, leader.x + 250.0 * std::cos(angle), leader.z + 250.0 * std::sin(angle),
			std::nullopt, true);
		if (route.found && route.points.size() >= 2 && std::hypot(route.points.back().x - leader.x, route.points.back().z - leader.z) > 150.0)
		{
			tx = route.points.back().x;
			tz = route.points.back().z;
			return true;
		}
	}
	return false;
}

// The town populate homes every Hub squad in a building (residents in routine mode, not world squads), so each scene sets down a wilderness
// squad of its own (as ksim_world_fighters): the nearest Travel homeless spawn of 2 to 8 with a relation of 0 or better and no merchant, 700 u
// east, clear of the town's people (escapees among the Hub's Holy Nation residents flee instead of following a route), the player beside it.
void setDownTraveller(Simulation& sim, const Start& start)
{
	const flecs::entity player = playerFaction(sim);
	flecs::entity pick;
	double best = 1e18;
	ecsOf(sim).each([&](flecs::entity squad, const state::SquadInstance& instance, const state::Position& at)
	{
		const double d = std::hypot(at.x - start.x, at.z - start.z);
		const std::size_t members = membersOf(sim, squad).size();
		if (instance.source != state::SpawnSource::BiomeHomeless || instance.faction.id == 0 || !ecsOf(sim).is_alive(instance.faction.id) || d >= best
			|| members < 2 || members > 8 || trader::isMerchant(*sim.gameData()->find(instance.templateSid))
			|| sim.ai()->registry().squad(instance.templateSid).behaviour != Behaviour::Travel
			|| factionRelation(*sim.gameData(), ecsOf(sim).entity(instance.faction.id), player) < 0)
			return;
		best = d;
		pick = squad;
	});
	test::require(pick.is_valid(), "a wilderness squad to set down at the Hub");
	pick.set<state::Position>(state::Position{start.x + 700.0, start.z});
	state::SquadInstance& instance = *pick.get_mut<state::SquadInstance>(); // its den moves with it (the AI leashes a homeless squad to its den)
	if (instance.hasDen)
	{
		instance.denX = start.x + 700.0;
		instance.denZ = start.z;
	}
	teleport(sim, start.player, start.x + 650.0, start.z);
}

void scenario(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order, combat, true, 21);
	const Start start = startOf(sim);
	setDownTraveller(sim, start);
	sim.run(200);
	std::vector<World> built = builtWorldSquads(sim);
	// The squad to send: a friendly one with at least two members and a destination (never one that fights the player).
	World chosen;
	double tx = 0.0;
	double tz = 0.0;
	for (const World& world : built)
	{
		const state::Fighter* leader = leaderOf(sim, world);
		if (leader != nullptr && world.members.size() >= 2 && world.relation > FactionBook::kHostile && world.faction.is_valid()
			&& !world.squad.has<state::GateCrew>() && destinationFor(sim, *leader, tx, tz))
		{
			chosen = world;
			break;
		}
	}
	test::require(chosen.squad.is_valid(), "a built friendly world squad with a way to go");
	std::cout << "chosen squad " << chosen.id << " of " << chosen.members.size() << " members, destination " << tx << "," << tz << "\n";
	std::vector<std::pair<double, double>> buildPoint;
	for (const flecs::entity member : chosen.members)
	{
		const state::Fighter* f = sim.combat()->find(idOf(member));
		buildPoint.emplace_back(f != nullptr ? f->x : 0.0, f != nullptr ? f->z : 0.0);
	}
	const double startToTarget = std::hypot(leaderOf(sim, chosen)->x - tx, leaderOf(sim, chosen)->z - tz);
	sendSquad(sim, chosen.squad, tx, tz);

	// The leader walks to the destination, the token is the leader, the members keep up.
	double worstSpread = 0.0;
	double worstToken = 0.0;
	// Closest approach, not the end point: a squad with its own travel package arrives and then takes its next goal.
	double closest = 1e18;
	for (int i = 0; i < 1200; ++i)
	{
		sim.run(1);
		if (sim.clockTick() % 20 == 1)
		{
			const state::Fighter* leader = leaderOf(sim, chosen);
			test::require(leader != nullptr, "the leader stays alive");
			const state::Position token = *chosen.squad.get<state::Position>();
			worstToken = std::max(worstToken, std::hypot(token.x - leader->x, token.z - leader->z));
			worstSpread = std::max(worstSpread, reach(sim, chosen, leader->x, leader->z));
			closest = std::min(closest, std::hypot(leader->x - tx, leader->z - tz));
		}
	}
	const state::Fighter* leader = leaderOf(sim, chosen);
	std::cout << "in 1200 ticks the leader came within " << closest << " u of the destination (" << startToTarget << " at the start); token off the leader by at most "
		<< worstToken << ", members at most " << worstSpread << " from the leader\n";
	test::require(closest < startToTarget - 100.0 || closest < 60.0, "the leader walked toward the destination");
	test::require(worstToken <= 8.0, "the token is the leader (written every second)");
	test::require(worstSpread <= 160.0, "the members keep up with the leader");
	for (std::size_t k = 0; k < chosen.members.size(); ++k)
	{
		const state::Fighter* f = sim.combat()->find(idOf(chosen.members[k]));
		if (f != nullptr && !f->dead)
			test::require(std::hypot(f->x - buildPoint[k].first, f->z - buildPoint[k].second) > 20.0, "every member left its build point");
	}

	// Demote and re-promote: the token takes the leader's place, the members are built at it.
	const double leaderX = leader->x;
	const double leaderZ = leader->z;
	double farX = 0.0;
	double farZ = 0.0;
	farPoint(sim, start, farX, farZ);
	teleport(sim, start.player, farX, farZ);
	runUntil(sim, [&] { return allOut(sim, chosen.members); }, 3000, "the carried squad was demoted");
	const state::Position parked = *chosen.squad.get<state::Position>();
	std::cout << "demoted: the token is " << std::hypot(parked.x - leaderX, parked.z - leaderZ) << " u from where the leader was\n";
	test::require(std::hypot(parked.x - leaderX, parked.z - leaderZ) < 150.0, "the token took the leader's position on demotion");
	teleport(sim, start.player, parked.x + 3.0, parked.z);
	runUntil(sim, [&] { return inArena(sim, chosen.members) == chosen.members.size(); }, 3000, "the carried squad was built again");
	const state::Position again = *chosen.squad.get<state::Position>();
	const double ring = reach(sim, chosen, again.x, again.z);
	std::cout << "re-promoted: the members are at most " << ring << " u from the token\n";
	test::require(ring < 80.0, "re-promotion builds the members at the token");

	// A fight holds the squad: the route progress waits while a member is in a fight.
	sim.enqueue(Command::setFactionRelation(*chosen.faction.get<state::Id>(), *playerFaction(sim).get<state::Id>(), -60));
	sim.run(1);
	teleport(sim, start.player, farX, farZ);
	runUntil(sim, [&] { return allOut(sim, chosen.members); }, 3000, "demoted before the fight");
	const state::Position hostileAt = *chosen.squad.get<state::Position>();
	teleport(sim, start.player, hostileAt.x + 3.0, hostileAt.z);
	runUntil(sim, [&] { return inArena(sim, chosen.members) == chosen.members.size(); }, 3000, "built again, hostile");
	{
		const state::Fighter& first = *leaderOf(sim, chosen);
		teleport(sim, start.player, first.x + 1.0, first.z);
	}
	// The player picks the fight (fixtures order only the player, SCENARIO_IMPORT.md "Rule"): an attack order on the leader; the squad answers through its
	// attacked / defend-squadmates goals (BQ13 / BQ17).
	sim.enqueue(Command::fighterAttack(state::Id{start.player}, state::Id{idOf(chosen.members.front())}));
	runUntil(sim, [&] { return anyTargets(sim, chosen.members, start.player); }, 2000, "the hostile squad attacked");
	sendSquad(sim, chosen.squad, tx, tz);
	sim.run(20);
	const double along = chosen.squad.get<state::GroupAI>()->along;
	std::size_t fightTicks = 0;
	for (int i = 0; i < 100; ++i)
	{
		sim.run(1);
		fightTicks += anyTargets(sim, chosen.members, 0) ? 1 : 0;
	}
	const double alongNow = chosen.squad.get<state::GroupAI>()->along;
	std::cout << "fight: " << fightTicks << " of 100 ticks fighting, route progress " << along << " -> " << alongNow << "\n";
	test::require(fightTicks > 50, "the fight went on");
	test::require(alongNow - along < 1.0 && chosen.squad.get<state::GroupAI>()->goal == state::GoalKind::Fight, "a fight replaces the patrol goal (the route progress waits, the goal is the fight)");
	std::cout << "ksim_world_follow: scenario ok\n";
}

// A save in the middle of a carried walk loads to the same hash and goes on identically (the carried set is derived from the arena).
void saveLoad(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_world_follow_test_" + std::to_string(getpid()) + ".ksav";
	Simulation straight = newGame(order, combat, true, 21);
	setDownTraveller(straight, startOf(straight));
	straight.run(200);
	World chosen;
	double tx = 0.0;
	double tz = 0.0;
	for (const World& world : builtWorldSquads(straight))
	{
		const state::Fighter* leader = leaderOf(straight, world);
		if (leader != nullptr && world.members.size() >= 2 && world.relation > FactionBook::kHostile && world.faction.is_valid()
			&& !world.squad.has<state::GateCrew>() && destinationFor(straight, *leader, tx, tz))
		{
			chosen = world;
			break;
		}
	}
	test::require(chosen.squad.is_valid(), "a squad to walk for the save test");
	sendSquad(straight, chosen.squad, tx, tz);
	straight.run(305);
	const std::uint64_t split = straight.stateHash();
	straight.saveGame(path);
	straight.forgetPathRetries(); // the failed-route ladder is runtime only: a load starts it empty, so the straight run does at the save point
	straight.run(400);

	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == split, "load gives the saved hash");
	enable(loaded, combat, true);
	loaded.enableNavigation(gNav);
	test::require(loaded.stateHash() == split, "enabling AI and combat does not change the hash");
	loaded.run(400);
	test::require(loaded.stateHash() == straight.stateHash(), "load and continue mid-walk gives the straight run's hash");
	std::remove(path.c_str());
	std::cout << "save mid-walk ok (hash " << std::hex << split << std::dec << ")\n";
}

// The regression: a squad whose goal moves it is not left standing at its build point for a game day. Natural goals, no hand-set route.
void dayRegression(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order, combat, true, 21);
	setDownTraveller(sim, startOf(sim));
	sim.run(200);
	struct Track
	{
		std::vector<std::pair<double, double>> buildPoint;
		bool goalMoved = false;
		bool built = false;
		double farthest = 0.0;
	};
	std::map<std::uint64_t, Track> tracks;
	// The Hub has few moving squads, so one friendly squad is given a new Roam leg whenever its last one is done (what the group layer does
	// by itself on a longer day): the whole day it has a goal that moves it.
	World driven;
	for (const World& world : builtWorldSquads(sim))
	{
		const state::Fighter* leader = leaderOf(sim, world);
		double dx = 0.0;
		double dz = 0.0;
		if (!driven.squad.is_valid() && leader != nullptr && world.members.size() >= 2 && world.relation > FactionBook::kHostile && world.faction.is_valid()
			&& !world.squad.has<state::GateCrew>() && destinationFor(sim, *leader, dx, dz))
			driven = world;
	}
	test::require(driven.squad.is_valid(), "a squad to drive for the day");
	double lastX = leaderOf(sim, driven)->x;
	double lastZ = leaderOf(sim, driven)->z;
	double walked = 0.0; // the leader's path length since the last hour mark
	std::size_t hoursMoved = 0;
	std::size_t hoursSeen = 0;
	for (std::uint64_t t = 0; t < static_cast<std::uint64_t>(kTicksPerGameDay); t += 20)
	{
		sim.run(20);
		if (const state::Fighter* leader = leaderOf(sim, driven); leader != nullptr && driven.squad.get<state::GroupAI>() != nullptr)
		{
			const state::GroupAI& ai = *driven.squad.get<state::GroupAI>();
			double dx = 0.0;
			double dz = 0.0;
			if ((ai.goal != state::GoalKind::Roam || ai.along >= ai.length) && destinationFor(sim, *leader, dx, dz))
				sendSquad(sim, driven.squad, dx, dz);
			walked += std::hypot(leader->x - lastX, leader->z - lastZ);
			lastX = leader->x;
			lastZ = leader->z;
			if (t % kTicksPerGameHour < 20)
			{
				++hoursSeen;
				hoursMoved += walked > 20.0 ? 1 : 0;
				walked = 0.0;
			}
		}
		for (const World& world : builtWorldSquads(sim))
		{
			Track& track = tracks[world.id];
			const state::GroupAI* ai = world.squad.get<state::GroupAI>();
			if (!track.built)
			{
				track.built = true;
				for (const flecs::entity member : world.members)
				{
					const state::Fighter* f = sim.combat()->find(idOf(member));
					track.buildPoint.emplace_back(f != nullptr ? f->x : 0.0, f != nullptr ? f->z : 0.0);
				}
			}
			track.goalMoved = track.goalMoved || (ai != nullptr && ai->goal != state::GoalKind::Idle && !ai->route.empty());
			for (std::size_t k = 0; k < world.members.size() && k < track.buildPoint.size(); ++k)
			{
				const state::Fighter* f = sim.combat()->find(idOf(world.members[k]));
				if (f != nullptr && !f->dead)
					track.farthest = std::max(track.farthest, std::hypot(f->x - track.buildPoint[k].first, f->z - track.buildPoint[k].second));
			}
		}
	}
	std::size_t withGoal = 0;
	std::size_t stuck = 0;
	for (const auto& [id, track] : tracks)
	{
		if (!track.goalMoved)
			continue;
		++withGoal;
		if (track.farthest <= 20.0)
		{
			++stuck;
			std::cerr << "  squad " << id << " had a moving goal but no member ever left its build point\n";
		}
	}
	std::cout << "game day: " << tracks.size() << " world squads built, " << withGoal << " with a moving goal at some time, " << stuck << " never moved\n";
	std::cout << "driven squad " << driven.id << ": the leader moved in " << hoursMoved << " of " << hoursSeen << " game hours\n";
	test::require(hoursSeen >= 20 && hoursMoved + 2 >= hoursSeen, "the driven squad keeps moving all day (it is not left standing)");
	test::require(stuck == 0, "no built world squad with a moving goal stood at its build point for a game day");
}

// A carried leader never waits for ever at a carrot it cannot reach (core rule: no terminal state). The squad is sent along straight
// 2-point routes in eight directions, through walls and over anything (the group route need not be walkable); each time the leader
// must be somewhere else 600 ticks (30 s) later: it walks to a carrot further along the route, and when no carrot can be routed
// at all, ResidentStats::leaderWarps places it after 10 s.
void carrotRecovery(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order, combat, true, 21);
	setDownTraveller(sim, startOf(sim));
	sim.run(200);
	World driven;
	for (const World& world : builtWorldSquads(sim))
	{
		const state::Fighter* leader = leaderOf(sim, world);
		double dx = 0.0;
		double dz = 0.0;
		if (!driven.squad.is_valid() && leader != nullptr && world.members.size() >= 2 && world.relation > FactionBook::kHostile && world.faction.is_valid()
			&& !world.squad.has<state::GateCrew>() && destinationFor(sim, *leader, dx, dz))
			driven = world;
	}
	test::require(driven.squad.is_valid(), "a squad for the carrot recovery");
	int worstWait = 0;
	for (int k = 0; k < 8; ++k)
	{
		const state::Fighter* leader = leaderOf(sim, driven);
		const double x0 = leader->x;
		const double z0 = leader->z;
		sendSquad(sim, driven.squad, x0 + 220.0 * std::cos(0.7853981633974483 * k), z0 + 220.0 * std::sin(0.7853981633974483 * k));
		int firstMove = -1;
		double farthest = 0.0; // the farthest it got: where it stands at 600 ticks is chance (a detour can bring it back past its start)
		for (int t = 0; t < 600; ++t)
		{
			sim.run(1);
			leader = leaderOf(sim, driven);
			const double away = std::hypot(leader->x - x0, leader->z - z0);
			farthest = std::max(farthest, away);
			if (firstMove < 0 && away > 5.0)
				firstMove = t;
		}
		test::require(firstMove >= 0 && farthest > 20.0,
			"the carried leader did not leave its spot within 30 s of a group route in direction " + std::to_string(k));
		worstWait = std::max(worstWait, firstMove);
	}
	std::cout << "carrot recovery: worst wait before the leader moved " << worstWait << " ticks, " << sim.residentStats().leaderWarps << " placed at the route's end\n";
	test::require(worstWait <= 400, "a leader waited more than 20 s before moving along its group route");
}

} // namespace

// The decision trace (kenshi-k16.6.43) is read only: the same run with the sink on and off ends in the same world hash. The Hub's residents
// give the town scorer's picks, the set-down traveller the world squads' selector.
struct TraceRun
{
	std::uint64_t hash = 0;
	std::size_t picks[3] = {0, 0, 0}; // by trace::Source
	std::size_t chosen = 0; // picks whose candidates hold exactly one Chosen, the result's
	std::size_t bad = 0;
};

TraceRun traceRun(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, bool sinkOn)
{
	Simulation sim = newGame(order, combat, true, 21);
	TraceRun run;
	if (sinkOn)
	{
		trace::Sink sink;
		sink.record = [&run](const trace::Pick& pick)
		{
			++run.picks[static_cast<std::size_t>(pick.source)];
			std::size_t chosen = 0;
			for (const trace::Candidate& c : pick.candidates)
				chosen += c.verdict == goalsel::Why::Chosen ? 1 : 0;
			if (pick.result == trace::Result::Chosen)
				run.chosen += chosen == 1 ? 1 : 0;
			run.bad += (pick.result == trace::Result::Chosen) != (chosen == 1) && pick.result != trace::Result::Fallback ? 1 : 0;
		};
		sim.setDecisionTrace(sink);
	}
	const Start start = startOf(sim);
	setDownTraveller(sim, start);
	sim.run(1500);
	run.hash = sim.stateHash();
	return run;
}

void traceReadOnly(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	const TraceRun off = traceRun(order, combat, false);
	const TraceRun on = traceRun(order, combat, true);
	std::cout << "trace: " << on.picks[0] << " world-selector picks, " << on.picks[1] << " town-scorer picks, " << on.chosen << " with one Chosen candidate\n";
	test::require(on.picks[0] > 0, "the world selector reported picks");
	test::require(on.picks[1] > 0, "the town scorer reported picks");
	test::require(on.bad == 0, "a Chosen pick has exactly one Chosen candidate (a default / fallback pick none)");
	test::require(on.hash == off.hash, "the world hash is the same with the trace sink on and off");
}

int main(int argc, char** argv)
{
	try
	{
		test::require(argc > 1, "argv[1]: the Hub navmesh cache");
		gNav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		std::shared_ptr<const CombatData> combat;
		{
			SpawnOptions options;
			options.seed = 21;
			options.wildernessByDensity = false;
			Simulation probe = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
			combat = CombatData::build(*probe.gameData(), KENSHI_DATA);
		}
		if (test::wantPart(argc, argv, 2, "traceReadOnly")) { traceReadOnly(order, combat); }
		if (test::wantPart(argc, argv, 2, "scenario")) { scenario(order, combat); }
		if (test::wantPart(argc, argv, 2, "saveLoad")) { saveLoad(order, combat); }
		if (test::wantPart(argc, argv, 2, "carrotRecovery")) { carrotRecovery(order, combat); }
		if (test::wantPart(argc, argv, 2, "dayRegression")) { dayRegression(order, combat); }
		std::cout << "ksim_world_follow: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_world_follow: FAILED: " << e.what() << "\n";
		return 1;
	}
}
