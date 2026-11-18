// Query-time clearance (docs/formats/NAVMESH_CACHE.md "Clearance"): a body of radius 4 refuses a gap narrower than the
// original's 9 u (minCharacterWidth, BLOCK_EDGES) that a radius-0 traveller walks through, and takes the same crossing
// when the gap is 12 u wide. The mesh is built here, not baked from the game: two walls across a 200 x 200 floor with a
// gap between them, so the width is exact. The bake is not eroded (the shipped mesh has no erosion either: the body
// width is applied at query time), so radius 0 and radius 4 see the very same polygons.

#include "knav/navmesh.hpp"
#include "ksim/nav/navigator.hpp"

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "Recast.h"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <iostream>
#include <vector>

namespace {

// Two triangles of a flat quad, y = 0, x/z from (x0, z0) to (x1, z1), wound up.
void addQuad(std::vector<float>& verts, std::vector<int>& tris, double x0, double z0, double x1, double z1)
{
	const int base = static_cast<int>(verts.size() / 3);
	const float corners[4][3] = { { float(x0), 0.0f, float(z0) }, { float(x1), 0.0f, float(z0) }, { float(x1), 0.0f, float(z1) },
		{ float(x0), 0.0f, float(z1) } };
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

// The Detour tile of a 256 u square at the grid origin: a floor at y 0 with two walls across it at z 95..105, the walls
// from x 0 to `gapLeft` and from `gapRight` to 200 (the gap between them is the only crossing).
knav::BakedTile bakeGapTile(double gapLeft, double gapRight)
{
	const float bmin[3] = { 0.0f, -2.0f, 0.0f };
	const float bmax[3] = { 256.0f, 22.0f, 256.0f };
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
	addBox(wallVerts, wallTris, 0.0, 95.0, gapLeft, 105.0);
	addBox(wallVerts, wallTris, gapRight, 95.0, 200.0, 105.0);
	areas.assign(wallTris.size() / 3, RC_NULL_AREA);
	rcRasterizeTriangles(&ctx, wallVerts.data(), static_cast<int>(wallVerts.size() / 3), wallTris.data(), areas.data(),
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

std::shared_ptr<ksim::MeshNavigator> gapNavigator(double gapLeft, double gapRight)
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
	return std::make_shared<ksim::MeshNavigator>(knav::NavMesh::fromTiles(config, { bakeGapTile(gapLeft, gapRight) }));
}

// The route across the wall from the south side to the north side at `radius`; the gap is at x 100.
ksim::Route cross(const ksim::MeshNavigator& nav, double radius)
{
	return nav.routeFrom({ 100.0, 0.5, 50.0 }, 100.0, 150.0, 0.5, false, radius);
}

} // namespace

int main()
{
	try
	{
		// A 7 u gap: radius 0 walks through, a body of radius 4 does not fit (no point of the crossing is 4 u from
		// both walls; the widest is 3.5 u). The original blocks gaps under 9 u (minCharacterWidth, BLOCK_EDGES).
		const auto narrow = gapNavigator(96.5, 103.5);
		const ksim::Route through = cross(*narrow, 0.0);
		test::require(through.found && !through.approach && through.points.size() >= 2, "radius 0 did not walk through the 7 u gap");
		test::require(through.points.back().z > 120.0, "the radius 0 route did not cross the wall");
		const ksim::Route refused = cross(*narrow, 4.0);
		test::require(!refused.found, "a radius 4 body fit through a 7 u gap");
		test::require(refused.failure == ksim::state::AiReason::NoRoute, "the refusal is not a NoRoute");
		test::require(through.points.back().z - through.points.front().z >= 100.0,
			"the radius 0 route did not reach the other side of the wall");
		std::cout << "7 u gap: radius 0 crosses (" << through.points.size() << " points, " << through.points.back().z
			<< "), radius 4 refused (NoRoute)\n";

		// The control: a 12 u gap (wider than the original's 9 u) takes the same body.
		const auto wide = gapNavigator(94.0, 106.0);
		const ksim::Route fits = cross(*wide, 4.0);
		test::require(fits.found, "a radius 4 body was refused a 12 u gap");
		test::require(fits.points.back().z > 120.0, "the radius 4 route did not cross the 12 u gap");
		std::cout << "12 u gap: radius 4 crosses (" << fits.points.size() << " points)\n";
		// A goal inside the narrow gap (kenshi-k16.7.62): a body of radius 4 cannot enter the 7 u passage, but a goal on the near
		// (south) side of it is reached by clamping the last hop, not refused (before: every one of these was a closed search). A goal
		// on the far side is across the wall for that body: it fails fast (a few nodes, never the budget).
		for (const double goalZ : { 96.0, 97.5, 99.0 })
		{
			const ksim::Route inGap = narrow->routeFrom({ 100.0, 0.5, 50.0 }, 100.0, goalZ, 0.5, false, 4.0);
			test::require(inGap.found, "a goal in the 7 u gap's near half did not route at radius 4");
			test::require(inGap.points.back().z > 90.0, "the gap-goal route stopped short of the gap");
		}
		const ksim::Route far = narrow->routeFrom({ 100.0, 0.5, 50.0 }, 100.0, 103.0, 0.5, false, 4.0);
		test::require(!far.found, "a goal across the 7 u gap routed for a radius 4 body");
		test::require(narrow->routeStats().routeClearanceNodes < 200, "the gap-goal searches expanded too many nodes");
		std::cout << "goals in the 7 u gap's near half route; the far half fails in " << narrow->routeStats().routeClearanceNodes << " nodes in all\n";
		std::cout << "knav_clearance: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "knav_clearance: FAIL: " << e.what() << "\n";
		return 1;
	}
}
