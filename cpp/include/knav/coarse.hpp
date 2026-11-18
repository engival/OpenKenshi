#pragma once

// Coarse region/portal graph over a baked navmesh (HPA*-style; docs/formats/NAVMESH_CACHE.md
// "Coarse graph"). The mesh is cut into clusters of kClusterTiles x kClusterTiles tiles. A node
// is a connected walkable region of one cluster; a portal is a chunk of cluster boundary (one
// tile edge) where a region meets a region of the neighbouring cluster. Edges join the portals of
// one region with the Detour path length between them, computed inside the cluster only, plus
// the stored straight path, so a position along a coarse route is always on walkable ground.
//
// The graph is derived data: a pure function of the mesh tiles, byte-identical whatever the
// thread count. Dynamic tile changes are applied with update(), which recomputes only the
// clusters the changed tiles can influence.

#include "knav/navmesh.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace knav {

// Tiles per cluster side. 3 tiles = 768 units: a boundary chunk is one tile edge, and the
// 72-tile world bake region is 24 clusters wide, so no cluster straddles a region file.
inline constexpr int kClusterTiles = 3;

struct ClusterId
{
	int x = 0;
	int z = 0;

	friend bool operator==(const ClusterId&, const ClusterId&) = default;
	friend auto operator<=>(const ClusterId& a, const ClusterId& b)
	{
		return a.z != b.z ? a.z <=> b.z : a.x <=> b.x;
	}
};

ClusterId clusterOfTile(int tileX, int tileZ);

enum class Side : std::uint8_t
{
	West,
	East,
	South, // -z
	North, // +z
};

// Where a region of this cluster meets a region of the neighbouring one, on one tile edge.
struct Portal
{
	Side side = Side::West;
	std::uint8_t chunk = 0; // which tile of the side, 0..kClusterTiles-1 along the boundary
	std::uint16_t node = 0; // region in this cluster
	std::uint16_t other = 0; // region in the neighbour
	std::uint8_t slot = 0; // tile of the cluster holding `poly`, row-major 0..8
	std::uint16_t poly = 0; // the polygon the point lies on
	Point point{}; // on the boundary, middle of the widest overlap of the two regions

	bool operator==(const Portal&) const = default;
};

// A door a stored path goes through, and how far the path runs inside the door's polygons.
struct DoorCross
{
	DoorKey door = 0;
	float length = 0.0f;

	bool operator==(const DoorCross&) const = default;
};

// A portal pair whose stored (cheapest) path goes through doors. The graph is built with every
// door open to everyone, so `cost` is the best case; the doors it crosses are listed here so a
// query can add the closed-door wait or refuse the edge from the DoorBook, and `bypass` is the
// cheapest path that avoids all door polygons (cost -1: none inside the cluster), taken when a
// door refuses. No tile or cluster is rebuilt when a door changes.
struct DoorEdge
{
	std::uint32_t pair = 0; // Cluster::pairIndex
	std::vector<DoorCross> doors; // ascending by door key
	float bypassCost = -1.0f;
	std::vector<Point> bypass; // corners between the portals, like Cluster::points

	bool operator==(const DoorEdge&) const = default;
};

struct Cluster
{
	std::uint16_t nodeCount = 0;
	std::vector<Point> anchors; // per node: the middle of its first polygon, a point on the mesh
	std::vector<Portal> portals; // sorted by (side, chunk, node, other)
	// Per portal pair (i < j) of one node: the cost (path length weighted by the graph's area
	// costs), and the corners of the straight path between the two portal points
	// (points[start[k]..start[k+1]], the portal points themselves left out).
	// Cost -1 and no points where the pair is in different nodes.
	std::vector<float> cost;
	std::vector<std::uint32_t> start;
	std::vector<Point> points;
	std::vector<DoorEdge> doorEdges; // ascending by pair

	static std::size_t pairIndex(std::size_t i, std::size_t j) noexcept { return j * (j - 1) / 2 + i; }
	float costBetween(std::size_t i, std::size_t j) const noexcept
	{
		return i < j ? cost[pairIndex(i, j)] : cost[pairIndex(j, i)];
	}
	// Straight path of the pair, from portal i's point to portal j's (the door-avoiding one if `bypass`).
	std::vector<Point> pathBetween(std::size_t i, std::size_t j, bool bypass = false) const;
	// The door information of a pair, or null when its path crosses no door.
	const DoorEdge* doorEdge(std::size_t i, std::size_t j) const noexcept;

	// What the pair costs a traveller of `doorClass` under `book` (null = doors as baked), and
	// whether that is the bypass; -1 when every route inside the cluster is refused.
	struct Crossing
	{
		float cost = -1.0f;
		bool bypass = false;
	};
	Crossing crossing(std::size_t i, std::size_t j, const TravelFilter& travel, const DoorQuery& doors) const noexcept;

	bool operator==(const Cluster&) const = default;
};

// One stretch of a coarse path, inside one cluster (or the hop across a boundary).
struct CoarseLeg
{
	ClusterId cluster; // for a hop: the cluster it leaves
	bool hop = false;
	PolyLoc from;
	PolyLoc to;
	float length = 0.0f;
	std::vector<Point> points; // straight path, from.point to to.point
};

struct CoarsePath
{
	enum class Failure
	{
		None,
		StartOffMesh, // no walkable polygon near the start
		EndOffMesh,
		NotBuilt, // an endpoint's cluster has no graph entry
		NoRoute, // both ends are on the mesh but the graph does not join them
	};

	bool found = false;
	Failure failure = Failure::None;
	std::vector<CoarseLeg> legs; // start to end; each leg ends where the next begins
	float length = 0.0f; // plain length of the legs
	float cost = 0.0f; // what the search minimised: length weighted by the graph's area costs
	std::vector<DoorKey> doors; // the doors the route goes through (not the ones a bypass avoids), in order

	// The point `distance` along the route, clamped to its ends. x and z lie on the legs' straight
	// paths, which stay inside mesh polygons; y is linear between their corners (snap it with
	// NavQuery::nearestPoint when the tiles are resident). Empty route: the origin.
	Point positionAt(float distance) const;
};

struct CoarseStats
{
	std::size_t clusters = 0;
	std::size_t nodes = 0;
	std::size_t portals = 0;
	std::size_t pairs = 0; // connected portal pairs
	std::size_t points = 0; // stored path corners
};

// A graph answers for one kind of traveller: `travel` fixes which areas its regions may use and
// what a unit of distance costs on each. Polygons the filter excludes belong to no region, so
// an island behind excluded water has no portals and no route reaches it. A sim keeps one
// graph per kind (swimmers, dry-only walkers, ...).
class CoarseGraph
{
public:
	// The clusters inside `clusters` (cluster coordinates) that hold tiles. The mesh must hold their
	// tiles and one cluster of margin, or portals toward a missing neighbour are not found.
	static CoarseGraph build(const NavMesh& mesh, const TileRect& clusters, int threads,
		const TravelFilter& travel = {});

	const TravelFilter& travel() const noexcept { return travel_; }
	// Clusters touching a tile rectangle.
	static TileRect clustersOf(const TileRect& tiles);

	// Takes over the clusters of `other` (replacing any of the same id); an empty graph also takes
	// its filter, otherwise the filters must be equal (throws kworld::Error).
	void merge(CoarseGraph&& other);

	// After `changed` tiles were replaced in `mesh`: recomputes the clusters holding them and
	// their four neighbours (a cluster depends on its own tiles and on its neighbours' region
	// numbering). Returns how many clusters were recomputed.
	std::size_t update(const NavMesh& mesh, const std::vector<TileCoord>& changed, int threads);

	const Cluster* cluster(ClusterId id) const noexcept;
	const std::map<ClusterId, Cluster>& clusters() const noexcept { return clusters_; }
	CoarseStats stats() const noexcept;

	// Route between the walkable points nearest to `from` and `to` (within `halfExtents`).
	// Each end must be inside the tiles of its mesh (they may be different meshes, e.g. two
	// regions); the route between them is read from the graph, so the clusters in the
	// middle need not be resident.
	//
	// `doors`: the DoorBook and traveller class to answer for. Edges through a closed door cost the
	// graph filter's DoorClosed cost over the length inside the door, edges through a locked or
	// refused door are dropped or replaced by their door-avoiding bypass. Without a book every door is
	// as baked (open, open to all). The meshes' own applied doors (NavMesh::applyDoors) are not read;
	// pass the same book to both.
	CoarsePath findPath(const NavMesh& mesh, const Point& from, const Point& to, const Point& halfExtents,
		const DoorQuery& doors = {}) const;
	CoarsePath findPath(const NavMesh& fromMesh, const NavMesh& toMesh, const Point& from, const Point& to,
		const Point& halfExtents, const DoorQuery& doors = {}) const;

	// Detailed path of one leg against the current mesh (only the leg's cluster is read), over
	// the areas this graph's filter allows, with the doors of `doors`.
	PathResult refine(const NavMesh& mesh, const CoarseLeg& leg, const DoorQuery& doors = {}) const;

	std::vector<std::uint8_t> serialize() const;
	static CoarseGraph deserialize(std::span<const std::uint8_t> bytes); // throws kworld::Error
	void save(const std::string& path) const;
	static CoarseGraph load(const std::string& path);

	bool operator==(const CoarseGraph&) const = default;

private:
	TravelFilter travel_;
	std::map<ClusterId, Cluster> clusters_;
};

} // namespace knav
