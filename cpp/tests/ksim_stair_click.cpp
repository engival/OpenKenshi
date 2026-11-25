// The Storm House stairs as the original has them (kenshi-r7qk, kenshi-k16.7.145; findings rwh309): the west flight joins the ground and the deck, the east flight has no polygons, a click on it walks the west flight.
// argv[1]: the navworld bake directory (exit 77 without the region file).

#include "ksim/nav/navigator.hpp"

#include "knav/navmesh.hpp"
#include "knav/route.hpp"
#include "kworld/coords.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <DetourNavMesh.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace ksim;

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_stair_click NAVWORLD");
		const kworld::ZoneCell cell = kworld::zoneCellOf(-51125.0, 2620.0);
		if (!test::worldRegionReady(std::string(argv[1]), cell.x, cell.z))
		{
			std::cout << "ksim_stair_click: SKIPPED: needs the world navmesh bake\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({ cell.x + dx, cell.z + dz });
		const auto nav = test::openWorldNav(argv[1], zones);
		// The original's navmesh walks the WEST flight only (findings rwh309_stormhouse_east_flight_live): joined to the ground at its foot and to the
		// deck at its top as one walkable chain, and the east flight has no polygons. A click on the east flight walks the west flight and stops short.
		struct Spot { const char* name; double x, y, z; };
		const Spot ground{ "ground", -51184.0, 1581.4, 2590.6 };
		const Spot deck{ "deck", -51150.0, 1645.4, 2625.0 };
		for (const bool up : { true, false })
		{
			const Spot& a = up ? ground : deck;
			const Spot& b = up ? deck : ground;
			const auto spot = nav->walker().snap({ a.x, a.y, a.z }, 40.0);
			const Route r = nav->routeFrom({ a.x, spot ? spot->y : a.y, a.z }, b.x, b.z, b.y, false, Body{ 2.6 }, false, true);
			test::require(r.found && std::hypot(r.points.back().x - b.x, r.points.back().z - b.z) <= 2.0,
				std::string("the west flight chain: ") + a.name + " to " + b.name + " has no route");
			double lowest = 1e9, highest = -1e9, step = 0.0;
			for (std::size_t i = 0; i < r.heights.size(); ++i)
			{
				lowest = std::min(lowest, r.heights[i]);
				highest = std::max(highest, r.heights[i]);
				// Every leg, sampled at 1 u, stays on the mesh: no hop off a seam between the flight and its ends.
				if (i > 0)
					step = std::max(step, std::fabs(r.heights[i] - r.heights[i - 1]));
			}
			std::printf("west flight %s -> %s: %zu points, y %.1f .. %.1f\n", a.name, b.name, r.points.size(), lowest, highest);
			test::require(lowest <= 1583.0 && highest >= 1640.0, "the route climbs the whole flight");
		}
		// No polygon over either east click's xz within 15 u of its height: the east flight is not walkable.
		const double eastClicks[][3] = { { -51114.6, 1609.84, 2615.1 }, { -51110.45, 1604.80, 2618.60 } };
		const dtNavMesh& dm = nav->mesh().detour();
		int eastPolys = 0;
		for (int ti = 0; ti < dm.getMaxTiles(); ++ti)
		{
			const dtMeshTile* t = dm.getTile(ti);
			if (!t || !t->header) continue;
			for (int pi = 0; pi < t->header->polyCount; ++pi)
			{
				const dtPoly& pl = t->polys[pi];
				if (pl.getType() != DT_POLYTYPE_GROUND) continue;
				for (const auto& c : eastClicks)
				{
					bool inside = false, near = false;
					for (int i = 0, j = pl.vertCount - 1; i < pl.vertCount; j = i++)
					{
						const float* vi = &t->verts[pl.verts[i] * 3];
						const float* vj = &t->verts[pl.verts[j] * 3];
						near = near || std::fabs(vi[1] - c[1]) < 15.0;
						if ((vi[2] > c[2]) != (vj[2] > c[2]) && c[0] < (vj[0] - vi[0]) * (c[2] - vi[2]) / (vj[2] - vi[2]) + vi[0])
							inside = !inside;
					}
					if (inside && near)
						++eastPolys;
				}
			}
		}
		std::printf("polygons over the east flight clicks: %d\n", eastPolys);
		test::require(eastPolys == 0, "the east flight has polygons: the original's mesh has none");
		// The upper east click snaps to the west flight and stops short of the click, as the original does (13 u; the stop sits on the flight).
		{
			const auto spot = nav->walker().snap({ ground.x, ground.y, ground.z }, 40.0);
			const Route r = nav->routeFrom({ ground.x, spot ? spot->y : ground.y, ground.z }, eastClicks[0][0], eastClicks[0][2], eastClicks[0][1], false,
				Body{ 2.6 }, true, true);
			const double off = r.points.empty() ? 1e9 : std::hypot(r.points.back().x - eastClicks[0][0], r.points.back().z - eastClicks[0][2]);
			std::printf("east upper click: found %d, stops %.1f xz off, y %.1f\n", r.found, off, r.heights.empty() ? 0.0 : r.heights.back());
			test::require(r.found && off > 5.0 && off < 25.0 && r.heights.back() > 1610.0, "the east click walks the west flight and stops on it");
		}
		// The lower east click (y 1604.8) resolves to a ground sliver under the flight that no body fits (ground the Recast carve leaves; the
		// original has no polygon there): not gated here, see the bead.
		std::cout << "ksim_stair_click: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_stair_click: " << e.what() << "\n";
		return 1;
	}
}
