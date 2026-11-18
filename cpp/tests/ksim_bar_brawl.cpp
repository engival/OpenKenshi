// Task k16.6.83: why a bar brawl between third parties starts after the player attacks a seated patron (playtest k16.22.2 note 3: "this
// started a brawl. Mercenaries are fighting ninjas"). The fixture is the Hub's bar nearest (-51193, 2678) with the residents the new game
// builds there. The player (a Wanderer) attacks one seated patron; the combat fight trace (CombatArena::setFightTrace) records every pick,
// so each joiner's goal, target and candidate list is printed, and the first pick between two characters that are neither the player nor
// the victim's side is reported with the goal that admitted it.
// It pins the original's participants (kenshi-k16.6.85; oracle rwh.82 / rwh.83).
//
// argv[1]: the world navmesh bake directory (knav_cache). argv[2] (optional): the faction name of the victim ("Mercenary Guild" by default: the
// nearest seated patron of that faction).

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/factions.hpp"
#include "ksim/trader.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/dialogue_book.hpp"
#include "ksim/command.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "nav_source.hpp"

#include <algorithm>
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

flecs::world& ecsOf(Simulation& sim)
{
	return const_cast<flecs::world&>(sim.world().ecs());
}

state::Fighter* fighterOf(Simulation& sim, std::uint64_t id)
{
	return const_cast<CombatArena*>(sim.combat())->find(id);
}

constexpr double kBarX = -51193.0;
constexpr double kBarZ = 2678.0;

struct Game
{
	std::string bake;
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;

	explicit Game(const std::string& bakeDir) : bake(bakeDir)
	{
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		mesh = test::openWorldNav(bake, hubZones);
	}

	void newGame()
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = 1;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->markPristine();
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(combat, settings);
		sim->enableDialogue(DialogueBook::build(sim->gameData()));
		sim->enableNavigation(mesh);
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, bake, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim->gameData());
		sim->enableAi(std::move(ai));
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

std::string factionOf(Simulation& sim, const state::Fighter& f)
{
	std::string name = "?";
	ecsOf(sim).query_builder<const state::Id, const state::SquadInstance>().build().each([&](const state::Id& id, const state::SquadInstance& instance)
	{
		if (id.value != f.squad || instance.faction.id == 0 || !ecsOf(sim).is_alive(instance.faction.id))
			return;
		const state::FactionState* faction = ecsOf(sim).entity(instance.faction.id).get<state::FactionState>();
		const kdata::Record* record = faction != nullptr ? sim.gameData()->find(faction->templateSid) : nullptr;
		if (record != nullptr)
			name = record->name;
	});
	return name;
}

std::string who(Simulation& sim, std::uint64_t id)
{
	const state::Fighter* f = sim.combat()->find(id);
	if (f == nullptr)
		return "#" + std::to_string(id) + " (gone)";
	char buffer[256];
	std::snprintf(buffer, sizeof buffer, "#%llu %s [%s] squad %llu team %d%s", static_cast<unsigned long long>(id), f->name.c_str(),
		factionOf(sim, *f).c_str(), static_cast<unsigned long long>(f->squad), f->team, sim.seated(id) ? " seated" : "");
	return buffer;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_bar_brawl: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		std::cout << std::unitbuf;
		Game game(argv[1]);
		game.newGame();
		test::require(game.player != 0, "the new game has a player character");
		Simulation& sim = *game.sim;
		sim.run(200);
		game.teleport(kBarX, kBarZ);
		// The bar squads relax at their tables from about hour 12 (ksim_town_bar): run to hour 13 with the player at the bar.
		while (std::fmod(trader::gameHours(sim.clockTick()), 24.0) < 13.0 || std::fmod(trader::gameHours(sim.clockTick()), 24.0) > 13.5)
			sim.run(kTicksPerGameHour / 12);
		sim.run(200);
		struct Row
		{
			double d;
			std::uint64_t id;
		};
		std::vector<Row> near;
		for (const CombatArena::Member& member : sim.combat()->members())
		{
			const double d = std::hypot(member.fighter.x - kBarX, member.fighter.z - kBarZ);
			if (member.id != game.player && d < 150.0)
				near.push_back({d, member.id});
		}
		std::sort(near.begin(), near.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
		std::printf("ksim_bar_brawl: %zu characters within 150 u of the bar point\n", near.size());
		for (const Row& row : near)
			std::printf("  %5.0f u  %s  bld %llu\n", row.d, who(sim, row.id).c_str(),
				static_cast<unsigned long long>(sim.combat()->find(row.id)->building));
		std::uint64_t patron = 0;
		const std::string victimFaction = argc > 2 ? argv[2] : "Mercenary Guild";
		for (const Row& row : near)
			if (sim.seated(row.id) && factionOf(sim, *sim.combat()->find(row.id)) == victimFaction)
			{
				patron = row.id;
				break;
			}
		test::require(patron != 0, "a seated patron of " + victimFaction + " stands within 150 u of the bar point");
		std::printf("ksim_bar_brawl: the victim: %s\n", who(sim, patron).c_str());
		const state::Fighter& victim = *sim.combat()->find(patron);
		game.teleport(victim.x + 20.0, victim.z);
		sim.run(10);
		double clock = 0.0;
		std::vector<std::pair<double, CombatArena::FightPick>> picks;
		const_cast<CombatArena*>(sim.combat())->setFightTrace([](std::uint64_t) { return true; },
			[&](const CombatArena::FightPick& pick) { picks.emplace_back(clock, pick); });
		const std::uint64_t attackTick = sim.clockTick();
		sim.enqueue(Command::fighterAttack(state::Id{game.player}, state::Id{patron}));
		for (int i = 0; i < 20 * 90; ++i)
		{
			sim.run(1);
			clock += 0.05;
		}
		const_cast<CombatArena*>(sim.combat())->setFightTrace({}, {});
		for (const DialoguePick& pick : sim.dialogue()->log())
		{
			if (pick.result != DialoguePick::Result::Picked || pick.event.speaker == game.player || pick.event.tick < attackTick)
				continue;
			const DialogueBook& book = sim.dialogue()->book();
			for (const ChildRef& child : book.children(book.dialogue(pick.dialogue).firstLines))
			{
				const LineDef& line = book.line(child.line);
				for (const ActDef& act : book.effects(line.effects))
					std::printf("      root line %s effect %u operand %d\n", std::string(book.sidOf(line.sid)).c_str(), act.kind, act.operand);
			}
			std::printf("  dialogue tick %llu: event %u speaker %s target %llu -> %s\n", static_cast<unsigned long long>(pick.event.tick), pick.event.event,
				who(sim, pick.event.speaker).c_str(), static_cast<unsigned long long>(pick.event.target),
				sim.dialogue()->book().dialogue(pick.dialogue).name.c_str());
		}
		std::uint64_t firstThird = 0;
		for (const auto& [at, pick] : picks)
		{
			std::printf("  t=%5.2f %s goal %u -> target %s (was goal %u)%s\n", at, who(sim, pick.member).c_str(), pick.goal,
				pick.target == 0 ? "none" : who(sim, pick.target).c_str(), pick.previousGoal, pick.order ? " ORDER" : "");
			if (firstThird == 0 && pick.member != game.player && pick.target != 0 && pick.target != game.player && !pick.order)
			{
				firstThird = pick.member;
				std::printf("ksim_bar_brawl: FIRST non-player pick: %s -> %s by goal %u; candidates:\n", who(sim, pick.member).c_str(),
					who(sim, pick.target).c_str(), pick.goal);
				for (const CombatArena::FightCandidate& c : pick.candidates)
					std::printf("      %s goal %u score %.2f\n", who(sim, c.id).c_str(), c.goal, c.score);
			}
		}
		// The original (live, rwh.82 / rwh.83): only the victim's own squad joins, by 143 PROTECT_OWN_SQUAD (32 for the victim); the bar's resident crew
		// (Trade Ninjas) answers with its home-intruder jobs: the standing player is arrested (DA_ARREST_TARGET: an attack order plus the 240 lift, the
		// "Vigilante generic assault" line) and the barman joins by 143; 34 ATTACK_TROUBLE_MAKERS never fires; no NPC ever targets another NPC (zero
		// NPC -> NPC hits); every chain drops at the KO.
		const state::Fighter* victimNow = sim.combat()->find(patron);
		test::require(victimNow != nullptr, "the victim is still there");
		const std::uint64_t victimSquad = victimNow->squad;
		std::size_t onPlayer = 0, victimSide = 0;
		for (const auto& [at, pick] : picks)
		{
			const state::Fighter* a = sim.combat()->find(pick.member);
			if (pick.target == 0 || pick.member == game.player || a == nullptr)
				continue;
			test::require(pick.target == game.player, "no NPC targets another NPC (" + who(sim, pick.member) + " -> " + who(sim, pick.target) + ", goal "
				+ std::to_string(pick.goal) + ")");
			test::require(pick.goal != 34, "goal 34 ATTACK_TROUBLE_MAKERS never fires on a patron assault (" + who(sim, pick.member) + ")");
			const bool crew = factionOf(sim, *a) == "Trade Ninjas";
			test::require(a->squad == victimSquad || crew, "only the victim's squad and the bar crew go after the player (" + who(sim, pick.member) + ")");
			++onPlayer;
			victimSide += a->squad == victimSquad ? 1 : 0;
		}
		test::require(victimSide > 0, "the victim's own squad answers the player");
		std::size_t stillFighting = 0;
		for (const CombatArena::Member& member : sim.combat()->members())
			if (!member.fighter.playerControlled && member.fighter.target != 0 && std::hypot(member.fighter.x - kBarX, member.fighter.z - kBarZ) < 300.0)
				++stillFighting;
		test::require(stillFighting == 0, "the bar is calm after the player's knock-out (" + std::to_string(stillFighting) + " still hold a target)");
		std::printf("ksim_bar_brawl: ok (%zu picks, %zu on the player, %zu by the victim's squad)\n", picks.size(), onPlayer, victimSide);
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_bar_brawl: FAILED: " << e.what() << "\n";
		return 1;
	}
}
