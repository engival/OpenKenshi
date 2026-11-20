// The homebase rows of the native selector (kinspect dt24_243 / dt24_275 / dt24_175): 243 AQUIRE_FOOD_AT_HOMEBASE,
// 275 SIT_AROUND and 175 SLEEP_ON_FLOOR.
//
// Where the data issues them (ai_coverage.py's package rows): 243 and 275 are rows of the two 'occupy town' packages
// (96451-rebirth.mod 'hostile occupy town' / 97241 'campaign victory occupy town'), carried by the bandit raid templates;
// 175 SLEEP_ON_FLOOR is a row of the animal packages (the farm-animals 0-24 window, the nocturnal and night windows). Every
// carrying template's behaviour is raid / travel / animal_territory - not a resident routine - so all three run in the
// NATIVE WORLD SELECTOR (stepWorldTasks), not the town scorer.
//
// These cases run the live path: a real new game at the Hub, a squad placed by its template, the selector picking the row,
// the GOAP plan, then the executor.
//  1. 243: a food box of the squad's faction by the squad. A member plans [67, 244, 243], walks to the box and takes one
//     food unit (244 GRAB_ONE_FOOD), then the goal re-scores.
//  2. 275: the squad inside the Hub's town circle. A member plans [190, 275], walks to a building of the town and holds
//     (0.2 + rand x 0.8 game hours).
//  3. 175: an animal template (holy farm bulls, the farm-animals package, 0-24). A member plans [175] alone, lies down where
//     it stands (asleep, the sleeponfloor clip) for its rolled hold, then gets up.
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/ai/blackboard.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/detail.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/economy/inventory_grid.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "knav/navmesh.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

// The wilderness slot the other Hub tests use (ksim_after_fight, ksim_empty_machine).
constexpr double kSpawnX = -48880.0;
constexpr double kSpawnZ = -823.0;
// The bandit template carrying the 'occupy town' package (the 243 / 275 rows).
constexpr const char* kBandits = "1302-gamedata.base";
// The animal template carrying 'farm animals' (175, the 0-24 window): holy farm bulls.
constexpr const char* kAnimals = "43935-rebirth.mod";

std::ostream& say()
{
	static std::ostream nowhere(nullptr);
	return std::getenv("KSIM_HOMEBASE_VERBOSE") != nullptr ? std::cout : nowhere;
}

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

std::vector<flecs::entity> membersOf(Simulation& sim, flecs::entity squad)
{
	std::vector<std::pair<std::uint64_t, flecs::entity>> found;
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each(
		[&](flecs::entity member, const state::Id& id) { found.emplace_back(id.value, member); });
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	std::vector<flecs::entity> out;
	for (const auto& [id, member] : found)
		out.push_back(member);
	return out;
}

// A storage box of the squad's faction, built the way the spawner builds one (ksim_empty_machine's makeBox).
void makeBox(Simulation& sim, std::uint64_t id, const std::string& recordSid, const std::string& ownerSid,
	const std::string& item, std::uint32_t count, double x, double y, double z)
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
	stock.add(item, count);
	flecs::entity entity = ecsOf(sim).entity();
	entity.set<state::Id>(state::Id{id});
	entity.set<state::Container>(std::move(container));
	entity.set<state::Inventory>(std::move(stock));
}

struct Game
{
	std::unique_ptr<Simulation> sim;
	std::shared_ptr<const CombatData> combat;
	std::shared_ptr<MeshNavigator> mesh;
	std::uint64_t player = 0;

	explicit Game(const std::string& bake)
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		mesh = test::openWorldNav(bake, hubZones);
		SpawnOptions options;
		options.seed = 1;
		// The wilderness spawner must exist (placeWildernessSquad), but no density spawn: the cases place their own squads.
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
		std::string sid;
		for (const kdata::Record* record : sim->gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			if (record->name == "Wanderer")
				sid = record->stringID;
		sim->enqueue(Command::newGame(sid, "Tester", 1));
		if (std::getenv("KSIM_HOMEBASE_VERBOSE") != nullptr)
			sim->setWorldSquadLog(1, [](const std::string& line) { std::cout << "world: " << line << "\n"; });
		sim->run(3);
		for (const CombatArena::Member& member : sim->combat()->members())
			if (member.fighter.playerControlled)
				player = member.id;
	}

	// The player's camera is the squad's spot (the area streams to Full, and a Full world squad builds Fighters); the player
	// CHARACTER stands well away, so the hostile squad has nobody to fight and the package goals are what the selector scores.
	void bringPlayer(double x, double y, double z)
	{
		if (state::Fighter* me = fighterOf(*sim, player))
		{
			me->x = x + 400.0;
			me->y = y;
			me->z = z + 400.0;
			me->clearRoute();
			me->down = false;
			me->unconscious = false;
		}
	}

	// The bandit squad on the 'occupy town' package. 96451 is the OccupyTown class (signal 30 = 0x1e): it never starts by itself, only
	// after PassingByTownAssault (dt41 F s3), so the squad is put on it as `change AI` would.
	void occupyTown(flecs::entity squad)
	{
		blackboard::Board* board = sim->blackboardOf(squad);
		test::require(board != nullptr, "the bandit squad has a blackboard");
		blackboard::Package occupy;
		test::require(blackboard::readPackage(*sim->gameData(), "96451-rebirth.mod", 2, occupy), "no AI package 96451");
		blackboard::changeAI(*board, std::move(occupy));
	}

	// A placed squad built at Full detail (the player's proximity).
	flecs::entity place(const char* templateSid, double x, double z)
	{
		const flecs::entity squad = sim->placeWildernessSquad(templateSid, x, z);
		bringPlayer(x, sim->groundHeight(x, z).value_or(sim->combat()->find(player)->y), z);
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

std::string factionOf(Simulation& sim, flecs::entity squad)
{
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	if (instance == nullptr || instance->faction.id == 0)
		return {};
	const state::FactionState* faction = ecsOf(sim).entity(instance->faction.id).get<state::FactionState>();
	return faction != nullptr ? faction->templateSid : std::string{};
}

// The first member of the squad on `goal`, its Routine and plan.
const state::Routine* onGoal(const std::vector<flecs::entity>& members, std::uint16_t goal, std::uint64_t* who)
{
	for (flecs::entity member : members)
	{
		const state::Routine* routine = member.get<state::Routine>();
		if (routine != nullptr && routine->goal == goal)
		{
			*who = member.get<state::Id>()->value;
			return routine;
		}
	}
	return nullptr;
}

std::string planText(const state::Routine& routine)
{
	std::string out;
	for (std::size_t i = 0; i < routine.plan.size(); ++i)
		out += (i == 0 ? "" : " -> ") + std::to_string(routine.plan[i]);
	return out;
}

// ---- 1. 243 AQUIRE_FOOD_AT_HOMEBASE ----
void testFood(Game& game)
{
	Simulation& sim = *game.sim;
	const flecs::entity squad = game.place(kBandits, kSpawnX, kSpawnZ);
	game.occupyTown(squad);
	const std::vector<flecs::entity> members = membersOf(sim, squad);
	test::require(!members.empty(), "the bandit squad was built");
	const std::string owner = factionOf(sim, squad);
	test::require(!owner.empty(), "the bandit squad has a faction");

	// Bread is a food item (ITEM function 3): the exe's food mask reads exactly that.
	const kdata::Record* bread = nullptr;
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("ITEM")))
		if (record->name == "Bread")
			bread = record;
	test::require(bread != nullptr, "the data carries Bread");
	const ItemInfo* info = game.combat->item(bread->stringID);
	test::require(info != nullptr && info->function == kItemFunctionFood, "Bread is a food item (function 3)");
	std::string box;
	for (const kdata::Record& record : sim.gameData()->records())
		if (box.empty() && record.type == kworld::itemTypeByName("BUILDING") && record.boolOr("has inventory", false))
			box = record.stringID;
	test::require(!box.empty(), "the data carries a storage-box BUILDING");

	// The box stands by the squad (the exe's finder runs over the base from the character's position).
	state::Fighter* leader = fighterOf(sim, members.front().get<state::Id>()->value);
	test::require(leader != nullptr, "the squad's first member is a Fighter");
	const double bx = leader->x + 40.0;
	const double bz = leader->z + 10.0;
	const double by = leader->y;
	// A distinctive Id: a small integer can collide with a world entity's own Id in this world.
	const std::uint64_t boxId = 0x600D000000000243ull;
	makeBox(sim, boxId, box, owner, bread->stringID, 3, bx, by, bz);
	test::require(entityOf(sim, boxId).get<state::Container>() != nullptr, "the food box stands (Container set)");
	test::require(entityOf(sim, boxId).get<state::Inventory>() != nullptr, "the food box holds an inventory");

	// The selector scores the package rows each 20-tick boundary; 243 is the only offering row out here (no town, no fight,
	// no patient, no body).
	if (std::getenv("KSIM_HOMEBASE_VERBOSE") != nullptr)
	{
		const Detail* detail = squad.get<Detail>();
		std::cout << "food: squad " << squad.get<state::Id>()->value << " members " << members.size() << " detail "
				  << (detail != nullptr ? static_cast<int>(detail->level) : -1) << " position " << (squad.get<state::Position>() != nullptr)
				  << " faction '" << owner << "'\n";
		for (flecs::entity member : members)
		{
			const state::Fighter* f = member.get<state::Fighter>();
			const state::Routine* r = member.get<state::Routine>();
			std::cout << "   member " << member.get<state::Id>()->value << " target " << (f != nullptr ? f->target : 0)
					  << " goal " << (r != nullptr ? r->goal : 0) << " role "
					  << (member.get<state::MemberRole>() != nullptr ? static_cast<int>(member.get<state::MemberRole>()->list) : -1)
					  << " fighter " << (sim.combat()->find(member.get<state::Id>()->value) != nullptr) << "\n";
		}
	}
	std::uint64_t who = 0;
	const state::Routine* routine = nullptr;
	for (int i = 0; i < 400 && routine == nullptr; ++i)
	{
		game.tick();
		routine = onGoal(membersOf(sim, squad), routines::task::AcquireFood, &who);
	}
	test::require(routine != nullptr, "a member picks 243 AQUIRE_FOOD_AT_HOMEBASE");
	say() << "food: member " << who << " plans " << planText(*routine) << "\n";
	// The G4 chain: 244 GRAB_ONE_FOOD is in front of the goal (67 the walk when the member is not at the box).
	test::require(std::find(routine->plan.begin(), routine->plan.end(), routines::task::GrabOneFood) != routine->plan.end(),
		"the 243 plan carries 244 GRAB_ONE_FOOD (" + planText(*routine) + ")");
	test::require(routine->plan.back() == routines::task::AcquireFood, "the plan ends at the 243 goal entry");
	test::require(routine->chainTarget == boxId, "the plan's hand is the food box");

	// The take: one unit out of the box and into the member's pack (244's quantity is one, not vf5's).
	const std::uint32_t before = entityOf(sim, boxId).get<state::Inventory>()->carried(bread->stringID);
	const std::uint32_t mineBefore = entityOf(sim, who).get<state::Inventory>()->carried(bread->stringID);
	for (int i = 0; i < 2000 && entityOf(sim, boxId).get<state::Inventory>()->carried(bread->stringID) == before; ++i)
		game.tick();
	const std::uint32_t after = entityOf(sim, boxId).get<state::Inventory>()->carried(bread->stringID);
	const std::uint32_t mineAfter = entityOf(sim, who).get<state::Inventory>()->carried(bread->stringID);
	test::require(after == before - 1, "244 takes one unit from the box (" + std::to_string(before) + " -> " + std::to_string(after) + ")");
	test::require(mineAfter == mineBefore + 1, "the unit lands in the member's pack");
	test::require(before > 0, "the box held food to take");
	// The box is emptied for the next case: its leftover bread (the squad's other members take units too, as many as arrive in the tick the
	// first one does) would make a squad in the town choose 243 over 275 and walk to it. Which members arrive in that tick is walk timing
	// (kenshi-k16.8.9: the eased arrival spread them out and left bread); the 275 case must not depend on it.
	entityOf(sim, boxId).set<state::Inventory>(state::Inventory{});
	std::cout << "243: member " << who << " took 1 x Bread from box " << boxId << " (plan " << planText(*routine) << ")\n";
}

// ---- 2. 275 SIT_AROUND ----
void testSit(Game& game)
{
	Simulation& sim = *game.sim;
	// Inside the Hub's town circle: atATown holds, and the town's buildings are the candidate seats.
	const flecs::entity squad = game.place(kBandits, kHubX + 30.0, kHubZ + 30.0);
	game.occupyTown(squad);
	const std::vector<flecs::entity> members = membersOf(sim, squad);
	test::require(!members.empty(), "the bandit squad was built in the Hub");
	std::uint64_t who = 0;
	const state::Routine* routine = nullptr;
	for (int i = 0; i < 400 && routine == nullptr; ++i)
	{
		game.tick();
		routine = onGoal(membersOf(sim, squad), routines::task::SitAround, &who);
	}
	test::require(routine != nullptr, "a member picks 275 SIT_AROUND in the town");
	say() << "sit: member " << who << " plans " << planText(*routine) << " at building " << routine->chainTarget << "\n";
	// The G4 chain [7 UNEQUIP_WEAPON, 190 AUTO_LABOURING_MINES_PRETEND, 275] for an armed bandit (190 is the only writer of
	// IS_WORKING_PRETEND; 7 sheathes for its hasWeaponEquipped = 0).
	test::require(routine->plan.size() >= 2 && routine->plan.back() == routines::task::SitAround
		&& std::find(routine->plan.begin(), routine->plan.end(), routines::task::PretendLabourPretend) != routine->plan.end(),
		"275 plans the pretend step in front of the goal (" + planText(*routine) + ")");
	test::require(routine->chainTarget != 0, "the routine carries the building the resolver picked");

	// The sit: the member reaches the building and the hold starts (a rolled 0.2 + rand x 0.8 game hours).
	const state::Fighter* f = nullptr;
	for (int i = 0; i < 4000; ++i)
	{
		game.tick();
		f = sim.combat()->find(who);
		const state::Routine* now = entityOf(sim, who).get<state::Routine>();
		if (f == nullptr || now == nullptr || now->goal != routines::task::SitAround)
			break;
		if (now->holdUntil != 0)
			break;
	}
	const state::Routine* sitting = entityOf(sim, who).get<state::Routine>();
	test::require(sitting != nullptr && sitting->goal == routines::task::SitAround && sitting->holdUntil != 0,
		"the member arrives and the sit's hold starts");
	const double hours = static_cast<double>(sitting->holdUntil - sim.clockTick()) / static_cast<double>(kTicksPerGameHour);
	test::require(hours > 0.15 && hours <= 1.05, "the hold is 0.2 + rand x 0.8 game hours (" + std::to_string(hours) + " h left)");
	std::cout << "275: member " << who << " sits in building " << sitting->chainTarget << " for the hold ("
			  << hours << " h left)\n";
}

// ---- 3. 175 SLEEP_ON_FLOOR ----
void testSleep(Game& game)
{
	Simulation& sim = *game.sim;
	const flecs::entity squad = game.place(kAnimals, kSpawnX, kSpawnZ);
	const std::vector<flecs::entity> members = membersOf(sim, squad);
	test::require(!members.empty(), "the animal squad was built");

	std::uint64_t who = 0;
	const state::Routine* routine = nullptr;
	for (int i = 0; i < 400 && routine == nullptr; ++i)
	{
		game.tick();
		routine = onGoal(membersOf(sim, squad), routines::task::SleepOnFloor, &who);
	}
	test::require(routine != nullptr, "an animal picks 175 SLEEP_ON_FLOOR");
	say() << "sleep: member " << who << " plans " << planText(*routine) << "\n";
	test::require(routine->plan.size() == 1 && routine->plan.front() == routines::task::SleepOnFloor,
		"175 is terminal: the plan is [175] (" + planText(*routine) + ")");

	// It lies down where it stands: asleep, the sleeponfloor clip, and a hold of the rolled floor-sleep time.
	const state::Fighter* f = nullptr;
	const state::Routine* sleeping = nullptr;
	for (int i = 0; i < 200; ++i)
	{
		game.tick();
		f = sim.combat()->find(who);
		sleeping = entityOf(sim, who).get<state::Routine>();
		if (f == nullptr || sleeping == nullptr || sleeping->goal != routines::task::SleepOnFloor)
			break;
		if (f->asleep)
			break;
	}
	test::require(f != nullptr && f->asleep, "the animal lies down (asleep)");
	test::require(sleeping != nullptr && sleeping->holdUntil != 0, "the floor sleep's hold is set");
	const std::optional<Simulation::Pose> pose = sim.pose(who);
	test::require(pose.has_value() && pose->clip == routines::kClipSleepOnFloor,
		"the sleeper's pose is the sleeponfloor clip");
	const double hours = static_cast<double>(sleeping->holdUntil - sim.clockTick()) / static_cast<double>(kTicksPerGameHour);
	say() << "sleep: asleep, pose " << pose->clip << ", " << hours << " h left\n";

	// It gets up when the time runs out (bounded by the rolled hold, at most the bed hold's 8 h).
	const std::uint64_t wake = sleeping->holdUntil;
	for (int i = 0; i < 200000; ++i)
	{
		if (sim.clockTick() >= wake + 2)
			break;
		game.tick();
	}
	const state::Fighter* up = sim.combat()->find(who);
	test::require(up != nullptr && !up->asleep, "the animal wakes when the hold ends");
	// Out of the task loop the selector re-scores; the row offers again (its window is open) - the member is either asleep
	// again or between sleeps, never stuck in the old Routine.
	std::cout << "175: member " << who << " slept (" << hours << " h) and got up\n";
}

} // namespace

// 255 SIT_ON_THRONE (dt24_43 section 2c): 43's resolver on the throne slot, the exe's finder mask 0x1000000 = building function 24 ("throne chair").
// The Throne buildings classify as the Throne site kind (the layout furniture index keeps them as exclusive real pieces), 255 is a modelled
// task with 43's hold, and a chair or table is not a throne.
void testThrone()
{
	const kdata::GameData data = kworld::loadMergedGameData(KENSHI_DATA);
	int thrones = 0;
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("BUILDING")))
	{
		const routines::SiteKind kind = routines::classifyPiece(data, *record);
		if (record->name == "Throne" || record->name == "Hive Queen Throne")
		{
			test::require(kind == routines::SiteKind::Throne, record->name + " is not classified as a throne");
			++thrones;
		} else if (kind == routines::SiteKind::Throne) {
			test::require(false, record->name + " classified as a throne");
		}
	}
	test::require(thrones >= 2, "the data holds no throne chair buildings");
	const routines::TaskInfo* info = routines::taskInfo(routines::task::SitOnThrone);
	test::require(info != nullptr && info->use == routines::TaskUse::Modelled, "255 is not a modelled task");
	const routines::Hold hold = routines::holdOf(routines::task::SitOnThrone);
	test::require(hold.base == routines::kHoldStayInHome.base && hold.span == routines::kHoldStayInHome.span, "255 does not hold like 43");
	std::cout << "  255: throne chairs classify as the Throne site kind; 255 holds like 43\n";
}

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_homebase NAVWORLD");
		Game game(argv[1]);
		testFood(game);
		testSit(game);
		testSleep(game);
		testThrone();
	} catch (const std::exception& e) {
		std::cerr << "ksim_homebase: FAILED: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_homebase OK\n";
	return 0;
}
