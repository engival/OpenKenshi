// kenshi-k16.8.54 (client k16.8.53): in order_sounds the Wanderer is ordered through the Storm House door and to the foot of its flight
// (the client's stairs_walk script: -50616 -1130, -50616 -1200, -50590 -1230). Every move_to must be accepted (the server's route is found,
// else it answers REJECTED no_route at once) and the walk must end: arrived, never abandoned in silence ("routes blocked"). The body then
// climbs: the move to the top of the flight lifts it a floor. Needs the world bake (argv[1]); without it the test is skipped (exit 77).

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
#include "ksim/dev/build_paths.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

struct Click { double x, z; };
const Click kClicks[] = {{-50616, -1130}, {-50616, -1200}, {-50590, -1230}, {-50578, -1246}};

}

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !std::filesystem::exists(argv[1]))
			return 77;
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		}
		const std::shared_ptr<MeshNavigator> nav = test::openWorldNav(argv[1], zones);
		const scenario::File file = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/order_sounds.kscen");
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
		const std::uint64_t id = placed.idOf.at(1);
		const auto find = [&]() -> const state::Fighter& { return *sim.combat()->find(id); };
		const double startY = find().y;
		double maxY = startY;
		std::uint32_t seq = 1;
		for (const Click& c : kClicks)
		{
			const state::Fighter& f = find();
			const Route route = sim.navigation()->routeFrom({f.x, f.y, f.z}, c.x, c.z, std::nullopt, false, sim.combat()->data().pathRadius(f.raceSid), true, true);
			test::require(route.found, "move_to " + std::to_string(c.x) + " " + std::to_string(c.z) + " from (" + std::to_string(f.x) + " " + std::to_string(f.y) + " "
				+ std::to_string(f.z) + ") gets no route: the server would answer REJECTED, not stay silent");
			sim.enqueue(Command::fighterMoveTo(state::Id{id}, route.points, route.heights, seq++, false, route.approach));
			const std::uint64_t blocked = sim.combat()->routesBlocked();
			int ticks = 0;
			sim.run(2);
			while (find().walking() && ticks < 6000)
			{
				sim.run(10);
				ticks += 10;
				maxY = std::max(maxY, find().y);
			}
			const state::Fighter& after = find();
			test::require(!after.walking(), "the walk to " + std::to_string(c.x) + " " + std::to_string(c.z) + " never ended");
			test::require(sim.combat()->routesBlocked() == blocked, "the walk to " + std::to_string(c.x) + " " + std::to_string(c.z) + " was abandoned (routes blocked) at ("
				+ std::to_string(after.x) + " " + std::to_string(after.y) + " " + std::to_string(after.z) + ")");
			test::require(std::hypot(after.x - c.x, after.z - c.z) < 12.0, "the walk to " + std::to_string(c.x) + " " + std::to_string(c.z) + " ended short, at ("
				+ std::to_string(after.x) + " " + std::to_string(after.z) + ")");
		}
		test::require(maxY - startY > 3.0 || find().y - startY > 3.0, "the body never climbed (y " + std::to_string(startY) + " -> max " + std::to_string(maxY) + ")");
		std::cout << "stair_silent: the four moves were routed and arrived; y " << startY << " -> " << find().y << " (max " << maxY << ")\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
