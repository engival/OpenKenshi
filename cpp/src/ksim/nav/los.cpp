// Line of sight phase 1 (ksim/nav/los.hpp; task AZ): per-region collision hull trees and the segment query.

#include "ksim/nav/los.hpp"

#include "kworld/coords.hpp"
#include "kworld/features.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/towns.hpp"

#include <algorithm>
#include <cmath>

namespace ksim {

namespace {

// The xz region index along one axis (RegionEngine::regionOf's clamping).
std::int32_t axisOf(double value, double origin, double size, std::int32_t count)
{
	double cell = std::floor((value - origin) / size);
	if (cell < 0.0)
		return 0;
	if (cell >= static_cast<double>(count))
		return count - 1;
	return static_cast<std::int32_t>(cell);
}

} // namespace

LineOfSight::LineOfSight(std::string dataDir, const kdata::GameData& gameData,
	const std::vector<kworld::PlacedInstance>& placements, std::int32_t regionsPerAxis, double regionSize)
	: dataDir_(std::move(dataDir))
	, gameData_(gameData)
	, placements_(placements)
	, regionsPerAxis_(std::max<std::int32_t>(regionsPerAxis, 1))
	, regionSize_(regionSize)
	, library_(std::make_unique<kworld::CollisionLibrary>(dataDir_, gameData_))
	, terrain_(std::make_unique<kworld::Heightmap>(kworld::Heightmap::open(dataDir_ + "/newland/land/fullmap.tif")))
{
	// Which placements can reach into which region (origin within the margin of it), once: a region's build then reads only
	// its own list, so an empty region is free.
	for (std::uint32_t i = 0; i < placements_.size(); ++i)
	{
		const kworld::PlacedInstance& placed = placements_[i];
		const std::int32_t xa = axisOf(placed.x - kPlacementMargin, kworld::kWorldMin, regionSize_, regionsPerAxis_);
		const std::int32_t xb = axisOf(placed.x + kPlacementMargin, kworld::kWorldMin, regionSize_, regionsPerAxis_);
		const std::int32_t za = axisOf(placed.z - kPlacementMargin, kworld::kWorldMin, regionSize_, regionsPerAxis_);
		const std::int32_t zb = axisOf(placed.z + kPlacementMargin, kworld::kWorldMin, regionSize_, regionsPerAxis_);
		for (std::int32_t z = za; z <= zb; ++z)
			for (std::int32_t x = xa; x <= xb; ++x)
				byRegion_[x + regionsPerAxis_ * z].push_back(i);
	}
}

struct LineOfSight::FoliageSource
{
	kworld::BiomeMap biomes;
	kworld::Features features;
	kworld::Towns towns;
};

LineOfSight::~LineOfSight() = default;

std::int32_t LineOfSight::regionOf(double x, double z) const
{
	return axisOf(x, kworld::kWorldMin, regionSize_, regionsPerAxis_)
		+ regionsPerAxis_ * axisOf(z, kworld::kWorldMin, regionSize_, regionsPerAxis_);
}

void LineOfSight::regionBounds(std::int32_t region, double& x0, double& z0, double& x1, double& z1) const
{
	const std::int32_t cx = region % regionsPerAxis_;
	const std::int32_t cz = region / regionsPerAxis_;
	x0 = kworld::kWorldMin + static_cast<double>(cx) * regionSize_;
	z0 = kworld::kWorldMin + static_cast<double>(cz) * regionSize_;
	x1 = x0 + regionSize_;
	z1 = z0 + regionSize_;
}

void LineOfSight::ensure(std::int32_t region)
{
	if (regions_.count(region) != 0)
		return;
	double x0 = 0.0, z0 = 0.0, x1 = 0.0, z1 = 0.0;
	regionBounds(region, x0, z0, x1, z1);
	kworld::CollisionCoverage coverage;
	std::vector<kworld::WorldShape> shapes;
	Region built;
	if (const auto near = byRegion_.find(region); near != byRegion_.end())
	{
		for (const std::uint32_t index : near->second)
		{
			const std::size_t before = shapes.size();
			library_->addBuilding(placements_[index], shapes, coverage);
			if (shapes.size() != before)
				++built.placed;
		}
	}
	// The foliage whose collision a ray can hit: the rocks and trunks of the zone cells the region overlaps (the scatter is the
	// exe's, kworld::generateFoliage; the instance's world transform is addFoliage's placeModel). The `navmesh cutter` discs (use
	// Projected, a unit high) are the bake's, not a hull.
	std::vector<kworld::WorldShape> foliageShapes;
	if (foliage_ == nullptr)
		foliage_ = std::make_unique<FoliageSource>(FoliageSource{kworld::BiomeMap::load(dataDir_ + "/newland/land/biomemap.png"),
			kworld::Features::join(gameData_, dataDir_), kworld::Towns::join(gameData_, dataDir_)});
	const kworld::ZoneCell lowCell = kworld::zoneCellOf(x0, z0);
	const kworld::ZoneCell highCell = kworld::zoneCellOf(x1 - 1.0, z1 - 1.0);
	for (int cz = lowCell.z; cz <= highCell.z; ++cz)
		for (int cx = lowCell.x; cx <= highCell.x; ++cx)
			for (const kworld::FoliageInstance& tree : kworld::generateFoliage({cx, cz}, *terrain_, foliage_->biomes, gameData_, foliage_->features, foliage_->towns))
			{
				std::vector<kworld::WorldShape> rock;
				library_->addFoliage(tree, rock, coverage);
				for (kworld::WorldShape& shape : rock)
					if (shape.use != kworld::NavUse::Projected)
						foliageShapes.push_back(std::move(shape));
			}
	// Keep only what can be seen from this region: a shape whose bounds overlap it (a building's wall that straddles the
	// border is in both regions). A doorway's leaf is its own tree (door key 0 is a ruin's doorway: no door, always open).
	std::vector<kworld::WorldShape> kept;
	std::map<kworld::DoorKey, std::vector<kworld::WorldShape>> leaves;
	kept.reserve(shapes.size());
	for (kworld::WorldShape& shape : shapes)
	{
		if (shape.max[0] < x0 || shape.min[0] > x1 || shape.max[2] < z0 || shape.min[2] > z1)
			continue;
		if (shape.use == kworld::NavUse::Door)
		{
			if (shape.door != 0)
				leaves[shape.door].push_back(std::move(shape));
			continue;
		}
		kept.push_back(std::move(shape));
	}
	for (auto& [key, leaf] : leaves)
	{
		built.doors.emplace_back();
		built.doors.back().key = key;
		built.doors.back().tree.build(leaf);
	}
	built.tree.build(kept);
	std::erase_if(foliageShapes, [&](const kworld::WorldShape& shape) { return shape.max[0] < x0 || shape.min[0] > x1 || shape.max[2] < z0 || shape.min[2] > z1; });
	built.foliage.build(foliageShapes);
	kept.erase(std::remove_if(kept.begin(), kept.end(), [](const kworld::WorldShape& shape) { return shape.use != kworld::NavUse::Walkable; }), kept.end());
	built.ground.build(kept);
	regions_.emplace(region, std::move(built));
	++built_;
	pairs_.clear(); // a new tree: the cached answers may change
}

void LineOfSight::drop(std::int32_t region)
{
	if (regions_.erase(region) != 0)
		pairs_.clear(); // a region left Full detail: an answer may have used its tree
}

void LineOfSight::clear()
{
	regions_.clear();
}

std::size_t LineOfSight::memoryBytes() const noexcept
{
	std::size_t bytes = 0;
	for (const auto& [region, entry] : regions_)
	{
		bytes += entry.tree.memoryBytes() + entry.foliage.memoryBytes() + entry.ground.memoryBytes();
		for (const DoorLeaf& leaf : entry.doors)
			bytes += leaf.tree.memoryBytes();
	}
	return bytes;
}

bool LineOfSight::doorOpen(kworld::DoorKey key) const
{
	const auto it = doorOpen_.find(key);
	return it != doorOpen_.end() && it->second;
}

std::size_t LineOfSight::doorLeafTriangles(kworld::DoorKey key) const
{
	std::size_t count = 0;
	for (const auto& [region, entry] : regions_)
		for (const DoorLeaf& leaf : entry.doors)
			if (leaf.key == key)
				count += leaf.tree.triangleCount();
	return count;
}

void LineOfSight::setDoorOpen(kworld::DoorKey key, bool open)
{
	if (doorOpen(key) == open)
		return;
	doorOpen_[key] = open;
	for (const auto& [region, entry] : regions_)
		for (const DoorLeaf& leaf : entry.doors)
			if (leaf.key == key)
			{
				pairs_.clear(); // a pair across this doorway may now differ
				return;
			}
}

std::size_t LineOfSight::triangles(std::int32_t region) const
{
	const auto it = regions_.find(region);
	if (it == regions_.end())
		return 0;
	std::size_t count = it->second.tree.triangleCount() + it->second.foliage.triangleCount();
	for (const DoorLeaf& leaf : it->second.doors)
		count += leaf.tree.triangleCount();
	return count;
}

const kworld::HullTree* LineOfSight::tree(std::int32_t region) const
{
	const auto it = regions_.find(region);
	return it != regions_.end() ? &it->second.tree : nullptr;
}

std::uint64_t LineOfSight::regionQueries(std::int32_t region) const
{
	const auto it = queries_.find(region);
	return it != queries_.end() ? it->second : 0;
}

bool LineOfSight::visible(std::uint64_t observer, const state::Fighter& a, std::uint64_t target, const state::Fighter& b) const
{
	// Only where both stand at Full detail (a tree is loaded): away from it nothing blocks, as perception had it before.
	if (!loaded(regionOf(a.x, a.z)) || !loaded(regionOf(b.x, b.z)))
		return true;
	const auto key = std::minmax(observer, target);
	const auto now = tick_;
	const std::array<double, 6> ends{a.x, a.y, a.z, b.x, b.y, b.z};
	if (const auto it = pairs_.find({key.first, key.second}); it != pairs_.end())
	{
		const PairAnswer& answer = it->second;
		// Exact ends only: a few units of tolerance let a save/load's fresh cache disagree with the straight run's warm one
		// (four hash fixtures failed), because an answer cast at a slightly different position can differ. The pair's scan
		// cadence (perception's 0.5-1.5 s per character) is what keeps the casts down for walkers.
		const bool same = answer.indoors == a.indoors && answer.ends[0] == a.x && answer.ends[1] == a.y && answer.ends[2] == a.z
			&& answer.ends[3] == b.x && answer.ends[4] == b.y && answer.ends[5] == b.z;
		if (same && now >= answer.tick && now - answer.tick < kPairTicks)
		{
			++pairHits_;
			return answer.visible;
		}
	}
	const std::array<double, 3> from{a.x, a.y + kEyeHeight, a.z};
	const std::array<double, 3> to{b.x, b.y + kEyeHeight, b.z};
	const bool open = !trace(from, to, a.indoors ? kSightIndoors : kSightOutdoors).blocked;
	++pairCasts_;
	if (pairs_.size() >= kPairCacheMax)
		pairs_.clear(); // OURS: a long session's cache drops whole rather than growing (rare; the next scan re-casts)
	pairs_[{key.first, key.second}] = PairAnswer{ends, now, open, a.indoors};
	return open;
}

bool LineOfSight::shotBlocked(const std::array<double, 3>& muzzle, const state::Fighter& target) const
{
	return firstBlocker(muzzle, target).blocked;
}

LineOfSight::Hit LineOfSight::firstBlocker(const std::array<double, 3>& muzzle, const state::Fighter& target) const
{
	if (!loaded(regionOf(target.x, target.z)))
		return {}; // no hull tree there: coarse, nothing blocks
	return trace(muzzle, {target.x, target.y + kEyeHeight, target.z}, kGunLine);
}

LineOfSight::Hit LineOfSight::trace(const std::array<double, 3>& from, const std::array<double, 3>& to, Layers layers) const
{
	return traceImpl(from, to, false, layers);
}

LineOfSight::Hit LineOfSight::traceGround(const std::array<double, 3>& from, const std::array<double, 3>& to) const
{
	return traceImpl(from, to, true);
}

LineOfSight::Hit LineOfSight::traceImpl(const std::array<double, 3>& from, const std::array<double, 3>& to, bool groundOnly, Layers layers) const
{
	++rays_;
	Hit hit;
	double best = 1.0;
	bool hullHit = false;
	// The hulls of every loaded region the segment crosses in xz.
	const std::int32_t xa = regionOf(std::min(from[0], to[0]), std::min(from[2], to[2])) % regionsPerAxis_;
	const std::int32_t za = regionOf(std::min(from[0], to[0]), std::min(from[2], to[2])) / regionsPerAxis_;
	const std::int32_t xb = regionOf(std::max(from[0], to[0]), std::max(from[2], to[2])) % regionsPerAxis_;
	const std::int32_t zb = regionOf(std::max(from[0], to[0]), std::max(from[2], to[2])) / regionsPerAxis_;
	for (std::int32_t z = za; z <= zb; ++z)
		for (std::int32_t x = xa; x <= xb; ++x)
		{
			const std::int32_t index = x + regionsPerAxis_ * z;
			const auto it = regions_.find(index);
			if (it == regions_.end())
				continue;
			++queries_[index];
			double t = 0.0;
			if ((groundOnly ? it->second.ground : it->second.tree).trace(from, to, t) && t < best)
			{
				best = t;
				hullHit = true;
			}
			if (!groundOnly && layers.foliage && it->second.foliage.trace(from, to, t) && t < best)
			{
				best = t;
				hullHit = true;
			}
			for (const DoorLeaf& leaf : it->second.doors)
				if (layers.doors && !groundOnly && !doorOpen(leaf.key) && leaf.tree.trace(from, to, t) && t < best)
				{
					best = t;
					hullHit = true;
				}
		}
	// The terrain: sample and bisect to the first crossing.
	const double dx = to[0] - from[0];
	const double dy = to[1] - from[1];
	const double dz = to[2] - from[2];
	const double length = std::sqrt(dx * dx + dz * dz);
	const int steps = std::max(1, static_cast<int>(std::ceil(length / kTerrainStep)));
	auto under = [this, &from, &dy, &dx, &dz](double t)
	{
		const double x = from[0] + dx * t;
		const double y = from[1] + dy * t;
		const double z = from[2] + dz * t;
		return y <= terrain_->terrainHeight(x, z);
	};
	double terrainHit = 2.0;
	for (int i = 1; i <= steps; ++i)
	{
		const double lo0 = static_cast<double>(i - 1) / steps;
		const double t1 = static_cast<double>(i) / steps;
		if (!under(t1))
			continue;
		if (under(lo0))
		{
			terrainHit = lo0; // the start of the sample is already under the ground
			break;
		}
		double lo = lo0;
		double hi = t1;
		for (int refine = 0; refine < kTerrainRefines; ++refine)
		{
			const double mid = 0.5 * (lo + hi);
			if (under(mid))
				hi = mid;
			else
				lo = mid;
		}
		terrainHit = hi;
		break;
	}
	if (hullHit && best <= terrainHit)
	{
		hit.blocked = true;
		hit.t = best;
		hit.hull = true;
	} else if (terrainHit <= 1.0) {
		hit.blocked = true;
		hit.t = terrainHit;
		hit.hull = false;
	}
	if (hit.blocked)
		for (int k = 0; k < 3; ++k)
			hit.x[k] = from[k] + (to[k] - from[k]) * hit.t;
	return hit;
}

} // namespace ksim
