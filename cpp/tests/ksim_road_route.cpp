// The road layer of the original's pathing (kenshi-k16.7.48; PATHFINDING_STATIC.md 1.13 and 2.8; roads_static.md).
// Part 1, kworld::RoadNetwork on hand-made roads: entry and exit on the nearest segment, A* over junctions, the use test
// (legs + roadLength x factor)^2 <= straight^2, hex marker order. Part 2 (needs the install): the vanilla roads load (2109) and
// a long move between two towns takes them. Part 3 (needs the install and its navtiles): Simulation::routeRetry takes a move over
// 1000 u through the road's markers when the use test passes, and the ladder's road rung (roadNetworkRoute) walks them.

#include "ksim/combat_data.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "kworld/roads.hpp"

#include "knav/native.hpp"

#include "test_util.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
using kworld::RoadNetwork;

RoadNetwork::Road road(int a, int b, std::vector<RoadNetwork::Marker> markers)
{
	RoadNetwork::Road r;
	r.start = a;
	r.end = b;
	r.markers = std::move(markers);
	for (std::size_t i = 0; i + 1 < r.markers.size(); ++i)
		r.length += std::hypot(r.markers[i + 1].x - r.markers[i].x, r.markers[i + 1].z - r.markers[i].z);
	return r;
}

void synthetic()
{
	// Two roads meeting at junction 2: x 0..2000 along z 0, then 2000..4000 bending up to z 300 and back.
	const RoadNetwork net = RoadNetwork::fromRoads({
		road(1, 2, { {0, 0, 0}, {1000, 0, 0}, {2000, 0, 0} }),
		road(2, 3, { {2000, 0, 0}, {3000, 0, 300}, {4000, 0, 0} }),
	});
	test::require(net.roads().size() == 2, "both roads kept");
	const auto path = net.route(-100.0, 50.0, 4100.0, 50.0);
	test::require(path.size() >= 5, "a move from beside the road's start to beside its end takes the road (" + std::to_string(path.size()) + " nodes)");
	test::require(std::fabs(path.front().x - 0.0) < 1.0 && std::fabs(path.back().x - 4000.0) < 1.0, "the entry is the road's start, the exit its end");
	bool bend = false;
	for (const auto& m : path)
		bend = bend || (std::fabs(m.x - 3000.0) < 1.0 && std::fabs(m.z - 300.0) < 1.0);
	test::require(bend, "the markers of the second road are walked in order");
	// The same move backwards walks them reversed.
	const auto back = net.route(4100.0, 50.0, -100.0, 50.0);
	test::require(back.size() == path.size() && std::fabs(back.front().x - 4000.0) < 1.0, "the reverse move walks the roads reversed");
	// Entry and exit mid-road: the markers between only.
	const auto mid = net.route(1200.0, 20.0, 3500.0, 20.0);
	test::require(!mid.empty() && std::fabs(mid.front().x - 1200.0) < 1.0, "an entry mid-segment is the projected point");
	// Use test: a goal off the road's line where the legs cost more than the road saves.
	test::require(net.route(1000.0, 18000.0, 3000.0, 18000.0).empty(), "legs of 18 km each way: the road is not taken");
	// A road that is nowhere near the way: the road runs 4 km one way while the goal is 1.2 km the other.
	test::require(net.route(2000.0, 0.0, 800.0, 3000.0).empty(), "a road that leads away from the goal is not taken");
	// A move under the one-road case: entry and exit on one road, nothing between.
	const auto same = net.route(100.0, 10.0, 900.0, 10.0);
	test::require(same.size() == 2, "entry and exit on one segment: the two points");
	// Not connected: a second road with no shared junction.
	const RoadNetwork split = RoadNetwork::fromRoads({ road(1, 2, { {0, 0, 0}, {2000, 0, 0} }), road(5, 6, { {2000, 0, 100}, {4000, 0, 100} }) });
	test::require(split.route(0.0, 20.0, 4000.0, 20.0).empty(), "roads that do not connect are not a route");
	// The stuck call's factor 0.2 discounts the road more (bigger detours pass): a 1.9x detour passes at 0.2 and fails at 0.6.
	const RoadNetwork bent = RoadNetwork::fromRoads({ road(1, 2, { {0, 0, 0}, {1000, 0, 1500}, {2000, 0, 0} }) });
	test::require(bent.route(0.0, 20.0, 2000.0, 20.0, 0.6).empty(), "a road 1.8x the straight line fails the use test at factor 0.6");
	test::require(!bent.route(0.0, 20.0, 2000.0, 20.0, 0.2).empty(), "and passes it at the stuck fallback's 0.2");
}

void install()
{
	if (!std::filesystem::is_directory(std::string(KENSHI_DATA) + "/newland/leveldata/Newwworld"))
	{
		std::cerr << "ksim_road_route: part 2 skipped: no Newwworld leveldata in " << KENSHI_DATA << "\n";
		return;
	}
	const RoadNetwork net = RoadNetwork::load(KENSHI_DATA);
	test::require(net.roads().size() > 2000 && net.roads().size() <= 2109, "the vanilla roads load: " + std::to_string(net.roads().size()));
	// The Hub (-50979, 2932) to a town 6 km along the road network: some move over 1 km takes the roads.
	int taken = 0;
	for (const double dx : { 3000.0, 6000.0, -6000.0, 12000.0 })
		for (const double dz : { 0.0, 4000.0, -5000.0 })
			if (!net.route(-50979.0, 2932.0, -50979.0 + dx, 2932.0 + dz).empty())
				++taken;
	test::require(taken > 0, "no long move from the Hub takes any road");
}

void simulation()
{
	if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
	{
		std::cerr << "ksim_road_route: part 3 skipped: no shipped navtiles in " << KENSHI_DATA << "\n";
		return;
	}
	std::vector<kworld::ZoneCell> hub;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			hub.push_back({ simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz });
	const std::shared_ptr<MeshNavigator> nav = MeshNavigator::openNative(KENSHI_DATA, hub);
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
	Simulation sim = Simulation::load(KENSHI_DATA, 77);
	sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
	sim.enableCombat(data, CombatSettings{});
	sim.enableNavigation(nav);
	std::string sid;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		if (r->name == "Escaped Servant")
			sid = r->stringID;
	std::uint64_t rng = 12345;
	state::Fighter f = data->makeFighter(gd, sid, "", rng);
	const auto spot = nav->walker().snap({ -51320.0, 1567.16, 2700.0 }, 8.0);
	test::require(spot.has_value(), "the street spot is off the mesh");
	f.x = spot->x;
	f.y = spot->y;
	f.z = spot->z;
	const std::uint64_t id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;

	// A goal 1.2-2 km away on walkable ground with a plain route no longer than 1.3x the straight line, and a detour point M beside the
	// middle that the plain route does not pass (so a route through M can only come from a road).
	const knav::Position from{ spot->x, spot->y, spot->z };
	const state::Fighter& me = *sim.combat()->find(id);
	bool placed = false;
	double gx = 0.0, gz = 0.0, mx = 0.0, mz = 0.0;
	for (int a = 0; a < 16 && !placed; ++a)
		for (const double reach : { 1500.0, 1800.0, 1300.0 })
		{
			const double ang = a * 3.14159265358979 / 8.0;
			const auto goal = nav->walker().snap({ spot->x + std::cos(ang) * reach, spot->y, spot->z + std::sin(ang) * reach }, 60.0);
			if (!goal || std::hypot(goal->x - spot->x, goal->z - spot->z) < 1100.0)
				continue;
			const Route plain = nav->routeFrom(from, goal->x, goal->z, std::nullopt, true, 4.0);
			if (!plain.found || plain.points.size() < 2)
				continue;
			double len = 0.0;
			for (std::size_t i = 1; i < plain.points.size(); ++i)
				len += std::hypot(plain.points[i].x - plain.points[i - 1].x, plain.points[i].z - plain.points[i - 1].z);
			const double straight = std::hypot(goal->x - spot->x, goal->z - spot->z);
			if (len > 1.3 * straight)
				continue;
			// M beside the middle, 250 u to the side.
			const double cx = (spot->x + goal->x) / 2.0 - std::sin(ang) * 250.0, cz = (spot->z + goal->z) / 2.0 + std::cos(ang) * 250.0;
			const auto m = nav->walker().snap({ cx, spot->y, cz }, 40.0);
			if (!m || !nav->routeFrom(from, m->x, m->z, std::nullopt, true, 4.0).found || !nav->routeFrom({ m->x, m->y, m->z }, goal->x, goal->z, std::nullopt, true, 4.0).found)
				continue;
			double nearest = 1e9;
			for (const auto& p : plain.points)
				nearest = std::min(nearest, std::hypot(p.x - m->x, p.z - m->z));
			if (nearest < 60.0)
				continue;
			gx = goal->x;
			gz = goal->z;
			mx = m->x;
			mz = m->z;
			placed = true;
			break;
		}
	test::require(placed, "no walkable 1.1 km move with a free detour point in the Hub window");
	const double straight = std::hypot(gx - spot->x, gz - spot->z);

	const auto passes = [&](const Route& r)
	{
		for (const auto& p : r.points)
			if (std::hypot(p.x - mx, p.z - mz) < 12.0)
				return true;
		return false;
	};
	// Before: no road, the walk is the plain route and misses M.
	sim.setRoadNetwork(std::make_shared<const RoadNetwork>());
	const Route plain = sim.routeRetry(id, me, gx, gz, std::nullopt, true);
	test::require(plain.found && !passes(plain), "without roads the route is the plain search");
	// A road from beside the walker through M to beside the goal.
	const auto net = std::make_shared<const RoadNetwork>(RoadNetwork::fromRoads({
		road(1, 2, { { spot->x + 5.0, 0, spot->z + 5.0 }, { mx, 0, mz }, { gx - 5.0, 0, gz - 5.0 } }) }));
	sim.setRoadNetwork(net);
	sim.forgetPathRetries();
	const Route viaRoad = sim.routeRetry(id, me, gx, gz, std::nullopt, true);
	test::require(viaRoad.found && passes(viaRoad), "a move over 1000 u takes the road: the route passes the marker");
	test::require(std::hypot(viaRoad.points.back().x - gx, viaRoad.points.back().z - gz) < 2.0, "and still ends on the goal");
	// Under 1000 u the road is not asked for: the same road, a goal 600 u along it.
	const double ux = (gx - spot->x) / straight, uz = (gz - spot->z) / straight;
	const auto shortGoal = nav->walker().snap({ spot->x + ux * 600.0, spot->y, spot->z + uz * 600.0 }, 30.0);
	if (shortGoal)
	{
		sim.forgetPathRetries();
		const Route shortRoute = sim.routeRetry(id, me, shortGoal->x, shortGoal->z, std::nullopt, true);
		test::require(shortRoute.found && !passes(shortRoute), "a move under 1000 u does not take the road");
	}
	// The ladder's road rung: the call itself, at the stuck fallback's factor, walks the markers; with no roads it is not found.
	const Route rung = sim.roadNetworkRoute(me, gx, gz);
	test::require(rung.found && passes(rung), "the road-network rung walks the road's markers");
	sim.setRoadNetwork(std::make_shared<const RoadNetwork>());
	test::require(!sim.roadNetworkRoute(me, gx, gz).found, "with no roads the rung finds nothing");
}

} // namespace

int main()
{
	try
	{
		synthetic();
		install();
		simulation();
		std::cout << "ksim_road_route: the road pre-route and the road-network rung take the road when the use test passes\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_road_route: " << e.what() << "\n";
		return 1;
	}
}
