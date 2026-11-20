#include "world/clientnav.hpp"

#include <cmath>
#include <cstdio>


namespace
{
	// Detour searches round a point: wide in xz for a click's slop, tall for decks and stairs
	constexpr float kReachExtents[3] = { 8, 40, 8 };
	// a hovered point (walkable: the original's vertical line, +-5000, findings/cursors.md "Wall hover"): this xz half width only lets
	// Detour's box query find the polygons, the point must lie over one
	constexpr float kOverXZ = 0.5f;
}

void ClientNav::update(const simlink::SimLinkDecoder& dec, std::deque<simlink::NavPatch> patches)
{
	if (dec.navGeneration != generation_)
	{
		generation_ = dec.navGeneration;
		if (dec.nav.valid) { rebuild(dec); }
	}
	for (const simlink::NavPatch& p : patches) { patch(p); }
}

// NAV_POLY_FLAGS (door state). A patch at or below the blob's version is already in it (a NAV_TILE overtook it); otherwise it must be the
// next version of what mesh_ holds.
void ClientNav::patch(const simlink::NavPatch& p)
{
	const auto it = held_.find(p.key);
	if (!mesh_ || it == held_.end() || p.version <= it->second.baked || p.version != it->second.version + 1) { return; }
	for (const simlink::NavPoly& q : p.polys)
	{
		if (!mesh_->setPolyFlags(p.key[0], p.key[1], p.key[2], q.poly, q.flags, q.area)) { return; }	// no such tile / polygon: the next NAV_TILE resyncs
	}
	it->second.version = p.version;
}

void ClientNav::rebuild(const simlink::SimLinkDecoder& dec)
{
	const bool fresh = !mesh_ || dec.nav.meshId != meshId_;
	if (fresh) { routeSets_ = knav::RouteSets{}; }	// the preview's connected sets belong to one mesh
	std::vector<knav::TileCoord> removed;
	std::vector<knav::BakedTile> added;
	if (!fresh)
	{
		for (auto it = held_.begin(); it != held_.end();)
		{
			const auto now = dec.navTiles.find(it->first);
			if (now == dec.navTiles.end() || now->second.baked != it->second.baked)
			{
				removed.push_back({ it->first[0], it->first[1] });
				it = held_.erase(it);
			}
			else { ++it; }
		}
	}
	else { held_.clear(); }
	for (const auto& [key, tile] : dec.navTiles)
	{
		if (key[2] != 0 || held_.count(key)) { continue; }	// the server bakes one layer per tile (knav)
		added.push_back({ key[0], key[1], tile.data, {} });
		held_[key] = { tile.baked, tile.baked };
	}
	if (removed.empty() && added.empty() && !fresh) { return; }
	try
	{
		if (fresh)
		{
			knav::NavMesh::DetourParams dp;
			for (int i = 0; i < 3; i++) { dp.origin[i] = dec.nav.origin[i]; }
			dp.tileWidth = dec.nav.tileWidth;
			dp.tileHeight = dec.nav.tileHeight;
			dp.maxTiles = dec.nav.maxTiles;
			dp.maxPolys = dec.nav.maxPolys;
			dp.version = dec.nav.detourVersion;
			walker_.reset();
			query_.reset();
			mesh_ = knav::NavMesh::fromParams(dp, std::move(added));
			meshId_ = dec.nav.meshId;
			origin_[0] = dec.nav.origin[0];
			origin_[1] = dec.nav.origin[2];
			tileSize_ = dec.nav.tileWidth;
		}
		else
		{
			walker_.reset();
			query_.reset();
			mesh_->replaceTiles(removed, std::move(added));
		}
		query_ = std::make_unique<knav::NavQuery>(*mesh_);
		walker_ = std::make_unique<knav::Walker>(*mesh_);
	}
	catch (const std::exception& e)
	{
		fprintf(stderr, "kclient: nav: %s; dropping the client navmesh\n", e.what());
		walker_.reset();
		query_.reset();
		mesh_.reset();
		held_.clear();
	}
}

ClientNav::Reach ClientNav::over(const double at[3], float halfY) const
{
	if (!query_ || tileSize_ <= 0) { return Reach::kUnknown; }
	const std::array<int32_t, 3> key = { (int32_t)std::floor((at[0] - origin_[0]) / tileSize_), (int32_t)std::floor((at[2] - origin_[1]) / tileSize_), 0 };
	if (!held_.count(key)) { return Reach::kUnknown; }
	const knav::Point p = { (float)at[0], (float)at[1], (float)at[2] };
	const knav::Point ext = { kOverXZ, halfY, kOverXZ };
	knav::Point near;
	return query_->nearestPoint(p, ext, near) && std::hypot(near[0] - p[0], near[2] - p[2]) < 0.01f ? Reach::kYes : Reach::kNo;
}

// kenshi-k16.2.412.3: the drawn click hit -> the navmesh point a body stands on there (the click's reverse through the ground solver). The window is the
// client's (ClientNav::kSnapXZ / kSnapY): the server's own goal snap is a box of knav::kClickReach = 340 u in 3D (knav/route.cpp:59,65), and a point that is
// already on the mesh snaps to itself there, so this only has to land the hit exactly on the surface it hit.
ClientNav::Reach ClientNav::snap(const double at[3], double out[3], float halfXZ, float halfY) const
{
	if (!query_ || tileSize_ <= 0) { return Reach::kUnknown; }
	const std::array<int32_t, 3> key = { (int32_t)std::floor((at[0] - origin_[0]) / tileSize_), (int32_t)std::floor((at[2] - origin_[1]) / tileSize_), 0 };
	if (!held_.count(key)) { return Reach::kUnknown; }
	const knav::Point p = { (float)at[0], (float)at[1], (float)at[2] };
	const knav::Point ext = { halfXZ, halfY, halfXZ };
	knav::Point near;
	if (!query_->nearestPoint(p, ext, near)) { return Reach::kNo; }
	for (int k = 0; k < 3; k++) { out[k] = near[k]; }
	return Reach::kYes;
}

ClientNav::Reach ClientNav::walkable(const double at[3]) const { return over(at, 5000); }

ClientNav::Reach ClientNav::surface(const double at[3]) const { return over(at, 2); }

ClientNav::Reach ClientNav::reach(const double from[3], const double to[3]) const
{
	if (!query_) { return Reach::kUnknown; }
	const knav::Point a = { (float)from[0], (float)from[1], (float)from[2] }, b = { (float)to[0], (float)to[1], (float)to[2] };
	const knav::Point ext = { kReachExtents[0], kReachExtents[1], kReachExtents[2] };
	if (!query_->onMesh(a, ext)) { return Reach::kUnknown; }
	const knav::PathResult r = query_->findPath(a, b, ext);
	using F = knav::PathResult::Failure;
	if (r.complete) { return Reach::kYes; }
	return r.failure == F::Closed || r.failure == F::NoPath || r.failure == F::EndOffMesh ? Reach::kNo : Reach::kUnknown;
}
