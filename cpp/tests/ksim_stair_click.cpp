// The Storm House stairs, clicked as drawn (kenshi-r7qk): a move_to with a click height on a stair step must end the route on the stair.
// argv[1]: the navworld bake directory (exit 77 without the region file).

#include "ksim/nav/navigator.hpp"

#include "knav/navmesh.hpp"
#include "knav/route.hpp"
#include "kworld/coords.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

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
		struct Click { const char* name; double x, y, z; bool known; };
		// The two clicks on the east flight (kenshi-r7qk): the bake has one flight only, rising the other way (docs/reference/MOVE_CLICK_LIVE.md: the
		// original snaps to the closest mesh point too, so the gap is the mesh's). The third is on the baked flight and always holds.
		const Click clicks[] = {
			{ "east flight upper", -51114.6, 1609.84, 2615.1, true },
			{ "east flight lower", -51110.45, 1604.80, 2618.60, true },
			{ "baked flight", -51146.64, 1598.73, 2597.08, false },
		};
		const double starts[][3] = { { -51125.5, 1631.8, 2620.7 }, { -51184.0, 1581.4, 2590.6 } };
		int misses = 0;
		for (const Click& c : clicks)
		{
			for (const auto& s : starts)
			{
				const auto spot = nav->walker().snap({ s[0], s[1], s[2] }, 40.0);
				const knav::Position from{ s[0], spot ? spot->y : s[1], s[2] };
				const Route r = nav->routeFrom(from, c.x, c.z, c.y, false, Body{ 2.6 }, true, true);
				const double dxz = r.points.empty() ? 1e9 : std::hypot(r.points.back().x - c.x, r.points.back().z - c.z);
				const double endY = r.heights.empty() ? 0.0 : r.heights.back();
				const bool onStair = r.found && dxz <= 2.0;
				std::printf("%s from y %.0f: found %d, %zu points, end off %.1f xz, y %.1f (click y %.1f)%s\n", c.name, s[1], r.found, r.points.size(), dxz,
					endY, c.y, onStair ? "" : " MISS");
				if (!onStair)
				{
					++misses;
					test::require(c.known, std::string(c.name) + ": the route does not end on the stair within 2 u");
				}
			}
		}
		// Known until the generator chain (k16.7.131 .. .145) lands: the east flight has no polygons in the bake. When it does, this fails so the
		// skip is removed and the clicks become a hard gate.
		if (misses > 0)
		{
			std::cout << "ksim_stair_click: SKIPPED: known gap, " << misses << " stair routes miss (kenshi-r7qk)\n";
			return 77;
		}
		test::require(false, "every stair click lands: drop the known flags in ksim_stair_click.cpp");
	} catch (const std::exception& e) {
		std::cerr << "ksim_stair_click: " << e.what() << "\n";
		return 1;
	}
}
