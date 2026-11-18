// A goal with no height prefers the start's connected set, and a walker off the mesh or sealed in a pocket is re-seated on the
// goal's ground (PATHFINDING_STATIC 1.10 / 1.11; kenshi-k16.7.65). The mesh is built here: a 200 x 200 floor, a sealed pocket
// (x 20..40, z 20..40) and a deck 30 u over the floor (x 120..180, z 20..80) that nothing connects to.

#include "knav/navmesh.hpp"
#include "ksim/nav/navigator.hpp"

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "Recast.h"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace {

// Two triangles of a flat quad, y = 0, x/z from (x0, z0) to (x1, z1), wound up.
void addQuad(std::vector<float>& verts, std::vector<int>& tris, double x0, double z0, double x1, double z1, float y = 0.0f)
{
	const int base = static_cast<int>(verts.size() / 3);
	const float corners[4][3] = { { float(x0), y, float(z0) }, { float(x1), y, float(z0) }, { float(x1), y, float(z1) },
		{ float(x0), y, float(z1) } };
	for (const auto& c : corners)
	{
		verts.push_back(c[0]);
		verts.push_back(c[1]);
		verts.push_back(c[2]);
	}
	for (const int i : { 0, 2, 1, 0, 3, 2 })
		tris.push_back(base + i);
}

// A closed box from y 0 to 20: unwalkable, so it blocks the heightfield column.
void addBox(std::vector<float>& verts, std::vector<int>& tris, double x0, double z0, double x1, double z1)
{
	const int base = static_cast<int>(verts.size() / 3);
	const float c[8][3] = { { float(x0), 0.0f, float(z0) }, { float(x1), 0.0f, float(z0) }, { float(x1), 0.0f, float(z1) },
		{ float(x0), 0.0f, float(z1) }, { float(x0), 20.0f, float(z0) }, { float(x1), 20.0f, float(z0) }, { float(x1), 20.0f, float(z1) },
		{ float(x0), 20.0f, float(z1) } };
	for (const auto& v : c)
	{
		verts.push_back(v[0]);
		verts.push_back(v[1]);
		verts.push_back(v[2]);
	}
	const int faces[12][3] = { { 0, 1, 2 }, { 0, 2, 3 }, { 4, 6, 5 }, { 4, 7, 6 }, { 0, 4, 5 }, { 0, 5, 1 },
		{ 1, 5, 6 }, { 1, 6, 2 }, { 2, 6, 7 }, { 2, 7, 3 }, { 3, 7, 4 }, { 3, 4, 0 } };
	for (const auto& f : faces)
	{
		tris.push_back(base + f[0]);
		tris.push_back(base + f[1]);
		tris.push_back(base + f[2]);
	}
}

knav::BakedTile bakeTile()
{
	const float bmin[3] = { 0.0f, -2.0f, 0.0f };
	const float bmax[3] = { 256.0f, 62.0f, 256.0f };
	rcConfig cfg{};
	cfg.cs = 1.0f;
	cfg.ch = 1.0f;
	rcCalcGridSize(bmin, bmax, cfg.cs, &cfg.width, &cfg.height);
	cfg.bmin[0] = bmin[0];
	cfg.bmin[1] = bmin[1];
	cfg.bmin[2] = bmin[2];
	cfg.bmax[0] = bmax[0];
	cfg.bmax[1] = bmax[1];
	cfg.bmax[2] = bmax[2];
	cfg.walkableSlopeAngle = 40.0f;
	cfg.walkableHeight = 18;
	cfg.walkableClimb = 5;
	cfg.walkableRadius = 0; // no erosion: the shipped mesh keeps the whole surface
	cfg.maxEdgeLen = 0;
	cfg.maxSimplificationError = 1.3f;
	cfg.minRegionArea = 64;
	cfg.mergeRegionArea = 400;
	cfg.maxVertsPerPoly = 6;
	cfg.detailSampleDist = 6.0f;
	cfg.detailSampleMaxError = 1.0f;
	cfg.borderSize = 0;

	rcContext ctx(false);
	rcHeightfield* hf = rcAllocHeightfield();
	rcCompactHeightfield* chf = rcAllocCompactHeightfield();
	rcContourSet* cset = rcAllocContourSet();
	rcPolyMesh* pmesh = rcAllocPolyMesh();
	rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
	if (hf == nullptr || chf == nullptr || cset == nullptr || pmesh == nullptr || dmesh == nullptr)
		throw test::Failure("Recast allocation failed");
	const auto cleanup = [&]
	{
		rcFreeHeightField(hf);
		rcFreeCompactHeightfield(chf);
		rcFreeContourSet(cset);
		rcFreePolyMesh(pmesh);
		rcFreePolyMeshDetail(dmesh);
	};
	if (!rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
	{
		cleanup();
		throw test::Failure("rcCreateHeightfield");
	}

	std::vector<float> floorVerts;
	std::vector<int> floorTris;
	addQuad(floorVerts, floorTris, 0.0, 0.0, 200.0, 200.0);
	std::vector<unsigned char> areas(floorTris.size() / 3, 1); // Recast area 1: Land (bake.cpp's Area + 1)
	rcRasterizeTriangles(&ctx, floorVerts.data(), static_cast<int>(floorVerts.size() / 3), floorTris.data(), areas.data(),
		static_cast<int>(areas.size()), *hf, cfg.walkableClimb);
	std::vector<float> wallVerts;
	std::vector<int> wallTris;
	// A sealed pocket: the floor x 20..40, z 20..40 inside four walls.
	addBox(wallVerts, wallTris, 15.0, 15.0, 45.0, 20.0);
	addBox(wallVerts, wallTris, 15.0, 40.0, 45.0, 45.0);
	addBox(wallVerts, wallTris, 15.0, 20.0, 20.0, 40.0);
	addBox(wallVerts, wallTris, 40.0, 20.0, 45.0, 40.0);
	areas.assign(wallTris.size() / 3, RC_NULL_AREA);
	rcRasterizeTriangles(&ctx, wallVerts.data(), static_cast<int>(wallVerts.size() / 3), wallTris.data(), areas.data(),
		static_cast<int>(areas.size()), *hf, cfg.walkableClimb);

	// A deck 30 u over the floor, x 120..180, z 20..80, with no way on or off it.
	floorVerts.clear();
	floorTris.clear();
	addQuad(floorVerts, floorTris, 120.0, 20.0, 180.0, 80.0, 30.0f);
	areas.assign(floorTris.size() / 3, 1);
	rcRasterizeTriangles(&ctx, floorVerts.data(), static_cast<int>(floorVerts.size() / 3), floorTris.data(), areas.data(),
		static_cast<int>(areas.size()), *hf, cfg.walkableClimb);

	rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
	rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
	rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);
	bool ok = rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf);
	if (ok)
		ok = rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf);
	if (ok)
		ok = rcBuildDistanceField(&ctx, *chf);
	if (ok)
		ok = rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea);
	if (ok)
		ok = rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset);
	if (ok)
		ok = rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh);
	if (ok)
		ok = rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh);
	if (!ok || pmesh->npolys == 0)
	{
		cleanup();
		throw test::Failure("the gap tile did not build");
	}
	for (int i = 0; i < pmesh->npolys; ++i)
	{
		pmesh->areas[i] = static_cast<unsigned char>(pmesh->areas[i] - 1); // Detour area: Land = 0
		pmesh->flags[i] = knav::areaFlag(knav::Area::Land);
	}
	dtNavMeshCreateParams params{};
	params.verts = pmesh->verts;
	params.vertCount = pmesh->nverts;
	params.polys = pmesh->polys;
	params.polyAreas = pmesh->areas;
	params.polyFlags = pmesh->flags;
	params.polyCount = pmesh->npolys;
	params.nvp = pmesh->nvp;
	params.detailMeshes = dmesh->meshes;
	params.detailVerts = dmesh->verts;
	params.detailVertsCount = dmesh->nverts;
	params.detailTris = dmesh->tris;
	params.detailTriCount = dmesh->ntris;
	params.tileX = 0;
	params.tileY = 0;
	std::copy(pmesh->bmin, pmesh->bmin + 3, params.bmin);
	std::copy(pmesh->bmax, pmesh->bmax + 3, params.bmax);
	params.walkableHeight = static_cast<float>(cfg.walkableHeight);
	params.walkableRadius = static_cast<float>(cfg.walkableRadius);
	params.walkableClimb = static_cast<float>(cfg.walkableClimb);
	params.cs = cfg.cs;
	params.ch = cfg.ch;
	params.buildBvTree = true;
	unsigned char* data = nullptr;
	int size = 0;
	knav::BakedTile tile;
	if (!dtCreateNavMeshData(&params, &data, &size))
	{
		cleanup();
		throw test::Failure("dtCreateNavMeshData");
	}
	tile.data.assign(data, data + size);
	dtFree(data);
	cleanup();
	return tile;
}

std::shared_ptr<ksim::MeshNavigator> makeNavigator()
{
	knav::NavConfig config;
	config.originX = 0.0;
	config.originZ = 0.0;
	config.tileSize = 256.0f;
	config.tilesX = 1;
	config.tilesZ = 1;
	config.window = { 0, 0, 0, 0 };
	config.cellSize = 1.0f;
	config.cellHeight = 1.0f;
	return std::make_shared<ksim::MeshNavigator>(knav::NavMesh::fromTiles(config, { bakeTile() }));
}

} // namespace

int main()
{
	try
	{
		const auto nav = makeNavigator();
		// A goal over the deck, no height, from the deck: the deck (y 30), not the floor under it, which no route joins.
		const ksim::Route onDeck = nav->routeFrom({ 125.0, 30.0, 50.0 }, 160.0, 50.0);
		test::require(onDeck.found && !onDeck.reseated, "a goal over the deck from the deck is not a plain route");
		test::require(!onDeck.heights.empty() && onDeck.heights.back() > 20.0, "the goal over the deck snapped to the floor under it");
		// From the floor the same goal is the lowest floor, as before (a tie, the walker's set holds the floor).
		const ksim::Route fromFloor = nav->routeFrom({ 100.0, 0.5, 50.0 }, 160.0, 50.0);
		test::require(fromFloor.found && fromFloor.heights.back() < 10.0, "a goal over the deck from the floor left the floor");

		// Sealed in the pocket: re-seated outside it and walks on to the goal.
		const ksim::Route pocket = nav->routeFrom({ 30.0, 0.5, 30.0 }, 150.0, 150.0, 0.5);
		test::require(pocket.found && pocket.reseated, "a walker sealed in a pocket was not re-seated");
		const ksim::state::GroundPoint seat = pocket.points.front();
		test::require(seat.x < 16.0 || seat.x > 44.0 || seat.z < 16.0 || seat.z > 44.0, "the pocket seat is still inside the pocket");
		test::require(std::hypot(pocket.points.back().x - 150.0, pocket.points.back().z - 150.0) < 1.0, "the re-seated route misses the goal");

		// On the island deck, goal on the floor.
		const ksim::Route island = nav->routeFrom({ 150.0, 30.0, 50.0 }, 100.0, 150.0, 0.5);
		test::require(island.found && island.reseated && island.heights.front() < 10.0, "a walker on the deck island was not re-seated on the floor");

		// Off the mesh altogether (inside the 1000 u search, outside the 340 u click reach of nothing: the start has no polygon).
		const ksim::Route off = nav->routeFrom({ 30.0, 0.5, -600.0 }, 150.0, 150.0, 0.5);
		test::require(off.found && off.reseated, "a walker off the mesh was not re-seated");

		// A start on the goal's own ground is no re-seat.
		const ksim::Route plain = nav->routeFrom({ 100.0, 0.5, 100.0 }, 150.0, 150.0, 0.5);
		test::require(plain.found && !plain.reseated, "a plain route was re-seated");
		std::cout << "ksim_snap_component: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_snap_component: FAIL: " << e.what() << "\n";
		return 1;
	}
}
