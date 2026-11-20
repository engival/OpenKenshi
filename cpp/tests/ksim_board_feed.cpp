// Task board-feed (2026-11-09): the selector feeds the G1 board's Situation from the live world (Simulation::situationOf), and the
// board's own per-package state (v94) crosses a save. Driven by LIVE world squads and no NPC order, on the raid-goals fixture:
//  - a Dust Bandit squad (faction 200) standing in an owned hostile town applies the passing-town-assault package 59293 by the ladder
//    alone (assaultTown);
//  - a wandering trader's squad with a TraderTrip that is `staying` walks the board's trader state 0 -> 1 -> 2 (atTown);
//  - a patrol staged 0.01 h from its 12.0 finish re-stamps in the straight run AND in the loaded world, where the loaded world's own
//    selector (not blackboard::tick by hand) does it. Needs the bake (argv[1]; exit 77 without it).

#include "ksim/ai/ai.hpp"
#include "ksim/ai/blackboard.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/newgame.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/save/save.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/towncircle.hpp"
#include "ksim/economy/trader.hpp"
#include "ksim/world/world.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

std::vector<std::pair<std::uint64_t, flecs::entity>> membersOf(Simulation& sim, flecs::entity squad)
{
	std::vector<std::pair<std::uint64_t, flecs::entity>> found;
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each(
		[&](flecs::entity member, const state::Id& id) { found.push_back({id.value, member}); });
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	return found;
}

// The town circles (world_tasks.cpp: every TownState, towncircle::of, in ecs order) - the selector's own myTown geometry.
std::vector<towncircle::Circle> townCircles(Simulation& sim, const kdata::GameData& gd)
{
	std::vector<towncircle::Circle> circles;
	ecsOf(sim).each([&](flecs::entity, const state::TownState& town) { circles.push_back(towncircle::of(gd, town)); });
	return circles;
}

// A building whose interior layout holds a cage piece (the site index's own source: indexBuilding -> addLayoutFurniture).
bool hasCageLayout(const kdata::GameData& gd, const routines::LayoutLibrary& layouts, const BuildingSite& site)
{
	std::string layoutName = site.interiorLayout;
	if (layoutName.empty() && !site.squadTemplate.empty())
		if (const kdata::Record* squad = gd.find(site.squadTemplate))
			layoutName = squad->stringOr("layout interior", "");
	const routines::Layout* layout = layoutName.empty() ? nullptr : layouts.find(site.buildingSid + "-" + layoutName);
	for (std::size_t i = 0; layout != nullptr && i < layout->pieces.size(); ++i)
		if (const kdata::Record* piece = gd.find(layout->pieces[i].target))
			if (routines::classifyPiece(gd, *piece) == routines::SiteKind::Cage)
				return true;
	return false;
}

struct Game
{
	std::shared_ptr<kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	std::vector<towncircle::Circle> circles;
	const BuildingSite* cage = nullptr; // the 276 fixture: a cage building in a fixture town
	const GateSite* gate = nullptr; // the 270 fixture: a gate in a fixture town
	Simulation sim;

	explicit Game(const std::string& bakeDir)
		: gd(std::make_shared<kdata::GameData>(kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA))))
		, data(CombatData::build(*gd, KENSHI_DATA))
		, sim(Simulation::newGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA)))
	{
		sim.markPristine(); // the New Game command needs the fresh world mark (SIMLINK order 84)
		// The world data is in before the nav: the fixture sites are chosen from it, then the bake is opened for their zones (and the
		// Hub's 3x3, where the start point is picked).
		circles = townCircles(sim, *gd);
		const routines::LayoutLibrary layouts = routines::LayoutLibrary::load(sim.world().dataDir());
		const auto fixtureTown = [&](double x, double z)
		{
			return towncircle::at(circles, x, z) >= 0 && std::hypot(x - kHubX, z - kHubZ) > 6000.0;
		};
		for (const BuildingSite& site : sim.world().buildingSites())
			if (cage == nullptr && fixtureTown(site.cx, site.cz) && hasCageLayout(*gd, layouts, site))
			{
				cage = &site;
				break;
			}
		for (const GateSite& site : sim.world().gateSites())
			// No gate crew (`squadTemplate` empty): a crew reopens a shut gate and removes its lock, so a locked gate of a crewed town
			// would fall open before the bash ends.
			if (gate == nullptr && site.squadTemplate.empty() && fixtureTown(site.x, site.z))
				gate = &site;
		test::require(cage != nullptr, "no building with a cage layout stands in a town");
		test::require(gate != nullptr, "no gate stands inside a town circle");
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		zones.push_back(kworld::zoneCellOf(cage->cx, cage->cz));
		zones.push_back(kworld::zoneCellOf(gate->x, gate->z));
		nav = test::openWorldNav(bakeDir, zones);
		sim.enableCombat(data, CombatSettings{});
		sim.enableNavigation(nav);
		// The AI system owns the doors (270's gate list) and the fight pass of the goal selector: without it the world has neither.
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = nav;
		ai.data = gd;
		sim.enableAi(std::move(ai));
		// The player (the new-game start): the detail anchor and the "a player exists" test the promotions need. Never ordered.
		const StartCatalog catalog = StartCatalog::build(*gd, KENSHI_DATA);
		std::string start;
		for (const StartOff& off : catalog.all())
			if (off.name.find("Wanderer") != std::string::npos)
			{
				start = off.sid;
				break;
			}
		test::require(!start.empty(), "no Wanderer start");
		std::string outcome;
		sim.addNewGameObserver(this, [&](const NewGameOutcome& result) { outcome = result.reason; });
		sim.enqueue(Command::newGame(start, "Tester", 1));
		sim.run(20);
		sim.setClockTick(static_cast<std::uint64_t>(kTicksPerGameDay * 0.5)); // noon: every goal's window is open
		test::require(playerOf(sim) != nullptr, "the new game has no player character (" + outcome + ")");
	}

	// The Dust Bandit squad at (x, z), built at Full detail: the player character is put there (the area it carries is the input;
	// it is never ordered), and the squad spawns beside it.
	flecs::entity placeSquad(double x, double z)
	{
		if (state::Fighter* player = const_cast<state::Fighter*>(playerOf(sim)))
		{
			player->x = x;
			player->z = z;
			player->y = sim.groundHeight(x, z).value_or(player->y);
			player->clearRoute();
			player->building = 0;
		}
		flecs::entity squad = sim.placeTownSquad("1302-gamedata.base", x, z);
		test::require(squad.is_valid(), "the Dust Bandit squad does not spawn");
		sim.run(300); // the resident pass promotes it (and takes the player area's Full detail)
		std::size_t built = 0;
		for (const auto& [id, entity] : membersOf(sim, squad))
			built += fighterOf(sim, id) != nullptr ? 1 : 0;
		test::require(built > 1, "the placed squad did not build at Full detail");
		return squad;
	}
};

std::string packageWithSignal(const kdata::GameData& data, std::int32_t signal)
{
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("AI_PACKAGE")))
		if (record->intOr("signal func", 0) == signal)
			return record->stringID;
	return {};
}

// Puts the squad on the package alone: the 24 h test contract / dialogue contract and the table are out of the way, as `change AI` does.
blackboard::Board& stage(Game& game, flecs::entity squad, std::int32_t signal)
{
	blackboard::Board* board = game.sim.blackboardOf(squad);
	test::require(board != nullptr, "the placed squad has no blackboard");
	const std::string sid = packageWithSignal(*game.gd, signal);
	test::require(!sid.empty(), "no AI package with the wanted signal");
	blackboard::Package replacement;
	test::require(blackboard::readPackage(*game.gd, sid, 0, replacement), "no AI package " + sid);
	board->contract = blackboard::Package{};
	board->contractExpiry = 0.0;
	blackboard::changeAI(*board, std::move(replacement));
	return *board;
}

// assaultTown: the squad stands in an owned town it is not allied to, and the ladder alone starts 59293.
void testAssaultFeed(Game& game)
{
	Simulation& sim = game.sim;
	flecs::entity squad = game.placeSquad(game.cage->cx, game.cage->cz);
	// The nearest owned town whose owner is hostile to the bandits' faction, and the squad moves into it.
	double bestX = 0.0, bestZ = 0.0, best = 1e18;
	ecsOf(sim).each([&](flecs::entity, const state::TownState& town)
	{
		const state::Id* owner = town.owner.id != 0 ? flecs::entity(ecsOf(sim).c_ptr(), town.owner.id).get<state::Id>() : nullptr;
		const double d = std::hypot(town.x - game.cage->cx, town.z - game.cage->cz);
		if (owner != nullptr && factionsHostile(*game.gd, squad.get<state::SquadInstance>()->faction.id != 0 ? flecs::entity(ecsOf(sim).c_ptr(), squad.get<state::SquadInstance>()->faction.id) : flecs::entity(), flecs::entity(ecsOf(sim).c_ptr(), town.owner.id)) && d < best)
		{
			best = d;
			bestX = town.x;
			bestZ = town.z;
		}
	});
	test::require(best < 1e17, "no town owner is hostile to the bandits");
	std::cout << "  hostile town " << best << " u from the fixture\n";
	for (const auto& [id, entity] : membersOf(sim, squad))
		if (state::Fighter* f = fighterOf(sim, id))
		{
			f->x = bestX;
			f->z = bestZ;
			f->clearRoute();
		}
	if (state::Fighter* player = const_cast<state::Fighter*>(playerOf(sim)))
	{
		player->x = bestX;
		player->z = bestZ;
	}
	blackboard::Board* board = sim.blackboardOf(squad);
	test::require(board != nullptr, "the placed squad has no blackboard");
	sim.run(120); // the selector's passes
	std::cout << "  applied in the hostile town: " << board->current << "\n";
	test::require(board->current == "59293-rebirth.mod", "assaultTown: the ladder did not start 59293 (applied: " + board->current + ")");
}

// atTown: a trader whose trip says it is staying walks the trader state 0 -> 1 -> 2.
void testTraderFeed(Game& game)
{
	Simulation& sim = game.sim;
	flecs::entity squad = game.placeSquad(game.cage->cx, game.cage->cz);
	blackboard::Board& board = stage(game, squad, blackboard::kWanderingTrader);
	board.traderState = 0;
	state::TraderTrip trip;
	trip.staying = true;
	squad.set<state::TraderTrip>(trip);
	sim.run(120);
	std::cout << "  trader state after the stay: " << board.traderState << " (applied " << board.current << ")\n";
	test::require(board.traderState == 2, "atTown: the staying trader did not reach state 2");
}

// A point of the opened zones that no town circle covers, for a squad that must not meet assaultTown.
std::pair<double, double> openGround(Game& game)
{
	for (int radius = 1000; radius <= 12000; radius += 500)
		for (int step = 0; step < 16; ++step)
		{
			const double angle = step * 3.14159265358979 / 8.0;
			const double x = kHubX + radius * std::cos(angle);
			const double z = kHubZ + radius * std::sin(angle);
			if (towncircle::at(game.circles, x, z) < 0 && game.nav != nullptr && game.sim.groundHeight(x, z).has_value())
				return {x, z};
		}
	test::require(false, "no open ground near the hub");
	return {0.0, 0.0};
}

// The loaded world, with the live systems the fixture turned on in the straight one.
Simulation loadLike(Game& game, const std::string& path)
{
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path, false, nullptr, RegionConfig{});
	loaded.setGameData(game.gd);
	loaded.enableCombat(game.data, CombatSettings{});
	loaded.enableNavigation(game.nav);
	AiSystem::Config ai;
	ai.registry = std::make_shared<BehaviourRegistry>(loaded.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	ai.navigator = game.nav;
	ai.data = game.gd;
	loaded.enableAi(std::move(ai));
	return loaded;
}

flecs::entity squadById(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity entity, const state::Id& key, const state::SquadInstance&)
	{
		if (key.value == id)
			found = entity;
	});
	return found;
}

// A patrol 0.01 h from its 12.0 finish: the straight world's selector re-stamps it, and so does the loaded world's own.
void testPatrolSave(Game& game)
{
	Simulation& sim = game.sim;
	flecs::entity squad = game.placeSquad(game.cage->cx, game.cage->cz);
	const auto [x, z] = openGround(game);
	for (const auto& [id, entity] : membersOf(sim, squad))
		if (state::Fighter* f = fighterOf(sim, id))
		{
			f->x = x;
			f->z = z;
			f->y = sim.groundHeight(x, z).value_or(f->y);
			f->clearRoute();
		}
	if (state::Fighter* player = const_cast<state::Fighter*>(playerOf(sim)))
	{
		player->x = x;
		player->z = z;
	}
	blackboard::Board& board = stage(game, squad, blackboard::kGoOutOnPatrol);
	sim.run(60);
	test::require(board.current == packageWithSignal(*game.gd, blackboard::kGoOutOnPatrol), "the staged patrol is not applied (" + board.current + ")");
	const double now = trader::gameHours(sim.clockTick());
	board.startedAt = now - (blackboard::kPatrolHours - 0.01);
	const double staged = board.startedAt;
	const std::uint64_t squadId = squad.get<state::Id>()->value;
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_board_feed.save";
	sim.saveGame(path);
	Simulation loaded = loadLike(game, path);
	std::remove(path.c_str());
	flecs::entity loadedSquad = squadById(loaded, squadId);
	test::require(loadedSquad.is_valid(), "the squad is gone after the load");
	const blackboard::Board* loadedBoard = loaded.blackboardOf(loadedSquad);
	test::require(loadedBoard != nullptr && loadedBoard->startedAt == staged, "the loaded board lost the staged patrol start");
	sim.run(200);
	loaded.run(200);
	loadedBoard = loaded.blackboardOf(squadById(loaded, squadId));
	std::cout << "  patrol start: staged " << staged << ", straight " << board.startedAt << ", loaded "
	          << (loadedBoard != nullptr ? loadedBoard->startedAt : -1.0) << "\n";
	test::require(board.startedAt > staged, "the straight world's selector did not re-stamp the finished patrol");
	test::require(loadedBoard != nullptr && loadedBoard->startedAt > staged, "the loaded world's selector did not re-stamp the finished patrol");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !std::filesystem::exists(argv[1]))
		{
			std::cout << "ksim_board_feed: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		Game game(argv[1]);
		testAssaultFeed(game);
		testTraderFeed(game);
		testPatrolSave(game);
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_board_feed OK\n";
	return 0;
}
