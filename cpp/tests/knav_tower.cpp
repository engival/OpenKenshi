// Several floors (docs/formats/NAVMESH_CACHE.md "Positions on several floors"): the Storm Watchtower
// (BUILDING 4104-D-TwoStorey.mod, about 285 units tall, four floors, a winding staircase) placed on flat
// open ground through NavWorld::setBuilding, i.e. a dynamic tile rebuild. Checks that every floor and the
// stair bake walkable and connected, that a saved position (x, y, z) re-attaches to the floor it was on
// with a small vertical box, and that the way from the roof to the ground is the stair.

#include "knav/world.hpp"
#include "kworld/collision.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace {

using knav::Point;

// A flat, dry, open site: a 500-unit plateau at y 163.9 (found by scanning x -90000..-20000, z -30000..20000),
// well over water level (100); the NavWorld holds no building or foliage but ours.
constexpr double kSiteX = -85800.0;
constexpr double kSiteZ = -28400.0;
constexpr double kFlatRadius = 240.0; // the tower reaches 135 from its origin; the approach 160 more

// Floor heights above the terrain at the building origin (the tower floors are modelled in its frame).
constexpr float kGround = 5.0f; // base boxes' top
constexpr float kFloor1 = 132.0f;
constexpr float kFloor2 = 188.0f;
constexpr float kRoof = 289.0f;
constexpr float kReattach = 4.0f; // vertical half extent of a re-attach query (NAVMESH_CACHE "Re-attach": a chord midpoint on the winding stair sits up to ~3.5 off the surface)

} // namespace

int main()
{
	try
	{
		const std::string dataDir = KENSHI_DATA;
		const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		double low = 1e9;
		double high = -1e9;
		for (double dz = -kFlatRadius; dz <= kFlatRadius; dz += 18)
			for (double dx = -kFlatRadius; dx <= kFlatRadius; dx += 18)
			{
				low = std::min(low, terrain.height(kSiteX + dx, kSiteZ + dz));
				high = std::max(high, terrain.height(kSiteX + dx, kSiteZ + dz));
			}
		test::require(high - low < 0.5 && low > 140.0, "the test site is not flat dry ground");
		const double siteY = terrain.height(kSiteX, kSiteZ);

		const auto gameData = kworld::loadMergedGameData(dataDir);
		kworld::CollisionLibrary library(dataDir, gameData);
		const kdata::Record* record = gameData.find("4104-D-TwoStorey.mod");
		test::require(record != nullptr && record->name == "Watchtower", "Watchtower record missing");
		kworld::PlacedInstance placed;
		placed.instanceID = "test-watchtower";
		placed.buildingID = record->stringID;
		placed.scale = record->floatOr("scale", 1.0f);
		placed.x = kSiteX;
		placed.y = siteY;
		placed.z = kSiteZ;
		std::vector<kworld::WorldShape> shapes;
		kworld::CollisionCoverage coverage;
		const auto nav = library.addBuilding(placed, shapes, coverage); // with the interior floors and stairs
		test::require(nav && shapes.size() > 40, "the Watchtower has no collision");

		// Dynamic rebuild: bake the empty site, then place the tower and rebuild the dirty tiles.
		const kworld::ZoneCell cell = kworld::zoneCellOf(kSiteX, kSiteZ);
		knav::NavWorld world(knav::configForZones(cell, cell), terrain,
			std::make_shared<const std::vector<kworld::WorldShape>>());
		world.bakeAll(4);
		const std::vector<knav::BakedTile> empty = world.mesh().tiles();
		world.setBuilding("tower", std::make_shared<const std::vector<kworld::WorldShape>>(std::move(shapes)), *nav);
		test::require(world.dirtyTileCount() > 0, "placing the tower dirtied no tile");
		auto job = world.prepareRebuild();
		job->run(4);
		world.apply(*job);
		std::cout << "tower rebuild: " << job->tiles().size() << " tiles\n";

		const knav::NavQuery query(world.mesh());
		const auto at = [&](double x, double y, double z)
		{
			return Point{ static_cast<float>(kSiteX + x), static_cast<float>(siteY + y), static_cast<float>(kSiteZ + z) };
		};
		const Point reach = { 6.0f, kReattach, 6.0f };
		const Point outside = at(0, 0, 160);

		// (a) The ground outside reaches every floor (the stair is the only way up: floors are 120 apart,
		// the climb is 5).
		struct Target
		{
			const char* name;
			Point at;
		};
		const Target targets[] = {
			{ "ground floor", at(0, kGround, 0) }, { "floor 1", at(0, kFloor1, -20) },
			{ "floor 2", at(0, kFloor2, -20) }, { "roof", at(-20, kRoof, -20) },
		};
		knav::PathResult toRoof;
		for (const Target& target : targets)
		{
			const knav::PathResult path = query.findPath(outside, target.at, reach);
			std::cout << target.name << ": " << (path.complete ? "path" : "NO PATH") << ", length " << path.length
				<< ", " << path.points.size() << " corners\n";
			test::require(path.complete, std::string("no path from the ground outside to the ") + target.name);
			if (target.at[1] - static_cast<float>(siteY) > kRoof - 1.0f)
				toRoof = path;
		}
		test::require(toRoof.complete, "no path to the roof");

		// (c) The way down from the roof is the stair: the same walk in reverse, climbing through every
		// 30-unit band of height with no step above the climb, and far longer than the straight line.
		const knav::PathResult down = query.findPath(targets[3].at, outside, reach);
		test::require(down.complete, "no path from the roof to the ground");
		std::vector<bool> band(10, false);
		float biggestDrop = 0.0f;
		for (std::size_t i = 0; i < down.points.size(); ++i)
		{
			const float height = down.points[i][1] - static_cast<float>(siteY);
			band[static_cast<std::size_t>(std::clamp(static_cast<int>(height / 30.0f), 0, 9))] = true;
			if (i > 0)
				biggestDrop = std::max(biggestDrop, std::fabs(down.points[i][1] - down.points[i - 1][1]));
		}
		for (std::size_t b = 0; b < 9; ++b) // the roof band (270..300) is the last
			test::require(band[b], "the way down skips the height band " + std::to_string(b * 30));
		std::cout << "roof to ground: length " << down.length << ", largest height change between corners " << biggestDrop
			<< "\n";
		test::require(biggestDrop < 40.0f, "the way down jumps between floors");
		test::require(down.length > 2.5f * 300.0f, "the way down is too short to be the winding stair");

		// (b) Re-attach: a saved position (x, y, z) on a landing or mid-stair, found again with a vertical
		// half extent of 4, lands on its own floor. Positions: every corner and every midpoint of the way
		// down (a midpoint is on the chord, so up to ~3.5 off a winding flight). Stacked: points where the mesh has another floor or flight within 150 above or below.
		int checked = 0;
		int stacked = 0;
		int wrongWithTallBox = 0;
		std::vector<Point> saved;
		for (std::size_t i = 0; i < down.points.size(); ++i)
		{
			saved.push_back(down.points[i]);
			if (i + 1 < down.points.size())
				saved.push_back({ (down.points[i][0] + down.points[i + 1][0]) / 2, (down.points[i][1] + down.points[i + 1][1]) / 2,
					(down.points[i][2] + down.points[i + 1][2]) / 2 });
		}
		saved.push_back(at(0, kFloor1, -20));
		saved.push_back(at(0, kFloor2, -20));
		saved.push_back(at(0, kGround, 0));
		for (const Point& position : saved)
		{
			knav::PolyLoc found;
			test::require(query.locate(position, { 2.0f, kReattach, 2.0f }, found), "a saved position does not re-attach: "
				+ std::to_string(position[0] - kSiteX) + " " + std::to_string(position[1] - siteY) + " " + std::to_string(position[2] - kSiteZ));
			// The box rejects hits outside it, so this also holds the saved y (midpoints are interpolated, not exact).
			test::require(std::fabs(found.point[1] - position[1]) <= kReattach + 1e-3f, "a saved position re-attached to another floor: wanted y "
				+ std::to_string(position[1] - siteY) + ", got " + std::to_string(found.point[1] - siteY));
			++checked;
			bool other = false;
			for (float offset = 25.0f; offset <= 150.0f && !other; offset += 5.0f)
				for (float sign : { -1.0f, 1.0f })
				{
					Point probe = position;
					probe[1] += sign * offset;
					Point ignored;
					if (query.nearestPoint(probe, { 2.0f, 2.5f, 2.0f }, ignored))
						other = true;
				}
			stacked += other;
			// The trap: with a tall box the nearest polygon in 3D can be the floor above or below.
			Point tall;
			if (query.nearestPoint(position, { 2.0f, 150.0f, 2.0f }, tall) && std::fabs(tall[1] - position[1]) > 25.0f)
				++wrongWithTallBox;
		}
		std::cout << "re-attach: " << checked << " positions, " << stacked << " with another floor or flight within 150 above or below; a +-150 box picked another floor for "
			<< wrongWithTallBox << "\n";
		test::require(stacked >= 10, "too few stacked positions: the test would not tell the floors apart");

		// Removing the tower restores the empty site byte for byte.
		world.removeBuilding("tower");
		auto restore = world.prepareRebuild();
		test::require(restore != nullptr, "removing the tower dirtied nothing");
		restore->run(4);
		world.apply(*restore);
		const std::vector<knav::BakedTile>& now = world.mesh().tiles();
		test::require(now.size() == empty.size(), "tile count after removal");
		for (std::size_t i = 0; i < now.size(); ++i)
			test::require(now[i].x == empty[i].x && now[i].z == empty[i].z && now[i].data == empty[i].data,
				"tiles differ from the empty site after removing the tower");
		std::cout << "knav_tower OK\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "knav_tower FAILED: " << e.what() << "\n";
		return 1;
	}
}
