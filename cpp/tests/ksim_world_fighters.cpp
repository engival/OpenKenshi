// World squads as Fighters (docs/reference/TOWN_LIFE_SLICE.md 5.14; no saved state, the marker is derived):
//   - after a NEW_GAME, a hostile world squad (a squad found in the data by its faction's relation to the player faction, never by name)
//     that the player walks up to has its members built into Fighters on a hostile team, they notice and attack the player;
//   - when the player leaves, the squad is demoted only once its members are out of the fight; the bodies stay, nothing leaks, and
//     re-promotion builds the same people;
//   - a neutral world squad is promoted on team 0 and never attacks;
//   - with and without the squad AI;
//   - a save in the middle of the fight loads to the same hash and continues identically (the marker is rebuilt from the arena).

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
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
// The seed is 23 since kenshi-k16.6.33: the Hub's population draws moved, and 21 / 22 give a different wilderness squad near the start whose fight the scenes do not expect.
Simulation newGame(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, bool withAi, std::uint64_t seed)
{
	SpawnOptions options;
	options.seed = seed;
	options.wildernessByDensity = false;
	options.settleSeconds = 0; // the tests pin the t0 world
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	sim.setResidentsArrive(false); // residents mid-walk at the save are not bit-exact after a load (kenshi-k16.6.37); the arrival is ksim_residents_arrive's
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

// Every entity with an Id but the producers: those materialise and drop with the players' distance (producers.hpp kMaterialiseRadius),
// not with the squads.
std::size_t instanceCount(Simulation& sim)
{
	std::size_t n = 0;
	ecsOf(sim).each([&](const state::Id&) { ++n; });
	ecsOf(sim).each([&](const state::Id&, const state::Producer&) { --n; });
	return n;
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

struct Person
{
	std::string race;
	std::string name;
	std::string weapon;
	bool operator==(const Person&) const = default;
};

std::vector<Person> peopleOf(Simulation& sim, const std::vector<flecs::entity>& members)
{
	std::vector<Person> people;
	for (const flecs::entity member : members)
	{
		const state::Fighter* f = sim.combat()->find(idOf(member));
		test::require(f != nullptr, "a promoted member is in the arena");
		// The equipped weapon, drawn or on the back: a member that fought stands down after it (k16.6.288) and is rebuilt sheathed (its Fighter then holds FISTS).
		people.push_back({f->raceSid, f->name, sim.sheathedWeaponOf(idOf(member)).empty() ? f->weapon.sid : sim.sheathedWeaponOf(idOf(member))});
	}
	return people;
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

// The team rule, from outside: friendly (above kHostile) is the player's team 0, anything else, and a squad with no faction, is not.
bool teamMatches(const Simulation& sim, const World& world)
{
	const bool friendly = world.faction.is_valid() && world.relation > FactionBook::kHostile;
	for (const flecs::entity member : world.members)
	{
		const state::Fighter* f = sim.combat()->find(idOf(member));
		if (f != nullptr && !f->playerControlled && !member.has<state::Routine>() && (f->team == 0) != friendly)
			return false;
	}
	return true;
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

// Everything the scenario and the save test share: a new game at the Hub, the built world squads found at run time, the second one's
// faction made hostile by command, both sent away and back (so the hostile one is built again, now by its relation), and the attack.
// How far east of the start the hostile squad is moved (clear of the Hub's residents).
constexpr double kHostileOffset = 700.0;

struct Scene
{
	Start start;
	double farX = 0.0;
	double farZ = 0.0;
	World neutral;
	World hostile;
	std::vector<Person> hostilePeople;
	std::size_t entities = 0;
};

// The entities there should be when `base` were there before the demotions: a member left far from its squad is a token of its own (k16.6.53), one new
// entity for each split and one gone for each merge.
std::size_t expectedEntities(const Simulation& sim, std::size_t base)
{
	return base + sim.residentStats().stragglerSplits - sim.residentStats().stragglerMerges;
}

Scene engage(Simulation& sim, bool withAi, const std::string& mode)
{
	Scene scene;
	scene.start = startOf(sim);
	// The town populate homes every Hub squad in a building (residents: routine mode with the AI, so not world squads), so the scene sets down a
	// wilderness squad of its own: the nearest homeless spawn of two or more with a relation of 0 or better (a mildly negative bandit squad is
	// put on a team of its own by the AI; no merchant or trade circuit: it keeps a squad routine, a bar table at the Hub), 250 u east of the start (inside the build radius).
	if (withAi)
	{
		const flecs::entity player = playerFaction(sim);
		flecs::entity pick;
		double best = 1e18;
		ecsOf(sim).each([&](flecs::entity squad, const state::SquadInstance& instance, const state::Position& at)
		{
			const double d = std::hypot(at.x - scene.start.x, at.z - scene.start.z);
			const std::size_t members = membersOf(sim, squad).size();
			if (instance.source != state::SpawnSource::BiomeHomeless || instance.faction.id == 0 || !ecsOf(sim).is_alive(instance.faction.id) || d >= best
				|| members < 2 || members > 8 || trader::isMerchant(*sim.gameData()->find(instance.templateSid))
				|| sim.ai()->registry().squad(instance.templateSid).behaviour == Behaviour::TradeCircuit || factionRelation(*sim.gameData(), ecsOf(sim).entity(instance.faction.id), player) < 0)
				return;
			best = d;
			pick = squad;
		});
		test::require(pick.is_valid(), "a wilderness squad to set down at the Hub");
		// Through the regions: setting its Position alone left it in its old region's detail, never built (the Hub's Mercenary Guild bar squad,
		// a world squad until 1fe6ad1, hid that).
		sim.moveSquad(pick, scene.start.x + 250.0, scene.start.z);
	}
	sim.run(200);
	farPoint(sim, scene.start, scene.farX, scene.farZ);
	const std::vector<World> built = builtWorldSquads(sim);
	std::size_t squads = 0;
	std::size_t fullSquads = 0;
	ecsOf(sim).each([&](flecs::entity squad, const state::SquadInstance&)
	{
		++squads;
		fullSquads += squad.get<Detail>() != nullptr && squad.get<Detail>()->level == DetailLevel::Full ? 1 : 0;
	});
	const Simulation::ResidentStats stats = sim.residentStats();
	std::cout << mode << ": at the new game " << squads << " squads, " << fullSquads << " at Full detail, " << sim.combat()->members().size()
		<< " fighters in the arena, " << stats.promotedSquads << " resident squads and " << built.size() << " world squads built\n";
	test::require(built.size() >= (withAi ? 1u : 2u), "world squads are built around the Hub");
	std::size_t hostileAtStart = 0;
	for (const World& world : built)
	{
		test::require(teamMatches(sim, world), "a built world squad's team follows its faction's relation");
		hostileAtStart += world.relation <= FactionBook::kHostile ? 1 : 0;
	}
	std::cout << mode << ": " << hostileAtStart << " of them hostile to the player at the start\n";

	// The neutral one: the first whose faction is friendly. The other one: a different faction, made hostile by command.
	for (const World& world : built)
	{
		if (world.faction.is_valid() && world.relation > FactionBook::kHostile && !scene.neutral.squad.is_valid())
			scene.neutral = world;
	}
	// (with the AI most Hub squads are in routine mode, and there may be one world squad only: then there is no neutral one to check)
	if (built.size() < 2)
		scene.neutral = World{};
	test::require(scene.neutral.squad.is_valid() || withAi, "a neutral world squad");
	for (const World& world : built)
	{
		// the biggest squad of another faction (a lone unarmed one may have no reason to fight)
		if (world.faction.is_valid() && world.faction.id() != scene.neutral.faction.id()
			&& (!scene.hostile.squad.is_valid() || world.members.size() > scene.hostile.members.size()))
			scene.hostile = world;
	}
	test::require(scene.hostile.squad.is_valid(), "a world squad of another faction");
	// The hostile squad stands clear of the Hub's residents (TOWN_LIFE_SLICE 5.17). The scene used to leave it among them, and the town's people
	// (the bar squads now walk to their Bar and their shops) fight a hostile squad that stands among them before it reaches the player; traced: the
	// three members it lost were hit by team-0 residents only, none by the player. The test is about a hostile world squad attacking the player.
	{
		const double x = scene.start.x + kHostileOffset;
		const double z = scene.start.z;
		for (std::size_t k = 0; k < scene.hostile.members.size(); ++k)
			teleport(sim, idOf(scene.hostile.members[k]), x + 2.0 * static_cast<double>(k), z);
		scene.hostile.squad.set<state::Position>(state::Position{x, z});
	}
	scene.entities = instanceCount(sim);
	sim.enqueue(Command::setFactionRelation(*scene.hostile.faction.get<state::Id>(), *playerFaction(sim).get<state::Id>(), -60));
	sim.run(1);

	// Away: both squads leave Full and go back to tokens (the fight has not started, so nothing holds them).
	teleport(sim, scene.start.player, scene.farX, scene.farZ);
	runUntil(sim, [&] { return allOut(sim, scene.hostile.members) && allOut(sim, scene.neutral.members); }, 3000, "the world squads were never demoted");
	test::require(instanceCount(sim) == expectedEntities(sim, scene.entities), "demotion leaves no entity behind but the stragglers' tokens");
	for (const flecs::entity member : scene.hostile.members)
		test::require(!member.has<state::Fighter>() && member.has<state::CharacterInstance>(), "a demoted member is a token again");

	// Back: built again, the hostile one on a team of its own.
	// (right beside the hostile squad: the nearest enemy of its members is then the player, not a townsperson)
	const state::Position hostileAt = *scene.hostile.squad.get<state::Position>();
	teleport(sim, scene.start.player, hostileAt.x + 3.0, hostileAt.z);
	runUntil(sim, [&] { return inArena(sim, scene.hostile.members) == scene.hostile.members.size() && inArena(sim, scene.neutral.members) == scene.neutral.members.size(); },
		3000, "the world squads were never built again");
	std::set<std::int32_t> teams;
	for (const flecs::entity member : scene.hostile.members)
	{
		const state::Fighter* f = sim.combat()->find(idOf(member));
		test::require(f->team != 0 && !f->playerControlled && !member.has<state::Routine>(), "the hostile world squad is on a team that is not the player's");
		teams.insert(f->team);
	}
	test::require(teams.size() == 1, "one squad, one team");
	for (const flecs::entity member : scene.neutral.members)
		test::require(sim.combat()->find(idOf(member))->team == 0, "the neutral world squad is on the player's team");
	{
		// Right on top of the first member: its nearest enemy is then the player, not a townsperson.
		const state::Fighter& first = *sim.combat()->find(idOf(scene.hostile.members.front()));
		teleport(sim, scene.start.player, first.x + 1.0, first.z);
	}
	scene.hostilePeople = peopleOf(sim, scene.hostile.members);
	test::require(instanceCount(sim) == expectedEntities(sim, scene.entities), "promotion creates no entities");
	// BQ17 (s81_static.md): a relation-hostile squad whose package has none of the on-sight goals (16, 21, 144) never attacks on sight; the arbitrary
	// squad this test makes hostile (often the Mercenary Guild, goal 15 only) is therefore given an explicit attack order on the player, which is the
	// input the test is about (a hostile world squad that is built and fights).
	for (const flecs::entity member : scene.hostile.members)
	{
		if (state::Fighter* f = const_cast<state::Fighter*>(sim.combat()->find(idOf(member))))
			f->orderTarget = scene.start.player;
	}
	runUntil(sim, [&] { return anyTargets(sim, scene.hostile.members, scene.start.player); }, 2000, "the hostile squad never attacked the player");
	std::cout << mode << ": hostile squad of " << scene.hostile.members.size() << " on team " << *teams.begin() << " attacked the player; neutral squad of "
		<< scene.neutral.members.size() << " on team 0\n";
	(void)withAi;
	return scene;
}

void scenario(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, bool withAi)
{
	const std::string mode = withAi ? "with AI" : "without AI";
	Simulation sim = newGame(order, combat, withAi, 23);
	Scene scene = engage(sim, withAi, mode);

	// The neutral squad never targets the player.
	test::require(!anyTargets(sim, scene.neutral.members, scene.start.player), "a neutral squad never targets the player");

	// The fight is on and the squad stops being eligible (it is given a trade trip: the same effect as the area moving off it, but at an
	// exact moment): it stays built for as long as a member is in a fight within range, then goes once the fight is over.
	scene.hostile.squad.add<state::TraderTrip>();
	std::size_t retained = 0;
	for (int i = 0; i < 100; ++i)
	{
		sim.run(1);
		if (anyTargets(sim, scene.hostile.members, 0))
			++retained;
	}
	test::require(retained > 50 && !allOut(sim, scene.hostile.members), "the squad stays built while a member is in a fight (" + std::to_string(retained) + " ticks)");
	// The player runs (far beyond the range of any fight): the squad goes when the fight is over.
	teleport(sim, scene.start.player, scene.farX, scene.farZ);
	for (std::uint64_t i = 0; i < 3000 && !allOut(sim, scene.hostile.members); ++i)
		sim.run(1);
	if (!allOut(sim, scene.hostile.members))
	{
		std::cerr << "  trip " << scene.hostile.squad.has<state::TraderTrip>() << " detail " << static_cast<int>(scene.hostile.squad.get<Detail>()->level) << " sq-routine "
			<< scene.hostile.squad.has<state::SquadRoutine>() << " demoted squads " << sim.residentStats().worldDemotedSquads << " tick " << sim.clockTick() << "\n";
		for (const flecs::entity member : scene.hostile.members)
		{
			const state::Fighter* f = sim.combat()->find(idOf(member));
			const state::Fighter* t = f != nullptr && f->target != 0 ? sim.combat()->find(f->target) : nullptr;
			std::cerr << "  held: member " << idOf(member) << (f ? " in arena" : " gone") << (f && f->dead ? " dead" : "") << (f && f->unconscious ? " ko" : "") << " target " << (f ? f->target : 0)
				<< (t ? " (in arena, team " + std::to_string(t->team) + (t->dead ? " dead" : "") + (t->unconscious ? " ko" : "") + ")" : std::string(" (not in arena)")) << "\n";
		}
	}
	test::require(allOut(sim, scene.hostile.members), "the hostile squad was never demoted after the fight");
	scene.hostile.squad.remove<state::TraderTrip>();
	runUntil(sim, [&] { return allOut(sim, scene.neutral.members); }, 3000, "the neutral squad was never demoted");
	test::require(instanceCount(sim) == expectedEntities(sim, scene.entities), "no entity growth");
	std::cout << mode << ": demoted after the fight (" << retained << " ticks held with a member still in it)\n";

	// A second visit builds the same people, still hostile.
	teleport(sim, scene.start.player, scene.start.x, scene.start.z);
	runUntil(sim, [&] { return inArena(sim, scene.hostile.members) == scene.hostile.members.size(); }, 3000, "built again");
	test::require(peopleOf(sim, scene.hostile.members) == scene.hostilePeople, "a second promotion builds the same people");
	for (const flecs::entity member : scene.hostile.members)
		test::require(sim.combat()->find(idOf(member))->team != 0, "still hostile");

	const Simulation::ResidentStats end = sim.residentStats();
	test::require(end.worldPromotedSquads >= 2 && end.worldDemotedSquads >= 2 && end.worldHostileMembers >= scene.hostile.members.size(), "the counters moved");
	std::cout << mode << ": ok (world squads promoted " << end.worldPromotedSquads << ", demoted " << end.worldDemotedSquads << ", members built "
		<< end.worldPromotedMembers << ", removed " << end.worldDemotedMembers << ", hostile members " << end.worldHostileMembers << ")\n";
}

// A save in the middle of the fight loads to the same hash, and both go on identically through the demotion.
void saveLoad(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, bool withAi)
{
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_world_fighters_test.ksav";
	Simulation straight = newGame(order, combat, withAi, 23);
	const Scene scene = engage(straight, withAi, withAi ? "save, with AI" : "save, without AI");
	straight.run(7);
	const std::uint64_t split = straight.stateHash();
	straight.saveGame(path);
	straight.run(100);

	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == split, "load gives the saved hash");
	enable(loaded, combat, withAi);
	loaded.enableNavigation(gNav);
	test::require(loaded.stateHash() == split, "enabling AI and combat does not change the hash");
	loaded.run(100);
	test::require(loaded.stateHash() == straight.stateHash(), "load and continue mid-fight gives the straight run's hash");

	// The marker was rebuilt from the arena: the loaded run demotes the squad when the player leaves, exactly as the straight one does.
	std::vector<flecs::entity> loadedMembers;
	for (const flecs::entity member : scene.hostile.members)
	{
		flecs::entity twin;
		ecsOf(loaded).each([&](flecs::entity e, const state::Id& id) { twin = id.value == idOf(member) ? e : twin; });
		loadedMembers.push_back(twin);
	}
	test::require(inArena(loaded, loadedMembers) == loadedMembers.size(), "the loaded run has the squad built");
	teleport(straight, scene.start.player, scene.farX, scene.farZ);
	teleport(loaded, scene.start.player, scene.farX, scene.farZ);
	runUntil(straight, [&] { return allOut(straight, scene.hostile.members); }, 3000, "the straight run never demoted");
	runUntil(loaded, [&] { return allOut(loaded, loadedMembers); }, 3000, "the loaded run never demoted (the marker was not rebuilt)");
	while (loaded.clockTick() < straight.clockTick())
		loaded.run(1);
	while (straight.clockTick() < loaded.clockTick())
		straight.run(1);
	test::require(loaded.stateHash() == straight.stateHash(), "after the demotion both runs agree");
	std::remove(path.c_str());
	std::cout << (withAi ? "with AI" : "without AI") << ": save mid-fight ok (hash " << std::hex << split << std::dec << ")\n";
}

// An idle solo player does not run to a fight that is not its own (user report 2026-11-04, kinspect's exe oracle). The player faction's
// AI goals are PROTECT_ALLIES (goal 15) only, and an ally is the same faction, a personal friend, or relation >= 50 (the Hub is at 0, so a
// townsperson fighting is not one), plus SELF_PRESERVATION (goal 32) once it is itself attacked at melee. The arena's PROTECT_ALLIES used
// to take every team-0 character (worldTeam gives every non-hostile faction the player's team) for an ally, so a hostile squad fighting a
// townsperson 800 u away pulled the player in.
void idlePlayer(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order, combat, false, 23);
	const Start start = startOf(sim);
	test::require(playerIds(sim).size() == 1, "the Wanderer start is one player character");
	sim.run(200);
	const std::vector<World> built = builtWorldSquads(sim);
	test::require(built.size() >= 2, "world squads are built around the Hub");
	// A friendly world squad for the townsperson (all its members built: a trader's are not), then the biggest squad of another faction.
	const auto whole = [&](const World& world) { return !world.members.empty() && inArena(sim, world.members) == world.members.size(); };
	World victim;
	for (const World& world : built)
		if (whole(world) && world.faction.is_valid() && world.relation > FactionBook::kHostile && !victim.squad.is_valid())
			victim = world;
	test::require(victim.squad.is_valid(), "a friendly world squad stands as the townsperson");
	World hostile;
	for (const World& world : built)
		if (whole(world) && world.faction.is_valid() && world.faction.id() != victim.faction.id()
			&& (!hostile.squad.is_valid() || world.members.size() > hostile.members.size()))
			hostile = world;
	test::require(hostile.squad.is_valid(), "a world squad of another faction");
	// 800 u east of the player, in the open (engage puts its squad 700 u east) and in daylight (a new game starts at 10:00).
	double vx = start.x + 800.0;
	double vz = start.z;
	for (const double dz : {0.0, -40.0, 40.0, -80.0, 80.0, -120.0, 120.0})
	{
		if (sim.groundHeight(vx, start.z + dz).has_value())
		{
			vz = start.z + dz;
			break;
		}
	}
	test::require(sim.groundHeight(vx, vz).has_value(), "open ground 800 u east of the start");
	// Leave (both squads demote), move their tokens there, make the hostile faction hostile, come back: the hostile squad is built on a
	// team of its own (engage's path).
	double farX = 0.0;
	double farZ = 0.0;
	farPoint(sim, start, farX, farZ);
	teleport(sim, start.player, farX, farZ);
	runUntil(sim, [&] { return allOut(sim, hostile.members) && allOut(sim, victim.members); }, 3000, "the world squads were never demoted");
	sim.moveSquad(victim.squad, vx, vz);
	sim.moveSquad(hostile.squad, vx + 6.0, vz + 4.0);
	sim.enqueue(Command::setFactionRelation(*hostile.faction.get<state::Id>(), *playerFaction(sim).get<state::Id>(), -60));
	sim.run(1);
	teleport(sim, start.player, start.x, start.z);
	runUntil(sim, [&] { return inArena(sim, hostile.members) == hostile.members.size() && inArena(sim, victim.members) == victim.members.size(); },
		3000, "the world squads were never built again");
	for (std::size_t k = 0; k < victim.members.size(); ++k)
		teleport(sim, idOf(victim.members[k]), vx + 2.0 * static_cast<double>(k), vz);
	for (std::size_t k = 0; k < hostile.members.size(); ++k)
		teleport(sim, idOf(hostile.members[k]), vx + 6.0 + 2.0 * static_cast<double>(k), vz + 4.0);
	for (const flecs::entity member : hostile.members)
		test::require(sim.combat()->find(idOf(member))->team != 0, "the hostile squad is on a team that is not the player's");
	for (const flecs::entity member : victim.members)
		test::require(sim.combat()->find(idOf(member))->team == 0, "the townsperson's squad is on the player's team");
	// The player sees the fight: it faces it (the sight cone, PERCEPTION_LIVE).
	{
		state::Fighter& player = const_cast<state::Fighter&>(*sim.combat()->find(start.player));
		player.yaw = std::atan2(vx - player.x, vz - player.z);
	}
	// One bandit attacks a townsperson: the fight is 800 u away, not the player's.
	const std::uint64_t victimId = idOf(victim.members.front());
	const std::uint64_t bandit = idOf(hostile.members.front());
	sim.enqueue(Command::fighterAttack(state::Id{bandit}, state::Id{victimId}));
	sim.run(2);
	test::require(sim.combat()->find(bandit)->target == victimId, "the bandit attacks the townsperson");
	const state::Fighter& atStart = *sim.combat()->find(start.player);
	const double fromX = atStart.x;
	const double fromZ = atStart.z;
	const std::uint64_t window = static_cast<std::uint64_t>(2.0 * kTicksPerGameMinute);
	for (std::uint64_t i = 0; i < window; ++i)
		sim.run(1);
	const state::Fighter& after = *sim.combat()->find(start.player);
	const double moved = std::hypot(after.x - fromX, after.z - fromZ);
	std::cout << "idle player: an ordered bandit fights a townsperson " << std::hypot(vx - fromX, vz - fromZ) << " u away; the player moved " << moved
		<< " u in 2 game-min, target " << after.target << "\n";
	test::require(after.target == 0, "the idle player does not join a fight it is not part of");
	test::require(moved < 20.0, "the idle player stands still while someone else fights 800 u away");
	// Attacked at melee: SELF_PRESERVATION (goal 32), the player turns on its attacker.
	const std::uint64_t attacker = idOf(hostile.members.back());
	{
		state::Fighter& f = const_cast<state::Fighter&>(*sim.combat()->find(attacker));
		f.x = after.x + 2.0;
		f.z = after.z;
		f.y = sim.groundHeight(f.x, f.z).value_or(f.y);
		f.clearRoute();
		f.building = 0;
	}
	sim.enqueue(Command::fighterAttack(state::Id{attacker}, state::Id{start.player}));
	for (int i = 0; i < 100 && sim.combat()->find(start.player)->target != attacker; ++i)
		sim.run(1);
	test::require(sim.combat()->find(start.player)->target == attacker, "an attacked player fights back (SELF_PRESERVATION, goal 32)");
	std::cout << "idle player: attacked at melee, the player targets its attacker " << attacker << "\n";
}

// The player can attack anyone outside its party (user report 2026-11-04, seen live in the client). Fighter::team is a combat grouping (worldTeam:
// every faction not hostile to the player shares team 0), not a party: a neutral Hub NPC used to answer `same_team`, and the sim's own
// attack command dropped the order as well. An order on a neutral makes the two squads hostile, so the NPC answers.
void attackNeutral(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order, combat, false, 23);
	const Start start = startOf(sim);
	sim.run(200);
	// A built neutral world squad's member: team 0, not player-controlled, no Routine.
	flecs::entity npc;
	for (const World& world : builtWorldSquads(sim))
	{
		if (!world.faction.is_valid() || world.relation <= FactionBook::kHostile)
			continue;
		for (const flecs::entity member : world.members)
		{
			const state::Fighter* f = sim.combat()->find(idOf(member));
			if (f != nullptr && !f->playerControlled && !f->dead && f->team == 0 && !member.has<state::Routine>())
			{
				npc = member;
				break;
			}
		}
		if (npc.is_valid())
			break;
	}
	test::require(npc.is_valid(), "a neutral Hub NPC (team 0, not the player's party)");
	const std::uint64_t npcId = idOf(npc);
	{
		const state::Fighter& player = *sim.combat()->find(start.player);
		teleport(sim, npcId, player.x + 3.0, player.z);
	}
	sim.enqueue(Command::fighterAttack(state::Id{start.player}, state::Id{npcId}));
	bool kept = false;
	for (int i = 0; i < 200 && !kept; ++i)
	{
		sim.run(1);
		kept = sim.combat()->find(start.player)->target == npcId;
	}
	test::require(kept, "the attack order on a neutral NPC is kept (not same_team)");
	bool answered = false;
	for (int i = 0; i < 400 && !answered; ++i)
	{
		sim.run(1);
		answered = sim.combat()->find(npcId)->target == start.player;
	}
	test::require(answered, "the attacked neutral NPC fights back");
	std::cout << "attack neutral: the order is kept and the NPC (team " << sim.combat()->find(npcId)->team << ") targets the player\n";
}

} // namespace

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
		scenario(order, combat, false);
		scenario(order, combat, true);
		idlePlayer(order, combat);
		attackNeutral(order, combat);
		saveLoad(order, combat, false);
		saveLoad(order, combat, true);
		std::cout << "ksim_world_fighters: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_world_fighters: FAILED: " << e.what() << "\n";
		return 1;
	}
}
