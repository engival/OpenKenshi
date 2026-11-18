// A player stuck "attacking" after a KO (client slaver re-test on 057ec506: the Wanderer attacks the boss, is KO'd, wakes and reads
// flags 0x41241 - combat bit set, goal FOCUSED_MELEE_ATTACK "Attacking target", target 0 - for minutes). The wire's combat bit and the
// goal row both come from `target != 0 || technique >= 0 || state Attack / Defend`. A block's technique pointer outlives its clip
// through the stagger (CombatArena::resolveIncoming), and a KO that lands while the body is already in state Stumble skipped the
// incapacitated branch's stopTechnique, so the pointer survived the wake. The original clears the goal of a KO'd character
// (inspect_goal.md), so the incapacitated branch drops the technique whatever the state was.
//
// argv[1]: the Hub navmesh cache (ctest fixture knav_cache).

#include "ksim/clock.hpp"
#include "ksim/combat.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

state::Fighter* fighterOf(Simulation& sim, std::uint64_t id)
{
	return const_cast<CombatArena*>(sim.combat())->find(id);
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_ko_stale_attack HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		test::require(nav != nullptr, "the Hub mesh did not load");

		Simulation sim = Simulation::load(KENSHI_DATA, 91);
		sim.setGameData(gd);
		simlink::ServerContext context;
		context.gameData = gd;
		context.dataHash = simlink::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		const simlink::Duel duel = simlink::spawnDuel(sim, *gd, *data, context);

		// The player is staggering with a block's technique pointer when the KO lands: state Stumble, technique set, a KO timer.
		state::Fighter* player = fighterOf(sim, duel.player);
		test::require(player != nullptr && !data->techniques.empty(), "no player / no techniques");
		player->state = state::CombatState::Stumble;
		player->technique = 0;
		player->techniqueFinished = true;
		player->koTimer = 3.0;
		player->unconscious = true;

		bool woke = false;
		for (int tick = 1; tick <= 20000; ++tick) // 5 sim minutes
		{
			sim.tick();
			player = fighterOf(sim, duel.player);
			if (player->unconscious)
			{
				test::require(player->technique < 0, "a KO'd body kept its technique at tick " + std::to_string(tick));
			} else if (!woke) {
				woke = true;
			}
			if (woke && tick > 4000)
				break;
		}
		test::require(woke, "the player never woke");
		const bool inFight = player->target != 0 || player->technique >= 0 || player->state == state::CombatState::Attack
			|| player->state == state::CombatState::Defend;
		test::require(!inFight, "the woken player still reads in combat with no target (technique " + std::to_string(player->technique)
			+ ", state " + std::to_string(static_cast<int>(player->state)) + ")");

		std::cout << "ksim_ko_stale_attack: OK\n";
		return 0;
	} catch (const std::exception& e) {
		std::cout << "ksim_ko_stale_attack FAILED: " << e.what() << "\n";
		return 1;
	}
}
