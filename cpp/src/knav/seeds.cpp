#include "knav/seeds.hpp"

#include "kworld/coords.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace knav {

std::vector<Point> readSeedsDef(const std::string& navtilesDir)
{
	std::ifstream in(navtilesDir + "/seeds.def", std::ios::binary);
	const std::vector<char> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	std::vector<Point> seeds(bytes.size() / sizeof(Point));
	std::memcpy(seeds.data(), bytes.data(), seeds.size() * sizeof(Point));
	return seeds;
}

std::vector<Point> zoneSeeds(const kworld::Heightmap& terrain, const kworld::AreaGeometry& area, const std::vector<Point>& seedsDef,
	kworld::ZoneCell min, kworld::ZoneCell max, float seaLevel)
{
	constexpr double kProbeInset = 1.0;
	constexpr float kRayStart = 100.0f; // the probe's ray starts this far above it
	const auto underBuilding = [&](double x, float y, double z)
	{
		for (const kworld::AreaBuilding& building : area.buildings)
			for (std::size_t i = 0; building.shapes && i < building.shapes->size(); ++i)
			{
				const kworld::WorldShape& shape = (*building.shapes)[i];
				if (x >= shape.min[0] && x <= shape.max[0] && z >= shape.min[2] && z <= shape.max[2]
					&& shape.max[1] >= y && shape.min[1] <= y + kRayStart)
					return true;
			}
		return false;
	};

	std::vector<Point> seeds;
	for (int cz = min.z; cz <= max.z; ++cz)
	{
		for (int cx = min.x; cx <= max.x; ++cx)
		{
			bool manual = false;
			for (const Point& seed : seedsDef)
			{
				if (kworld::zoneCellOf(seed[0], seed[2]) != kworld::ZoneCell{ cx, cz })
					continue;
				manual = true;
				const float y = seed[1] > 0.0f ? seed[1] : static_cast<float>(terrain.height(seed[0], seed[2]));
				seeds.push_back({ seed[0], y, seed[2] });
			}
			if (manual)
				continue;
			const double step = (kworld::kZoneSize - 2 * kProbeInset) / 2.0;
			for (int j = 0; j < 3; ++j)
			{
				for (int i = 0; i < 3; ++i)
				{
					const double x = kworld::zoneMinX(cx) + kProbeInset + i * step;
					const double z = kworld::zoneMinZ(cz) + kProbeInset + j * step;
					const float y = std::max(static_cast<float>(terrain.height(x, z)), seaLevel);
					if (!underBuilding(x, y, z))
						seeds.push_back({ static_cast<float>(x), y, static_cast<float>(z) });
				}
			}
		}
	}
	return seeds;
}

std::vector<Point> buildingSeeds(const kworld::AreaGeometry& area)
{
	std::vector<Point> seeds;
	for (const kworld::AreaBuilding& building : area.buildings)
		if (building.nav.pathMode == kworld::PathMode::Walkable)
			seeds.push_back({ static_cast<float>(building.placed.x), static_cast<float>(building.placed.y),
				static_cast<float>(building.placed.z) });
	return seeds;
}

std::vector<DoorSeed> doorSeedsOf(const kworld::AreaGeometry& area)
{
	std::vector<DoorSeed> doors;
	for (const kworld::AreaBuilding& building : area.buildings)
		if (!building.nav.doors.empty())
			doors.push_back({ building.nav.doors.front(), building.placed.x, building.placed.z });
	return doors;
}

std::vector<Point> doorSeeds(const NavMesh& mesh, const std::vector<DoorSeed>& doors, float seaLevel)
{
	constexpr float kStandOff = 15.0f;
	std::vector<Point> seeds;
	for (const DoorSeed& door : doors)
	{
		Point centre;
		NavMesh::DoorFootprint footprint;
		if (!mesh.doorCentre(door.door, centre, &footprint) || centre[1] < seaLevel)
			continue;
		float normalX = -footprint.axisZ;
		float normalZ = footprint.axisX;
		if (normalX * (centre[0] - door.originX) + normalZ * (centre[2] - door.originZ) < 0.0)
		{
			normalX = -normalX;
			normalZ = -normalZ;
		}
		seeds.push_back({ centre[0] + normalX * kStandOff, centre[1], centre[2] + normalZ * kStandOff });
	}
	return seeds;
}

} // namespace knav
