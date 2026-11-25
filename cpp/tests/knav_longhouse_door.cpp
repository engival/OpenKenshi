// Heft Longhouse entrance stairs (kenshi-k16.6.229, user playtest 2026-11-14 cage_prisoner: "no route to walk into this clearly open building", "the navmesh
// clearly has a gap"). The Longhouse at (68500, -70750), floor 1383: its entrance stairs climb the slope into the doorway. The building's closed walkable hull
// fills its footprint, which nulled the upper spans of the stair that run under the hull; the door mat gave back only the spans within an agent climb of the
// door's floor, so a band of the stair stayed nulled and the stair's polygons stopped a few units short of the doorway's: the interior was an island
// (every move_to on the floor inside REJECTED: no_route). The mat now gives back a stair slope's worth more below the door (bake.cpp kMatStairDrop).
// Bakes the zone's tiles around the entrance from the game data: no cache (the shipped bake needs a re-bake to carry the fix). Asserts a complete path from
// the stairs and from the street below to the floor just inside the doorway.

#include "knav/area.hpp"
#include "knav/navmesh.hpp"
#include "knav/zone_gen.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <string>

int main()
{
	try
	{
		const std::string dataDir = KENSHI_DATA;
		const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const kworld::ZoneCell cell = kworld::zoneCellOf(68500.0, -70750.0);
		const knav::AreaGeometry area = knav::gatherArea(dataDir, terrain, cell, cell);
		const knav::NavConfig config = knav::configForWorldZones(cell, cell);
		// the cutover bake of the zone (knav::bakeZone: ground + surfgen), no cache
		const knav::NavMesh mesh = knav::NavMesh::fromTiles(config, knav::bakeZone(config, terrain, area,
			knav::zoneBakeSeeds(terrain, area, knav::readSeedsDef(dataDir + "/navtiles"), cell, config.waterLevel), 4));
		const knav::NavQuery query(mesh);

		// the floor inside the doorway (the playtest's rejected clicks), from the stairs where the Wanderer stood and from the street at their foot
		const float inside[][3] = { { 68500.0f, 1383.0f, -70747.0f }, { 68497.0f, 1383.0f, -70750.0f }, { 68504.0f, 1383.0f, -70752.0f } };
		const knav::Point starts[] = { { 68521.9f, 1369.1f, -70771.5f }, { 68550.0f, static_cast<float>(terrain.height(68550.0, -70805.0)), -70805.0f } };
		for (const knav::Point& start : starts)
			for (const auto& goal : inside)
			{
				knav::Point floor{};
				test::require(query.nearestPoint({ goal[0], goal[1], goal[2] }, { 3.0f, 4.0f, 3.0f }, floor), "the floor inside the doorway is on the mesh");
				const knav::PathResult path = query.findPath(start, floor, { 20.0f, 8.0f, 20.0f });
				std::cout << "from (" << start[0] << ", " << start[1] << ", " << start[2] << ") to (" << floor[0] << ", " << floor[1] << ", " << floor[2] << "): "
					<< (path.complete ? "path" : "NO PATH") << ", " << path.points.size() << " points\n";
				test::require(path.complete, "a complete path from the entrance stairs / the street to the floor inside the Longhouse");
			}
		std::cout << "knav_longhouse_door ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "knav_longhouse_door: " << e.what() << "\n";
		return 1;
	}
}
