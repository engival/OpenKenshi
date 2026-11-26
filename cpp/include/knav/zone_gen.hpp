#pragma once

// A zone's tiles from our own generators (kenshi-k16.7.189): knav::ground for the terrain and every building's shapes, knav::surfgen for
// the walkable buildings with an interior mask, one nativeTiles call. No Recast and no shipped tile; a building the generator throws on is
// reported (ZoneGenReport::failedIds), never baked another way.

#include "knav/native.hpp"
#include "knav/navmesh.hpp"
#include "knav/seeds.hpp"
#include "kworld/collision_area.hpp"
#include "kworld/heightmap.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace knav
{

struct GeneratedBuilding
{
	const kworld::AreaBuilding* building = nullptr;
	native::Mesh interior; // info set: a generated interior
	native::Mesh zoneSide;
	double min[2]{}, max[2]{}; // xz box of the building's shapes
};

struct ZoneGenReport
{
	std::size_t buildings = 0; // generated
	std::size_t buildingsFailed = 0; // the generator threw: no interior, no tops; the building still cuts the ground
	std::vector<std::string> failedIds;
	std::vector<std::string> failedWhy; // the exception text of each failedIds entry (same index)
	// bakeZone (the cutover: ground + surfgen, no Recast)
	std::size_t groundFaces = 0, topFaces = 0; // the zone window's faces, and the walkable tops among them
	std::size_t welded = 0, weldSplit = 0, weldPaired = 0, weldDropped = 0, weldOrphanOpen = 0, weldOrphanGap = 0, seamPruned = 0, weldThrough = 0, weldByLive[4] = {}; // ground::weldMesh over the window
	std::size_t stitched = 0; // rim vertices inserted by stitchRims over the four neighbour windows
	std::size_t doorFaces = 0, doorFacesPainted = 0; // zone-side door faces (data 4 / 5) and ground faces painted from them
	std::size_t tiles = 0, overfullTiles = 0; // tiles built / left out over the polygon cap
	int maxPolys = 0; // most polygons in one tile
	std::size_t openLinks = 0, openShared = 0; // NativeTileStats: streaming-link / shared edges that found no partner polygon
	std::vector<std::array<int, 3>> tilePolys; // {x, z, polygons} per tile reached, over the cap or not
	std::map<std::string, double> topKept; // kept top area (xz u2) per source label (a building id, "foliage", "feature @ x, z")
	double msGround = 0, msSurf = 0, msTiles = 0;
};

// The buildings of `area` the generator takes, generated (a throwing building is counted in `report` and left out). With `config`, only
// the buildings whose box (plus 16 u) meets its tile window: a neighbour zone's building touches none of its tiles.
std::vector<GeneratedBuilding> generateBuildings(const kworld::AreaGeometry& area, ZoneGenReport* report = nullptr, int threads = 1, const NavConfig* config = nullptr);

// A zone's prune seeds as xz points: its probes (seeds.def, else the 3 x 3 grid) and the building origins.
std::vector<std::array<double, 2>> zoneBakeSeeds(const kworld::Heightmap& terrain, const kworld::AreaGeometry& area,
	const std::vector<Point>& seedsDef, kworld::ZoneCell zone, float waterLevel);

// The ground windows of a bake run, each built once from its own zone's gathered area and seeds (kenshi-k16.7.215). bakeZone takes its own
// window and its four neighbours' rims from here, so a zone's mesh and its neighbours' agree by construction; a window is built by the first
// zone that needs it (as a neighbour or as itself). A scan in row order lets bakeZone drop the entry two rows back. `gather` gathers a zone's area.
struct GroundWindows
{
	struct Entry;
	std::function<kworld::AreaGeometry(kworld::ZoneCell)> gather;
	std::vector<Point> seedsDef;
	std::map<std::pair<int, int>, std::shared_ptr<Entry>> entries;
};

// The cutover bake of a zone (config = configForWorldZones(zone, zone)): every tile of its window from knav::ground (terrain, every
// building's shapes as cutters, walkable tops of no-mask buildings, foliage and features) and knav::surfgen (masked buildings: interior
// meshes, zone sides joined as tops, door faces painted), the window stitched to its four neighbours' rims, and one nativeTiles call.
// `seeds`: the prune's xz points (zoneSeeds + buildingSeeds). No Recast, no shipped tile, no fallback: what could not be built is in `report`.
std::vector<BakedTile> bakeZone(const NavConfig& config, const kworld::Heightmap& terrain, const kworld::AreaGeometry& area,
	const std::vector<std::array<double, 2>>& seeds, int threads, ZoneGenReport* report = nullptr,
	GroundWindows* windows = nullptr, kworld::ZoneCell zone = {});

} // namespace knav
