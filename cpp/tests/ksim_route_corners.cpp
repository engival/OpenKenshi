// Routes carry no needless pushed corners (user 2026-11-15, kenshi-k16.6.247 / .248): the Heft Longhouse route 134 u against 90 u straight,
// each interior corner 5-9 u off the line through its neighbours. planPath pushed every inner corner off its wall by r + kCornerPush
// whether or not a wall held it, and never pulled the string tight again. Checked here over real routes at the Wanderer's clearance
// (the runtime mesh, or the bake in argv[1] with KSIM_NAVWORLD_BAKE=1; the test says which):
//   - no removable waypoint: the leg joining a waypoint's neighbours does not walk for the whole body (NavQuery::walkLeg at the radius);
//   - the length stays within a few u (kFewUnits) of the shortest clear path: the route's own corners moved on a grid (kReach, kGrid) where both
//     legs still walk for the whole body, keeping the shortest (an upper bound of the true shortest; a door route skips it);
// The routes: the Longhouse case, the Storm House door's legs (ksim_storm_door's points) and 24 seeded routes round the Hub bar.
// Labels: [grounded rwh198] = measured on the original (findings/rwh198_route_waypoints_live.md): the walked / straight ratio of the Longhouse
// click (1.44-1.63x) and of each Storm House door leg, and the door legs crossing the door plane inside the leaf's span. [pin] = our own planner's
// output (the removable-waypoint and shortest-clear-path checks compare our planner with itself: the original keeps no waypoint list).
// Needs the world mesh only; without the Longhouse zone's region file the test is skipped (exit 77).

#include "ksim/nav/navigator.hpp"

#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace ksim;

constexpr double kBody = 4.0; // the Wanderer's pathRadius (the Greenlander RACE footprint)
constexpr double kFewUnits = 12.0; // the route may be this much longer than the shortest clear path found: every corner keeps its push of r + 5
	// (the walker's margin: it overshoots a corner on momentum), and the search's corners may sit at r; a ~100 degree turn like the Longhouse's
	// turns the 5 u into 10. Pulling the corners in to r brings it to 2 u but blocked the walker (knav_interior_walk's ramp).
constexpr double kMargin = 5.0; // the corner push: a skip must keep r + kMargin from the walls to count as clear (navmesh.cpp kCornerPush)
constexpr double kDoorNear = 20.0; // waypoints this near a door's middle are the door's (navmesh.cpp kDoorClutter)
constexpr double kReach = 12.0; // the corner search: how far a corner moves
constexpr double kGrid = 1.5;

// rwh198 Storm House door legs: walked / straight ratios (to the click, to the arrival point) of the original, per (inside point, outside point, direction).
struct DoorBand { double toGoal, toArrival; };
const DoorBand kDoorIn[5][2] = { { { 1.14, 1.03 }, { 1.28, 1.21 } }, { { 1.54, 1.51 }, { 1.63, 1.59 } }, { { 0.91, 1.01 }, { 1.03, 1.18 } },
	{ { 0.98, 1.02 }, { 1.22, 1.22 } }, { { 1.19, 1.21 }, { 1.31, 1.32 } } };
const DoorBand kDoorOut[5][2] = { { { 1.04, 1.04 }, { 1.48, 1.49 } }, { { 1.14, 1.08 }, { 1.58, 1.56 } }, { { 1.03, 1.03 }, { 1.40, 1.38 } },
	{ { 1.03, 1.03 }, { 1.18, 1.13 } }, { { 1.20, 1.18 }, { 1.38, 1.38 } } };
constexpr double kRatioMargin = 0.15; // on top of the original's largest ratio: its walked lengths include goal overshoot (upper bounds)
constexpr double kDoorPlaneX = -50619.11; // the Hub bar door's plane and leaf span (rwh198)
constexpr double kLeafZLo = -1170.981, kLeafZHi = -1141.101;
constexpr double kLeafSlack = 3.0;
constexpr double kMaxSnap = 12.5; // the original's goal snap stopped its walker 0.1-12.3 u short of these clicks

struct Leg
{
	std::string what;
	double ratioLo = 0.0, ratioHi = 0.0; // [grounded rwh198] walked / straight band; 0 = none
	bool door = false; // [grounded rwh198] every crossing of the door plane lies in the leaf span
	knav::Position from;
	double toX, toY, toZ;
};

int ratioFails = 0;
int doorFails = 0;
double routeMicros = 0.0; // the body routes' own time (planPath's cost, k16.6.244 watches it)

double planar(double ax, double az, double bx, double bz) { return std::hypot(ax - bx, az - bz); }

double lengthOf(const Route& route)
{
	double length = 0.0;
	for (std::size_t i = 1; i < route.points.size(); ++i)
		length += planar(route.points[i - 1].x, route.points[i - 1].z, route.points[i].x, route.points[i].z);
	return length;
}

// Returns false (and prints) when the route fails a check. `removable` counts removable waypoints, `worstExtra` the length over the shortest clear path found.
bool check(const MeshNavigator& nav, const knav::NavQuery& query, const Leg& leg, int& routes, int& removable, double& worstExtra, bool print)
{
	const auto began = std::chrono::steady_clock::now();
	const Route body = nav.routeFrom(leg.from, leg.toX, leg.toZ, std::optional<double>(leg.toY), false, kBody);
	routeMicros += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count();
	if (!body.found)
		return true; // no route either way: not this test's business
	++routes;
	bool ok = true;
	knav::TravelFilter travel;
	travel.radius = static_cast<float>(kBody);
	// A door route keeps the waypoints at its door (throughDoorMiddle: the way through the leaf and the approach to it).
	const knav::Point legFrom{ static_cast<float>(leg.from.x), static_cast<float>(leg.from.y), static_cast<float>(leg.from.z) };
	const knav::Point legTo{ static_cast<float>(leg.toX), static_cast<float>(leg.toY), static_cast<float>(leg.toZ) };
	std::vector<knav::Point> doorSpots;
	for (const knav::DoorKey door : query.findPath(legFrom, legTo, { 20.0f, 20.0f, 20.0f }, travel).doors)
	{
		knav::Point centre;
		double axisX = 0.0;
		double axisZ = 0.0;
		if (query.doorFrame(door, centre, axisX, axisZ))
			doorSpots.push_back(centre);
	}
	const auto atDoor = [&](double x, double z)
	{
		return std::any_of(doorSpots.begin(), doorSpots.end(), [&](const knav::Point& d) { return planar(x, z, d[0], d[2]) < kDoorNear; });
	};
	for (std::size_t j = 1; j + 1 < body.points.size(); ++j)
	{
		const knav::Point a{ static_cast<float>(body.points[j - 1].x), static_cast<float>(body.heights[j - 1]), static_cast<float>(body.points[j - 1].z) };
		const knav::Point b{ static_cast<float>(body.points[j + 1].x), static_cast<float>(body.heights[j + 1]), static_cast<float>(body.points[j + 1].z) };
		if (!atDoor(body.points[j].x, body.points[j].z) && query.walkLeg(a, b, travel.radius + kMargin, travel))
		{
			++removable;
			ok = false;
			if (print)
				std::printf("REMOVABLE [pin] %s: waypoint %zu of %zu (%.1f, %.1f)\n", leg.what.c_str(), j, body.points.size(), body.points[j].x, body.points[j].z);
		}
	}
	// The shortest clear path, as far as a search finds it: the route's own corners moved on a grid (kReach u either way, kGrid apart,
	// two passes) wherever both their legs still walk for the whole body, keeping the shortest. A door route skips this (its door
	// waypoints are fixed).
	double shortest = lengthOf(body);
	if (doorSpots.empty() && body.points.size() > 2)
	{
		std::vector<knav::Point> pts;
		for (std::size_t i = 0; i < body.points.size(); ++i)
			pts.push_back({ static_cast<float>(body.points[i].x), static_cast<float>(body.heights[i]), static_cast<float>(body.points[i].z) });
		const auto dist = [](const knav::Point& p, const knav::Point& q) { return std::hypot(p[0] - q[0], p[2] - q[2]); };
		for (int pass = 0; pass < 2; ++pass)
			for (std::size_t i = 1; i + 1 < pts.size(); ++i)
			{
				const knav::Point base = pts[i];
				double best = dist(pts[i - 1], base) + dist(base, pts[i + 1]);
				for (double dx = -kReach; dx <= kReach; dx += kGrid)
					for (double dz = -kReach; dz <= kReach; dz += kGrid)
					{
						const knav::Point cand{ base[0] + static_cast<float>(dx), base[1], base[2] + static_cast<float>(dz) };
						const double len = dist(pts[i - 1], cand) + dist(cand, pts[i + 1]);
						if (len < best - 1e-3 && query.walkLeg(pts[i - 1], cand, travel.radius, travel) && query.walkLeg(cand, pts[i + 1], travel.radius, travel))
						{
							best = len;
							pts[i] = cand;
						}
					}
			}
		shortest = 0.0;
		for (std::size_t i = 1; i < pts.size(); ++i)
			shortest += dist(pts[i - 1], pts[i]);
	}
	// Start through the waypoints to the last one (the arrival point: the original's goal snap may stop short of the click, rwh198 compares to it).
	double walked = 0.0;
	double px = leg.from.x, pz = leg.from.z;
	for (const auto& p : body.points)
	{
		walked += planar(px, pz, p.x, p.z);
		px = p.x;
		pz = p.z;
	}
	const double straight = planar(leg.from.x, leg.from.z, px, pz);
	if (leg.ratioHi > 0.0)
	{
		const double ratio = walked / std::max(straight, 1e-6);
		if (print)
			std::printf("  [grounded rwh198] %s: walked %.1f u / straight %.1f u = %.2fx (band %.2f-%.2f)\n", leg.what.c_str(), walked, straight, ratio, leg.ratioLo, leg.ratioHi);
		if (ratio < leg.ratioLo || ratio > leg.ratioHi)
		{
			ok = false;
			++ratioFails;
			std::printf("RATIO [grounded rwh198] %s: %.2fx outside %.2f-%.2f\n", leg.what.c_str(), ratio, leg.ratioLo, leg.ratioHi);
		}
	}
	if (leg.door)
	{
		// The route passes the door plane inside the leaf span: some point of it within 0.5 u of the plane lies in the span (+-kLeafSlack:
		// the original's outside walkway re-crosses the plane up to 2.5 u beyond the leaf end, and a waypoint may sit on the plane).
		bool through = false;
		double ax = leg.from.x, az = leg.from.z;
		for (std::size_t i = 0; i < body.points.size() && !through; ++i)
		{
			const double bx = body.points[i].x, bz = body.points[i].z;
			const int steps = std::max(1, static_cast<int>(planar(ax, az, bx, bz) / 0.25));
			for (int k = 0; k <= steps && !through; ++k)
			{
				const double x = ax + (bx - ax) * k / steps, z = az + (bz - az) * k / steps;
				through = std::fabs(x - kDoorPlaneX) <= 0.5 && z >= kLeafZLo - kLeafSlack && z <= kLeafZHi + kLeafSlack;
			}
			ax = bx;
			az = bz;
		}
		if (!through)
		{
			ok = false;
			++doorFails;
			std::printf("DOOR [grounded rwh198] %s: never passes the door plane inside the leaf span %.1f..%.1f\n", leg.what.c_str(), kLeafZLo, kLeafZHi);
		}
		else if (print)
			std::printf("  [grounded rwh198] %s: passes the door plane inside the leaf span\n", leg.what.c_str());
	}
	const double extra = lengthOf(body) - shortest;
	if (print && leg.what == "Longhouse")
	{
		std::printf("Longhouse: %zu points, %.1f u (shortest clear path found %.1f):", body.points.size(), lengthOf(body), shortest);
		for (const auto& p : body.points)
			std::printf(" (%.1f %.1f)", p.x, p.z);
		std::printf("\n");
	}
	worstExtra = std::max(worstExtra, extra);
	if (extra > kFewUnits)
	{
		ok = false;
		if (print)
			std::printf("LONG [pin] %s: %.1f u against %.1f for the shortest clear path found (%zu points)\n", leg.what.c_str(), lengthOf(body), shortest, body.points.size());
	}
	if (!ok && print)
	{
		std::printf("  %s route:", leg.what.c_str());
		for (const auto& p : body.points)
			std::printf(" (%.3f %.3f)", p.x, p.z);
		std::printf("\n");
	}
	return ok;
}

std::shared_ptr<MeshNavigator> openAround(const char* bake, double x, double z)
{
	const kworld::ZoneCell cell = kworld::zoneCellOf(x, z);
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			zones.push_back({ cell.x + dx, cell.z + dz });
	return test::openWorldNav(bake, zones);
}

bool hasRegion(const char* bake, double x, double z)
{
	const kworld::ZoneCell cell = kworld::zoneCellOf(x, z);
	return test::worldRegionReady(bake, cell.x, cell.z);
}

} // namespace

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	test::require(argc == 2, "usage: ksim_route_corners NAVWORLD");
	if (!hasRegion(argv[1], 68340.0, -71150.0) || !hasRegion(argv[1], -50612.0, -1158.0))
	{
		std::printf("ksim_route_corners: SKIPPED: needs the world navmesh\n");
		return 77;
	}
	std::printf("ksim_route_corners: on the %s\n", test::bakeNav() ? "navworld bake (KSIM_NAVWORLD_BAKE)" : "runtime mesh");
	int routes = 0;
	int removable = 0;
	double worstExtra = 0.0;
	bool ok = true;

	{
		const auto nav = openAround(argv[1], 68340.0, -71150.0);
		test::require(nav != nullptr, "the Longhouse zone opened");
		const knav::NavQuery query(nav->walker().mesh());
		Leg longhouse{ "Longhouse", 1.30, 1.63 + 0.04, false, { 68338.93f, 1174.30f, -71117.49f }, 68330.125, 1174.300, -71207.391 };
		ok = check(*nav, query, longhouse, routes, removable, worstExtra, true) && ok;
		// [grounded rwh198] the reverse click (130.3 u, 1.44x for the original) goes round the other side of the obstruction
		const Leg reverse{ "Longhouse reverse", longhouse.ratioLo, longhouse.ratioHi, false, { 68330.125f, 1174.30f, -71207.391f }, 68338.93, 1174.300, -71117.49 };
		ok = check(*nav, query, reverse, routes, removable, worstExtra, true) && ok;
		longhouse.ratioHi = 0.0;
		// The same legs from a ring of starts a body could stand on (the route's waypoints move with the start).
		for (int i = 0; i < 12; ++i)
		{
			const double angle = 6.283185307179586 * i / 12.0;
			Leg ring = longhouse; // (rings start 6 u off: no rwh198 band)
			ring.ratioLo = ring.ratioHi = 0.0;
			ring.what = "Longhouse ring " + std::to_string(i);
			ring.from = { static_cast<float>(68338.93 + 6.0 * std::sin(angle)), 1174.30f, static_cast<float>(-71117.49 + 6.0 * std::cos(angle)) };
			ok = check(*nav, query, ring, routes, removable, worstExtra, true) && ok;
		}
	}
	{
		const auto nav = openAround(argv[1], -50612.7, -1158.0);
		test::require(nav != nullptr, "the Hub zone opened");
		const knav::NavQuery query(nav->walker().mesh());
		struct Pt { double x, z; };
		const Pt outside[] = { { -50642.5, -1095.3 }, { -50660.0, -1120.0 } };
		const Pt inside[] = { { -50612.7, -1158.0 }, { -50610.3, -1159.5 }, { -50600.0, -1190.0 }, { -50620.0, -1180.0 }, { -50585.0, -1225.0 } };
		const auto at = [&](const Pt& p, double y) { return nav->walker().snap({ p.x, y, p.z }, 40.0); };
		for (int ia = 0; ia < 5; ++ia)
			for (int ib = 0; ib < 2; ++ib)
				for (const bool leaving : { true, false })
				{
					const Pt& a = inside[ia];
					const Pt& b = outside[ib];
					const auto from = at(leaving ? a : b, leaving ? 1081.0 : 1089.0);
					const auto to = at(leaving ? b : a, leaving ? 1089.0 : 1081.0);
					if (!from || !to)
						continue;
					// [grounded rwh198] a leg starts and ends on the mesh: each click snaps no further than the original's goal snap did
					const Pt& rawFrom = leaving ? a : b;
					const Pt& rawTo = leaving ? b : a;
					if (planar(from->x, from->z, rawFrom.x, rawFrom.z) > kMaxSnap || planar(to->x, to->z, rawTo.x, rawTo.z) > kMaxSnap)
					{
						std::printf("OFFMESH [grounded rwh198] Storm door i%d o%d: a click is more than %.1f u from the mesh\n", ia + 1, ib + 1, kMaxSnap);
						ok = false;
					}
					const DoorBand& band = (leaving ? kDoorOut : kDoorIn)[ia][ib];
					const Leg leg{ "Storm door i" + std::to_string(ia + 1) + " o" + std::to_string(ib + 1) + (leaving ? " out" : " in"), 0.99,
						std::max(band.toGoal, band.toArrival) + kRatioMargin, true,
						{ static_cast<float>(from->x), static_cast<float>(from->y), static_cast<float>(from->z) }, to->x, to->y, to->z };
					ok = check(*nav, query, leg, routes, removable, worstExtra, true) && ok;
				}
		// 24 seeded routes round the Hub bar (a fixed seed: the same sample every run, so the planPath cost compares).
		std::mt19937 rng(20261115);
		std::uniform_real_distribution<double> spread(-150.0, 150.0);
		int made = 0;
		for (int tries = 0; tries < 400 && made < 24; ++tries)
		{
			const Pt a{ -50612.7 + spread(rng), -1158.0 + spread(rng) };
			const Pt b{ -50612.7 + spread(rng), -1158.0 + spread(rng) };
			const auto from = at(a, 1085.0);
			const auto to = at(b, 1085.0);
			if (!from || !to || planar(from->x, from->z, to->x, to->z) < 20.0)
				continue;
			const Leg leg{ "Hub sample " + std::to_string(made), 0.0, 0.0, false, { static_cast<float>(from->x), static_cast<float>(from->y), static_cast<float>(from->z) }, to->x, to->y, to->z };
			const int before = routes;
			ok = check(*nav, query, leg, routes, removable, worstExtra, true) && ok;
			made += routes > before ? 1 : 0;
		}
		test::require(made >= 20, "at least 20 seeded Hub routes found");
	}
	std::printf("ksim_route_corners: %d routes, %d removable waypoints, worst length over the shortest clear path %.1f u, %.0f us per route\n",
		routes, removable, worstExtra, routeMicros / std::max(1, routes));
	std::printf("ksim_route_corners: [grounded rwh198] %d ratio failures, %d door-plane failures; [pin] %d removable waypoints\n", ratioFails, doorFails, removable);
	test::require(ok, "[grounded rwh198] ratios and door crossings hold; [pin] no removable waypoint or detour over the shortest clear path");
	return 0;
}
