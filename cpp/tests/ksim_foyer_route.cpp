// The Hub bar foyer behind the counter is reachable for a 4.0 u body (kenshi-k16.8.46, kenshi-k16.8.45): on bake w21 a move_to
// (-50616, 1082, -1200) was refused with "path: out of search nodes" from both the stair landing and the floor-1 wire pose.
// argv[1]: the navworld bake directory (exit 77 without the region file).

#include "ksim/nav/navigator.hpp"

#include "kworld/coords.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace ksim;

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_foyer_route NAVWORLD");
		const kworld::ZoneCell cell = kworld::zoneCellOf(-50616.0, -1200.0);
		if (!test::worldRegionReady(std::string(argv[1]), cell.x, cell.z))
		{
			std::cout << "ksim_foyer_route: SKIPPED: needs the world navmesh bake\n";
			return 77;
		}
		std::set<std::pair<int, int>> seen;
		std::vector<kworld::ZoneCell> zones;
		for (const double z : { -1100.0, -1200.0, -1300.0 })
			for (const double x : { -50700.0, -50616.0, -50530.0 })
				for (int dz = -1; dz <= 1; ++dz)
					for (int dx = -1; dx <= 1; ++dx)
					{
						const kworld::ZoneCell c = kworld::zoneCellOf(x, z);
						if (seen.insert({ c.x + dx, c.z + dz }).second)
							zones.push_back({ c.x + dx, c.z + dz });
					}
		const auto nav = test::openWorldNav(argv[1], zones);
		struct Case { double x, y, z; };
		const Case starts[] = { { -50616.0, 1077.8, -1129.9 }, { -50553.1, 1131.4, -1275.0 } };
		int failed = 0;
		for (const double radius : { 2.6, 4.0 })
			for (const Case& s : starts)
			{
				const Route route = nav->routeFrom({ s.x, s.y, s.z }, -50616.0, -1200.0, 1082.0, false, Body{ radius }, true, true);
				std::cout << "r " << radius << " from (" << s.x << " " << s.y << " " << s.z << "): found " << route.found
					<< " points " << route.points.size() << " check '" << route.check << "'\n";
				if (radius == 4.0 && !route.found) ++failed;
			}
		test::require(failed == 0, std::to_string(failed) + " of 2 foyer routes (4.0 u body) found no route");
		std::cout << "ksim_foyer_route: OK\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_foyer_route: FAILED: " << e.what() << "\n";
		return 1;
	}
}
