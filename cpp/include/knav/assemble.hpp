#pragma once

// Tile assembly (docs/formats/NAVMESH_GENERATOR.md section 0, F2; kenshi-k16.7.144): the Recast bake's polygons for a tile become
// native::Mesh faces (the third producer, after the shipped tiles and the generator), the generator's surfaces cut them, and ONE
// nativeTiles call builds the tile. Offline form: nothing in the runtime bake calls it yet (stage 4 is the switch).
//
//  - The Recast pass leaves the generated shapes unfilled (the caller bakes the tile from the world's shapes minus the generated
//    buildings), so the ground under a building is plain terrain.
//  - A generated face cuts a Recast face where it lies above it by (-ownerTolerance, characterHeight] (the generator's cut rule, F4):
//    the Recast face loses that xz footprint. A cutter solid (a box of the generated shapes' obstacles) cuts the same way.
//  - Terrain triangles cut generated zone-side faces under ground: the part of a face that lies below the terrain surface is removed.

#include "knav/native.hpp"
#include "knav/native_tiles.hpp"
#include "knav/navmesh.hpp"
#include "kworld/collision.hpp"
#include "kworld/heightmap.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace knav::assembly
{

struct Settings
{
	double cutBase = 0.1; // a generated face within this of a Recast face (either way) takes the overlap
	double characterHeight = 18.0; // ... and above it by at most this
	double undergroundTolerance = 0.25; // a generated face is under ground where terrain is above it by more than this
	double minPieceArea = 0.05; // clip pieces under this xz area are dropped
	double lattice = 1.0 / 256.0; // output corners weld on this
};

// A tile's Recast mesh as native faces, world coordinates: corners from the polygon, heights from the detail mesh (its first vertices
// are the polygon's; the interior of a polygon is flattened to the corner heights, which nativeTiles does anyway for a non-planar face). Door polygons
// (Area::DoorOpen with a door) carry faceData 5; `doorCentres` receives, per such face, its key and the face's corners (xz) for lookup.
struct RecastFaces
{
	native::Mesh mesh;
	std::vector<std::pair<DoorKey, std::vector<std::array<double, 3>>>> doors;
};

RecastFaces recastFaces(const BakedTile& tile);

// The terrain triangles of the world's 36-unit lattice (the bake's), over the xz box.
std::vector<std::array<std::array<double, 3>, 3>> terrainTriangles(const kworld::Heightmap& terrain, double x0, double z0, double x1, double z1);

struct Report
{
	std::size_t recastFaces = 0;
	std::size_t recastCut = 0; // Recast faces a generated face or cutter touched
	double recastRemovedArea = 0.0; // xz u2
	std::size_t generatedFaces = 0; // zone-side faces in
	std::size_t generatedUnderground = 0; // zone-side faces cut by terrain
	std::size_t worldFaces = 0; // faces of the assembled world mesh
};

// A solid that cuts Recast faces: its xz footprint (convex, either winding) over a height range.
struct Cutter
{
	std::vector<std::array<double, 2>> footprint;
	double low = 0.0;
	double high = 0.0;
};

struct Input
{
	const NavConfig* config = nullptr;
	TileCoord tile;
	const BakedTile* recast = nullptr; // the tile's Recast bake without the generated buildings; none: terrain-free tile
	std::vector<const native::Mesh*> interiors; // generated interior meshes (info set): kept as their own meshes, they cut Recast faces
	std::vector<const native::Mesh*> zoneSide; // generated zone-side faces: join the world mesh, cut by terrain
	std::vector<Cutter> cutters;
	std::vector<std::array<std::array<double, 3>, 3>> terrain; // terrainTriangles of the tile box (with its border)
};

// The tile's world mesh before nativeTiles: Recast faces cut by the generated footprints and cutters, plus the zone-side faces cut by
// terrain. Deterministic.
native::Mesh assembleWorld(const Input& input, const Settings& settings = {}, Report* report = nullptr);

// One nativeTiles call: the assembled world mesh and the interiors, built as the tile `input.tile`.
std::vector<BakedTile> assembleTile(const Input& input, const Settings& settings = {}, Report* report = nullptr,
	const NativeDoorKeys& doorKeys = {}, NativeTileStats* stats = nullptr);

} // namespace knav::assembly
