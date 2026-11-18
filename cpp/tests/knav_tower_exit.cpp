// The Hub tower's stair (user play 2026-11-07): the Wanderer stood on the round tower next to the Hub bar, at the head of the
// building's outside stair about 50 u above the ground, and every move order down was refused with `MOVE_TO REJECTED: no_route`
// (simlink.cpp routes with the character's clearance). The client's pick called both ground clicks walkable and the debug overlay
// draws the stair as mesh. The cause was clearance: the roof is narrow, so most spots on it are polygons a 4 u body does not fit on
// (`wallDistance < r`), and the clearance search cannot leave such a polygon - the route closed there (a partial corridor, `Closed`),
// while the same ends route at radius 0. The fix is in the system (knav::NavQuery::findPath): the start snaps on to the nearest
// polygon the body fits on before the search, as the original's start snap r + 0.1 does; the goal is left to the caller's snap.
//
// This asks the order's own legs on the shipped world bake (argv[1], ~/git/kenshi/navworld_v11 at the time of the report), both ways, at radius
// 0 and at the race's clearance: from the reported roof spot and from a ring of roof starts within 8 u of it (a character stands
// somewhere on the roof, not exactly where the log rounds it), to each of the two refused ground clicks, and back up. Needs the bake
// only, never the game install; without a region file for the Hub's zone the test is skipped (exit 77).

#include "ksim/navigator.hpp"
#include "ksim/state.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "nav_source.hpp"

namespace {

using namespace ksim;

// The roof spot the server logged, and the two ground clicks the client's orders were refused for.
constexpr double kRoofX = -51341.5;
constexpr double kRoofY = 1619.2;
constexpr double kRoofZ = 2535.6;
struct Goal
{
	double x;
	double y;
	double z;
};
constexpr Goal kGround[] = { { -51353.0, 1569.0, 2630.0 }, { -51343.0, 1568.0, 2637.0 } };

// The Wanderer's clearance: the Greenlander RACE `pathfind footprint radius` (CombatData::pathRadius; the exe's
// CharMovement+0x364, NAVMESH_CACHE.md "Clearance"). The test reads no game data, so the number is the decoded instance.
constexpr double kBody = 4.0;
// Where a route may end (the click snap) and one step (the bake's walkableClimb): a start the fix moves must stay on the body's
// own level, not drop to the ground under a roof edge.
constexpr double kEndSnap = 5.0;
constexpr double kStep = 5.0;

// The Hub's zone cell (simlink_scenario.hpp) and the server's initial coverage round it (ksim_server).
constexpr int kHubZoneX = 20;
constexpr int kHubZoneZ = 32;

const char* reasonName(state::AiReason reason)
{
	switch (reason)
	{
	case state::AiReason::None: return "none";
	case state::AiReason::NoRoute: return "NoRoute";
	case state::AiReason::OffMesh: return "OffMesh";
	default: return "other";
	}
}

// One leg, both radii. Checked: found; the route ends near the goal; and it runs from the start's height to the goal's (the roof's
// 1619 to the ground's 1569, or back) - a route that stays on the roof's island is not the walk down.
bool leg(const MeshNavigator& nav, const knav::Position& from, const Goal& to, const char* what)
{
	bool ok = true;
	for (const double radius : { 0.0, kBody })
	{
		const Route route = nav.routeFrom(from, to.x, to.z, std::optional<double>(to.y), false, radius);
		const bool found = route.found && !route.points.empty();
		const double endGap = found ? std::hypot(route.points.back().x - to.x, route.points.back().z - to.z) : 0.0;
		const double fromY = found ? route.heights.front() : 0.0;
		const double toY = found ? route.heights.back() : 0.0;
		const bool height = std::fabs(fromY - from.y) <= kStep && std::fabs(toY - to.y) <= kStep;
		const bool pass = found && endGap <= kEndSnap && height;
		if (!pass)
			std::printf("FAIL %s from (%.1f, %.1f, %.1f) to (%.1f, %.1f): radius %4.1f found %d (%s) %zu points, end %.1f away, y %.1f -> %.1f\n",
				what, from.x, from.y, from.z, to.x, to.z, radius, found ? 1 : 0, found ? "ok" : reasonName(route.failure),
				route.points.size(), endGap, fromY, toY);
		ok = ok && pass;
	}
	return ok;
}

} // namespace

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0); // a failed leg's line must not sit in the buffer when require() aborts
	if (argc < 2)
	{
		std::printf("usage: knav_tower_exit <world bake dir>\n");
		return 77;
	}
	const std::string bake = argv[1];
	// The bake's regions are 4 x 4 zones: the Hub's zone (20, 32) lives in region_5_8.
	if (!std::filesystem::exists(bake + "/region_5_8.knav"))
	{
		std::printf("no region_5_8.knav in %s: the test needs the world bake\n", bake.c_str());
		return 77;
	}
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			zones.push_back({ kHubZoneX + dx, kHubZoneZ + dz });
	const std::shared_ptr<MeshNavigator> nav = test::openWorldNav(bake, zones);
	test::require(nav != nullptr, "the Hub zone opened");

	// The roof starts: the reported spot and a ring round it (the roof is round and the logged position is rounded to 0.1).
	std::vector<knav::Position> starts{ { static_cast<float>(kRoofX), static_cast<float>(kRoofY), static_cast<float>(kRoofZ) } };
	for (const double ring : { 1.0, 2.0, 3.0, 5.0, 8.0 })
		for (int a = 0; a < 8; ++a)
		{
			const double angle = 6.283185307179586 * a / 8.0;
			starts.push_back({ static_cast<float>(kRoofX + ring * std::sin(angle)), static_cast<float>(kRoofY),
				static_cast<float>(kRoofZ + ring * std::cos(angle)) });
		}

	bool ok = true;
	for (const knav::Position& start : starts)
		for (const Goal& goal : kGround)
			ok = leg(*nav, start, goal, "roof -> ground") && ok;
	// The reverse: every ground click back up to the roof, at the reported spot.
	for (const Goal& goal : kGround)
	{
		const knav::Position foot{ static_cast<float>(goal.x), static_cast<float>(goal.y), static_cast<float>(goal.z) };
		const Goal roof{ kRoofX, kRoofY, kRoofZ };
		ok = leg(*nav, foot, roof, "ground -> roof") && ok;
	}
	test::require(ok, "the tower's roof, its stair and the ground are one walk at the body's clearance, both ways");
	std::printf("tower exit: %zu roof starts x %zu ground clicks and back, radius 0 and %.1f: all route through the stair\n",
		starts.size(), std::size(kGround), kBody);
	return 0;
}
