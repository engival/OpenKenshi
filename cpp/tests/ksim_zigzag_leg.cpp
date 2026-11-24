// Probe (kenshi-e9s9.15): the gate_heft open-sand leg planned on the real mesh; corners, then a walkability map round the hole near the line.
#include "ksim/nav/navigator.hpp"
#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"
#include "nav_source.hpp"
#include "test_util.hpp"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace ksim;
int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	test::require(argc >= 2, "usage: ksim_zigzag_leg NAVWORLD");
	const double sx = 66496.56, sz = -53185.93, gx = 66254.188, gz = -52593.66;
	const kworld::ZoneCell cell = kworld::zoneCellOf(sx, sz);
	if (!test::worldRegionReady(argv[1], cell.x, cell.z)) { std::printf("SKIPPED\n"); return 77; }
	std::vector<kworld::ZoneCell> zones;
	const int ring = argc > 2 ? std::atoi(argv[2]) : 1;
	for (int dz = -ring; dz <= ring; ++dz) for (int dx = -ring; dx <= ring; ++dx) zones.push_back({cell.x + dx, cell.z + dz});
	const auto nav = test::openWorldNav(argv[1], zones);
	test::require(nav != nullptr, "zones opened");
	const double starts[2][2] = {{sx, sz}, {66465.625, -53248.0}};
	for (int s = 0; s < 2; ++s)
		for (int straighten = 0; straighten < 2; ++straighten)
		{
			const Route r = nav->routeFrom({(float)starts[s][0], 970.f, (float)starts[s][1]}, gx, gz, std::nullopt, false, 4.0, straighten != 0, false);
			double len = 0, px = starts[s][0], pz = starts[s][1], off = 0;
			const double dx = gx - px, dz = gz - pz, dl = std::hypot(dx, dz);
			std::printf("start %d straighten %d found %d n %zu:", s, straighten, r.found, r.points.size());
			for (const auto& p : r.points)
			{
				len += std::hypot(p.x - px, p.z - pz); px = p.x; pz = p.z;
				off = std::max(off, std::fabs((p.x - starts[s][0]) * dz - (p.z - starts[s][1]) * dx) / dl);
				std::printf(" (%.1f %.1f)", p.x, p.z);
			}
			std::printf("\n  length %.1f straight %.1f maxoff %.1f\n", len, dl, off);
		}
	// Walkable map round the line (radius 0 / 4 clearance): '.' walkable at r4, 'o' r0 only, '#' none. Rows z, columns x, 8 u.
	const knav::NavQuery query(nav->walker().mesh());
	knav::TravelFilter t4; t4.radius = 4.0f;
	for (double z = -53260; z <= -53090; z += 8)
	{
		std::printf("z %.0f ", z);
		for (double x = 66300; x <= 66560; x += 4)
		{
			const knav::Point a{(float)x, 970.f, (float)z}, b{(float)x + 0.5f, 970.f, (float)z};
			const bool r0 = query.walkLeg(a, b, 0.0f), r4 = query.walkLeg(a, b, 4.0f, t4);
			std::putchar(r4 ? '.' : r0 ? 'o' : '#');
		}
		std::putchar('\n');
	}
	return 0;
}
