// Coarse region/portal graph on baked world regions: knav_coarse DIR RX0 RZ0 RX1 RZ1 builds the graph
// over those regions of a `knav_bake --world DIR` bake, then routes from the Hub to the nearest other
// town inside them.

#include "knav/coarse.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/towns.hpp"
#include "test_util.hpp"
#include "hub_point.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace {

using knav::Point;
using Clock = std::chrono::steady_clock;

const Point kReach = { 40.0f, 60.0f, 40.0f };
constexpr int kRegionTiles = knav::kRegionZones * knav::kTilesPerZone;

double millis(Clock::time_point from)
{
	return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

float flatDistance(const Point& a, const Point& b)
{
	return std::hypot(a[0] - b[0], a[2] - b[2]);
}

std::pair<int, int> regionOf(double x, double z)
{
	const kworld::ZoneCell cell = kworld::zoneCellOf(x, z);
	return { cell.x / knav::kRegionZones, cell.z / knav::kRegionZones };
}

// Region meshes, loaded when first asked for.
class Meshes
{
public:
	explicit Meshes(std::string dir) : dir_(std::move(dir)) {}

	const knav::NavMesh& at(std::pair<int, int> region)
	{
		auto found = meshes_.find(region);
		if (found == meshes_.end())
			found = meshes_.emplace(region, knav::NavMesh::loadRegion(dir_, region.first, region.second, 0)).first;
		return found->second;
	}

private:
	std::string dir_;
	std::map<std::pair<int, int>, knav::NavMesh> meshes_;
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 6, "usage: knav_coarse DIR RX0 RZ0 RX1 RZ1");
		const std::string dir = argv[1];
		const int rx0 = std::atoi(argv[2]);
		const int rz0 = std::atoi(argv[3]);
		const int rx1 = std::atoi(argv[4]);
		const int rz1 = std::atoi(argv[5]);
		const std::string dataDir = KENSHI_DATA;
		const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const auto ground = [&](double x, double z)
		{
			return Point{ static_cast<float>(x), static_cast<float>(terrain.height(x, z)), static_cast<float>(z) };
		};

		auto step = Clock::now();
		knav::CoarseGraph graph;
		for (int rz = rz0; rz <= rz1; ++rz)
		{
			for (int rx = rx0; rx <= rx1; ++rx)
			{
				const knav::NavMesh mesh = knav::NavMesh::loadRegion(dir, rx, rz, knav::kClusterTiles);
				const knav::TileRect own{ rx * kRegionTiles, rz * kRegionTiles, (rx + 1) * kRegionTiles - 1,
					(rz + 1) * kRegionTiles - 1 };
				graph.merge(knav::CoarseGraph::build(mesh, knav::CoarseGraph::clustersOf(own), 4));
			}
		}
		const knav::CoarseStats stats = graph.stats();
		const std::vector<std::uint8_t> bytes = graph.serialize();
		std::cout << "graph over regions " << rx0 << "," << rz0 << " .. " << rx1 << "," << rz1 << ": " << millis(step) / 1000
			<< " s, " << stats.clusters << " clusters, " << stats.nodes << " nodes, " << stats.portals << " portals, "
			<< stats.pairs << " intra edges, " << stats.points << " path points, " << bytes.size() << " bytes\n";
		test::require(stats.clusters > 100 && stats.portals > stats.clusters, "graph is nearly empty");

		// Byte-identical after a round trip, and for one thread against four (a Hub-sized patch).
		test::require(knav::CoarseGraph::deserialize(bytes).serialize() == bytes, "serialisation does not round trip");
		{
			const auto [hx, hz] = regionOf(kHubX, kHubZ);
			const knav::NavMesh mesh = knav::NavMesh::loadRegion(dir, hx, hz, knav::kClusterTiles);
			const knav::ClusterId centre = knav::clusterOfTile(
				static_cast<int>(std::floor((kHubX - kworld::kWorldMin) / 256)), static_cast<int>(std::floor((kHubZ - kworld::kWorldMin) / 256)));
			const knav::TileRect patch{ centre.x - 3, centre.z - 3, centre.x + 3, centre.z + 3 };
			step = Clock::now();
			const auto single = knav::CoarseGraph::build(mesh, patch, 1).serialize();
			const double singleMs = millis(step);
			step = Clock::now();
			const auto quad = knav::CoarseGraph::build(mesh, patch, 4).serialize();
			std::cout << "7x7 clusters around the Hub: 1 thread " << singleMs << " ms, 4 threads " << millis(step) << " ms, "
				<< single.size() << " bytes\n";
			test::require(single == quad, "graph differs between 1 and 4 threads");
			test::require(single.size() > 1000, "Hub patch is empty");
		}

		// Nearest other town inside the baked regions.
		const auto gameData = kworld::loadMergedGameData(dataDir);
		const auto towns = kworld::Towns::join(gameData, dataDir);
		const kworld::TownPlacement* nearest = nullptr;
		double nearestDistance = std::numeric_limits<double>::max();
		for (const kworld::TownPlacement& town : towns.placements())
		{
			const auto [tx, tz] = regionOf(town.x, town.z);
			const double distance = std::hypot(town.x - kHubX, town.z - kHubZ);
			if (distance < 100.0 || tx < rx0 || tx > rx1 || tz < rz0 || tz > rz1 || distance >= nearestDistance)
				continue;
			nearest = &town;
			nearestDistance = distance;
		}
		test::require(nearest != nullptr, "no other town inside the baked regions");
		std::cout << "nearest other town: " << nearest->targetID << " at (" << nearest->x << ", " << nearest->z << "), "
			<< nearestDistance << " units from the Hub\n";

		Meshes meshes(dir);
		const Point hub = ground(kHubX, kHubZ);
		const Point there = ground(nearest->x, nearest->z);
		const knav::NavMesh& hubMesh = meshes.at(regionOf(kHubX, kHubZ));
		const knav::NavMesh& townMesh = meshes.at(regionOf(nearest->x, nearest->z));

		// (a) A route exists, and refining it gives about the length the graph said.
		step = Clock::now();
		const knav::CoarsePath route = graph.findPath(hubMesh, townMesh, hub, there, kReach);
		const double routeMs = millis(step);
		test::require(route.found, "no coarse route from the Hub to the nearest town");
		float refined = 0.0f;
		step = Clock::now();
		for (const knav::CoarseLeg& leg : route.legs)
		{
			const knav::PathResult detail = graph.refine(meshes.at({ leg.cluster.x * knav::kClusterTiles / kRegionTiles, leg.cluster.z * knav::kClusterTiles / kRegionTiles }), leg);
			test::require(detail.complete, "leg " + std::to_string(&leg - route.legs.data()) + " of " + std::to_string(route.legs.size())
				+ " does not refine (hop " + std::to_string(leg.hop) + ", cluster " + std::to_string(leg.cluster.x) + "," + std::to_string(leg.cluster.z)
				+ ", from tile " + std::to_string(leg.from.tileX) + "," + std::to_string(leg.from.tileZ) + " poly " + std::to_string(leg.from.poly)
				+ ", to tile " + std::to_string(leg.to.tileX) + "," + std::to_string(leg.to.tileZ) + " poly " + std::to_string(leg.to.poly) + ")");
			refined += detail.length;
		}
		const double refineMs = millis(step);
		const float straight = flatDistance(hub, there);
		std::cout << "route: " << route.legs.size() << " legs, coarse length " << route.length << ", refined " << refined
			<< ", straight " << straight << " (factor " << refined / straight << "), query " << routeMs << " ms, refine "
			<< refineMs << " ms\n";
		test::require(std::fabs(refined - route.length) <= 0.05f * route.length, "refined length differs from the coarse length");
		test::require(refined >= straight * 0.99f && refined <= straight * 3.0f, "refined length is not sane against the straight line");

		// The coarse route against a plain Detour path on a mesh holding the corridor between the two (the
		// regions are far too big to hold together, so only the tiles around the straight line).
		{
			const auto tileOf = [](double v) { return static_cast<int>(std::floor((v - kworld::kWorldMin) / 256.0)); };
			const knav::TileRect box{ std::min(tileOf(kHubX), tileOf(nearest->x)) - 4, std::min(tileOf(kHubZ), tileOf(nearest->z)) - 4,
				std::max(tileOf(kHubX), tileOf(nearest->x)) + 4, std::max(tileOf(kHubZ), tileOf(nearest->z)) + 4 };
			std::vector<std::string> files;
			std::set<std::pair<int, int>> wanted = { regionOf(kHubX, kHubZ), regionOf(nearest->x, nearest->z) };
			for (const auto& [rx, rz] : wanted)
				files.push_back(dir + "/" + knav::regionFileName(rx, rz));
			const knav::NavMesh corridor = knav::NavMesh::load(files, box);
			// Since 18289478 (no bake-time erosion: the wall band stays in the mesh as Area::Edge polygons) the corridor holds several
			// times the polygons it did, and one plain Detour search runs out of its 4,096 nodes (OutOfNodes) about 3,950 u into this
			// 4,000 u trip and returns a partial path. The mesh did not get worse: the search is bounded by design (that is what the coarse
			// graph is for). The reference path is therefore chained: each partial path's end starts the next search, and the lengths add.
			const knav::NavQuery plain(corridor);
			knav::PathResult direct;
			Point from = hub;
			for (int hop = 0; hop < 8; ++hop)
			{
				const knav::PathResult part = plain.findPath(from, there, kReach);
				direct.found = part.found;
				direct.complete = part.complete;
				direct.length += part.length;
				if (part.complete || part.failure != knav::PathResult::Failure::OutOfNodes || part.points.empty())
					break;
				from = part.points.back();
			}
			std::cout << "plain Detour path on the corridor mesh (" << corridor.tiles().size() << " tiles): " << direct.length << "\n";
			test::require(direct.complete, "no direct path on the corridor mesh");
			test::require(route.length >= direct.length * 0.99f && route.length <= direct.length * 1.25f,
				"the coarse route is not within 25% of the plain Detour path");
		}

		// (b) A squad moving along the route stands on walkable ground the whole way.
		test::require(flatDistance(route.positionAt(0.0f), hub) < 40.0f && flatDistance(route.positionAt(route.length), there) < 40.0f,
			"route ends are not at the endpoints");
		int sampled = 0;
		Point previous = route.positionAt(0.0f);
		for (float d = 0.0f; d <= route.length; d += 25.0f)
		{
			const Point at = route.positionAt(d);
			test::require(flatDistance(at, previous) <= 25.0f + 0.5f, "position along the route jumps");
			previous = at;
			const auto [rx, rz] = regionOf(at[0], at[2]);
			// x and z are on the route's straight paths; y is linear between their corners, which can be a
			// long way from the surface (128 units over a 768-unit hump on a 40 degree slope). Detour's
			// nearest polygon is the closest in 3D, so a wrong y picks a neighbour; look for the surface
			// layer by layer instead.
			const knav::NavQuery query(meshes.at({ rx, rz }));
			bool onMesh = false;
			for (float y = at[1] - 400.0f; y <= at[1] + 400.0f && !onMesh; y += 8.0f)
			{
				Point snapped;
				onMesh = query.nearestPoint({ at[0], y, at[2] }, { 3.0f, 4.0f, 3.0f }, snapped) && flatDistance(snapped, at) <= 3.0f;
			}
			test::require(onMesh, "position at " + std::to_string(d) + " of the route is off the navmesh");
			++sampled;
		}
		std::cout << "positions along the route on the navmesh: " << sampled << " of " << sampled << "\n";

		// (c) A point nowhere near the mesh is off it, cleanly.
		const knav::CoarsePath off = graph.findPath(hubMesh, townMesh, ground(kHubX, kHubZ + 400000.0), there, kReach);
		test::require(!off.found && off.failure == knav::CoarsePath::Failure::StartOffMesh, "far-off start should be off the mesh");

		// (d) An island: a region of a cluster with no portal, in any region of the graph (the Hub's own one joined once the door mat kept a stair's slope, kenshi-k16.6.229). Both ends are on the mesh; the graph says no route.
		int islands = 0;
		for (const auto& [id, cluster] : graph.clusters())
		{
			std::set<int> reachable;
			for (const knav::Portal& portal : cluster.portals)
				reachable.insert(portal.node);
			if (reachable.size() == cluster.nodeCount || cluster.nodeCount == 0)
				continue;
			const auto [rx, rz] = regionOf(kworld::kWorldMin + (id.x * knav::kClusterTiles + 1.5) * 256.0,
				kworld::kWorldMin + (id.z * knav::kClusterTiles + 1.5) * 256.0);
			for (std::size_t node = 0; node < cluster.nodeCount && islands == 0; ++node)
			{
				if (reachable.contains(static_cast<int>(node)))
					continue;
				const knav::CoarsePath path = graph.findPath(meshes.at({ rx, rz }), hubMesh, cluster.anchors[node], hub, { 4.0f, 30.0f, 4.0f });
				test::require(!path.found && path.failure == knav::CoarsePath::Failure::NoRoute, "an island has a route");
				++islands;
			}
			if (islands > 0)
				break;
		}
		std::cout << "island found with a clean no-route: " << islands << "\n";
		test::require(islands > 0, "no island region reported no route");

		// (e) A long query: the Hub to the farthest town of the baked regions, averaged.
		const kworld::TownPlacement* farthest = nullptr;
		double farthestDistance = 0.0;
		for (const kworld::TownPlacement& town : towns.placements())
		{
			const auto [tx, tz] = regionOf(town.x, town.z);
			const double distance = std::hypot(town.x - kHubX, town.z - kHubZ);
			if (tx < rx0 || tx > rx1 || tz < rz0 || tz > rz1 || distance <= farthestDistance)
				continue;
			if (graph.findPath(hubMesh, meshes.at({ tx, tz }), hub, ground(town.x, town.z), kReach).found)
			{
				farthest = &town;
				farthestDistance = distance;
			}
		}
		test::require(farthest != nullptr, "no far town is reachable");
		const knav::NavMesh& farMesh = meshes.at(regionOf(farthest->x, farthest->z));
		const Point far = ground(farthest->x, farthest->z);
		constexpr int kRuns = 20;
		step = Clock::now();
		knav::CoarsePath longRoute;
		for (int i = 0; i < kRuns; ++i)
			longRoute = graph.findPath(hubMesh, farMesh, hub, far, kReach);
		std::cout << "long query: Hub to " << farthest->targetID << ", straight " << farthestDistance << ", route " << longRoute.length
			<< ", " << longRoute.legs.size() << " legs, " << millis(step) / kRuns << " ms average of " << kRuns << "\n";
		test::require(longRoute.found, "long route not found");
	} catch (const std::exception& e) {
		std::cerr << "knav_coarse: " << e.what() << "\n";
		return 1;
	}
	std::cout << "knav_coarse ok\n";
	return 0;
}
