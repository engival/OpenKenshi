// Victory AI stay (kenshi-h2v.22.10; docs/COMBAT_OPTIONS.md 10, FACTIONS.md 2.1b): a force that won its town (leader alive and standing there, no
// hostile fighter able to fight in it) occupies with no retreat, no deadline expiry and no walk home, past its stay and retreat (game hours), while
// a force with the option off walks home as before. Setup of ksim_raid_occupy (its Game fixture, copied; the shared rig is kenshi-k16.15.6): Crab
// Raiders in a PLAYER-owned town assault and occupy it; the test then gives the squad a raid order whose stay is over a second later.
// Needs the bake (argv[1]; exit 77).
//
// Copied from ksim_raid_occupy: Crab Raiders
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
#include "ksim/dev/build_paths.hpp"

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

	explicit Game(const std::string& bakeDir, bool victoryAi)
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
		ai.victoryAi = victoryAi;
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


struct Outcome
{
	state::OrderStatus status = state::OrderStatus::None;
	std::uint64_t until = 0;
	std::uint64_t deadline = 0;
	bool alive = false;
	bool inTown = false;
};

// The occupied town, then the raid order, then `hours` of game time past the end of the retreat.
Outcome run(const std::string& bake, bool victoryAi)
{
	Game game(bake, victoryAi);
	Simulation& sim = game.sim;
	const std::string crab = templateNamed(*game.gd, "Crab raiders patrol");
	test::require(!crab.empty(), "no SQUAD_TEMPLATE 'Crab raiders patrol'");
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
	const towncircle::Circle circle = circles[static_cast<std::size_t>(at)];
	flecs::entity squad = game.placeSquad(crab, game.townX, game.townZ);
	blackboard::Board* board = sim.blackboardOf(squad);
	test::require(board != nullptr, "the raider squad has no blackboard");
	const std::string occupySid = "96451-rebirth.mod";
	const std::uint64_t humanId = [&] { for (const CombatArena::Member& m : sim.combat()->members()) if (m.fighter.playerControlled) return m.id; return std::uint64_t(0); }();
	{
		const flecs::entity raiders(ecsOf(sim).c_ptr(), squad.get<state::SquadInstance>()->faction.id);
		sim.setWar(*raiders.get<state::Id>(), state::Id{player});
		sim.run(2);
	}
	// The force assaults and occupies the player's town (ksim_raid_occupy); the player is held down once the stay began, so no defender is left.
	bool occupied = false;
	for (int tick = 0; tick < 24000 && !occupied; ++tick)
	{
		sim.run(1);
		occupied = board->current == occupySid;
		// rwh.228 s2: the handover needs the player down, so he is held down while the assault runs (stand-in for the beating).
		if (state::Fighter* f = fighterOf(sim, humanId); f != nullptr && !f->dead && board->current != occupySid && sim.clockTick() > 3)
		{
			f->koTimer = 1.0e9;
			f->unconscious = true;
		}
	}
	test::require(occupied, "the force never occupied the player's town");
	if (state::Fighter* f = fighterOf(sim, humanId); f != nullptr && !f->dead)
	{
		f->koTimer = 1.0e9;
		f->unconscious = true;
	}
	// The raid order: on site, the stay ends in a second, then 3 game hours of retreat; the way home starts 400 u outside the circle.
	const std::uint64_t now = sim.clockTick();
	state::GroupAI group = *squad.get<state::GroupAI>();
	group.goal = state::GoalKind::Raid;
	group.since = now;
	group.route.clear();
	group.length = 0.0;
	group.along = 0.0;
	group.until = now + 20;
	group.order = state::RaidOrder{};
	group.order.status = state::OrderStatus::Staying;
	group.order.target.id = town.id();
	group.order.originX = circle.x + circle.radius + 400.0;
	group.order.originZ = circle.z;
	group.order.stay = 20;
	group.order.retreat = 3 * kTicksPerGameHour;
	group.order.deadline = now + 24 * kTicksPerGameHour;
	group.order.issued = now;
	squad.set<state::GroupAI>(std::move(group));
	// Past stay + retreat (game hours); the player is kept down.
	const std::uint64_t end = now + 20 + 3 * kTicksPerGameHour + kTicksPerGameHour;
	while (sim.clockTick() < end)
	{
		sim.run(200);
		if (state::Fighter* f = fighterOf(sim, humanId); f != nullptr && !f->dead)
			f->unconscious = true;
	}
	Outcome out;
	if (const state::GroupAI* g = squad.is_valid() ? squad.get<state::GroupAI>() : nullptr)
	{
		out.alive = true;
		out.status = g->order.status;
		out.until = g->until;
		out.deadline = g->order.deadline;
		for (const auto& [id, member] : membersOf(sim, squad))
			if (const state::Fighter* f = sim.combat()->find(id); f != nullptr && !f->dead && towncircle::at(circles, f->x, f->z) == at)
				out.inTown = true;
	}
	return out;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !std::filesystem::exists(argv[1]))
		{
			std::cout << "ksim_raid_victory_stay: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const Outcome on = run(argv[1], true);
		std::cout << "  victory AI on: status " << static_cast<int>(on.status) << ", alive " << on.alive << ", in the town " << on.inTown << "\n";
		test::require(on.alive, "the winners were despawned");
		test::require(on.status == state::OrderStatus::Retreating && on.until == ~std::uint64_t(0) && on.deadline == ~std::uint64_t(0),
			"the winners do not hold the town past stay + retreat (status " + std::to_string(static_cast<int>(on.status)) + ")");
		test::require(on.inTown, "the winners are not in the town after stay + retreat");
		const Outcome off = run(argv[1], false);
		std::cout << "  victory AI off: status " << static_cast<int>(off.status) << ", alive " << off.alive << ", in the town " << off.inTown << "\n";
		test::require(!off.alive || (off.status != state::OrderStatus::Retreating && off.until != ~std::uint64_t(0)), "with the option off the force still holds the town");
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_raid_victory_stay OK\n";
	return 0;
}
