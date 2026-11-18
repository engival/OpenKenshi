// Outpost s-IV, floor 0 -> floor 2 on the bake path (kenshi-k16.7.51; the runtime-mesh twin is knav_outpost_floor2, kenshi-k16.6.260).
// The way up is the interior ramp, BUILDING part 42261 (`is stairs`, an `interior` entry whose chance is 0). The part roll took 0
// as "never" (selectParts, kworld/collision.cpp), so the baked floor-0 tiles had no polygon on the ramp and floor 2 (the slab at
// y 1246) was an island; the world bake v15 routes it. Bakes the zone's tiles around s-IV from the game data, like
// knav_longhouse_door: no cache, so it needs no world bake. Asserts a complete path that climbs the ramp and ends on floor 2.

#include "knav/area.hpp"
#include "knav/navmesh.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>

int main()
{
	try
	{
		const std::string dataDir = KENSHI_DATA;
		const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const kworld::ZoneCell cell = kworld::zoneCellOf(67772.7, -71427.9);
		const knav::AreaGeometry area = knav::gatherArea(dataDir, terrain, cell, cell);
		const std::vector<kworld::WorldShape> all = area.flatShapes();
		const knav::NavConfig config = knav::configForWorldZones(cell, cell);
		std::vector<const kworld::WorldShape*> pointers;
		std::set<std::pair<int, int>> tiles;
		for (const kworld::WorldShape& shape : all)
		{
			pointers.push_back(&shape);
			if (shape.max[0] < 67640.0f || shape.min[0] > 67900.0f || shape.max[2] < -71560.0f || shape.min[2] > -71300.0f)
				continue;
			for (const knav::TileCoord& t : knav::tilesTouching(config, shape))
				tiles.insert({ t.x, t.z });
		}
		std::vector<knav::TileCoord> coords;
		for (const auto& [x, z] : tiles)
			coords.push_back({ x, z });
		const knav::NavMesh mesh = knav::NavMesh::fromTiles(config, knav::bakeTileSet(config, terrain, pointers, coords, 8));
		const knav::NavQuery query(mesh);

		// the playtest's case: the Wanderer inside s-IV at floor 0, the goal in the floor-2 bunk room
		const knav::Point goal{ 67724.0f, 1246.29f, -71480.0f };
		knav::Point start{}, floor2{};
		test::require(query.nearestPoint({ 67772.7f, 1203.4f, -71427.9f }, { 3.0f, 4.0f, 3.0f }, start), "the floor-0 start is on the mesh");
		test::require(query.nearestPoint(goal, { 3.0f, 4.0f, 3.0f }, floor2), "the floor-2 bunk room is on the mesh");
		const knav::PathResult path = query.findPath(start, floor2, { 20.0f, 8.0f, 20.0f });
		std::cout << "floor 0 -> floor 2: " << (path.complete ? "path" : "NO PATH") << ", " << path.points.size() << " points\n";
		test::require(path.complete, "a complete path from floor 0 to the floor-2 bunk room");
		const knav::Point& end = path.points.back();
		test::require(end[1] > 1240.0f && end[1] < 1252.0f, "the path does not end on floor 2");
		float top = 0.0f;
		for (const knav::Point& p : path.points)
			top = std::max(top, p[1]);
		test::require(top < 1262.0f, "the path climbs past floor 2 (onto the roof slab)");
		test::require(path.points.size() > 2, "the path is a straight line: it does not climb the ramp");
		std::cout << "knav_outpost_floor2_bake ok (end y " << end[1] << ", top y " << top << ")\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "knav_outpost_floor2_bake: " << e.what() << "\n";
		return 1;
	}
}
