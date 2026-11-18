// A move next to a wall starts its route at the character (kenshi-k16.6.352): the Storm House door wall, the pose of the user's F12
// 2026-11-17 (order seq 211: a click 11 u away on open floor gave a route whose first point lay 17 u behind the character) and the sideways
// moves along the inside of the wall that left the building. argv[1]: the navworld bake directory (exit 77 without the region file).

#include "ksim/navigator.hpp"

#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <DetourNavMesh.h>

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
					// A sideways move costs about its straight length (kenshi-k16.6.357: a start in the wall's clearance band was refit up to
					// 4 r away and the route went back and forth, 29 u for 10).
					const double direct = std::hypot(to.x - from.x, to.z - from.z);
					test::require(legLength(from, move) <= direct * 1.3 + 2.0, what + "the route is " + std::to_string(legLength(from, move)) + " u for a " + std::to_string(direct) + " u move:" + dump(move));
					for (const auto& point : move.points)
						test::require(point.z <= -1157.5, what + "a point left the building side (z " + std::to_string(point.z) + "):" + dump(move));
					++walks;
				}
		// The two poses of the bead, inside the Storm House.
		for (const double fromX : { -50631.7, -50627.8 })
		{
			const Spot from{ fromX, -1160.3 };
			const Spot to{ fromX + (fromX < -50630.0 ? 10.0 : 6.0), -1161.6 };
			const auto start = nav->walker().snap({ from.x, 1080.0, from.z }, 6.0);
			const auto end = nav->walker().snap({ to.x, 1080.0, to.z }, 6.0);
			test::require(start && end, "the bead's poses are on the mesh");
			const Route move = plan(from, start->y, to, end->y);
			test::require(move.found, "the bead's pose: a route");
			const double direct = std::hypot(to.x - from.x, to.z - from.z);
			test::require(legLength(from, move) <= direct * 1.3 + 2.0, "the bead's pose " + std::to_string(fromX) + ": " + std::to_string(legLength(from, move)) + " u for " + std::to_string(direct) + ":" + dump(move));
		}
		// A start exactly on a polygon edge (the midpoint and the ends of every edge shared with a neighbour) must not U-turn the funnel
		// (kenshi-k16.6.357): the same bound, over the edges inside the grid's area.
		int edgeStarts = 0;
		const dtNavMesh& detour = nav->mesh().detour();
		for (int t = 0; t < detour.getMaxTiles(); ++t)
		{
			const dtMeshTile* tile = detour.getTile(t);
			if (tile == nullptr || tile->header == nullptr)
				continue;
			for (int p = 0; p < tile->header->polyCount; ++p)
			{
				const dtPoly& poly = tile->polys[p];
				if (poly.getType() != DT_POLYTYPE_GROUND)
					continue;
				for (int e = 0; e < poly.vertCount; ++e)
				{
					if (poly.neis[e] == 0)
						continue;
					const float* a = &tile->verts[poly.verts[e] * 3];
					const float* b = &tile->verts[poly.verts[(e + 1) % poly.vertCount] * 3];
					for (const float along : { 0.0f, 0.5f })
					{
						const Spot from{ a[0] + (b[0] - a[0]) * along, a[2] + (b[2] - a[2]) * along };
						const double fromY = a[1] + (b[1] - a[1]) * along;
						if (from.x < -50632.0 || from.x > -50604.0 || from.z > -1160.0 || from.z < -1166.0 || std::fabs(fromY - 1080.0) > 3.0)
							continue;
						for (const double step : { -10.0, 10.0 })
						{
							const Spot to{ from.x + step, from.z - 1.3 };
							const auto end = nav->walker().snap({ to.x, 1080.0, to.z }, 6.0);
							if (!end || std::fabs(end->y - 1080.0) > 3.0)
								continue;
							const Route move = plan(from, fromY, to, end->y);
							if (!move.found)
								continue;
							const double direct = std::hypot(to.x - from.x, to.z - from.z);
							test::require(legLength(from, move) <= direct * 1.3 + 2.0, "edge start " + std::to_string(from.x) + "," + std::to_string(from.z) + " -> " + std::to_string(to.x) + "," + std::to_string(to.z) + ": " + std::to_string(legLength(from, move)) + " u for " + std::to_string(direct) + ":" + dump(move));
							++edgeStarts;
						}
					}
				}
			}
		}
		std::cout << edgeStarts << " edge-start moves\n";
		test::require(edgeStarts > 0, "no edge starts in the grid");
		test::require(walks > 40, "the grid found too few walkable moves: " + std::to_string(walks));
		std::cout << walks << " sideways moves stay inside\n";
		std::cout << "ksim_route_wall_start: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_route_wall_start: " << e.what() << "\n";
		return 1;
	}
}
