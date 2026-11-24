// Task k16.6.14 (kinspect rwh.8 / rwh.9): a slaver caravan that has cuffed a captive takes the slaves to the nearest SLAVE_CAMP and
// hands them over (SLAVERY section 6). The platoon's package switches to the deliver package once its ownership set is non-empty, the
// finder installs the camp, the squad walks there by the world route and the leader gives every owned slave to the camp's keeper;
// the emptied set finishes the package and the ladder falls back. Runs on the server's Hub game with the squad AI; the camp is
// placed next to the Hub (the data's own camps are tens of km away). Needs the world bake (argv[1]); exit 77 without it.
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "knav/navmesh.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "nav_source.hpp"
#include "sim_access.hpp"
#include "ksim/dev/build_paths.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {
int gIdle = 0; // extra idle ticks at the start of each fixture: a phase shift the outcome checks must survive

using namespace ksim;

// A wilderness spawn point the other Hub tests use (ksim_after_fight): a colour slot with room for a squad.
constexpr double kSpawnX = -48880.0;
constexpr double kSpawnZ = -823.0;
// without walking on (task BP).
// The nearest slave-storage building of the data (kinspect rwh.8: designation 10), the camp's hand-over point.
constexpr double kStorageX = -40419.143518;
constexpr double kStorageZ = -12433.025172;

flecs::entity entityOf(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { found = i.value == id ? e : found; });
	return found;
}

struct Person
{
	std::uint64_t id = 0;
	flecs::entity entity;
};

std::vector<Person> membersOf(Simulation& sim, flecs::entity squad)
{
	std::vector<Person> found;
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& id)
	{
		found.push_back({id.value, member});
	});
	std::sort(found.begin(), found.end(), [](const Person& a, const Person& b) { return a.id < b.id; });
	return found;
}

flecs::entity playerFaction(Simulation& sim)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::PlayerFaction&, const state::FactionState&) { found = e; });
	return found;
}

struct Game
{
	std::string world;
	kworld::LoadOrder order;
	std::shared_ptr<const CombatData> combat;
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;
	bool withAi = false;
	std::function<void(Simulation&)> beforeAi; // the squad AI reads the towns once, when it is enabled

	Game(const std::string& bake, bool ai, double aroundX = kHubX, double aroundZ = kHubZ)
		: world(bake)
		, order(kworld::resolveLoadOrder(KENSHI_DATA, ""))
		, withAi(ai)
	{
		std::vector<kworld::ZoneCell> zones;
		const kworld::ZoneCell centre = kworld::zoneCellOf(aroundX, aroundZ);
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({centre.x + dx, centre.z + dz});
		}
		mesh = test::openWorldNav(world, zones);
	}

	void newGame()
	{
		SpawnOptions options;
		options.seed = 1;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->markPristine();
		combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings s;
		s.substeps = 2;
		sim->enableCombat(combat, s);
		sim->enableNavigation(mesh);
		if (beforeAi)
			beforeAi(*sim);
		if (withAi)
		{
			AiSystem::Config ai;
			ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
			ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
			ai.factions = FactionBook::build(sim->gameData());
			sim->enableAi(std::move(ai));
		}
		std::string sid;
		for (const kdata::Record* record : sim->gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
		{
			if (record->name == "Wanderer")
				sid = record->stringID;
		}
		sim->enqueue(Command::newGame(sid, "Tester", 1));
		sim->run(3 + gIdle);
		for (const CombatArena::Member& member : sim->combat()->members())
		{
			if (member.fighter.playerControlled)
				player = member.id;
		}
	}

	state::Fighter& me() { return *fighterOf(*sim, player); }

	void teleport(double x, double z)
	{
		state::Fighter& f = me();
		f.x = x;
		f.z = z;
		f.y = sim->groundHeight(x, z).value_or(f.y);
		f.clearRoute();
		f.building = 0;
	}
};

// Places a wilderness squad, makes it hostile to the player's faction and waits until its members are built (Fighters).
struct Placed
{
	flecs::entity squad;
	std::vector<Person> members;
};

Placed placeHostile(Game& game, const char* templateSid, std::size_t expected, double x = kSpawnX, double z = kSpawnZ)
{
	Simulation& sim = *game.sim;
	const flecs::entity squad = sim.placeWildernessSquad(templateSid, x, z);
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	test::require(instance != nullptr && instance->faction.id != 0, "the placed squad has a faction");
	sim.enqueue(Command::setFactionRelation(*ecsOf(sim).entity(instance->faction.id).get<state::Id>(),
		*playerFaction(sim).get<state::Id>(), -60));
	Placed placed;
	placed.squad = squad;
	for (int i = 0; i < 20 * 20; ++i)
	{
		sim.run(1);
		placed.members = membersOf(sim, squad);
		const bool built = std::all_of(placed.members.begin(), placed.members.end(), [&](const Person& p) { return sim.combat()->find(p.id) != nullptr; });
		if (built && placed.members.size() >= expected)
			break;
	}
	test::require(placed.members.size() >= expected, "the squad was built with its members");
	for (const Person& p : placed.members)
	{
		test::require(sim.combat()->find(p.id) != nullptr, "the world squad's members are Fighters (it was promoted)");
	}
	return placed;
}

// Runs until the player is knocked out (or down); a player still standing after `seconds` is knocked out by the fixture (printed).
void runToKo(Game& game, double seconds, const char* what)
{
	Simulation& sim = *game.sim;
	for (int i = 0; i < static_cast<int>(20 * seconds); ++i)
	{
		sim.run(1);
		const state::Fighter& me = game.me();
		if (me.unconscious || me.down)
			return;
	}
	state::Fighter& me = game.me();
	std::cout << what << ": the player was still up after " << seconds << " s; the fixture knocks it out\n";
	me.unconscious = true;
	me.koTimer = 600.0;
	sim.run(1);
}


// True while the squad's applied package is one of the slaver shipping classes (0xb / 0x1c).
bool shipping(const blackboard::Board& board)
{
	for (const auto& [priority, packages] : board.table)
	{
		for (const blackboard::Package& package : packages)
		{
			if (package.sid == board.current && blackboard::slaverShipping(package.signalFunc))
				return true;
		}
	}
	return false;
}

// A SLAVE_CAMP (TOWN type 5) at the given point: a bare world has no TownState, the fixture makes one from the data's TOWN records.
std::uint64_t makeCamp(Simulation& sim, double x, double z)
{
	const kdata::Record* record = nullptr;
	for (const kdata::Record* candidate : sim.gameData()->byType(kworld::itemTypeByName("TOWN")))
	{
		if (candidate->intOr("type", 2) == 5)
		{
			record = candidate;
			break;
		}
	}
	test::require(record != nullptr, "the data has a TOWN of type 5 (SLAVE_CAMP)");
	state::TownState town;
	town.rootSid = record->stringID;
	town.activeSid = record->stringID;
	town.x = x;
	town.z = z;
	flecs::entity entity = state::createInstance(ecsOf(sim));
	entity.set<state::TownState>(town);
	return entity.get<state::Id>()->value;
}

// The caravan carries a captive from the capture to a slave camp 700 u away and hands it over. `shipsToo` is the expectation under test:
// without the delivery package the squad never switches and nothing is handed over.
void deliver(const std::string& bake)
{
	Game game(bake, true, kStorageX, kStorageZ);
	std::uint64_t camp = 0;
	double campX = 0.0;
	double campZ = 0.0;
	game.beforeAi = [&](Simulation& built)
	{
		// The camp's town point on the walkable ground by the building (the building's own centre is inside it, off the mesh). The start is
		// at the ground's height, as a sim route's is (the runtime mesh finds no route from a point 1,000 u below it).
		const double startY = built.groundHeight(kStorageX - 700.0, kStorageZ).value_or(0.0);
		const Route reach = built.navigation()->routeFrom({kStorageX - 700.0, startY, kStorageZ}, kStorageX, kStorageZ);
		test::require(reach.found && !reach.points.empty(), "the camp's approach has a walkable end");
		campX = reach.points.back().x;
		campZ = reach.points.back().z;
		camp = makeCamp(built, campX, campZ);
	};
	game.newGame();
	Simulation& sim = *game.sim;
	const BuildingSite* site = nullptr;
	for (const BuildingSite& candidate : sim.world().buildingSites())
		site = std::hypot(candidate.cx - kStorageX, candidate.cz - kStorageZ) < 1.0 && !candidate.squadTemplate.empty() ? &candidate : site;
	test::require(site != nullptr && camp != 0, "the storage building is placed and the camp made");

	// The camp: a TownState on the building, the keeper its squad's resident (homeBuilding names the building, as a town resident's does).
	game.teleport(site->cx + 30.0, site->cz);
	sim.run(20);
	const flecs::entity keepers = sim.placeWildernessSquad(site->squadTemplate.c_str(), site->cx, site->cz);
	sim.run(200); // the residents are built at Full detail
	// Every keeper-resident of the squad is a candidate: the sim hands over to the one nearest the building (slaveStoragesNear), which
	// the formation steer moves from run to run, so the test reads the pick back instead of assuming the squad's first member.
	std::vector<std::uint64_t> keepersAll;
	std::string keeperSide;
	for (const Person& p : membersOf(sim, keepers))
	{
		if (sim.combat()->find(p.id) != nullptr)
		{
			keepersAll.push_back(p.id);
			fighterOf(sim, p.id)->homeBuilding = site->key;
			// A resident placed outside its town has no faction: the camp's owner (the building's) is its side.
			keeperSide = site->ownerFaction;
			entityOf(sim, p.id).set<state::Allegiance>(state::Allegiance{keeperSide});
		}
	}
	test::require(!keepersAll.empty() && !keeperSide.empty(), "the camp has a keeper with a faction");

	// The caravan 700 u west of the camp, the player beside it.
	constexpr double kStart = 700.0;
	game.teleport(site->cx - kStart + 18.0, site->cz);
	sim.run(2);
	Placed caravan = placeHostile(game, "3387-gamedata.base", 2, site->cx - kStart, site->cz);
	// The camp's owner is allied to the caravan's faction (194 takes only an allied building; the data's own relation of the two is not
	// read, UNSURE: kinspect rwh.9 saw the Slave Traders' camp owned by the Holy Nation).
	flecs::entity ownerFaction;
	ecsOf(sim).each([&](flecs::entity e, const state::FactionState& faction) { ownerFaction = faction.templateSid == site->ownerFaction ? e : ownerFaction; });
	test::require(ownerFaction.is_valid(), "the camp's owner faction exists");
	const state::Id caravanFaction = *ecsOf(sim).entity(caravan.squad.get<state::SquadInstance>()->faction.id).get<state::Id>();
	sim.enqueue(Command::setFactionRelation(caravanFaction, *ownerFaction.get<state::Id>(), 80));
	sim.enqueue(Command::setFactionRelation(*ownerFaction.get<state::Id>(), caravanFaction, 80));
	sim.run(20 * 11); // the arena's bonds are rebuilt every 10 s
	sim.enqueue(Command::fighterAttack(state::Id{game.player}, state::Id{caravan.members.front().id}));
	runToKo(game, 90.0, "caravan");
	test::require(game.me().unconscious || game.me().down, "the caravan puts the player down");

	blackboard::Board* board = sim.blackboardOf(caravan.squad);
	test::require(board != nullptr, "the caravan has a package board");
	test::require(!shipping(*board) && board->owned.empty(), "before the cuff the squad is not shipping");

	std::uint64_t cuffTick = 0;
	std::uint64_t switchTick = 0;
	std::uint64_t carryTick = 0;
	std::uint64_t doneTick = 0;
	double nearest = 1.0e30;
	std::size_t peakOwned = 0;
	const std::uint64_t startTick = sim.clockTick();
	for (int i = 0; i < 20 * 600 && doneTick == 0; ++i)
	{
		state::Fighter& me = game.me();
		if (cuffTick == 0 && me.slaveState != 1)
		{
			// The captive stays down until the cuff: whether it comes round of itself in the wait is the fight's outcome (it moved with the entity Ids), and
			// the test is about carrying a downed captive.
			me.unconscious = true;
			me.koTimer = 600.0;
		}
		if (doneTick == 0 && me.slaveState == 1 && me.captorSid.size() && me.captorSid != keeperSide && (me.unconscious || me.carriedBy != 0))
		{
			me.unconscious = true; // the KO is held through the walk (the capture is live-checked with a KO'd captive)
			me.koTimer = 600.0;
		}
		sim.run(1);
		board = sim.blackboardOf(caravan.squad);
		if (cuffTick == 0 && game.me().slaveState == 1)
			cuffTick = sim.clockTick();
		if (switchTick == 0 && shipping(*board))
			switchTick = sim.clockTick();
		if (carryTick == 0 && game.me().carriedBy != 0)
			carryTick = sim.clockTick();
		peakOwned = std::max(peakOwned, board->owned.size());
		for (const Person& p : caravan.members)
		{
			const state::Fighter* f = sim.combat()->find(p.id);
			if (f != nullptr && !f->dead)
				nearest = std::min(nearest, std::hypot(f->x - campX, f->z - campZ));
		}
		if (cuffTick != 0 && switchTick != 0 && board->owned.empty() && !shipping(*board))
			doneTick = sim.clockTick();
		if (i % (20 * 30) == 0)
			std::cout << "t+" << (sim.clockTick() - startTick) / 20 << " s: slave " << (int)game.me().slaveState << " carried " << game.me().carriedBy
				<< " owned " << board->owned.size() << " shipping " << shipping(*board) << " depot " << board->depot << " nearest " << nearest << "\n";
	}
	const state::Fighter& me = game.me();
	std::cout << "deliver: cuff " << cuffTick << " switch " << switchTick << " carry " << carryTick << " done " << doneTick << " nearest "
		<< nearest << " peakOwned " << peakOwned << " captor " << me.captorSid << " follow " << me.followTarget << " (keepers " << keepersAll.size() << " side " << keeperSide << ")\n";

	std::vector<std::string> failures;
	const auto check = [&](bool ok, const std::string& what) { if (!ok) failures.push_back(what); };
	check(cuffTick != 0, "the caravan cuffed the player");
	check(switchTick != 0 && cuffTick != 0 && switchTick >= cuffTick && switchTick - cuffTick <= 20 * 2, "the squad switches to the deliver package within 2 s of the cuff");
	check(peakOwned == 1, "the ownership set held the one captive");
	check(carryTick != 0, "the captive was carried");
	check(nearest <= 300.0, "the squad walked to the camp (from 700 u away)");
	check(doneTick != 0, "the package finished (the set emptied) and the ladder fell back");
	check(me.captorSid == keeperSide, "the captive is re-owned to the camp faction");
	check(std::find(keepersAll.begin(), keepersAll.end(), me.followTarget) != keepersAll.end(), "the captive follows one of the camp's keepers");
	check(me.carriedBy == 0, "the captive was put down at the hand-over");
	if (failures.empty())
	{
		std::cout << "deliver: ok\n";
		return;
	}
	for (const std::string& f : failures)
		std::cerr << "error: " << f << "\n";
	throw std::runtime_error("deliver: " + std::to_string(failures.size()) + " failure(s)");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_slaver_deliver: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		std::cout << std::unitbuf;
		for (const int idle : {0, 3, 7})
		{
			gIdle = idle;
			deliver(argv[1]);
		}
		std::cout << "ksim_slaver_deliver: ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << "\n";
		return 1;
	}
}
