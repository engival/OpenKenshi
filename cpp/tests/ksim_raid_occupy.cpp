// Task raid-stay (kenshi-h2v.22.1; kinspect rwh.53 sections 2.4, 3, 4.3; docs/reference/FACTIONS.md 2.1b): the stay after a won raid. Crab Raiders
// (a squad that carries `passing town assault then occupy` and `hostile occupy town`) in a PLAYER-owned town:
//  - they assault it (Package_PassingByTownAssault), and once two members are engaged the package finishes and leads on to Package_OccupyTown;
//  - the occupiers pass 273's "the town is ours" test through the town's occupancy map, so one of them takes a downed player character
//    (273 -> 274) and throws it out of the gates, while TownState::owner never changes (rwh.53 4.3);
//  - once no living, standing member is in the town, the occupation ends 4 s later (a session that moves them out), and the occupancy map
//    forgets the faction.
// The squad is built at Full detail on the server's new game and the navmesh bake (as ksim_raid_goals). Needs the bake (argv[1]; exit 77).

#include "ksim/ai/ai.hpp"
#include "ksim/ai/blackboard.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/newgame.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/economy/trader.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/towncircle.hpp"
#include "ksim/world/world.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"
#include "live_world_rig.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
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

struct Game
{
	std::shared_ptr<kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	std::vector<towncircle::Circle> circles;
	double townX = 0.0; // the fixture town: a gate site of a town away from the Hub, no gate crew
	double townZ = 0.0;
	Simulation sim;

	explicit Game(const std::string& bakeDir)
		: gd(std::make_shared<kdata::GameData>(kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA))))
		, data(CombatData::build(*gd, KENSHI_DATA))
		, sim(Simulation::newGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA)))
	{
		sim.markPristine(); // the New Game command needs the fresh world mark (SIMLINK order 84)
		// The world data is in before the nav: the fixture sites are chosen from it, then the bake is opened for their zones (and the
		// Hub's 3x3, where the start point is picked).
		circles = live_rig::townCircles(sim, *gd);
		const GateSite* gate = nullptr;
		for (const GateSite& site : sim.world().gateSites())
			// No gate crew (`squadTemplate` empty): the town's own guards would take the raiders on before the test's.
			if (gate == nullptr && site.squadTemplate.empty() && live_rig::fixtureTownAt(circles, site.x, site.z))
				gate = &site;
		test::require(gate != nullptr, "no gate stands inside a town circle");
		townX = gate->x;
		townZ = gate->z;
		nav = test::openWorldNav(bakeDir, live_rig::hubZonesWith({{townX, townZ}}));
		sim.enableCombat(data, CombatSettings{});
		sim.enableNavigation(nav);
		// The AI system owns the doors (270's gate list) and the fight pass of the goal selector: without it the world has neither.
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = nav;
		ai.data = gd;
		sim.enableAi(std::move(ai));
		// The player (the new-game start): the detail anchor and the "a player exists" test the promotions need. Never ordered.
		live_rig::startWanderer(sim, *gd);
	}

	// The raider squad at (x, z), built at Full detail: the player character is put there (the area it carries is the input;
	// it is never ordered), and the squad spawns beside it.
	flecs::entity placeSquad(const std::string& templateSid, double x, double z)
	{
		live_rig::standPlayerAt(sim, x, z);
		flecs::entity squad = sim.placeTownSquad(templateSid, x, z);
		test::require(squad.is_valid(), "the raider squad does not spawn");
		sim.run(300); // the resident pass promotes it (and takes the player area's Full detail)
		std::size_t built = 0;
		for (const auto& [id, entity] : membersOf(sim, squad))
			built += fighterOf(sim, id) != nullptr ? 1 : 0;
		test::require(built > 1, "the placed squad did not build at Full detail");
		return squad;
	}
};


// The first SQUAD_TEMPLATE of the data with this name.
std::string templateNamed(const kdata::GameData& gd, const std::string& name)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
		if (record->name == name)
			return record->stringID;
	return {};
}

void testStay(Game& game)
{
	Simulation& sim = game.sim;
	const std::string crab = templateNamed(*game.gd, "Crab raiders patrol");
	test::require(!crab.empty(), "no SQUAD_TEMPLATE 'Crab raiders patrol'");
	// The fixture town becomes the player's: its owner is the player character's faction entity. Only the owner field is the test's input.
	const std::uint64_t player = playerOf(sim)->faction;
	flecs::entity playerFaction;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& id) { if (id.value == player && e.has<state::FactionState>()) playerFaction = e; });
	test::require(playerFaction.is_valid(), "the player has no faction entity");
	flecs::entity town;
	const std::vector<towncircle::Circle> circles = live_rig::townCircles(sim, *game.gd);
	const int at = towncircle::at(circles, game.townX, game.townZ);
	int index = 0;
	ecsOf(sim).each([&](flecs::entity e, const state::TownState&) { if (index++ == at) town = e; });
	test::require(town.is_valid(), "the fixture town is not in the world");
	{
		state::TownState state = *town.get<state::TownState>();
		state.owner.id = playerFaction.id();
		town.set<state::TownState>(std::move(state));
	}
	const flecs::entity_t owner = town.get<state::TownState>()->owner.id;
	const towncircle::Circle circle = circles[static_cast<std::size_t>(at)];

	flecs::entity squad = game.placeSquad(crab, game.townX, game.townZ);
	blackboard::Board* board = sim.blackboardOf(squad);
	test::require(board != nullptr, "the raider squad has no blackboard");
	const std::string assaultSid = "59293-rebirth.mod";
	const std::string occupySid = "96451-rebirth.mod";
	const auto members = [&] { return membersOf(sim, squad); };
	const std::uint64_t humanId = [&] { for (const CombatArena::Member& m : sim.combat()->members()) if (m.fighter.playerControlled) return m.id; return std::uint64_t(0); }();

	// The raiders and the player are enemies (the relation a real raid is aimed with); a conscious player draws the force into a fight.
	{
		const flecs::entity raiders(ecsOf(sim).c_ptr(), squad.get<state::SquadInstance>()->faction.id);
		sim.setWar(*raiders.get<state::Id>(), state::Id{player});
		sim.run(2);
	}
	// 1. assault -> occupy; the player character is knocked out once the stay began (273 wants a body). The raiders are never ordered.
	bool assaulted = false;
	bool occupied = false;
	bool threw = false;
	std::uint64_t occupiedAt = 0;
	for (int tick = 0; tick < 24000 && !(occupied && threw); ++tick)
	{
		sim.run(1);
		if (state::Fighter* f = fighterOf(sim, humanId); f != nullptr && !f->dead && occupied)
			f->unconscious = true; // held down once the stay began: nothing in this test heals or wakes it
		assaulted = assaulted || board->current == assaultSid;
		if (board->current == occupySid && !occupied)
		{
			occupied = true;
			occupiedAt = sim.clockTick();
		}
		for (const auto& [id, entity] : members())
			if (const state::Routine* routine = entity.get<state::Routine>(); occupied && routine != nullptr
				&& (routine->goal == routines::task::PickupIntrudersTown || routine->goal == routines::task::TakeIntruderOutsideTown))
				threw = true;
	}
	test::require(assaulted, "59293 passing town assault never started in the player's town");
	test::require(occupied, "the force never switched to 96451 hostile occupy town (applied: " + board->current + ")");
	test::require(threw, "no occupier took 273 / 274 on the downed player character");
	test::require(town.get<state::TownState>()->owner.id == owner, "the town's owner changed (rwh.53 4.3: it never does)");
	test::require(board->occupyTown == town.id(), "the package does not hold the player's town");
	std::cout << "  the force assaulted, switched to the occupy package after " << (sim.clockTick() - occupiedAt) << " ticks of waiting for the throw-out, "
		<< "and an occupier took the downed player character; the owner is unchanged\n";

	// 2. The end rule: nobody living and standing in the town. The raiders walk out (they are put 400 u outside the circle).
	test::require(board->current == occupySid, "the occupation ended while its members were still in the town");
	const std::uint64_t left = sim.clockTick();
	for (const auto& [id, entity] : members())
		if (state::Fighter* f = fighterOf(sim, id))
		{
			f->x = circle.x + circle.radius + 400.0;
			f->z = circle.z;
			f->clearRoute();
		}
	sim.run(1);
	std::uint64_t endedAt = 0;
	for (int tick = 0; tick < 400 && endedAt == 0; ++tick)
	{
		sim.run(1);
		if (board->current != occupySid)
			endedAt = sim.clockTick();
	}
	test::require(endedAt != 0, "the occupation did not end after the raiders left the town");
	const double seconds = static_cast<double>(endedAt - left) * kTickSeconds;
	test::require(seconds >= 4.0 - 0.1 && seconds <= 4.0 + 1.1, "the occupation ended after " + std::to_string(seconds) + " s, not 4 s");
	test::require(town.get<state::TownState>()->owner.id == owner, "the town's owner changed when the occupation ended");
	std::cout << "  the occupation ended " << seconds << " s after the last occupier left; the owner is unchanged\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !std::filesystem::exists(argv[1]))
		{
			std::cout << "ksim_raid_occupy: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		Game game(argv[1]);
		testStay(game);
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_raid_occupy OK\n";
	return 0;
}
