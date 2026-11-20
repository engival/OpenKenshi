// A clear click walks straight to the exact point (kenshi-k16.7.45, PATHFINDING_STATIC section 1.8, rwh.144 exe 1403aaa90): no goal snap, no
// wall push, no search. Hub sample starts and goals taken 4.2 u off a wall contact (a slide from open ground into the wall: the body fits, r 4,
// but the click lies inside r + 0.5 u of the wall, where the old planPath pushed the end off the click). Every pair whose body walk to the click is clear must route as the two points start -> click, ending on the click within 0.05 u. Needs the Hub zone (else exit 77).

#include "ksim/nav/navigator.hpp"

#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace ksim;

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	test::require(argc == 2, "usage: ksim_straight_walk NAVWORLD");
	const double hubX = -50612.7;
	const double hubZ = -1158.0;
	const kworld::ZoneCell cell = kworld::zoneCellOf(hubX, hubZ);
	if (std::getenv("KSIM_NATIVE_NAV") == nullptr
		&& !std::filesystem::exists(std::string(argv[1]) + "/" + knav::regionFileName(cell.x / knav::kRegionZones, cell.z / knav::kRegionZones)))
	{
		std::printf("ksim_straight_walk: SKIPPED: needs the world navmesh\n");
		return 77;
	}
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			zones.push_back({ cell.x + dx, cell.z + dz });
	const auto nav = test::openWorldNav(argv[1], zones);
	test::require(nav != nullptr, "the Hub zone opened");
	const knav::NavQuery query(nav->walker().mesh());
	knav::TravelFilter travel;
	travel.radius = 4.0f;
	std::mt19937 rng(20261116);
	std::uniform_real_distribution<double> spread(-150.0, 150.0);
	std::uniform_real_distribution<double> angle(0.0, 6.283185307179586);
	int taken = 0;
	int bad = 0;
	for (int tries = 0; tries < 30000 && taken < 30; ++tries)
	{
		const auto from = nav->walker().snap({ hubX + spread(rng), 1085.0, hubZ + spread(rng) }, 40.0);
		const auto open = nav->walker().snap({ hubX + spread(rng), 1085.0, hubZ + spread(rng) }, 40.0);
		if (!from || !open)
			continue;
		const double a = angle(rng);
		const knav::Point openPoint{ static_cast<float>(open->x), static_cast<float>(open->y), static_cast<float>(open->z) };
		const knav::Point aim{ openPoint[0] + 60.0f * std::sin(static_cast<float>(a)), openPoint[1], openPoint[2] + 60.0f * std::cos(static_cast<float>(a)) };
		knav::Point hit;
		bool blocked = false;
		if (!query.slide(openPoint, aim, { 1.0f, 12.0f, 1.0f }, hit, blocked, travel) || !blocked)
			continue; // no wall within 60 u that way
		const knav::Point start{ static_cast<float>(from->x), static_cast<float>(from->y), static_cast<float>(from->z) };
		const float length = std::hypot(hit[0] - start[0], hit[2] - start[2]);
		if (length < 20.0f)
			continue;
		// The click 4.2 u back from the wall contact: the body fits (r 4) but the old goal push (r + 0.5) moved it.
		const knav::Point click{ hit[0] - 4.2f * std::sin(static_cast<float>(a)), hit[1], hit[2] - 4.2f * std::cos(static_cast<float>(a)) };
		if (std::fabs(start[1] - hit[1]) > 3.0f || !query.walkLeg(start, click, 4.0f, travel))
			continue; // not a clear straight walk: the search's business
		knav::PolyLoc at;
		if (!query.locate(click, { 1.0f, 12.0f, 1.0f }, at, travel) || query.wallDistance(at, 4.0f) < 4.0f)
			continue; // not a clear straight walk: the search's business
		if (!query.findPath(start, click, { 20.0f, 20.0f, 20.0f }, travel).doors.empty())
			continue; // a walk through a door polygon keeps the door routing (the shortcut refuses it)
		const Route route = nav->routeFrom(*from, click[0], click[2], std::optional<double>(click[1]), false, 4.0);
		++taken;
		const bool exact = route.found && route.points.size() == 2 && std::hypot(route.points.back().x - click[0], route.points.back().z - click[2]) < 0.05;
		if (!exact)
		{
			++bad;
			std::printf("  not straight: from (%.1f %.1f) click (%.2f %.2f): %zu points, end (%.2f %.2f)\n", start[0], start[2], click[0], click[2],
				route.points.size(), route.points.empty() ? 0.0 : route.points.back().x, route.points.empty() ? 0.0 : route.points.back().z);
		}
	}
	std::printf("ksim_straight_walk: %d clear wall-contact clicks, %d not routed straight to the click\n", taken, bad);
	test::require(taken >= 10, "at least 10 clear wall-contact clicks found");
	test::require(bad == 0, "every clear click routes straight to the exact point");
	return 0;
}
