// DEV ONLY (kenshi-k16.7.204): a named zone set for the resident navmesh, so a run or a test that must not load the whole world (some 970 zones)
// loads a few. The server's --nav-zones takes one. Without it the mesh is the whole world (MeshNavigator::worldZones).
// Names: "hub" (the Hub's 3 x 3 zones), "hub5" (5 x 5), or a box "x0,z0,x1,z1" (zone cells, inclusive).
#pragma once

#include "kworld/coords.hpp"
#include "ksim/net/simlink_scenario.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace ksim::dev {

inline std::optional<std::vector<kworld::ZoneCell>> namedZoneSet(const std::string& name)
{
	int x0 = 0, z0 = 0, x1 = -1, z1 = -1;
	if (name == "hub" || name == "hub5")
	{
		const int radius = name == "hub" ? 1 : 2;
		x0 = simlink::kHubZoneX - radius;
		z0 = simlink::kHubZoneZ - radius;
		x1 = simlink::kHubZoneX + radius;
		z1 = simlink::kHubZoneZ + radius;
	} else if (std::sscanf(name.c_str(), "%d,%d,%d,%d", &x0, &z0, &x1, &z1) != 4) {
		return std::nullopt;
	}
	std::vector<kworld::ZoneCell> zones;
	for (int z = z0; z <= z1; ++z)
	{
		for (int x = x0; x <= x1; ++x)
			zones.push_back({x, z});
	}
	if (zones.empty() || x0 < 0 || z0 < 0 || x1 >= kworld::kZoneCellsPerAxis || z1 >= kworld::kZoneCellsPerAxis)
		return std::nullopt;
	return zones;
}

} // namespace ksim::dev
