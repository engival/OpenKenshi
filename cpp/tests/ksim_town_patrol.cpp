// The patrol family (task patrol, kinspect dt24_36; task CH town-residents-3, dt24_patrol_town_pot): 36 PATROL_TOWN, 22 PATROL and
// 216 RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE, run by a built world squad at the Hub (the G2 selector, world_tasks.cpp).
//  - 36: every leg is a uniform point within townRadius/4 of the package town's centre (the squad's home town, fixed for the
//    package's life); outside that town the atDestinationTown precondition refuses the goal.
//  - 22: the same goal-less walk around the member's own point, in the wilderness where 36's atDestinationTown refuses.
//  - 216: a 3-member squad in a town with interior buildings spreads over them: two members are in different buildings at once, and
//    every search point they look around at stands inside a building on its ground floor (OURS by the user's ruling, FEATURES).
// Needs the world bake (argv[1], like ksim_capture_live; exit 77 without it).

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/ai/blackboard.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trader.hpp"
#include "ksim/world/towncircle.hpp"

#include "knav/navmesh.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "../src/ksim/world/world_resolvers.hpp"

#include "test_util.hpp"
#include "nav_source.hpp"
#include "sim_access.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kWildX = -48880.0;
constexpr double kWildZ = -823.0;

struct Game
{
	std::string world;
	kworld::LoadOrder order;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;

	explicit Game(const std::string& bake)
		: world(bake)
		, order(kworld::resolveLoadOrder(KENSHI_DATA, ""))
	{
	}

	void newGame(const std::vector<kworld::ZoneCell>& zones)
	{
		std::shared_ptr<MeshNavigator> mesh = test::openWorldNav(world, zones);
		SpawnOptions options;
		options.seed = 1;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->markPristine();
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(combat, settings);
		sim->enableNavigation(mesh);
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim->gameData());
		sim->enableAi(std::move(ai));
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

	void teleportPlayer(double x, double z)
	{
		state::Fighter& f = const_cast<state::Fighter&>(*sim->combat()->find(player));
		f.x = x;
		f.z = z;
		f.y = sim->groundHeight(x, z).value_or(f.y);
		f.clearRoute();
		f.building = 0;
	}
};

// The Hub's 3 x 3 zones.
std::vector<kworld::ZoneCell> hubZones()
{
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			zones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
	return zones;
}

// The zone of the wilderness spawn, with its neighbours.
std::vector<kworld::ZoneCell> wildZones()
{
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			zones.push_back({21 + dx, 31 + dz});
	return zones;
}

// The faction that owns the town holding (x, z): the squad placed there is made a member of it, so the town's residents are not its
// enemies and the fixture watches the patrol, not a fight.
state::EntityRef townFaction(Simulation& sim, double x, double z)
{
	state::EntityRef out;
	ecsOf(sim).each([&](flecs::entity, const state::TownState& town)
	{
		if (out.id != 0)
			return;
		const towncircle::Circle circle = towncircle::of(*sim.gameData(), town);
		if (circle.radius > 0.0 && std::hypot(x - circle.x, z - circle.z) <= circle.radius)
			out = town.owner;
	});
	return out;
}

// The TownState entity whose circle holds (x, z), 0 when none: a squad's home town, which the 36 walk reads.
flecs::entity_t townAt(Simulation& sim, double x, double z)
{
	flecs::entity_t out = 0;
	ecsOf(sim).each([&](flecs::entity entity, const state::TownState& town)
	{
		if (out != 0)
			return;
		const towncircle::Circle circle = towncircle::of(*sim.gameData(), town);
		if (circle.radius > 0.0 && std::hypot(x - circle.x, z - circle.z) <= circle.radius)
			out = entity.id();
	});
	return out;
}

// A squad placed at (x, z) and run until its members are Fighters (the squad is at Full detail).
flecs::entity placeBuilt(Game& game, std::string_view templateSid, double x, double z, std::vector<flecs::entity>& out)
{
	Simulation& sim = *game.sim;
	flecs::entity squad = sim.placeWildernessSquad(templateSid, x, z);
	const state::EntityRef owner = townFaction(sim, x, z);
	if (owner.id != 0)
	{
		state::SquadInstance* instance = squad.get_mut<state::SquadInstance>();
		instance->faction = owner;
	}
	for (int i = 0; i < 20 * 30; ++i)
	{
		sim.run(1);
		out = membersOf(sim, squad);
		if (!out.empty() && std::all_of(out.begin(), out.end(), [&](flecs::entity member)
			{
				const state::Id* id = member.get<state::Id>();
				return id != nullptr && sim.combat()->find(id->value) != nullptr;
			}))
			break;
	}
	test::require(!out.empty() && std::all_of(out.begin(), out.end(), [&](flecs::entity member)
		{
			const state::Id* id = member.get<state::Id>();
			return id != nullptr && sim.combat()->find(id->value) != nullptr;
		}), "the squad was built at Full detail");
	return squad;
}

// ---- 36: the package's town ----

void patrolTown(const std::string& bake)
{
	std::cout << "== 36 PATROL_TOWN: a member walks random legs of its town\n";
	Game game(bake);
	game.newGame(hubZones());
	game.teleportPlayer(kHubX, kHubZ);
	game.sim->run(2);
	std::vector<flecs::entity> members;
	// Flotsam borderguard: package "Guard building, residents", whose only goal the world path runs is PATROL_TOWN 36.
	const flecs::entity squad = placeBuilt(game, "49767-rebirth.mod", kHubX + 60.0, kHubZ + 40.0, members);
	Simulation& sim = *game.sim;
	// The package's town (section 1) is the platoon's town: a wilderness placement has none, so give it the Hub, as a town populate would.
	const flecs::entity_t hub = townAt(sim, kHubX, kHubZ);
	test::require(hub != 0, "the Hub's town state");
	squad.get_mut<state::SquadInstance>()->homeTown = state::EntityRef{hub};

	// A member on 36 (the row's window is 0-24).
	flecs::entity walker;
	for (int i = 0; i < 20 * 30 && !walker.is_valid(); ++i)
	{
		sim.run(1);
		for (flecs::entity member : members)
			if (const state::Routine* r = member.get<state::Routine>(); r != nullptr && r->goal == routines::task::PatrolTown)
				walker = member;
	}
	test::require(walker.is_valid(), "a member of the squad took PATROL_TOWN 36");

	// The package's target is the squad's TOWN (dt24_patrol_town_pot section 1), fixed for the package's life: ours is its home town.
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	test::require(instance != nullptr && instance->homeTown.id != 0, "the squad's package town exists (its home town)");
	const flecs::entity townEntity = ecsOf(sim).entity(instance->homeTown.id);
	test::require(townEntity.is_valid() && townEntity.get<state::TownState>() != nullptr, "the home town is loaded");
	const towncircle::Circle town = towncircle::of(*sim.gameData(), *townEntity.get<state::TownState>());
	test::require(town.radius > 0.0, "the home town has a circle (the Hub)");

	// Each leg is a uniform point in a square of half-side max(500, townRadius*0.5)/2 around the town centre (section 2).
	const double half = std::max(worldres::kPatrolOrderFloor, town.radius * 0.5) * 0.5;
	test::require(half == town.radius * 0.25, "the Hub is above the move order's 500 floor: legs within townRadius/4");
	const std::uint64_t walkerId = walker.get<state::Id>()->value;
	double farthest = 0.0;
	int legs = 0;
	for (int i = 0; i < 20 * 60; ++i)
	{
		sim.run(1);
		const state::Routine* r = walker.get<state::Routine>();
		const state::Fighter* me = sim.combat()->find(walkerId);
		if (me == nullptr || r == nullptr || r->goal != routines::task::PatrolTown || me->route.empty())
			continue;
		++legs;
		farthest = std::max({farthest, std::fabs(me->route.back().x - town.x), std::fabs(me->route.back().z - town.z)});
	}
	std::cout << "  " << legs << " leg routes, farthest " << farthest << " u from the town centre (limit " << half << ")\n";
	test::require(legs > 0, "the patroller drew legs");
	test::require(farthest <= half + 20.0, "every leg stays within townRadius/4 of the town centre (a nav snap has 20 u)");

	// atDestinationTown (section 1): outside its town the goal is not OFFERED; back inside it is again. A held 36 keeps its hold (the
	// exe's goal too), so the member is re-selected by dropping the routine first.
	state::Fighter& f = const_cast<state::Fighter&>(*sim.combat()->find(walkerId));
	const auto drop = [&]()
	{
		walker.remove<state::Routine>();
		f.clearRoute();
	};
	f.x = town.x + town.radius + 300.0;
	f.z = town.z;
	f.y = sim.groundHeight(f.x, f.z).value_or(f.y);
	f.building = 0;
	drop();
	bool outsideOn36 = false;
	for (int i = 0; i < 20 * 10; ++i)
	{
		sim.run(1);
		const state::Routine* r = walker.get<state::Routine>();
		outsideOn36 = outsideOn36 || (r != nullptr && r->goal == routines::task::PatrolTown);
	}
	test::require(!outsideOn36, "outside its town a member never takes PATROL_TOWN 36 (atDestinationTown)");
	f.x = town.x;
	f.z = town.z;
	f.y = sim.groundHeight(f.x, f.z).value_or(f.y);
	f.building = 0;
	drop();
	bool back = false;
	for (int i = 0; i < 20 * 30 && !back; ++i)
	{
		sim.run(1);
		const state::Routine* r = walker.get<state::Routine>();
		back = r != nullptr && r->goal == routines::task::PatrolTown;
	}
	test::require(back, "back in its town it takes PATROL_TOWN 36 again");
}

// ---- 22: the member's own point ----

void patrol(const std::string& bake)
{
	std::cout << "== 22 PATROL: a wilderness member patrols around itself\n";
	// A template whose package has PATROL 22 for a squad member and no other goal the world path runs, placed OUTSIDE every town
	// circle: 36's atDestinationTown refuses there, so 22 is the row the selector takes. The templates are tried in turn because a
	// nest template draws one member (the follow leader, who the selector leaves out).
	const char* const candidates[] = {"18014-rebirth.mod", "18009-rebirth.mod", "4021-gamedata.base", "56094-rebirth.mod",
		"66297-Boneyard Wolf.mod", "96078-__Southern hive.mod"};
	for (const char* templateSid : candidates)
	{
		Game game(bake);
		game.newGame(wildZones());
		game.teleportPlayer(kWildX + 300.0, kWildZ + 200.0);
		game.sim->run(2);
		std::vector<flecs::entity> members;
		try
		{
			placeBuilt(game, templateSid, kWildX + 300.0, kWildZ + 200.0, members);
		}
		catch (const std::exception&)
		{
			std::cout << "  " << templateSid << ": not placeable here\n";
			continue;
		}
		if (members.size() < 2)
		{
			std::cout << "  " << templateSid << ": " << members.size() << " member(s), nothing scored\n";
			continue;
		}
		Simulation& sim = *game.sim;
		flecs::entity walker;
		double startX = 0.0;
		double startZ = 0.0;
		for (int i = 0; i < 20 * 40 && !walker.is_valid(); ++i)
		{
			sim.run(1);
			for (flecs::entity member : members)
				if (const state::Routine* r = member.get<state::Routine>(); r != nullptr && r->goal == routines::task::Patrol)
				{
					walker = member;
					const state::Fighter* me = sim.combat()->find(member.get<state::Id>()->value);
					startX = me->x;
					startZ = me->z;
				}
		}
		if (!walker.is_valid())
		{
			std::cout << "  " << templateSid << ": " << members.size() << " members, no PATROL 22 in 40 s\n";
			continue;
		}
		const std::uint64_t walkerId = walker.get<state::Id>()->value;
		// It walks points around itself: it moves, and every point stays within the patrol radius of where it stood then.
		double moved = 0.0;
		for (int i = 0; i < 20 * 40; ++i)
		{
			sim.run(1);
			const state::Fighter* me = sim.combat()->find(walkerId);
			const state::Routine* r = walker.get<state::Routine>();
			if (r == nullptr || r->goal != routines::task::Patrol)
				continue;
			moved = std::max(moved, std::hypot(me->x - startX, me->z - startZ));
		}
		std::cout << "  " << templateSid << ": member " << walkerId << " on 22, moved " << moved << " u\n";
		test::require(moved > 5.0, "the patroller walked around its own point");
		return;
	}
	test::require(false, "a wilderness squad took PATROL 22 outside every town");
}

// ---- 216: the town search ----

void searchTown(const std::string& bake)
{
	std::cout << "== 216 RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE: the squad spreads over the town's buildings\n";
	Game game(bake);
	game.newGame(hubZones());
	game.teleportPlayer(kHubX, kHubZ);
	game.sim->run(2);
	std::vector<flecs::entity> members;
	// Bandit raid small: package "passing town assault then occupy" (216 in the leader list, Squad empty -> every member).
	const flecs::entity squad = placeBuilt(game, "56604-Dialogue.mod", kHubX + 80.0, kHubZ - 60.0, members);
	Simulation& sim = *game.sim;
	// The raid's own package, as a contract on the squad's board (dt19 section 8: a contract overrides the table): the squad's own
	// template ranks "hostile occupy town" above it and that package has no 216, so the raid package takes over the plan. The board
	// is built at the squad's first world step, after the promotion: a second of running gives it one.
	sim.run(20);
	bool contractPushed = false;
	if (blackboard::Board* board = sim.blackboardOf(squad))
	{
		blackboard::Package raid;
		if (blackboard::readPackage(*sim.gameData(), "14544-gamedata.base", 99, raid))
		{
			blackboard::pushContract(*board, raid, 24.0, trader::gameHours(sim.clockTick()));
			contractPushed = board->current == "14544-gamedata.base";
		}
	}
	std::cout << "  raid contract " << (contractPushed ? "applied" : "NOT applied") << "\n";

	std::size_t onGoal = 0;
	std::size_t spread = 0; // the best number of members in different buildings at one tick
	std::size_t looking = 0; // look-arounds that started at a point inside a building
	std::size_t outside = 0; // a look-around that started outside every building (a failure)
	std::uint64_t firstGoal = 0;
	std::map<std::uint64_t, std::uint64_t> lastLook; // member -> its look timer last tick (a start is 0 -> set)
	for (int i = 0; i < 20 * 180; ++i)
	{
		sim.run(1);
		std::vector<std::uint64_t> buildings;
		std::size_t holding = 0;
		for (flecs::entity member : members)
		{
			const std::uint64_t memberId = member.get<state::Id>()->value;
			const state::Routine* r = member.get<state::Routine>();
			const state::Fighter* me = sim.combat()->find(memberId);
			if (r == nullptr || r->goal != routines::task::RunAroundTown || me == nullptr)
			{
				lastLook[memberId] = 0;
				continue;
			}
			++holding;
			if (firstGoal == 0)
				firstGoal = sim.clockTick();
			if (me->building != 0 && std::find(buildings.begin(), buildings.end(), me->building) == buildings.end())
				buildings.push_back(me->building);
			// A look-around must start (the timer goes from 0 to set) inside a building.
			if (r->searchLookUntil != 0 && lastLook[memberId] == 0)
			{
				const std::uint64_t here = sim.buildingAt(me->x, me->y, me->z);
				if (here != 0)
					++looking;
				else
				{
					++outside;
					std::cout << "  a look-around outside a building: member " << memberId << " at " << me->x << "," << me->y
						<< "," << me->z << "\n";
				}
			}
			lastLook[memberId] = r->searchLookUntil;
		}
		onGoal = std::max(onGoal, holding);
		spread = std::max(spread, buildings.size());
		if (spread >= 2 && looking >= 2)
			break;
	}
	std::cout << "  members on 216: " << onGoal << ", buildings at once: " << spread << ", look-arounds inside: " << looking
		<< ", outside: " << outside << ", first goal tick " << firstGoal << "\n";
	if (onGoal < 2)
	{
		const blackboard::Board* board = sim.blackboardOf(squad);
		std::cout << "  board current " << (board != nullptr ? board->current : std::string("none")) << "\n";
		for (flecs::entity member : members)
		{
			const state::Routine* r = member.get<state::Routine>();
			const state::Fighter* me = sim.combat()->find(member.get<state::Id>()->value);
			const state::Inventory* pack = member.get<state::Inventory>();
			std::size_t weapons = 0;
			if (pack != nullptr)
				for (const state::Inventory::Entry& entry : pack->items)
					weapons += entry.slot == state::Inventory::kWeapon ? 1 : 0;
			const state::MemberRole* role = member.get<state::MemberRole>();
			std::cout << "  member " << member.get<state::Id>()->value << " role " << (role != nullptr ? int(role->list) : -1)
				<< " goal " << (r != nullptr ? int(r->goal) : -1) << " target " << (me != nullptr ? me->target : 0)
				<< " weapons " << weapons << " building " << (me != nullptr ? me->building : 0)
				<< " at " << (me != nullptr ? me->x : 0.0) << "," << (me != nullptr ? me->z : 0.0) << "\n";
		}
	}
	test::require(onGoal >= 2, "two members of the squad took RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE");
	test::require(spread >= 2, "two members were in different town buildings at the same time");
	// kenshi-k16.6.33: one look-around at least; the Hub lost its second Bar blackmarket and Thieves Guild shack (rwh.35), and the dice pick buildings.
	test::require(looking >= 1, "a member looked around inside a building");
	test::require(outside == 0, "no search point stood outside a building");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		std::setvbuf(stdout, nullptr, _IONBF, 0);
		std::cout.setf(std::ios::unitbuf);
		const std::string bake = argc > 1 ? argv[1] : "";
		if (bake.empty() || !std::filesystem::exists(bake))
		{
			std::cerr << "ksim_town_patrol: needs the world bake (argv[1])\n";
			return 77;
		}
		patrolTown(bake);
		patrol(bake);
		searchTown(bake);
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "ksim_town_patrol: %s\n", e.what());
		return 1;
	}
	return 0;
}
