// The user's Heft Longhouse route (2026-11-15, kenshi-k16.6.247, route sidecar kclient-20261114-235725 click seq 28, route v7): character 42555 on
// floor 0 at (68338.93, 1174.30, -71117.49) to (68330.125, 1174.300, -71207.391), 5 waypoints, 134 u against 90 u straight, interior corners
// 8.8 / 8.1 / 8.0 / 5.4 u off the line through their neighbours. Replayed at the Wanderer's radius on the runtime mesh (or the bake with
// KSIM_NAVWORLD_BAKE=1): prints the waypoints and each interior corner's offset from the line through its neighbours, and fails when a corner
// is one a straight walk at r + kCornerPush clears; the length is printed (the straight line is not clear: a wall stands across it, 1->5 does not walk even at r).
// Labels: [grounded rwh198] the length is within the original's walked / straight band for this click (130.3-144.6 u, 1.44-1.63x of 90.33 u,
// findings/rwh198_route_waypoints_live.md); [pin] the removable-waypoint check is our planner against itself (the original keeps no waypoint list).
// Needs the world mesh only; without the Longhouse zone's region file the test is skipped (exit 77).

#include "ksim/nav/navigator.hpp"

#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <vector>

namespace {

using namespace ksim;

constexpr double kBody = 4.0;   // the Wanderer's pathRadius
constexpr double kRatioLo = 1.30, kRatioHi = 1.63 + 0.04; // rwh198: forward 1.60-1.63x, reverse 1.44x (walked arcs include overshoot, so a planned corridor may be ~10% shorter)
constexpr double kMargin = 5.0; // navmesh.cpp kCornerPush

double planar(double ax, double az, double bx, double bz) { return std::hypot(ax - bx, az - bz); }

} // namespace

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	test::require(argc == 2, "usage: ksim_route_longhouse NAVWORLD");
	const double sx = 68338.93, sy = 1174.30, sz = -71117.49;
	const double gx = 68330.125, gy = 1174.300, gz = -71207.391;
	const kworld::ZoneCell cell = kworld::zoneCellOf(sx, sz);
	if (!test::worldRegionReady(argv[1], cell.x, cell.z))
	{
		std::printf("ksim_route_longhouse: SKIPPED: needs the world navmesh\n");
		return 77;
	}
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			zones.push_back({ cell.x + dx, cell.z + dz });
	const auto nav = test::openWorldNav(argv[1], zones);
	test::require(nav != nullptr, "the Longhouse zone opened");
	const knav::NavQuery query(nav->walker().mesh());
	const Route route = nav->routeFrom({ sx, sy, sz }, gx, gz, std::optional<double>(gy), false, kBody);
	test::require(route.found, "the Longhouse route is found");
	knav::TravelFilter travel;
	travel.radius = static_cast<float>(kBody);
	double length = 0.0;
	int removable = 0;
	std::printf("Longhouse route: %zu waypoints\n", route.points.size());
	for (std::size_t i = 0; i < route.points.size(); ++i)
	{
		if (i)
			length += planar(route.points[i - 1].x, route.points[i - 1].z, route.points[i].x, route.points[i].z);
		std::printf("  %zu (%.2f %.2f %.2f)", i, route.points[i].x, route.heights[i], route.points[i].z);
		if (i && i + 1 < route.points.size())
		{
			const auto& a = route.points[i - 1];
			const auto& p = route.points[i];
			const auto& b = route.points[i + 1];
			const double span = planar(a.x, a.z, b.x, b.z);
			const double off = std::fabs((b.x - a.x) * (a.z - p.z) - (a.x - p.x) * (b.z - a.z)) / std::max(span, 1e-6);
			const knav::Point ka{ static_cast<float>(a.x), static_cast<float>(route.heights[i - 1]), static_cast<float>(a.z) };
			const knav::Point kb{ static_cast<float>(b.x), static_cast<float>(route.heights[i + 1]), static_cast<float>(b.z) };
			const bool clear = query.walkLeg(ka, kb, travel.radius + kMargin, travel);
			removable += clear ? 1 : 0;
			std::printf("  off the line %.1f u%s", off, clear ? "  REMOVABLE" : "");
		}
		std::printf("\n");
	}
	for (std::size_t i = 0; i < route.points.size(); ++i)
		for (std::size_t j = i + 3; j < route.points.size(); ++j)
		{
			const knav::Point ka{ static_cast<float>(route.points[i].x), static_cast<float>(route.heights[i]), static_cast<float>(route.points[i].z) };
			const knav::Point kb{ static_cast<float>(route.points[j].x), static_cast<float>(route.heights[j]), static_cast<float>(route.points[j].z) };
			const bool clear = query.walkLeg(ka, kb, travel.radius + kMargin, travel);
			removable += clear ? 1 : 0;
			if (clear)
				std::printf("  REMOVABLE: %zu walks to %zu at r + push\n", i, j);
		}
	const double straight = planar(sx, sz, gx, gz);
	std::printf("Longhouse route: %.1f u against %.1f straight, %d removable corners\n", length, straight, removable);
	const double ratio = length / straight; // (the route ends on the click)
	std::printf("  [grounded rwh198] ratio %.2fx, original band %.2f-%.2f\n", ratio, kRatioLo, kRatioHi);
	test::require(ratio >= kRatioLo && ratio <= kRatioHi, "[grounded rwh198] walked / straight within the original's walked / straight band (1.44-1.63x of the walked arcs, margin below for overshoot and above)");
	test::require(removable == 0, "[pin] no waypoint (or run of them) that a straight walk at r + kCornerPush clears");
	return 0;
}
