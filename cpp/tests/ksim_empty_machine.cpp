// The pull tasks (dt24_emptymachine_dismantle; kinspect 2026-11-07): 92 COLLECT_OUTPUT_RESOURCE, 247
// COLLECT_OUTPUT_RESOURCE_BUILD_MATS and 284 LOOT_CONTAINER.
//
//  1. the vf5 quantity (pullQuantity): the stock of the item in the OTHER nearby machines of my faction adds on, a machine
//     that cannot stock it but can take it returns the flat 5, an empty-handed character still takes one food item, BuildMats
//     is always two and the task's +0x78 mode ten;
//  2. the picker (machine92 / container284): the nearest building of mine within reach, its first collectable type;
//  3. 92 empties a machine's output into the carrier one unit at a time and stops at n (two nearby machines, n = the other's
//     stock);
//  4. the flat 5: a nearby machine of the same output type with room and no stock;
//  5. 247 takes two per pass whatever the neighbourhood;
//  6. 284 takes from a storage box: the stock elsewhere adds on (the two-box case), and a food item alone is one per pass.
//
// Where the vanilla game issues these: they have no AI_TASK row in the install (ai_enum_map.tsv: "0 AI_TASK records (code-only:
// player order/dialogue/sub-goal)"). The exe reaches them from the machine goal nodes 113 EMPTY_MACHINE_OUTPUTS / 94
// WANT_TO_FILL_MACHINE / 115 FIND_SOME_BUILDING_MATERIALS / 131 LOOT_FOOD_AND_STUFF, which the player's job queue (and a hauling
// order's sub-goals) hand out; the job-to-goal table itself is not decoded (UNSURE). The executor is therefore driven here
// through a direct state::Routine on a built world squad member, as the 112 ladder is in ksim_town_guards.
// argv[1]: the Hub navmesh bake (KSIM_NAVWORLD).
#include "../src/ksim/world/world_resolvers.hpp"

#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/economy/inventory_grid.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/economy/producers.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "knav/walk.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "nav_source.hpp"
#include "sim_access.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

// The wilderness slot the other Hub tests use (ksim_after_fight; the slaver caravan's).
constexpr double kSpawnX = -48880.0;
constexpr double kSpawnZ = -823.0;

flecs::entity entityOf(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i)
	{
		if (i.value == id)
			found = e;
	});
	return found;
}

std::uint32_t carriedOf(Simulation& sim, std::uint64_t id, const std::string& item)
{
	const flecs::entity entity = entityOf(sim, id);
	const state::Inventory* pack = entity.get<state::Inventory>();
	return pack != nullptr ? pack->carried(item) : 0;
}

// One machine the member can see: a state::Producer entity with its output stock, built the way spawnProducer builds one
// (producers.cpp) without needing a site.
std::uint64_t makeMachine(Simulation& sim, std::uint64_t id, const std::string& recordSid, const std::string& output,
	const std::string& ownerSid, std::uint32_t count, std::uint32_t cap, double x, double y, double z)
{
	state::Producer producer;
	producer.recordSid = recordSid;
	producer.ownerSid = ownerSid;
	producer.output = output;
	producer.cap = cap;
	producer.maxOperators = 1;
	producer.useRange = 20.0;
	producer.x = x;
	producer.y = y;
	producer.z = z;
	producer.useX = x;
	producer.useY = y;
	producer.useZ = z;
	state::Inventory stock;
	stock.stackLimit = cap;
	stock.version = 1;
	const grid::Size slot = grid::Rules(*sim.gameData()).footprint(output);
	stock.gridW = static_cast<std::uint8_t>(slot.w);
	stock.gridH = static_cast<std::uint8_t>(slot.h);
	if (count > 0)
		stock.add(output, count);
	// An empty machine the case needs to keep (the flat-5 neighbour) is mid-batch, not pristine: stepProducers drops a pristine
	// producer that is not near a player character (producerSaved / kDropRadius). A zero-progress machine with no stock would
	// be destroyed before the carrier takes anything from its neighbour.
	if (count == 0)
		producer.progress = 0.5;
	flecs::entity entity = ecsOf(sim).entity();
	entity.set<state::Id>(state::Id{id});
	entity.set<state::Producer>(std::move(producer));
	entity.set<state::Inventory>(std::move(stock));
	return id;
}

// One storage box: a state::Container entity (interact.hpp; SIMLINK.md section 13) with its contents.
std::uint64_t makeBox(Simulation& sim, std::uint64_t id, const std::string& recordSid, const std::string& ownerSid,
	const std::vector<std::pair<std::string, std::uint32_t>>& contents, double x, double y, double z)
{
	state::Container container;
	container.recordSid = recordSid;
	container.ownerSid = ownerSid;
	container.x = x;
	container.y = y;
	container.z = z;
	container.useX = x;
	container.useY = y;
	container.useZ = z;
	state::Inventory stock;
	stock.version = 1;
	stock.gridW = 8;
	stock.gridH = 10;
	for (const auto& [item, count] : contents)
		stock.add(item, count);
	flecs::entity entity = ecsOf(sim).entity();
	entity.set<state::Id>(state::Id{id});
	entity.set<state::Container>(std::move(container));
	entity.set<state::Inventory>(std::move(stock));
	return id;
}

std::vector<std::uint64_t> membersOf(Simulation& sim, flecs::entity squad)
{
	std::vector<std::uint64_t> found;
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each(
		[&](flecs::entity, const state::Id& id) { found.push_back(id.value); });
	std::sort(found.begin(), found.end());
	return found;
}

struct Game
{
	std::shared_ptr<const CombatData> combat;
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;

	explicit Game(const std::string& bake)
	{
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		mesh = test::openWorldNav(bake, hubZones);
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = 1;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->markPristine();
		combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(combat, settings);
		sim->enableNavigation(mesh);
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

	std::uint64_t player = 0;

	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->run(1);
	}
};

// A built world squad (the bare build: no squad AI, as the slaver test's fixture A) and a carrier member.
struct Squad
{
	flecs::entity entity;
	std::vector<std::uint64_t> members;
	std::string factionSid;
	std::uint64_t carrier = 0;
};

Squad placeSquad(Game& game)
{
	Simulation& sim = *game.sim;
	Squad squad;
	squad.entity = sim.placeWildernessSquad("3387-gamedata.base", kSpawnX, kSpawnZ);
	test::require(squad.entity.is_valid(), "the wilderness squad was placed");
	for (int i = 0; i < 400; ++i)
	{
		sim.run(1);
		squad.members = membersOf(sim, squad.entity);
		const bool built = !squad.members.empty()
			&& std::all_of(squad.members.begin(), squad.members.end(), [&](std::uint64_t id) { return sim.combat()->find(id) != nullptr; });
		if (built)
			break;
	}
	test::require(!squad.members.empty(), "the world squad has members");
	const state::SquadInstance* instance = squad.entity.get<state::SquadInstance>();
	test::require(instance != nullptr && instance->faction.id != 0, "the placed squad has a faction");
	if (const state::FactionState* faction = ecsOf(sim).entity(instance->faction.id).get<state::FactionState>())
		squad.factionSid = faction->templateSid;
	test::require(!squad.factionSid.empty(), "the squad's faction has a record");
	squad.carrier = squad.members.front();
	return squad;
}


// Each case starts with an empty pack: Raw Iron does not stack, so a previous case's take would fill the carrier's grid and
// shrink what the next one can fit.
void resetPack(Simulation& sim, std::uint64_t id)
{
	flecs::entity actor = entityOf(sim, id);
	state::Inventory* pack = actor.is_valid() ? actor.get_mut<state::Inventory>() : nullptr;
	if (pack == nullptr)
		return;
	state::Inventory empty;
	empty.gridW = pack->gridW;
	empty.gridH = pack->gridH;
	empty.stackLimit = pack->stackLimit;
	empty.version = pack->version + 1;
	*pack = std::move(empty);
}

// Stands a member at (`x`, `y`, `z`) with no route and no speed, its follow ended.
void standAt(Simulation& sim, std::uint64_t id, double x, double y, double z)
{
	state::Fighter& f = *fighterOf(sim, id);
	CombatArena::endFollow(f);
	f.x = x;
	f.y = y;
	f.z = z;
	f.clearRoute();
	f.forwardSpeed = 0.0;
	f.lateralSpeed = 0.0;
}

// Stands the carrier at (`x`, `y`, `z`) and makes it run a pull task at `building` for `ticks` ticks. Every pull case stands the
// carrier at the building's use point: the walk is the same executor path for all of them and is not what these cases test.
void runPull(Game& game, const Squad& squad, std::uint16_t goal, std::uint64_t building, double x, double y, double z, int ticks)
{
	Simulation& sim = *game.sim;
	// The executor's window test (a selector goal whose package rows lost it) runs on the 20-tick boundary: a direct Routine
	// must not start on one, or the check ends it before the pull (no package row carries these enums in the install).
	while ((sim.clockTick() + 1) % Simulation::kSquadIntervalTicks == 0)
		sim.run(1);
	// The player stands by: a world squad away from every player is demoted and its member entities culled.
	if (state::Fighter* me = fighterOf(sim, game.player))
	{
		me->x = x + 20.0;
		me->z = z + 20.0;
		me->y = y;
		me->clearRoute();
	}
	state::Fighter& f = *fighterOf(sim, squad.carrier);
	CombatArena::endFollow(f); // the squad's follow must not walk it off before the pull
	f.x = x;
	f.y = y;
	f.z = z;
	f.clearRoute();
	f.forwardSpeed = 0.0;
	f.lateralSpeed = 0.0;
	flecs::entity actor = entityOf(sim, squad.carrier);
	state::Routine routine;
	routine.goal = goal;
	routine.since = sim.clockTick();
	routine.nextScore = sim.clockTick();
	routine.chainTarget = building;
	actor.set<state::Routine>(routine);
	sim.run(ticks);
}

// The items this test pulls: Raw Iron (a plain ITEM, function 0: the miners' output) and Bread (function 3, the food
// fallback's one). Both exist in the vanilla data, and a BUILDING that produces Raw Iron for the machines' record.
struct Items
{
	std::string iron;
	std::string bread;
	std::string ironBuilding;
	std::string boxBuilding; // a BUILDING with `has inventory` (the storage boxes' record)
};

Items findItems(Simulation& sim)
{
	Items items;
	for (const kdata::Record& record : sim.gameData()->records())
	{
		if (record.type == kworld::itemTypeByName("ITEM") && record.name == "Raw Iron")
			items.iron = record.stringID;
		if (record.type == kworld::itemTypeByName("ITEM") && record.name == "Bread")
			items.bread = record.stringID;
		if (items.boxBuilding.empty() && record.type == kworld::itemTypeByName("BUILDING") && record.boolOr("has inventory", false))
			items.boxBuilding = record.stringID;
	}
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("BUILDING")))
	{
		producers::Spec spec;
		if (items.ironBuilding.empty() && !items.iron.empty() && producers::specOf(*sim.gameData(), *record, spec) && spec.output == items.iron)
			items.ironBuilding = record->stringID;
	}
	return items;
}

// The pure quantity function (dt24_emptymachine_dismantle section 2.1): vf5, the BuildMats override and the +0x78 mode.
void testQuantity()
{
	const std::vector<worldres::NearbyStock> none;
	test::require(worldres::pullQuantity(0, false, none, false, false) == 0, "an empty neighbourhood and no food: n = 0");
	test::require(worldres::pullQuantity(0, true, none, false, false) == 1, "an empty-handed food take: n = 1");
	const std::vector<worldres::NearbyStock> stock{{3, false}};
	test::require(worldres::pullQuantity(0, false, stock, false, false) == 3, "another machine's stock adds on (3)");
	const std::vector<worldres::NearbyStock> room{{0, true}};
	test::require(worldres::pullQuantity(0, false, room, false, false) == 5, "a machine that can take it: the flat 5");
	test::require(worldres::pullQuantity(2, false, room, false, false) == 5, "the flat 5 replaces the carried count, not adds to it");
	test::require(worldres::pullQuantity(2, false, none, false, false) == 2, "only the carried count: n = 2");
	test::require(worldres::pullQuantity(0, false, none, true, false) == 2, "BuildMats takes two");
	test::require(worldres::pullQuantity(0, false, none, false, true) == 10, "the +0x78 mode takes ten");
	test::require(worldres::pullQuantity(4, true, stock, false, false) == 7, "the stock elsewhere adds onto the carried count");
}

// The picker (machine92 / container284): the nearest building of mine, and the type it lists first.
void testPickers()
{
	state::Fighter self;
	self.x = 100.0;
	self.z = 100.0;
	worldres::StockBuilding far;
	far.id = 7;
	far.x = 250.0;
	far.z = 100.0;
	far.mine = true;
	far.types = {"Raw Iron"};
	worldres::StockBuilding nearOne;
	nearOne.id = 8;
	nearOne.x = 110.0;
	nearOne.z = 100.0;
	nearOne.mine = true;
	nearOne.types = {"Bread", "Raw Iron"};
	worldres::StockBuilding foreign;
	foreign.id = 9;
	foreign.x = 101.0;
	foreign.z = 100.0;
	foreign.mine = false;
	foreign.types = {"Bread"};
	const std::vector<worldres::StockBuilding> buildings{far, nearOne, foreign};
	std::string type;
	test::require(worldres::machine92(buildings, self, worldres::kStockReach, &type) == 8, "92 picks the nearest machine of mine");
	test::require(type == "Bread", "and its first collectable type");
	test::require(worldres::container284(buildings, self, worldres::kStockReach, nullptr) == 8, "284 picks the nearest building of mine");
	worldres::StockBuilding distant;
	distant.id = 10;
	distant.x = 400.0;
	distant.z = 100.0;
	distant.mine = true;
	distant.types = {"Bread"};
	const std::vector<worldres::StockBuilding> outOfReach{distant};
	test::require(worldres::machine92(outOfReach, self, worldres::kStockReach, nullptr) == 0, "nothing past the picker's reach");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_empty_machine NAVWORLD");
		testQuantity();
		testPickers();

		Game game(argv[1]);
		Simulation& sim = *game.sim;
		sim.run(200);
		const Items items = findItems(sim);
		test::require(!items.iron.empty() && !items.bread.empty(), "Raw Iron and Bread exist in the data");
		test::require(!items.ironBuilding.empty(), "a BUILDING that produces Raw Iron exists in the data");
		Squad squad = placeSquad(game);

		// Every case stands its buildings in its own patch (more than the picker's 200 u apart) so the vf5 neighbourhood is
		// only the one it builds. A patch snaps to the nearest walkable mesh point: the executor walks to the use point, and an
		// off-mesh patch could never be reached (the Hub bake ends well before some of these offsets).
		const auto ground = [&](double x, double z) { return sim.groundHeight(x, z).value_or(0.0); };
		const auto snap = [&](double x, double z)
		{
			const knav::Position at{x, ground(x, z), z};
			const std::optional<knav::Position> spot = game.mesh->walker().snap(at, 300.0);
			return spot ? std::array<double, 3>{spot->x, spot->y, spot->z} : std::array<double, 3>{at.x, at.y, at.z};
		};

		// ---- 3. 92: one unit at a time, up to the stock elsewhere ----
		{
			const auto patch = snap(kSpawnX, kSpawnZ);
			const double x = patch[0];
			const double y = patch[1];
			const double z = patch[2];
			const std::uint64_t target = makeMachine(sim, 9001, items.ironBuilding, items.iron, squad.factionSid, 10, 20, x, y, z);
			makeMachine(sim, 9002, items.ironBuilding, items.iron, squad.factionSid, 3, 20, x + 40.0, y, z);
			resetPack(sim, squad.carrier);
			runPull(game, squad, routines::task::EmptyMachine, target, x, y, z, 60);
			const std::uint32_t took = carriedOf(sim, squad.carrier, items.iron);
			test::require(took == 3, "92 takes the other machine's stock (3), not the target's ten: took " + std::to_string(took));
			test::require(entityOf(sim, target).get<state::Inventory>()->carried(items.iron) == 7, "the target keeps the rest");
			std::cout << "92: 3 of 10 into the carrier (the nearby machine's stock)\n";
		}
		// ---- 4. the flat 5: a nearby machine of the same type with room and no stock ----
		{
			const auto patch = snap(kSpawnX, kSpawnZ - 600.0);
			const double x = patch[0];
			const double y = patch[1];
			const double z = patch[2];
			const std::uint64_t target = makeMachine(sim, 9003, items.ironBuilding, items.iron, squad.factionSid, 10, 20, x, y, z);
			makeMachine(sim, 9004, items.ironBuilding, items.iron, squad.factionSid, 0, 20, x + 40.0, y, z);
			resetPack(sim, squad.carrier);
			const std::uint32_t before = carriedOf(sim, squad.carrier, items.iron);
			runPull(game, squad, routines::task::EmptyMachine, target, x, y, z, 60);
			const std::uint32_t took = carriedOf(sim, squad.carrier, items.iron) - before;
			test::require(took == 5, "the nearby machine's room gives the flat 5: took " + std::to_string(took));
			std::cout << "92: 5 into the carrier (the flat 5)\n";
		}
		// ---- 5. 247: two per pass, whatever the neighbourhood ----
		{
			const auto patch = snap(kSpawnX + 600.0, kSpawnZ);
			const double x = patch[0];
			const double y = patch[1];
			const double z = patch[2];
			const std::uint64_t target = makeMachine(sim, 9005, items.ironBuilding, items.iron, squad.factionSid, 10, 20, x, y, z);
			resetPack(sim, squad.carrier);
			const std::uint32_t before = carriedOf(sim, squad.carrier, items.iron);
			runPull(game, squad, routines::task::BuildMats, target, x, y, z, 60);
			const std::uint32_t took = carriedOf(sim, squad.carrier, items.iron) - before;
			test::require(took == 2, "247 takes two per pass: took " + std::to_string(took));
			std::cout << "247: 2 into the carrier (BuildMats)\n";
		}
		// ---- 6a. 284: a storage box, the stock elsewhere adds on ----
		{
			const auto patch = snap(kSpawnX, kSpawnZ - 1200.0);
			const double x = patch[0];
			const double y = patch[1];
			const double z = patch[2];
			const std::uint64_t target = makeBox(sim, 9006, items.boxBuilding, squad.factionSid, {{items.iron, 4}}, x, y, z);
			makeBox(sim, 9007, items.boxBuilding, squad.factionSid, {{items.iron, 2}}, x + 40.0, y, z);
			resetPack(sim, squad.carrier);
			const std::uint32_t before = carriedOf(sim, squad.carrier, items.iron);
			runPull(game, squad, routines::task::LootContainer, target, x, y, z, 60);
			const std::uint32_t took = carriedOf(sim, squad.carrier, items.iron) - before;
			test::require(took == 2, "284 takes the other box's stock (2): took " + std::to_string(took));
			test::require(entityOf(sim, target).get<state::Inventory>()->carried(items.iron) == 2, "the target box keeps the rest");
			std::cout << "284: 2 of 4 into the carrier (the second box's stock)\n";
		}
		// ---- 6b. 284: a food item alone is one per pass (the empty-handed function-3 baseline) ----
		{
			const auto patch = snap(kSpawnX + 600.0, kSpawnZ - 600.0);
			const double x = patch[0];
			const double y = patch[1];
			const double z = patch[2];
			const std::uint64_t target = makeBox(sim, 9008, items.boxBuilding, squad.factionSid, {{items.bread, 3}}, x, y, z);
			resetPack(sim, squad.carrier);
			const std::uint32_t before = carriedOf(sim, squad.carrier, items.bread);
			runPull(game, squad, routines::task::LootContainer, target, x, y, z, 60);
			const std::uint32_t took = carriedOf(sim, squad.carrier, items.bread) - before;
			test::require(took == 1, "a lone food item: one per pass, took " + std::to_string(took));
			std::cout << "284: 1 of 3 bread into the carrier (function 3, empty-handed)\n";
		}
		// ---- 7. the live planner (dt24_emptymachine_source section 3): the rows read against the live world ----
		{
			const auto patch = snap(kSpawnX + 1200.0, kSpawnZ);
			const double x = patch[0];
			const double y = patch[1];
			const double z = patch[2];
			const std::uint64_t machine = makeMachine(sim, 9009, items.ironBuilding, items.iron, squad.factionSid, 20, 20, x, y, z);
			resetPack(sim, squad.carrier);
			standAt(sim, squad.carrier, x, y, z);
			const goap::Plan full = sim.planWorkChain(squad.carrier, goap::kAutoLabouringMines, machine);
			test::require(full.found() && full.steps.size() == 3
				&& full.steps[0].task == routines::task::EmptyMachine && full.steps[1].task == routines::task::OperateMachinery
				&& full.steps[2].task == goap::kAutoLabouringMines,
				"a full machine, an empty-handed worker: the plan is 92 -> 87 -> the goal");
			std::cout << "plan: a full machine plans 92 -> 87 (the collection inserted before the operate step)\n";

			const std::uint64_t box = makeBox(sim, 9010, items.boxBuilding, squad.factionSid, {{items.bread, 2}}, x, y, z + 40.0);
			resetPack(sim, squad.carrier);
			standAt(sim, squad.carrier, x, y, z + 40.0);
			const goap::Plan loot = sim.planWorkChain(squad.carrier, goap::kLootFoodAndStuff, box);
			test::require(loot.found() && loot.steps.size() == 2 && loot.steps[0].task == routines::task::LootContainer
				&& loot.steps[1].task == goap::kLootFoodAndStuff, "131 at a stocked box: 284 -> the goal");
			// Take from it, then plan again: the goal state haveSomeLootFrom holds (the worker carries the loot it took).
			runPull(game, squad, routines::task::LootContainer, box, x, y, z + 40.0, 60);
			const goap::Plan after = sim.planWorkChain(squad.carrier, goap::kLootFoodAndStuff, box);
			test::require(after.found() && after.steps.size() == 1 && after.steps[0].task == goap::kLootFoodAndStuff,
				"with the loot in hand the goal holds: no 284 in the new plan");
			std::cout << "plan: 131 plans 284 at a box, and only the goal once the loot is in hand\n";
		}
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_empty_machine OK\n";
	return 0;
}
