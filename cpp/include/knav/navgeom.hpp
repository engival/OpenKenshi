#pragma once

// Staleness by agreement (kenshi-k16.7.128, docs/formats/NAVMESH_CACHE.md "Staleness by agreement"): a shipped tile whose hash is
// stale is the original's own mesh of the place, and still right unless the world changed under it. Where our rebake of the same
// tile covers the shipped surface and adds none of its own (knav/compare.hpp's score over the tile's grid), the shipped tile
// stays and our bake is dropped; where they differ, our bake stays. The thresholds are what tiles of fresh-hash zones (shipped
// right by the hash) pass against our bake of them.

#include "knav/compare.hpp"
#include "knav/native.hpp"
#include "knav/navmesh.hpp"

#include <set>
#include <vector>

namespace knav {

struct TileAgreement
{
	TileCoord tile;
	std::size_t nativeKept = 0; // shipped samples beyond kRadius of a shipped wall
	std::size_t nativeHit = 0; // ... within kTol of one of ours
	std::size_t oursTotal = 0;
	std::size_t oursNoNative = 0; // ours with no shipped surface within kTol
	std::size_t bridges = 0; // regions of our surface (>= kRegionCells) that join two or more regions of the shipped surface kept kAgreeRadius off its walls: a connection ours has and the shipped mesh lacks for a body
	std::size_t cuts = 0; // regions (>= kRegionCells) of shipped surface kept kAgreeRadius off its walls that our bake does not cover: a cut ours has (a rock, a node, furniture) and the shipped mesh lacks
	std::size_t offHeight = 0; // shipped samples (kept kAgreeRadius off its walls) whose nearest surface of ours is more than kHeightOff away in height
	std::size_t eroded = 0; // the shipped samples kept kAgreeRadius off its walls
	double covered() const noexcept { return nativeKept == 0 ? 100.0 : 100.0 * static_cast<double>(nativeHit) / static_cast<double>(nativeKept); }
	double surplus() const noexcept { return oursTotal == 0 ? 0.0 : 100.0 * static_cast<double>(oursNoNative) / static_cast<double>(oursTotal); }
	std::size_t misses() const noexcept { return nativeKept - nativeHit + oursNoNative; }
	// The calibrated rule (see NAVMESH_CACHE.md): a tile with few samples is judged by its absolute misses.
	bool agrees() const noexcept;
};

constexpr double kHeightOff = 6.0; // u: a shipped surface this far off ours in height is the same place at another level (a coarse hill, a water plane), not the same floor
constexpr double kAgreeTol = 30.0; // u in height: the shipped ground is coarse in height, so this measures where, not how high (knav_compare)
constexpr double kAgreeRadius = 4.0; // u: shipped surface this near a shipped wall is not compared (our erosion differs there)

// The agreement of each tile in `only` (every tile of `bake` when empty) between the shipped `meshes` and our `bake`.
std::vector<TileAgreement> tileAgreements(const std::vector<native::Mesh>& meshes, const std::vector<BakedTile>& bake,
	const NavConfig& config, const std::set<TileCoord>& only = {}, double tol = kAgreeTol);

} // namespace knav
