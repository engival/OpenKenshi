// Water areas and traveller filters, on real terrain with no buildings or foliage (the
// shapes do not matter for the water rule and the bakes stay small):
//  - an island east of the continent (peak 103050, -31878) is unreachable for a dry-only
//    filter, on the plain mesh and in the coarse graph, and reachable for a swimmer;
//  - on a coast in the west (zones 17..18 x 15..16) a swim shortcut exists, and a swimmer's
//    default costs still lean to the land route, while a filter that makes swimming free takes it;
//  - the bake tags polygons with all three areas and their flags match.

#include "knav/coarse.hpp"
#include "knav/navmesh.hpp"
#include "knav/zone_gen.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <DetourNavMesh.h>

#include <array>
#include <cmath>
#include <iostream>
#include <string>

namespace {

using knav::Point;

const Point kReach{ 40.0f, 60.0f, 40.0f };

knav::NavMesh bake(const kworld::Heightmap& terrain, kworld::ZoneCell min, kworld::ZoneCell max)
{
	// terrain only: an empty area per zone through the cutover bake (knav::bakeZone), joined on the world grid
	std::vector<knav::BakedTile> tiles;
	for (int z = min.z; z <= max.z; ++z)
		for (int x = min.x; x <= max.x; ++x)
			for (knav::BakedTile& tile : knav::bakeZone(knav::configForWorldZones({ x, z }, { x, z }), terrain, kworld::AreaGeometry{}, {}, 4))
				tiles.push_back(std::move(tile));
	return knav::NavMesh::fromTiles(knav::configForWorldZones(min, max), std::move(tiles));
}

std::array<std::size_t, knav::kAreaCount> countAreas(const knav::NavMesh& mesh)
{
	std::array<std::size_t, knav::kAreaCount> counts{};
	for (const knav::BakedTile& tile : mesh.tiles())
	{
		const dtMeshTile* baked = mesh.detour().getTileAt(tile.x, tile.z, 0);
		for (int i = 0; i < baked->header->polyCount; ++i)
		{
			const dtPoly& poly = baked->polys[i];
			test::require(poly.flags == knav::areaFlag(static_cast<knav::Area>(poly.getArea())), "polygon flags do not match its area");
			++counts[poly.getArea()];
		}
	}
	return counts;
}

// Length of a route per area, sampled every 4 u, and the cost it pays under `travel`: says where two routes part.
void printAreas(const char* name, const knav::NavMesh& mesh, const knav::NavQuery& query, const knav::PathResult& path)
{
	std::array<float, knav::kAreaCount> in{};
	float off = 0.0f;
	for (std::size_t i = 1; i < path.points.size(); ++i)
	{
		const Point a = path.points[i - 1];
		const Point b = path.points[i];
		const float len = std::hypot(b[0] - a[0], b[2] - a[2]);
		const int steps = std::max(1, static_cast<int>(len / 4.0f));
		for (int k = 0; k < steps; ++k)
		{
			const float t = (k + 0.5f) / steps;
			knav::PolyLoc loc;
			if (query.locate({ a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t }, { 8.0f, 40.0f, 8.0f }, loc))
				in[mesh.detour().getTileAt(loc.tileX, loc.tileZ, 0)->polys[loc.poly].getArea()] += len / steps;
			else
				off += len / steps;
		}
	}
	std::cout << "  " << name << ": land " << in[0] << " shallow " << in[1] << " swim " << in[2] << " off-mesh " << off << "\n";
}

} // namespace

int main()
{
	try
	{
		const auto terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");

		// Island: three areas exist, a swimmer crosses, a dry-only traveller cannot.
		const knav::NavMesh sea = bake(terrain, { 52, 23 }, { 55, 26 });
		const auto counts = countAreas(sea);
		std::cout << "sea window: " << sea.tiles().size() << " tiles, " << sea.polyCount() << " polys (land " << counts[0]
			<< ", shallow " << counts[1] << ", swim " << counts[2] << ")\n";
		for (const knav::Area water : { knav::Area::Land, knav::Area::Shallow, knav::Area::Swim })
			test::require(counts[static_cast<std::size_t>(water)] > 0, "a water area has no polygons");
		test::require(counts[static_cast<std::size_t>(knav::Area::DoorOpen)] == 0, "door polygons without any door");

		const Point island{ 103050.0f, 147.0f, -31878.0f };
		const Point mainland{ 101358.0f, 120.0f, -32706.0f };
		const knav::NavQuery query(sea);
		const knav::PathResult swum = query.findPath(mainland, island, kReach);
		const knav::PathResult dry = query.findPath(mainland, island, kReach, knav::TravelFilter::dryOnly());
		std::cout << "island: swimmer " << (swum.complete ? swum.length : -1.0f) << ", dry-only complete " << dry.complete << "\n";
		test::require(swum.complete, "a swimmer cannot reach the island");
		test::require(!dry.complete, "a dry-only filter reached the island");

		const knav::TileRect clusters = knav::CoarseGraph::clustersOf(sea.config().window);
		const knav::CoarseGraph swimmers = knav::CoarseGraph::build(sea, clusters, 4);
		const knav::CoarseGraph walkers = knav::CoarseGraph::build(sea, clusters, 4, knav::TravelFilter::dryOnly());
		const knav::CoarsePath coarseSwim = swimmers.findPath(sea, mainland, island, kReach);
		const knav::CoarsePath coarseDry = walkers.findPath(sea, mainland, island, kReach);
		std::cout << "coarse: swimmer " << (coarseSwim.found ? coarseSwim.length : -1.0f) << " (cost " << coarseSwim.cost
			<< "), dry-only found " << coarseDry.found << " failure " << static_cast<int>(coarseDry.failure) << "\n";
		test::require(coarseSwim.found && coarseSwim.cost > coarseSwim.length * 1.3f, "swimmer's coarse route is missing or not charged for water");
		test::require(!coarseDry.found && coarseDry.failure == knav::CoarsePath::Failure::NoRoute, "the dry-only graph reached the island");
		test::require(walkers.stats().portals < swimmers.stats().portals, "excluding water did not remove portals");
		const knav::CoarseGraph reloaded = knav::CoarseGraph::deserialize(walkers.serialize());
		test::require(reloaded == walkers && reloaded.travel() == knav::TravelFilter::dryOnly(), "the filter does not survive serialisation");

		// Coast: land is not absurdly longer, so the default costs keep the route on land.
		const knav::NavMesh coast = bake(terrain, { 17, 15 }, { 18, 16 });
		const Point from{ -65502.0f, 110.0f, -76032.0f };
		const Point to{ -64962.0f, 110.0f, -77112.0f };
		const knav::NavQuery coastQuery(coast);
		knav::TravelFilter freeSwim;
		freeSwim.cost = { 1.0f, 1.0f, 1.0f };
		const knav::PathResult byDefault = coastQuery.findPath(from, to, kReach);
		const knav::PathResult onLand = coastQuery.findPath(from, to, kReach, knav::TravelFilter::dryOnly());
		const knav::PathResult shortcut = coastQuery.findPath(from, to, kReach, freeSwim);
		std::cout << "coast: default " << byDefault.length << ", dry-only " << onLand.length << ", free swimming " << shortcut.length << "\n";
		test::require(byDefault.complete && onLand.complete && shortcut.complete, "a coast path is incomplete");
		test::require(shortcut.length < onLand.length * 0.9f, "the test coast has no swim shortcut");
		// A few wet steps at the shore may still pay (wading costs 1.5, swimming 3); the route must
		// lean to the land one, not to the free-swimming line.
		test::require(byDefault.length > shortcut.length + 0.4f * (onLand.length - shortcut.length),
			"the default costs took the swim shortcut");

		// The race's water cost scale (PATHFINDING_STATIC.md item 4): a human (7.0) keeps to the land route, a Fishman (0.25) takes the
		// swim shortcut, both polygons of water costing the same per unit.
		knav::TravelFilter human;
		human.setWaterCost(7.0f);
		knav::TravelFilter fishman;
		fishman.setWaterCost(0.25f);
		const knav::PathResult humanPath = coastQuery.findPath(from, to, kReach, human);
		const knav::PathResult fishmanPath = coastQuery.findPath(from, to, kReach, fishman);
		std::cout << "coast by race: human " << humanPath.length << ", fishman " << fishmanPath.length << "\n";
		printAreas("free swim", coast, coastQuery, shortcut);
		printAreas("fishman", coast, coastQuery, fishmanPath);
		printAreas("land", coast, coastQuery, onLand);
		printAreas("human", coast, coastQuery, humanPath);
		test::require(humanPath.complete && fishmanPath.complete, "a race coast path is incomplete");
		test::require(humanPath.length > onLand.length * 0.97f, "a human (water 7.0) took the swim shortcut");
		test::require(fishmanPath.length < shortcut.length + 0.1f * (onLand.length - shortcut.length), "a Fishman (water 0.25) did not take the swim shortcut");
		test::require(fishmanPath.length < humanPath.length * 0.9f, "the races took the same coast route");

		const knav::CoarseGraph coastGraph = knav::CoarseGraph::build(coast, knav::CoarseGraph::clustersOf(coast.config().window), 4);
		const knav::CoarsePath coarse = coastGraph.findPath(coast, from, to, kReach);
		std::cout << "coast coarse: " << (coarse.found ? coarse.length : -1.0f) << " (cost " << coarse.cost << ")\n";
		test::require(coarse.found && coarse.length < onLand.length * 1.25f, "the coarse route is not sane against the land route");
	} catch (const std::exception& e) {
		std::cerr << "knav_water: " << e.what() << "\n";
		return 1;
	}
	std::cout << "knav_water ok\n";
	return 0;
}
