#include "knav/navmesh.hpp"

#include "kworld/error.hpp"

#include <DetourAlloc.h>
#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourNode.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>
#if defined(KSIM_PROFILE) && KSIM_PROFILE
#include "knav/route_measure.hpp"
#endif

namespace knav {

namespace {

static_assert(std::endian::native == std::endian::little, "cache files are little-endian");

constexpr char kMagic[4] = { 'K', 'N', 'A', 'V' };
constexpr std::uint32_t kCacheVersion = 4; // 4: tile records carry their door polygons; 3 is still read
constexpr int kMaxPathPolys = 4096;

template <class T>
void put(std::ofstream& out, const T& value)
{
	out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <class T>
T get(std::ifstream& in)
{
	T value;
	in.read(reinterpret_cast<char*>(&value), sizeof(T));
	if (!in)
		throw kworld::Error("navmesh cache is truncated");
	return value;
}

float distance(const Point& a, const Point& b)
{
	return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1])
		+ (a[2] - b[2]) * (a[2] - b[2]));
}

// findNearestPoly returns the closest point of any polygon whose bounding box
// meets the query box, which can lie well outside it; keep only hits inside.
bool snapToMesh(const dtNavMeshQuery& query, const Point& point, const Point& halfExtents,
	dtPolyRef& ref, Point& out, const dtQueryFilter& filter = dtQueryFilter())
{
	ref = 0;
	if (dtStatusFailed(query.findNearestPoly(point.data(), halfExtents.data(), &filter, &ref, out.data())))
		return false;
	return ref != 0 && std::fabs(out[0] - point[0]) <= halfExtents[0]
		&& std::fabs(out[1] - point[1]) <= halfExtents[1] && std::fabs(out[2] - point[2]) <= halfExtents[2];
}

// A tile polygon whose detail triangles flag no edge as a polygon boundary crashes
// dtNavMesh::closestPointOnPoly (it picks the nearest boundary edge and dereferences a null edge when
// there is none; found at (-33812, -105859), region 6,2, 2026-10-31). Recast flags an edge only when
// both ends lie within 0.001 of a polygon edge in 3D, and a sliver triangle whose detail vertices were
// sampled at another height than the straight edge has none. Such a polygon gets its flags redone in
// the xz plane. Works on the Detour copy, so the cache bytes do not change and older bakes are covered.
void repairDetailEdges(unsigned char* data)
{
	const auto* header = reinterpret_cast<const dtMeshHeader*>(data);
	std::size_t at = dtAlign4(sizeof(dtMeshHeader));
	const auto* verts = reinterpret_cast<const float*>(data + at);
	at += dtAlign4(sizeof(float) * 3 * static_cast<std::size_t>(header->vertCount));
	const auto* polys = reinterpret_cast<const dtPoly*>(data + at);
	at += dtAlign4(sizeof(dtPoly) * static_cast<std::size_t>(header->polyCount));
	at += dtAlign4(sizeof(dtLink) * static_cast<std::size_t>(header->maxLinkCount));
	const auto* details = reinterpret_cast<const dtPolyDetail*>(data + at);
	at += dtAlign4(sizeof(dtPolyDetail) * static_cast<std::size_t>(header->detailMeshCount));
	const auto* detailVerts = reinterpret_cast<const float*>(data + at);
	at += dtAlign4(sizeof(float) * 3 * static_cast<std::size_t>(header->detailVertCount));
	auto* tris = data + at;

	constexpr float kEdgeTolerance = 0.05f; // world units, xz
	for (int i = 0; i < header->polyCount; ++i)
	{
		const dtPoly& poly = polys[i];
		const dtPolyDetail& detail = details[i];
		if (poly.getType() != DT_POLYTYPE_GROUND)
			continue;
		bool flagged = false;
		for (int t = 0; t < detail.triCount; ++t)
			flagged = flagged || tris[(detail.triBase + t) * 4 + 3] != 0;
		if (flagged)
			continue;
		const auto corner = [&](int index)
		{
			return index < poly.vertCount ? verts + poly.verts[index] * 3
				: detailVerts + (detail.vertBase + index - poly.vertCount) * 3;
		};
		const auto onBoundary = [&](const float* a, const float* b)
		{
			for (int j = 0, k = poly.vertCount - 1; j < poly.vertCount; k = j++)
			{
				const float* p = verts + poly.verts[k] * 3;
				const float* q = verts + poly.verts[j] * 3;
				float dummy;
				if (dtDistancePtSegSqr2D(a, p, q, dummy) < kEdgeTolerance * kEdgeTolerance
					&& dtDistancePtSegSqr2D(b, p, q, dummy) < kEdgeTolerance * kEdgeTolerance)
					return true;
			}
			return false;
		};
		for (int t = 0; t < detail.triCount; ++t)
		{
			unsigned char* tri = &tris[(detail.triBase + t) * 4];
			unsigned int flags = 0;
			for (int e = 0; e < 3; ++e)
				if (onBoundary(corner(tri[e]), corner(tri[(e + 1) % 3])))
					flags |= 1u << (e * 2);
			tri[3] = static_cast<unsigned char>(flags);
		}
	}
}

// Detour owns the copy (DT_TILE_FREE_DATA).
void addToDetour(dtNavMesh& mesh, const BakedTile& tile)
{
	if (const auto* header = reinterpret_cast<const dtMeshHeader*>(tile.data.data()); header->polyCount > kMaxPolysPerTile)
		throw kworld::Error("navmesh tile " + std::to_string(header->x) + " " + std::to_string(header->y) + " has "
			+ std::to_string(header->polyCount) + " polygons, more than the poly id allows");
	auto* data = static_cast<unsigned char*>(dtAlloc(tile.data.size(), DT_ALLOC_PERM));
	std::memcpy(data, tile.data.data(), tile.data.size());
	repairDetailEdges(data);
	if (dtStatusFailed(mesh.addTile(data, static_cast<int>(tile.data.size()), DT_TILE_FREE_DATA, 0, nullptr)))
	{
		dtFree(data);
		throw kworld::Error("dtNavMesh::addTile failed");
	}
}

} // namespace

NavMesh::NavMesh(NavMesh&&) noexcept = default;
NavMesh& NavMesh::operator=(NavMesh&&) noexcept = default;
NavMesh::~NavMesh() = default;

NavMesh NavMesh::fromTiles(const NavConfig& config, std::vector<BakedTile> tiles)
{
	if (tiles.size() > static_cast<std::size_t>(kMaxResidentTiles))
		throw kworld::Error("navmesh has more resident tiles than the tile id allows");
	NavMesh nav;
	nav.config_ = config;
	nav.tiles_ = std::move(tiles);
	nav.mesh_ = { dtAllocNavMesh(), dtFreeNavMesh };

	dtNavMeshParams params{};
	params.orig[0] = static_cast<float>(config.originX);
	params.orig[2] = static_cast<float>(config.originZ);
	params.tileWidth = config.tileSize;
	params.tileHeight = config.tileSize;
	params.maxTiles = static_cast<int>(std::min<long long>(config.window.area(), kMaxResidentTiles));
	params.maxPolys = kMaxPolysPerTile;
	if (dtStatusFailed(nav.mesh_->init(&params)))
		throw kworld::Error("dtNavMesh::init failed");

	for (const BakedTile& tile : nav.tiles_)
		addToDetour(*nav.mesh_, tile);
	nav.countPolys();
	nav.indexDoors();
	return nav;
}

NavMesh NavMesh::fromParams(const DetourParams& params, std::vector<BakedTile> tiles)
{
	if (params.version != DT_NAVMESH_VERSION)
		throw kworld::Error("navmesh params are for another Detour version");
	NavMesh nav;
	nav.config_.originX = params.origin[0];
	nav.config_.originZ = params.origin[2];
	nav.config_.tileSize = params.tileWidth;
	nav.tiles_ = std::move(tiles);
	std::sort(nav.tiles_.begin(), nav.tiles_.end(), [](const BakedTile& a, const BakedTile& b)
		{ return TileCoord{ a.x, a.z } < TileCoord{ b.x, b.z }; });
	TileRect window{ 1 << 20, 1 << 20, -(1 << 20), -(1 << 20) };
	for (const BakedTile& tile : nav.tiles_)
	{
		window.x0 = std::min(window.x0, tile.x);
		window.z0 = std::min(window.z0, tile.z);
		window.x1 = std::max(window.x1, tile.x);
		window.z1 = std::max(window.z1, tile.z);
	}
	nav.config_.window = window;
	nav.mesh_ = { dtAllocNavMesh(), dtFreeNavMesh };
	dtNavMeshParams p{};
	for (int i = 0; i < 3; ++i)
		p.orig[i] = params.origin[i];
	p.tileWidth = params.tileWidth;
	p.tileHeight = params.tileHeight;
	p.maxTiles = static_cast<int>(params.maxTiles);
	p.maxPolys = static_cast<int>(params.maxPolys);
	if (dtStatusFailed(nav.mesh_->init(&p)))
		throw kworld::Error("dtNavMesh::init failed");
	for (const BakedTile& tile : nav.tiles_)
		addToDetour(*nav.mesh_, tile);
	nav.countPolys();
	nav.indexDoors();
	return nav;
}

bool NavMesh::setPolyFlags(int tileX, int tileZ, int layer, int poly, std::uint16_t flags, std::uint8_t area)
{
	const dtMeshTile* tile = std::as_const(*mesh_).getTileAt(tileX, tileZ, layer);
	if (tile == nullptr || poly < 0 || poly >= tile->header->polyCount)
		return false;
	const dtPolyRef ref = mesh_->getPolyRefBase(tile) | static_cast<dtPolyRef>(poly);
	mesh_->setPolyFlags(ref, flags);
	mesh_->setPolyArea(ref, area);
	return true;
}

void NavMesh::indexDoors()
{
	doorRefs_.clear();
	doorOfRef_.clear();
	for (const BakedTile& tile : tiles_)
	{
		const dtMeshTile* loaded = mesh_->getTileAt(tile.x, tile.z, 0);
		for (const DoorPoly& door : tile.doors)
		{
			const dtPolyRef ref = mesh_->getPolyRefBase(loaded) | door.poly;
			doorRefs_[door.door].push_back(ref);
			doorOfRef_[ref] = door.door;
		}
	}
	paintDoors();
}

std::map<std::pair<int, int>, std::vector<NavMesh::PolyPaint>> NavMesh::paintDoors()
{
	std::map<std::pair<int, int>, std::vector<PolyPaint>> changed;
	for (const auto& [door, refs] : doorRefs_)
	{
		for (const dtPolyRef ref : refs)
		{
			const dtMeshTile* tile = nullptr;
			const dtPoly* poly = nullptr;
			std::as_const(*mesh_).getTileAndPolyByRefUnsafe(ref, &tile, &poly);
			const std::uint16_t flags = poly->flags;
			const std::uint8_t area = poly->getArea();
			const DoorPaint paint = doorPaint(book_.get(door));
			setPolyFlags(tile->header->x, tile->header->y, 0, static_cast<int>(poly - tile->polys), paint.flags, paint.area);
			if (poly->flags != flags || poly->getArea() != area)
				changed[{ tile->header->x, tile->header->y }].push_back(
					{ static_cast<std::uint16_t>(poly - tile->polys), poly->flags, poly->getArea() });
		}
	}
	for (auto& [at, polys] : changed)
		std::sort(polys.begin(), polys.end(), [](const PolyPaint& a, const PolyPaint& b) { return a.poly < b.poly; });
	return changed;
}

std::uint64_t NavMesh::nextRevision() noexcept
{
	static std::atomic<std::uint64_t> counter{0};
	return ++counter;
}

void NavMesh::applyDoors(const DoorBook& book)
{
	book_ = book;
	for (auto& [at, polys] : paintDoors())
	{
		revision_ = nextRevision();
		TileLog& log = tileLogs_[at];
		log.patches.push_back({ ++log.version, std::move(polys) });
		if (log.patches.size() > kMaxTilePatches)
			log.patches.erase(log.patches.begin());
	}
}

std::uint32_t NavMesh::tileVersion(int x, int z) const noexcept
{
	const auto found = tileLogs_.find({ x, z });
	return found == tileLogs_.end() ? 1u : found->second.version;
}

const std::vector<NavMesh::TilePatch>& NavMesh::tilePatches(int x, int z) const noexcept
{
	static const std::vector<TilePatch> none;
	const auto found = tileLogs_.find({ x, z });
	return found == tileLogs_.end() ? none : found->second.patches;
}

std::vector<std::uint8_t> NavMesh::tileBlob(int x, int z) const
{
	const BakedTile* baked = tile(x, z);
	if (baked == nullptr)
		return {};
	std::vector<std::uint8_t> blob = baked->data;
	const dtMeshTile* loaded = std::as_const(*mesh_).getTileAt(x, z, 0);
	const std::ptrdiff_t polys = reinterpret_cast<const unsigned char*>(loaded->polys) - loaded->data;
	for (const DoorPoly& door : baked->doors)
	{
		const dtPoly& live = loaded->polys[door.poly];
		unsigned char* at = blob.data() + polys + static_cast<std::ptrdiff_t>(door.poly) * static_cast<std::ptrdiff_t>(sizeof(dtPoly));
		std::memcpy(at + offsetof(dtPoly, flags), &live.flags, sizeof(live.flags));
		std::memcpy(at + offsetof(dtPoly, areaAndtype), &live.areaAndtype, sizeof(live.areaAndtype));
	}
	return blob;
}

std::vector<DoorKey> NavMesh::doorKeys() const
{
	std::vector<DoorKey> keys;
	for (const auto& [door, refs] : doorRefs_)
		keys.push_back(door);
	return keys;
}

std::vector<PolyLoc> NavMesh::doorPolys(DoorKey door) const
{
	std::vector<PolyLoc> polys;
	const auto found = doorRefs_.find(door);
	if (found == doorRefs_.end())
		return polys;
	for (const dtPolyRef ref : found->second)
	{
		unsigned int salt;
		unsigned int tileIndex;
		unsigned int poly;
		mesh_->decodePolyId(ref, salt, tileIndex, poly);
		const dtMeshHeader* header = std::as_const(*mesh_).getTile(static_cast<int>(tileIndex))->header;
		polys.push_back({ header->x, header->y, static_cast<int>(poly), {} });
	}
	std::sort(polys.begin(), polys.end(), [](const PolyLoc& a, const PolyLoc& b)
		{ return std::tie(a.tileZ, a.tileX, a.poly) < std::tie(b.tileZ, b.tileX, b.poly); });
	return polys;
}

NavMesh::DetourParams NavMesh::detourParams() const
{
	const dtNavMeshParams* p = mesh_->getParams();
	DetourParams out;
	for (int i = 0; i < 3; ++i)
		out.origin[i] = p->orig[i];
	out.tileWidth = p->tileWidth;
	out.tileHeight = p->tileHeight;
	out.maxTiles = static_cast<std::uint32_t>(p->maxTiles);
	out.maxPolys = static_cast<std::uint32_t>(p->maxPolys);
	out.version = DT_NAVMESH_VERSION;
	return out;
}

bool NavMesh::doorCentre(DoorKey door, Point& out, DoorFootprint* footprint) const
{
	const auto found = doorRefs_.find(door);
	if (found == doorRefs_.end())
		return false;
	double sum[3] = {};
	std::size_t count = 0;
	for (const dtPolyRef ref : found->second)
	{
		const dtMeshTile* tile = nullptr;
		const dtPoly* poly = nullptr;
		if (dtStatusFailed(std::as_const(*mesh_).getTileAndPolyByRef(ref, &tile, &poly)))
			continue;
		for (int i = 0; i < poly->vertCount; ++i)
		{
			const float* v = tile->verts + poly->verts[i] * 3;
			sum[0] += v[0];
			sum[1] += v[1];
			sum[2] += v[2];
			++count;
		}
	}
	if (count == 0)
		return false;
	for (int i = 0; i < 3; ++i)
		out[static_cast<std::size_t>(i)] = static_cast<float>(sum[i] / static_cast<double>(count));
	if (footprint != nullptr)
	{
		// The principal axis of the vertices (their xz covariance), then the extents along it and across it.
		std::vector<std::array<double, 2>> offsets;
		double xx = 0.0, zz = 0.0, xz = 0.0;
		for (const dtPolyRef ref : found->second)
		{
			const dtMeshTile* tile = nullptr;
			const dtPoly* poly = nullptr;
			if (dtStatusFailed(std::as_const(*mesh_).getTileAndPolyByRef(ref, &tile, &poly)))
				continue;
			for (int i = 0; i < poly->vertCount; ++i)
			{
				const float* v = tile->verts + poly->verts[i] * 3;
				const double dx = v[0] - out[0], dz = v[2] - out[2];
				offsets.push_back({dx, dz});
				xx += dx * dx;
				zz += dz * dz;
				xz += dx * dz;
			}
		}
		const double angle = 0.5 * std::atan2(2.0 * xz, xx - zz);
		const double ax = std::cos(angle), az = std::sin(angle);
		double along = 0.0, across = 0.0;
		for (const std::array<double, 2>& o : offsets)
		{
			along = std::max(along, std::fabs(o[0] * ax + o[1] * az));
			across = std::max(across, std::fabs(-o[0] * az + o[1] * ax));
		}
		footprint->axisX = static_cast<float>(ax);
		footprint->axisZ = static_cast<float>(az);
		footprint->halfLong = static_cast<float>(along);
		footprint->halfDeep = static_cast<float>(across);
	}
	return true;
}

const BakedTile* NavMesh::tile(int x, int z) const noexcept
{
	const auto found = std::lower_bound(tiles_.begin(), tiles_.end(), TileCoord{ x, z },
		[](const BakedTile& t, const TileCoord& at) { return TileCoord{ t.x, t.z } < at; });
	return found != tiles_.end() && found->x == x && found->z == z ? &*found : nullptr;
}

void NavMesh::countPolys()
{
	polyCount_ = 0;
	for (const BakedTile& tile : tiles_)
		polyCount_ += mesh_->getTileAt(tile.x, tile.z, 0)->header->polyCount;
}

void NavMesh::replaceTiles(const std::vector<TileCoord>& removed, std::vector<BakedTile> added)
{
	const auto drop = [&](TileCoord at)
	{
		const auto found = std::find_if(tiles_.begin(), tiles_.end(),
			[&](const BakedTile& t) { return t.x == at.x && t.z == at.z; });
		if (found == tiles_.end())
			return;
		mesh_->removeTile(mesh_->getTileRefAt(at.x, at.z, 0), nullptr, nullptr);
		tiles_.erase(found);
	};
	std::vector<TileCoord> touched = removed;
	for (const TileCoord& at : removed)
		drop(at);
	for (BakedTile& tile : added)
	{
		touched.push_back({ tile.x, tile.z });
		drop({ tile.x, tile.z });
		addToDetour(*mesh_, tile);
		tiles_.push_back(std::move(tile));
	}
	std::sort(tiles_.begin(), tiles_.end(), [](const BakedTile& a, const BakedTile& b)
		{ return TileCoord{ a.x, a.z } < TileCoord{ b.x, b.z }; });
	countPolys();
	indexDoors();
	// A rebuilt tile is a whole new tile: one version step, and no patch chain across it.
	revision_ = nextRevision();
	for (const TileCoord& at : touched)
	{
		TileLog& log = tileLogs_[{ at.x, at.z }];
		++log.version;
		log.patches.clear();
	}
}

NavMesh NavMesh::loadRegion(const std::string& dir, int rx, int rz, int margin)
{
	constexpr int kRegionTiles = kRegionZones * kTilesPerZone;
	constexpr int kRegions = kworld::kZoneCellsPerAxis / kRegionZones;
	std::vector<std::string> paths;
	for (int z = std::max(rz - 1, 0); z <= std::min(rz + 1, kRegions - 1); ++z)
		for (int x = std::max(rx - 1, 0); x <= std::min(rx + 1, kRegions - 1); ++x)
			if (std::string path = dir + "/" + regionFileName(x, z); std::ifstream(path).good())
				paths.push_back(std::move(path));
	return load(paths, { rx * kRegionTiles - margin, rz * kRegionTiles - margin, (rx + 1) * kRegionTiles + margin - 1,
		(rz + 1) * kRegionTiles + margin - 1 });
}

std::size_t NavMesh::prune(const std::vector<Point>& seeds, const Point& reach, bool keepWindowEdge)
{
	const dtNavMesh& nav = *mesh_;
	const auto refOf = [&](const dtMeshTile* tile, int poly) { return nav.getPolyRefBase(tile) | static_cast<dtPolyRef>(poly); };
	for (const BakedTile& baked : tiles_)
	{
		const dtMeshTile* tile = nav.getTileAt(baked.x, baked.z, 0);
		for (int p = 0; p < tile->header->polyCount; ++p)
			if (tile->polys[p].flags == 0)
				mesh_->setPolyFlags(refOf(tile, p), areaFlag(static_cast<Area>(tile->polys[p].getArea())));
	}

	std::unique_ptr<dtNavMeshQuery, void (*)(dtNavMeshQuery*)> query(dtAllocNavMeshQuery(), dtFreeNavMeshQuery);
	if (dtStatusFailed(query->init(&nav, 64)))
		throw kworld::Error("dtNavMeshQuery::init failed");
	std::set<dtPolyRef> reached;
	std::vector<dtPolyRef> stack;
	std::vector<dtPolyRef> roots;
	for (const Point& seed : seeds)
	{
		dtPolyRef ref;
		Point on;
		if (snapToMesh(*query, seed, reach, ref, on))
			roots.push_back(ref);
	}
	if (keepWindowEdge)
	{
		// Detour portal sides: 0 = x+, 2 = z+, 4 = x-, 6 = z-.
		static constexpr int kSideX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
		static constexpr int kSideZ[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
		for (const BakedTile& baked : tiles_)
		{
			const dtMeshTile* tile = nav.getTileAt(baked.x, baked.z, 0);
			for (int p = 0; p < tile->header->polyCount; ++p)
				for (int e = 0; e < tile->polys[p].vertCount; ++e)
				{
					const unsigned short neighbour = tile->polys[p].neis[e];
					if ((neighbour & DT_EXT_LINK) == 0)
						continue;
					const int side = neighbour & 7;
					const int nx = baked.x + kSideX[side];
					const int nz = baked.z + kSideZ[side];
					if (nx < config_.window.x0 || nx > config_.window.x1 || nz < config_.window.z0 || nz > config_.window.z1)
						roots.push_back(refOf(tile, p));
				}
		}
	}
	for (const dtPolyRef ref : roots)
	{
		if (!reached.insert(ref).second)
			continue;
		stack.push_back(ref);
		while (!stack.empty())
		{
			const dtMeshTile* here = nullptr;
			const dtPoly* poly = nullptr;
			nav.getTileAndPolyByRefUnsafe(stack.back(), &here, &poly);
			stack.pop_back();
			for (unsigned int link = poly->firstLink; link != DT_NULL_LINK; link = here->links[link].next)
				if (reached.insert(here->links[link].ref).second)
					stack.push_back(here->links[link].ref);
		}
	}

	std::size_t cut = 0;
	for (BakedTile& baked : tiles_)
	{
		const dtMeshTile* tile = nav.getTileAt(baked.x, baked.z, 0);
		const std::ptrdiff_t polys = reinterpret_cast<const unsigned char*>(tile->polys) - tile->data;
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			const dtPolyRef ref = refOf(tile, p);
			if (doorOfRef_.contains(static_cast<std::uint32_t>(ref)))
				continue;
			if (!reached.contains(ref))
			{
				mesh_->setPolyFlags(ref, 0);
				++cut;
			}
			unsigned char* at = baked.data.data() + polys + static_cast<std::ptrdiff_t>(p) * static_cast<std::ptrdiff_t>(sizeof(dtPoly));
			std::memcpy(at + offsetof(dtPoly, flags), &tile->polys[p].flags, sizeof(tile->polys[p].flags));
		}
	}
	return cut;
}

void NavMesh::save(const std::string& path) const
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(kMagic, sizeof(kMagic));
	put<std::uint32_t>(out, kCacheVersion);
	put<std::uint32_t>(out, DT_NAVMESH_VERSION);
	put(out, config_.cellSize);
	put(out, config_.cellHeight);
	put(out, config_.agentHeight);
	put(out, config_.agentRadius);
	put(out, config_.agentClimb);
	put(out, config_.agentMaxSlope);
	put(out, config_.tileSize);
	put(out, config_.waterLevel);
	put(out, config_.wadeDepth);
	put(out, config_.swimDepth);
	put(out, config_.originX);
	put(out, config_.originZ);
	put<std::int32_t>(out, config_.tilesX);
	put<std::int32_t>(out, config_.tilesZ);
	put<std::int32_t>(out, config_.window.x0);
	put<std::int32_t>(out, config_.window.z0);
	put<std::int32_t>(out, config_.window.x1);
	put<std::int32_t>(out, config_.window.z1);
	put<std::uint32_t>(out, static_cast<std::uint32_t>(tiles_.size()));
	for (const BakedTile& tile : tiles_)
	{
		put<std::int32_t>(out, tile.x);
		put<std::int32_t>(out, tile.z);
		put<std::uint32_t>(out, static_cast<std::uint32_t>(tile.data.size()));
		out.write(reinterpret_cast<const char*>(tile.data.data()),
			static_cast<std::streamsize>(tile.data.size()));
		put<std::uint32_t>(out, static_cast<std::uint32_t>(tile.doors.size()));
		for (const DoorPoly& door : tile.doors)
		{
			put(out, door.poly);
			put(out, door.door);
		}
	}
	if (!out)
		throw kworld::Error("cannot write " + path);
}

NavMesh NavMesh::load(const std::string& path)
{
	return load(std::vector<std::string>{ path }, { 0, 0, std::numeric_limits<int>::max(), std::numeric_limits<int>::max() });
}

NavMesh NavMesh::load(const std::vector<std::string>& paths, const TileRect& keep)
{
	NavConfig merged;
	std::vector<BakedTile> kept = readTiles(paths, keep, merged);
	return fromTiles(merged, std::move(kept));
}

std::vector<BakedTile> NavMesh::readTiles(const std::vector<std::string>& paths, const TileRect& keep, NavConfig& merged)
{
	merged = NavConfig{};
	std::vector<BakedTile> kept;
	for (const std::string& path : paths)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			throw kworld::Error("cannot open " + path);
		char magic[4];
		in.read(magic, sizeof(magic));
		if (!in || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
			throw kworld::Error("not a navmesh cache: " + path);
		const auto version = get<std::uint32_t>(in);
		if (version != kCacheVersion && version != 3)
			throw kworld::Error("unsupported navmesh cache version");
		if (get<std::uint32_t>(in) != DT_NAVMESH_VERSION)
			throw kworld::Error("navmesh cache was baked with another Detour tile version");

		NavConfig config;
		config.cellSize = get<float>(in);
		config.cellHeight = get<float>(in);
		config.agentHeight = get<float>(in);
		config.agentRadius = get<float>(in);
		config.agentClimb = get<float>(in);
		config.agentMaxSlope = get<float>(in);
		config.tileSize = get<float>(in);
		config.waterLevel = get<float>(in);
		config.wadeDepth = get<float>(in);
		config.swimDepth = get<float>(in);
		config.originX = get<double>(in);
		config.originZ = get<double>(in);
		config.tilesX = get<std::int32_t>(in);
		config.tilesZ = get<std::int32_t>(in);
		config.window = { get<std::int32_t>(in), get<std::int32_t>(in), get<std::int32_t>(in), get<std::int32_t>(in) };
		const auto count = get<std::uint32_t>(in);
		if (config.tilesX <= 0 || config.tilesZ <= 0 || config.window.empty() || config.window.x0 < 0
			|| config.window.z0 < 0 || config.window.x1 >= config.tilesX || config.window.z1 >= config.tilesZ
			|| count > static_cast<std::uint64_t>(config.window.area()))
			throw kworld::Error("navmesh cache has an impossible tile grid");

		const TileRect window = config.window;
		if (&path == &paths.front())
			merged = config;
		else
		{
			config.window = merged.window;
			if (!(config == merged))
				throw kworld::Error("navmesh caches differ in grid or agent parameters");
			merged.window = { std::min(merged.window.x0, window.x0), std::min(merged.window.z0, window.z0),
				std::max(merged.window.x1, window.x1), std::max(merged.window.z1, window.z1) };
		}

		for (std::uint32_t i = 0; i < count; ++i)
		{
			BakedTile tile;
			tile.x = get<std::int32_t>(in);
			tile.z = get<std::int32_t>(in);
			const auto size = get<std::uint32_t>(in);
			if (!window.contains(tile.x, tile.z) || size < sizeof(dtMeshHeader) || size > (64u << 20))
				throw kworld::Error("navmesh cache has a bad tile record");
			const bool wanted = keep.contains(tile.x, tile.z);
			if (wanted)
			{
				tile.data.resize(size);
				in.read(reinterpret_cast<char*>(tile.data.data()), size);
			} else {
				in.seekg(size, std::ios::cur);
			}
			if (version >= 4)
			{
				const auto doorCount = get<std::uint32_t>(in);
				if (doorCount > static_cast<std::uint32_t>(kMaxPolysPerTile))
					throw kworld::Error("navmesh cache has a bad door list");
				for (std::uint32_t d = 0; d < doorCount; ++d)
				{
					DoorPoly door;
					door.poly = get<std::uint16_t>(in);
					door.door = get<DoorKey>(in);
					tile.doors.push_back(door);
				}
			}
			if (wanted)
				kept.push_back(std::move(tile));
			if (!in)
				throw kworld::Error("navmesh cache is truncated");
		}
	}
	merged.window = { std::max(merged.window.x0, keep.x0), std::max(merged.window.z0, keep.z0),
		std::min(merged.window.x1, keep.x1), std::min(merged.window.z1, keep.z1) };
	std::sort(kept.begin(), kept.end(), [](const BakedTile& a, const BakedTile& b)
		{ return TileCoord{ a.x, a.z } < TileCoord{ b.x, b.z }; });
	return kept;
}

namespace {

// Query-time clearance (docs/formats/NAVMESH_CACHE.md "Clearance"; kinspect findings/navmesh_runtime_spec.md §3). The
// original bakes with no erosion and applies each traveller's radius r when it plans: a portal is crossed only where r
// fits, the start is pushed off the walls by r + 0.1 Havok, the goal by r + 0.05, the straight path's corners by r + 0.5.
constexpr float kStartPush = 1.0f;
constexpr float kGoalPush = 0.5f;
constexpr float kStraightGoal = 10.0f; // the shortcut's goal tolerance: 1.0 Havok (rwh.144 DAT_14167c308)
constexpr float kCornerPush = 5.0f;
constexpr float kShrinkShare = 0.5f; // shrunkFunnel moves a portal end inward by at most this share of the portal's width
constexpr float kBodyHop = 4.0f; // walkLeg's hop for a body
constexpr float kLevelTolerance = 6.0f; // walkLeg: how far off the straight line between its ends a leg's height may drift
constexpr float kDoorClutter = 20.0f; // route corners this near a door's middle are replaced by its run-in points
constexpr float kDoorRunIn = 8.0f; // the run-in point each side of a door's middle
constexpr float kDoorBesideRange = 4.5f; // a route with both ends on one side of the door's plane is through it only when an end is nearer its middle than this
constexpr float kDoorLocalRange = 12.0f; // ... and the other end is farther than this (a move within the door's own neighbourhood is not)
constexpr float kDoorMiddleSlack = 1.0f; // a route passing a door's centroid nearer than this is left as it is
// A portal fits when some point on it lies r from every wall; points are tried this far apart, the middle first.
constexpr float kClearanceStep = 0.5f;
constexpr int kClearanceSamples = 32;
constexpr float kHeuristicScale = 0.999f; // Detour's H_SCALE
// The fail-fast budget of a clearance search (NavQuery::findPath; NAVMESH_CACHE.md "Clearance"). A radius that fits
// nowhere explores the whole connected set before it proves so (385 ms on the Hub at radius 30); the budget stops it
// at about 40 ms (measured, 2,400 nodes on the Hub's shipped native mesh). Two things keep long routes from running into
// it. The search is skipped when the radius-0 probe's own corridor already lets the radius through (corridorFits). And
// the budget is also kClearanceProbeScale times the nodes the probe used, so a long way round a narrow gap gets room in
// proportion: the Hub to the low ground SW of the copper deposit ran out of 2,400 and answered no_route (client, task U).
// The cost: a hopeless long pair (8 km, the soak) takes up to ~160 ms instead of ~40 ms. A search that runs out says
// OutOfNodes, never a silent shorter walk.
constexpr int kClearanceNodes = 2400;
constexpr int kClearanceProbeScale = 4;
// Routes with TravelFilter::straighten whose ends are closer than kRefineRange xz (k16.6.16): the clearance search places its nodes on the way
// (clearanceCorridor `onTheWay`), and a probe corridor that fits is also checked for a detour when its pulled-tight path is
// over kRefineRatio times the distance (the corner push adds more after). A long walk keeps Detour's node placement and the
// probe's speed (the soak's mean doubled with it).
constexpr float kRefineRange = 300.0f;
constexpr float kRefineRatio = 1.02f;
// A band start's route longer than this (times the xz distance, plus the slack) is planned again from the start's own polygon.
constexpr double kBandDetour = 1.3;
constexpr double kBandSlack = 2.0;
constexpr double kBandSaving = 0.7; // ... and replaces it only when it walks at most this share of it (a long way round a building is not a detour)
constexpr float kBandCut = 0.5f; // the own-polygon route may come this much nearer a wall than the body's own spot (or r)

// What one portal is known to let through: every radius up to `passes`, none from `fails` on.
struct PortalClearance
{
	float passes = 0.0f;
	float fails = std::numeric_limits<float>::infinity();
};

using ClearanceMap = std::unordered_map<std::uint64_t, PortalClearance>; // key: polygon ref << 32 | link index

// Walls are geometric: an edge with no neighbour, whatever the areas on either side.
dtQueryFilter wallFilter()
{
	dtQueryFilter walls;
	walls.setIncludeFlags(0xffff);
	walls.setExcludeFlags(0);
	return walls;
}

// The stretch of `poly`'s edge that `link` crosses (Detour's getPortalPoints, which is private).
void portalOf(const dtMeshTile& tile, const dtPoly& poly, const dtLink& link, float* left, float* right)
{
	const float* a = &tile.verts[poly.verts[link.edge] * 3];
	const float* b = &tile.verts[poly.verts[(link.edge + 1) % poly.vertCount] * 3];
	dtVcopy(left, a);
	dtVcopy(right, b);
	if (link.side != 0xff && (link.bmin != 0 || link.bmax != 255))
	{
		dtVlerp(left, a, b, link.bmin / 255.0f);
		dtVlerp(right, a, b, link.bmax / 255.0f);
	}
}

bool fits(const dtNavMeshQuery& query, ClearanceMap& cache, dtPolyRef ref, unsigned link, const float* left,
	const float* right, float radius)
{
	PortalClearance& known = cache[static_cast<std::uint64_t>(ref) << 32 | link];
	if (radius <= known.passes)
		return true;
	if (radius >= known.fails)
		return false;
	const dtQueryFilter walls = wallFilter();
	const int samples = std::clamp(static_cast<int>(dtVdist2D(left, right) / kClearanceStep), 1, kClearanceSamples);
	for (int k = 0; k <= 2 * samples; ++k)
	{
		const int i = samples / 2 + ((k & 1) != 0 ? (k + 1) / 2 : -(k / 2));
		if (i < 0 || i >= samples)
			continue;
		float at[3];
		dtVlerp(at, left, right, (static_cast<float>(i) + 0.5f) / static_cast<float>(samples));
		float hit = 0.0f;
		float hitPos[3];
		float hitNormal[3];
		if (!dtStatusFailed(query.findDistanceToWall(ref, at, radius, &walls, &hit, hitPos, hitNormal)) && hit >= radius)
		{
			known.passes = radius;
			return true;
		}
	}
	known.fails = radius;
	return false;
}

// Whether every portal of the corridor `polys` (start to end) lets `radius` through. Two polygons can share more than one
// link (a tile border split), so a step passes when any link between them fits, as the clearance A* would take it.
bool corridorFits(const dtNavMeshQuery& query, ClearanceMap& cache, const std::vector<dtPolyRef>& polys, int count,
	float radius, dtPolyRef endRef, bool exemptFirst)
{
	const dtNavMesh& nav = *query.getAttachedNavMesh();
	for (int k = 0; k + 1 < count; ++k)
	{
		const dtPolyRef from = polys[static_cast<std::size_t>(k)];
		const dtPolyRef to = polys[static_cast<std::size_t>(k + 1)];
		if (to == endRef || (k == 0 && exemptFirst))
			continue; // the last hop into the goal is exempt, as in clearanceCorridor; so is the first out of a narrow start
		const dtMeshTile* tile = nullptr;
		const dtPoly* poly = nullptr;
		nav.getTileAndPolyByRefUnsafe(from, &tile, &poly);
		bool crossed = false;
		for (unsigned i = poly->firstLink; i != DT_NULL_LINK && !crossed; i = tile->links[i].next)
		{
			const dtLink& link = tile->links[i];
			if (link.ref != to)
				continue;
			float left[3];
			float right[3];
			portalOf(*tile, *poly, link, left, right);
			crossed = fits(query, cache, from, i, left, right, radius);
		}
		if (!crossed)
			return false;
	}
	return true;
}

// Where a route from `from` towards `to` crosses the portal left..right: the point of the portal on the straight line
// (xz), or its nearer end when the line misses it. Detour places a node at the portal's middle, which on a polygon edge
// 140 u long sends a 47 u walk to the far side of it and back (k16.6.16); this keeps the node on the way.
Point crossingOn(const float* left, const float* right, const Point& from, const Point& to)
{
	const float edgeX = right[0] - left[0];
	const float edgeZ = right[2] - left[2];
	const float lineX = to[0] - from[0];
	const float lineZ = to[2] - from[2];
	const float denom = edgeX * lineZ - edgeZ * lineX;
	float along = 0.5f;
	if (std::fabs(denom) > 1e-6f)
		along = std::clamp(((from[0] - left[0]) * lineZ - (from[2] - left[2]) * lineX) / denom, 0.0f, 1.0f);
	Point at;
	dtVlerp(at.data(), left, right, along);
	return at;
}

// A* over the Detour polygons in Detour's manner (one node per polygon, placed at the middle of the portal it was
// reached through, or with `onTheWay` where the route crosses it: slower on a long search, which expands more nodes),
// skipping the portals `radius` does not fit. `polys` runs from the start to the end, or to the
// node nearest the end when it is out of reach; empty when nothing was searched.
void clearanceCorridor(const dtNavMeshQuery& query, ClearanceMap& cache, dtPolyRef startRef, dtPolyRef endRef,
	const Point& start, const Point& end, const dtQueryFilter& filter, const TravelFilter& travel, int maxNodes,
	std::vector<dtPolyRef>& polys, bool& outOfNodes, bool onTheWay, int& expanded, bool exemptFirst, float squeezeReach)
{
	struct Node
	{
		dtPolyRef ref;
		int parent;
		float g;
		float total;
		Point pos;
		bool closed;
	};
	const dtNavMesh& nav = *query.getAttachedNavMesh();
	const float scale = kHeuristicScale * std::min(1.0f, travel.minCost());
	float dummyT = 0.0f;
	std::vector<Node> nodes{ { startRef, -1, 0.0f, distance(start, end) * scale, start, false } };
	std::unordered_map<dtPolyRef, int> seen{ { startRef, 0 } };
	using Entry = std::pair<float, int>;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
	open.push({ nodes[0].total, 0 });
	int best = 0;
	float bestHeuristic = nodes[0].total;
	outOfNodes = false;
	expanded = 0;
	while (!open.empty())
	{
		const auto [total, at] = open.top();
		open.pop();
		if (nodes[static_cast<std::size_t>(at)].closed || total > nodes[static_cast<std::size_t>(at)].total)
			continue;
		nodes[static_cast<std::size_t>(at)].closed = true;
		++expanded;
		const Node current = nodes[static_cast<std::size_t>(at)];
		if (current.ref == endRef)
		{
			best = at;
			break;
		}
		const dtMeshTile* tile = nullptr;
		const dtPoly* poly = nullptr;
		nav.getTileAndPolyByRefUnsafe(current.ref, &tile, &poly);
		const dtPolyRef parentRef = current.parent >= 0 ? nodes[static_cast<std::size_t>(current.parent)].ref : 0;
		for (unsigned i = poly->firstLink; i != DT_NULL_LINK; i = tile->links[i].next)
		{
			const dtLink& link = tile->links[i];
			if (link.ref == 0 || link.ref == parentRef)
				continue;
			const dtMeshTile* nextTile = nullptr;
			const dtPoly* nextPoly = nullptr;
			nav.getTileAndPolyByRefUnsafe(link.ref, &nextTile, &nextPoly);
			// dtQueryFilter::passFilter, which the shared library does not export.
			if ((nextPoly->flags & filter.getIncludeFlags()) == 0 || (nextPoly->flags & filter.getExcludeFlags()) != 0)
				continue;
			float left[3];
			float right[3];
			portalOf(*tile, *poly, link, left, right);
			// The last hop into the goal's polygon is exempt: a goal against a wall, on a lip or in a narrow doorway is reached by
			// clamping (the shrunk funnel keeps the corner off the wall), not refused after the whole budget (kenshi-k16.7.62).
			// squeezeReach > 0 (the retry of a refused goal, planPathFrom): the portals that close to the goal are exempt too, so a body
			// squeezes only its last step onto a spot a standing character already holds, never through a gap mid-route; only through wall-band (Area::Edge)
			// polygons, the strip the mesh already calls walkable floor too close to a wall for a body (a bare 7 u pinch between two walls stays shut).
			const bool nearGoal = squeezeReach > 0.0f && (poly->getArea() == static_cast<unsigned char>(Area::Edge) || nextPoly->getArea() == static_cast<unsigned char>(Area::Edge))
				&& dtDistancePtSegSqr2D(end.data(), left, right, dummyT) <= squeezeReach * squeezeReach;
			if (link.ref != endRef && !nearGoal && !(exemptFirst && at == 0) && !fits(query, cache, current.ref, i, left, right, travel.radius))
				continue;
			Point mid;
			if (onTheWay)
				mid = crossingOn(left, right, current.pos, end);
			else
				dtVlerp(mid.data(), left, right, 0.5f);
			float g = current.g + distance(current.pos, mid) * filter.getAreaCost(poly->getArea());
			float heuristic = 0.0f;
			if (link.ref == endRef)
				g += distance(mid, end) * filter.getAreaCost(nextPoly->getArea());
			else
				heuristic = distance(mid, end) * scale;
			int index = 0;
			if (const auto found = seen.find(link.ref); found != seen.end())
			{
				index = found->second;
				Node& node = nodes[static_cast<std::size_t>(index)];
				if (g + heuristic >= node.total)
					continue;
				node = { link.ref, at, g, g + heuristic, mid, false };
			} else {
				if (static_cast<int>(nodes.size()) >= maxNodes)
				{
					outOfNodes = true;
					continue;
				}
				index = static_cast<int>(nodes.size());
				nodes.push_back({ link.ref, at, g, g + heuristic, mid, false });
				seen.emplace(link.ref, index);
			}
			open.push({ g + heuristic, index });
			if (heuristic < bestHeuristic)
			{
				bestHeuristic = heuristic;
				best = index;
			}
		}
	}
	polys.clear();
	for (int at = best; at >= 0; at = nodes[static_cast<std::size_t>(at)].parent)
		polys.push_back(nodes[static_cast<std::size_t>(at)].ref);
	std::reverse(polys.begin(), polys.end());
}

// Moves `point` (on `ref`) over the surface until it is `distance` from every wall, or as far as a few steps get it.
void pushOffWalls(const dtNavMeshQuery& query, dtPolyRef& ref, Point& point, float distance, const dtQueryFilter& filter)
{
	const dtQueryFilter walls = wallFilter();
	for (int round = 0; round < 4; ++round)
	{
		float hit = 0.0f;
		float hitPos[3];
		float hitNormal[3];
		if (dtStatusFailed(query.findDistanceToWall(ref, point.data(), distance, &walls, &hit, hitPos, hitNormal))
			|| hit >= distance)
			return;
		float away[2] = { point[0] - hitPos[0], point[2] - hitPos[2] };
		float length = std::hypot(away[0], away[1]);
		if (length < 1e-3f)
		{
			// On the wall itself: head for the middle of the polygon instead.
			const dtMeshTile* tile = nullptr;
			const dtPoly* poly = nullptr;
			query.getAttachedNavMesh()->getTileAndPolyByRefUnsafe(ref, &tile, &poly);
			float centre[3] = { 0.0f, 0.0f, 0.0f };
			for (int v = 0; v < poly->vertCount; ++v)
				dtVadd(centre, centre, &tile->verts[poly->verts[v] * 3]);
			away[0] = centre[0] / poly->vertCount - point[0];
			away[1] = centre[2] / poly->vertCount - point[2];
			length = std::hypot(away[0], away[1]);
			if (length < 1e-3f)
				return;
		}
		const float push = distance - hit;
		const Point target{ point[0] + away[0] / length * push, point[1], point[2] + away[1] / length * push };
		Point moved;
		std::array<dtPolyRef, 16> visited;
		int count = 0;
		if (dtStatusFailed(query.moveAlongSurface(ref, point.data(), target.data(), &filter, moved.data(), visited.data(),
				&count, static_cast<int>(visited.size()))) || count == 0)
			return;
		ref = visited[static_cast<std::size_t>(count - 1)];
		query.getPolyHeight(ref, moved.data(), &moved[1]);
		if (std::hypot(moved[0] - point[0], moved[2] - point[2]) < 1e-3f)
			return;
		point = moved;
	}
}

// The corridor's portals with their wall-vertex ends moved inward, then pulled tight (the funnel) between them: shrink first,
// pull second (kenshi-k16.6.248; the original's corner offset max(0, r + 5 - clearance), rwh.144 3b, applied to the portal ends
// rather than to the corners after the pull). An end that is a wall vertex (clearance 0) moves r + kCornerPush along its portal, at
// most half the portal's width, so the pulled path rounds the vertex at that distance and a corner is only ever where a wall holds
// it; an end with room already (a tile border, an open neighbour) moves by the shortfall only. Returns the corners start..end.
std::vector<Point> shrunkFunnel(const dtNavMeshQuery& query, const std::vector<dtPolyRef>& polys, int count, const Point& start,
	const Point& end, float push)
{
	const dtNavMesh& nav = *query.getAttachedNavMesh();
	const dtQueryFilter walls = wallFilter();
	struct Portal { Point left; Point right; };
	std::vector<Portal> portals;
	portals.reserve(static_cast<std::size_t>(count));
	for (int k = 0; k + 1 < count; ++k)
	{
		const dtMeshTile* tile = nullptr;
		const dtPoly* poly = nullptr;
		nav.getTileAndPolyByRefUnsafe(polys[static_cast<std::size_t>(k)], &tile, &poly);
		Portal portal{};
		bool found = false;
		for (unsigned i = poly->firstLink; i != DT_NULL_LINK && !found; i = tile->links[i].next)
			if (tile->links[i].ref == polys[static_cast<std::size_t>(k + 1)])
			{
				portalOf(*tile, *poly, tile->links[i], portal.left.data(), portal.right.data());
				found = true;
			}
		if (!found)
			return {};
		const float width = dtVdist2D(portal.left.data(), portal.right.data());
		if (push > 0.0f && width > 1e-3f)
		{
			const auto shortfall = [&](const Point& at)
			{
				float hit = 0.0f;
				float hitPos[3];
				float hitNormal[3];
				if (dtStatusFailed(query.findDistanceToWall(polys[static_cast<std::size_t>(k)], at.data(), push, &walls, &hit,
						hitPos, hitNormal)))
					hit = 0.0f;
				return std::min(std::max(0.0f, push - hit), kShrinkShare * width) / width;
			};
			const float moveLeft = shortfall(portal.left);
			const float moveRight = shortfall(portal.right);
			const Portal whole = portal;
			dtVlerp(portal.left.data(), whole.left.data(), whole.right.data(), moveLeft);
			dtVlerp(portal.right.data(), whole.right.data(), whole.left.data(), moveRight);
		}
		portals.push_back(portal);
	}
	std::vector<Point> corners{ start };
	Point apex = start;
	Point left = start;
	Point right = start;
	int leftIndex = 0;
	int rightIndex = 0;
	const int n = count;
	for (int i = 0; i < n; ++i)
	{
		const Point& l = i + 1 < n ? portals[static_cast<std::size_t>(i)].left : end;
		const Point& r = i + 1 < n ? portals[static_cast<std::size_t>(i)].right : end;
		if (dtTriArea2D(apex.data(), right.data(), r.data()) <= 0.0f)
		{
			if (dtVequal(apex.data(), right.data()) || dtTriArea2D(apex.data(), left.data(), r.data()) > 0.0f)
			{
				right = r;
				rightIndex = i;
			} else {
				corners.push_back(left);
				apex = left;
				const int apexIndex = leftIndex;
				left = apex;
				right = apex;
				leftIndex = apexIndex;
				rightIndex = apexIndex;
				i = apexIndex;
				continue;
			}
		}
		if (dtTriArea2D(apex.data(), left.data(), l.data()) >= 0.0f)
		{
			if (dtVequal(apex.data(), left.data()) || dtTriArea2D(apex.data(), right.data(), l.data()) < 0.0f)
			{
				left = l;
				leftIndex = i;
			} else {
				corners.push_back(right);
				apex = right;
				const int apexIndex = rightIndex;
				left = apex;
				right = apex;
				leftIndex = apexIndex;
				rightIndex = apexIndex;
				i = apexIndex;
				continue;
			}
		}
	}
	if (!dtVequal(corners.back().data(), end.data()))
		corners.push_back(end);
	return corners;
}

} // namespace

struct NavQuery::ClearanceCache
{
	ClearanceMap known;
	std::uint64_t revision = 0; // the mesh revision `known` was computed on
};

NavQuery::NavQuery(const NavMesh& mesh, int maxNodes)
	: mesh_(mesh), query_(dtAllocNavMeshQuery()), maxNodes_(maxNodes), clearance_(std::make_unique<ClearanceCache>())
{
	if (dtStatusFailed(query_->init(&mesh.detour(), maxNodes)))
	{
		dtFreeNavMeshQuery(query_);
		throw kworld::Error("dtNavMeshQuery::init failed");
	}
}

NavQuery::~NavQuery()
{
	dtFreeNavMeshQuery(query_);
}

PolyComponents::PolyComponents(const NavMesh& mesh, std::uint16_t flags)
	: mesh_(&mesh)
{
	const dtNavMesh& nav = mesh.detour();
	offset_.assign(static_cast<std::size_t>(nav.getMaxTiles()), kNone);
	std::uint32_t total = 0;
	for (int i = 0; i < nav.getMaxTiles(); ++i)
	{
		if (nav.getTile(i)->header == nullptr)
			continue;
		offset_[static_cast<std::size_t>(i)] = total;
		total += static_cast<std::uint32_t>(nav.getTile(i)->header->polyCount);
	}
	label_.assign(total, kNone);
	std::vector<std::pair<std::uint32_t, std::uint32_t>> stack; // Detour tile index, polygon
	for (int i = 0; i < nav.getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = nav.getTile(i);
		if (tile->header == nullptr)
			continue;
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			std::uint32_t& start = label_[offset_[static_cast<std::size_t>(i)] + static_cast<std::uint32_t>(p)];
			if (start != kNone || (tile->polys[p].flags & flags) == 0)
				continue;
			start = count_;
			stack.push_back({ static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(p) });
			while (!stack.empty())
			{
				const auto [ti, pi] = stack.back();
				stack.pop_back();
				const dtMeshTile* here = nav.getTile(static_cast<int>(ti));
				for (unsigned int link = here->polys[pi].firstLink; link != DT_NULL_LINK; link = here->links[link].next)
				{
					unsigned int salt;
					unsigned int nti;
					unsigned int npi;
					nav.decodePolyId(here->links[link].ref, salt, nti, npi);
					const dtMeshTile* there = nav.getTile(static_cast<int>(nti));
					std::uint32_t& next = label_[offset_[nti] + npi];
					if (next != kNone || (there->polys[npi].flags & flags) == 0)
						continue;
					next = count_;
					stack.push_back({ nti, npi });
				}
			}
			++count_;
		}
	}
}

std::uint32_t PolyComponents::at(int tileX, int tileZ, int poly) const
{
	const dtNavMesh& nav = mesh_->detour();
	const dtMeshTile* tile = nav.getTileAt(tileX, tileZ, 0);
	if (tile == nullptr || poly < 0 || poly >= tile->header->polyCount)
		return kNone;
	return label_[offset_[static_cast<std::size_t>(tile - nav.getTile(0))] + static_cast<std::uint32_t>(poly)];
}

void PolyComponents::forEach(const std::function<void(int, int, int, std::uint32_t)>& visit) const
{
	const dtNavMesh& nav = mesh_->detour();
	for (int i = 0; i < nav.getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = nav.getTile(i);
		if (tile->header == nullptr)
			continue;
		for (int p = 0; p < tile->header->polyCount; ++p)
			visit(tile->header->x, tile->header->y, p, label_[offset_[static_cast<std::size_t>(i)] + static_cast<std::uint32_t>(p)]);
	}
}

void PolyComponents::relabel(const std::vector<std::uint32_t>& table)
{
	for (std::uint32_t& label : label_)
		label = label == kNone ? kNone : table[label];
}

std::vector<std::uint32_t> PolyComponents::around(const std::vector<PolyLoc>& polys) const
{
	const dtNavMesh& nav = mesh_->detour();
	std::vector<std::uint32_t> labels;
	for (const PolyLoc& at : polys)
	{
		const dtMeshTile* tile = nav.getTileAt(at.tileX, at.tileZ, 0);
		if (tile == nullptr || at.poly < 0 || at.poly >= tile->header->polyCount)
			continue;
		for (unsigned int link = tile->polys[at.poly].firstLink; link != DT_NULL_LINK; link = tile->links[link].next)
		{
			unsigned int salt;
			unsigned int nti;
			unsigned int npi;
			nav.decodePolyId(tile->links[link].ref, salt, nti, npi);
			const std::uint32_t label = label_[offset_[nti] + npi];
			if (label != kNone)
				labels.push_back(label);
		}
	}
	std::sort(labels.begin(), labels.end());
	labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
	return labels;
}

bool NavQuery::nearestPoint(const Point& point, const Point& halfExtents, Point& out) const
{
	dtPolyRef ref;
	return snapToMesh(*query_, point, halfExtents, ref, out);
}

bool NavQuery::nearestPointXZ(const Point& point, float reach, Point& out, const std::function<bool(const PolyLoc&)>& accept,
	PolyLoc* where) const
{
	const dtQueryFilter filter;
	std::array<dtPolyRef, 4096> refs;
	int count = 0;
	// Every height counts: the box spans the world vertically, so 0 is as good a centre as any.
	const Point centre{point[0], 0.0f, point[2]};
	const Point extents{reach, 1.0e5f, reach};
	if (dtStatusFailed(query_->queryPolygons(centre.data(), extents.data(), &filter, refs.data(), &count,
			static_cast<int>(refs.size()))))
		return false;
	bool found = false;
	float best = 0.0f;
	for (int i = 0; i < count; ++i)
	{
		// The point itself when it is over the polygon (its y from the height detail), else the nearest
		// point on its boundary, which is where a polygon that only meets the box at an edge lands.
		Point closest;
		bool over = false;
		if (dtStatusFailed(query_->closestPointOnPoly(refs[static_cast<std::size_t>(i)], centre.data(), closest.data(),
				&over)))
			continue;
		const float dx = closest[0] - centre[0];
		const float dz = closest[2] - centre[2];
		const float dist = std::sqrt(dx * dx + dz * dz);
		PolyLoc loc;
		if (accept || where != nullptr)
		{
			unsigned int salt;
			unsigned int tileIndex;
			unsigned int poly;
			mesh_.detour().decodePolyId(refs[static_cast<std::size_t>(i)], salt, tileIndex, poly);
			const dtMeshHeader* header = mesh_.detour().getTile(static_cast<int>(tileIndex))->header;
			loc = {header->x, header->y, static_cast<int>(poly), closest};
			if (accept && !accept(loc))
				continue;
		}
		// The query box is a square and the reach a radius: a polygon's box can meet the box with its
		// closest point outside the reach.
		if (dist > reach)
			continue;
		// The smallest distance wins; an exact tie is a floor stacked over another, and the lowest stays (the ground
		// under a deck, as the 3D snap from y = 0 chose).
		if (found && (dist > best || (dist == best && closest[1] >= out[1])))
			continue;
		found = true;
		best = dist;
		out = closest;
		if (where != nullptr)
			*where = loc;
	}
	return found;
}

bool NavQuery::locate(const Point& point, const Point& halfExtents, PolyLoc& out, const TravelFilter& travel) const
{
	dtPolyRef ref;
	dtQueryFilter filter;
	configureFilter(filter, travel);
	if (!snapToMesh(*query_, point, halfExtents, ref, out.point, filter))
		return false;
	unsigned int salt;
	unsigned int tileIndex;
	unsigned int poly;
	mesh_.detour().decodePolyId(ref, salt, tileIndex, poly);
	const dtMeshHeader* header = mesh_.detour().getTile(static_cast<int>(tileIndex))->header;
	out.tileX = header->x;
	out.tileZ = header->y;
	out.poly = static_cast<int>(poly);
	return true;
}

bool NavQuery::locateWhere(const Point& point, const Point& halfExtents,
	const std::function<bool(const PolyLoc&)>& accept, PolyLoc& out, const TravelFilter& travel) const
{
	dtQueryFilter filter;
	configureFilter(filter, travel);
	std::array<dtPolyRef, 2048> refs;
	int count = 0;
	if (dtStatusFailed(query_->queryPolygons(point.data(), halfExtents.data(), &filter, refs.data(), &count,
			static_cast<int>(refs.size()))))
		return false;
	bool found = false;
	float best = 0.0f;
	for (int i = 0; i < count; ++i)
	{
		PolyLoc loc;
		bool over = false;
		if (dtStatusFailed(query_->closestPointOnPoly(refs[static_cast<std::size_t>(i)], point.data(),
				loc.point.data(), &over)))
			continue;
		if (std::fabs(loc.point[0] - point[0]) > halfExtents[0] || std::fabs(loc.point[1] - point[1]) > halfExtents[1]
			|| std::fabs(loc.point[2] - point[2]) > halfExtents[2])
			continue;
		unsigned int salt;
		unsigned int tileIndex;
		unsigned int poly;
		mesh_.detour().decodePolyId(refs[static_cast<std::size_t>(i)], salt, tileIndex, poly);
		const dtMeshHeader* header = mesh_.detour().getTile(static_cast<int>(tileIndex))->header;
		loc.tileX = header->x;
		loc.tileZ = header->y;
		loc.poly = static_cast<int>(poly);
		const float dist = distance(loc.point, point);
		if ((found && dist >= best) || !accept(loc))
			continue;
		found = true;
		best = dist;
		out = loc;
	}
	return found;
}

bool NavQuery::onMesh(const Point& point, const Point& halfExtents) const
{
	Point nearest;
	return nearestPoint(point, halfExtents, nearest);
}

bool NavQuery::slide(const Point& from, const Point& to, const Point& halfExtents, Point& out, bool& blocked,
	const TravelFilter& travel) const
{
	dtQueryFilter filter;
	configureFilter(filter, travel);
	dtPolyRef startRef = 0;
	Point start;
	if (!snapToMesh(*query_, from, halfExtents, startRef, start, filter))
		return false;
	constexpr int kVisited = 16;
	std::array<dtPolyRef, kVisited> visited;
	int visitedCount = 0;
	// Floats near the world edge are 0.004 apart, so a hair is a few units in the last place.
	constexpr float kHair = 0.01f;
	constexpr float kVertexHair = 0.03f;
	constexpr float kOvershoot = 1.5f; // the start snaps to the mesh by up to kStand before the slide (Walker's kHopOvershoot)
	auto remaining = [&](const Point& at) { return std::hypot(at[0] - to[0], at[2] - to[2]); };
	bool moved = !dtStatusFailed(query_->moveAlongSurface(startRef, start.data(), to.data(), &filter, out.data(),
		visited.data(), &visitedCount, kVisited)) && visitedCount > 0;
	// A walk never ends farther from where it started than the aim: moveAlongSurface from a polygon corner has returned a point 9 u off a
	// 2 u hop (R73 leg 28 of ksim_route_walk, k16.8.37). That result is no move; the neighbours below get their turn.
	const float aim = std::hypot(to[0] - from[0], to[2] - from[2]);
	auto overshoots = [&](const Point& at) { return std::hypot(at[0] - start[0], at[2] - start[2]) > aim + kOvershoot; };
	const bool wild = moved && overshoots(out);
	if (wild)
		moved = false;
	if (!moved)
		out = start;
	// A walker standing on a polygon corner (a route corner is one) can be snapped to a polygon the segment
	// does not leave through: moveAlongSurface then goes nowhere (found at (-51940, 2688), the Hub's west
	// gate climb, 2026-11-02), or fails with no polygon visited at all (the same corner, a 0.1 u hop to the
	// route's next point, (-49192, 3004) 2026-11-05). Every other polygon touching the start gets its turn,
	// and the best result stands.
	if (!moved || (remaining(out) > kHair && std::hypot(out[0] - start[0], out[2] - start[2]) <= kHair))
	{
		constexpr float kCorner = 0.25f;
		const Point box{kCorner, halfExtents[1], kCorner};
		std::array<dtPolyRef, 16> around;
		int aroundCount = 0;
		if (!dtStatusFailed(query_->queryPolygons(start.data(), box.data(), &filter, around.data(), &aroundCount,
				static_cast<int>(around.size()))))
		{
			for (int i = 0; i < aroundCount; ++i)
			{
				if (around[static_cast<std::size_t>(i)] == startRef)
					continue;
				Point from2;
				bool over = false;
				Point tryOut;
				std::array<dtPolyRef, kVisited> tryVisited;
				int tryCount = 0;
				if (dtStatusFailed(query_->closestPointOnPoly(around[static_cast<std::size_t>(i)], start.data(),
						from2.data(), &over))
					|| dtStatusFailed(query_->moveAlongSurface(around[static_cast<std::size_t>(i)], from2.data(), to.data(),
						&filter, tryOut.data(), tryVisited.data(), &tryCount, kVisited)) || tryCount == 0)
					continue;
				if (overshoots(tryOut))
					continue;
				if (!moved || remaining(tryOut) < remaining(out))
				{
					out = tryOut;
					visited = tryVisited;
					visitedCount = tryCount;
					moved = true;
				}
			}
		}
	}
	// A hop that ENDS exactly on a polygon vertex or tile-border corner can fail outright (no polygon visited), not only stop short: a route
	// corner is such a point (R221 leg 39, R234 leg 42 of ksim_route_walk, k16.8.37). The same hop a hair short of it is tried before the
	// walker is called off the mesh.
	if (!moved)
	{
		const float full = std::hypot(to[0] - start[0], to[2] - start[2]);
		if (full > kVertexHair * 4.0f)
		{
			const Point shorter{to[0] + (start[0] - to[0]) / full * kVertexHair, to[1], to[2] + (start[2] - to[2]) / full * kVertexHair};
			Point tryOut;
			std::array<dtPolyRef, kVisited> tryVisited;
			int tryCount = 0;
			if (!dtStatusFailed(query_->moveAlongSurface(startRef, start.data(), shorter.data(), &filter, tryOut.data(), tryVisited.data(),
					&tryCount, kVisited)) && tryCount > 0)
			{
				out = tryOut;
				visited = tryVisited;
				visitedCount = tryCount;
				moved = true;
			}
		}
	}
	if (!moved && wild)
	{
		out = start; // a wild result and no neighbour that walks: blocked where it stands, still on the mesh
		visited[0] = startRef;
		visitedCount = 1;
		moved = true;
	}
	if (!moved)
		return false;
	// The detail surface under the end point: the last polygon, else an earlier visited one, else the polygon's closest point (which
	// is detail-corrected). A point on a polygon seam can miss every detail triangle by a hair, and moveAlongSurface's own height is
	// the flat polygon plane, metres off on a steep stair polygon (seed 6 of ksim_fight_levels, Hub bar stair (-51139, 2620.7)).
	{
		float height = out[1];
		bool found = false;
		for (int i = visitedCount - 1; i >= 0 && !found; --i)
			found = !dtStatusFailed(query_->getPolyHeight(visited[static_cast<std::size_t>(i)], out.data(), &height));
		if (!found)
		{
			Point closest;
			bool over = false;
			found = !dtStatusFailed(query_->closestPointOnPoly(visited[static_cast<std::size_t>(visitedCount - 1)], out.data(), closest.data(), &over));
			height = closest[1];
		}
		if (found)
			out[1] = height;
	}
	blocked = remaining(out) > kHair;
	// moveAlongSurface can also refuse a hop that ENDS exactly on a polygon vertex: it stops a step short on the
	// edge (the Hub's ramp top (-50802, 2756), where the walk stopped at (-50806, 2754), k16.8.6). The same hop to
	// a point a hair short of the vertex arrives, so that is tried once and stands only if it gets there; the
	// caller is then a hair from the target, not blocked. A real wall blocks the shortened hop too.
	const float length = std::hypot(to[0] - start[0], to[2] - start[2]);
	if (blocked && length > kVertexHair * 4.0f)
	{
		const Point shorter{to[0] + (start[0] - to[0]) / length * kVertexHair, to[1], to[2] + (start[2] - to[2]) / length * kVertexHair};
		Point reach;
		std::array<dtPolyRef, kVisited> reachVisited;
		int reachCount = 0;
		if (!dtStatusFailed(query_->moveAlongSurface(startRef, start.data(), shorter.data(), &filter, reach.data(),
				reachVisited.data(), &reachCount, kVisited)) && reachCount > 0
			&& std::hypot(reach[0] - shorter[0], reach[2] - shorter[2]) <= kHair)
		{
			out = reach;
			float reachHeight = out[1];
			if (!dtStatusFailed(query_->getPolyHeight(reachVisited[static_cast<std::size_t>(reachCount - 1)], out.data(), &reachHeight)))
				out[1] = reachHeight;
			blocked = false;
		}
	}
	return true;
}

float NavQuery::wallDistance(const PolyLoc& loc, float limit) const
{
	const dtMeshTile* tile = mesh_.detour().getTileAt(loc.tileX, loc.tileZ, 0);
	if (tile == nullptr)
		return 0.0f;
	const dtQueryFilter walls = wallFilter();
	float hit = 0.0f;
	float hitPos[3];
	float hitNormal[3];
	if (dtStatusFailed(query_->findDistanceToWall(mesh_.detour().getPolyRefBase(tile) | static_cast<dtPolyRef>(loc.poly),
			loc.point.data(), limit, &walls, &hit, hitPos, hitNormal)))
		return 0.0f;
	return hit;
}

void configureFilter(dtQueryFilter& out, const TravelFilter& travel)
{
	out.setIncludeFlags(travel.allowed);
	out.setExcludeFlags(travel.throughLocks ? 0u : kDoorLockedFlag);
	for (int i = 0; i < kAreaCount; ++i)
		out.setAreaCost(i, travel.cost[static_cast<std::size_t>(i)]);
}

// A straight leg the walker's own slide can walk, as a point (`radius` 0) or as a body: the slide also runs along the two lines
// `radius` to the left and right of the leg, so a wall nearer than the body's radius (or an edge of the mesh) closes it.
bool NavQuery::walkLeg(const Point& from, const Point& to, float radius, const TravelFilter& travel) const
{
	if (from == to)
		return true;
	// A walker's step (1 u) for a point; a body's legs hop 4 u: the offset lines already hold the wall room, and the shortcut pass tries many legs.
	const float hop = radius > 0.0f ? kBodyHop : 1.0f;
	const auto open = [&](const Point& a, const Point& b)
	{
		const Point extents{ 1.0f, 12.0f, 1.0f }; // the walker's own snap (walk.cpp kStand)
		const int hops = std::max(1, static_cast<int>(std::ceil(distance(a, b) / hop)));
		Point at = a;
		for (int i = 1; i <= hops; ++i)
		{
			const Point target{ a[0] + (b[0] - a[0]) * i / hops, at[1], a[2] + (b[2] - a[2]) * i / hops };
			bool blocked = false;
			Point out;
			if (!slide(at, target, extents, out, blocked, travel) || blocked)
				return false;
			// A body's leg stays on its own level (the revert pass above walks as it always did): where floors stack (a stair's bend, a deck over ground) the slide may take the other one.
			if (radius > 0.0f && std::fabs(out[1] - (a[1] + (b[1] - a[1]) * i / hops)) > kLevelTolerance)
				return false;
			at = out;
		}
		return true;
	};
	if (!open(from, to))
		return false;
	if (radius <= 0.0f)
		return true;
	const float length = distance(from, to);
	if (length < 1e-3f)
		return true;
	const float sideX = -(to[2] - from[2]) / length * radius;
	const float sideZ = (to[0] - from[0]) / length * radius;
	for (const float sign : { 1.0f, -1.0f })
	{
		const Point a{ from[0] + sign * sideX, from[1], from[2] + sign * sideZ };
		const Point b{ to[0] + sign * sideX, to[1], to[2] + sign * sideZ };
		if (!open(a, b))
			return false;
	}
	return true;
}

// The body's width walk for the straight-walk shortcut: the goal is ground the body fits on (no wall or ledge within r: a chaser's exact goal on
// a ledge edge walked a fighter off the upper floor, ksim_fight_levels; such a click keeps the snap and push), and the leg walks clear for the
// whole body. One polygon-corridor ray with the same filter confirms the walk crosses no door polygon (water and closed doors are the filter's).
bool NavQuery::straightWalk(const Point& from, const Point& to, const TravelFilter& travel) const
{
	PolyLoc at;
	if (distance(from, to) < 1e-3f || !locate(to, { 1.0f, 12.0f, 1.0f }, at, travel) || wallDistance(at, travel.radius) < travel.radius
		|| !walkLeg(from, to, travel.radius, travel))
		return false;
	dtQueryFilter filter;
	configureFilter(filter, travel);
	const float extents[3] = { 1.0f, 12.0f, 1.0f };
	dtPolyRef a = 0;
	dtPolyRef b = 0;
	float nearA[3];
	float nearB[3];
	if (dtStatusFailed(query_->findNearestPoly(from.data(), extents, &filter, &a, nearA)) || a == 0
		|| dtStatusFailed(query_->findNearestPoly(to.data(), extents, &filter, &b, nearB)) || b == 0)
		return false;
	std::vector<dtPolyRef> hit(kMaxPathPolys);
	float t = 0.0f;
	float normal[3];
	int count = 0;
	if (dtStatusFailed(query_->raycast(a, nearA, nearB, &filter, &t, normal, hit.data(), &count, kMaxPathPolys)) || t < 1.0f - 0.3f / distance(from, to)) // the ray may stop a hair short on a boundary click
		return false;
	for (int i = 0; i < count; ++i)
		if (mesh_.doorOfRef_.count(hit[static_cast<std::size_t>(i)]) != 0)
			return false;
	return true;
}

NavQuery::ClearanceCache& NavQuery::clearanceFor() const
{
	// Keys are polygon refs and link indices: replaceTiles re-links neighbour tiles (an index can name another portal) and a
	// new wall across a seam changes widths, and a door patch repaints polygons. Any revision step drops the lot (kenshi-k16.7.63).
	if (clearance_->revision != mesh_.revision())
	{
		clearance_->known.clear();
		clearance_->revision = mesh_.revision();
	}
	return *clearance_;
}

std::size_t NavQuery::clearanceEntries() const
{
	return clearanceFor().known.size();
}

PathResult NavQuery::findPath(const Point& from, const Point& to, const Point& halfExtents,
	const TravelFilter& travel) const
{
	Point snappedStart{};
	PathResult result = planPath(from, to, halfExtents, travel, &snappedStart);
	if (result.found && travel.radius > 0.0f && result.doors.size() == 1 && result.points.size() >= 2)
		throughDoorMiddle(result, from, to, halfExtents, travel, snappedStart);
	return result;
}

PathResult NavQuery::planPath(const Point& from, const Point& to, const Point& halfExtents,
	const TravelFilter& travel, Point* snappedStart) const
{
	// The refit route first (a start in a wall's band is planned from the nearest ground the body fits). When that route doubles back
	// (the refit can lie up to 4 r away, across the room), the start's own polygon is planned too, its way out exempt from the clearance
	// test, and the shorter complete route stays (kenshi-k16.6.357). Only the detoured routes change: a plain refit is kept as it was.
	bool banded = false;
	Point snapped{};
	PathResult refit = planPathFrom(from, to, halfExtents, travel, &snapped, false, &banded);
	if (snappedStart != nullptr)
		*snappedStart = snapped;
	const double direct = std::hypot(to[0] - from[0], to[2] - from[2]);
	// What the body walks: the route's own length plus the leg from where it stands to the route's first point (the refit's).
	const auto walked = [&](const PathResult& route)
	{
		return route.length + (route.points.empty() ? 0.0 : std::hypot(route.points[0][0] - from[0], route.points[0][2] - from[2]));
	};
	// A refit whose first point the walker cannot reach from where the body stands (a wall corner between: the Hub bar foyer, k16.8.54) is no
	// route at all: the body slides to a halt on it and the walk is abandoned unseen. The start's own polygon is planned instead, whatever it walks.
	const bool leadShut = banded && refit.found && !refit.points.empty() && !walkLeg(from, refit.points[0], 0.0f, travel);
	// A refit that ran out of nodes is a hopeless pair: a second search from the own polygon would spend a second budget on it.
	if (!banded || refit.failure == PathResult::Failure::OutOfNodes || !refit.found || (!leadShut && walked(refit) <= kBandDetour * direct + kBandSlack))
		return refit;
	PathResult own = planPathFrom(from, to, halfExtents, travel, nullptr, true, &banded);
	// Both searches' work is the call's work, whichever route stays (the deterministic cost count the route soak gates on).
	own.clearanceNodes += refit.clearanceNodes;
	refit.clearanceNodes = own.clearanceNodes;
	if (!own.found || !own.complete || (!leadShut && walked(own) >= kBandSaving * walked(refit)))
		return refit;
	// The exempt first hop may cut a corner: the walk keeps every sample at least as far from the walls as the body's own spot is (or r).
	const float r = travel.radius;
	const Point reach{ 1.0f, std::min(halfExtents[1], 4.0f * r), 1.0f };
	const auto wallAt = [&](const Point& at)
	{
		PolyLoc loc;
		return locate(at, reach, loc, travel) ? wallDistance(loc, r) : 0.0f;
	};
	// Never below kStartPush: a start standing on a wall (distance 0) must not license a route along it.
	const float floorDistance = std::max(std::min(wallAt(snapped), r) - kBandCut, kStartPush);
	for (std::size_t i = 1; i < own.points.size(); ++i)
	{
		const Point& a = own.points[i - 1];
		const Point& b = own.points[i];
		const int steps = std::max(1, static_cast<int>(distance(a, b) / kClearanceStep));
		for (int k = 1; k <= steps; ++k)
		{
			Point at;
			dtVlerp(at.data(), a.data(), b.data(), static_cast<float>(k) / static_cast<float>(steps));
			if (wallAt(at) < floorDistance)
				return refit;
		}
	}
	return own;
}

PathResult NavQuery::planPathFrom(const Point& from, const Point& to, const Point& halfExtents,
	const TravelFilter& travel, Point* snappedStart, bool ownStart, bool* exempted) const
{
	PathResult result;
	bool exemptFirst = false;
	ClearanceMap& clearance = clearanceFor().known;
	dtQueryFilter filter;
	configureFilter(filter, travel);
	dtPolyRef startRef = 0;
	dtPolyRef endRef = 0;
	Point start;
	Point end;
	if (!snapToMesh(*query_, from, halfExtents, startRef, start, filter))
	{
		result.failure = PathResult::Failure::StartOffMesh;
		return result;
	}
	if (!snapToMesh(*query_, to, halfExtents, endRef, end, filter))
	{
		result.failure = PathResult::Failure::EndOffMesh;
		return result;
	}
	// Straight-walk shortcut first (rwh.144 section 1d.2, exe 1403aaa90; PATHFINDING_STATIC section 1.8): a width-aware walk along the mesh from
	// the start to the raw goal, accepted when it ends within kStraightGoal of the goal; then no goal snap, no wall push and no search. The body
	// walks clear for the whole body and the goal is ground it fits on (straightWalk); a click inside r of a wall keeps the snap and push.
	// A walk through a door polygon is refused (the door's own routing, throughDoorMiddle, needs the corridor).
	if (travel.radius > 0.0f && distance(end, to) <= kStraightGoal && straightWalk(start, end, travel))
	{
		result.found = true;
		result.complete = true;
		result.points = { start, end };
		result.length = distance(start, end);
		result.cost = result.length;
		if (snappedStart != nullptr)
			*snappedStart = start;
		return result;
	}
	const dtPolyRef plainStart = startRef; // before a refit onto ground the body fits
	if (snappedStart != nullptr)
		*snappedStart = start; // on the mesh, before the refit and the wall push
	if (travel.radius > 0.0f)
	{
		// A start standing where its body does not fit (a roof edge, a stair lip, a doorway threshold: `wallDistance < r` on the
		// polygon the plain snap picked) is on a polygon the clearance search can never leave, so every route off it closes right
		// there (the Hub tower's roof, user play 2026-11-07: every order off a roof the character had walked onto was no_route,
		// while the same ends route at radius 0). The start then snaps on to the nearest polygon the body fits on - the
		// original's start snap r + 0.1 (kinspect navmesh_runtime_spec section 3) - and the route runs from there. The goal is
		// left to the caller's snap: a narrow goal polygon is reachable (the corridor arrives and the end is clamped onto it),
		// only a narrow start closes. The fitting ground is taken within 4 r in every direction, OURS (about the body's own
		// height, 18 u): a body walks to its own start snap, it does not teleport. The same bound down matters as much: the
		// caller's box is 340 high, and a roof edge over ground 50 u below must not turn the start into a drop through the roof
		// (a squad token on a hillside may still sit a few units off the mesh's own height, ksim_world_follow). With none in
		// reach the plain snap stands and the search refuses as before.
		const float r = travel.radius;
		const Point reach{ std::min(halfExtents[0], 4.0f * r), std::min(halfExtents[1], 4.0f * r),
			std::min(halfExtents[2], 4.0f * r) };
		PolyLoc at;
		PolyLoc fit;
		if (locate(start, reach, at, travel) && wallDistance(at, r) < r
			&& locateWhere(start, reach, [&](const PolyLoc& loc) { return wallDistance(loc, r) >= r; }, fit, travel))
		{
			const dtMeshTile* tile = mesh_.detour().getTileAt(fit.tileX, fit.tileZ, 0);
			// First try the start's own polygon, its way out exempt from the clearance test (the character stands there; a refit across
			// the room sent the route back and forth, kenshi-k16.6.357). A route that does not get out is planned again from the refit.
			*exempted = true;
			if (ownStart)
				exemptFirst = true;
			else if (tile != nullptr)
			{
				start = fit.point;
				startRef = mesh_.detour().getPolyRefBase(tile) | static_cast<dtPolyRef>(fit.poly);
			}
		}
	}

	Scratch& sc = scratch_;
	std::vector<dtPolyRef>& polys = sc.polys;
	polys.assign(kMaxPathPolys, 0); // full size: Detour is handed polys.data() with a capacity of kMaxPathPolys
	int polyCount = 0;
	dtStatus searched = DT_SUCCESS;
	if (travel.radius > 0.0f)
	{
		// The route the same traveller takes with no clearance. When there is none at all there is none with
		// clearance either (clearance only refuses portals), so a hopeless pair costs this one search instead of the
		// whole budget. About 0.1 ms on the Hub.
		const dtStatus probe = query_->findPath(startRef, endRef, start.data(), end.data(), &filter, polys.data(),
			&polyCount, kMaxPathPolys);
		if (dtStatusFailed(probe) || polyCount == 0)
		{
			result.failure = PathResult::Failure::NoPath;
			result.plainFailed = startRef == plainStart;
			return result;
		}
		const int probed = query_->getNodePool() != nullptr ? query_->getNodePool()->getNodeCount() : 0;
		// A probe that closed short of the goal (the whole reachable set searched, no node limit hit) is the answer (kenshi-k16.6.262):
		// a clearance search only refuses portals, so it cannot get there either and used to run its whole budget (30-50 ms at Heft)
		// to say so. The original accepts only a goal-reached search and discards partials (rwh.144 section 2). Taken before the pushes,
		// which move the ends.
		const bool probeClosed = polys[static_cast<std::size_t>(polyCount - 1)] != endRef && (probe & DT_OUT_OF_NODES) == 0
			&& polyCount < kMaxPathPolys;
		result.plainFailed = probeClosed && startRef == plainStart;
		pushOffWalls(*query_, startRef, start, travel.radius + kStartPush, filter);
		pushOffWalls(*query_, endRef, end, travel.radius + kGoalPush, filter);
		// The probe's corridor is the plain search's best: when it is complete, still runs between the pushed ends, and
		// every portal on it fits, the clearance A* (the same costs, fewer portals) would find the same one, unless it is a
		// long way round open ground (Detour's nodes sit at portal middles, k16.6.16): a short route whose pulled-tight path
		// is much longer than the straight line gets the clearance search too, and the shorter path stays.
		const bool probeFits = polys[0] == startRef && polys[static_cast<std::size_t>(polyCount - 1)] == endRef
			&& corridorFits(*query_, clearance, polys, polyCount, travel.radius, endRef, exemptFirst);
		const auto pulledLength = [&](const std::vector<dtPolyRef>& corridor, int count)
		{
			std::vector<float>& corners = sc.corners;
			corners.resize(3ull * kMaxPathPolys);
			int cornerCount = 0;
			float length = 0.0f;
			if (!dtStatusFailed(query_->findStraightPath(start.data(), end.data(), corridor.data(), count, corners.data(), nullptr,
					nullptr, &cornerCount, kMaxPathPolys)))
				for (int i = 1; i < cornerCount; ++i)
					length += dtVdist(&corners[3ull * (i - 1)], &corners[3ull * i]);
			return length;
		};
		const float direct = distance(start, end);
		const bool shortWay = travel.straighten && direct < kRefineRange;
		const float probeLength = probeFits && shortWay ? pulledLength(polys, polyCount) : 0.0f; // once: used twice below
		const bool roundabout = probeFits && shortWay && probeLength > kRefineRatio * direct;
		if (!probeClosed && (!probeFits || roundabout))
		{
#if defined(KSIM_PROFILE) && KSIM_PROFILE
			const std::uint64_t clearBegan = measure::now();
			(roundabout ? measure::counters.probeFitsRoundabout : measure::counters.clearanceNoFit).fetch_add(1, std::memory_order_relaxed);
#endif
			bool outOfNodes = false;
			std::vector<dtPolyRef>& clear = sc.clear;
			clear.clear();
			clearanceCorridor(*query_, clearance, startRef, endRef, start, end, filter, travel,
				std::min(maxNodes_, std::max(kClearanceNodes, kClearanceProbeScale * probed)), clear, outOfNodes, shortWay,
				result.clearanceNodes, exemptFirst, 0.0f);
			// The strict search refused a goal the plain search reaches (the Hub bar foyer: the last portals before the barman's spot are
			// furniture-ring gaps no body fits, so the search floods its budget, kenshi-k16.8.46). Run once more with the portals within
			// a body diameter of the goal exempt. Only after a refusal, so a successful search costs nothing extra.
			if (!roundabout && polys[0] == startRef && polys[static_cast<std::size_t>(polyCount - 1)] == endRef
				&& (outOfNodes || clear.empty() || clear.back() != endRef))
			{
				int again = 0;
				clearanceCorridor(*query_, clearance, startRef, endRef, start, end, filter, travel,
					std::min(maxNodes_, std::max(kClearanceNodes, kClearanceProbeScale * probed)), clear, outOfNodes, shortWay,
					again, exemptFirst, 2.0f * travel.radius);
				result.clearanceNodes += again;
			}
			// A copy, never a move: `polys` is also the buffer the probe above hands Detour with a capacity of kMaxPathPolys
			// (`polys.data()`), and a moved-from vector drops to the corridor's own size. A second search from a shrunk buffer was
			// a heap overflow (a retry-after-failure variant of the start refit, 2026-11-07: Detour wrote its whole path over the
			// heap and the abort surfaced in clearanceCorridor's own map; the shipped code searches once per call, so nothing
			// here was reachable, and the invariant keeps it that way).
			if (!roundabout || (!outOfNodes && !clear.empty() && clear.back() == endRef
					&& pulledLength(clear, static_cast<int>(clear.size())) < probeLength))
			{
#if defined(KSIM_PROFILE) && KSIM_PROFILE
				if (roundabout)
					measure::counters.clearanceWon.fetch_add(1, std::memory_order_relaxed);
#endif
				polys.assign(clear.begin(), clear.end());
				polyCount = static_cast<int>(polys.size());
			}
			if (outOfNodes && !roundabout)
				searched |= DT_OUT_OF_NODES;
#if defined(KSIM_PROFILE) && KSIM_PROFILE
			const std::uint64_t clearTook = measure::now() - clearBegan;
			measure::counters.clearanceNs.fetch_add(clearTook, std::memory_order_relaxed);
			if (roundabout)
				measure::counters.roundaboutNs.fetch_add(clearTook, std::memory_order_relaxed);
#endif
		}
	} else {
		searched = query_->findPath(startRef, endRef, start.data(), end.data(), &filter, polys.data(), &polyCount,
			kMaxPathPolys);
	}
	if (dtStatusFailed(searched) || polyCount == 0)
	{
		result.failure = PathResult::Failure::NoPath;
		return result;
	}
	result.found = true;
	for (int i = 0; i < polyCount; ++i)
	{
		const auto door = mesh_.doorOfRef_.find(polys[static_cast<std::size_t>(i)]);
		if (door != mesh_.doorOfRef_.end() && (result.doors.empty() || result.doors.back() != door->second))
			result.doors.push_back(door->second);
	}
	result.complete = polys[static_cast<std::size_t>(polyCount - 1)] == endRef;
	if (!result.complete)
		result.failure = (searched & DT_OUT_OF_NODES) != 0 ? PathResult::Failure::OutOfNodes
			: PathResult::Failure::Closed;
	if (!result.complete)
		query_->closestPointOnPoly(polys[static_cast<std::size_t>(polyCount - 1)], end.data(), end.data(), nullptr);

	std::vector<float>& straight = sc.straight;
	std::vector<dtPolyRef>& cornerRefs = sc.cornerRefs;
	straight.resize(3ull * kMaxPathPolys);
	cornerRefs.resize(kMaxPathPolys);
	int straightCount = 0;
	if (dtStatusFailed(query_->findStraightPath(start.data(), end.data(), polys.data(), polyCount,
			straight.data(), nullptr, cornerRefs.data(), &straightCount, kMaxPathPolys)))
	{
		result.found = false;
		return result;
	}
	for (int i = 0; i < straightCount; ++i)
		result.points.push_back({ straight[3ull * i], straight[3ull * i + 1], straight[3ull * i + 2] });
	// Shrink first, pull second: the corners above are the radius-0 pull, kept for the cost; a body with a radius takes the funnel
	// over the shrunken portals (nothing pushes or reverts a corner afterwards).
	if (travel.radius > 0.0f)
	{
		std::vector<Point> shrunk = shrunkFunnel(*query_, polys, polyCount, start, end, travel.radius + kCornerPush);
		if (shrunk.size() >= 2)
			result.points = std::move(shrunk);
		// A start in a wall's band can leave the shrunken portals' funnel a jog back past itself (Storm House pocket, -50584, -1214: 3 u
		// out and back, k16.7.103). A corner is no corner when the points on either side of it walk to each other for the whole body plus
		// its push: from each kept point, the farthest later point it walks to takes the string tight (Heft Longhouse, k16.6.460: the
		// corners 1 and 2 of a 10-point route lay inside a straight walk from point 0 to point 3, which no neighbour test sees).
		for (std::size_t i = 0; i + 2 < result.points.size(); ++i)
			for (std::size_t j = result.points.size() - 1; j >= i + 2; --j)
				if (walkLeg(result.points[i], result.points[j], travel.radius + kCornerPush, travel))
				{
					// A loop back onto its own start (the same point twice): the repeat goes with it, no zero-length leg is left.
					const std::size_t last = result.points[j] == result.points[i] ? j + 1 : j;
					result.points.erase(result.points.begin() + static_cast<std::ptrdiff_t>(i + 1), result.points.begin() + static_cast<std::ptrdiff_t>(last));
					break;
				}
		for (std::size_t i = 1; i + 1 < result.points.size();)
			if (walkLeg(result.points[i - 1], result.points[i + 1], travel.radius + kCornerPush, travel))
				result.points.erase(result.points.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
	}
	// A corner lies on a portal, and the portal's ends are interpolated along the polygon's edge: a long edge whose neighbour across the
	// tile border has more vertices on it (a T-junction on steep terrain: a 256 u edge facing 3 shorter ones) is straight where the surface
	// is not, so the corner can be 16 u off the floor (R15 leg 2, kenshi-k16.8.37). The corner stands on the corridor's own surface.
	constexpr float kCornerSnapMax = 24.0f;
	for (std::size_t i = 1; i + 1 < result.points.size(); ++i)
	{
		float best = 0.0f;
		bool found = false;
		for (int k = 0; k < polyCount; ++k)
		{
			float h = 0.0f;
			if (dtStatusFailed(query_->getPolyHeight(polys[static_cast<std::size_t>(k)], result.points[i].data(), &h)))
				continue;
			if (!found || std::fabs(h - result.points[i][1]) < std::fabs(best - result.points[i][1]))
				best = h;
			found = true;
		}
		// Only a small fix: a corner's own portal is off by an edge's interpolation, never by a floor (stacked floors share xz).
		if (found && std::fabs(best - result.points[i][1]) <= kCornerSnapMax)
			result.points[i][1] = best;
	}
	for (std::size_t i = 1; i < result.points.size(); ++i)
		result.length += distance(result.points[i - 1], result.points[i]);

	// The cost: a second straight path with a point on every edge crossing says which polygon each
	// stretch lies in.
	std::vector<dtPolyRef>& refs = sc.refs;
	refs.resize(kMaxPathPolys);
	int crossings = 0;
	if (dtStatusFailed(query_->findStraightPath(start.data(), end.data(), polys.data(), polyCount, straight.data(),
			nullptr, refs.data(), &crossings, kMaxPathPolys, DT_STRAIGHTPATH_ALL_CROSSINGS)))
	{
		result.cost = result.length;
		return result;
	}
	for (int i = 0; i + 1 < crossings; ++i)
	{
		const dtMeshTile* tile = nullptr;
		const dtPoly* poly = nullptr;
		mesh_.detour().getTileAndPolyByRefUnsafe(refs[static_cast<std::size_t>(i)], &tile, &poly);
		const float* a = &straight[3ull * i];
		result.cost += distance({ a[0], a[1], a[2] }, { a[3], a[4], a[5] }) * travel.cost[poly->getArea()];
	}
	return result;
}

namespace {

std::size_t indexOfPoint(const std::vector<Point>& points, const Point& wanted)
{
	return static_cast<std::size_t>(std::find(points.begin(), points.end(), wanted) - points.begin());
}

} // namespace

// A door's middle (the mean of its polygons' vertices, which is the state::Door x, z) and its axis through the wall: the short principal
// axis of those vertices (a slab wide along the wall, thin through it), as a unit vector.
bool NavQuery::doorFrame(DoorKey door, Point& centre, double& axisX, double& axisZ) const
{
	const auto refs = mesh_.doorRefs_.find(door);
	if (refs == mesh_.doorRefs_.end())
		return false;
	std::vector<Point> verts;
	for (const std::uint32_t ref : refs->second)
	{
		const dtMeshTile* tile = nullptr;
		const dtPoly* poly = nullptr;
		mesh_.detour().getTileAndPolyByRefUnsafe(ref, &tile, &poly);
		for (int v = 0; v < poly->vertCount; ++v)
			verts.push_back({ tile->verts[poly->verts[v] * 3], tile->verts[poly->verts[v] * 3 + 1], tile->verts[poly->verts[v] * 3 + 2] });
	}
	if (verts.empty())
		return false;
	centre = { 0.0f, 0.0f, 0.0f };
	for (const Point& vertex : verts)
		for (std::size_t axis = 0; axis < 3; ++axis)
			centre[axis] += vertex[axis] / static_cast<float>(verts.size());
	double sxx = 0.0;
	double szz = 0.0;
	double sxz = 0.0;
	for (const Point& vertex : verts)
	{
		const double dx = vertex[0] - centre[0];
		const double dz = vertex[2] - centre[2];
		sxx += dx * dx;
		szz += dz * dz;
		sxz += dx * dz;
	}
	const double angle = 0.5 * std::atan2(2.0 * sxz, sxx - szz); // the long axis
	axisX = -std::sin(angle);
	axisZ = std::cos(angle);
	return true;
}

// A door is passed through its middle (kenshi-k16.6.222). The pulled-tight path takes the straight line through the door's polygons, which can
// graze a jamb (the Hub bar's door, -50616, -1156.5: a leg 1 u off the east frame; the body hit it, backed off and tried again three times), and
// the corridor over a whole route can wander (a Z outside the door). So a route that passes the middle by more than kDoorMiddleSlack gets it as a
// corner, with a run-in point kDoorRunIn each side along the door's axis (when the neighbouring corner is farther), so the turn before the door does
// not carry the body wide into the frame. A start within a run-in of the door keeps its own spot (the r + 1 wall push walked it back into the room
// first). The corners within kDoorClutter of the middle are dropped when the walk between the new neighbours is open, and each side's run in the open
// is planned again on its own (the plain search, no door in it); the shorter whole is kept.
void NavQuery::throughDoorMiddle(PathResult& result, const Point& from, const Point& to, const Point& halfExtents,
	const TravelFilter& travel, const Point& snappedStart) const
{
	Point centre;
	double axisX = 0.0;
	double axisZ = 0.0;
	if (!doorFrame(result.doors.front(), centre, axisX, axisZ))
		return;
	std::vector<Point>& points = result.points;
	// Only a route that crosses the door's middle plane, or comes from afar (kDoorLocalRange) to end within kDoorBesideRange of its middle
	// (a goal in the doorway), passes through the door: one whose ends are on one side of the plane and neither (a start already past
	// the plane, standing in the frame or beside it; a sideways walk along the wall inside), takes its own pulled path, with no run-in point on the far side of the door (user 2026-11-17: a 11 u move beside the door wall went 8 u back
	// out of the door and returned, 45 u for 11; kenshi-k16.6.352).
	const auto sumLength = [](const std::vector<Point>& path)
	{
		float length = 0.0f;
		for (std::size_t i = 1; i < path.size(); ++i)
			length += distance(path[i - 1], path[i]);
		return length;
	};
	if (distance(snappedStart, centre) <= kDoorRunIn && distance(points[0], snappedStart) > 0.5f)
		points[0] = snappedStart;
	std::size_t nearest = 0;
	float gap = 1.0e9f;
	for (std::size_t i = 0; i + 1 < points.size(); ++i)
	{
		const float dx = points[i + 1][0] - points[i][0];
		const float dz = points[i + 1][2] - points[i][2];
		const float lengthSquared = dx * dx + dz * dz;
		const float t = lengthSquared < 1e-6f ? 0.0f : std::clamp(((centre[0] - points[i][0]) * dx + (centre[2] - points[i][2]) * dz) / lengthSquared, 0.0f, 1.0f);
		const float away = std::hypot(centre[0] - (points[i][0] + dx * t), centre[2] - (points[i][2] + dz * t));
		if (away < gap)
		{
			gap = away;
			nearest = i;
		}
	}
	const auto side = [&](const Point& at) { return axisX * (at[0] - centre[0]) + axisZ * (at[2] - centre[2]); };
	if (side(points.front()) * side(points.back()) > 0.0)
	{
		const float nearEnd = std::min(distance(points.front(), centre), distance(points.back(), centre));
		const float farEnd = std::max(distance(points.front(), centre), distance(points.back(), centre));
		if (nearEnd >= kDoorBesideRange || farEnd <= kDoorLocalRange)
			return;
	}
	const Point before = points[nearest];
	const Point after = points[nearest + 1];
	if (axisX * (after[0] - before[0]) + axisZ * (after[2] - before[2]) < 0.0)
	{
		axisX = -axisX;
		axisZ = -axisZ;
	}
	dtQueryFilter filter;
	configureFilter(filter, travel);
	const auto runIn = [&](double side) -> std::optional<Point>
	{
		const Point target{ static_cast<float>(centre[0] + axisX * side * kDoorRunIn), centre[1], static_cast<float>(centre[2] + axisZ * side * kDoorRunIn) };
		const float extents[3] = { 2.0f, 6.0f, 2.0f };
		dtPolyRef ref = 0;
		Point on;
		if (dtStatusFailed(query_->findNearestPoly(target.data(), extents, &filter, &ref, on.data())) || ref == 0
			|| std::hypot(on[0] - target[0], on[2] - target[2]) > 1.5f)
			return std::nullopt;
		return on;
	};
	const std::optional<Point> pre = distance(before, centre) > kDoorRunIn + 2.0f ? runIn(-1.0) : std::nullopt;
	const std::optional<Point> post = distance(after, centre) > kDoorRunIn + 2.0f ? runIn(1.0) : std::nullopt;
	std::vector<Point> added;
	if (pre)
		added.push_back(*pre);
	if (gap > kDoorMiddleSlack)
		added.push_back(centre);
	if (post)
		added.push_back(*post);
	if (!added.empty())
	{
		std::vector<Point> rebuilt;
		for (std::size_t i = 0; i < points.size(); ++i)
		{
			if (i == 0 || i + 1 == points.size() || distance(points[i], centre) >= kDoorClutter)
				rebuilt.push_back(points[i]);
			if (i == nearest)
				rebuilt.insert(rebuilt.end(), added.begin(), added.end());
		}
		std::vector<Point> split = rebuilt;
		if (pre)
		{
			const PathResult in = planPath(from, *pre, halfExtents, travel, nullptr);
			const std::size_t at = indexOfPoint(split, *pre);
			if (in.found && in.complete && !in.points.empty() && at < split.size())
			{
				std::vector<Point> next = in.points;
				next.insert(next.end(), split.begin() + static_cast<std::ptrdiff_t>(at) + 1, split.end());
				split = std::move(next);
			}
		}
		if (post)
		{
			const PathResult out = planPath(*post, to, halfExtents, travel, nullptr);
			const std::size_t at = indexOfPoint(split, *post);
			if (out.found && out.complete && !out.points.empty() && at < split.size())
			{
				split.resize(at);
				split.insert(split.end(), out.points.begin(), out.points.end());
			}
		}
		const auto open = [&](const Point& a, const Point& b)
		{
			const float extents[3] = { 2.0f, 6.0f, 2.0f };
			dtPolyRef ref = 0;
			Point on;
			float t = 0.0f;
			float normal[3];
			std::array<dtPolyRef, 32> visited;
			int count = 0;
			return !dtStatusFailed(query_->findNearestPoly(a.data(), extents, &filter, &ref, on.data())) && ref != 0
				&& !dtStatusFailed(query_->raycast(ref, on.data(), b.data(), &filter, &t, normal, visited.data(), &count, static_cast<int>(visited.size())))
				&& t > 1.0f;
		};
		bool rebuiltOpen = true;
		for (std::size_t i = 0; i + 1 < rebuilt.size() && rebuiltOpen; ++i)
			rebuiltOpen = open(rebuilt[i], rebuilt[i + 1]);
		if (sumLength(split) + 2.0f < sumLength(rebuilt))
			points = std::move(split);
		else if (rebuiltOpen)
			points = std::move(rebuilt);
		else
			points.insert(points.begin() + static_cast<std::ptrdiff_t>(nearest) + 1, added.begin(), added.end());
	}
	result.length = sumLength(points);
}

} // namespace knav
