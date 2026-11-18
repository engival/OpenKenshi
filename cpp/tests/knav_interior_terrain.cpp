// BUILDING `interior terrain` in the bake (kenshi-k16.7.18, oracle kenshi-rwh.90, user playtest 2026-11-12 cage_prisoner): "Original kenshi 'carves out'
// the interior of a building against the landscape. So even if the building is sunk, you have access to the entire thing." The slavers' Longhouse
// (4028-TwoStorey.mod, interior terrain 0, a dune rising above its floor) baked its interior under the dune. With the volume of the building's interior
// mask carved out of the terrain, the floor stands alone and joins the door. The same bake with the carve stripped is the "before" (the build under
// test gathers the carve; stripping it from the shapes reproduces the old bake bit for bit). Bakes the Longhouse's six tiles from the game data: no cache.

#include "knav/area.hpp"
#include "knav/navmesh.hpp"
#include "kworld/collision.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

namespace
{

constexpr const char* kLonghouse = "46,16/9-Newwworld-INGAME"; // 4028-TwoStorey.mod at 68339, 1168, -71264 (the Heft slaver camp)
// The user's pick inside it (the client's "walkable no" at the dune), and the floor height there.
constexpr float kPick[3] = { 68328.0f, 1173.0f, -71194.0f };
constexpr double kDoorX = 68412.0;
constexpr double kDoorZ = -71283.0;
constexpr double kFloorTop = 1178.0; // terrain above this inside the volume is the dune (the floor polygons lie at 1174.5-1175)

bool insideVolumes(const std::vector<std::vector<std::array<float, 4>>>& volumes, double x, double y, double z)
{
	for (const auto& planes : volumes)
	{
		bool inside = true;
		for (const auto& p : planes)
			inside = inside && p[0] * x + p[1] * y + p[2] * z <= p[3];
		if (inside)
			return true;
	}
	return false;
}

} // namespace

int main()
{
	try
	{
		const std::string dataDir = KENSHI_DATA;
		const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const kworld::ZoneCell cell = kworld::zoneCellOf(kPick[0], kPick[2]);
		const knav::AreaGeometry area = knav::gatherArea(dataDir, terrain, cell, cell);
		const knav::AreaBuilding* house = nullptr;
		for (const knav::AreaBuilding& building : area.buildings)
			if (building.id == kLonghouse)
				house = &building;
		test::require(house != nullptr, "the Longhouse is in zone 46,16");
		test::require(!house->nav.interiorTerrain && house->shapes && !house->shapes->empty() && (*house->shapes)[0].carveTerrain
			&& !(*house->shapes)[0].carveTerrain->empty(), "the Longhouse (interior terrain 0) carries its interior volume for the bake");
		const std::vector<std::vector<std::array<float, 4>>>& volumes = *(*house->shapes)[0].carveTerrain;
		test::require(volumes.size() == 3, "the Longhouse's mask is three boxes: " + std::to_string(volumes.size()));

		// ---- the same bake with and without the carve ----
		const std::vector<kworld::WorldShape> all = area.flatShapes();
		std::vector<kworld::WorldShape> stripped = all;
		for (kworld::WorldShape& shape : stripped)
			shape.carveTerrain.reset();
		const knav::NavConfig config = knav::configForWorldZones(cell, cell);
		std::set<std::pair<int, int>> tiles;
		for (const kworld::WorldShape& shape : *house->shapes)
			for (const knav::TileCoord& t : knav::tilesTouching(config, shape))
				tiles.insert({ t.x, t.z });
		std::vector<knav::TileCoord> coords;
		for (const auto& [x, z] : tiles)
			coords.push_back({ x, z });
		const auto bake = [&](const std::vector<kworld::WorldShape>& shapes)
		{
			std::vector<const kworld::WorldShape*> pointers;
			for (const kworld::WorldShape& shape : shapes)
				pointers.push_back(&shape);
			return knav::NavMesh::fromTiles(config, knav::bakeTileSet(config, terrain, pointers, coords, 8));
		};
		const knav::NavMesh meshAfter = bake(all);
		const knav::NavMesh meshBefore = bake(stripped);
		const knav::NavQuery after(meshAfter);
		const knav::NavQuery before(meshBefore);

		// (a) the pick on the floor is on the mesh, and reachable from outside through the door
		knav::Point floor{};
		test::require(after.nearestPoint({ kPick[0], kPick[1], kPick[2] }, { 2.0f, 4.0f, 2.0f }, floor) && std::fabs(floor[1] - 1174.5f) < 2.0f,
			"a point on the Longhouse floor (68328, -71194) is on the mesh at y 1174.5");
		knav::Point under{};
		test::require(!before.nearestPoint({ kPick[0], kPick[1], kPick[2] }, { 2.0f, 4.0f, 2.0f }, under),
			"(before the carve the floor point was under the dune: not on the mesh)");
		const double outsideX = 68450.0;
		const double outsideZ = -71260.0; // outside the east door
		const knav::Point start = { static_cast<float>(outsideX), static_cast<float>(terrain.height(outsideX, outsideZ)), static_cast<float>(outsideZ) };
		const knav::PathResult path = after.findPath(start, floor, { 20.0f, 8.0f, 20.0f });
		test::require(path.complete, "no path from outside the Longhouse's east door to its floor");
		// The walk's corners are far apart: the door is passed when a leg of the path comes within 15 u of its centre (xz) and the walk ends on the floor.
		double doorGap = 1.0e9;
		for (std::size_t i = 0; i + 1 < path.points.size(); ++i)
		{
			const knav::Point& a = path.points[i];
			const knav::Point& b = path.points[i + 1];
			const double ex = b[0] - a[0];
			const double ez = b[2] - a[2];
			const double t = std::clamp(((kDoorX - a[0]) * ex + (kDoorZ - a[2]) * ez) / (ex * ex + ez * ez), 0.0, 1.0);
			doorGap = std::min(doorGap, std::hypot(a[0] + t * ex - kDoorX, a[2] + t * ez - kDoorZ));
		}
		test::require(doorGap < 15.0, "the walk to the floor passes the east door (gap " + std::to_string(doorGap) + " u)");
		std::cout << "floor point on the mesh, path " << path.points.size() << " points, " << path.length << " u\n";

		// (a2) The far end of the floor (kenshi-k16.7.24, the client lead's live check: a move to the user's pick (68433.5, 1174.5, -71227.3) ended at
		// (68404.4, 1173.0, -71246.6), 35 u short). The pick lies inside the Longhouse's east block (shape 11 of its collision: a 41 x 22 x 78 box at yaw 43,
		// y 1172-1255, bottom corners (68431.7, -71245.8) (68401.8, -71217.9)) and 8 u outside the mask's box 2; the original's own mesh (interior 8042f reaches
		// x 68416) has no surface there either (knav_compare --at). The floor stops at the block, so the nearest floor point is the walk's end: it is on the floor
		// of the east end, the nearest point of the mesh to the pick (29 u away), and the path from outside the east door to it is complete.
		constexpr double kFarX = 68433.5;
		constexpr double kFarZ = -71227.3;
		knav::Point farFloor{};
		test::require(after.nearestPoint({ static_cast<float>(kFarX), 1174.5f, static_cast<float>(kFarZ) }, { 40.0f, 8.0f, 40.0f }, farFloor)
			&& std::fabs(farFloor[1] - 1173.0f) < 2.0f, "a floor point near the far end (68433.5, -71227.3) is on the mesh");
		const double farGap = std::hypot(farFloor[0] - kFarX, farFloor[2] - kFarZ);
		test::require(farGap < 40.0, "the nearest floor point to the far pick is within 40 u of it (" + std::to_string(farGap) + " u)");
		const knav::PathResult farPath = after.findPath(start, farFloor, { 20.0f, 8.0f, 20.0f });
		test::require(farPath.complete, "no complete path from outside the east door to the far end of the Longhouse floor");
		std::cout << "far end: floor point (" << farFloor[0] << ", " << farFloor[1] << ", " << farFloor[2] << "), " << farGap << " u from the pick, path " << farPath.points.size() << " points, "
			<< farPath.length << " u\n";

		// (b) no polygon on the dune surface inside the interior volume (terrain above the floor, inside the mask boxes)
		int dune = 0;
		int duneAfter = 0;
		int duneBefore = 0;
		for (double z = kPick[2] - 160.0; z <= kPick[2] + 160.0; z += 4.0)
			for (double x = kPick[0] - 160.0; x <= kPick[0] + 160.0; x += 4.0)
			{
				const double y = terrain.height(x, z);
				if (y < kFloorTop || !insideVolumes(volumes, x, y, z))
					continue;
				++dune;
				knav::Point out;
				const knav::Point at = { static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) };
				duneAfter += after.nearestPoint(at, { 1.0f, 1.5f, 1.0f }, out);
				duneBefore += before.nearestPoint(at, { 1.0f, 1.5f, 1.0f }, out);
			}
		std::cout << "dune samples inside the volume: " << dune << ", on the mesh after " << duneAfter << ", before " << duneBefore << "\n";
		test::require(dune >= 50, "the dune rises into the Longhouse's interior volume (" + std::to_string(dune) + " samples)");
		test::require(duneAfter == 0, "terrain polygons remain inside the interior volume: " + std::to_string(duneAfter));
		test::require(duneBefore > 0, "the bake without the carve has no polygon on the dune: the test does not discriminate");

		// (c) `interior terrain` = 1 keeps its terrain: the three base buildings that say so gather no carve; the Longhouse's record does
		const kdata::GameData gameData = kworld::loadMergedGameData(dataDir);
		kworld::CollisionLibrary library(dataDir, gameData);
		for (const char* id : { "40591-Newwworld.mod", "14535-Newwworld.mod", "55108-Newwworld.mod", "4028-TwoStorey.mod" })
		{
			kworld::PlacedInstance placed;
			placed.instanceID = "interior-terrain-test";
			placed.buildingID = id;
			placed.x = kPick[0];
			placed.y = 1168.0;
			placed.z = kPick[2];
			placed.scale = 1.0f;
			std::vector<kworld::WorldShape> shapes;
			kworld::CollisionCoverage coverage;
			const auto nav = library.addBuilding(placed, shapes, coverage, true, false);
			test::require(nav && !shapes.empty(), std::string(id) + " gathers shapes");
			bool carved = false;
			for (const kworld::WorldShape& shape : shapes)
				carved = carved || shape.carveTerrain != nullptr;
			const bool keeps = std::string(id) != "4028-TwoStorey.mod";
			test::require(nav->interiorTerrain == keeps, std::string(id) + " reads `interior terrain`");
			test::require(carved != keeps, std::string(id) + (keeps ? " (interior terrain 1) must keep its terrain" : " (interior terrain 0) must carve it"));
		}
	} catch (const std::exception& e) {
		std::cerr << "knav_interior_terrain: " << e.what() << "\n";
		return 1;
	}
	std::cout << "knav_interior_terrain ok\n";
	return 0;
}
