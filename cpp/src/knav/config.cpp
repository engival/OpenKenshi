#include "knav/navmesh.hpp"

#include "kworld/coords.hpp"

#include <cmath>
#include <string>

namespace knav
{

NavConfig configForZones(kworld::ZoneCell min, kworld::ZoneCell max)
{
	NavConfig config;
	config.originX = kworld::zoneMinX(min.x);
	config.originZ = kworld::zoneMinZ(min.z);
	config.tilesX = static_cast<int>(std::lround((max.x - min.x + 1) * kworld::kZoneSize / config.tileSize));
	config.tilesZ = static_cast<int>(std::lround((max.z - min.z + 1) * kworld::kZoneSize / config.tileSize));
	config.window = { 0, 0, config.tilesX - 1, config.tilesZ - 1 };
	return config;
}

std::string regionFileName(int rx, int rz)
{
	return "region_" + std::to_string(rx) + "_" + std::to_string(rz) + ".knav";
}

NavConfig configForWorldZones(kworld::ZoneCell min, kworld::ZoneCell max)
{
	NavConfig config;
	config.originX = kworld::kWorldMin;
	config.originZ = kworld::kWorldMin;
	config.tilesX = kworld::kZoneCellsPerAxis * kTilesPerZone;
	config.tilesZ = config.tilesX;
	config.window = { min.x * kTilesPerZone, min.z * kTilesPerZone, (max.x + 1) * kTilesPerZone - 1,
		(max.z + 1) * kTilesPerZone - 1 };
	return config;
}

} // namespace knav
