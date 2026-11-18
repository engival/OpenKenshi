#include "ksim/nav/navigator.hpp"

#include "knav/area.hpp"
#include "knav/coarse.hpp"
#include "knav/native.hpp"
#include "knav/native_tiles.hpp"
#include "knav/seeds.hpp"
#include "knav/navmesh.hpp"
#include "knav/world.hpp"
#include "kworld/collision_area.hpp"
#include "kworld/coords.hpp"
#include "kworld/error.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/navhash_source.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

namespace ksim {

namespace {

using RegionKey = std::pair<int, int>;

constexpr int kRegionTiles = knav::kRegionZones * knav::kTilesPerZone;

// Detour A* node pool of a local query, one query per region.
constexpr int kLocalNodes = 8192;
// MeshNavigator's own query: a click is routed whole or refused, so the search must not run dry early
// (the original runs 100000 iterations).
constexpr int kClickNodes = 32768; // Detour's pool is at most 65535
// The wide retry window has the larger pool.
constexpr int kWideNodes = 16384;
// A stale zone's rebake (one zone; the Hub is ~0.8 s of gather and bake on this).
constexpr int kNativeBakeThreads = 4;

// UNSURE: how far a start or end may be from the mesh (a town centre is often
// inside a building). Horizontal 64, vertical 128 around the terrain height.
const knav::Point kReach = {64.0f, 128.0f, 64.0f};

std::uint64_t nowNs()
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

RegionKey regionOf(double x, double z)
{
	const kworld::ZoneCell cell = kworld::zoneCellOf(x, z);
	return {cell.x / knav::kRegionZones, cell.z / knav::kRegionZones};
}

class UnionFind
{
public:
	std::uint32_t add(std::uint32_t count)
	{
		const std::uint32_t base = static_cast<std::uint32_t>(parent_.size());
		parent_.resize(parent_.size() + count);
		std::iota(parent_.begin() + base, parent_.end(), base);
		return base;
	}

	std::uint32_t find(std::uint32_t a)
	{
		while (parent_[a] != a)
		{
			parent_[a] = parent_[parent_[a]];
			a = parent_[a];
		}
		return a;
	}

	// The smaller root wins: deterministic labels.
	void join(std::uint32_t a, std::uint32_t b)
	{
		a = find(a);
		b = find(b);
		if (a != b)
			parent_[std::max(a, b)] = std::min(a, b);
	}

private:
	std::vector<std::uint32_t> parent_;
};

// What a Detour search failure means for the server log (Route::check).
std::string_view pathCheck(knav::PathResult::Failure failure) noexcept
{
	switch (failure)
	{
	case knav::PathResult::Failure::StartOffMesh: return "start: no walkable poly near the start";
	case knav::PathResult::Failure::EndOffMesh: return "goal: no walkable poly near the goal";
	case knav::PathResult::Failure::NoPath: return "path: goal poly not connected to the start (another island / layer, no stair link)";
	case knav::PathResult::Failure::Closed: return "path: search closed short of the goal (a gap too narrow for the body or a closed door)";
	case knav::PathResult::Failure::OutOfNodes: return "path: out of search nodes";
	case knav::PathResult::Failure::None: break;
	}
	return "path: incomplete";
}

} // namespace

struct KnavNavigator::Impl
{
	explicit Impl(kworld::Heightmap terrainMap)
		: terrain(std::move(terrainMap))
	{
	}

	knav::Point ground(double x, double z) const
	{
		return {static_cast<float>(x), static_cast<float>(terrain.height(x, z)),
			static_cast<float>(z)};
	}

	const knav::NavMesh* meshAt(double x, double z) const
	{
		const auto found = meshes.find(regionOf(x, z));
		return found == meshes.end() ? nullptr : &found->second;
	}

	// One query per region, made on first use (a NavQuery allocates its node
	// pool): the navigator is used from the serial squad step only.
	const knav::NavQuery& queryAt(const RegionKey& key, const knav::NavMesh& mesh) const
	{
		std::unique_ptr<knav::NavQuery>& slot = queries[key];
		if (!slot)
			slot = std::make_unique<knav::NavQuery>(mesh, kLocalNodes);
		return *slot;
	}

	// The mesh for a local walk between two regions: the tiles of the box both ends are in,
	// plus `margin` tiles round it, gathered from the region meshes. Detour links the
	// tiles of one mesh across the region seams, so a path can leave a region and come back.
	// Built per walk (about 2 us a tile): a cache would not hit, squads are spread over the
	// whole world and every walk has its own box. A box with more tiles than one mesh holds (the ends
	// are regions apart: an imported squad far from its goal) gives a Window with no query.
	struct Window
	{
		std::unique_ptr<knav::NavMesh> mesh;
		std::unique_ptr<knav::NavQuery> query;
	};

	static constexpr int kWindowMargin = 4;
	static constexpr int kWideMargin = 12;

	Window windowFor(double fromX, double fromZ, double toX, double toZ,
		const knav::NavConfig& config, int margin, int nodes) const
	{
		auto tile = [&](double v, double origin)
		{
			return static_cast<int>(std::floor((v - origin) / config.tileSize));
		};
		const int ax = tile(fromX, config.originX);
		const int az = tile(fromZ, config.originZ);
		const int bx = tile(toX, config.originX);
		const int bz = tile(toZ, config.originZ);
		const knav::TileRect box{std::max(0, std::min(ax, bx) - margin),
			std::max(0, std::min(az, bz) - margin),
			std::min(config.tilesX - 1, std::max(ax, bx) + margin),
			std::min(config.tilesZ - 1, std::max(az, bz) + margin)};
		if (box.area() > knav::kMaxResidentTiles)
			return {};
		std::vector<knav::BakedTile> gathered;
		for (int z = box.z0; z <= box.z1; ++z)
		{
			for (int x = box.x0; x <= box.x1; ++x)
			{
				const auto region = meshes.find({x / kRegionTiles, z / kRegionTiles});
				const knav::BakedTile* found = region == meshes.end() ? nullptr : region->second.tile(x, z);
				if (found != nullptr)
					gathered.push_back(*found);
			}
		}
		knav::NavConfig windowConfig = config;
		windowConfig.window = box;
		Window made;
		made.mesh = std::make_unique<knav::NavMesh>(
			knav::NavMesh::fromTiles(windowConfig, std::move(gathered)));
		if (!book.settings().empty())
			made.mesh->applyDoors(book);
		made.query = std::make_unique<knav::NavQuery>(*made.mesh, nodes);
		return made;
	}

	// Connected sets of polygons over the whole loaded world, regions joined across their seams:
	// two points are on the same walkable ground when their polygons share a label. A local goal
	// is picked among the polygons of the squad's own set, so it cannot be unreachable.
	// The swim-capable travel filter of the graph decides what counts as passable.
	//
	// Doors are not part of the sets: a door polygon has no label, and the sets it joins are listed
	// in `doorSets` (door key -> labels of the polygons beside it). Which of them a traveller class
	// may cross is the book's business (classTable), so a gate closing relabels nothing.
	void labelComponents()
	{
		const std::uint16_t flags = graph.travel().allowed
			& ~(knav::areaFlag(knav::Area::DoorOpen) | knav::areaFlag(knav::Area::DoorClosed));
		UnionFind sets;
		std::map<RegionKey, std::uint32_t> base;
		std::map<knav::DoorKey, std::vector<std::uint32_t>> beside; // union-find ids
		for (const auto& [key, mesh] : meshes)
		{
			knav::PolyComponents& labels = components.emplace(key, knav::PolyComponents(mesh, flags)).first->second;
			base[key] = sets.add(labels.count());
			for (const knav::DoorKey door : mesh.doorKeys())
				for (const std::uint32_t label : labels.around(mesh.doorPolys(door)))
					beside[door].push_back(base[key] + label);
		}
		// A strip of the two tile columns (rows) either side of a seam links the regions.
		auto strip = [&](const RegionKey& a, const knav::TileRect& box)
		{
			const knav::NavMesh& first = meshes.at(a);
			std::vector<knav::BakedTile> gathered;
			for (int z = box.z0; z <= box.z1; ++z)
			{
				for (int x = box.x0; x <= box.x1; ++x)
				{
					const RegionKey home{x / kRegionTiles, z / kRegionTiles};
					const auto region = meshes.find(home);
					const knav::BakedTile* tile = region == meshes.end() ? nullptr : region->second.tile(x, z);
					if (tile != nullptr)
						gathered.push_back(*tile);
				}
			}
			if (gathered.empty())
				return;
			knav::NavConfig config = first.config();
			config.window = box;
			const knav::NavMesh seam = knav::NavMesh::fromTiles(config, std::move(gathered));
			const knav::PolyComponents joined(seam, flags);
			const std::uint32_t at = sets.add(joined.count());
			for (const knav::DoorKey door : seam.doorKeys())
				for (const std::uint32_t label : joined.around(seam.doorPolys(door)))
					beside[door].push_back(at + label);
			joined.forEach([&](int x, int z, int poly, std::uint32_t label)
			{
				if (label == knav::PolyComponents::kNone)
					return;
				const RegionKey home{x / kRegionTiles, z / kRegionTiles};
				const std::uint32_t own = components.at(home).at(x, z, poly);
				if (own != knav::PolyComponents::kNone)
					sets.join(base.at(home) + own, at + label);
			});
		};
		for (const auto& entry : meshes)
		{
			const RegionKey& key = entry.first;
			const RegionKey east{key.first + 1, key.second};
			const RegionKey south{key.first, key.second + 1};
			const int x0 = key.first * kRegionTiles;
			const int z0 = key.second * kRegionTiles;
			if (meshes.contains(east))
				strip(key, {x0 + kRegionTiles - 1, z0, x0 + kRegionTiles, z0 + kRegionTiles - 1});
			if (meshes.contains(south))
				strip(key, {x0, z0 + kRegionTiles - 1, x0 + kRegionTiles - 1, z0 + kRegionTiles});
		}
		// Roots in ascending order of the first label met: a label table per region.
		for (auto& [key, labels] : components)
		{
			std::vector<std::uint32_t> table(labels.count());
			for (std::uint32_t i = 0; i < labels.count(); ++i)
				table[i] = sets.find(base.at(key) + i);
			labels.relabel(table);
		}
		doorSets.clear();
		for (auto& [door, ids] : beside)
		{
			std::vector<std::uint32_t>& roots = doorSets[door];
			for (const std::uint32_t id : ids)
				roots.push_back(sets.find(id));
			std::sort(roots.begin(), roots.end());
			roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
		}
	}

	// The set label a traveller class stands in, given the doors of the book: the sets joined by
	// a door it may use share one representative. Only sets beside a door ever differ from their
	// own label, so a table of the few hundred of them is all there is (built per class, on use).
	std::uint32_t classRep(std::uint32_t label, int doorClass) const
	{
		if (label == knav::PolyComponents::kNone)
			return label;
		ClassTable& slot = classes[static_cast<std::size_t>(doorClass) % classes.size()];
		if (!slot.valid)
			buildClass(slot, doorClass, false);
		const auto found = slot.rep.find(label);
		return found == slot.rep.end() ? label : found->second;
	}

	// Every door open to everyone: what the sets were before gates existed.
	std::uint32_t openRep(std::uint32_t label) const
	{
		if (label == knav::PolyComponents::kNone)
			return label;
		if (!open.valid)
			buildClass(open, 0, true);
		const auto found = open.rep.find(label);
		return found == open.rep.end() ? label : found->second;
	}

	struct ClassTable
	{
		bool valid = false;
		std::unordered_map<std::uint32_t, std::uint32_t> rep;
	};

	void buildClass(ClassTable& slot, int doorClass, bool everyDoorOpen) const
	{
		std::unordered_map<std::uint32_t, std::uint32_t> parent;
		const auto find = [&](std::uint32_t a)
		{
			auto at = parent.try_emplace(a, a).first;
			while (at->second != a)
			{
				a = at->second;
				at = parent.try_emplace(a, a).first;
			}
			return a;
		};
		for (const auto& [door, labels] : doorSets)
		{
			if (!everyDoorOpen && book.get(door).verdict(doorClass) == knav::DoorVerdict::Blocked)
				continue;
			for (std::size_t i = 1; i < labels.size(); ++i)
			{
				const std::uint32_t a = find(labels[0]);
				const std::uint32_t b = find(labels[i]);
				if (a != b)
					parent[std::max(a, b)] = std::min(a, b);
			}
		}
		slot.rep.clear();
		for (const auto& entry : parent)
			slot.rep[entry.first] = find(entry.first);
		slot.valid = true;
	}

	// The set of the polygon nearest `point` within reach (any area the filter allows).
	std::uint32_t componentAt(double x, double z, const knav::Point& reach) const
	{
		const knav::NavMesh* mesh = meshAt(x, z);
		if (mesh == nullptr)
			return knav::PolyComponents::kNone;
		knav::PolyLoc loc;
		if (!queryAt(regionOf(x, z), *mesh).locate(ground(x, z), reach, loc, graph.travel()))
			return knav::PolyComponents::kNone;
		return components.at(tileRegion(loc.tileX, loc.tileZ)).at(loc.tileX, loc.tileZ, loc.poly);
	}

	static RegionKey tileRegion(int tileX, int tileZ)
	{
		return {tileX / kRegionTiles, tileZ / kRegionTiles};
	}

	kworld::Heightmap terrain;
	std::map<RegionKey, knav::NavMesh> meshes;
	std::map<RegionKey, knav::PolyComponents> components;
	// The sets beside each door (labelComponents), the book in force, and the per-class view of it.
	std::map<knav::DoorKey, std::vector<std::uint32_t>> doorSets;
	knav::DoorBook book;
	mutable std::array<ClassTable, knav::kDoorClasses> classes;
	mutable ClassTable open;
	mutable std::map<RegionKey, std::unique_ptr<knav::NavQuery>> queries;
	knav::CoarseGraph graph;
};

KnavNavigator::KnavNavigator(std::unique_ptr<Impl> impl)
	: impl_(std::move(impl))
{
}

KnavNavigator::~KnavNavigator() = default;

std::unique_ptr<KnavNavigator> KnavNavigator::open(const std::string& dataDir,
	const std::string& bakeDir, int rx0, int rz0, int rx1, int rz1, int threads)
{
	auto impl = std::make_unique<Impl>(
		kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif"));
	for (int rz = rz0; rz <= rz1; ++rz)
	{
		for (int rx = rx0; rx <= rx1; ++rx)
		{
			// The graph needs a cluster of margin; routes only the region itself.
			const knav::NavMesh padded = knav::NavMesh::loadRegion(bakeDir, rx, rz,
				knav::kClusterTiles);
			const knav::TileRect own{rx * kRegionTiles, rz * kRegionTiles,
				(rx + 1) * kRegionTiles - 1, (rz + 1) * kRegionTiles - 1};
			impl->graph.merge(knav::CoarseGraph::build(padded,
				knav::CoarseGraph::clustersOf(own), threads));
			impl->meshes.emplace(RegionKey{rx, rz}, knav::NavMesh::loadRegion(bakeDir, rx, rz, 0));
		}
	}
	impl->labelComponents();
	return std::unique_ptr<KnavNavigator>(new KnavNavigator(std::move(impl)));
}

std::unique_ptr<KnavNavigator> KnavNavigator::openWithGraph(const std::string& dataDir,
	const std::string& bakeDir, int rx0, int rz0, int rx1, int rz1, const std::string& graphPath)
{
	auto impl = std::make_unique<Impl>(
		kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif"));
	impl->graph = knav::CoarseGraph::load(graphPath);
	for (int rz = rz0; rz <= rz1; ++rz)
	{
		for (int rx = rx0; rx <= rx1; ++rx)
			impl->meshes.emplace(RegionKey{rx, rz}, knav::NavMesh::loadRegion(bakeDir, rx, rz, 0));
	}
	impl->labelComponents();
	return std::unique_ptr<KnavNavigator>(new KnavNavigator(std::move(impl)));
}

std::optional<state::GroundPoint> Navigator::nearestWalkable(double x, double z, double, double) const
{
	if (!covers(x, z))
		return std::nullopt;
	return state::GroundPoint{x, z};
}

std::optional<state::GroundPoint> Navigator::nearestReachable(double x, double z, double reach,
	double, double) const
{
	return nearestWalkable(x, z, reach);
}

std::optional<state::GroundPoint> KnavNavigator::nearestWalkable(double x, double z,
	double reach, double radius) const
{
	const RegionKey key = regionOf(x, z);
	const knav::NavMesh* mesh = impl_->meshAt(x, z);
	if (mesh == nullptr)
		return std::nullopt;
	const std::uint64_t began = nowNs();
	const knav::NavQuery& query = impl_->queryAt(key, *mesh);
	const knav::Point from = impl_->ground(x, z);
	const float limit = static_cast<float>(reach);
	const knav::Point extents = {limit, 400.0f, limit};
	knav::PolyLoc found;
	const float clearance = static_cast<float>(radius);
	bool located = false;
	if (clearance > 0.0f)
	{
		// A body that wide: only ground where it fits (no wall within `radius` of the point).
		const auto fits = [&](const knav::PolyLoc& loc) { return query.wallDistance(loc, clearance) >= clearance; };
		located = query.locateWhere(from, extents, fits, found, knav::TravelFilter::dryOnly())
			|| query.locateWhere(from, extents, fits, found);
	} else {
		located = query.locate(from, extents, found, knav::TravelFilter::dryOnly())
			|| query.locate(from, extents, found);
	}
	++stats_.snapCalls;
	stats_.snapNs += nowNs() - began;
	if (!located)
		return std::nullopt;
	return state::GroundPoint{found.point[0], found.point[2]};
}

std::optional<state::GroundPoint> KnavNavigator::nearestReachable(double x, double z, double reach,
	double fromX, double fromZ) const
{
	return nearestReachableFor(Traveller{}, x, z, reach, fromX, fromZ);
}

std::optional<state::GroundPoint> KnavNavigator::nearestReachableFor(const Traveller& who, double x,
	double z, double reach, double fromX, double fromZ) const
{
	const knav::NavMesh* mesh = impl_->meshAt(x, z);
	const std::uint32_t set = impl_->classRep(impl_->componentAt(fromX, fromZ, kReach), who.doorClass);
	if (mesh == nullptr || set == knav::PolyComponents::kNone)
		return nearestWalkable(x, z, reach);
	const std::uint64_t began = nowNs();
	const knav::NavQuery& query = impl_->queryAt(regionOf(x, z), *mesh);
	const knav::Point from = impl_->ground(x, z);
	const float limit = static_cast<float>(reach);
	const knav::Point extents = {limit, 400.0f, limit};
	const auto sameSet = [&](const knav::PolyLoc& loc)
	{
		const std::uint32_t label = impl_->components.at(Impl::tileRegion(loc.tileX, loc.tileZ))
			.at(loc.tileX, loc.tileZ, loc.poly);
		if (impl_->classRep(label, who.doorClass) == set)
			return true;
		// Beyond a gate this class may not pass, though the ground is connected.
		if (!impl_->book.settings().empty() && label != knav::PolyComponents::kNone
			&& impl_->openRep(label) == impl_->openRep(set))
			++stats_.doorSkippedPolys;
		return false;
	};
	// A traveller with a body also needs the spot to fit it (NavQuery::wallDistance, the clearance rule).
	const float clearance = who.footprint;
	const auto usable = [&](const knav::PolyLoc& loc)
	{
		return sameSet(loc) && (clearance <= 0.0f || query.wallDistance(loc, clearance) >= clearance);
	};
	// The nearest polygon almost always is on the squad's ground; the full search is for the rest.
	knav::PolyLoc found;
	bool located = false;
	for (const knav::TravelFilter& filter : {knav::TravelFilter::dryOnly(), knav::TravelFilter{}})
	{
		located = query.locate(from, extents, found, filter) && usable(found);
		if (!located)
		{
			++stats_.snapSearches;
			located = query.locateWhere(from, extents, usable, found, filter);
		}
		if (located)
			break;
	}
	++stats_.snapCalls;
	stats_.snapNs += nowNs() - began;
	if (!located)
		return std::nullopt;
	return state::GroundPoint{found.point[0], found.point[2]};
}

bool KnavNavigator::covers(double x, double z) const
{
	return impl_->meshAt(x, z) != nullptr;
}

std::size_t KnavNavigator::clusterCount() const
{
	return impl_->graph.clusters().size();
}

Route KnavNavigator::localRoute(double fromX, double fromZ, double toX, double toZ) const
{
	return localRouteFor(Traveller{}, fromX, fromZ, toX, toZ);
}

Route KnavNavigator::localRouteFor(const Traveller& who, double fromX, double fromZ, double toX,
	double toZ) const
{
	knav::TravelFilter travel = impl_->graph.travel();
	travel.doorClass = static_cast<std::uint8_t>(who.doorClass);
	travel.radius = who.footprint;
	const RegionKey key = regionOf(fromX, fromZ);
	const knav::NavMesh* mesh = impl_->meshAt(fromX, fromZ);
	Route route;
	if (mesh == nullptr || impl_->meshAt(toX, toZ) == nullptr)
	{
		route.failure = state::AiReason::OutsideWindow;
		route.check = "window: an end is outside the loaded tiles";
		return route;
	}
	const std::uint64_t began = nowNs();
	// Inside one region its own mesh; over a border a window of tiles of both regions.
	const bool sameRegion = key == regionOf(toX, toZ);
	const knav::Point from = impl_->ground(fromX, fromZ);
	const knav::Point to = impl_->ground(toX, toZ);
	Impl::Window window;
	if (!sameRegion)
	{
		window = impl_->windowFor(fromX, fromZ, toX, toZ, mesh->config(), Impl::kWindowMargin, kLocalNodes);
		if (!window.query)
		{
			++stats_.localFailures[static_cast<std::size_t>(knav::PathResult::Failure::NoPath)];
			route.failure = state::AiReason::NoRoute;
			route.check = "window: no tile window joins the two regions";
			return route;
		}
	}
	const knav::NavQuery& query = sameRegion ? impl_->queryAt(key, *mesh) : *window.query;
	knav::PathResult path = query.findPath(from, to, kReach, travel);
	++stats_.localCalls;
	stats_.crossCalls += sameRegion ? 0 : 1;
	// The goal is on the start's connected ground (nearestReachable), so a search that closes
	// without reaching it was clipped: the way round leaves the region, or the window. One
	// retry in a wider window (a pool-exhausted search would only exhaust the larger one).
	if (path.failure == knav::PathResult::Failure::Closed)
	{
		++stats_.wideRetries;
		const Impl::Window wide = impl_->windowFor(fromX, fromZ, toX, toZ, mesh->config(),
			Impl::kWideMargin, kWideNodes);
		if (wide.query)
		{
			path = wide.query->findPath(from, to, kReach, travel);
			stats_.wideFound += path.complete ? 1 : 0;
		}
	}
	stats_.localNs += nowNs() - began;
	stats_.crossNs += sameRegion ? 0 : nowNs() - began;
	// Not handed to the coarse graph when it fails: proving a point unreachable there
	// means exploring the whole connected world (hundreds of ms).
	if (!path.found || !path.complete || path.points.empty())
	{
		++stats_.localFailures[static_cast<std::size_t>(path.failure)];
		stats_.crossFailures[static_cast<std::size_t>(path.failure)] += sameRegion ? 0 : 1;
		route.failure = path.failure == knav::PathResult::Failure::Closed
			|| path.failure == knav::PathResult::Failure::OutOfNodes
			|| path.failure == knav::PathResult::Failure::NoPath
			? state::AiReason::NoRoute : state::AiReason::OffMesh;
		route.check = pathCheck(path.failure);
		return route;
	}
	route.found = true;
	++stats_.localFound;
	stats_.crossFound += sameRegion ? 0 : 1;
	for (const knav::Point& point : path.points)
	{
		const state::GroundPoint next{point[0], point[2]};
		if (route.points.empty() || route.points.back().x != next.x
			|| route.points.back().z != next.z)
			route.points.push_back(next);
	}
	return route;
}

Route KnavNavigator::route(double fromX, double fromZ, double toX, double toZ) const
{
	return routeFor(Traveller{}, fromX, fromZ, toX, toZ);
}

Route KnavNavigator::routeFor(const Traveller& who, double fromX, double fromZ, double toX,
	double toZ) const
{
	const std::uint64_t began = nowNs();
	const Route answer = routeUntimed(who, fromX, fromZ, toX, toZ);
	++stats_.routeCalls;
	stats_.routeFound += answer.found ? 1 : 0;
	stats_.routeNs += nowNs() - began;
	return answer;
}

Route KnavNavigator::routeUntimed(const Traveller& who, double fromX, double fromZ, double toX,
	double toZ) const
{
	Route route;
	const knav::NavMesh* fromMesh = impl_->meshAt(fromX, fromZ);
	const knav::NavMesh* toMesh = impl_->meshAt(toX, toZ);
	if (fromMesh == nullptr || toMesh == nullptr)
	{
		route.failure = state::AiReason::OutsideWindow;
		route.check = "window: an end is outside the loaded meshes";
		return route;
	}
	// The connected sets know a route is impossible before the graph proves it (which explores
	// everything the start can reach): ends in different sets, as this class sees the doors, have
	// no route, and it is a gate that refuses them when the sets are one with every door open.
	const std::uint32_t fromLabel = impl_->componentAt(fromX, fromZ, kReach);
	const std::uint32_t toLabel = impl_->componentAt(toX, toZ, kReach);
	if (!who.bash && fromLabel != knav::PolyComponents::kNone && toLabel != knav::PolyComponents::kNone
		&& impl_->classRep(fromLabel, who.doorClass) != impl_->classRep(toLabel, who.doorClass))
	{
		const bool gate = impl_->openRep(fromLabel) == impl_->openRep(toLabel);
		route.failure = gate ? state::AiReason::DoorRefused : state::AiReason::NoRoute;
		route.check = gate ? "door set: the ends join only through a door this traveller is refused"
			: "island: the ends are in different connected components (no stair / link between them)";
		stats_.doorRefusedRoutes += gate ? 1 : 0;
		++stats_.routeSetRefusals;
		return route;
	}
	const bool gated = !impl_->book.settings().empty() && !who.bash;
	const knav::CoarsePath path = impl_->graph.findPath(*fromMesh, *toMesh,
		impl_->ground(fromX, fromZ), impl_->ground(toX, toZ), kReach,
		knav::DoorQuery{gated ? &impl_->book : nullptr, who.bash ? knav::kDoorClasses : who.doorClass});
	if (!path.found)
	{
		route.failure = path.failure == knav::CoarsePath::Failure::NoRoute
			|| path.failure == knav::CoarsePath::Failure::NotBuilt
			? state::AiReason::NoRoute : state::AiReason::OffMesh;
		route.check = route.failure == state::AiReason::NoRoute ? "coarse graph: no cluster path between the ends"
			: "coarse graph: an end has no walkable poly";
		return route;
	}
	// Legs share their end points; keep each corner once.
	for (const knav::CoarseLeg& leg : path.legs)
	{
		for (const knav::Point& point : leg.points)
		{
			const state::GroundPoint next{point[0], point[2]};
			if (route.points.empty() || route.points.back().x != next.x
				|| route.points.back().z != next.z)
				route.points.push_back(next);
		}
	}
	route.found = !route.points.empty();
	if (!route.found)
	{
		route.failure = state::AiReason::NoRoute;
		route.check = "coarse graph: the path has no points";
	}
	return route;
}

void KnavNavigator::applyDoors(const knav::DoorBook& book) const
{
	const std::uint64_t began = nowNs();
	impl_->book = book;
	for (auto& [key, mesh] : impl_->meshes)
		mesh.applyDoors(book);
	for (Impl::ClassTable& table : impl_->classes)
		table.valid = false;
	++stats_.doorApplies;
	stats_.doorApplyNs += nowNs() - began;
}

const knav::DoorBook& KnavNavigator::doors() const noexcept
{
	return impl_->book;
}

std::uint32_t KnavNavigator::reachSet(const Traveller& who, double x, double z) const
{
	const std::uint32_t label = impl_->componentAt(x, z, kReach);
	return label == knav::PolyComponents::kNone ? kNoSet : impl_->classRep(label, who.doorClass);
}

MeshNavigator::MeshNavigator(knav::NavMesh mesh)
	: mesh_(std::move(mesh))
	, walker_(mesh_)
	, query_(mesh_, kClickNodes)
{
}

std::shared_ptr<MeshNavigator> MeshNavigator::bake(const std::string& dataDir, kworld::ZoneCell min,
	kworld::ZoneCell max, int threads)
{
	const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
	const knav::AreaGeometry area = knav::gatherArea(dataDir, terrain, min, max);
	knav::NavWorld world(knav::configForZones(min, max), terrain, area.foliage);
	for (const knav::AreaBuilding& building : area.buildings)
		world.setBuilding(building.id, building.shapes, building.nav);
	world.bakeAll(threads);
	auto navigator = std::make_shared<MeshNavigator>(knav::NavMesh::fromTiles(world.mesh().config(), world.mesh().tiles()));
	return navigator;
}

std::shared_ptr<MeshNavigator> MeshNavigator::load(const std::string& path, const std::string& /*dataDir*/)
{
	auto navigator = std::make_shared<MeshNavigator>(knav::NavMesh::load(path));
	return navigator;
}

namespace
{

// Zone cell -> the tile rectangle it covers on `config`'s grid (a zone is 4,608 units, 18 tiles of 256).
knav::TileRect zoneTiles(const knav::NavConfig& config, kworld::ZoneCell zone)
{
	const int tx = static_cast<int>(std::lround((kworld::zoneMinX(zone.x) - config.originX) / config.tileSize));
	const int tz = static_cast<int>(std::lround((kworld::zoneMinZ(zone.z) - config.originZ) / config.tileSize));
	const int span = static_cast<int>(std::lround(kworld::kZoneSize / config.tileSize));
	return {tx, tz, tx + span - 1, tz + span - 1};
}

bool validZone(kworld::ZoneCell zone)
{
	return zone.x >= 0 && zone.z >= 0 && zone.x < kworld::kZoneCellsPerAxis && zone.z < kworld::kZoneCellsPerAxis;
}

int regionKeyOf(kworld::ZoneCell zone)
{
	return (zone.z / knav::kRegionZones) * (kworld::kZoneCellsPerAxis / knav::kRegionZones) + zone.x / knav::kRegionZones;
}

std::pair<int, int> regionOfKey(int key)
{
	const int regionsPerAxis = kworld::kZoneCellsPerAxis / knav::kRegionZones;
	return { key % regionsPerAxis, key / regionsPerAxis };
}

std::string regionPathOf(const std::string& dir, kworld::ZoneCell zone)
{
	return dir + "/" + knav::regionFileName(zone.x / knav::kRegionZones, zone.z / knav::kRegionZones);
}

double msSince(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// The tiles an interior mesh reaches: its vertices' tile squares, clipped to its zone and the region window.
std::vector<knav::TileCoord> meshTileCoords(const knav::NavConfig& config, kworld::ZoneCell zone, const knav::native::Mesh& mesh)
{
	if (mesh.vertices.empty())
		return {};
	double minX = mesh.vertices.front()[0], maxX = minX, minZ = mesh.vertices.front()[2], maxZ = minZ;
	for (const std::array<double, 3>& vertex : mesh.vertices)
	{
		minX = std::min(minX, vertex[0]);
		maxX = std::max(maxX, vertex[0]);
		minZ = std::min(minZ, vertex[2]);
		maxZ = std::max(maxZ, vertex[2]);
	}
	const auto tile = [&](double value, double origin)
	{
		return static_cast<int>(std::floor((value - origin) / config.tileSize));
	};
	const knav::TileRect own = zoneTiles(config, zone);
	std::vector<knav::TileCoord> coords;
	for (int z = std::max({ tile(minZ, config.originZ), config.window.z0, own.z0 });
		z <= std::min({ tile(maxZ, config.originZ), config.window.z1, own.z1 }); ++z)
		for (int x = std::max({ tile(minX, config.originX), config.window.x0, own.x0 });
			x <= std::min({ tile(maxX, config.originX), config.window.x1, own.x1 }); ++x)
			coords.push_back({ x, z });
	return coords;
}

// ---- the disk cache (docs/formats/NAVMESH_CACHE.md "Patch cache") ----

// The entry layout's recipe version: bump with any change to the conversion, the bake recipe or its tuning, or an entry's
// layout. The .knav cache version and the Detour version are checked by NavMesh itself.
constexpr std::uint32_t kNativeCacheRecipe = 21; // 21: furniture body ring (footprint + clearance + margin nulled, stairs and use-node paths spared) and the slide height fallback (kenshi-k16.7.89); 20: a detail vertex more than the climb off a level polygon's corners takes the nearest corner height, so a slab edge band over a stair well has no skirt down to the stair (Hub bar stair, kenshi-k16.8.41); 12-19: unused (stale ring-branch caches on dev); 11: the zone key folds the gathered geometry digest and the heightmap file stamp (kenshi-k16.7.67); 10: k16.7.50 made bakes deterministic (Recast read unwritten heap bytes in rcBuildContours); caches baked before it are rebuilt, no mesh rule change; 9: the zone is pruned with its own seeds (zone probes or seeds.def, building origins, door outside points), keeping every set that touches the zone's outer border (kenshi-k16.6.270); 8: a foliage shell wider than 750 u cuts only its own triangle heights, not the whole shell height (kenshi-k16.7.59); 7: a ramp strip of an open walkable building sheet (faces 40-55 degrees joining flat walkable faces at both ends) is walkable, so a wall-walk's 47 degree ramp is (kenshi-k16.7.58); 6: unused (k16.7.59 before its merge); 5: the door mat gives back a stair slope more under the door, the stair joins the doorway (kenshi-k16.6.229); 4: walkable surfaces at their exact collision height, not the voxel top (kenshi-k16.7.36); 3: terrain inside an interior volume is carved, interior terrain 0 (kenshi-k16.7.18); 2: a rock shell cuts per triangle (kenshi-k16.19.4)

std::uint64_t cacheMix(std::uint64_t hash, const void* data, std::size_t size)
{
	const auto* bytes = static_cast<const std::uint8_t*>(data);
	for (std::size_t i = 0; i < size; ++i)
	{
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

std::uint64_t cacheMix(std::uint64_t hash, std::uint64_t value) { return cacheMix(hash, &value, sizeof(value)); }

std::string cacheHex(std::uint64_t value)
{
	char text[17];
	std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
	return text;
}

// The cache root: the option, KENSHI_NAV_CACHE, else $XDG_CACHE_HOME/kenshi-sim/navmesh (~/.cache/...). Empty when there is no
// home to put it under.
std::string defaultCacheRoot()
{
	if (const char* env = std::getenv("KENSHI_NAV_CACHE"); env != nullptr && *env != '\0')
		return env;
	if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg != '\0')
		return std::string(xdg) + "/kenshi-sim/navmesh";
	if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
		return std::string(home) + "/.cache/kenshi-sim/navmesh";
	return {};
}

// The grid a cache entry must be on: the same origin, tile size, world grid and agent/water parameters.
bool sameGrid(const knav::NavConfig& a, const knav::NavConfig& b)
{
	return a.originX == b.originX && a.originZ == b.originZ && a.tileSize == b.tileSize && a.tilesX == b.tilesX
		&& a.tilesZ == b.tilesZ && a.cellSize == b.cellSize && a.cellHeight == b.cellHeight
		&& a.agentHeight == b.agentHeight && a.agentRadius == b.agentRadius && a.agentClimb == b.agentClimb
		&& a.agentMaxSlope == b.agentMaxSlope && a.waterLevel == b.waterLevel && a.wadeDepth == b.wadeDepth
		&& a.swimDepth == b.swimDepth;
}

} // namespace

std::shared_ptr<MeshNavigator> MeshNavigator::openWorld(const std::string& dir, const std::vector<kworld::ZoneCell>& initial,
	const std::string& /*dataDir*/)
{
	std::string first;
	for (const kworld::ZoneCell zone : initial)
	{
		if (validZone(zone))
		{
			first = regionPathOf(dir, zone);
			break;
		}
	}
	if (first.empty())
		throw kworld::Error("openWorld: no valid initial zone");
	// The grid parameters come from a region file's header; the mesh holds the whole grid as its window (maxTiles is then the
	// resident limit), whatever the files' windows.
	knav::NavConfig config;
	knav::NavMesh::readTiles({first}, knav::TileRect{}, config);
	config.window = {0, 0, config.tilesX - 1, config.tilesZ - 1};
	auto navigator = std::make_shared<MeshNavigator>(knav::NavMesh::fromTiles(config, {}));
	navigator->worldDir_ = dir;
	navigator->setCoverage(initial);
	return navigator;
}

std::shared_ptr<MeshNavigator> MeshNavigator::openNative(const std::string& dataDir, const std::vector<kworld::ZoneCell>& initial,
	std::function<void(const std::string&)> log, const kdata::GameData* gameData, NativeCacheOptions cache)
{
	const std::string navtiles = knav::native::navtilesDir(dataDir);
	if (!std::filesystem::is_directory(navtiles))
		throw kworld::Error("openNative: no shipped navtiles in " + navtiles);
	// The whole grid, absolute tile coordinates, as the world bake's: the same tile borders as openWorld, so a zone of native
	// tiles and a zone of the bake sit in one mesh.
	knav::NavConfig config = knav::configForWorldZones({0, 0}, {0, 0});
	config.window = {0, 0, config.tilesX - 1, config.tilesZ - 1};
	auto navigator = std::make_shared<MeshNavigator>(knav::NavMesh::fromTiles(config, {}));
	navigator->nativeSource_ = true;
	navigator->nativeDir_ = navtiles;
	navigator->dataDir_ = dataDir;
	navigator->sharedGameData_ = gameData;
	navigator->log_ = std::move(log);
	if (!navigator->log_) // the fallbacks are never silent
		navigator->log_ = [](const std::string& line) { std::cerr << "knav: " << line << '\n'; };
	navigator->openNativeCache(cache);
	navigator->openNativeData();
	navigator->setCoverage(initial);
	navigator->logNativeCache();
	return navigator;
}

MeshNavigator::~MeshNavigator() = default;

void MeshNavigator::openNativeData()
{
	if (hashSource_)
		return;
	terrain_ = std::make_unique<kworld::Heightmap>(kworld::Heightmap::open(dataDir_ + "/newland/land/fullmap.tif"));
	if (sharedGameData_ != nullptr)
		gatherer_ = std::make_unique<kworld::AreaGatherer>(dataDir_, *terrain_, *sharedGameData_);
	else
		gatherer_ = std::make_unique<kworld::AreaGatherer>(dataDir_, *terrain_);
	hashSource_ = std::make_unique<kworld::NavHashSource>(dataDir_, gatherer_->sources());
}

const MeshNavigator::NativeTileReport* MeshNavigator::nativeReport(kworld::ZoneCell zone) const
{
	const auto found = staleReports_.find(kworld::zoneIndexOf(zone));
	return found == staleReports_.end() ? nullptr : &found->second;
}

std::vector<knav::BakedTile> MeshNavigator::bakeNativeZone(kworld::ZoneCell zone) const
{
	const auto start = std::chrono::steady_clock::now();
	const kworld::AreaGeometry area = gatherer_->gather(zone, zone);
	knav::NavConfig config = knav::configForWorldZones(zone, zone);
	knav::NavWorld world(config, *terrain_, area.foliage);
	for (const kworld::AreaBuilding& building : area.buildings)
		world.setBuilding(building.id, building.shapes, building.nav);
	// The bake's seeds for this one zone (knav_bake setSeeds): a pure function of the zone, so the cache entry carries the cut.
	if (seedsDef_.empty())
		seedsDef_ = knav::readSeedsDef(nativeDir_);
	std::vector<knav::Point> seeds = knav::zoneSeeds(*terrain_, area, seedsDef_, zone, zone, config.waterLevel);
	const std::vector<knav::Point> origins = knav::buildingSeeds(area);
	seeds.insert(seeds.end(), origins.begin(), origins.end());
	world.setSeeds(std::move(seeds), knav::doorSeedsOf(area));
	world.setKeepWindowEdge(true);
	world.bakeAll(kNativeBakeThreads);
	staleStats_.bakeMs += msSince(start);
	return world.mesh().tiles();
}

std::uint64_t MeshNavigator::bakedZoneDigest(kworld::ZoneCell zone) const
{
	std::uint64_t hash = 14695981039346656037ull;
	for (const knav::BakedTile& tile : bakeNativeZone(zone))
	{
		hash = cacheMix(hash, static_cast<std::uint64_t>(tile.x));
		hash = cacheMix(hash, static_cast<std::uint64_t>(tile.z));
		hash = cacheMix(hash, tile.data.data(), tile.data.size());
		for (const knav::DoorPoly& door : tile.doors)
		{
			hash = cacheMix(hash, static_cast<std::uint64_t>(door.poly));
			hash = cacheMix(hash, static_cast<std::uint64_t>(door.door));
		}
	}
	return hash;
}

std::uint32_t MeshNavigator::nativeCacheRecipe() { return kNativeCacheRecipe; }

// The Detour tiles of the region (rx, rz) from the shipped navtiles: the conversion `knav_native OUT --region RX RZ` runs on
// disk, in memory (docs/formats/NAVMESH_CACHE.md "Runtime mesh"; the shipped tiles, the faces cut into 256 u tile polygons).
// A zone of margin each way brings an interior stored next door into the mesh, as the tool does. The tiles are the shipped
// meshes as converted; the original's stale-tile check (applyStaleCheck) replaces the ones a zone decision rejects.
std::vector<knav::BakedTile> MeshNavigator::convertNativeRegion(int rx, int rz) const
{
	const kworld::ZoneCell min{rx * knav::kRegionZones, rz * knav::kRegionZones};
	const kworld::ZoneCell max{min.x + knav::kRegionZones - 1, min.z + knav::kRegionZones - 1};
	for (int z = min.z; z <= max.z; ++z)
		for (int x = min.x; x <= max.x; ++x)
			staleReports_.erase(kworld::zoneIndexOf({ x, z })); // re-converted: the decisions belong to the new tiles
	const knav::NavConfig config = knav::configForWorldZones(min, max);
	const std::string name = "region " + std::to_string(rx) + "," + std::to_string(rz);
	// The disk cache (docs/formats/NAVMESH_CACHE.md "Patch cache"): the converted tiles of a region are a pure function of the
	// shipped tile files it reads and the conversion, so a byte hash of those files is the key.
	const std::string path = cacheTree_.empty() ? std::string{}
		: cacheEntryPath("region", rx, rz, regionCacheKey(rx, rz));
	if (!cacheTree_.empty())
	{
		if (const std::optional<std::vector<knav::BakedTile>> cached = readCacheEntry(path, config, config.window))
		{
			++cacheStats_.regionHits;
			log_("native " + name + ": " + std::to_string(cached->size()) + " tiles from the cache");
			return *cached;
		}
		++cacheStats_.regionMisses;
	}
	std::vector<knav::native::Mesh> meshes;
	for (int j = std::max(min.z - 1, 0); j <= std::min(max.z + 1, kworld::kZoneCellsPerAxis - 1); ++j)
		for (int i = std::max(min.x - 1, 0); i <= std::min(max.x + 1, kworld::kZoneCellsPerAxis - 1); ++i)
			for (knav::native::Mesh& mesh : knav::native::loadTile(nativeDir_, i, j))
				meshes.push_back(std::move(mesh));
	std::vector<knav::TileCoord> coords;
	for (int z = config.window.z0; z <= config.window.z1; ++z)
		for (int x = config.window.x0; x <= config.window.x1; ++x)
			coords.push_back({ x, z });
	std::vector<knav::BakedTile> converted = knav::nativeTiles(config, meshes, coords);
	if (!cacheTree_.empty())
		writeCacheEntry(path, config, converted);
	return converted;
}

// ---- the disk cache (docs/formats/NAVMESH_CACHE.md "Patch cache") ----

void MeshNavigator::openNativeCache(const NativeCacheOptions& cache)
{
	cacheOptions_ = cache;
	cacheTree_.clear();
	if (!cache.enabled || cache.worldKey == 0)
		return; // off: no directory is touched
	const std::string root = cache.dir.empty() ? defaultCacheRoot() : cache.dir;
	if (root.empty())
		return;
	const std::string tree = root + "/w" + std::to_string(kNativeCacheRecipe) + "_" + cacheHex(cache.worldKey);
	std::error_code error;
	std::filesystem::create_directories(tree, error);
	if (error)
	{
		log_("nav cache: cannot create " + tree + " (" + error.message() + "); off");
		return;
	}
	cacheTree_ = tree;
}

void MeshNavigator::logNativeCache() const
{
	if (cacheTree_.empty())
	{
		log_(!cacheOptions_.enabled ? std::string("nav cache: off")
			: cacheOptions_.worldKey == 0 ? std::string("nav cache: off (no load order key)")
			: std::string("nav cache: off (no usable directory)"));
		return;
	}
	std::string line = "nav cache " + cacheTree_ + ": regions " + std::to_string(cacheStats_.regionHits) + " hit / "
		+ std::to_string(cacheStats_.regionMisses) + " miss, zones " + std::to_string(cacheStats_.zoneHits) + " hit / "
		+ std::to_string(cacheStats_.zoneMisses) + " miss, wrote " + std::to_string(cacheStats_.filesWritten) + " files ("
		+ std::to_string(cacheStats_.bytesWritten) + " B)";
	if (cacheStats_.loadFailures != 0)
		line += ", " + std::to_string(cacheStats_.loadFailures) + " unusable rebuilt";
	log_(line);
}

std::string MeshNavigator::cacheEntryPath(const char* kind, int x, int z, std::uint64_t key) const
{
	return cacheTree_ + "/" + kind + "_" + std::to_string(x) + "_" + std::to_string(z) + "_" + cacheHex(key) + ".knav";
}

// The key of a converted region: the bytes of the shipped tile files the conversion reads, its 16 zones plus the one-zone ring
// (a missing file mixes a marker). The conversion is a pure function of them and the code's version.
std::uint64_t MeshNavigator::regionCacheKey(int rx, int rz) const
{
	std::uint64_t hash = 14695981039346656037ull;
	hash = cacheMix(hash, static_cast<std::uint64_t>(kNativeCacheRecipe));
	const kworld::ZoneCell min{rx * knav::kRegionZones, rz * knav::kRegionZones};
	const kworld::ZoneCell max{min.x + knav::kRegionZones - 1, min.z + knav::kRegionZones - 1};
	std::array<char, 1 << 16> block;
	for (int j = std::max(min.z - 1, 0); j <= std::min(max.z + 1, kworld::kZoneCellsPerAxis - 1); ++j)
	{
		for (int i = std::max(min.x - 1, 0); i <= std::min(max.x + 1, kworld::kZoneCellsPerAxis - 1); ++i)
		{
			hash = cacheMix(hash, static_cast<std::uint64_t>(i));
			hash = cacheMix(hash, static_cast<std::uint64_t>(j));
			std::ifstream in(nativeDir_ + "/tile" + std::to_string(i) + "." + std::to_string(j) + ".hkt", std::ios::binary);
			if (!in)
			{
				const std::uint8_t missing = 0xff;
				hash = cacheMix(hash, &missing, 1);
				continue;
			}
			while (in)
			{
				in.read(block.data(), static_cast<std::streamsize>(block.size()));
				const std::streamsize got = in.gcount();
				if (got <= 0)
					break;
				hash = cacheMix(hash, block.data(), static_cast<std::size_t>(got));
			}
		}
	}
	return hash;
}

// The key of a zone's bake: its recomputed zone hash and its interiors' (ours, not the shipped tile's stored ones: the bake is
// a function of the current world). The interiors are sorted by uid, so the tile's file order cannot split one bake into two
// entries; the zone hash folds neither the children nor the furniture, which is why the interiors are in it.
std::uint64_t MeshNavigator::zoneCacheKey(kworld::ZoneCell zone, const std::vector<std::uint64_t>& interiorUids) const
{
	if (hashSource_ == nullptr)
		return 0;
	std::uint64_t hash = 14695981039346656037ull;
	hash = cacheMix(hash, static_cast<std::uint64_t>(kNativeCacheRecipe));
	const kworld::NavHashSource::ZoneHashes hashes = hashSource_->zone(zone);
	hash = cacheMix(hash, static_cast<std::uint64_t>(hashes.hash.value_or(0)));
	hash = cacheMix(hash, static_cast<std::uint64_t>(hashes.hash.has_value() ? 1u : 0u));
	std::vector<std::uint64_t> uids = interiorUids;
	std::sort(uids.begin(), uids.end());
	for (const std::uint64_t uid : uids)
	{
		const kworld::NavHashSource::InteriorHashes interior = hashSource_->interior(zone, uid);
		hash = cacheMix(hash, uid);
		hash = cacheMix(hash, static_cast<std::uint64_t>(interior.hash.value_or(0)));
		hash = cacheMix(hash, static_cast<std::uint64_t>(interior.basis));
	}
	// What the bake reads beyond the placements' integer positions: the gathered geometry (collision meshes, rotation, scale,
	// furniture, foliage; the 3 x 3 zones the bake gathers) and the heightmap file (size and mtime).
	hash = cacheMix(hash, kworld::areaDigest(gatherer_->gather(zone, zone)));
	std::error_code error;
	const std::string& heightmap = terrain_->path();
	const std::uint64_t size = std::filesystem::file_size(heightmap, error);
	const auto stamp = std::filesystem::last_write_time(heightmap, error).time_since_epoch().count();
	hash = cacheMix(hash, size);
	hash = cacheMix(hash, static_cast<std::uint64_t>(stamp));
	return hash;
}

// A cache entry's tiles, or nullopt on a miss. An entry that fails to load (truncated, another version, another grid) is
// deleted and logged: never a crash, and never a silent fallback that hides a bad file.
std::optional<std::vector<knav::BakedTile>> MeshNavigator::readCacheEntry(const std::string& path, const knav::NavConfig& expected,
	const knav::TileRect& keep) const
{
	if (cacheTree_.empty())
		return std::nullopt;
	std::error_code error;
	if (!std::filesystem::is_regular_file(path, error))
		return std::nullopt;
	try
	{
		knav::NavConfig config;
		std::vector<knav::BakedTile> tiles = knav::NavMesh::readTiles({ path }, keep, config);
		if (!sameGrid(config, expected))
			throw kworld::Error("the entry is on another grid");
		for (const knav::BakedTile& tile : tiles)
			if (!keep.contains(tile.x, tile.z))
				throw kworld::Error("a tile lies outside its window");
		return tiles;
	} catch (const std::exception& e) {
		++cacheStats_.loadFailures;
		log_("nav cache: unusable entry " + path + " (" + e.what() + "); deleting and rebuilding");
		std::filesystem::remove(path, error);
		return std::nullopt;
	}
}

// Writes tiles as one entry: a temp file, then a rename, so a reader never sees a partial one. A failed write only costs the
// next open a rebuild.
bool MeshNavigator::writeCacheEntry(const std::string& path, const knav::NavConfig& config,
	const std::vector<knav::BakedTile>& tiles) const
{
	if (cacheTree_.empty())
		return false;
	const std::string temp = path + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
	try
	{
		knav::NavMesh::fromTiles(config, tiles).save(temp);
		std::error_code error;
		const std::uintmax_t bytes = std::filesystem::file_size(temp, error);
		std::filesystem::rename(temp, path, error);
		if (error)
		{
			std::filesystem::remove(temp, error);
			log_("nav cache: cannot write " + path + " (" + error.message() + ")");
			return false;
		}
		++cacheStats_.filesWritten;
		cacheStats_.bytesWritten += static_cast<std::size_t>(bytes);
		return true;
	} catch (const std::exception& e) {
		std::error_code error;
		std::filesystem::remove(temp, error);
		log_("nav cache: cannot write " + path + " (" + e.what() + ")");
		return false;
	}
}

// The original's stale-tile check for one zone of a converted region: recompute the zone's hash (and, in a fresh zone, each
// interior's) from our world data and compare with the hashes stored in the shipped tile. A stale or unverifiable unit's
// tiles leave the region and our bake's take their place: the zone's whole tile square, or the tile square of a stale
// interior (the shipped zone mesh around it stays). The decision is recorded (NativeTileReport, NativeStaleStats) and is
// made once per converted region (a second call for the zone is a no-op). A zone with no shipped tile is baked whole.
bool MeshNavigator::applyStaleCheck(std::vector<knav::BakedTile>& tiles, kworld::ZoneCell zone) const
{
	const int key = kworld::zoneIndexOf(zone);
	if (staleReports_.count(key) != 0)
		return false;
	const auto start = std::chrono::steady_clock::now();
	const knav::NavConfig config = knav::configForWorldZones(zone, zone);
	const knav::TileRect own = zoneTiles(config, zone);
	const auto drop = [&](const std::set<knav::TileCoord>& coords)
	{
		tiles.erase(std::remove_if(tiles.begin(), tiles.end(),
			[&](const knav::BakedTile& tile) { return coords.count({ tile.x, tile.z }) != 0; }), tiles.end());
	};
	// The zone's bake, needed at most once (a stale zone takes its whole square, a fresh zone its stale interiors' tiles): from
	// the disk cache (docs/formats/NAVMESH_CACHE.md "Patch cache") or baked and written. A zone with no shipped tile lists no
	// interiors for the key, so it is not cached.
	knav::native::StoredHashes stored;
	const std::vector<knav::native::Mesh> meshes = knav::native::loadTile(nativeDir_, zone.x, zone.z, &stored);
	std::vector<std::uint64_t> interiorUids;
	for (const knav::native::Mesh& mesh : meshes)
	{
		if (!mesh.interior())
			continue;
		try
		{
			interiorUids.push_back(std::stoull(mesh.info, nullptr, 16));
		} catch (const std::exception&) {
			interiorUids.push_back(0);
		}
	}
	const std::string patchPath = cacheTree_.empty() || meshes.empty() ? std::string{}
		: cacheEntryPath("zone", zone.x, zone.z, zoneCacheKey(zone, interiorUids));
	bool bakeLoaded = false;
	std::vector<knav::BakedTile> bake;
	const auto zoneBake = [&]() -> const std::vector<knav::BakedTile>&
	{
		if (bakeLoaded)
			return bake;
		bakeLoaded = true;
		if (!patchPath.empty())
		{
			if (std::optional<std::vector<knav::BakedTile>> cached = readCacheEntry(patchPath, config, own))
			{
				++cacheStats_.zoneHits;
				bake = std::move(*cached);
				return bake;
			}
			++cacheStats_.zoneMisses;
		}
		bake = bakeNativeZone(zone);
		if (!patchPath.empty())
			writeCacheEntry(patchPath, config, bake);
		return bake;
	};
	const auto take = [&](const std::set<knav::TileCoord>& coords)
	{
		for (const knav::BakedTile& tile : zoneBake())
			if (coords.count({ tile.x, tile.z }) != 0)
				tiles.push_back(tile);
	};
	NativeTileReport report;
	report.zone = zone;
	const kworld::NavHashSource::ZoneHashes hashes = hashSource_->zone(zone);
	report.decision.stored = stored.hasZone ? std::optional<std::uint32_t>(stored.zone) : std::nullopt;
	report.decision.ours = hashes.hash;
	++staleStats_.zonesScanned;
	kworld::StaleVerdict verdict = kworld::staleVerdict(report.decision.stored, hashes.hash);
	if (meshes.empty())
		verdict = kworld::StaleVerdict::Unverifiable; // no shipped tile: nothing to check, and nothing to use
	if (verdict != kworld::StaleVerdict::Fresh)
	{
		report.decision.choice = verdict == kworld::StaleVerdict::Stale
			? NativeChoice::Rebaked : NativeChoice::NotComputable;
		report.decision.reason = meshes.empty() ? "no shipped tile"
			: !stored.hasZone ? "no stored zone hash"
			: verdict == kworld::StaleVerdict::Stale ? "stale hash" : hashes.reason;
		++(verdict == kworld::StaleVerdict::Stale ? staleStats_.zonesRebaked : staleStats_.zonesNotComputable);
		std::set<knav::TileCoord> coords;
		for (int z = own.z0; z <= own.z1; ++z)
			for (int x = own.x0; x <= own.x1; ++x)
				coords.insert({ x, z });
		report.rebaked.assign(coords.begin(), coords.end());
		drop(coords);
		take(coords);
		staleStats_.checkMs += msSince(start);
		staleReports_[key] = std::move(report);
		return true;
	}
	// A fresh zone keeps its shipped world mesh; each interior is checked on its own.
	report.decision.choice = NativeChoice::Shipped;
	++staleStats_.zonesShipped;
	std::set<knav::TileCoord> rebaked;
	std::size_t interiorIndex = 0;
	for (const knav::native::Mesh& mesh : meshes)
	{
		if (!mesh.interior())
			continue;
		const std::optional<std::uint32_t>& storedHash = stored.interiors[interiorIndex++];
		std::uint64_t uid = 0;
		try
		{
			uid = std::stoull(mesh.info, nullptr, 16);
		}
		catch (const std::exception&)
		{
		}
		const kworld::NavHashSource::InteriorHashes interior = hashSource_->interior(zone, uid);
		const kworld::StaleVerdict interiorVerdict = kworld::staleVerdict(storedHash, interior.hash, interior.basis);
		NativeInteriorReport interiorReport;
		interiorReport.uid = uid;
		interiorReport.decision.stored = storedHash;
		interiorReport.decision.ours = interior.hash;
		if (interiorVerdict == kworld::StaleVerdict::Fresh)
		{
			interiorReport.decision.choice = NativeChoice::Shipped;
			++staleStats_.interiorsShipped;
		} else {
			interiorReport.decision.choice = interiorVerdict == kworld::StaleVerdict::Stale
				? NativeChoice::Rebaked : NativeChoice::NotComputable;
			interiorReport.decision.reason = !storedHash ? "no stored interior hash"
				: interiorVerdict == kworld::StaleVerdict::Stale ? "stale hash" : kworld::staleReason(interior.basis);
			if (interiorVerdict == kworld::StaleVerdict::Stale)
				++staleStats_.interiorsRebaked;
			else
			{
				++staleStats_.interiorsNotComputable;
				if (interior.basis == kworld::NavHashSource::Basis::ChildOrder)
					++staleStats_.interiorChildOrder;
				else if (interior.basis == kworld::NavHashSource::Basis::Unpaired)
					++staleStats_.interiorUnpaired;
				else if (interior.basis == kworld::NavHashSource::Basis::Unjoined)
					++staleStats_.interiorUnjoined;
			}
			for (const knav::TileCoord& tile : meshTileCoords(config, zone, mesh))
				rebaked.insert(tile);
		}
		report.interiors.push_back(std::move(interiorReport));
	}
	if (!rebaked.empty())
	{
		report.rebaked.assign(rebaked.begin(), rebaked.end());
		drop(rebaked);
		take(rebaked);
	}
	staleStats_.checkMs += msSince(start);
	staleReports_[key] = std::move(report);
	return !rebaked.empty();
}

bool MeshNavigator::canCover(kworld::ZoneCell zone) const
{
	if (!streaming() || !validZone(zone))
		return false;
	if (nativeSource_)
		return true; // every region converts; one with no shipped tiles simply adds none
	const auto known = fileOk_.find(kworld::zoneIndexOf(zone));
	if (known != fileOk_.end())
		return known->second;
	return fileOk_[kworld::zoneIndexOf(zone)] = std::filesystem::exists(regionPathOf(worldDir_, zone));
}

std::vector<std::pair<int, int>> MeshNavigator::nativeCachedRegions() const
{
	std::vector<std::pair<int, int>> regions;
	for (const auto& [key, tiles] : nativeRegions_)
		regions.push_back(regionOfKey(key));
	return regions;
}

std::vector<kworld::ZoneCell> MeshNavigator::coveredZones() const
{
	std::vector<kworld::ZoneCell> zones;
	for (const int index : zones_)
		zones.push_back(kworld::zoneCellOfIndex(index));
	return zones;
}

MeshNavigator::CoverageChange MeshNavigator::setCoverage(const std::vector<kworld::ZoneCell>& wanted) const
{
	CoverageChange change;
	if (!streaming())
		return change;
	const NativeStaleStats staleBefore = staleStats_;
	std::set<int> target;
	for (const kworld::ZoneCell zone : wanted)
	{
		if (!canCover(zone))
			continue;
		if (target.size() >= kMaxZones)
			++change.overflow;
		else
			target.insert(kworld::zoneIndexOf(zone));
	}
	const knav::NavConfig& config = mesh_.config();
	std::vector<knav::TileCoord> removedTiles;
	for (const int index : zones_)
	{
		if (target.count(index) == 0)
		{
			++change.removed;
			const knav::TileRect rect = zoneTiles(config, kworld::zoneCellOfIndex(index));
			for (int z = rect.z0; z <= rect.z1; ++z)
			{
				for (int x = rect.x0; x <= rect.x1; ++x)
				{
					if (mesh_.tile(x, z) != nullptr)
						removedTiles.push_back({x, z});
				}
			}
		}
	}
	// New zones, obtained one region at a time: the zones of a region share one pass over its file, or one conversion.
	std::vector<kworld::ZoneCell> newZones;
	for (const int index : target)
	{
		if (zones_.count(index) == 0)
		{
			++change.added;
			newZones.push_back(kworld::zoneCellOfIndex(index));
		}
	}
	if (!change.changed())
		return change;
	const auto readStart = std::chrono::steady_clock::now();
	std::vector<knav::BakedTile> addedTiles;
	// The tiles of a zone (from a region file, or converted from the shipped navtiles): the rest of the region is dropped.
	const auto keepZoneTiles = [&](const std::vector<knav::BakedTile>& tiles, const std::vector<kworld::ZoneCell>& zones)
	{
		for (const knav::BakedTile& tile : tiles)
		{
			for (const kworld::ZoneCell zone : zones)
			{
				if (zoneTiles(config, zone).contains(tile.x, tile.z))
				{
					addedTiles.push_back(tile);
					break;
				}
			}
		}
	};
	if (nativeSource_)
	{
		// One conversion per region whatever zones of it are wanted: a region of 4 x 4 zones converts whole.
		std::map<RegionKey, std::vector<kworld::ZoneCell>> byRegion;
		for (const kworld::ZoneCell zone : newZones)
			byRegion[{zone.x / knav::kRegionZones, zone.z / knav::kRegionZones}].push_back(zone);
		for (const auto& [region, regionZones] : byRegion)
		{
			// Converted on the first zone of the region wanted; a zone that comes back finds its region's tiles here.
			const int key = region.second * (kworld::kZoneCellsPerAxis / knav::kRegionZones) + region.first;
			auto cached = nativeRegions_.find(key);
			if (cached == nativeRegions_.end())
				cached = nativeRegions_.emplace(key, convertNativeRegion(region.first, region.second)).first;
			// The original's stale-tile check, on the zones actually wanted: a zone nobody loads is not checked (its
			// shipped tiles stay in the region's cache until it is), a zone that comes back finds its decision here.
			if (!cacheOptions_.shippedOnly)
				for (const kworld::ZoneCell zone : regionZones)
					applyStaleCheck(cached->second, zone);
			keepZoneTiles(cached->second, regionZones);
			nativeRecent_.erase(std::remove(nativeRecent_.begin(), nativeRecent_.end(), key), nativeRecent_.end());
			nativeRecent_.insert(nativeRecent_.begin(), key); // in use order, most recent first
		}
	} else {
		std::map<std::string, std::vector<kworld::ZoneCell>> byFile;
		for (const kworld::ZoneCell zone : newZones)
			byFile[regionPathOf(worldDir_, zone)].push_back(zone);
		for (const auto& [path, zones] : byFile)
		{
			knav::TileRect box = zoneTiles(config, zones.front());
			for (const kworld::ZoneCell zone : zones)
			{
				const knav::TileRect rect = zoneTiles(config, zone);
				box = {std::min(box.x0, rect.x0), std::min(box.z0, rect.z0), std::max(box.x1, rect.x1), std::max(box.z1, rect.z1)};
			}
			knav::NavConfig ignored;
			keepZoneTiles(knav::NavMesh::readTiles({path}, box, ignored), zones);
		}
	}
	change.readMs = msSince(readStart);
	change.tilesAdded = addedTiles.size();
	change.tilesRemoved = removedTiles.size();
	const auto applyStart = std::chrono::steady_clock::now();
	mesh_.replaceTiles(removedTiles, std::move(addedTiles));
	change.applyMs = msSince(applyStart);
	zones_ = std::move(target);
	if (nativeSource_)
	{
		// The cache: the regions with a resident zone stay (a zone coming back is instant), plus the two most recently used
		// others (a step back across a border does not convert again); the rest are dropped, so a long walk over many regions
		// keeps a bounded amount of memory.
		std::set<int> keep;
		for (const int index : zones_)
			keep.insert(regionKeyOf(kworld::zoneCellOfIndex(index)));
		int extra = 0; // the small LRU beside the resident regions
		for (const int key : nativeRecent_)
			if (keep.count(key) == 0 && extra++ < 2)
				keep.insert(key);
		for (auto it = nativeRegions_.begin(); it != nativeRegions_.end();)
			it = keep.count(it->first) != 0 ? std::next(it) : nativeRegions_.erase(it);
		nativeRecent_.erase(std::remove_if(nativeRecent_.begin(), nativeRecent_.end(),
			[&](int key) { return nativeRegions_.count(key) == 0; }), nativeRecent_.end());
	}
	if (nativeSource_ && change.changed() && log_)
	{
		// The stale-tile check's decisions for this coverage change: nothing falls back silently.
		char line[512];
		std::snprintf(line, sizeof line,
			"native coverage: zones %zu shipped, %zu rebaked (stale hash), %zu rebaked (unverifiable); interiors %zu shipped, "
			"%zu rebaked (stale hash), %zu rebaked (unverifiable); check %.0f ms, bake %.0f ms",
			staleStats_.zonesShipped - staleBefore.zonesShipped, staleStats_.zonesRebaked - staleBefore.zonesRebaked,
			staleStats_.zonesNotComputable - staleBefore.zonesNotComputable,
			staleStats_.interiorsShipped - staleBefore.interiorsShipped, staleStats_.interiorsRebaked - staleBefore.interiorsRebaked,
			staleStats_.interiorsNotComputable - staleBefore.interiorsNotComputable,
			staleStats_.checkMs - staleBefore.checkMs, staleStats_.bakeMs - staleBefore.bakeMs);
		log_(line);
	}
	totals_.added += change.added;
	totals_.removed += change.removed;
	totals_.tilesAdded += change.tilesAdded;
	totals_.tilesRemoved += change.tilesRemoved;
	totals_.readMs += change.readMs;
	totals_.applyMs += change.applyMs;
	return change;
}

bool MeshNavigator::covers(double x, double z) const
{
	const knav::NavConfig& config = mesh_.config();
	const int tx = static_cast<int>(std::floor((x - config.originX) / config.tileSize));
	const int tz = static_cast<int>(std::floor((z - config.originZ) / config.tileSize));
	return mesh_.tile(tx, tz) != nullptr;
}

Route MeshNavigator::route(double fromX, double fromZ, double toX, double toZ) const
{
	// No height given: the walkable ground nearest the xz position, whatever its height.
	const auto at = nearestWalkable(fromX, fromZ, kEndReach);
	if (!at)
	{
		Route none;
		none.failure = state::AiReason::OffMesh;
		return none;
	}
	const auto y = walker_.surfaceY(at->x, 0.0, at->z, 1.0e5);
	return routeFrom({at->x, y.value_or(0.0), at->z}, toX, toZ);
}

std::optional<state::GroundPoint> MeshNavigator::nearestWalkable(double x, double z, double reach, double radius) const
{
	knav::Point out;
	const knav::Point from{static_cast<float>(x), 0.0f, static_cast<float>(z)};
	if (radius > 0.0)
	{
		// A body `radius` wide: the nearest ground where it fits (no wall within `radius` of the point).
		const float limit = static_cast<float>(radius);
		knav::PolyLoc found;
		const auto fits = [&](const knav::PolyLoc& loc) { return query_.wallDistance(loc, limit) >= limit; };
		if (!query_.locateWhere(from, {static_cast<float>(reach), 1.0e5f, static_cast<float>(reach)}, fits, found))
			return std::nullopt;
		return state::GroundPoint{found.point[0], found.point[2]};
	}
	// No height given: the ground nearest in xz, whatever its y (NavQuery::nearestPointXZ).
	if (!query_.nearestPointXZ(from, static_cast<float>(reach), out))
		return std::nullopt;
	return state::GroundPoint{out[0], out[2]};
}

Route MeshNavigator::routeFrom(const knav::Position& from, double toX, double toZ, std::optional<double> toY, bool planned,
	Body body, bool straighten, bool clickGoal) const
{
#if KSIM_PROFILE
	// The tick profiler's path_queries / path_us (every character route lands here; kenshi-k16.6.234).
	const std::uint64_t began = nowNs();
	const std::uint64_t nodes0 = stats_.routeClearanceNodes;
	const auto failures0 = stats_.routeFailures;
	Route route = routeFromUntimed(from, toX, toZ, toY, planned, body, straighten, clickGoal);
	++stats_.routeCalls;
	stats_.routeFound += route.found ? 1 : 0;
	const std::uint64_t took = nowNs() - began;
	stats_.routeNs += took;
	if (took > 5'000'000 && std::getenv("KSIM_SLOWQ"))
	{
		// SLOWQ (kenshi-k16.6.319): one line per route query over 5 ms; failure is the knav::PathResult::Failure index (0 = found).
		int failure = 0;
		for (std::size_t i = 0; i < failures0.size(); ++i)
			if (stats_.routeFailures[i] != failures0[i])
				failure = static_cast<int>(i);
		std::fprintf(stderr, "SLOWQ ms=%.1f from=%.0f,%.0f to=%.0f,%.0f r=%.2f planned=%d fail=%d nodes=%llu found=%d approach=%d plainFailed=%d\n",
			static_cast<double>(took) * 1e-6, from.x, from.z, toX, toZ, body.radius, planned ? 1 : 0, failure,
			static_cast<unsigned long long>(stats_.routeClearanceNodes - nodes0), route.found ? 1 : 0, route.approach ? 1 : 0, route.plainFailed ? 1 : 0);
	}
	return route;
#else
	return routeFromUntimed(from, toX, toZ, toY, planned, body, straighten, clickGoal);
#endif
}

// A walker is stranded when its connected set holds under 1/kStrandedRatio of the goal set's polygons (PATHFINDING_STATIC 1.11).
static constexpr std::uint32_t kStrandedRatio = 4;

std::uint32_t MeshNavigator::setOf(const knav::PolyLoc& loc) const
{
	if (!sets_ || setsRevision_ != mesh_.revision())
	{
		// Doors count as ground: a locked gate is the door check's business, not a reason to seat a walker elsewhere.
		sets_.emplace(mesh_, static_cast<std::uint16_t>(0xffff));
		setsRevision_ = mesh_.revision();
		setSizes_.assign(sets_->count(), 0);
		sets_->forEach([&](int, int, int, std::uint32_t label)
		{
			if (label != knav::PolyComponents::kNone)
				++setSizes_[label];
		});
	}
	return sets_->at(loc.tileX, loc.tileZ, loc.poly);
}

std::uint32_t MeshNavigator::setSize(std::uint32_t label) const
{
	return label < setSizes_.size() ? setSizes_[label] : 0;
}

Route MeshNavigator::routeFromUntimed(const knav::Position& from, double toX, double toZ, std::optional<double> toY, bool planned,
	Body body, bool straighten, bool clickGoal) const
{
	// Class kDoorClasses is no class: no denied-door bit is excluded, so locked doors stay in the search.
	knav::TravelFilter travel;
	travel.radius = static_cast<float>(body.radius);
	if (body.water)
		travel.setWaterCost(*body.water);
	travel.straighten = straighten;
	if (planned)
		travel.doorClass = knav::kDoorClasses;
	Route route;
	const float reach = static_cast<float>(kClickReach);
	const knav::Point start{static_cast<float>(from.x), static_cast<float>(from.y), static_cast<float>(from.z)};
	const knav::Point end{static_cast<float>(toX), static_cast<float>(toY.value_or(from.y)), static_cast<float>(toZ)};
	// The end is looked up in a box of kClickReach: with a click height the nearest point must also be
	// within kClickReach in 3D; without one any height counts (a point on the terrain under a deck, or on it),
	// so the ground nearest in xz is meant, not the lowest ground in a box spanning every height.
	const knav::Point extents{reach, toY ? reach : 1.0e5f, reach};
	knav::Point snapped;
	// The start's own connected set (none when it is off the mesh): a goal with no height prefers ground in it.
	knav::PolyLoc startLoc;
	const bool startOn = query_.locate(start, {kEndReach, 40.0f, kEndReach}, startLoc);
	const std::uint32_t startSet = startOn ? setOf(startLoc) : knav::PolyComponents::kNone;
	bool snappedOk = toY ? query_.nearestPoint(end, extents, snapped) : query_.nearestPointXZ(end, reach, snapped);
	if (snappedOk && !toY && startSet != knav::PolyComponents::kNone)
	{
		// Ground in the start's set no farther than 1 u (xz) than the nearest ground wins: a deck and the ground under it are both
		// at distance 0 and the walker's own level is meant, not the lowest floor.
		knav::Point own;
		const auto sameSet = [&](const knav::PolyLoc& loc) { return setOf(loc) == startSet; };
		if (query_.nearestPointXZ(end, reach, own, sameSet)
			&& std::hypot(own[0] - end[0], own[2] - end[2]) <= std::hypot(snapped[0] - end[0], snapped[2] - end[2]) + 1.0)
			snapped = own;
	}
	if (!snappedOk
		|| (toY && std::sqrt((snapped[0] - end[0]) * (snapped[0] - end[0]) + (snapped[1] - end[1]) * (snapped[1] - end[1])
			+ (snapped[2] - end[2]) * (snapped[2] - end[2])) > reach))
	{
		route.failure = state::AiReason::OffMesh;
		route.check = snappedOk ? "goal: nearest poly is farther than the click reach in 3D (wrong layer / height)"
			: "goal: no walkable poly within the click reach";
		return route;
	}
	// A click without a height (`clickGoal`): the goal is the ground nearest in xz (`snapped`), at ITS height. findPath snaps its end in 3D from the walker's own
	// height, which where floors stack picked the floor level with the walker, ground beside the click: the Hub's ramp top at (-50802, 2756) is
	// 31 u over the ground 5.6 u away, and the goal push then took the approach 12 u from the click (kenshi-k16.8.10). When the snapped floor
	// cannot be reached the walker's own level is tried, as before.
	knav::PathResult path;
	bool tried = false;
	if (clickGoal && !toY)
	{
		// Only where the walker's level is over 1 u farther from the click in xz than the ground nearest it: floors stacked over the click
		// (a room behind a shut door, a deck) stay with the walker's level, as before.
		knav::Point level;
		const auto xz = [&](const knav::Point& p) { return std::hypot(p[0] - end[0], p[2] - end[2]); };
		if (!query_.nearestPoint(end, extents, level) || xz(level) > xz(snapped) + 1.0)
		{
			knav::Point goal = end;
			goal[1] = snapped[1];
			path = query_.findPath(start, goal, extents, travel);
			tried = true;
		}
	}
	if (!tried || !path.found || !path.complete || path.points.empty())
		path = query_.findPath(start, end, extents, travel);
	stats_.routeClearanceNodes += static_cast<std::uint64_t>(path.clearanceNodes);
	// PATHFINDING_STATIC 1.11: a walker off the mesh, or in a set with no way to the goal, is seated on the nearest ground of the goal's
	// set, searched in 1000 u then 10000 u (the pocket / island / flipped edge polygon a character can land on has no way back otherwise).
	bool reseated = false;
	if ((!path.found || !path.complete || path.points.empty()) && path.failure != knav::PathResult::Failure::EndOffMesh)
	{
		knav::PolyLoc goalLoc;
		const std::uint32_t goalSet = query_.locate(snapped, {2.0f, 40.0f, 2.0f}, goalLoc) ? setOf(goalLoc) : knav::PolyComponents::kNone;
		// Only a walker that is stranded: off the mesh, or in a set under a quarter of the goal's (a pocket, an island, a deck). A goal on
		// an island, a roof, is no reason to move a walker standing on the mainland, and a walker in a big set walled off from the goal's
		// (a chase to an actor behind a wall) is not stranded: it gets NoRoute, so the give-up / retry / leave ladder runs.
		if (goalSet != knav::PolyComponents::kNone
			&& (!startOn || (startSet != goalSet && setSize(startSet) * kStrandedRatio < setSize(goalSet))))
		{
			const auto inGoalSet = [&](const knav::PolyLoc& loc) { return setOf(loc) == goalSet; };
			for (const float widen : {1000.0f, 10000.0f})
			{
				knav::Point seat;
				if (!query_.nearestPointXZ(start, widen, seat, inGoalSet))
					continue;
				const knav::PathResult again = query_.findPath(seat, end, extents, travel);
				stats_.routeClearanceNodes += static_cast<std::uint64_t>(again.clearanceNodes);
				if (again.found && again.complete && !again.points.empty())
				{
					path = again;
					reseated = true;
				}
				break;
			}
		}
	}
	if (!path.found || !path.complete || path.points.empty())
	{
		++stats_.routeFailures[static_cast<std::size_t>(path.failure)];
		route.failure = path.failure == knav::PathResult::Failure::StartOffMesh
			|| path.failure == knav::PathResult::Failure::EndOffMesh ? state::AiReason::OffMesh
			: state::AiReason::NoRoute;
		route.check = pathCheck(path.failure);
		route.plainFailed = path.plainFailed;
		return route;
	}
	const knav::Point& last = path.points.back();
	route.approach = std::hypot(last[0] - toX, last[2] - toZ) > kClickExact;
	// Nowhere to go: the snapped point is the spot the walker stands on.
	if (route.approach && path.points.size() < 2)
	{
		route.approach = false;
		route.failure = state::AiReason::NoRoute;
		route.check = "goal: only a partial path, ending on the start's own spot (the goal's poly is not connected)";
		return route;
	}
	route.found = true;
	route.reseated = reseated;
	for (const knav::Point& p : path.points)
	{
		route.points.push_back({p[0], p[2]});
		route.heights.push_back(p[1]);
	}
	return route;
}

bool MeshNavigator::lineClear(double ax, double ay, double az, double tx, double ty, double tz) const
{
	const double dx = tx - ax;
	const double dz = tz - az;
	const double length = std::hypot(dx, dz);
	const int steps = std::max(1, static_cast<int>(std::ceil(length)));
	const knav::Point half{1.5f, 12.0f, 1.5f};
	double gap = 0.0;
	bool blocked = false;
	for (int i = 1; i < steps; ++i)
	{
		const double t = static_cast<double>(i) / steps;
		const knav::Point p{static_cast<float>(ax + dx * t), static_cast<float>(ay + (ty - ay) * t), static_cast<float>(az + dz * t)};
		if (query_.onMesh(p, half))
			continue;
		blocked = true;
		gap += length / steps;
		if (gap > kThinObstacle)
			return false;
	}
	if (!blocked)
		return true;
	// Something thin is in the way: a free-standing obstacle if the walk round it is short.
	const knav::Point from{static_cast<float>(ax), static_cast<float>(ay), static_cast<float>(az)};
	const knav::Point to{static_cast<float>(tx), static_cast<float>(ty), static_cast<float>(tz)};
	const knav::PathResult path = query_.findPath(from, to, {6.0f, 20.0f, 6.0f});
	return path.found && path.complete && path.length <= kDetourRatio * length + kDetourSlack;
}

Route MeshNavigator::routeToStand(const knav::Position& from, double tx, double ty, double tz, double reach, bool planned,
	Body body) const
{
	constexpr int kAngles = 16;
	constexpr double kRings[] = {0.5, 0.75, 0.9};
	constexpr double kPi = 3.14159265358979323846;
	// The candidates that stand on the mesh within reach with a clear line to the target, in ring-then-angle order.
	struct Candidate
	{
		knav::Point at;
		double straight; // from `from`: no route is shorter, so it bounds the candidate from below
		int order;
	};
	std::vector<Candidate> candidates;
	for (const double ring : kRings)
	{
		for (int a = 0; a < kAngles; ++a)
		{
			const double angle = 2.0 * kPi * a / kAngles;
			const double cx = tx + std::sin(angle) * reach * ring;
			const double cz = tz + std::cos(angle) * reach * ring;
			knav::Point snapped;
			if (!query_.nearestPoint({static_cast<float>(cx), static_cast<float>(ty), static_cast<float>(cz)}, {3.0f, 12.0f, 3.0f}, snapped))
				continue;
			if (std::hypot(snapped[0] - tx, snapped[2] - tz) > reach * 0.95 || std::fabs(snapped[1] - ty) > 30.0)
				continue;
			if (!lineClear(snapped[0], snapped[1], snapped[2], tx, ty, tz))
				continue;
			candidates.push_back({snapped, std::hypot(snapped[0] - from.x, snapped[2] - from.z), static_cast<int>(candidates.size())});
		}
	}
	// Nearest first, and the search stops once the straight line to the next is longer than the best route found (a route is never
	// shorter than the line): the same winner as routing every candidate (the first of equal lengths in the old order), with the
	// far side of the ring never searched (kenshi-k16.7.64).
	std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& l, const Candidate& r) { return l.straight < r.straight; });
	Route best;
	double bestLength = 0.0;
	int bestOrder = 0;
	for (const Candidate& c : candidates)
	{
		if (best.found && c.straight > bestLength)
			break;
		Route route = routeFrom(from, c.at[0], c.at[2], c.at[1], planned, body);
		if (!route.found || route.approach)
			continue;
		double length = 0.0;
		double px = from.x;
		double pz = from.z;
		for (const state::GroundPoint& point : route.points)
		{
			length += std::hypot(point.x - px, point.z - pz);
			px = point.x;
			pz = point.z;
		}
		if (!best.found || length < bestLength || (length == bestLength && c.order < bestOrder))
		{
			best = std::move(route);
			bestLength = length;
			bestOrder = c.order;
		}
	}
	return best;
}

} // namespace ksim
