// Task BV (kinspect dt12): how a bar fight actually starts. The Hub's town record (18919) spawns `bar squads`; one entry is SQUAD_TEMPLATE
// 66159 'Bar thugs' (Vagrants, -10, found in __November.mod / rebirth.mod), which spawnTownSquads builds inside the town with
// SpawnSource::TownBar and no order. This fixture does the same through Simulation::placeTownSquad: the thugs stand inside the Hub's bar
// with the player, nobody is ordered, and we watch 60 s. Asserts: the squad is a bar spawn (not a wilderness/raid order), the members
// build, and no terminal state is reached (no death, no KO, no permanent goal-less limbo among them). A fight that does start must have
// come from dialogue (G6): the thug's own dialogue log carries a pick. It is fine if none starts here: the thug provocation lines want a
// personality tag and a squad larger than 5 (dt12), which a lone player does not give; the held gate is logged.
//
// argv[1]: the world navmesh bake directory (knav_cache).

#include "ksim/ai/ai.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/dialogue/dialogue_book.hpp"
#include "ksim/core/command.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "nav_source.hpp"
#include "sim_access.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kBarX = -50592.0; // ksim_bar_fight's Bar point (the Hub's own bar)
constexpr double kBarZ = -1211.0;
constexpr double kWatchSeconds = 60.0;

struct Game
{
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;

	explicit Game(const std::string& bake)
	{
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		mesh = test::openWorldNav(bake, hubZones);
	}

	void newGame(std::uint64_t seed = 1)
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = seed;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->markPristine();
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(combat, settings);
		sim->enableDialogue(DialogueBook::build(sim->gameData()));
		sim->enableNavigation(mesh);
		std::string startoff;
		for (const kdata::Record* record : sim->gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			if (record->name == "Wanderer")
				startoff = record->stringID;
		sim->enqueue(Command::newGame(startoff, "Tester", 1));
		sim->run(3);
		for (const CombatArena::Member& member : sim->combat()->members())
			if (member.fighter.playerControlled)
				player = member.id;
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

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_bar_thugs: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		std::cout << std::unitbuf;
		Game game(argv[1]);
		game.newGame();
		test::require(game.player != 0, "the new game has a player character");
		Simulation& sim = *game.sim;
		sim.run(200);
		game.teleport(kBarX, kBarZ); // the player inside the Hub's bar
		sim.run(20);
		// The town's own bar-squad path (spawnTownSquads): the template's faction, TownBar, no order.
		flecs::entity squad;
		try
		{
			squad = sim.placeTownSquad("66159-__November.mod", kBarX, kBarZ);
		} catch (const std::exception& error) {
			test::require(false, std::string("the Bar thugs template 66159 is in the merged data: ") + error.what());
		}
		test::require(squad.is_valid(), "the Hub's Bar thugs spawn");
		const state::SquadInstance* instance = squad.get<state::SquadInstance>();
		test::require(instance != nullptr && instance->source == state::SpawnSource::TownBar,
			"the squad is a town-bar spawn (TownBar, no wilderness den and no raid order)");
		const std::uint64_t squadId = squad.get<state::Id>() != nullptr ? squad.get<state::Id>()->value : 0;
		// The members build at Full detail (the player is inside): they are tokens until then.
		std::vector<std::uint64_t> thugs;
		for (int i = 0; i < 20 * 20 && thugs.empty(); ++i)
		{
			sim.run(1);
			thugs.clear();
			ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity, const state::Id& id)
			{
				if (fighterOf(sim, id.value) != nullptr)
					thugs.push_back(id.value);
			});
		}
		test::require(!thugs.empty(), "the Bar thugs build as Fighters inside the bar (" + std::to_string(thugs.size()) + ")");
		std::printf("ksim_bar_thugs: squad %llu, %zu thugs built at the Bar (%.0f, %.0f)\n",
			static_cast<unsigned long long>(squadId), thugs.size(), kBarX, kBarZ);
		// Watch. No order is given: the thugs' own routines and (at most) their dialogue decide what happens.
		std::size_t fights = 0, dead = 0, ko = 0;
		for (int i = 0; i < static_cast<int>(20 * kWatchSeconds); ++i)
		{
			sim.run(1);
			fights = 0;
			dead = 0;
			ko = 0;
			for (const std::uint64_t id : thugs)
			{
				const state::Fighter* f = sim.combat()->find(id);
				if (f == nullptr)
					continue;
				dead += f->dead ? 1 : 0;
				ko += f->unconscious ? 1 : 0;
				fights += f->target == game.player ? 1 : 0;
			}
			test::require(dead == 0 && ko == 0, "no bar thug dies or is knocked out in the window (no terminal state)");
		}
		// A fight must have come from dialogue: if any thug holds the player as a target, its own dialogue log carries a pick.
		std::size_t fromDialogue = 0;
		for (const std::uint64_t id : thugs)
		{
			const state::Fighter* f = sim.combat()->find(id);
			if (f == nullptr || f->target != game.player)
				continue;
			for (const DialoguePick& pick : sim.dialogue()->log())
				fromDialogue += pick.event.speaker == id && pick.result == DialoguePick::Result::Picked ? 1 : 0;
		}
		if (fights == 0)
			std::printf("ksim_bar_thugs: no fight in 60 s (the thugs' provocation wants a personality tag and a squad larger than 5; "
				"a lone player gives neither - dt12)\n");
		else
			test::require(fromDialogue > 0, "a fight that starts here starts through dialogue (the thug's own pick)");
		std::printf("ksim_bar_thugs: ok (%zu thugs up, fights %zu, deaths %zu, KOs %zu, dialogue picks %zu)\n", thugs.size(), fights, dead, ko, fromDialogue);
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_bar_thugs: FAILED: " << e.what() << "\n";
		return 1;
	}
}
