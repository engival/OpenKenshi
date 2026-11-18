// A move order onto walkable ground inside a crowded building is not refused (kenshi-h2v.25, the user's bar_lure capture of 2026-11-11:
// the Rebel Base "Storm House", ten clicks all no_route). Needs the world bake: argv[1] is its directory; without it the test is skipped (exit 77).

#include "ksim/combat/combat_data.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/nav/placement.hpp"

#include "kworld/loadorder.hpp"

#include "kworld/coords.hpp"

#include "test_util.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "nav_source.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_SCENARIOS_DIR
#error KSIM_SCENARIOS_DIR must be defined
#endif

namespace {

using namespace ksim;

struct Click
{
	double x, y, z;
};

// The capture's clicks (kclient-20261111-143851.events.txt), all with a height.
const Click kClicks[] = {
	{-50528, 1072, -1234}, {-50606, 1080, -1181}, {-50629, 1148, -1197}, {-50607, 1082, -1237}, {-50624, 1081, -1224},
	{-50610, 1080, -1226}, {-50608, 1080, -1225}, {-50617, 1081, -1204}, {-50582, 1077, -1259},
};
// The selected Wanderer where the capture shows it (the client draws it 8 u lower than the sim keeps it).
constexpr knav::Position kStart{-50672.9, 1084.3, -1116.5};

}

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !std::filesystem::exists(argv[1]))
			return 77;
		const kworld::ZoneCell centre = {simlink::kHubZoneX, simlink::kHubZoneZ}; // the server's own start window
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({centre.x + dx, centre.z + dz});
		}
		const std::shared_ptr<MeshNavigator> nav = test::openWorldNav(argv[1], zones);
		// The Wanderer's own positions at each click's tick (the capture's trails.tsv, kind sim): it was walking, 10 to 20 u above the clicks.
		const knav::Position starts[] = {{-50656.207, 1099.826, -1071.001}, {-50647.918, 1097.842, -1092.191}, {-50645.949, 1098.379, -1088.377},
			{-50646.391, 1098.325, -1088.759}, {-50659.590, 1095.827, -1101.981}, {-50665.766, 1094.201, -1110.085}, {-50667.121, 1093.916, -1111.706},
			{-50670.098, 1093.815, -1111.672}, {-50672.477, 1092.960, -1116.023}};
		for (std::size_t i = 0; i < std::size(kClicks); ++i)
		{
			const Route r = nav->routeFrom(starts[i], kClicks[i].x, kClicks[i].z, kClicks[i].y, false, 4.0, true, true);
			test::require(r.found, "mesh alone: click " + std::to_string(i) + " of the capture has no route from the Wanderer's trail position");
		}
		// The same clicks in a live simulation: the bar_lure scenario, the doors and the crowd in place, the Wanderer at the capture's spot.
		const scenario::File file = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/bar_lure.kscen");
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions spawn;
		spawn.seed = 7;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		sim.markPristine();
		const std::shared_ptr<const kdata::GameData> gd = sim.gameData();
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		sim.enableWeather(*gd);
		sim.newWorld();
		sim.run(1);
		simlink::ServerContext context;
		context.squadLimit = 50;
		const scenario::Placed placed = scenario::apply(sim, *gd, *data, context, file, 5);
		sim.run(3);
		const auto find = [&]() -> state::Fighter& { return *const_cast<CombatArena*>(sim.combat())->find(placed.idOf.at(1)); }; // the arena's storage moves: look it up again
		state::Fighter& wanderer = find();
		wanderer.x = kStart.x;
		wanderer.y = kStart.y;
		wanderer.z = kStart.z;
		sim.run(20);
		const state::Fighter& after = find();
		for (const Click& c : kClicks)
		{
			const Route r = sim.navigation()->routeFrom({after.x, after.y, after.z}, c.x, c.z, c.y, false, sim.combat()->data().pathRadius(after.raceSid), true, true);
			test::require(r.found, "live sim: the click (" + std::to_string(c.x) + " " + std::to_string(c.z) + ") has no route from the doorway");
		}
		std::cout << "move_no_route: the capture's nine clicks route at radius 4 on the mesh alone and in the bar_lure sim\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
