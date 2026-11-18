// Streaming navmesh coverage (docs/formats/NAVMESH_CACHE.md "Streaming coverage"): the MeshNavigator of a world bake holds the zones of
// the full-detail areas, and the doors and containers of newly covered zones get their entities.

#include "ksim/core/simulation.hpp"

#include "ksim/world/interact.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/world/region.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"

#include <cmath>
#include <set>
#include <vector>

namespace ksim {

void Simulation::syncNavCoverage()
{
	if (navigation_ == nullptr || !navigation_->streaming() || regions_ == nullptr)
		return;
	const std::vector<Area>& areas = regions_->areas();
	if (areas.empty())
	{
		furnishNavCoverage(); // nobody plays: what is loaded stays loaded (a NEW_GAME may have loaded its start)
		return;
	}
	// The zones of every area region (a region is one zone or a block of them), in area order (areas are sorted by region): when more than
	// MeshNavigator::kMaxZones are wanted the later ones are left out.
	const int perRegion = std::max(1, static_cast<int>(std::lround(regions_->config().regionSize / kworld::kZoneSize)));
	const int perAxis = regions_->regionsPerAxis();
	std::vector<kworld::ZoneCell> wanted;
	std::set<int> seen;
	for (const Area& area : areas)
	{
		for (const std::int32_t region : area.regions)
		{
			for (int dz = 0; dz < perRegion; ++dz)
			{
				for (int dx = 0; dx < perRegion; ++dx)
				{
					const kworld::ZoneCell zone{(region % perAxis) * perRegion + dx, (region / perAxis) * perRegion + dz};
					if (seen.insert(kworld::zoneIndexOf(zone)).second)
						wanted.push_back(zone);
				}
			}
		}
	}
	navigation_->setCoverage(wanted);
	furnishNavCoverage();
}

void Simulation::furnishNavCoverage()
{
	if (navigation_ == nullptr || !navigation_->streaming())
		return;
	bool fresh = false;
	for (const kworld::ZoneCell zone : navigation_->coveredZones())
		fresh = furnishedZones_.insert(kworld::zoneIndexOf(zone)).second || fresh;
	if (!fresh)
		return;
	// spawnMeshDoors / spawnMeshContainers skip what has an entity already (a zone that was dropped and comes back, a load).
	spawnMeshDoors(*this, *navigation_);
	const kdata::Record* contents = nullptr;
	if (gameData_ != nullptr)
	{
		for (const kdata::Record* record : gameData_->byType(kworld::itemTypeByName("VENDOR_LIST")))
		{
			if (record->name == "house contents")
				contents = record;
		}
	}
	spawnMeshContainers(*this, *navigation_, contents);
	spawnMeshItemNodes(*this, *navigation_);
}

} // namespace ksim
