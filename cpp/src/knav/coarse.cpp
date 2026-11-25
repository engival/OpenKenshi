#include "knav/coarse.hpp"
#include "knav/query_filter.hpp"

#include "kworld/error.hpp"

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <thread>
#include <utility>
#include <tuple>
#include <unordered_map>

namespace knav {

namespace {

static_assert(std::endian::native == std::endian::little, "graph files are little-endian");

constexpr char kMagic[4] = { 'K', 'C', 'R', 'S' };
constexpr std::uint32_t kGraphVersion = 4; // 4: six area costs (Edge); 3: five, per-cluster door edges; 3 and 2 are still read
constexpr int kSlots = kClusterTiles * kClusterTiles;
constexpr int kMaxPathPolys = 4096;
constexpr std::uint64_t kGoal = ~0ull;

float distance(const Point& a, const Point& b)
{
	return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1])
		+ (a[2] - b[2]) * (a[2] - b[2]));
}

float pathLength(const std::vector<Point>& points)
{
	float length = 0.0f;
	for (std::size_t i = 1; i < points.size(); ++i)
		length += distance(points[i - 1], points[i]);
	return length;
}

int slotOf(ClusterId id, int tileX, int tileZ)
{
	return (tileZ - id.z * kClusterTiles) * kClusterTiles + (tileX - id.x * kClusterTiles);
}

PolyLoc portalLoc(ClusterId id, const Portal& portal)
{
	return { id.x * kClusterTiles + portal.slot % kClusterTiles, id.z * kClusterTiles + portal.slot / kClusterTiles,
		portal.poly, portal.point };
}

ClusterId neighbour(ClusterId id, Side side)
{
	switch (side)
	{
	case Side::West: return { id.x - 1, id.z };
	case Side::East: return { id.x + 1, id.z };
	case Side::South: return { id.x, id.z - 1 };
	default: return { id.x, id.z + 1 };
	}
}

Side opposite(Side side)
{
	constexpr Side flipped[] = { Side::East, Side::West, Side::North, Side::South };
	return flipped[static_cast<int>(side)];
}

std::uint64_t nodeKey(ClusterId id, std::size_t portal)
{
	return static_cast<std::uint64_t>(id.z) << 40 | static_cast<std::uint64_t>(id.x) << 16 | portal;
}

ClusterId keyCluster(std::uint64_t key)
{
	return { static_cast<int>(key >> 16 & 0xffffff), static_cast<int>(key >> 40) };
}

std::size_t keyPortal(std::uint64_t key)
{
	return key & 0xffff;
}

// A scratch Detour mesh holding only the tiles of one cluster. Paths found in it cannot leave the
// cluster, and its polygon links give the cluster's connected regions.
class ClusterMesh
{
public:
	// The tiles as baked: every door open and open to all. With a book, the door polygons are painted
	// from it after the regions are found, so region numbers never depend on door state.
	ClusterMesh(const NavMesh& mesh, ClusterId id, const TravelFilter& travel, const DoorBook* book = nullptr)
		: travel_(travel), id_(id), nav_(dtAllocNavMesh(), dtFreeNavMesh)
	{
		const NavConfig& config = mesh.config();
		dtNavMeshParams params{};
		params.orig[0] = static_cast<float>(config.originX);
		params.orig[2] = static_cast<float>(config.originZ);
		params.tileWidth = config.tileSize;
		params.tileHeight = config.tileSize;
		params.maxTiles = kSlots;
		params.maxPolys = kMaxPolysPerTile;
		if (dtStatusFailed(nav_->init(&params)))
			throw kworld::Error("dtNavMesh::init failed");
		for (int slot = 0; slot < kSlots; ++slot)
		{
			const BakedTile* tile = mesh.tile(id.x * kClusterTiles + slot % kClusterTiles,
				id.z * kClusterTiles + slot / kClusterTiles);
			if (!tile)
				continue;
			auto* data = static_cast<unsigned char*>(dtAlloc(tile->data.size(), DT_ALLOC_PERM));
			std::memcpy(data, tile->data.data(), tile->data.size());
			if (dtStatusFailed(nav_->addTile(data, static_cast<int>(tile->data.size()), DT_TILE_FREE_DATA, 0, nullptr)))
			{
				dtFree(data);
				throw kworld::Error("dtNavMesh::addTile failed");
			}
		}
		findNodes();
		for (int slot = 0; slot < kSlots; ++slot)
		{
			const BakedTile* tile = mesh.tile(id.x * kClusterTiles + slot % kClusterTiles,
				id.z * kClusterTiles + slot / kClusterTiles);
			if (!tile)
				continue;
			const dtMeshTile* loaded = nav_->getTileAt(tile->x, tile->z, 0);
			for (const DoorPoly& door : tile->doors)
			{
				const dtPolyRef ref = nav_->getPolyRefBase(loaded) | door.poly;
				doorOf_[ref] = door.door;
				if (book)
					paintDoor(*nav_, ref, book->get(door.door));
			}
		}
	}

	std::uint16_t nodeCount() const noexcept { return nodeCount_; }
	const std::vector<Point>& anchors() const noexcept { return anchors_; }

	// Region of a polygon; 0xffff when the polygon is not in this cluster or its area is excluded.
	std::uint16_t node(int slot, int poly) const noexcept
	{
		return slot >= 0 && slot < kSlots && static_cast<std::size_t>(poly) < nodes_[slot].size()
			? nodes_[slot][static_cast<std::size_t>(poly)] : std::uint16_t{ 0xffff };
	}

	// Straight path between two polygons of the cluster; false when there is none. `cost` gets the
	// path length weighted by the area costs: each stretch between crossings of polygon edges
	// is charged at the cost of the polygon it lies in.
	// `crossed` gets the doors the path runs through and how far inside each; `avoidDoors` keeps the
	// search off all door polygons (the path may still start or end on one).
	bool path(const PolyLoc& from, const PolyLoc& to, std::vector<Point>& out, float& cost,
		std::vector<DoorCross>* crossed = nullptr, bool avoidDoors = false)
	{
		const dtPolyRef start = ref(from);
		const dtPolyRef end = ref(to);
		if (!start || !end)
			return false;
		if (!query_)
		{
			query_.reset(dtAllocNavMeshQuery());
			if (dtStatusFailed(query_->init(nav_.get(), kMaxPathPolys)))
				throw kworld::Error("dtNavMeshQuery::init failed");
			polys_.resize(kMaxPathPolys);
			straight_.resize(3ull * kMaxPathPolys);
			crossingRefs_.resize(kMaxPathPolys);
		}
		TravelQueryFilter filter(travel_);
		if (avoidDoors)
			filter.setExcludeFlags(filter.getExcludeFlags() | areaFlag(Area::DoorOpen) | areaFlag(Area::DoorClosed));
		int polyCount = 0;
		if (dtStatusFailed(query_->findPath(start, end, from.point.data(), to.point.data(), &filter,
				polys_.data(), &polyCount, kMaxPathPolys)) || polyCount == 0
			|| polys_[static_cast<std::size_t>(polyCount - 1)] != end)
			return false;
		int count = 0;
		if (dtStatusFailed(query_->findStraightPath(from.point.data(), to.point.data(), polys_.data(),
				polyCount, straight_.data(), nullptr, nullptr, &count, kMaxPathPolys)) || count == 0)
			return false;
		out.clear();
		for (int i = 0; i < count; ++i)
			out.push_back({ straight_[3ull * i], straight_[3ull * i + 1], straight_[3ull * i + 2] });

		// The corners alone do not say which polygon a stretch lies in; a second straight path with
		// a point on every edge crossing does (the polygon entered at each point).
		int crossings = 0;
		if (dtStatusFailed(query_->findStraightPath(from.point.data(), to.point.data(), polys_.data(),
				polyCount, straight_.data(), nullptr, crossingRefs_.data(), &crossings, kMaxPathPolys,
				DT_STRAIGHTPATH_ALL_CROSSINGS)))
			return false;
		cost = 0.0f;
		bool prevDoor = false;
		std::map<DoorKey, float> inside;
		for (int i = 0; i + 1 < crossings; ++i)
		{
			const dtPoly* poly = nullptr;
			const dtMeshTile* tile = nullptr;
			nav_->getTileAndPolyByRefUnsafe(crossingRefs_[static_cast<std::size_t>(i)], &tile, &poly);
			const float* a = &straight_[3ull * i];
			const float length = distance({ a[0], a[1], a[2] }, { a[3], a[4], a[5] });
			cost += length * travel_.cost[poly->getArea()];
			if (isDoorArea(poly->getArea()) && !prevDoor)
				cost += travel_.doorCrossing;
			prevDoor = isDoorArea(poly->getArea());
			if (const auto door = doorOf_.find(crossingRefs_[static_cast<std::size_t>(i)]); door != doorOf_.end())
				inside[door->second] += length;
		}
		if (crossed)
		{
			crossed->clear();
			for (const auto& [door, length] : inside)
				crossed->push_back({ door, length });
		}
		return true;
	}

private:
	dtPolyRef ref(const PolyLoc& loc) const
	{
		const dtMeshTile* tile = nav_->getTileAt(loc.tileX, loc.tileZ, 0);
		return tile && loc.poly >= 0 && loc.poly < tile->header->polyCount ? nav_->getPolyRefBase(tile) | loc.poly : 0;
	}

	// Flags 0 is a polygon NavMesh::prune cut (no seed reaches it): no query filter takes it, so no node or portal is made of it
	// (dc210836; before this the graph kept nodes for pruned islands whose anchors are off the mesh).
	bool allowed(const dtPoly& poly) const noexcept { return poly.flags != 0 && travel_.allows(static_cast<Area>(poly.getArea())); }

	// Union of polygons joined by links; regions numbered by their first polygon in (tile z, x, index).
	void findNodes()
	{
		std::array<const dtMeshTile*, kSlots> tiles{};
		std::array<int, kSlots> base{};
		int total = 0;
		for (int slot = 0; slot < kSlots; ++slot)
		{
			tiles[slot] = nav_->getTileAt(id_.x * kClusterTiles + slot % kClusterTiles,
				id_.z * kClusterTiles + slot / kClusterTiles, 0);
			base[slot] = total;
			total += tiles[slot] ? tiles[slot]->header->polyCount : 0;
		}
		std::vector<int> parent(static_cast<std::size_t>(total));
		std::iota(parent.begin(), parent.end(), 0);
		const auto root = [&](int at)
		{
			while (parent[static_cast<std::size_t>(at)] != at)
			{
				parent[static_cast<std::size_t>(at)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(at)])];
				at = parent[static_cast<std::size_t>(at)];
			}
			return at;
		};
		for (int slot = 0; slot < kSlots; ++slot)
		{
			const dtMeshTile* tile = tiles[slot];
			for (int ip = 0; tile && ip < tile->header->polyCount; ++ip)
			{
				if (!allowed(tile->polys[ip]))
					continue;
				for (unsigned k = tile->polys[ip].firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
				{
					unsigned salt, it, iq;
					nav_->decodePolyId(tile->links[k].ref, salt, it, iq);
					const dtMeshTile* otherTile = std::as_const(*nav_).getTile(static_cast<int>(it));
					if (!allowed(otherTile->polys[iq]))
						continue;
					const dtMeshHeader* other = otherTile->header;
					const int a = root(base[slot] + ip);
					const int b = root(base[slotOf(id_, other->x, other->y)] + static_cast<int>(iq));
					parent[static_cast<std::size_t>(std::max(a, b))] = std::min(a, b);
				}
			}
		}
		std::vector<int> label(static_cast<std::size_t>(total), -1);
		for (int slot = 0; slot < kSlots; ++slot)
		{
			const int count = tiles[slot] ? tiles[slot]->header->polyCount : 0;
			nodes_[slot].resize(static_cast<std::size_t>(count));
			for (int ip = 0; ip < count; ++ip)
			{
				if (!allowed(tiles[slot]->polys[ip]))
				{
					nodes_[slot][static_cast<std::size_t>(ip)] = 0xffff;
					continue;
				}
				int& l = label[static_cast<std::size_t>(root(base[slot] + ip))];
				if (l < 0)
				{
					l = nodeCount_++;
					Point middle{};
					const dtPoly& poly = tiles[slot]->polys[ip];
					for (int v = 0; v < poly.vertCount; ++v)
						for (std::size_t axis = 0; axis < 3; ++axis)
							middle[axis] += tiles[slot]->verts[3 * poly.verts[v] + axis] / static_cast<float>(poly.vertCount);
					anchors_.push_back(middle);
				}
				nodes_[slot][static_cast<std::size_t>(ip)] = static_cast<std::uint16_t>(l);
			}
		}
	}

	TravelFilter travel_;
	ClusterId id_;
	std::unique_ptr<dtNavMesh, void (*)(dtNavMesh*)> nav_;
	std::unique_ptr<dtNavMeshQuery, void (*)(dtNavMeshQuery*)> query_{ nullptr, dtFreeNavMeshQuery };
	std::array<std::vector<std::uint16_t>, kSlots> nodes_;
	std::unordered_map<dtPolyRef, DoorKey> doorOf_;
	std::uint16_t nodeCount_ = 0;
	std::vector<Point> anchors_;
	std::vector<dtPolyRef> polys_;
	std::vector<float> straight_;
	std::vector<dtPolyRef> crossingRefs_;
};

// The widest overlap of a polygon edge with the edge across the boundary, per (side, chunk, node, other).
struct Widest
{
	float width = 0.0f;
	float at = 0.0f; // position along the boundary
	int slot = 0;
	int poly = 0;
	Point point{};
};

bool better(const Widest& a, const Widest& b)
{
	return std::tie(a.width, b.at, b.slot, b.poly) > std::tie(b.width, a.at, a.slot, a.poly);
}

std::optional<Cluster> computeCluster(const NavMesh& mesh, ClusterId id, const TravelFilter& travel)
{
	bool any = false;
	for (int slot = 0; slot < kSlots; ++slot)
		any = any || mesh.tile(id.x * kClusterTiles + slot % kClusterTiles, id.z * kClusterTiles + slot / kClusterTiles);
	if (!any)
		return std::nullopt;

	ClusterMesh self(mesh, id, travel);
	std::array<std::optional<ClusterMesh>, 4> across;
	const auto acrossMesh = [&](Side side) -> ClusterMesh&
	{
		auto& cached = across[static_cast<std::size_t>(side)];
		if (!cached)
			cached.emplace(mesh, neighbour(id, side), travel);
		return *cached;
	};

	std::map<std::tuple<int, int, int, int>, Widest> widest;
	const dtNavMesh& nav = mesh.detour();
	for (int slot = 0; slot < kSlots; ++slot)
	{
		const int tx = id.x * kClusterTiles + slot % kClusterTiles;
		const int tz = id.z * kClusterTiles + slot / kClusterTiles;
		const dtMeshTile* tile = nav.getTileAt(tx, tz, 0);
		for (int ip = 0; tile && ip < tile->header->polyCount; ++ip)
		{
			const dtPoly& poly = tile->polys[ip];
			if (0xffff == self.node(slot, ip))
				continue;
			const dtPolyRef myRef = nav.getPolyRefBase(tile) | static_cast<dtPolyRef>(ip);
			for (unsigned k = poly.firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
			{
				const dtLink& link = tile->links[k];
				unsigned salt, it, iq;
				nav.decodePolyId(link.ref, salt, it, iq);
				const dtMeshTile* far = nav.getTile(static_cast<int>(it));
				const ClusterId farId = clusterOfTile(far->header->x, far->header->y);
				const int dx = farId.x - id.x;
				const int dz = farId.z - id.z;
				if (dx == 0 && dz == 0)
					continue;
				if (std::abs(dx) + std::abs(dz) != 1)
					continue;
				const Side side = dx < 0 ? Side::West : dx > 0 ? Side::East : dz < 0 ? Side::South : Side::North;
				const int axis = dx != 0 ? 2 : 0; // along the boundary
				const std::uint16_t otherNode = acrossMesh(side).node(slotOf(farId, far->header->x, far->header->y), static_cast<int>(iq));
				if (0xffff == otherNode)
					continue;

				// The far polygon's edge on this boundary, found by its link back.
				const dtPoly& farPoly = far->polys[iq];
				int farEdge = -1;
				for (unsigned back = farPoly.firstLink; back != DT_NULL_LINK; back = far->links[back].next)
					if (far->links[back].ref == myRef)
						farEdge = far->links[back].edge;
				if (farEdge < 0)
					continue;
				const float* a = &tile->verts[3 * poly.verts[link.edge]];
				const float* b = &tile->verts[3 * poly.verts[(link.edge + 1) % poly.vertCount]];
				const float* c = &far->verts[3 * farPoly.verts[farEdge]];
				const float* d = &far->verts[3 * farPoly.verts[(farEdge + 1) % farPoly.vertCount]];
				const float low = std::max(std::min(a[axis], b[axis]), std::min(c[axis], d[axis]));
				const float high = std::min(std::max(a[axis], b[axis]), std::max(c[axis], d[axis]));
				if (high <= low)
					continue;

				Widest here;
				here.width = high - low;
				here.at = (low + high) * 0.5f;
				here.slot = slot;
				here.poly = ip;
				const float along = b[axis] == a[axis] ? 0.0f : (here.at - a[axis]) / (b[axis] - a[axis]);
				for (int i = 0; i < 3; ++i)
					here.point[static_cast<std::size_t>(i)] = a[i] + (b[i] - a[i]) * along;
				const auto key = std::make_tuple(static_cast<int>(side), dx != 0 ? tz - id.z * kClusterTiles : tx - id.x * kClusterTiles,
					static_cast<int>(self.node(slot, ip)), static_cast<int>(otherNode));
				const auto found = widest.find(key);
				if (found == widest.end())
					widest.emplace(key, here);
				else if (better(here, found->second))
					found->second = here;
			}
		}
	}

	Cluster cluster;
	cluster.nodeCount = self.nodeCount();
	cluster.anchors = self.anchors();
	for (const auto& [key, best] : widest)
	{
		const auto& [side, chunk, node, other] = key;
		cluster.portals.push_back({ static_cast<Side>(side), static_cast<std::uint8_t>(chunk), static_cast<std::uint16_t>(node),
			static_cast<std::uint16_t>(other), static_cast<std::uint8_t>(best.slot), static_cast<std::uint16_t>(best.poly), best.point });
	}

	cluster.start.push_back(0);
	std::vector<Point> path;
	std::vector<DoorCross> crossed;
	for (std::size_t j = 0; j < cluster.portals.size(); ++j)
	{
		for (std::size_t i = 0; i < j; ++i)
		{
			float cost = -1.0f;
			if (cluster.portals[i].node == cluster.portals[j].node
				&& self.path(portalLoc(id, cluster.portals[i]), portalLoc(id, cluster.portals[j]), path, cost, &crossed))
			{
				// Only the corners between the portals; the portals' own points are the ends.
				if (path.size() > 2)
					cluster.points.insert(cluster.points.end(), path.begin() + 1, path.end() - 1);
				if (!crossed.empty())
				{
					// The way round the doors, if the cluster has one that really avoids them.
					DoorEdge edge{ static_cast<std::uint32_t>(Cluster::pairIndex(i, j)), crossed, -1.0f, {} };
					std::vector<DoorCross> around;
					if (self.path(portalLoc(id, cluster.portals[i]), portalLoc(id, cluster.portals[j]), path, edge.bypassCost,
							&around, true) && around.empty())
					{
						if (path.size() > 2)
							edge.bypass.assign(path.begin() + 1, path.end() - 1);
					} else {
						edge.bypassCost = -1.0f;
					}
					cluster.doorEdges.push_back(std::move(edge));
				}
			}
			cluster.cost.push_back(cost);
			cluster.start.push_back(static_cast<std::uint32_t>(cluster.points.size()));
		}
	}
	return cluster;
}

template <class T>
void put(std::vector<std::uint8_t>& out, const T& value)
{
	const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
	out.insert(out.end(), bytes, bytes + sizeof(T));
}

struct Reader
{
	std::span<const std::uint8_t> bytes;
	std::size_t at = 0;

	template <class T>
	T get()
	{
		if (bytes.size() - at < sizeof(T))
			throw kworld::Error("coarse graph is truncated");
		T value;
		std::memcpy(&value, bytes.data() + at, sizeof(T));
		at += sizeof(T);
		return value;
	}
};

} // namespace

ClusterId clusterOfTile(int tileX, int tileZ)
{
	return { tileX / kClusterTiles, tileZ / kClusterTiles };
}

const DoorEdge* Cluster::doorEdge(std::size_t i, std::size_t j) const noexcept
{
	const auto pair = static_cast<std::uint32_t>(i < j ? pairIndex(i, j) : pairIndex(j, i));
	const auto found = std::lower_bound(doorEdges.begin(), doorEdges.end(), pair,
		[](const DoorEdge& edge, std::uint32_t at) { return edge.pair < at; });
	return found != doorEdges.end() && found->pair == pair ? &*found : nullptr;
}

Cluster::Crossing Cluster::crossing(std::size_t i, std::size_t j, const TravelFilter& travel,
	const DoorQuery& doors) const noexcept
{
	Crossing best{ costBetween(i, j), false };
	const DoorEdge* edge = best.cost < 0.0f ? nullptr : doorEdge(i, j);
	if (!edge || !doors.book)
		return best;
	for (const DoorCross& door : edge->doors)
	{
		const DoorVerdict verdict = doors.book->get(door.door).verdict(doors.throughLocks);
		if (verdict == DoorVerdict::Blocked)
		{
			best.cost = -1.0f;
			break;
		}
		if (verdict == DoorVerdict::Wait)
			best.cost += door.length * (travel.cost[static_cast<std::size_t>(Area::DoorClosed)]
				- travel.cost[static_cast<std::size_t>(Area::DoorOpen)]);
	}
	if (edge->bypassCost >= 0.0f && (best.cost < 0.0f || edge->bypassCost < best.cost))
		best = { edge->bypassCost, true };
	return best;
}

std::vector<Point> Cluster::pathBetween(std::size_t i, std::size_t j, bool bypass) const
{
	const std::size_t pair = i < j ? pairIndex(i, j) : pairIndex(j, i);
	std::vector<Point> path;
	if (bypass)
		path = doorEdge(i, j)->bypass;
	else
		path.assign(points.begin() + start[pair], points.begin() + start[pair + 1]);
	if (i > j)
		std::reverse(path.begin(), path.end());
	path.insert(path.begin(), portals[i].point);
	path.push_back(portals[j].point);
	return path;
}

Point CoarsePath::positionAt(float distance) const
{
	float remaining = std::clamp(distance, 0.0f, length);
	const Point* last = nullptr;
	for (const CoarseLeg& leg : legs)
	{
		for (std::size_t i = 1; i < leg.points.size(); ++i)
		{
			const Point& a = leg.points[i - 1];
			const Point& b = leg.points[i];
			const float segment = ::knav::distance(a, b);
			if (remaining <= segment && segment > 0.0f)
			{
				const float t = remaining / segment;
				return { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t };
			}
			remaining -= segment;
		}
		if (!leg.points.empty())
			last = &leg.points.back();
	}
	return last ? *last : Point{};
}

TileRect CoarseGraph::clustersOf(const TileRect& tiles)
{
	return { tiles.x0 / kClusterTiles, tiles.z0 / kClusterTiles, tiles.x1 / kClusterTiles, tiles.z1 / kClusterTiles };
}

namespace {

// Computes the clusters on `threads` workers; the result does not depend on the count.
std::vector<std::optional<Cluster>> computeAll(const NavMesh& mesh, const std::vector<ClusterId>& ids, int threads,
	const TravelFilter& travel)
{
	std::vector<std::optional<Cluster>> built(ids.size());
	std::atomic<std::size_t> next{ 0 };
	const auto work = [&]
	{
		for (std::size_t i = next++; i < ids.size(); i = next++)
			built[i] = computeCluster(mesh, ids[i], travel);
	};
	std::vector<std::thread> pool;
	for (int i = 1; i < std::max(threads, 1); ++i)
		pool.emplace_back(work);
	work();
	for (std::thread& thread : pool)
		thread.join();
	return built;
}

} // namespace

CoarseGraph CoarseGraph::build(const NavMesh& mesh, const TileRect& clusters, int threads,
	const TravelFilter& travel)
{
	std::vector<ClusterId> ids;
	for (int z = clusters.z0; z <= clusters.z1; ++z)
		for (int x = clusters.x0; x <= clusters.x1; ++x)
			ids.push_back({ x, z });
	auto built = computeAll(mesh, ids, threads, travel);
	CoarseGraph graph;
	graph.travel_ = travel;
	for (std::size_t i = 0; i < ids.size(); ++i)
		if (built[i])
			graph.clusters_.emplace(ids[i], std::move(*built[i]));
	return graph;
}

void CoarseGraph::merge(CoarseGraph&& other)
{
	if (clusters_.empty())
		travel_ = other.travel_;
	else if (!(travel_ == other.travel_))
		throw kworld::Error("coarse graphs differ in travel filter");
	for (auto& [id, cluster] : other.clusters_)
		clusters_[id] = std::move(cluster);
}

std::size_t CoarseGraph::update(const NavMesh& mesh, const std::vector<TileCoord>& changed, int threads)
{
	std::set<ClusterId> affected;
	for (const TileCoord& tile : changed)
	{
		// A cluster reads its own tiles and the region numbering of its four neighbours, which a
		// change anywhere in a neighbour can renumber, so the neighbours are recomputed too.
		const ClusterId id = clusterOfTile(tile.x, tile.z);
		affected.insert(id);
		for (const Side side : { Side::West, Side::East, Side::South, Side::North })
			affected.insert(neighbour(id, side));
	}
	std::erase_if(affected, [](ClusterId id) { return id.x < 0 || id.z < 0; });
	const std::vector<ClusterId> ids(affected.begin(), affected.end());
	auto built = computeAll(mesh, ids, threads, travel_);
	for (std::size_t i = 0; i < ids.size(); ++i)
	{
		if (built[i])
			clusters_[ids[i]] = std::move(*built[i]);
		else
			clusters_.erase(ids[i]);
	}
	return ids.size();
}

const Cluster* CoarseGraph::cluster(ClusterId id) const noexcept
{
	const auto found = clusters_.find(id);
	return found == clusters_.end() ? nullptr : &found->second;
}

CoarseStats CoarseGraph::stats() const noexcept
{
	CoarseStats stats;
	stats.clusters = clusters_.size();
	for (const auto& [id, cluster] : clusters_)
	{
		stats.nodes += cluster.nodeCount;
		stats.portals += cluster.portals.size();
		stats.points += cluster.points.size();
		stats.pairs += static_cast<std::size_t>(std::count_if(cluster.cost.begin(), cluster.cost.end(),
			[](float cost) { return cost >= 0.0f; }));
	}
	return stats;
}

PathResult CoarseGraph::refine(const NavMesh& mesh, const CoarseLeg& leg, const DoorQuery& doors) const
{
	PathResult result;
	if (leg.hop)
	{
		result.points = leg.points;
	} else {
		TravelFilter travel = travel_;
		travel.throughLocks = doors.throughLocks;
		ClusterMesh scratch(mesh, leg.cluster, travel, doors.book);
		float cost;
		if (!scratch.path(leg.from, leg.to, result.points, cost))
			return result;
	}
	result.found = true;
	result.complete = true;
	result.length = pathLength(result.points);
	return result;
}

CoarsePath CoarseGraph::findPath(const NavMesh& mesh, const Point& from, const Point& to,
	const Point& halfExtents, const DoorQuery& doors) const
{
	return findPath(mesh, mesh, from, to, halfExtents, doors);
}

CoarsePath CoarseGraph::findPath(const NavMesh& fromMesh, const NavMesh& toMesh, const Point& from,
	const Point& to, const Point& halfExtents, const DoorQuery& doors) const
{
	CoarsePath result;
	PolyLoc start;
	PolyLoc end;
	// Locating the ends ignores locks, so the result does not depend on which door state the meshes happen to have applied.
	TravelFilter travel = travel_;
	travel.throughLocks = doors.throughLocks;
	TravelFilter locating = travel_;
	locating.throughLocks = true;
	if (!NavQuery(fromMesh).locate(from, halfExtents, start, locating))
	{
		result.failure = CoarsePath::Failure::StartOffMesh;
		return result;
	}
	if (!NavQuery(toMesh).locate(to, halfExtents, end, locating))
	{
		result.failure = CoarsePath::Failure::EndOffMesh;
		return result;
	}
	const ClusterId startId = clusterOfTile(start.tileX, start.tileZ);
	const ClusterId endId = clusterOfTile(end.tileX, end.tileZ);
	const Cluster* startCluster = cluster(startId);
	const Cluster* endCluster = cluster(endId);
	if (!startCluster || !endCluster)
	{
		result.failure = CoarsePath::Failure::NotBuilt;
		return result;
	}
	result.failure = CoarsePath::Failure::NoRoute;

	ClusterMesh startMesh(fromMesh, startId, travel, doors.book);
	std::optional<ClusterMesh> otherEndMesh;
	if (endId != startId)
		otherEndMesh.emplace(toMesh, endId, travel, doors.book);
	ClusterMesh& endMesh = otherEndMesh ? *otherEndMesh : startMesh;
	const std::uint16_t startNode = startMesh.node(slotOf(startId, start.tileX, start.tileZ), start.poly);
	const std::uint16_t endNode = endMesh.node(slotOf(endId, end.tileX, end.tileZ), end.poly);

	const auto finish = [&](std::vector<CoarseLeg> legs, float total)
	{
		result.legs = std::move(legs);
		result.cost = total;
		for (const CoarseLeg& leg : result.legs)
			result.length += leg.length;
		result.found = true;
		result.failure = CoarsePath::Failure::None;
		return result;
	};
	const auto leg = [](ClusterId id, const PolyLoc& legFrom, const PolyLoc& legTo, std::vector<Point> points)
	{
		return CoarseLeg{ id, false, legFrom, legTo, pathLength(points), std::move(points) };
	};

	std::vector<Point> points;
	std::vector<DoorCross> crossed;
	float cost = 0.0f;
	const auto noteDoors = [&](const std::vector<DoorCross>& along)
	{
		for (const DoorCross& door : along)
			if (result.doors.empty() || result.doors.back() != door.door)
				result.doors.push_back(door.door);
	};
	if (startId == endId && startNode == endNode && startMesh.path(start, end, points, cost, &crossed))
	{
		noteDoors(crossed);
		return finish({ leg(startId, start, end, points) }, cost);
	}

	// Search the portals: seeded from the start, goal reached through the end cluster's portals.
	std::unordered_map<std::size_t, float> toEnd; // end-cluster portal -> path cost to `end`
	for (std::size_t i = 0; i < endCluster->portals.size(); ++i)
		if (endCluster->portals[i].node == endNode && endMesh.path(portalLoc(endId, endCluster->portals[i]), end, points, cost))
			toEnd[i] = cost;

	struct Record
	{
		float g = 0.0f;
		std::uint64_t parent = kGoal; // kGoal: seeded from the start
		bool closed = false;
	};
	std::unordered_map<std::uint64_t, Record> records;
	using Entry = std::pair<float, std::uint64_t>;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
	const auto relax = [&](std::uint64_t key, float g, std::uint64_t parent, const Point& at)
	{
		auto [slot, fresh] = records.try_emplace(key);
		if (!fresh && (slot->second.closed || slot->second.g <= g))
			return;
		slot->second = { g, parent, false };
		open.push({ g + (key == kGoal ? 0.0f : distance(at, to) * travel_.minCost()), key });
	};
	for (std::size_t i = 0; i < startCluster->portals.size(); ++i)
		if (startCluster->portals[i].node == startNode
			&& startMesh.path(start, portalLoc(startId, startCluster->portals[i]), points, cost))
			relax(nodeKey(startId, i), cost, kGoal, startCluster->portals[i].point);

	while (!open.empty())
	{
		const std::uint64_t key = open.top().second;
		open.pop();
		Record& record = records[key];
		if (record.closed)
			continue;
		record.closed = true;
		if (key == kGoal)
			break;
		const float g = record.g;
		const ClusterId id = keyCluster(key);
		const Cluster& here = *cluster(id);
		const std::size_t i = keyPortal(key);
		const Portal& portal = here.portals[i];
		for (std::size_t j = 0; j < here.portals.size(); ++j)
			if (j != i && here.portals[j].node == portal.node)
				if (const float edge = here.crossing(i, j, travel_, doors).cost; edge >= 0.0f)
					relax(nodeKey(id, j), g + edge, key, here.portals[j].point);
		const ClusterId farId = neighbour(id, portal.side);
		if (const Cluster* far = cluster(farId))
		{
			for (std::size_t j = 0; j < far->portals.size(); ++j)
			{
				const Portal& across = far->portals[j];
				if (across.side == opposite(portal.side) && across.chunk == portal.chunk
					&& across.node == portal.other && across.other == portal.node)
					relax(nodeKey(farId, j), g + distance(portal.point, across.point), key, across.point);
			}
		}
		if (id == endId && portal.node == endNode)
			if (const auto reach = toEnd.find(i); reach != toEnd.end())
				relax(kGoal, g + reach->second, key, to);
	}
	const auto goal = records.find(kGoal);
	if (goal == records.end() || !goal->second.closed)
		return result;

	// Walk the parents back to the seed, then lay the legs out start to end.
	std::vector<std::uint64_t> chain;
	for (std::uint64_t key = goal->second.parent; key != kGoal; key = records[key].parent)
		chain.push_back(key);
	std::reverse(chain.begin(), chain.end());

	std::vector<CoarseLeg> legs;
	const auto loc = [&](std::uint64_t key)
	{
		return portalLoc(keyCluster(key), cluster(keyCluster(key))->portals[keyPortal(key)]);
	};
	startMesh.path(start, loc(chain.front()), points, cost, &crossed);
	noteDoors(crossed);
	legs.push_back(leg(startId, start, loc(chain.front()), points));
	for (std::size_t n = 1; n < chain.size(); ++n)
	{
		const ClusterId id = keyCluster(chain[n - 1]);
		if (id == keyCluster(chain[n]))
		{
			const Cluster& here = *cluster(id);
			const Cluster::Crossing edge = here.crossing(keyPortal(chain[n - 1]), keyPortal(chain[n]), travel_, doors);
			points = here.pathBetween(keyPortal(chain[n - 1]), keyPortal(chain[n]), edge.bypass);
			if (const DoorEdge* through = edge.bypass ? nullptr : here.doorEdge(keyPortal(chain[n - 1]), keyPortal(chain[n])))
				noteDoors(through->doors);
			legs.push_back(leg(id, loc(chain[n - 1]), loc(chain[n]), points));
		} else {
			legs.push_back(leg(id, loc(chain[n - 1]), loc(chain[n]), { loc(chain[n - 1]).point, loc(chain[n]).point }));
			legs.back().hop = true;
		}
	}
	endMesh.path(loc(chain.back()), end, points, cost, &crossed);
	noteDoors(crossed);
	legs.push_back(leg(endId, loc(chain.back()), end, points));
	return finish(std::move(legs), goal->second.g);
}

std::vector<std::uint8_t> CoarseGraph::serialize() const
{
	std::vector<std::uint8_t> out(kMagic, kMagic + sizeof(kMagic));
	put(out, kGraphVersion);
	put<std::uint32_t>(out, kClusterTiles);
	for (const float cost : travel_.cost)
		put(out, cost);
	put(out, travel_.allowed);
	put<std::uint32_t>(out, static_cast<std::uint32_t>(clusters_.size()));
	for (const auto& [id, cluster] : clusters_)
	{
		put<std::int32_t>(out, id.x);
		put<std::int32_t>(out, id.z);
		put<std::uint16_t>(out, cluster.nodeCount);
		for (const Point& anchor : cluster.anchors)
			for (const float v : anchor)
				put(out, v);
		put<std::uint32_t>(out, static_cast<std::uint32_t>(cluster.portals.size()));
		for (const Portal& portal : cluster.portals)
		{
			put<std::uint8_t>(out, static_cast<std::uint8_t>(portal.side));
			put(out, portal.chunk);
			put(out, portal.node);
			put(out, portal.other);
			put(out, portal.slot);
			put(out, portal.poly);
			for (const float v : portal.point)
				put(out, v);
		}
		for (std::size_t pair = 0; pair < cluster.cost.size(); ++pair)
		{
			put(out, cluster.cost[pair]);
			put<std::uint32_t>(out, cluster.start[pair + 1] - cluster.start[pair]);
			for (std::uint32_t p = cluster.start[pair]; p < cluster.start[pair + 1]; ++p)
				for (const float v : cluster.points[p])
					put(out, v);
		}
		put<std::uint32_t>(out, static_cast<std::uint32_t>(cluster.doorEdges.size()));
		for (const DoorEdge& edge : cluster.doorEdges)
		{
			put(out, edge.pair);
			put<std::uint32_t>(out, static_cast<std::uint32_t>(edge.doors.size()));
			for (const DoorCross& door : edge.doors)
			{
				put(out, door.door);
				put(out, door.length);
			}
			put(out, edge.bypassCost);
			put<std::uint32_t>(out, static_cast<std::uint32_t>(edge.bypass.size()));
			for (const Point& point : edge.bypass)
				for (const float v : point)
					put(out, v);
		}
	}
	return out;
}

CoarseGraph CoarseGraph::deserialize(std::span<const std::uint8_t> bytes)
{
	Reader in{ bytes };
	if (bytes.size() < sizeof(kMagic) || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
		throw kworld::Error("not a coarse graph");
	in.at = sizeof(kMagic);
	const auto version = in.get<std::uint32_t>();
	if ((version != kGraphVersion && version != 3 && version != 2) || in.get<std::uint32_t>() != kClusterTiles)
		throw kworld::Error("unsupported coarse graph version or cluster size");
	CoarseGraph graph;
	// Version 2 knew three areas, 3 five: the newer areas keep their defaults and are always allowed.
	const std::size_t areas = version == 2 ? 3u : version == 3 ? 5u : static_cast<std::size_t>(kAreaCount);
	for (std::size_t area = 0; area < areas; ++area)
		graph.travel_.cost[area] = in.get<float>();
	graph.travel_.allowed = in.get<std::uint16_t>();
	if (version == 2)
		graph.travel_.allowed |= areaFlag(Area::DoorOpen) | areaFlag(Area::DoorClosed);
	if (version <= 3)
		graph.travel_.allowed |= areaFlag(Area::Edge);
	for (auto count = in.get<std::uint32_t>(); count > 0; --count)
	{
		ClusterId id;
		id.x = in.get<std::int32_t>();
		id.z = in.get<std::int32_t>();
		Cluster cluster;
		cluster.nodeCount = in.get<std::uint16_t>();
		for (std::uint16_t i = 0; i < cluster.nodeCount; ++i)
		{
			Point anchor;
			for (float& v : anchor)
				v = in.get<float>();
			cluster.anchors.push_back(anchor);
		}
		const auto portals = in.get<std::uint32_t>();
		if (portals > in.bytes.size())
			throw kworld::Error("coarse graph has an impossible portal count");
		for (std::uint32_t i = 0; i < portals; ++i)
		{
			Portal portal;
			portal.side = static_cast<Side>(in.get<std::uint8_t>());
			portal.chunk = in.get<std::uint8_t>();
			portal.node = in.get<std::uint16_t>();
			portal.other = in.get<std::uint16_t>();
			portal.slot = in.get<std::uint8_t>();
			portal.poly = in.get<std::uint16_t>();
			for (float& v : portal.point)
				v = in.get<float>();
			cluster.portals.push_back(portal);
		}
		cluster.start.push_back(0);
		for (std::size_t pair = 0, pairs = portals ? std::size_t{ portals } * (portals - 1) / 2 : 0; pair < pairs; ++pair)
		{
			cluster.cost.push_back(in.get<float>());
			for (auto points = in.get<std::uint32_t>(); points > 0; --points)
			{
				Point point;
				for (float& v : point)
					v = in.get<float>();
				cluster.points.push_back(point);
			}
			cluster.start.push_back(static_cast<std::uint32_t>(cluster.points.size()));
		}
		for (auto edges = version == 2 ? 0u : in.get<std::uint32_t>(); edges > 0; --edges)
		{
			DoorEdge edge;
			edge.pair = in.get<std::uint32_t>();
			for (auto doors = in.get<std::uint32_t>(); doors > 0; --doors)
			{
				DoorCross door;
				door.door = in.get<DoorKey>();
				door.length = in.get<float>();
				edge.doors.push_back(door);
			}
			edge.bypassCost = in.get<float>();
			for (auto corners = in.get<std::uint32_t>(); corners > 0; --corners)
			{
				Point point;
				for (float& v : point)
					v = in.get<float>();
				edge.bypass.push_back(point);
			}
			cluster.doorEdges.push_back(std::move(edge));
		}
		graph.clusters_.emplace(id, std::move(cluster));
	}
	if (in.at != bytes.size())
		throw kworld::Error("coarse graph has trailing bytes");
	return graph;
}

void CoarseGraph::save(const std::string& path) const
{
	const std::vector<std::uint8_t> bytes = serialize();
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (!out)
		throw kworld::Error("cannot write " + path);
}

CoarseGraph CoarseGraph::load(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		throw kworld::Error("cannot open " + path);
	const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return deserialize(bytes);
}

} // namespace knav
