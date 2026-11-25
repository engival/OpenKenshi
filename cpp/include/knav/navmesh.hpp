#pragma once

// Tiled Recast/Detour navmesh: offline bake, cache file and path queries
// (docs/formats/NAVMESH_CACHE.md). World units, 10 per metre; Detour works in
// the same absolute world coordinates (floats).

#include "knav/door.hpp"
#include "knav/travel.hpp"
#include "kworld/collision.hpp"
#include "kworld/coords.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <DetourNavMesh.h> // dtPolyRef (64-bit: DT_POLYREF64)
class dtNavMeshQuery;
class dtQueryFilter;

namespace kworld {
class Heightmap;
}

namespace knav {

using Point = std::array<float, 3>;

// Clearance component labels (NavQuery::refusedByComponents) are built whole on the first query by default, which is deterministic: the
// world hash does not depend on when a build finished. A live server calls this to build them in slices instead (kenshi-k16.6.699): no tick
// stalls 0.2-0.8 s per streamed region, but a query answers "no refusal" while a build is pending, so routes then depend on query history.
void setSlicedComponents(bool sliced) noexcept;

// Inclusive rectangle of tile coordinates.
struct TileRect
{
	int x0 = 0;
	int z0 = 0;
	int x1 = -1;
	int z1 = -1;

	bool empty() const noexcept { return x1 < x0 || z1 < z0; }
	bool contains(int x, int z) const noexcept { return x >= x0 && x <= x1 && z >= z0 && z <= z1; }
	long long area() const noexcept { return empty() ? 0 : static_cast<long long>(x1 - x0 + 1) * (z1 - z0 + 1); }
	bool operator==(const TileRect&) const = default;
};

// A prop the bake gives a hard body clearance (kenshi-k16.7.89): a closed Obstacle or Projected solid that is not a rock shell
// or a leaning hull. propClearanceHull: its footprint grown by `radius` (an octagon round the corners), xyz triples, y 0, counter-clockwise;
// empty for a degenerate shape.
// The clearance a body keeps from furniture (OURS, kenshi-k16.7.91). The original has no capsule: its follower (radius 4 u) slides
// along an unEroded mesh's boundary, and only `path mode` != 0 pieces are cut (rwh.183); live, bodies stand 0.3-2 u from the
// pieces' boxes (rwh.184, Heft Noble House). Our eroded mesh plus this small ring reproduces that spacing without a body inside a barrel.
constexpr float kFurnitureBodyClearance = 0.75f; // not a whole number of cells: at 1.0 the hull edge (1.0 + half a 2 u cell) fell on cell centres and marked a column more, which closed the Bar's corridor to the raiders (k16.7.95)
// Dev side table (kenshi-k16.6.383): the spans the prop clearance ring hands back, as cell centres (kind 'g' gap lane, 's' stair, 'u' use-node spare), recorded by
// the bake while recordExemptCells(true); takeExemptCells() returns and clears them.
struct ExemptCell { float x, y, z; char kind; };
void recordExemptCells(bool on);
std::vector<ExemptCell> takeExemptCells();
bool propClearanceShape(const kworld::WorldShape& shape);
void propClearanceHull(const kworld::WorldShape& shape, float radius, std::vector<float>& out);

// Agent and grid choices. Height, climb and slope are the original's Havok generation settings
// (HAVOK_NAV.md), radius is the humans' path radius; cell sizes and the tile size are our choices.
struct NavConfig
{
	float cellSize = 2.0f; // xz voxel
	float cellHeight = 1.0f; // y voxel
	float agentHeight = 18.0f; // the original's Havok character height
	float agentRadius = 4.0f; // the original's human path radius
	float agentClimb = 5.0f; // the original's edge-matching step height (UNSURE that it is also the walk climb)
	float agentMaxSlope = 40.0f; // degrees; the original's maxWalkableSlope, on the 36-unit terrain lattice
	float tileSize = 256.0f; // world units; kWorldMin is a multiple of it

	// Water: one flat plane for the whole map (WATER.md: constant 100.0 in the exe), and the depths
	// (plane minus walkable surface) where a polygon becomes Area::Shallow and Area::Swim. The exe
	// wades at 6 * h and swims at 15 * h, h an unresolved per-character float taken as 1: UNSURE.
	float waterLevel = 100.0f;
	float wadeDepth = 6.0f;
	float swimDepth = 15.0f;

	// Tile (x, z) covers [originX + x * tileSize, +tileSize) on each axis.
	double originX = 0.0;
	double originZ = 0.0;
	int tilesX = 0;
	int tilesZ = 0;
	// The tiles this mesh bakes and holds (a region of a larger grid). Tile
	// coordinates always count from the origin, so regions of one world grid
	// share coordinates and stitch together when loaded side by side.
	TileRect window;

	bool operator==(const NavConfig&) const = default;
};

// Tiles per zone side, and zones per side of a world bake region file.
inline constexpr int kTilesPerZone = 18; // 4608 / 256
inline constexpr int kRegionZones = 4;
// Most tiles one dtNavMesh holds, and most polygons in one tile. Detour is built with DT_POLYREF64 (cmake/recast.cmake): a
// dtPolyRef is 16 salt + 28 tile + 20 poly bits, so these are limits of our own, not of the ref.
inline constexpr int kMaxResidentTiles = 8192;
inline constexpr int kMaxPolysPerTile = 8192;
static_assert(kMaxResidentTiles <= (1 << DT_TILE_BITS) && kMaxPolysPerTile <= (1 << DT_POLY_BITS), "dtPolyRef split");

// File name of the world bake region (rx, rz), counting regions of kRegionZones zones.
std::string regionFileName(int rx, int rz);

// Grid origin at the first zone, window the whole grid: tiles covering the zone cells min..max.
NavConfig configForZones(kworld::ZoneCell min, kworld::ZoneCell max);
// World-origin grid (all 64 x 64 zones), window the tiles of the zone cells min..max.
NavConfig configForWorldZones(kworld::ZoneCell min, kworld::ZoneCell max);

// Sets `out` to the allowed area flags and per-area costs of `travel`.
void configureFilter(dtQueryFilter& out, const TravelFilter& travel);

// A polygon of a tile that belongs to a door (its area is DoorOpen as baked).
struct DoorPoly
{
	std::uint16_t poly = 0;
	DoorKey door = 0;

	bool operator==(const DoorPoly&) const = default;
};

struct BakedTile
{
	int x = 0;
	int z = 0;
	std::vector<std::uint8_t> data; // dtCreateNavMeshData output
	std::vector<DoorPoly> doors; // ascending poly; the door polygons of `data`
};

struct TileCoord
{
	int x = 0;
	int z = 0;

	friend bool operator==(const TileCoord&, const TileCoord&) = default;
	friend auto operator<=>(const TileCoord& a, const TileCoord& b)
	{
		return a.z != b.z ? a.z <=> b.z : a.x <=> b.x;
	}
};

struct PolyLoc;

class NavMesh
{
public:
	NavMesh(NavMesh&&) noexcept;
	NavMesh& operator=(NavMesh&&) noexcept;
	~NavMesh();

	struct DetourParams
	{
		float origin[3] = {};
		float tileWidth = 0.0f;
		float tileHeight = 0.0f;
		std::uint32_t maxTiles = 0;
		std::uint32_t maxPolys = 0;
		std::uint32_t version = 0;
	};
	static NavMesh fromTiles(const NavConfig& config, std::vector<BakedTile> tiles);
	// The mesh a NAV_PARAMS record describes (SIMLINK.md section 12): the dtNavMeshParams of `params` exactly (origin, tile size,
	// maxTiles, maxPolys), with the tiles as received; config() then holds only what the params carry (origin, tile size, a
	// window that bounds the tiles). Throws kworld::Error on a Detour version other than this build's, or a bad tile.
	static NavMesh fromParams(const DetourParams& params, std::vector<BakedTile> tiles);
	// Throws kworld::Error on a bad or mismatched file.
	static NavMesh load(const std::string& path);
	// Merges cache files of one grid (same parameters, any windows), keeping
	// only tiles inside `keep`; the window is the union of the files' windows
	// clipped to `keep`. At most kMaxResidentTiles tiles are resident.
	static NavMesh load(const std::vector<std::string>& paths, const TileRect& keep);
	// The tiles of load() without building a mesh: the cache files' tiles inside `keep`, sorted by (z, x). `config` receives the
	// merged grid parameters (window as load() gives it). An empty `keep` reads no tile data, only the config. Streaming a
	// world bake in and out of a resident mesh (replaceTiles) uses this, docs/formats/NAVMESH_CACHE.md "Streaming coverage".
	static std::vector<BakedTile> readTiles(const std::vector<std::string>& paths, const TileRect& keep, NavConfig& config);
	// The world bake region (rx, rz) of `dir` (regionFileName) plus `margin` tiles of its neighbours
	// that exist as files. Throws if the region's own file is missing.
	static NavMesh loadRegion(const std::string& dir, int rx, int rz, int margin);
	void save(const std::string& path) const;

	// Swaps in rebuilt tiles: `removed` leave the mesh, `added` replace or join
	// it. Not safe against concurrent queries; call at a tick boundary.
	void replaceTiles(const std::vector<TileCoord>& removed, std::vector<BakedTile> added);

	const NavConfig& config() const noexcept { return config_; }
	const std::vector<BakedTile>& tiles() const noexcept { return tiles_; }
	std::size_t polyCount() const noexcept { return polyCount_; }
	const dtNavMesh& detour() const noexcept { return *mesh_; }
	// The baked tile at (x, z), or null.
	const BakedTile* tile(int x, int z) const noexcept;

	// Doors (door.hpp). Paints the polygons of every door of this mesh from `book`: no tile is
	// rebuilt or reloaded, only the area and flags of the door polygons change. Like
	// replaceTiles, not safe against concurrent queries; call at a tick boundary. The book is
	// remembered and re-applied to tiles that replaceTiles brings in.
	// Every tile whose polygons changed gets its version bumped by one and a TilePatch logged.
	void applyDoors(const DoorBook& book);
	const DoorBook& appliedDoors() const noexcept { return book_; }
	// One door polygon's paint: what NAV_POLY_FLAGS carries (SIMLINK.md section 12).
	struct PolyPaint
	{
		std::uint16_t poly = 0; // index in the tile
		std::uint16_t flags = 0;
		std::uint8_t area = 0;
	};
	// The change that took a tile from version - 1 to `version`.
	struct TilePatch
	{
		std::uint32_t version = 0;
		std::vector<PolyPaint> polys;
	};
	// A tile's version: 1 as baked, +1 for every door repaint that changed it and for every rebuild (replaceTiles). One counter,
	// so a client that holds version v - 1 can apply the patch v (the navmesh tile stream, SIMLINK.md section 12).
	std::uint32_t tileVersion(int x, int z) const noexcept;
	// Changes whenever the tile set or any tile version does (replaceTiles, a door repaint that changed a tile), and is unique per
	// mesh built in the process: a stream that saw this value need not look at the tiles again.
	std::uint64_t revision() const noexcept { return revision_; }
	// Steps only when tiles are added or removed (a door repaint changes flags, not shapes): what the clearance components are keyed on.
	std::uint64_t geometryRevision() const noexcept { return geometryRevision_; }
	// The patches that took the tile to its current version, oldest first (only the last kMaxTilePatches are kept; a tile
	// whose chain from a given version is cut must be sent whole).
	const std::vector<TilePatch>& tilePatches(int x, int z) const noexcept;
	static constexpr std::size_t kMaxTilePatches = 64;
	// Sets one polygon's flags and area (a NAV_POLY_FLAGS entry, or a door repaint); false if the tile or polygon is absent.
	// Does not touch versions or the patch log: those belong to the server (applyDoors); a client keeps its own.
	bool setPolyFlags(int tileX, int tileZ, int layer, int poly, std::uint16_t flags, std::uint8_t area);
	// The tile blob as it is now: the baked data with the current door paint on its door polygons. Empty if there is no tile.
	std::vector<std::uint8_t> tileBlob(int x, int z) const;
	// The dtNavMeshParams this mesh was initialised with and Detour's tile format version (the navmesh tile stream,
	// SIMLINK.md section 12: a client builds its own dtNavMesh the same way).
	DetourParams detourParams() const;
	// The doors with polygons in this mesh, ascending by key.
	std::vector<DoorKey> doorKeys() const;
	// A door's polygons (tile and index; the point is unset), in (tile z, x, poly) order.
	std::vector<PolyLoc> doorPolys(DoorKey door) const;
	// The door a Detour polygon ref belongs to (0: none).
	DoorKey doorOfRef(dtPolyRef ref) const
	{
		const auto found = doorOfRef_.find(ref);
		return found == doorOfRef_.end() ? 0 : found->second;
	}
	// The mean of the vertices of a door's polygons (the doorway's centre, on its surface), or false if the door has none.
	// `footprint`, when given, is the slab's xz extent about that centre (a gate is wide, and while it is shut its polygons are
	// a wall, so a walker stops at their edge, not at the centre).
	struct DoorFootprint
	{
		float axisX = 1.0f, axisZ = 0.0f; // unit direction of the long side
		float halfLong = 0.0f, halfDeep = 0.0f; // the vertices' largest extents along the axis and across it
	};
	bool doorCentre(DoorKey door, Point& out, DoorFootprint* footprint = nullptr) const;

	// Region pruning, the original's recipe (kinspect findings/navbake_static.md C42): every connected set of polygons (over
	// Detour links, any area) with no seed point within `reach` is cut: its polygons get flags 0, in the Detour mesh and in the
	// tile data, so no filter takes them and save() and tileBlob() carry the cut. Polygons an earlier prune cut are given back
	// their area's flag first, so a rebuilt neighbour can reconnect them. Door polygons are left to the door paint. Not safe
	// against concurrent queries. Returns the polygons cut. `keepWindowEdge`: a set that touches the outer border of the config's
	// window (a portal edge to a tile outside it) is kept too, as a seed: it may join a seed through a zone not in this mesh.
	std::size_t prune(const std::vector<Point>& seeds, const Point& reach, bool keepWindowEdge = false);

private:
	friend class NavQuery;
	NavMesh() = default;
	void countPolys();
	void indexDoors();
	struct TileLog
	{
		std::uint32_t version = 1;
		std::vector<TilePatch> patches;
	};
	// Repaints every door polygon from book_; the polygons whose area or flags changed, by tile.
	std::map<std::pair<int, int>, std::vector<PolyPaint>> paintDoors();

	NavConfig config_;
	std::vector<BakedTile> tiles_;
	DoorBook book_;
	std::map<DoorKey, std::vector<dtPolyRef>> doorRefs_; // Detour polygon refs
	std::unordered_map<dtPolyRef, DoorKey> doorOfRef_;
	std::map<std::pair<int, int>, TileLog> tileLogs_;
	std::size_t polyCount_ = 0;
	std::uint64_t revision_ = nextRevision();
	std::uint64_t geometryRevision_ = nextRevision();
	static std::uint64_t nextRevision() noexcept;
	std::unique_ptr<dtNavMesh, void (*)(dtNavMesh*)> mesh_{ nullptr, nullptr };
};

// The connected sets of the polygons of one mesh over its Detour links, for the polygons whose area
// flags meet `flags` (TravelFilter::allowed). Labels are local to the mesh; the caller joins the
// labels of meshes that touch. The mesh must outlive it, and stay unchanged.
class PolyComponents
{
public:
	static constexpr std::uint32_t kNone = ~0u;

	PolyComponents() = default;
	PolyComponents(const NavMesh& mesh, std::uint16_t flags);

	std::uint32_t count() const noexcept { return count_; }
	// The label of polygon `poly` of tile (tileX, tileZ); kNone when absent or not allowed.
	std::uint32_t at(int tileX, int tileZ, int poly) const;
	// visit(tileX, tileZ, poly, label) for every polygon, in storage order.
	void forEach(const std::function<void(int, int, int, std::uint32_t)>& visit) const;
	// Replaces each label by table[label].
	void relabel(const std::vector<std::uint32_t>& table);
	// The distinct labels (ascending, kNone left out) of the polygons that Detour links to any of
	// `polys` (tile and index): what a door's polygons connect when the door itself is not allowed.
	std::vector<std::uint32_t> around(const std::vector<PolyLoc>& polys) const;

private:
	const NavMesh* mesh_ = nullptr;
	std::vector<std::uint32_t> offset_; // per Detour tile index: first polygon in label_
	std::vector<std::uint32_t> label_;
	std::uint32_t count_ = 0;
};

// Where a point lands on the mesh: the polygon (tile and index) and the closest point on it.
struct PolyLoc
{
	int tileX = 0;
	int tileZ = 0;
	int poly = 0;
	Point point{};
};

// One portal of a walked corridor (kenshi-e9s9.4, PATH_WALK.md 3a): the stretch of the polygon edge a leg crosses, `left` / `right` as seen walking
// the leg (x east, z north: walking +z, left is at smaller x), so (right - left) x (position - left) in xz is positive on the far side. A point
// portal (left == right) is where a leg could not be walked by a ray or where a join leaves the polygon. `point` is the index into the point list
// of the leg's end.
struct CorridorPortal
{
	Point left{};
	Point right{};
	float clearLeft = 0.0f; // findDistanceToWall of each end, up to the limit given
	float clearRight = 0.0f;
	std::uint64_t ref = 0; // the polygon the walker is in before this portal (Detour ref; not stable across mesh changes)
	int point = 0;
};

struct PathResult
{
	bool found = false; // a polygon path exists from the start poly to the end poly
	bool complete = false; // ... and it reaches the end poly (not a partial path)
	std::vector<Point> points; // straight path, start to end
	float length = 0.0f;
	float cost = 0.0f; // length with each stretch charged at its polygon's area cost in the filter
	std::vector<DoorKey> doors; // the doors the polygon path goes through, in order
	// Why `complete` is false (None when it is true). StartOffMesh / EndOffMesh: no allowed polygon
	// within the half extents; NoPath: Detour found nothing; Closed: the partial path ends because the
	// search ran out of reachable polygons (the end is in another connected set of the allowed areas);
	// OutOfNodes: the search hit the node pool (4,096) before it got there, so the end may be reachable.
	enum class Failure : std::uint8_t { None, StartOffMesh, EndOffMesh, NoPath, Closed, OutOfNodes };
	Failure failure = Failure::None;
	// The same traveller with no clearance did not reach the end either (a clearance search only refuses portals, so a larger body cannot
	// do better, and a retry at radius 0 would fail the same way; kenshi-k16.6.262). Set only on a search with a radius.
	bool plainFailed = false;
	// Polygons the clearance search closed (0 when none ran): the cost behind a slow route (kenshi-k16.7.62).
	int clearanceNodes = 0;
};

// Query state for one thread; the NavMesh must outlive it.
class NavQuery
{
public:
	// maxNodes: the A* node pool (Detour's default 4,096 polygons explored runs dry on a 1,500 unit
	// walk across a dense town); a search that runs dry returns a partial path (Failure::OutOfNodes).
	explicit NavQuery(const NavMesh& mesh, int maxNodes = 4096);
	~NavQuery();
	NavQuery(const NavQuery&) = delete;
	NavQuery& operator=(const NavQuery&) = delete;

	// Nearest walkable point within the half extents of `point`.
	// Portal clearances cached so far (a test hook: it is 0 right after any mesh change).
	std::size_t clearanceEntries() const;
	bool nearestPoint(const Point& point, const Point& halfExtents, Point& out) const;
	// The walkable point nearest `point` in the xz plane, at any height, within `reach` (y is the
	// polygon's surface height). For a caller that has no height of its own: nearestPoint would pick
	// the polygon nearest in 3D, i.e. the lowest ground anywhere in the box, whose closest point can even
	// fall outside it. When two floors cover the point (a deck over the ground) both are at xz distance 0;
	// the lowest stays (the ground under a deck).
	// `accept`: only polygons it takes count (a polygon in the walker's own connected set). `where` gets the polygon of the hit.
	bool nearestPointXZ(const Point& point, float reach, Point& out, const std::function<bool(const PolyLoc&)>& accept = {},
		PolyLoc* where = nullptr) const;
	// As nearestPoint, and says which polygon.
	// Only polygons of the areas `travel` allows count.
	bool locate(const Point& point, const Point& halfExtents, PolyLoc& out, const TravelFilter& travel = {}) const;
	// As locate, but only polygons for which `accept` is true count (the nearest of those within the
	// half extents; every polygon whose box meets the extents is tried, not only the nearest one).
	bool locateWhere(const Point& point, const Point& halfExtents, const std::function<bool(const PolyLoc&)>& accept,
		PolyLoc& out, const TravelFilter& travel = {}) const;
	// True when a walkable polygon exists within the half extents of `point`.
	bool onMesh(const Point& point, const Point& halfExtents) const;
	// One movement step: from the walkable point nearest `from` (within the half extents), straight
	// towards `to` over the polygons `travel` allows, stopping at the mesh boundary (Detour's
	// moveAlongSurface slides along an edge it meets). `out` is on the surface, y included.
	// `blocked` is set when `out` is more than a hair short of `to` in the xz plane. False when
	// `from` is not within the half extents of the mesh.
	// `crossing`: doors the walker's route was planned through while they were not locked. The lock refuses a PLAN, not a walk already
	// under way (rwh.231 c3a, rwh.244: a walker planned at openness 0.72 crossed the 0.6 flip with no stop): the slide goes through
	// these doors' polygons even when locked, and never through another locked door's.
	bool slide(const Point& from, const Point& to, const Point& halfExtents, Point& out, bool& blocked,
		const TravelFilter& travel = {}, const std::vector<DoorKey>& crossing = {}) const;
	// Path between the nearest walkable points to `from` and `to`, over the areas `travel` allows;
	// the path is the cheapest by its area costs, `length` is its plain length.
	// With `travel.radius` above 0 the search is ours, not Detour's: a portal is crossed only where the traveller
	// fits (clearance), both ends are pushed off the walls and the corners of the straight path away from them.
	PathResult findPath(const Point& from, const Point& to, const Point& halfExtents,
		const TravelFilter& travel = {}) const;
	// True when the straight leg `from` -> `to` walks: the slide reaches the end (blocked nowhere), and with `radius` above 0
	// the two lines `radius` to either side of it walk too, so the body fits along the whole leg. The route test's skip check.
	bool walkLeg(const Point& from, const Point& to, float radius, const TravelFilter& travel = {}) const;
	// The straight-walk shortcut's width walk (planPath): clear for the body up to its width short of `to`, then as a point, crossing no door polygon.
	bool straightWalk(const Point& from, const Point& to, const TravelFilter& travel) const;
	// Distance from `point` (on polygon `loc`) to the nearest wall, up to `limit`; walls are mesh edges with no
	// neighbour, whatever the areas.
	float wallDistance(const PolyLoc& loc, float limit) const;
	// wallDistance at a point: located within `halfExtents` first; -1 when the point is on no polygon.
	float clearanceAt(const Point& point, const Point& halfExtents, float limit) const;

	// The portal corridor of a point list: per leg, the polygons a Detour raycast along it crosses, chained from `startRef` (0 = locate the
	// first point); a leg the ray cannot walk (or one over an off-mesh link) ends in a point portal at its end, as does a join that leaves the
	// chained polygon. Every point after the first ends in a portal of its own or a crossing at it. `clearLimit` caps the wall-distance probes
	// (radius + 5). Portals under 0.5 u wide are point portals.
	std::vector<CorridorPortal> corridor(const std::vector<Point>& points, float clearLimit, const TravelFilter& travel) const;

	// A door's middle (the mean of its polygons' vertices) and its axis through the wall, as a unit vector; false for an unknown door.
	bool doorFrame(DoorKey door, Point& centre, double& axisX, double& axisZ) const;

private:
	struct ClearanceCache; // what each portal is known to let through, filled lazily (navmesh.cpp)

	// A door shut on a body (kenshi-k16.6.552): true, and the door, when `point` stands on a LOCKED door polygon (the leaf closed on it). The original's
	// close never moves or refuses an occupant (rwh231 / rwh232), and the occupant keeps a route: a locked polygon refuses entry, not a body on it.
	bool standsOnLocked(const Point& point, const Point& halfExtents, DoorKey& door) const;
	// A route / slide from a locked door's leaf walks as if through locks, kept only when it passes no OTHER locked door.
	bool passesOtherLocked(const std::vector<DoorKey>& doors, DoorKey held) const;
	PathResult findPathFrom(const Point& from, const Point& to, const Point& halfExtents, const TravelFilter& travel) const;

	// findPath without the door pass (a leg of a door route plans with this); `snappedStart` gets the start on the mesh before the clearance refit and push.
	PathResult planPath(const Point& from, const Point& to, const Point& halfExtents, const TravelFilter& travel, Point* snappedStart) const;
	PathResult planPathFrom(const Point& from, const Point& to, const Point& halfExtents, const TravelFilter& travel, Point* snappedStart,
		bool ownStart, bool* exempted) const;
	void throughDoorMiddle(PathResult& result, const Point& from, const Point& to, const Point& halfExtents, const TravelFilter& travel,
		const Point& snappedStart) const;

	// True when no clearance search from `start` to `end` at `radius` can arrive: the two lie in different clearance components (portals the
	// body fits, the first hop out of the start when `exemptFirst` and the last into the goal exempt, as in the search). Labelled per radius
	// over the whole mesh on first use, kept until tiles change. Doors and filters are ignored, so a refusal is never wrong.
	bool refusedByComponents(dtPolyRef startRef, dtPolyRef endRef, const Point& end, float radius, bool exemptFirst) const;

	ClearanceCache& clearanceFor() const; // the cache, emptied first if the mesh changed since it was filled

	const NavMesh& mesh_;
	dtNavMeshQuery* query_;
	int maxNodes_;
	std::unique_ptr<ClearanceCache> clearance_;
	// planPath's working buffers, kept between calls (a NavQuery is single-threaded: query_ is too). Each was a fresh
	// kMaxPathPolys-sized allocation per route (kenshi-k16.7.64).
	struct Scratch
	{
		std::vector<dtPolyRef> polys, clear, cornerRefs, refs;
		std::vector<float> corners, straight;
	};
	mutable Scratch scratch_;
};

} // namespace knav
