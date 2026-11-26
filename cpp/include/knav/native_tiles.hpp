#pragma once

// The original's shipped navmesh (knav/native.hpp) as Detour tiles: the runtime base mesh, which our recipe bake patches
// only where the world changes (docs/formats/NAVMESH_CACHE.md "Runtime mesh"). Each face is ear-clipped in xz, the
// triangles are clipped to the 256 u tile square, and the pieces of one face are merged back into convex polygons of at
// most six corners; the triangles become the detail mesh, so a non-planar face keeps its heights at the corners.

#include "knav/native.hpp"
#include "knav/navmesh.hpp"
#include "kworld/collision_area.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace knav {

// The door a door face (faceData 4 / 5) belongs to, from the mean of its polygon's corners (world); 0 = none, the polygon
// stays DoorOpen with no door to repaint it.
using NativeDoorKeys = std::function<DoorKey(const std::array<double, 3>& centre)>;

// The doors of `buildings` as NativeDoorKeys. The shipped faces carry no door id, so a door face belongs to the door whose
// footprint, grown by `reach` (the bake's door margin), holds its centre in xz, at most `below` under the door's bottom and
// not above its top; the nearest footprint wins.
NativeDoorKeys nativeDoorKeys(const std::vector<kworld::AreaBuilding>& buildings, double reach = 8.0, double below = 10.0);

struct NativeTileStats
{
	std::size_t faces = 0; // faces placed, once per tile they reach
	std::size_t polys = 0;
	std::size_t slivers = 0; // polygons with no area on the 1/8 u vertices, kept as bridges
	std::size_t fanned = 0; // faces whose outline is not simple (after the spike and pinch repair): left out of the tile, once per tile they reach
	std::size_t overfull = 0; // tiles past kMaxPolysPerTile, left out
	std::size_t duplicates = 0; // coincident polygons merged into one
	std::size_t openLinks = 0; // streaming-link edges with no partner in their tile and not on its border
	std::size_t openShared = 0; // edges with an opposite face that found no partner (welding or clipping lost it)
	std::size_t partnered = 0; // polygon edges joined to a neighbour polygon in their tile
	std::size_t doorPolys = 0; // polygons of door faces (faceData 4 / 5)
	std::size_t unkeyedDoors = 0; // ... that no door claimed
	int maxPolys = 0; // most polygons in one tile
	std::vector<std::array<int, 3>> tilePolys; // {x, z, polygons} of every tile built or left out over the cap
	std::vector<std::uint32_t> fannedFaces; // world-mesh faces that would not ear-clip (may repeat per tile)
	std::map<std::pair<int, int>, std::vector<std::uint32_t>> polyFace; // per tile, per polygon: its source face in the world mesh (0xffffffff: an interior), the fold census
	std::map<std::pair<int, int>, std::vector<std::uint8_t>> topPolys; // per tile, per polygon in Detour order: 1 = from an object top (the fold census)
};

// Converts the faces of `meshes` (any mix of zone world meshes and interiors) into the tiles `coords` of `config`'s grid,
// in `coords` order; tiles no face reaches are absent. Vertices are kept to 1/8 u.
std::vector<BakedTile> nativeTiles(const NavConfig& config, const std::vector<native::Mesh>& meshes,
	const std::vector<TileCoord>& coords, const NativeDoorKeys& doorKeys = {}, NativeTileStats* stats = nullptr);

} // namespace knav
