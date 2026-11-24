// Task k16.6.21, k16.6.482: the slaver caravan of scenarios/slavers_downed.kscen on the server's own world (the runtime mesh over the shipped
// navtiles, the default world, the squad AI) cuffs the player and the deliver package picks the nearest camp, a town ~58 km south: off the
// four streamed regions. A squad carrying the player stays detailed (the area follows the player, k16.6.468) and walks the far goal one zone at a
// time (the exe's far move goal, rwh208; MeshNavigator::routeFarFrom through UtilityGroupLayer): no hold at the mesh edge where the token
// continuation used to stand (k16.6.21, k16.7.15), the captive going along. Exit 77 without the shipped navtiles.
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/server_ai.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "knav/native.hpp"
#include "ksim/core/simulation.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

// One run with `idle` extra ticks before the scenario is placed (a phase shift of every walk and fight after it).
// npc: no player captive; the caravan cuffs a downed bandit placed beside it (the player stands by, uncaptured, so the area is loaded the same).
static int run(int idle, bool npc)
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_slaver_route: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, {});
		SpawnOptions spawn;
		spawn.seed = 1;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		simlink::ServerContext context;
		context.gameData = sim.gameData();
		sim.setGameData(context.gameData);
		sim.enableWeather(*context.gameData);
		const std::shared_ptr<const CombatData> data = CombatData::build(*context.gameData, KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		std::vector<kworld::ZoneCell> hub;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hub.push_back({ simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz });
		// The server's default: the runtime mesh over the shipped navtiles (ksim_server without a nav argument).
		sim.enableNavigation(MeshNavigator::openNative(KENSHI_DATA, hub, {}, context.gameData.get(), NativeCacheOptions{}));
		scenario::File file = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/slavers_downed.kscen");
		if (npc)
		{
			scenario::Squad squad;
			squad.id = 3;
			squad.name = "Bandits";
			squad.faction = "200-gamedata.base";
			squad.templateSid = "1302-gamedata.base";
			file.squads.push_back(squad);
			scenario::Char victim;
			victim.id = 10;
			victim.squad = 3;
			victim.name = "Starving bandit";
			victim.templateSid = "1301-gamedata.base";
			victim.faction = "200-gamedata.base";
			victim.x = -50960.0;
			victim.y = 1533.0;
			victim.z = 2940.0;
			victim.leader = true;
			victim.unconscious = true;
			victim.incapacitated = true;
			file.chars.push_back(victim);
		}
		// The scenario's ids follow the world's (the client's script named 41968 / 41969 / 41970): the squad, the player, the boss.
		const std::uint64_t firstId = sim.world().ecs().get<state::Ids>()->next;
		const std::uint64_t kSquad = firstId + 1;
		std::uint64_t kPlayer = firstId + 2;
		std::uint64_t kBoss = firstId + 3; // the scenario's char 2 (looked up below: the NPC case's extra squad shifts the ids)
		const std::uint64_t kDepot = 4459; // the nearest camp's town id (4772 before kenshi-k16.6.33 changed the Hub's residents)
		sim.run(idle);
		const scenario::Placed placed = scenario::apply(sim, *context.gameData, *data, context, file, 1);
		kBoss = placed.idOf.at(2);
		const std::uint64_t kHuman = placed.idOf.at(1); // the player's id (the attacker, and the one the area follows)
		if (npc)
		{
			test::require(placed.idOf.count(10) != 0, "the bandit was placed");
			kPlayer = placed.idOf.at(10);
		}
		ServerAiOptions ai;
		ai.registryPath = KSIM_AI_TABLE;
		enableServerAi(sim, ai);
		std::vector<std::string> lines;
		sim.setTaskLog([&](const std::string& line) { lines.push_back(line); });
		// The client's script: the player attacks the boss; the caravan downs and cuffs the player.
		if (!npc)
		{
			Command attack = Command::fighterAttack(state::Id{kHuman}, state::Id{kBoss});
			attack.tick = sim.clockTick() + 100;
			sim.enqueue(attack);
		}
		const double bossX0 = sim.combat()->find(kBoss)->x;
		const double bossZ0 = sim.combat()->find(kBoss)->z;
		// 30000 ticks = 13.8 game hours: the capture is done by tick ~1000, the walk to the edge (~7.5 km) by tick ~10000 at the caravan's 15 u/s, ~15000
		// at the weakened captive's pace (k16.6.22: the delivery waits for it).
		// kTicks only bounds the loop (it stops at the hold line). The arrival tick is not fixed: the caravan's escorts break off to chase a wanderer within
		// 900 u (the squad holds while they are away, squadFighting), and where the wanderers are shifts with any change to the other squads' walks
		// (k16.8.54 moved it ~115 ticks past 18000: kenshi-k16.6.453).
		constexpr int kTicks = 26000; // was 40000: the boss gains 10500 u by tick ~22000 (13200 u at 26000, measured 2026-11-22); the later ticks only repeated the no-hold / far-gap checks (kenshi-k16.5.21)
		constexpr double kFarGap = 250.0; // the gap the old check allowed at any tick
		constexpr double kEdgeGain = 7500.0; // where the squad used to hold at the mesh edge (from the start)
		constexpr int kFarTicks = 600; // 30 s: a detour past an obstacle, not a captive left behind (k16.6.22 fell 2.3 km and stayed)
		// The test is about the walk to the edge hold. A captive dying on the walk drops the delivery context (pruneOwned drops a dead slave, UNSURE vs the
		// exe, rwh.8), and then nothing holds at the edge: whether the player's wounds (a vital part pushed past its limit) kill it on the way
		// is down to the fight's rolls, so after the capture (tick 1500) its wounds are closed, no part is left below 0 health and its blood
		// is topped up each tick; its being alive at the end is asserted below.
		double worstGap = 0.0; // the captive's distance to the boss, the worst after the first minute of the walk (tick 1500 on)
		const auto count = [&](const std::string& needle)
		{
			return std::count_if(lines.begin(), lines.end(), [&](const std::string& line) { return line.find(needle) != std::string::npos; });
		};
		struct Seen
		{
			double x = 0.0;
			double z = 0.0;
			bool dead = false;
		};
		int farRun = 0;
		int longestFar = 0;
		Seen boss; // the last built state of the boss and the captive
		Seen captive;
		double depotX = 0.0;
		double depotZ = 0.0;
		sim.world().ecs().each([&](const state::Id& id, const state::TownState& town)
		{
			if (id.value == kDepot)
			{
				depotX = town.x;
				depotZ = town.z;
			}
		});
		const auto toDepot = [&](double x, double z) { return std::hypot(x - depotX, z - depotZ); };
		for (int i = 0; i < kTicks; ++i)
		{
			sim.tick();
			const state::Fighter* c = sim.combat()->find(kPlayer);
			const state::Fighter* b = sim.combat()->find(kBoss);
			test::require(c != nullptr && b != nullptr, std::string(c == nullptr ? "player " : "") + (b == nullptr ? "boss " : "") + "gone: the squad with the captive stays detailed on the walk (tick " + std::to_string(i) + ")");
			boss = Seen{b->x, b->z, b->dead};
			captive = Seen{c->x, c->z, c->dead};
			if (npc && i >= 700 && i % 100 == 0)
			{
				// The player tails the caravan (set down at the boss every 5 s), so the loaded area, which follows the player (k16.6.468), goes with
				// it: the squad stays in detail, as it does with the player a captive.
				state::Fighter* tail = const_cast<state::Fighter*>(sim.combat()->find(kHuman));
				if (tail != nullptr && !tail->dead)
				{
					tail->x = b->x + 15.0;
					tail->z = b->z;
					tail->y = b->y;
					tail->clearRoute();
				}
			}
			if (npc && i >= 20 && i < 600 && c->slaveState == 0)
			{
				// The bandit starved down: held unconscious until the caravan has cuffed it.
				state::Fighter* bandit = const_cast<state::Fighter*>(c);
				bandit->unconscious = true;
				bandit->down = true;
				bandit->koLatch = true;
				bandit->koTimer = 60.0;
			}
			if (i % 4000 == 0)
				std::cerr << "ksim_slaver_route: tick " << i << " boss gain " << toDepot(bossX0, bossZ0) - toDepot(b->x, b->z) << " captive gap " << std::hypot(c->x - b->x, c->z - b->z) << "\n";
			if (i >= 1500)
			{
				const double gap = std::hypot(c->x - b->x, c->z - b->z);
				worstGap = std::max(worstGap, gap);
				// A detour round an obstacle (a captive in a pocket the boss has passed walks 400 u of route for 300 u of gap) is a transient; falling behind for good is not.
				farRun = gap > kFarGap ? farRun + 1 : 0;
				longestFar = std::max(longestFar, farRun);
			}
			state::Fighter* held = i >= 1500 ? const_cast<state::Fighter*>(c) : nullptr;
			if (held != nullptr && !held->dead)
			{
				held->wounds.clear();
				held->blood = held->maxBloodRace;
				for (state::FighterPart& part : held->parts)
					part.health = std::max(part.health, 0.0);
			}
		}

		test::require(count("slaver deliver squad " + std::to_string(kSquad) + " starts") == 1, "the caravan's deliver package starts once");
		test::require(count("depot town " + std::to_string(kDepot)) >= 1, "the finder picks town " + std::to_string(kDepot));
		// The cause: the depot is off the streamed mesh.
		test::require(!sim.navigationOwner()->covers(depotX, depotZ), "the depot lies off the runtime mesh (so no route can exist)");
		test::require(boss.x != 0.0 && captive.x != 0.0, "the boss and the captive were built on the walk");
		test::require(!captive.dead, "the captive is alive at the end of the walk (a dead one drops the delivery context)");
		// A squad carrying the player captive stays detailed (the area follows the player, k16.6.468) and walks the far goal zone by zone (k16.6.482, the
		// exe's far move goal, rwh208): no hold at the mesh edge, no token leg. The old edge hold was ~7.5 km out; the walk goes 3000 u past it.
		const double gain = toDepot(bossX0, bossZ0) - toDepot(boss.x, boss.z);
		std::cerr << "ksim_slaver_route: the boss gained " << gain << " u toward the depot; the captive stayed within " << worstGap << " u of it (from tick 1500), " << std::hypot(captive.x - boss.x, captive.z - boss.z) << " u at the end\n";
		test::require(gain > kEdgeGain + 3000.0, "the boss walked on in detail past the old mesh-edge hold (gain " + std::to_string(gain) + " u)");
		test::require(count("holding") == 0, "no hold at the mesh edge");
		test::require(longestFar <= kFarTicks, "the captive stayed more than " + std::to_string(kFarGap) + " u behind the boss for " + std::to_string(longestFar) + " ticks on the walk (worst "
			+ std::to_string(worstGap) + " u; the delivery waits for a slave on foot)");
		test::require(std::hypot(captive.x - boss.x, captive.z - boss.z) < 250.0, "the captive is with the squad at the end");
		std::cerr << "ksim_slaver_route: idle " << idle << " ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}

int main(int argc, char** argv)
{
	// Parts for ctest to run in parallel (kenshi-k16.5.21): argv[1] = the one idle value, argv[2] = "npc": the NPC-captive case (idle 0) instead.
	// No arguments: all three runs.
	const bool npcOnly = argc > 2 && std::string(argv[2]) == "npc";
	if (!npcOnly)
	{
		for (const int idle : std::vector<int>{0, 5})
		{
			if (argc > 1 && std::atoi(argv[1]) != idle)
				continue;
			if (const int status = run(idle, false); status != 0)
				return status;
		}
	}
	if (argc > 1 && !npcOnly)
		return 0;
	// A squad that cuffs an NPC walks the same leg in detail (k16.6.672): the far walk is the squad's detail state, not the captive's identity.
	return run(0, true);
}
