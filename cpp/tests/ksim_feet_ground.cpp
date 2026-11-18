// Feet on the ground (kenshi-k16.7.14): in `scenarios/spectate_fight.kscen` (the slope by the windmill) every character on open ground
// stands at the terrain heightmap's height (Walker::setGround), not the baked detail mesh's, which floats 0..4 u above a slope. Sampled
// every 5 game s over 60 game s; a character indoors (building != 0) or further than Walker::kGroundSnap off the terrain (a deck) is no
// open ground and is left out, but at least half of the samples must be open ground, so the test cannot pass by excluding everyone.
// Needs the world bake (argv[1], like ksim_scenario_load; exit 77 without its regions).
#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/combat.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/factions.hpp"
#include "ksim/scenario_file.hpp"
#include "ksim/server_ai.hpp"
#include "ksim/simlink.hpp"
#include "ksim/perception.hpp"
#include "ksim/simulation.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"
#include "kworld/heightmap.hpp"
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

constexpr int kRunTicks = 20 * 60; // 60 game s
constexpr int kSampleEvery = 20 * 5;
constexpr double kTolerance = 0.3; // the client draws the same heightmap: they agree to ~0.3 u
constexpr const char* kName = "spectate_fight";

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const std::string bake = argc > 1 ? argv[1] : "";
		const scenario::ScenarioInfo info = [&]
		{
			for (const scenario::ScenarioInfo& listed : scenario::listScenarios(KSIM_SCENARIOS_DIR))
				if (listed.name == kName)
					return listed;
			throw std::runtime_error(std::string("scenario not listed: ") + kName);
		}();
		const scenario::File file = scenario::parseFile(info.path);
		test::require(file.squads.size() == 2, "the file places two squads");
		for (const scenario::Squad& squad : file.squads)
			test::require(!squad.player, "no squad of the file is the player's");
		if (bake.empty())
		{
			std::cout << "ksim_feet_ground: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const kworld::ZoneCell zone = kworld::zoneCellOf(file.centreX, file.centreZ);
		const std::string region = bake + "/" + knav::regionFileName(zone.x / knav::kRegionZones, zone.z / knav::kRegionZones);
		if (test::bakeNav() && !std::filesystem::exists(region))
		{
			std::cout << "ksim_feet_ground: SKIPPED: no " << region << "\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		const std::shared_ptr<MeshNavigator> mesh = test::openWorldNav(bake, zones);
		SpawnOptions options;
		options.seed = 11;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options);
		sim.markPristine();
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combat, settings);
		sim.enableNavigation(mesh);
		// The server's own AI enablement (server_ai.hpp), with no --bake: it takes the server's navmesh. This test used to hand-build
		// the AI with a world-bake navigator while the server only enabled it under --bake, so it passed while a live server ran
		// every character goal-less (client report, 2026-11-08).
		ServerAiOptions ai;
		ai.registryPath = KSIM_AI_TABLE;
		test::require(std::string(enableServerAi(sim, ai)) == "navmesh", "the server AI took the simulation's navigator");

		simlink::ServerContext context;
		context.gameData = sim.gameData();
		context.squadLimit = 50;
		scenario::check(*sim.gameData(), file);
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *sim.gameData(), *combat, context, file, 11);
		test::require(placed.players.empty(), "the scenario places no player character");
		test::require(placed.orders == 0, "the scenario issues no order");
		test::require(placed.others.size() >= 8, "the two squads are built (" + std::to_string(placed.others.size()) + ")");

		const kworld::Heightmap terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");
		std::size_t open = 0;
		std::size_t total = 0;
		double worst = 0.0;
		for (int tick = 1; tick <= kRunTicks; ++tick)
		{
			sim.tick();
			if (tick % kSampleEvery != 0)
				continue;
			for (const CombatArena::Member& m : sim.combat()->members())
			{
				const state::Fighter& f = m.fighter;
				if (f.dead || f.building != 0 || f.carriedBy != 0)
					continue;
				++total;
				const double gap = f.y - terrain.terrainHeight(f.x, f.z);
				if (std::fabs(gap) > knav::Walker::kGroundSnap)
					continue;
				++open;
				worst = std::max(worst, std::fabs(gap));
				test::require(std::fabs(gap) <= kTolerance, "character " + std::to_string(m.id) + " at tick " + std::to_string(tick)
					+ " stands " + std::to_string(gap) + " u off the terrain");
			}
		}
		test::require(total > 0 && open * 2 >= total, "too few samples on open ground (" + std::to_string(open) + " of " + std::to_string(total) + ")");
		std::printf("feet_ground: %zu of %zu samples on open ground, worst gap %.3f u\n", open, total, worst);
		return 0;
	} catch (const std::exception& e) {
		std::printf("ksim_feet_ground: %s\n", e.what());
		return 1;
	}
}
