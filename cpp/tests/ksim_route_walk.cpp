// Walks every route ksim_route_soak plans (same sample points, radius 4) with the sim's walker, leg by leg (kenshi-k16.6.298): a leg
// the walker cannot walk is a regression of the planner. A leg STALLS when kStillSteps steps in a row move it less than kStillMove, or
// it is not done within its step budget; a walk that ends on another floor than the leg's end (kFloorTol) LEAVES the floor. Each stall is
// reported with its ends, where it stopped, whether NavQuery::walkLeg at the body radius (4 u hop) also refuses the leg, and a cause
// (door: within kDoorNear of a door on the route; height: the leg's ends differ by > kHeightCause: stair, ramp, ledge; flat: a wall, prop or
// mesh edge). The stall count is pinned (kPinnedStalls): it must not rise. argv[1]: the world bake, like ksim_route_soak. Exit 77 without it.

#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"

#include "DetourNavMesh.h"

#include "knav/navmesh.hpp"
#include "knav/walk.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

#include "nav_source.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr double kBody = 4.0; // the soak's radius
constexpr double kStepLen = 2.0; // u per step (a walker at 70 u/s on a 20 Hz tick steps 3.5 u: a bit finer here)
constexpr double kStillMove = 0.1; // a step that moves less is "no progress"
constexpr int kStillSteps = 8; // N: no-progress steps in a row that make a stall
constexpr double kArrive = 1.0; // the sim's own reach of a route point (combat.cpp kArriveReach); a looser one ends a leg on the wrong side of a narrow portal (R103 leg 28, k16.8.37)
constexpr double kFloorTol = 12.0;
constexpr double kDoorNear = 14.0;
constexpr double kHeightCause = 2.0;
constexpr int kPinnedStalls = 0; // 3 before k16.8.37 (2 stalls + 1 off-floor leg of 6,885): a portal corner on a T-junction edge was 16 u off the floor; the test's own reach was looser than the sim's

struct Spot
{
	double x = 0.0, y = 0.0, z = 0.0;
};

// As ksim_route_soak: every nth polygon centre, sorted by x then z.
std::vector<Spot> samplePoints(const ksim::MeshNavigator& nav, std::size_t wanted)
{
	const dtNavMesh& mesh = nav.mesh().detour();
	std::vector<Spot> all;
	for (int t = 0; t < mesh.getMaxTiles(); ++t)
	{
		const dtMeshTile* tile = mesh.getTile(t);
		if (tile == nullptr || tile->header == nullptr)
			continue;
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			const dtPoly& poly = tile->polys[p];
			if (poly.getType() != DT_POLYTYPE_GROUND || poly.vertCount == 0)
				continue;
			Spot c;
			for (int k = 0; k < poly.vertCount; ++k)
			{
				const float* v = &tile->verts[poly.verts[k] * 3];
				c.x += v[0]; c.y += v[1]; c.z += v[2];
			}
			c.x /= poly.vertCount; c.y /= poly.vertCount; c.z /= poly.vertCount;
			all.push_back(c);
		}
	}
	std::sort(all.begin(), all.end(), [](const Spot& a, const Spot& b) { return a.x != b.x ? a.x < b.x : a.z < b.z; });
	std::vector<Spot> points;
	if (all.empty())
		return points;
	const std::size_t step = std::max<std::size_t>(1, all.size() / wanted);
	for (std::size_t i = 0; i < all.size() && points.size() < wanted; i += step)
		points.push_back(all[i]);
	return points;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !std::filesystem::exists(std::string(argv[1]) + "/" + knav::regionFileName(ksim::simlink::kHubZoneX / knav::kRegionZones,
				ksim::simlink::kHubZoneZ / knav::kRegionZones)))
		{
			std::cout << "ksim_route_walk: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({ ksim::simlink::kHubZoneX + dx, ksim::simlink::kHubZoneZ + dz });
		const std::shared_ptr<ksim::MeshNavigator> nav = test::openWorldNav(argv[1], hubZones);
		const std::vector<Spot> points = samplePoints(*nav, 240);
		test::require(points.size() >= 100, "too few walkable spots");
		const knav::NavQuery query(nav->walker().mesh());
		knav::TravelFilter travel;
		travel.radius = static_cast<float>(kBody);

		int routes = 0, legs = 0, stalls = 0, offFloor = 0, neverEnd = 0, walkLegRefused = 0;
		std::map<std::string, int> byCause;
		for (std::size_t i = 1; i < points.size(); ++i)
		{
			const Spot& from = points[i - 1];
			const Spot& to = points[i];
			const ksim::Route route = nav->routeFrom({ from.x, from.y, from.z }, to.x, to.z, to.y, false, kBody);
			if (!route.found || route.points.size() < 2 || route.heights.size() != route.points.size())
				continue;
			++routes;
			std::vector<knav::Point> doors;
			for (const knav::DoorKey door : query.findPath({ static_cast<float>(from.x), static_cast<float>(from.y), static_cast<float>(from.z) },
					{ static_cast<float>(to.x), static_cast<float>(to.y), static_cast<float>(to.z) }, { 20.0f, 20.0f, 20.0f }, travel).doors)
			{
				knav::Point centre;
				double ax = 0.0, az = 0.0;
				if (query.doorFrame(door, centre, ax, az))
					doors.push_back(centre);
			}
			knav::Position at{ route.points[0].x, route.heights[0], route.points[0].z };
			bool routeStalled = false;
			for (std::size_t j = 1; j < route.points.size(); ++j)
			{
				++legs;
				const double tx = route.points[j].x, tz = route.points[j].z, ty = route.heights[j];
				const knav::Position legStart = at;
				const double length = std::hypot(tx - at.x, tz - at.z);
				const int budget = static_cast<int>(length / kStepLen * 2.0) + 40;
				int still = 0;
				bool arrived = false, stalled = false;
				for (int s = 0; s < budget && !arrived && !stalled; ++s)
				{
					const double dx = tx - at.x, dz = tz - at.z;
					const double d = std::hypot(dx, dz);
					if (d < kArrive) { arrived = true; break; }
					const double len = std::min(kStepLen, d);
					const knav::StepResult r = nav->walker().step(at, dx / d * len, dz / d * len);
					if (!r.onMesh) { stalled = true; break; }
					still = r.moved < kStillMove ? still + 1 : 0;
					at = r.at;
					stalled = still >= kStillSteps;
				}
				if (!arrived)
					stalled = true;
				const bool left = arrived && std::fabs(at.y - ty) > kFloorTol;
				if (!stalled && !left)
					continue;
				const knav::Point a{ static_cast<float>(legStart.x), static_cast<float>(route.heights[j - 1]), static_cast<float>(route.points[j - 1].z) };
				(void)a;
				const knav::Point pa{ static_cast<float>(route.points[j - 1].x), static_cast<float>(route.heights[j - 1]), static_cast<float>(route.points[j - 1].z) };
				const knav::Point pb{ static_cast<float>(tx), static_cast<float>(ty), static_cast<float>(tz) };
				const bool refused = !query.walkLeg(pa, pb, static_cast<float>(kBody), travel);
				std::string cause = std::fabs(ty - route.heights[j - 1]) > kHeightCause ? "height" : "flat";
				for (const knav::Point& d : doors)
					if (std::hypot(d[0] - at.x, d[2] - at.z) < kDoorNear || std::hypot(d[0] - tx, d[2] - tz) < kDoorNear)
						cause = "door";
				++(stalled ? stalls : offFloor);
				walkLegRefused += refused ? 1 : 0;
				++byCause[cause + (stalled ? " stall" : " off-floor")];
				std::printf("%s route %zu leg %zu/%zu: (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f), stopped (%.1f, %.1f, %.1f), %s, walkLeg %s\n",
					stalled ? "STALL" : "OFFFLOOR", i, j, route.points.size() - 1, route.points[j - 1].x, route.heights[j - 1], route.points[j - 1].z,
					tx, ty, tz, at.x, at.y, at.z, cause.c_str(), refused ? "refuses" : "accepts");
				routeStalled = true;
				at = { tx, ty, tz }; // carry on from the waypoint: the next leg is judged on its own
			}
			if (routeStalled)
				++neverEnd;
		}
		std::printf("routes walked: %d, legs: %d, stalled legs: %d, off-floor legs: %d, routes with a bad leg: %d, of the bad legs walkLeg also refuses: %d\n",
			routes, legs, stalls, offFloor, neverEnd, walkLegRefused);
		for (const auto& [cause, n] : byCause)
			std::printf("  %s: %d\n", cause.c_str(), n);
		test::require(routes >= 160, "only " + std::to_string(routes) + " routes found");
		if (kPinnedStalls >= 0)
			test::require(stalls + offFloor <= kPinnedStalls, std::to_string(stalls + offFloor) + " bad legs, pinned at " + std::to_string(kPinnedStalls));
		std::cout << "ksim_route_walk: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_route_walk: FAIL: " << e.what() << "\n";
		return 1;
	}
}
