// The runtime native loader (MeshNavigator::openNative, docs/formats/NAVMESH_CACHE.md "Runtime mesh"): the server's streaming
// navigation over the game's shipped navtiles, converted to Detour at load, no --navworld bake.
//  - the Hub's 3 x 3 zones open from the shipped tiles; each region's conversion is timed;
//  - street -> bar floor 1 at clearance radius 4 completes at the shipped length (~365 u; the bake's 327 at r 0), and the
//    back stair climbs (the route ends on floor 1, y > 1640);
//  - that route walked with knav::Walker arrives (a resident's routine walk on the runtime mesh);
//  - argv[1] (a world bake dir, optional) opens the same zones from the bake and prints its RSS and load time beside the native's;
//    `world-only DIR` measures a bake alone, `native-gamedata own|shared` the server's startup (the sim's game data loaded
//    first, the loader loading a second copy or sharing it).
// The Hub's shipped tile is stale, so its zone comes from our bake (the loader's stale-tile check, ksim_native_stale):
// both the shipped zones around it and the rebaked Hub stream through the same mesh. Runs against the install
// (KENSHI_DATA); exit 77 without the navtiles.

#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include "knav/native.hpp"
#include "knav/navmesh.hpp"
#include "knav/walk.hpp"

#include "test_util.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

constexpr float kStreet[3] = { -51320.0f, 1580.0f, 2700.0f }; // a street west of the Hub bar (knav_native_tiles)
constexpr float kBarFloor[3] = { -51169.0f, 1647.0f, 2665.0f }; // the middle of the bar's floor 1 (slab 20, y 1640..1647)

double msSince(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Resident set size in kB (the loader's memory is the point of the comparison; Linux only, 0 elsewhere).
std::size_t rssKb()
{
	std::ifstream status("/proc/self/status");
	std::string line;
	while (std::getline(status, line))
	{
		if (line.rfind("VmRSS:", 0) == 0)
			return static_cast<std::size_t>(std::stoul(line.substr(6)));
	}
	return 0;
}

struct Pt
{
	double x;
	double z;
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_native_nav: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> hub;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hub.push_back({ ksim::simlink::kHubZoneX + dx, ksim::simlink::kHubZoneZ + dz });
		// `world-only BAKE`: the bake's own footprint in a fresh process (the native conversion's arenas would mask it in one run).
		if (argc >= 3 && std::string(argv[1]) == "world-only")
		{
			const std::size_t before = rssKb();
			const auto begin = std::chrono::steady_clock::now();
			const std::shared_ptr<ksim::MeshNavigator> world = ksim::MeshNavigator::openWorld(argv[2], hub);
			std::cout << "world-only: " << world->mesh().tiles().size() << " tiles, " << world->mesh().polyCount() << " polys, "
					  << msSince(begin) << " ms, RSS +" << (rssKb() - before) / 1024 << " MB\n";
			return 0;
		}
		// `native-gamedata own|shared`: the server's startup as a process: the sim's merged game data loaded first (as
		// Simulation::newGame does), then the native loader over the Hub. `own` is the loader loading a second copy (what it
		// did before it could share); `shared` passes the sim's (openNative's gameData). Prints the data's own cost and the
		// whole process's RSS beside it.
		if (argc >= 3 && std::string(argv[1]) == "native-gamedata")
		{
			const std::size_t rssStart = rssKb();
			const auto dataStart = std::chrono::steady_clock::now();
			const auto data = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
			const std::size_t rssData = rssKb();
			std::cout << "game data: " << msSince(dataStart) << " ms, RSS +" << (rssData - rssStart) / 1024 << " MB ("
					  << rssData / 1024 << " MB total)\n";
			const bool shared = std::string(argv[2]) == "shared";
			const auto openStart = std::chrono::steady_clock::now();
			const std::shared_ptr<ksim::MeshNavigator> nav = ksim::MeshNavigator::openNative(KENSHI_DATA,
				{ { ksim::simlink::kHubZoneX, ksim::simlink::kHubZoneZ } }, {},
				shared ? data.get() : nullptr);
			nav->setCoverage(hub); // the three regions the main path converts beside the Hub's own
			const std::size_t rssOpen = rssKb();
			std::cout << (shared ? "shared" : "own") << " data: open + " << hub.size() << " zones " << msSince(openStart)
					  << " ms, RSS +" << (rssOpen - rssData) / 1024 << " MB (" << rssOpen / 1024 << " MB total)\n";
			return 0;
		}

		// The Hub's own zone first: one region converts at open; the rest zone by zone, each region's conversion timed once.
		const std::size_t rssStart = rssKb();
		const auto openStart = std::chrono::steady_clock::now();
		const std::shared_ptr<ksim::MeshNavigator> nav = ksim::MeshNavigator::openNative(KENSHI_DATA,
			{ { ksim::simlink::kHubZoneX, ksim::simlink::kHubZoneZ } });
		test::require(nav->native() && nav->streaming(), "openNative is not a streaming native navigator");
		std::cout << "open (zone " << ksim::simlink::kHubZoneX << "," << ksim::simlink::kHubZoneZ << ", region "
				  << ksim::simlink::kHubZoneX / knav::kRegionZones << "," << ksim::simlink::kHubZoneZ / knav::kRegionZones << " converted): "
				  << msSince(openStart) << " ms, " << nav->mesh().tiles().size() << " tiles\n";

		std::vector<kworld::ZoneCell> wanted{ { ksim::simlink::kHubZoneX, ksim::simlink::kHubZoneZ } };
		std::map<std::pair<int, int>, std::vector<kworld::ZoneCell>> byRegion;
		for (const kworld::ZoneCell zone : hub)
			byRegion[{ zone.x / knav::kRegionZones, zone.z / knav::kRegionZones }].push_back(zone);
		for (const auto& [region, zones] : byRegion)
		{
			wanted.insert(wanted.end(), zones.begin(), zones.end());
			const ksim::MeshNavigator::CoverageChange change = nav->setCoverage(wanted);
			std::cout << "region " << region.first << "," << region.second << ": " << change.readMs << " ms to obtain, "
					  << change.tilesAdded << " tiles added, " << change.applyMs << " ms applied\n";
		}
		test::require(nav->coveredZones().size() == hub.size(), "the Hub's 3 x 3 zones are not all covered");
		const std::size_t nativeTiles = nav->mesh().tiles().size();
		const std::size_t nativePolys = nav->mesh().polyCount();
		const std::size_t rssNative = rssKb();
		std::cout << "native: " << nativeTiles << " tiles, " << nativePolys << " polys, " << msSince(openStart)
				  << " ms, RSS +" << (rssNative - rssStart) / 1024 << " MB (" << rssNative / 1024 << " MB total)\n";
		test::require(nativeTiles > 2000, "the converted Hub has too few tiles");
		test::require(nativePolys > 30000, "the converted Hub has too few polygons");

		// The clearance query: a human (radius 4) gets from the street up the bar's back stair to floor 1 (the Hub is our
		// bake, its shipped tile stale).
		const knav::NavQuery query(nav->mesh(), 65535);
		knav::PolyLoc seed;
		test::require(query.locate({ kStreet[0], kStreet[1], kStreet[2] }, { 8.0f, 40.0f, 8.0f }, seed),
			"no converted polygon at the street seed");
		knav::TravelFilter travel;
		travel.radius = 4.0f;
		const auto routeStart = std::chrono::steady_clock::now();
		const knav::PathResult up = query.findPath(seed.point, { kBarFloor[0], kBarFloor[1], kBarFloor[2] }, { 8.0f, 6.0f, 8.0f }, travel);
		std::cout << "street -> bar floor 1 at radius 4: " << (up.complete ? "complete" : "INCOMPLETE")
				  << " (failure " << static_cast<int>(up.failure) << "), " << up.length << " u, ends at y "
				  << (up.points.empty() ? 0.0f : up.points.back()[1]) << ", " << msSince(routeStart) << " ms\n";
		test::require(up.complete, "street -> bar floor 1 at radius 4 did not complete");
		test::require(!up.points.empty() && up.points.back()[1] > 1640.0f, "the bar's back stair did not climb to floor 1");
		// 390 u at r 4 on the rebaked Hub (390.5 on 2026-11-05; the shipped-tile route was 365).
		test::require(up.length > 300.0f && up.length < 450.0f, "the bar route is not the expected one (~390 u)");

		// The same route walked, as a resident walks to the tavern: knav::Walker over the planned corners, arriving at floor 1.
		std::vector<Pt> corners;
		for (const knav::Point& p : up.points)
			corners.push_back({ p[0], p[2] });
		const knav::Walker walker(nav->mesh());
		knav::Position at{ seed.point[0], seed.point[1], seed.point[2] };
		std::size_t next = 0;
		int guard = 0;
		while (next < corners.size() && ++guard < 20000)
		{
			const knav::StepResult step = walker.follow(at, corners, next, 100.0 * 0.05);
			test::require(step.onMesh, "the walk left the mesh at (" + std::to_string(at.x) + ", " + std::to_string(at.z) + ")");
			at = step.at;
		}
		test::require(next == corners.size(), "the walked route did not finish");
		test::require(std::hypot(at.x - corners.back().x, at.z - corners.back().z) < 1.0, "the walk did not arrive");
		test::require(at.y > 1640.0, "the walk did not arrive on the bar's floor 1");
		std::cout << "walked to the bar: arrived at " << at.x << "," << at.y << "," << at.z << " after " << guard << " steps\n";

		// The cache bound (openNative keeps the resident regions plus two): walk the coverage A (the Hub) -> B -> C, far apart,
		// and check the oldest regions are dropped and RSS does not keep growing with the hops.
		const auto block = [](int cx, int cz)
		{
			std::vector<kworld::ZoneCell> zones;
			for (int dz = -1; dz <= 1; ++dz)
				for (int dx = -1; dx <= 1; ++dx)
					zones.push_back({ cx + dx, cz + dz });
			return zones;
		};
		const auto regionsOf = [](const std::vector<kworld::ZoneCell>& zones)
		{
			std::set<std::pair<int, int>> regions;
			for (const kworld::ZoneCell zone : zones)
				regions.insert({ zone.x / knav::kRegionZones, zone.z / knav::kRegionZones });
			return regions;
		};
		const std::vector<kworld::ZoneCell> farB = block(34, 20);
		const std::vector<kworld::ZoneCell> farC = block(12, 44);
		const std::vector<kworld::ZoneCell> farD = block(44, 44); // as many regions as C: the hop that shows the memory plateau
		const std::vector<kworld::ZoneCell> farE = block(52, 12);
		const std::vector<kworld::ZoneCell> farF = block(20, 44);
		const std::size_t rssHub = rssKb();
		nav->setCoverage(farB);
		const std::vector<std::pair<int, int>> afterB = nav->nativeCachedRegions();
		const std::size_t rssB = rssKb();
		const std::set<std::pair<int, int>> bRegions = regionsOf(farB);
		bool hubOldestKept = false;
		for (const auto& region : afterB)
			hubOldestKept = hubOldestKept || region == std::make_pair(4, 7);
		test::require(!hubOldestKept, "the oldest Hub region stayed cached after the coverage moved far away");
		test::require(afterB.size() <= bRegions.size() + 2, "the region cache is not the resident regions plus two");
		std::size_t rssLast = 0;
		std::size_t rssPrev = 0;
		std::cout << "cache: Hub " << rssHub / 1024 << " MB, after B (" << afterB.size() << " regions kept) " << rssB / 1024 << " MB";
		for (const std::vector<kworld::ZoneCell>* far : { &farC, &farD, &farE, &farF })
		{
			nav->setCoverage(*far);
			test::require(nav->nativeCachedRegions().size() <= regionsOf(*far).size() + 2,
				"the region cache is not the resident regions plus two");
			rssPrev = rssLast;
			rssLast = rssKb();
			std::cout << ", " << rssLast / 1024 << " MB";
		}
		std::cout << " over C-F\n";
		// The working set reaches its peak converting a second block of regions (the allocator keeps the freed conversion
		// temporaries), then holds: the last two hops of the same footprint must not grow it again.
		test::require(rssLast <= rssPrev + 16 * 1024, "RSS keeps growing with the coverage hops (more than 16 MB after the peak)");

		// Beside a world bake of the same zones (the numbers the streaming coverage was measured with).
		if (argc >= 2 && std::string(argv[1]) != "-")
		{
			const auto worldStart = std::chrono::steady_clock::now();
			const std::shared_ptr<ksim::MeshNavigator> world = ksim::MeshNavigator::openWorld(argv[1], hub);
			const std::size_t rssWorld = rssKb();
			std::cout << "navworld " << argv[1] << ": " << world->mesh().tiles().size() << " tiles, " << world->mesh().polyCount()
					  << " polys, " << msSince(worldStart) << " ms, RSS +" << (rssWorld - rssNative) / 1024 << " MB (" << rssWorld / 1024 << " MB total)\n";
			std::cout << "native vs bake: " << nativeTiles << " vs " << world->mesh().tiles().size() << " tiles, " << nativePolys << " vs "
					  << world->mesh().polyCount() << " polys\n";
		}
		std::cout << "ksim_native_nav ok\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_native_nav: " << e.what() << "\n";
		return 1;
	}
}
