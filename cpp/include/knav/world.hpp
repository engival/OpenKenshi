#pragma once

// A navmesh that follows the building set. The static bake (terrain, foliage,
// the buildings that exist at the start) is the baseline; adding, removing or
// changing a building marks the tiles it can reach dirty, and only those are
// rebuilt. A tile is a pure function of the terrain and the shapes that are
// active, so the result does not depend on thread count or order of edits.
// The mesh is derived data: it is never saved at runtime (NAVMESH_CACHE.md).

#include "knav/navmesh.hpp"
#include "knav/seeds.hpp"
#include "kworld/collision.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace knav {

struct BuildingState
{
	float builtFraction = 1.0f; // 0..1
	bool destroyed = false;
};

// Whether a building's shapes are in the navmesh: path mode is not IGNORE, it
// is built past `build threshold` percent, and it is not a destroyed building
// with `destroyed navmesh` set (without it a destroyed building keeps its shapes).
bool affectsNavmesh(const kworld::BuildingNav& nav, const BuildingState& state);

class NavWorld
{
public:
	using Shapes = std::shared_ptr<const std::vector<kworld::WorldShape>>;

	// A snapshot of the dirty tiles and the shapes to rebuild them from.
	// Self-contained: run() may be called on any thread, but not concurrently
	// on the same job. `terrain` must outlive it.
	class Rebuild
	{
	public:
		const std::vector<TileCoord>& tiles() const noexcept { return coords_; }
		// Bakes the tiles; the NavWorld is not touched. May be repeated.
		void run(int threads);
		const std::vector<BakedTile>& result() const noexcept { return result_; }
		bool done() const noexcept { return done_; }

	private:
		friend class NavWorld;
		NavConfig config_;
		const kworld::Heightmap* terrain_ = nullptr;
		std::vector<Shapes> keep_;
		std::vector<const kworld::WorldShape*> shapes_;
		std::vector<TileCoord> coords_;
		std::vector<BakedTile> result_;
		bool done_ = false;
	};

	NavWorld(const NavConfig& config, const kworld::Heightmap& terrain, Shapes fixedShapes);

	// Edits only change the building set and the dirty-tile list.
	void setBuilding(const std::string& id, Shapes shapes, const kworld::BuildingNav& nav,
		BuildingState state = {});
	void setBuildingState(const std::string& id, BuildingState state);
	void removeBuilding(const std::string& id);

	std::size_t dirtyTileCount() const noexcept { return dirty_.size(); }
	void markAllDirty();

	// Takes the dirty tiles and the current active shapes (canonical order:
	// buildings by id, then the fixed shapes). Null when nothing is dirty.
	// Jobs must be applied in the order they were prepared.
	std::unique_ptr<Rebuild> prepareRebuild();
	// Swaps a finished job's tiles into the mesh. Call at a tick boundary,
	// with no query running.
	void apply(Rebuild& job);

	// Region pruning (NavMesh::prune, knav/seeds.hpp): the fixed seeds (zone probes, seeds.def, building origins) and the
	// buildings' door seeds; apply() then prunes the whole mesh from them. None (the default): nothing is pruned.
	void setSeeds(std::vector<Point> seeds, std::vector<DoorSeed> doors)
	{
		seeds_ = std::move(seeds);
		doorSeeds_ = std::move(doors);
	}

	// With seeds: also keep every set touching the window's outer border (NavMesh::prune keepWindowEdge), for a one-zone mesh
	// whose sets may join a seed through a neighbour. Off by default.
	void setKeepWindowEdge(bool keep) noexcept { keepWindowEdge_ = keep; }

	// Convenience: markAllDirty, prepare, run, apply.
	void bakeAll(int threads);

	const NavMesh& mesh() const noexcept { return mesh_; }
	// For NavMesh::applyDoors; the tiles themselves change only through apply().
	NavMesh& mesh() noexcept { return mesh_; }

private:
	struct Building
	{
		Shapes shapes;
		kworld::BuildingNav nav;
		BuildingState state;
	};

	void markDirty(const Building& building);

	NavConfig config_;
	const kworld::Heightmap& terrain_;
	Shapes fixed_;
	std::map<std::string, Building> buildings_;
	std::set<TileCoord> dirty_;
	std::vector<Point> seeds_;
	std::vector<DoorSeed> doorSeeds_;
	bool keepWindowEdge_ = false;
	NavMesh mesh_;
};

} // namespace knav
