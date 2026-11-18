// Task k16.7.12: a squad travels to a town that lies off the streamed regions. On the server's own world (the runtime mesh over the shipped
// navtiles) the wandering traders of scenarios/slavers_downed.kscen's Hub pick their 4 nearest towns, all off the mesh; the circuit used to skip
// every one of them and rest, so the caravan stood for ever. Now the pick walks to the mesh edge toward the town (the depot's edge walk,
// k16.6.21): after its stay a Hub caravan leaves. From the edge it goes on as a token (k16.7.15): straight to the town at the token speed,
// and its stay starts there. Exit 77 without the shipped navtiles.
#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/factions.hpp"
#include "ksim/navigator.hpp"
#include "ksim/server_ai.hpp"
#include "ksim/scenario_file.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "knav/native.hpp"
#include "ksim/simulation.hpp"
#include "ksim/trader.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
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
			std::cerr << "ksim_offmesh_travel: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
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
		scenario::apply(sim, *context.gameData, *data, context, file, 1);
		ServerAiOptions ai;
		ai.registryPath = KSIM_AI_TABLE;
		enableServerAi(sim, ai);
		// Per trader: its first leave from a stay, the pick it left for, how far that pick is, when it next stays (the arrival), and the worst it
		// walked back (the distance to the pick grew by this much between two looks).
		struct Walker
		{
			std::uint64_t id = 0;
			double x0 = 0.0;
			double z0 = 0.0;
			bool stayed = false;
			bool left = false;
			bool arrived = false;
			std::uint64_t leaveTick = 0;
			std::uint64_t arriveTick = 0;
			std::uint64_t pick = 0;
			double distAtLeave = 0.0;
			double lastDist = 0.0;
			double worstBack = 0.0;
			double distAtArrival = 0.0;
			double farthest = 0.0;
			bool pickOnMesh = true;
		};
		std::map<std::uint64_t, Walker> walkers;
		const auto scan = [&]()
		{
			sim.world().ecs().each([&](const state::Id& id, const state::TraderTrip& trip, const state::Position& at)
			{
				Walker& w = walkers[id.value];
				if (w.id == 0)
				{
					w.id = id.value;
					w.x0 = at.x;
					w.z0 = at.z;
				}
				w.farthest = std::max(w.farthest, std::hypot(at.x - w.x0, at.z - w.z0));
				const flecs::entity town(sim.world().ecs().c_ptr(), trip.town.id);
				const state::TownState* where = trip.town.id != 0 && town.is_valid() ? town.get<state::TownState>() : nullptr;
				const double dist = where != nullptr ? std::hypot(at.x - where->x, at.z - where->z) : 0.0;
				if (trip.staying && !w.left)
					w.stayed = true;
				if (w.stayed && !w.left && !trip.staying && where != nullptr)
				{
					w.left = true;
					w.leaveTick = sim.clockTick();
					w.pick = trip.town.id;
					w.distAtLeave = dist;
					w.lastDist = dist;
					w.pickOnMesh = sim.navigationOwner()->covers(where->x, where->z);
				} else if (w.left && !w.arrived) {
					if (trip.staying && trip.town.id == w.pick)
					{
						w.arrived = true;
						w.arriveTick = sim.clockTick();
						w.distAtArrival = dist;
					} else if (trip.town.id == w.pick) {
						w.worstBack = std::max(w.worstBack, dist - w.lastDist);
						w.lastDist = dist;
					}
				}
			});
		};
		sim.tick();
		scan();
		// 26 game hours from 11:00: the stay is U(5, 8) h, the leg is the mesh edge at the caravan's pace and then ~58 km at the token speed.
		for (int i = 0; i < 26 * 2200; ++i)
		{
			sim.tick();
			if (i % 50 == 0)
				scan();
		}
		// The Hub's caravan: the trader that began within a few hundred u of the player's start.
		const Walker* caravan = nullptr;
		for (const auto& [id, w] : walkers)
		{
			if (std::hypot(w.x0 - -50978.8, w.z0 - 2932.48) < 500.0)
				caravan = &w;
		}
		test::require(caravan != nullptr, "a wandering trader stands at the Hub");
		test::require(caravan->stayed && caravan->left, "the Hub caravan stayed and then left its town");
		test::require(!caravan->pickOnMesh, "its pick lies off the streamed mesh (the case of this test)");
		test::require(caravan->farthest > 2000.0, "the Hub caravan walked " + std::to_string(caravan->farthest) + " u toward its off-mesh town (it stood still before k16.7.12)");
		const double seconds = static_cast<double>(caravan->arriveTick - caravan->leaveTick) * kTickSeconds;
		std::cerr << "ksim_offmesh_travel: the Hub caravan " << caravan->id << " left for town " << caravan->pick << ", " << caravan->distAtLeave << " u away, and "
			<< (caravan->arrived ? "arrived after " + std::to_string(seconds) + " s" : std::string("did not arrive")) << "; worst step back " << caravan->worstBack << " u\n";
		test::require(caravan->arrived, "the caravan reached its off-mesh town and started its stay there");
		test::require(caravan->distAtArrival <= UtilityGroupLayer::kHomeRadius, "it stays inside the town's circle");
		// Within the distance at the pace of the slowest leg (the caravan's own on the mesh part, the token's beyond), and not faster than the token cap.
		test::require(seconds <= caravan->distAtLeave / trader::kWanderSpeed + 600.0, "the leg took " + std::to_string(seconds) + " s for " + std::to_string(caravan->distAtLeave) + " u");
		test::require(seconds >= (caravan->distAtLeave - UtilityGroupLayer::kHomeRadius) / UtilityGroupLayer::kTokenSpeedCap, "the leg is not faster than the token cap");
		test::require(caravan->worstBack < 100.0, "the caravan never headed back inland (worst step back " + std::to_string(caravan->worstBack) + " u)");
		std::cerr << "ksim_offmesh_travel: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
