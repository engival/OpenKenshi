// kenshi-k16.6.103 (playtest kenshi-h2v.3.1 / h2v.3.3): a patrol placed in the Hub, a town, runs the town scorer's routine goals (Relaxing
// 214 walks), which offer none of the package's fight goals. In the original an URGENT goal whose finder returns a target (16 on a sensed
// hostile) beats any routine goal at the next selectGoal (s73_static BQ1), and being targeted writes the attacker into the victim squad
// as a personal hostile (rwh82 section 4, AI::onTargetedBy). The fight-goal pass (world_tasks.cpp) skipped a member that held a Routine,
// so a Relaxing patrol never took a target while it walked: the raiders strolled past the player for 20 s and only a landed swing woke them.
//
//  1. bar_lure_hostile (Blood Raiders, always hostile, the default world's Hub, nobody ordered): within 8 s a raider takes goal 16 on the
//     player, and every raider that has a target reports that fight's goal on the wire (CHAR_STATS goalEnum), not the routine's 214;
//  2. bar_lure (Dust Bandits, neutral to the player): the player is ordered to attack one and is still far from any swing: the squad has
//     engaged the player within 6 s of the order (the attacker is written into the victim squad), with no damage dealt.
// argv[1]: the world bake (exit 77 without the Hub's region).

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/factions.hpp"
#include "ksim/scenario_file.hpp"
#include "ksim/scenario_run.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"
#include "kdata/gamedata.hpp"
#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nav_source.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_SCENARIOS_DIR
#error KSIM_SCENARIOS_DIR must be defined
#endif

namespace {

using namespace ksim;

struct Built
{
	std::unique_ptr<Simulation> sim;
	std::shared_ptr<const CombatData> combat;
};

// The server's default world (ksim_server without --no-world): the new game's towns and residents, combat, navigation, the AI.
Built openWorld(const std::string& bake)
{
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			zones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
	SpawnOptions options;
	options.seed = 7;
	std::unique_ptr<Simulation> sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
	sim->markPristine();
	std::shared_ptr<const CombatData> combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
	CombatSettings settings;
	settings.substeps = 2;
	sim->enableCombat(combat, settings);
	sim->enableNavigation(test::openWorldNav(bake, zones));
	AiSystem::Config ai;
	ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	ai.navigator = KnavNavigator::open(KENSHI_DATA, bake, 4, 7, 6, 9, 4);
	ai.factions = FactionBook::build(sim->gameData());
	sim->enableAi(std::move(ai));
	return {std::move(sim), std::move(combat)};
}

// Loads the scenario on the pristine world (SCENARIO_LOAD: newWorld, then the file).
scenario::Placed load(Built& world, const scenario::File& file)
{
	Simulation& sim = *world.sim;
	simlink::ServerContext context;
	context.gameData = sim.gameData();
	context.squadLimit = 50;
	sim.newWorld();
	sim.run(1);
	scenario::check(*sim.gameData(), file);
	return scenario::apply(sim, *sim.gameData(), *world.combat, context, file, 100);
}

const scenario::ScenarioInfo& infoOf(const std::vector<scenario::ScenarioInfo>& infos, const char* name)
{
	const auto it = std::find_if(infos.begin(), infos.end(), [&](const scenario::ScenarioInfo& info) { return info.name == name; });
	test::require(it != infos.end(), std::string("no scenario ") + name);
	return *it;
}

bool isPlayer(const scenario::Placed& placed, std::uint64_t id)
{
	return std::find(placed.players.begin(), placed.players.end(), id) != placed.players.end();
}

} // namespace

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	try
	{
		const std::string bake = argc > 1 ? argv[1] : "";
		const std::vector<scenario::ScenarioInfo> infos = scenario::listScenarios(KSIM_SCENARIOS_DIR);
		const scenario::File hostileFile = scenario::parseFile(infoOf(infos, "bar_lure_hostile").path);
		const scenario::File lureFile = scenario::parseFile(infoOf(infos, "bar_lure").path);
		const kworld::ZoneCell zone = kworld::zoneCellOf(hostileFile.centreX, hostileFile.centreZ);
		if (bake.empty() || !test::worldRegionReady(bake, zone.x, zone.z))
		{
			std::cout << "ksim_town_aggro: SKIPPED: needs the world navmesh bake\n";
			return 77;
		}
		Built world = openWorld(bake);
		Simulation& sim = *world.sim;

		// 1. On sight.
		scenario::Placed placed = load(world, hostileFile);
		std::set<std::uint64_t> raiders;
		for (const CombatArena::Member& member : sim.combat()->members())
			if (member.id > *std::max_element(placed.players.begin(), placed.players.end()))
				raiders.insert(member.id);
		test::require(raiders.size() == 11, "the scenario places eleven raiders (got " + std::to_string(raiders.size()) + ")");
		sim.run(8 * 20);
		std::size_t onPlayer = 0;
		std::size_t routineLabel = 0;
		for (const std::uint64_t id : raiders)
		{
			const state::Fighter& f = *sim.combat()->find(id);
			const Simulation::CharStats stats = sim.charStats(id);
			if (f.target != 0 && isPlayer(placed, f.target) && f.combatGoal == routines::task::AttackEnemies)
				++onPlayer;
			if (f.target != 0 && stats.goalEnum != f.combatGoal)
				++routineLabel;
		}
		std::printf("on sight: %zu of 11 raiders on the player under goal 16 after 8 s, %zu fighting under another label\n", onPlayer, routineLabel);
		test::require(onPlayer >= 1, "bar_lure_hostile: no raider took goal 16 on the visible player within 8 s");
		test::require(routineLabel == 0, "a fighting raider reports its routine goal instead of the fight's");

		// 2. Being targeted.
		placed = load(world, lureFile);
		const state::Fighter& me = *sim.combat()->find(placed.players.front());
		std::uint64_t nearest = 0;
		double nearestD = 0.0;
		std::set<std::uint64_t> crew;
		for (const CombatArena::Member& member : sim.combat()->members())
		{
			if (isPlayer(placed, member.id) || member.id < placed.players.front())
				continue;
			crew.insert(member.id);
			const double d = std::hypot(member.fighter.x - me.x, member.fighter.z - me.z);
			if (nearest == 0 || d < nearestD)
			{
				nearest = member.id;
				nearestD = d;
			}
		}
		test::require(!crew.empty(), "bar_lure places a patrol");
		sim.run(5 * 20); // settled: nobody fights the player
		for (const std::uint64_t id : crew)
			test::require(sim.combat()->find(id)->target == 0, "bar_lure: a bandit targets the player before the player does anything");
		bool victimHit = false;
		const_cast<CombatArena*>(sim.combat())->setObserver([&](const CombatEvent& e)
		{
			if (e.kind == CombatEvent::Kind::Hit && e.who == nearest)
				victimHit = true;
		});
		sim.enqueue(Command::fighterAttack(state::Id{placed.players.front()}, state::Id{nearest}));
		// k16.6.135 (rwh.108): a swing starts when the target's 1.2 s point is in reach, so the player and a bandit running at each other are in melee
		// within a second. The squad's reaction is sampled at the first moment the order could have been answered by the fight itself: the tick the
		// player's first technique starts or the gap to its target falls below 60 u, whichever is first (6 s at the latest), and before any hit landed
		// on the targeted bandit - so engaged >= 1 comes from the targeting order and not from the first landed swing.
		std::size_t engaged = 0;
		double gap = 0.0;
		bool sampled = false;
		for (int tick = 0; tick < 6 * 20 && !sampled; ++tick)
		{
			sim.tick();
			const state::Fighter& player = *sim.combat()->find(placed.players.front());
			const state::Fighter& victim = *sim.combat()->find(nearest);
			gap = std::hypot(player.x - victim.x, player.z - victim.z);
			sampled = player.technique >= 0 || gap < 60.0 || tick == 6 * 20 - 1;
		}
		test::require(!victimHit, "bar_lure: a hit landed on the targeted bandit before the sample (the sample is too late to prove the order's effect)");
		for (const std::uint64_t id : crew)
		{
			const state::Fighter& f = *sim.combat()->find(id);
			if (f.target != 0 && isPlayer(placed, f.target))
				++engaged;
		}
		std::printf("targeted: %zu of %zu bandits on the player at the first swing / gap < 60 u (gap %.0f u)\n", engaged, crew.size(), gap);
		test::require(engaged >= 1, "bar_lure: the targeted bandit's squad did not engage before the first swing");
		std::cout << "ksim_town_aggro: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::printf("ksim_town_aggro: %s\n", e.what());
		return 1;
	}
}
