// Dynamic tile rebuild: a wall placed across the road outside the Hub's west
// gate, then removed; rebuild determinism across thread counts; build
// threshold and destroyed-navmesh rules. The coarse graph follows: only the
// clusters around the changed tiles are recomputed, the coarse route changes
// with the wall, and removing it restores the graph exactly.

#include "knav/area.hpp"
#include "knav/coarse.hpp"
#include "knav/world.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>

namespace {

using knav::Point;
using Clock = std::chrono::steady_clock;

double millis(Clock::time_point from)
{
	return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

bool sameTiles(const std::vector<knav::BakedTile>& a, const std::vector<knav::BakedTile>& b)
{
	if (a.size() != b.size())
		return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (a[i].x != b[i].x || a[i].z != b[i].z || a[i].data != b[i].data)
			return false;
	return true;
}

} // namespace

int main()
{
	try
	{
		const std::string dataDir = KENSHI_DATA;
		const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const knav::AreaGeometry area = knav::gatherArea(dataDir, terrain, { 19, 31 }, { 21, 33 });
		knav::NavWorld world(knav::configForZones({ 19, 31 }, { 21, 33 }), terrain, area.foliage);
		for (const knav::AreaBuilding& building : area.buildings)
			world.setBuilding(building.id, building.shapes, building.nav);
		world.bakeAll(4);
		const std::vector<knav::BakedTile> baseline = world.mesh().tiles();
		std::cout << "baseline: " << baseline.size() << " tiles, " << world.mesh().polyCount() << " polys\n";
		const knav::TileRect allClusters = knav::CoarseGraph::clustersOf(world.mesh().config().window);
		auto started = Clock::now();
		knav::CoarseGraph graph = knav::CoarseGraph::build(world.mesh(), allClusters, 4);
		const knav::CoarseStats graphStats = graph.stats();
		std::cout << "coarse graph: " << millis(started) << " ms on 4 threads, " << graphStats.clusters << " clusters, "
			<< graphStats.nodes << " nodes, " << graphStats.portals << " portals, " << graphStats.pairs << " intra edges, "
			<< graph.serialize().size() << " bytes\n";
		const std::vector<std::uint8_t> baselineGraph = graph.serialize();
		test::require(knav::CoarseGraph::build(world.mesh(), allClusters, 1).serialize() == baselineGraph,
			"graph differs between 1 and 4 threads");

		// The road outside the west gate (-51562, 2780), walking in towards the Hub.
		const knav::NavQuery query(world.mesh());
		const Point reach = { 40.0f, 60.0f, 40.0f };
		const auto ground = [&](double x, double z)
		{
			return Point{ static_cast<float>(x), static_cast<float>(terrain.height(x, z)), static_cast<float>(z) };
		};
		const Point centre = ground(-50978.8, 2932.48);
		const Point outside = ground(-51562.0 - 0.8 * 120, 2780.0 + 0.55 * 120);
		const knav::PathResult before = query.findPath(outside, centre, reach);
		test::require(before.complete && before.points.size() >= 2, "no baseline path");
		const knav::CoarsePath coarseBefore = graph.findPath(world.mesh(), outside, centre, reach);
		test::require(coarseBefore.found, "no baseline coarse route");
		std::cout << "coarse route: " << coarseBefore.legs.size() << " legs, length " << coarseBefore.length
			<< " (detailed " << before.length << ")\n";

		// Points inside solid buildings are off the mesh: a clean failure, not a route.
		int insideSolids = 0;
		for (const kworld::WorldShape& shape : area.flatShapes())
		{
			if (!shape.closed || shape.max[0] - shape.min[0] < 24 || shape.max[2] - shape.min[2] < 24 || shape.max[1] - shape.min[1] < 16)
				continue;
			const Point middle = { (shape.min[0] + shape.max[0]) / 2, (shape.min[1] + shape.max[1]) / 2, (shape.min[2] + shape.max[2]) / 2 };
			if (query.onMesh(middle, { 2.0f, 3.0f, 2.0f }))
				continue;
			const knav::CoarsePath inside = graph.findPath(world.mesh(), middle, centre, { 2.0f, 3.0f, 2.0f });
			test::require(!inside.found && inside.failure == knav::CoarsePath::Failure::StartOffMesh,
				"a point inside a building did not report off the mesh");
			if (++insideSolids == 20)
				break;
		}
		std::cout << "points inside buildings reporting no route: " << insideSolids << "\n";
		test::require(insideSolids == 20, "too few building interiors tested");

		// A Defensive Wall IV across the first leg, 50 units in.
		const auto gameData = kworld::loadMergedGameData(dataDir);
		kworld::CollisionLibrary library(dataDir, gameData);
		const kdata::Record* wall = gameData.find("2939-Walls2.mod");
		test::require(wall != nullptr, "wall record missing");
		const Point& a = before.points[0];
		const Point& b = before.points[1];
		const double length = std::hypot(b[0] - a[0], b[2] - a[2]);
		const double tx = (b[0] - a[0]) / length;
		const double tz = (b[2] - a[2]) / length;
		const double wx = -tz; // wall direction: perpendicular to the walk
		const double wz = tx;
		const double yaw = std::atan2(wx, wz); // local +z maps to (sin yaw, cos yaw)
		kworld::PlacedInstance placed;
		placed.instanceID = "test-wall";
		placed.buildingID = wall->stringID;
		placed.scale = wall->floatOr("scale", 1.0f);
		const double halfLength = 150.0 * placed.scale; // the wall runs z -11..314 locally
		placed.x = a[0] + tx * 50 - wx * halfLength;
		placed.z = a[2] + tz * 50 - wz * halfLength;
		placed.y = terrain.height(placed.x, placed.z);
		placed.qw = std::cos(yaw / 2);
		placed.qy = std::sin(yaw / 2);
		std::vector<kworld::WorldShape> shapes;
		kworld::CollisionCoverage coverage;
		const auto nav = library.addBuilding(placed, shapes, coverage);
		test::require(nav && !shapes.empty(), "wall has no collision");
		std::cout << "wall: path mode " << static_cast<int>(nav->pathMode) << ", build threshold "
			<< nav->buildThreshold << ", destroyed navmesh " << nav->destroyedNavmesh << ", "
			<< shapes.size() << " shapes\n";
		const auto wallShapes = std::make_shared<const std::vector<kworld::WorldShape>>(std::move(shapes));

		// Build threshold: half built is below it, so nothing changes.
		world.setBuilding("test-wall", wallShapes, *nav, { 0.5f, false });
		test::require(world.dirtyTileCount() == 0, "a half-built wall dirtied tiles");
		world.setBuildingState("test-wall", { 1.0f, false });
		test::require(world.dirtyTileCount() > 0, "a finished wall dirtied nothing");

		started = Clock::now();
		auto job = world.prepareRebuild();
		const double prepareMs = millis(started);
		const std::size_t dirty = job->tiles().size();
		started = Clock::now();
		job->run(1);
		const double singleMs = millis(started);
		const auto single = job->result();
		started = Clock::now();
		job->run(4);
		const double quadMs = millis(started);
		test::require(sameTiles(single, job->result()), "1 and 4 worker threads built different tiles");
		started = Clock::now();
		world.apply(*job);
		const double applyMs = millis(started);
		std::cout << "wall rebuild: " << dirty << " tiles; prepare " << prepareMs << " ms, run 1 thread "
			<< singleMs << " ms, 4 threads " << quadMs << " ms, apply " << applyMs << " ms\n";

		started = Clock::now();
		const std::size_t recomputed = graph.update(world.mesh(), job->tiles(), 4);
		std::cout << "coarse update: " << recomputed << " of " << graphStats.clusters << " clusters in " << millis(started) << " ms\n";
		test::require(recomputed > 0 && recomputed < graphStats.clusters / 10, "the update touched too many clusters");
		test::require(graph.serialize() != baselineGraph, "the wall did not change the graph");
		const knav::CoarsePath coarseDuring = graph.findPath(world.mesh(), outside, centre, reach);
		std::cout << "coarse route length " << coarseBefore.length << " -> " << (coarseDuring.found ? coarseDuring.length : -1.0f) << "\n";
		test::require(coarseDuring.found && coarseDuring.length > coarseBefore.length + 10.0f, "the wall did not change the coarse route");
		test::require(knav::CoarseGraph::build(world.mesh(), allClusters, 4).serialize() == graph.serialize(),
			"the updated graph differs from a fresh build");

		const knav::PathResult during = query.findPath(outside, centre, reach);
		std::cout << "path length " << before.length << " -> " << (during.found ? during.length : -1.0f)
			<< (during.complete ? "" : " (incomplete)") << "\n";
		test::require(!query.onMesh(ground(placed.x + wx * halfLength, placed.z + wz * halfLength),
			{ 3.0f, 30.0f, 3.0f }), "the wall's centre is still walkable");
		test::require(!during.complete || during.length > before.length + 20.0f,
			"the wall did not change the path");
		test::require(!sameTiles(world.mesh().tiles(), baseline), "tiles unchanged by the wall");

		// Destroyed: `destroyed navmesh` set removes it, else it stays.
		world.setBuildingState("test-wall", { 1.0f, true });
		test::require((world.dirtyTileCount() > 0) == nav->destroyedNavmesh,
			"destroyed-navmesh rule not honoured");
		if (world.dirtyTileCount() > 0)
		{
			auto destroyed = world.prepareRebuild();
			destroyed->run(4);
			world.apply(*destroyed);
			test::require(sameTiles(world.mesh().tiles(), baseline), "a destroyed wall left tiles behind");
			world.setBuildingState("test-wall", { 1.0f, false });
			auto rebuilt = world.prepareRebuild();
			rebuilt->run(4);
			world.apply(*rebuilt);
		}

		// Remove: tiles return to the baseline byte for byte.
		world.removeBuilding("test-wall");
		auto restore = world.prepareRebuild();
		test::require(restore != nullptr, "removing the wall dirtied nothing");
		restore->run(4);
		world.apply(*restore);
		graph.update(world.mesh(), restore->tiles(), 4);
		test::require(graph.serialize() == baselineGraph, "the graph differs from the static graph after removal");
		const knav::CoarsePath coarseAfter = graph.findPath(world.mesh(), outside, centre, reach);
		test::require(coarseAfter.found && std::fabs(coarseAfter.length - coarseBefore.length) < 0.01f, "coarse route not restored");
		test::require(sameTiles(world.mesh().tiles(), baseline), "tiles differ from the static bake after removal");
		const knav::PathResult after = query.findPath(outside, centre, reach);
		test::require(after.complete && std::fabs(after.length - before.length) < 0.01f, "path not restored");
	} catch (const std::exception& e) {
		std::cerr << "knav_dynamic: " << e.what() << "\n";
		return 1;
	}
	std::cout << "knav_dynamic ok\n";
	return 0;
}
