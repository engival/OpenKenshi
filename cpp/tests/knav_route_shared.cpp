// The shared route planner (knav::planRoute, kenshi-k16.6.354): the same routes planned through it directly and through MeshNavigator::routeFrom
// give identical waypoints, heights and flags: the Storm House door-wall pose of kenshi-k16.6.352, a trip through the Hub bar's door both ways,
// a click without a height, and a goal off the mesh. argv[1]: the navworld bake directory (exit 77 without the region file).

#include "ksim/nav/navigator.hpp"

#include "knav/navmesh.hpp"
#include "knav/route.hpp"
#include "kworld/coords.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace ksim;

namespace {

struct Case
{
	const char* name;
	double fromX, fromY, fromZ;
	double toX, toZ;
	std::optional<double> toY;
	bool planned;
	double radius;
	bool clickGoal;
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: knav_route_shared NAVWORLD");
		const kworld::ZoneCell cell = kworld::zoneCellOf(-50604.0, -1159.0);
		if (!test::worldRegionReady(std::string(argv[1]), cell.x, cell.z))
		{
			std::cout << "knav_route_shared: SKIPPED: needs the world navmesh bake\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({ cell.x + dx, cell.z + dz });
		const auto nav = test::openWorldNav(argv[1], zones);
		// What a client holds: the mesh, a query on it and the set cache; nothing of the simulation.
		const knav::NavQuery query(nav->mesh());
		knav::RouteSets sets;
		const Case cases[] = {
			// kenshi-k16.6.352: the user's order seq 211 beside the Storm House door wall.
			{ "wall pose", -50604.12, 1079.92, -1159.23, -50607.60, -1169.64, 1080.115, false, 2.6, true },
			{ "wall sideways", -50620.0, 1080.0, -1161.5, -50630.0, -1162.8, 1080.0, false, 2.6, true },
			// Through the Hub bar's door, out and in (the door at its default paint).
			{ "door out", -50612.7, 1081.0, -1158.0, -50642.5, -1095.3, 1088.8, false, 4.0, false },
			{ "door in", -50642.5, 1089.0, -1095.3, -50612.7, -1158.0, std::nullopt, false, 4.0, true },
			{ "door in, planned", -50660.0, 1089.0, -1120.0, -50600.0, -1190.0, std::nullopt, true, 4.0, true },
			{ "no radius, no height", -50642.5, 1089.0, -1095.3, -50585.0, -1225.0, std::nullopt, false, 0.0, false },
			{ "goal off the mesh", -50642.5, 1089.0, -1095.3, -50642.5, -1095.3 + 5000.0, std::nullopt, false, 2.6, true },
		};
		int compared = 0;
		for (const Case& c : cases)
		{
			const auto spot = nav->walker().snap({ c.fromX, c.fromY, c.fromZ }, 40.0);
			const knav::Position from{ c.fromX, spot ? spot->y : c.fromY, c.fromZ };
			const Route viaNav = nav->routeFrom(from, c.toX, c.toZ, c.toY, c.planned, Body{ c.radius }, true, c.clickGoal);
			knav::RouteRequest request;
			request.from = from;
			request.toX = c.toX;
			request.toZ = c.toZ;
			request.toY = c.toY;
			request.planned = c.planned;
			request.radius = c.radius;
			request.clickGoal = c.clickGoal;
			const knav::RoutePlan direct = knav::planRoute(nav->mesh(), query, sets, request);
			const std::string what = std::string(c.name) + ": ";
			test::require(direct.found == viaNav.found, what + "found differs");
			test::require(direct.approach == viaNav.approach && direct.reseated == viaNav.reseated, what + "flags differ");
			test::require(direct.check == viaNav.check, what + "check differs");
			test::require(direct.points.size() == viaNav.points.size(), what + "waypoint count differs");
			for (std::size_t i = 0; i < direct.points.size(); ++i)
			{
				test::require(static_cast<double>(direct.points[i][0]) == viaNav.points[i].x
					&& static_cast<double>(direct.points[i][2]) == viaNav.points[i].z
					&& static_cast<double>(direct.points[i][1]) == viaNav.heights[i], what + "waypoint " + std::to_string(i) + " differs");
			}
			std::cout << what << (direct.found ? std::to_string(direct.points.size()) + " waypoints" : std::string(direct.check)) << " identical\n";
			++compared;
		}
		test::require(compared == static_cast<int>(sizeof cases / sizeof cases[0]), "every case compared");
		std::cout << "knav_route_shared: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "knav_route_shared: " << e.what() << "\n";
		return 1;
	}
}
