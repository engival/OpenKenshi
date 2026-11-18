// A move next to a wall starts its route at the character (kenshi-k16.6.352): the Storm House door wall, the pose of the user's F12
// 2026-11-17 (order seq 211: a click 11 u away on open floor gave a route whose first point lay 17 u behind the character) and the sideways
// moves along the inside of the wall that left the building. argv[1]: the navworld bake directory (exit 77 without the region file).

#include "ksim/navigator.hpp"

#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace ksim;

namespace {

constexpr double kRadius = 2.6; // the human's pathfind footprint (CombatData::pathRadius)

struct Spot { double x, z; };

double legLength(const Spot& from, const Route& route)
{
	double length = 0.0;
	double px = from.x;
	double pz = from.z;
	for (const auto& point : route.points)
	{
		length += std::hypot(point.x - px, point.z - pz);
		px = point.x;
		pz = point.z;
	}
	return length;
}

std::string dump(const Route& route)
{
	std::string text;
	for (const auto& point : route.points)
		text += " (" + std::to_string(point.x) + " " + std::to_string(point.z) + ")";
	return text;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_route_wall_start NAVWORLD");
		const kworld::ZoneCell cell = kworld::zoneCellOf(-50604.0, -1159.0);
		if (!test::worldRegionReady(std::string(argv[1]), cell.x, cell.z))
		{
			std::cout << "ksim_route_wall_start: SKIPPED: needs the world navmesh bake\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({ cell.x + dx, cell.z + dz });
		const auto nav = test::openWorldNav(argv[1], zones);
		const auto plan = [&](const Spot& from, double fromY, const Spot& to, double toY)
		{
			return nav->routeFrom({ from.x, fromY, from.z }, to.x, to.z, toY, false, Body{ kRadius }, true, true);
		};
		// The user's order seq 211: the character stood at the wire position of the sidecar, the click 11 u away on open floor.
		const Spot at{ -50604.12, -1159.23 };
		const Spot goal{ -50607.60, -1169.64 };
		const Route route = plan(at, 1079.92, goal, 1080.115);
		test::require(route.found, "seq 211: a route");
		const double straight = std::hypot(goal.x - at.x, goal.z - at.z);
		const double length = legLength(at, route);
		std::cout << "seq 211: length " << length << " straight " << straight << " points" << dump(route) << "\n";
		// The first leg starts at the character and nothing lies behind it: the click is -z of the character, so no point may lie
		// more than 3 u to the +z side of it (the start's own push off the wall is 1.4 u).
		for (const auto& point : route.points)
			test::require(point.z <= at.z + 3.0, "seq 211: a point " + std::to_string(point.z - at.z) + " u behind the character");
		test::require(length <= straight + 4.0, "seq 211: the route is " + std::to_string(length) + " u for a " + std::to_string(straight) + " u click");
		// A sideways move along the inside of the door wall (orders seq 224-229: goals 5-10 u apart, x -50615..-50632, floor 0) never
		// leaves the building: no point of the route past the door's middle plane (z -1156.5).
		int walks = 0;
		for (double z = -1160.3; z >= -1166.0; z -= 2.1)
			for (double x = -50631.7; x <= -50604.0; x += 3.9)
				for (const double step : { -10.0, -5.0, 5.0, 10.0 })
				{
					const Spot from{ x, z };
					const Spot to{ x + step, z - 1.3 };
					const auto start = nav->walker().snap({ from.x, 1080.0, from.z }, 6.0);
					const auto end = nav->walker().snap({ to.x, 1080.0, to.z }, 6.0);
					if (!start || !end || std::fabs(start->y - 1080.0) > 3.0 || std::fabs(end->y - 1080.0) > 3.0)
						continue;
					const Route move = plan(from, start->y, to, end->y);
					if (!move.found)
						continue;
					const std::string what = "move " + std::to_string(from.x) + "," + std::to_string(from.z) + " -> " + std::to_string(to.x) + "," + std::to_string(to.z) + ": ";
					// (The length is not bounded here: a start in the wall's clearance band is refit onto fitting ground, which can still send the
					// route back and forth inside the room, a separate matter from the trip outside.)
					for (const auto& point : move.points)
						test::require(point.z <= -1157.5, what + "a point left the building side (z " + std::to_string(point.z) + "):" + dump(move));
					++walks;
				}
		test::require(walks > 40, "the grid found too few walkable moves: " + std::to_string(walks));
		std::cout << walks << " sideways moves stay inside\n";
		std::cout << "ksim_route_wall_start: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_route_wall_start: " << e.what() << "\n";
		return 1;
	}
}
