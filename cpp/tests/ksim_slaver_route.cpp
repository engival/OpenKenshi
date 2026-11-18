// Task k16.6.21: the slaver caravan of scenarios/slavers_downed.kscen on the server's own world (the runtime mesh over the shipped
// navtiles, the default world, the squad AI) cuffs the player and the deliver package picks the nearest camp, a town ~58 km south: off the
// four streamed regions, so no route exists. The walk fails visibly (one task-log line with the reason, then the cooldown: not a silent
// route attempt every squad step) and the squad walks toward the depot as far as the mesh reaches (the mesh point nearest it along the line),
// holds there and says so once, the captive going along (kenshi-k16.6.21 steps 1 and 2). Exit 77 without the shipped navtiles.
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
#include "caravan_unhurt.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_SCENARIOS_DIR
#error KSIM_SCENARIOS_DIR must be defined
#endif

using namespace ksim;

int main()
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
		const scenario::File file = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/slavers_downed.kscen");
		// The scenario's ids follow the world's (the client's script named 41968 / 41969 / 41970): the squad, the player, the boss.
		const std::uint64_t firstId = sim.world().ecs().get<state::Ids>()->next;
		const std::uint64_t kSquad = firstId + 1;
		const std::uint64_t kPlayer = firstId + 2;
		const std::uint64_t kBoss = firstId + 3;
		const std::uint64_t kDepot = 4459; // the nearest camp's town id (4772 before kenshi-k16.6.33 changed the Hub's residents)
		scenario::apply(sim, *context.gameData, *data, context, file, 1);
		// This fixture pins the walk to the edge, not the aid (the kitted guards go on 58 over the follow, k16.6.287): the caravan carries no kit.
		stripMedkits(sim, kBoss, kBoss + 8);
		ServerAiOptions ai;
		ai.registryPath = KSIM_AI_TABLE;
		enableServerAi(sim, ai);
		std::vector<std::string> lines;
		sim.setTaskLog([&](const std::string& line) { lines.push_back(line); });
		// The client's script: the player attacks the boss; the caravan downs and cuffs the player.
		Command attack = Command::fighterAttack(state::Id{kPlayer}, state::Id{kBoss});
		attack.tick = sim.clockTick() + 100;
		sim.enqueue(attack);
		const double bossX0 = sim.combat()->find(kBoss)->x;
		const double bossZ0 = sim.combat()->find(kBoss)->z;
		// 18000 ticks = 8.3 game hours: the capture is done by tick ~1000, the walk to the edge (~7.5 km) by tick ~10000 at the caravan's 15 u/s, ~15000
		// at the weakened captive's pace (k16.6.22: the delivery waits for it).
		constexpr int kTicks = 18000;
		// The test is about the walk to the edge hold. A captive dying on the walk drops the delivery context (pruneOwned drops a dead slave, UNSURE vs the
		// exe, rwh.8), and then nothing holds at the edge: whether the player's wounds (a vital part pushed past its limit) kill it on the way
		// is down to the fight's rolls, so after the capture (tick 1500) its wounds are closed, no part is left below 0 health and its blood
		// is topped up each tick; its being alive at the end is asserted below.
		double worstGap = 0.0; // the captive's distance to the boss, the worst after the first minute of the walk (tick 1500 on)
		const auto count = [&](const std::string& needle)
		{
			return std::count_if(lines.begin(), lines.end(), [&](const std::string& line) { return line.find(needle) != std::string::npos; });
		};
		const std::string holdLine = "slaver deliver squad " + std::to_string(kSquad) + ": at the mesh edge toward depot town " + std::to_string(kDepot) + ", holding";
		// The hold at the edge is where the caravan stops being built (k16.7.15: it goes on as a token): the walk is checked up to it.
		struct Seen
		{
			double x = 0.0;
			double z = 0.0;
			bool dead = false;
		};
		Seen boss; // the last built state of the boss and the captive (both are gone with the demotion at the hold)
		Seen captive;
		for (int i = 0; i < kTicks && count(holdLine) == 0; ++i)
		{
			sim.tick();
			const state::Fighter* c = sim.combat()->find(kPlayer);
			for (std::uint64_t id = kBoss; id < kBoss + 8; ++id)
				keepCaravanUnhurt(sim, id);
			const state::Fighter* b = sim.combat()->find(kBoss);
			if (c == nullptr || b == nullptr)
				break; // demoted on the tick of the hold
			boss = Seen{b->x, b->z, b->dead};
			captive = Seen{c->x, c->z, c->dead};
			if (i >= 1500)
				worstGap = std::max(worstGap, std::hypot(c->x - b->x, c->z - b->z));
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
		bool depotCovered = true;
		sim.world().ecs().each([&](const state::Id& id, const state::TownState& town)
		{
			if (id.value == kDepot)
				depotCovered = sim.navigationOwner()->covers(town.x, town.z);
		});
		test::require(!depotCovered, "the depot lies off the runtime mesh (so no route can exist)");
		// The visible failure once (the cooldown keeps the walk from being re-routed every step), then the walk to the mesh edge.
		test::require(count("slaver deliver squad " + std::to_string(kSquad) + ": no route to depot town " + std::to_string(kDepot) + " (off mesh); walking to the mesh point nearest it") == 1,
			"the refused depot walk logs its reason once and walks to the mesh edge");
		// The boss (41970, the caravan's leader) is hundreds of u nearer the depot, and the captive (41969) came along.
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
		test::require(boss.x != 0.0 && captive.x != 0.0, "the boss and the captive were built on the walk");
		test::require(!captive.dead, "the captive is alive at the end of the walk (a dead one drops the delivery context)");
		const double gain = std::hypot(bossX0 - depotX, bossZ0 - depotZ) - std::hypot(boss.x - depotX, boss.z - depotZ);
		test::require(count("slaver deliver squad " + std::to_string(kSquad) + ": at the mesh edge toward depot town " + std::to_string(kDepot) + ", holding") == 1, "the squad holds at the edge, logged once");
		test::require(gain > 300.0, "the boss walked toward the depot");
		// The captive is lifted, set down once it wakes and then follows its carrier on foot (171, SLAVERY.md 5). k16.6.22: its gait is slower than the
		// caravan's (the test closes its wounds, the limbs stay weak), and it fell ~2.3 km behind and stayed there: the delivery's carrot now waits
		// for a slave on foot, so it stays with the squad all the way to the edge hold.
		test::require(std::hypot(bossX0 - depotX, bossZ0 - depotZ) - std::hypot(captive.x - depotX, captive.z - depotZ) > 300.0,
			"the captive goes along toward the depot");
		std::cerr << "ksim_slaver_route: the captive stayed within " << worstGap << " u of the boss (from tick 1500), " << std::hypot(captive.x - boss.x, captive.z - boss.z)
			<< " u at the edge hold\n";
		test::require(worstGap < 250.0, "the captive fell " + std::to_string(worstGap) + " u behind the boss on the walk (the delivery waits for a slave on foot)");
		test::require(std::hypot(captive.x - boss.x, captive.z - boss.z) < 250.0, "the captive is with the squad at the edge hold");
		// k16.7.15: from the edge the squad goes on to the depot as a token (the Fighters are gone, the squad's position moves in a straight line).
		const auto squadAt = [&]()
		{
			double x = 0.0;
			double z = 0.0;
			sim.world().ecs().each([&](const state::Id& id, const state::SquadInstance&, const state::Position& at)
			{
				if (id.value == kSquad)
				{
					x = at.x;
					z = at.z;
				}
			});
			return std::hypot(x - depotX, z - depotZ);
		};
		const double atEdge = squadAt();
		for (int i = 0; i < 3000; ++i)
			sim.tick();
		const double later = squadAt();
		std::cerr << "ksim_slaver_route: the squad is " << atEdge << " u from the depot at the hold, " << later << " u 3000 ticks later\n";
		test::require(count("slaver deliver squad " + std::to_string(kSquad) + ": at the mesh edge toward depot town " + std::to_string(kDepot) + ", holding") == 1, "the hold is logged once");
		test::require(atEdge - later > 1000.0, "the squad went on toward the depot as a token (" + std::to_string(atEdge - later) + " u)");
		test::require(sim.combat()->find(kBoss) == nullptr, "the boss is a token (no Fighter) on the leg");
		std::cerr << "ksim_slaver_route: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
