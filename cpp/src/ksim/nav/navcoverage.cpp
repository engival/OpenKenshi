// Streaming navmesh coverage (docs/formats/NAVMESH_CACHE.md "Streaming coverage"): the MeshNavigator of a world bake holds the zones of
// the full-detail areas, and the doors and containers of newly covered zones get their entities.

#include "ksim/core/simulation.hpp"

#include "ksim/world/interact.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/world/region.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace ksim {

void Simulation::syncNavCoverage(bool spread)
{
	if (navigation_ == nullptr || !navigation_->streaming() || regions_ == nullptr)
		return;
	if (navigation_->resident())
	{
		// The whole mesh is loaded (kenshi-k16.7.204): no zone comes or goes, only the doors and containers of the zones are furnished.
		navCoverageNow_ = false;
		promotionSaved_.nav = false;
		furnishNavCoverage();
		return;
	}
	if (navCoverageNow_)
	{
		navCoverageNow_ = false;
		spread = false;
	}
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
	const auto addRegion = [&](std::int32_t region)
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
	};
	for (const Area& area : areas)
	{
		for (const std::int32_t region : area.regions)
			addRegion(region);
	}
	if (!spread && promotionSaved_.nav)
	{
		// A loaded save's mesh holds the zones it held (a spread load may have been part way); the rest follow as in the straight run.
		std::vector<kworld::ZoneCell> held;
		for (const std::uint32_t zone : promotionSaved_.zones)
			held.push_back(kworld::zoneCellOfIndex(static_cast<int>(zone)));
		navigation_->setCoverage(held);
		promotionSaved_.nav = false;
		furnishNavCoverage();
		return;
	}
	if (!spread)
	{
		navigation_->setCoverage(wanted);
		furnishNavCoverage();
		return;
	}
	// One step a tick: a zone's doors and containers are furnished on the tick after it loads (each is ~10-15 ms).
	for (const kworld::ZoneCell zone : navigation_->coveredZones())
	{
		if (furnishedZones_.count(kworld::zoneIndexOf(zone)) == 0)
		{
			furnishNavCoverage();
			return;
		}
	}
	const std::size_t areaZones = wanted.size();
	// Prefetch: the area the player walks into, from the heading (stepPlayerAreas), once past the middle of the region: its zones come last.
	const state::PlayerAreas* centres = world_.ecs().get<state::PlayerAreas>();
	if (navPrefetch_ && travelKnown_ && centres != nullptr && !centres->centres.empty())
	{
		const double reach = regions_->config().regionSize * 0.75;
		const std::int32_t ahead = regions_->regionOf(travelX_ + headX_ * reach, travelZ_ + headZ_ * reach);
		const int radius = regions_->config().areaRadiusRegions;
		for (int dz = -radius; dz <= radius; ++dz)
		{
			for (int dx = -radius; dx <= radius; ++dx)
			{
				const int rx = ahead % perAxis + dx;
				const int rz = ahead / perAxis + dz;
				if (rx >= 0 && rz >= 0 && rx < perAxis && rz < perAxis)
					addRegion(rx + perAxis * rz);
			}
		}
	}
	if (wanted.size() > MeshNavigator::kMaxZones)
		wanted.resize(std::max<std::size_t>(areaZones, MeshNavigator::kMaxZones));
	// What stays: the loaded zones some area (or the prefetch) wants. What comes this tick: the nearest missing area zones, then prefetch ones.
	std::set<int> loaded;
	for (const kworld::ZoneCell zone : navigation_->coveredZones())
		loaded.insert(kworld::zoneIndexOf(zone));
	std::vector<kworld::ZoneCell> target;
	std::vector<std::pair<double, kworld::ZoneCell>> missing;
	for (std::size_t i = 0; i < wanted.size(); ++i)
	{
		if (loaded.count(kworld::zoneIndexOf(wanted[i])) != 0)
		{
			target.push_back(wanted[i]);
			continue;
		}
		if (!navigation_->canCover(wanted[i]))
			continue; // no region file (sea, off the bake): it would sit first for ever
		const double zx = kworld::kWorldMin + (wanted[i].x + 0.5) * kworld::kZoneSize;
		const double zz = kworld::kWorldMin + (wanted[i].z + 0.5) * kworld::kZoneSize;
		double nearest = 1e30;
		if (centres != nullptr)
		{
			for (const state::GroundPoint& point : centres->centres)
				nearest = std::min(nearest, std::hypot(point.x - zx, point.z - zz));
		}
		missing.emplace_back(i < areaZones ? nearest : 1e29 + nearest, wanted[i]); // prefetch zones after every area zone
	}
	std::stable_sort(missing.begin(), missing.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	// Drops go first and alone (the cap on resident zones counts the new one); the loads follow, kNavZonesPerTick a tick.
	if (target.size() == loaded.size())
	{
		for (std::size_t i = 0; i < missing.size() && i < kNavZonesPerTick; ++i)
			target.push_back(missing[i].second);
	}
	if (target.size() == loaded.size() && std::all_of(target.begin(), target.end(), [&](kworld::ZoneCell zone) { return loaded.count(kworld::zoneIndexOf(zone)) != 0; }))
		return; // nothing to load or drop
	navigation_->setCoverage(target);
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
