#pragma once

// Line of sight (docs/reference/LINE_OF_SIGHT.md; task AZ): the PhysX collision of the placed buildings as a per-region
// bounding-volume tree (kworld::HullTree), built when a region goes to Full detail and freed when it leaves, and a segment
// query against those hulls and the terrain heightfield. Phase 2 wires two queries: `visible` (the eye-to-eye pair perception
// asks, cached per pair) and `shotBlocked` (a projectile from a muzzle to a body; ranged combat itself is not implemented).
//
// A ray is a straight world segment (eye to eye: eye height +18, LINE_OF_SIGHT.md 2). It is blocked by the first hull or
// terrain hit. Foliage rocks and trunks with a collision file are in it (task k16.13.4); a doorway's leaf is a tree of its own
// that blocks only while its door is shut. A region not at Full detail has no tree and blocks nothing (coarse perception is
// unchanged).

#include "ksim/combat/fighter_state.hpp"

#include "kworld/collision.hpp"
#include "kworld/hull_tree.hpp"
#include "kworld/zones.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kdata {
class GameData;
}
namespace kworld {
class BiomeMap;
class Features;
class Heightmap;
class Towns;
}

namespace ksim {

class LineOfSight
{
public:
	struct Hit
	{
		bool blocked = false;
		double t = 0.0; // the first hit as a fraction of the segment (0 at from, 1 at to)
		std::array<double, 3> x{}; // the hit point
		bool hull = false; // true: a collision hull; false: the terrain
	};

	// `dataDir` reads the collision files and the heightmap; `placements` are the world's placed buildings (World::placements);
	// the region grid is the RegionEngine's (regionSize, regionsPerAxis).
	LineOfSight(std::string dataDir, const kdata::GameData& gameData, const std::vector<kworld::PlacedInstance>& placements,
		std::int32_t regionsPerAxis, double regionSize);
	~LineOfSight();

	LineOfSight(const LineOfSight&) = delete;
	LineOfSight& operator=(const LineOfSight&) = delete;

	std::int32_t regionOf(double x, double z) const;
	// The region's world xz rectangle.
	void regionBounds(std::int32_t region, double& x0, double& z0, double& x1, double& z1) const;

	// Builds the region's tree from the placements near it (and any shape that spans into it) when absent; frees it when
	// `keep` is false. A region the world places nothing in keeps an (empty) tree: still loaded, zero triangles.
	void ensure(std::int32_t region);
	void drop(std::int32_t region);
	void clear();
	bool loaded(std::int32_t region) const { return regions_.count(region) != 0; }
	std::size_t loadedRegions() const noexcept { return regions_.size(); }
	std::size_t memoryBytes() const noexcept; // hull triangles + BVH nodes (the placements and the terrain are not counted)
	std::size_t triangles(std::int32_t region) const;
	// The region's tree (null when not loaded): for tests and the physics work later.
	const kworld::HullTree* tree(std::int32_t region) const;
	// How many rays consulted the region's tree (the across-the-border test).
	std::uint64_t regionQueries(std::int32_t region) const;
	std::int32_t regionsPerAxis() const noexcept { return regionsPerAxis_; }
	double regionSize() const noexcept { return regionSize_; }

	// The segment from -> to: the first hit of the hulls of every loaded region it crosses in xz, or of the terrain. The
	// terrain is sampled every kTerrainStep and bisected (kTerrainRefines) to the crossing.
	Hit trace(const std::array<double, 3>& from, const std::array<double, 3>& to) const;
	// The same segment against the walkable shapes (floors, stairs, ramps: the BUILDING path mode Walkable) and the terrain only: no
	// furniture, counters, walls or doors. What the client rays a body onto (the height sweep's level check).
	Hit traceGround(const std::array<double, 3>& from, const std::array<double, 3>& to) const;

	// ---- phase 2: perception and ranged fire ----
	// Eye height over a character's feet (perception.hpp: the original's ray starts at eye +18; LINE_OF_SIGHT.md 2).
	static constexpr double kEyeHeight = 18.0;
	// The pair cache (OURS: the original re-casts): an answer is reused while both ends stand at exactly the spot it was cast
	// at (a tolerance broke the save/load hash fixtures) and it is under kPairTicks 10 sim ticks old. A tree change (a region
	// loaded, unloaded or rebuilt) drops the cache. The cap stops a long session's map from growing without bound.
	static constexpr std::uint64_t kPairTicks = 10;
	static constexpr std::size_t kPairCacheMax = 32768;

	// A door's state for sight (k16.13.4): its leaf blocks while it is shut or locked and passes while it is open or broken. A door
	// never told is shut (the sim's default). A change of a door that has a leaf in a loaded tree drops the pair cache whole (a pair
	// across any door may change; simpler than finding the pairs).
	void setDoorOpen(kworld::DoorKey key, bool open);
	bool doorOpen(kworld::DoorKey key) const;
	// The leaf of a door in a loaded region's tree (its triangle count; 0 = none): for the tests and the tooling.
	std::size_t doorLeafTriangles(kworld::DoorKey key) const;

	// The sim tick the pair cache ages against (Simulation::stepCombat sets it once a tick).
	void setTick(std::uint64_t tick) const noexcept { tick_ = tick; }
	// Drops every cached pair: the trees changed under it (a region loaded, unloaded or was rebuilt), so no answer is
	// reusable. ensure() and drop() call it when they actually change the set (a sync that changes nothing keeps the cache).
	void forgetPairs() const noexcept { pairs_.clear(); }
	// Perception's answer: an eye (+18) to eye ray between two characters, cached per pair. True when nothing blocks it,
	// and whenever either character stands in a region without a tree (not Full detail): coarse perception is unchanged.
	bool visible(std::uint64_t observer, const state::Fighter& a, std::uint64_t target, const state::Fighter& b) const;
	// A projectile from `muzzle` to the target's body (eye height: there is no capsule; OURS / UNSURE). True when a hull or
	// the terrain is in the way. Ranged combat itself is not implemented (FEATURES "Crossbows"), so this is the query it
	// will use, not a shot.
	bool shotBlocked(const std::array<double, 3>& muzzle, const state::Fighter& target) const;
	// The first thing in a shot's way (ranged fire, task ranged; dt41_A `1404346d0`): the hit of the segment muzzle -> the target's body,
	// `blocked` false when it is clear or the target's region has no tree. `t` and `x` say where it stopped, so the caller can tell a wall
	// short of the target from one beyond it.
	Hit firstBlocker(const std::array<double, 3>& muzzle, const state::Fighter& target) const;

	std::uint64_t rays() const noexcept { return rays_; }
	std::uint64_t regionsBuilt() const noexcept { return built_; }
	// Sight pairs cast (the cache's misses) and answered from the cache.
	std::uint64_t pairCasts() const noexcept { return pairCasts_; }
	std::uint64_t pairHits() const noexcept { return pairHits_; }

	static constexpr double kTerrainStep = 8.0; // world units between heightfield samples
	static constexpr int kTerrainRefines = 8;
	// A placement this far outside a region can still reach into it: the collision of a big building (the widest parts are
	// ~300 u; OURS, a guard against clipping a wall that straddles the border).
	static constexpr double kPlacementMargin = 512.0;

private:
	// One doorway's leaf: its own tree, so a trace skips it while the door is open.
	struct DoorLeaf
	{
		kworld::DoorKey key = 0;
		kworld::HullTree tree;
	};
	struct Region
	{
		kworld::HullTree tree;
		kworld::HullTree ground; // the Walkable shapes alone: what a character's ground ray lands on (traceGround)
		std::vector<DoorLeaf> doors;
		std::size_t placed = 0; // the placements that contributed shapes
	};
	struct PairAnswer
	{
		std::array<double, 6> ends{}; // ax, ay, az, bx, by, bz at the cast
		std::uint64_t tick = 0;
		bool visible = true;
	};

	std::string dataDir_;
	const kdata::GameData& gameData_;
	const std::vector<kworld::PlacedInstance>& placements_;
	Hit traceImpl(const std::array<double, 3>& from, const std::array<double, 3>& to, bool groundOnly) const;
	std::int32_t regionsPerAxis_ = 1;
	double regionSize_ = 4608.0;
	std::unique_ptr<kworld::CollisionLibrary> library_;
	std::unique_ptr<kworld::Heightmap> terrain_;
	std::map<std::int32_t, std::vector<std::uint32_t>> byRegion_; // placement indices near a region (built once, kPlacementMargin)
	std::map<std::int32_t, Region> regions_;
	std::unordered_map<kworld::DoorKey, bool> doorOpen_; // door key -> open (absent: shut)
	// The foliage scatter's inputs, loaded with the first region that needs them (a bare world never pays for them).
	struct FoliageSource;
	std::unique_ptr<FoliageSource> foliage_;
	mutable std::map<std::int32_t, std::uint64_t> queries_; // rays that consulted a region's tree
	mutable std::uint64_t rays_ = 0;
	std::uint64_t built_ = 0;
	mutable std::uint64_t tick_ = 0;
	mutable std::map<std::pair<std::uint64_t, std::uint64_t>, PairAnswer> pairs_; // key: the two ids, smaller first
	mutable std::uint64_t pairCasts_ = 0;
	mutable std::uint64_t pairHits_ = 0;
};

} // namespace ksim
