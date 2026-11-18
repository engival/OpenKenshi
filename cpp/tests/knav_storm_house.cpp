// Storm house interiors (docs/reference/STORM_HOUSE_INTERIOR_LIVE.md, kinspect item 23): the floor of a
// Hub storm house is walkable and reached from outside by the east-side ramp. The floor is three convex
// slabs (Storm_House_01FloorCOL01/02/03) of the base part's collision, 60 units thick, and the Hub houses
// are tilted with the terrain (up to 5 degrees): a footprint fill that ended below the highest slab corner
// removed the whole top face where it is lower, so the bake had almost no interior (see knav/bake.cpp
// walkableTopLow, COLLISION_FORMAT.md). Reads the baked Hub cache (argv[1]).

#include "knav/area.hpp"
#include "knav/navmesh.hpp"
#include "kworld/collision.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/layouts.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace {

using knav::Point;
using kworld::WorldShape;

constexpr const char* kHouse = "20,32/32-Newwworld-INGAME"; // Storm House (3384-TwoStorey.mod) at -51192, 3192
constexpr float kFloorAboveOrigin = 4.5f; // the slab tops, in the building frame
// kinspect: the ramp the walks enter by, from (-51085, 3145) on the ground.
constexpr float kRampStart[2] = { -51085.0f, 3145.0f };
constexpr float kRampMid[2] = { -51115.0f, 3172.0f };

// Highest upward face of a shape over (x, z): the slab top there. False when the shape has none.
bool topY(const WorldShape& shape, double x, double z, double& y)
{
	bool found = false;
	for (std::size_t t = 0; t + 2 < shape.indices.size(); t += 3)
	{
		const float* a = &shape.vertices[3ull * shape.indices[t]];
		const float* b = &shape.vertices[3ull * shape.indices[t + 1]];
		const float* c = &shape.vertices[3ull * shape.indices[t + 2]];
		const double nx = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]);
		const double ny = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
		const double nz = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
		const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
		if (length < 1e-9 || std::fabs(ny) / length < 0.9)
			continue;
		const double d1 = (b[0] - a[0]) * (z - a[2]) - (b[2] - a[2]) * (x - a[0]);
		const double d2 = (c[0] - b[0]) * (z - b[2]) - (c[2] - b[2]) * (x - b[0]);
		const double d3 = (a[0] - c[0]) * (z - c[2]) - (a[2] - c[2]) * (x - c[0]);
		if ((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0))
			continue;
		const double top = a[1] - (nx * (x - a[0]) + nz * (z - a[2])) / ny;
		if (!found || top > y)
		{
			y = top;
			found = true;
		}
	}
	return found;
}

// A floor slab: a closed walkable hull 55..95 units tall (the base part's FloorCOL hulls, tilted).
bool isFloorSlab(const WorldShape& shape)
{
	const float height = shape.max[1] - shape.min[1];
	return shape.use == kworld::NavUse::Walkable && shape.closed && height > 55.0f && height < 95.0f;
}

// Standing on the floor under one of the building's layout furniture pieces (knav_furniture), grown by the agent radius: an
// obstacle there is the furniture, not a hole in the floor. `furniture` holds only those pieces' shapes.
bool underFurniture(const std::vector<WorldShape>& furniture, double x, double y, double z)
{
	const float grow = knav::NavConfig{}.agentRadius;
	for (const WorldShape& shape : furniture)
	{
		if (std::fabs(shape.min[1] - y) < 8.0 && x > shape.min[0] - grow && x < shape.max[0] + grow && z > shape.min[2] - grow
			&& z < shape.max[2] + grow)
			return true;
	}
	return false;
}

// Fraction of the slab tops (sampled every 4 units) that have a walkable polygon within a few units; samples under furniture
// do not count. `samples` counts every slab sample, `free` those not under furniture.
double floorWalkable(const knav::NavQuery& query, const std::vector<WorldShape>& shapes, const std::vector<WorldShape>& furniture,
	int& samples, int& free)
{
	int hit = 0;
	samples = 0;
	free = 0;
	for (const WorldShape& shape : shapes)
	{
		if (!isFloorSlab(shape))
			continue;
		for (double z = shape.min[2]; z <= shape.max[2]; z += 4.0)
			for (double x = shape.min[0]; x <= shape.max[0]; x += 4.0)
			{
				double y;
				if (!topY(shape, x, z, y))
					continue;
				++samples;
				if (underFurniture(furniture, x, y, z))
					continue;
				++free;
				Point out;
				hit += query.nearestPoint({ static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) },
					{ 1.5f, 3.0f, 1.5f }, out);
			}
	}
	return free == 0 ? 0.0 : static_cast<double>(hit) / free;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: knav_storm_house CACHE");
		const auto mesh = knav::NavMesh::load(argv[1]);
		const std::string dataDir = KENSHI_DATA;
		const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const knav::AreaGeometry area = knav::gatherArea(dataDir, terrain, { 19, 31 }, { 21, 33 });
		const knav::NavQuery query(mesh);
		const kdata::GameData gameData = kworld::loadMergedGameData(dataDir);
		kworld::CollisionLibrary library(dataDir, gameData);
		kworld::CollisionCoverage coverage;
		const kworld::InteriorLayouts layouts = kworld::InteriorLayouts::load(dataDir);

		// (a) Every Hub storm house has a walkable interior floor. Before the fix the tilted ones had
		// 3 to 22 % of their slab tops walkable (the untilted ones about 35 %: the rest is under the
		// walls and their erosion); now all have over a quarter.
		int houses = 0;
		for (const knav::AreaBuilding& building : area.buildings)
		{
			const std::string& type = building.placed.buildingID;
			if (type != "3384-TwoStorey.mod" && type != "3793-TwoStorey.mod")
				continue;
			std::vector<WorldShape> furniture;
			if (const kworld::InteriorLayout* layout = layouts.of(building.placed))
				library.addFurniture(building.placed, *layout, furniture, coverage);
			int samples = 0;
			int free = 0;
			const double walkable = floorWalkable(query, *building.shapes, furniture, samples, free);
			std::cout << building.id << " " << type << ": slab samples " << samples << " (" << free << " not under furniture), walkable "
				<< static_cast<int>(walkable * 100) << " %\n";
			test::require(samples > 1000, "a storm house has no floor slabs");
			test::require(walkable > 0.25, "a storm house floor is not walkable");
			++houses;
		}
		test::require(houses >= 5, "too few storm houses in the Hub");

		// (b) Storm House 3384 at -51192, 3192: from outside by the east ramp (kinspect trace
		// (-51085, 1530, 3145) -> (-51115, 1563, 3172) -> floor at origin + 4.5) to the floor.
		const kworld::PlacedInstance* house = nullptr;
		for (const knav::AreaBuilding& building : area.buildings)
			if (building.id == kHouse)
				house = &building.placed;
		test::require(house != nullptr, "the test storm house is not in the Hub");
		const Point start = { kRampStart[0], static_cast<float>(terrain.height(kRampStart[0], kRampStart[1])),
			kRampStart[1] };
		const float floorY = static_cast<float>(house->y) + kFloorAboveOrigin;
		const Point inside = { static_cast<float>(house->x), floorY, static_cast<float>(house->z) };
		const knav::PathResult path = query.findPath(start, inside, { 40.0f, 6.0f, 40.0f });
		std::cout << "ramp walk: complete " << path.complete << " points " << path.points.size() << " length "
			<< path.length << "\n";
		test::require(path.complete, "no path from outside the east ramp onto the storm house floor");
		test::require(std::fabs(path.points.back()[1] - 1577.0f) < 3.0f, "the storm house floor is not at y 1577");
		bool onRamp = false;
		for (const Point& p : path.points)
			onRamp = onRamp || (std::hypot(p[0] - kRampMid[0], p[2] - kRampMid[1]) < 25.0f && std::fabs(p[1] - 1563.0f) < 8.0f);
		test::require(onRamp, "the walk into the storm house does not climb the east ramp");
	} catch (const std::exception& e) {
		std::cerr << "knav_storm_house: " << e.what() << "\n";
		return 1;
	}
	std::cout << "knav_storm_house ok\n";
	return 0;
}
