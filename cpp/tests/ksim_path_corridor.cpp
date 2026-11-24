// PATH_WALK.md 3a (kenshi-e9s9.4): the corridor derived from a final point list on a hand-made mesh (a 200 x 200 floor, a wall across it at z 95..105
// from x 0 to 140, so the only way north is the gap at x 140..200 and the polygons round the wall end are the corridor). Dog-leg, a leg the ray cannot
// walk (through the wall), joins, point portals, a hairpin, the crossing sign (positive on the leg's far side), portalNext against routeNext.

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
struct Wall
{
	double x0, z0, x1, z1;
};

knav::BakedTile bakeTile(const std::vector<Wall>& walls)
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
	for (const Wall& wall : walls)
		addBox(wallVerts, wallTris, wall.x0, wall.z0, wall.x1, wall.z1);
	if (!walls.empty())
	{
		areas.assign(wallTris.size() / 3, RC_NULL_AREA);
		rcRasterizeTriangles(&ctx, wallVerts.data(), static_cast<int>(wallVerts.size() / 3), wallTris.data(), areas.data(),
			static_cast<int>(areas.size()), *hf, cfg.walkableClimb);
	}

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
		throw test::Failure("the tile did not build");
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

std::shared_ptr<ksim::MeshNavigator> makeNavigator(const std::vector<Wall>& walls)
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
	return std::make_shared<ksim::MeshNavigator>(knav::NavMesh::fromTiles(config, { bakeTile(walls) }));
}

using Points = std::vector<ksim::state::RoutePoint>;
using Portals = std::vector<ksim::state::RoutePortal>;

bool isPoint(const ksim::state::RoutePortal& p) { return p.isPoint(); }

// Every crossing portal of the leg ending at route point `end`: the leg's start is behind it, the end in front (the sign is pinned here).
void requireSigns(const Portals& portals, const Points& pts)
{
	for (const auto& p : portals)
	{
		if (isPoint(p))
			continue;
		test::require(p.point >= 1 && p.point < pts.size(), "a portal names a point outside the route");
		const auto& a = pts[p.point - 1];
		const auto& b = pts[p.point];
		test::require(p.side(a.x, a.z) <= 0.01, "the leg's start is not behind its portal");
		test::require(p.side(b.x, b.z) >= -0.01, "the leg's end is not in front of its portal");
	}
}

void pointPortalNone(const Portals& portals, const char* what)
{
	for (std::size_t i = 0; i + 1 < portals.size(); ++i)
		test::require(!isPoint(portals[i]), what);
}

} // namespace

int main()
{
	try
	{
		using namespace ksim;
		const auto nav = makeNavigator({ { 0.0, 95.0, 140.0, 105.0 } });
		const Body body{ 4.0 };

		// The crossing sign on a bare portal: walking +z, left at smaller x, positive past the line.
		{
			state::RoutePortal p;
			p.lx = -1.0;
			p.rx = 1.0;
			test::require(p.side(0.0, 1.0) > 0.0 && p.side(0.0, -1.0) < 0.0, "the far side of a portal walked towards +z is not positive");
			test::require(std::fabs(p.side(0.0, 1.0) - 1.0) < 1e-9, "side is not a distance");
			p.rx = -0.8; // 0.2 wide: a point portal
			test::require(p.isPoint() && p.side(0.0, 1.0) == 0.0, "a portal under 0.5 u is not a point portal");
		}

		// A dog-leg round the wall's end: up the east side, back west above the wall.
		const Points dog{ { 50.0, 0.5, 50.0 }, { 170.0, 0.5, 70.0 }, { 170.0, 0.5, 130.0 }, { 50.0, 0.5, 150.0 } };
		const Portals dogCorridor = nav->corridorFor(dog, body);
		test::require(!dogCorridor.empty(), "no corridor for the dog-leg");
		requireSigns(dogCorridor, dog);
		test::require(isPoint(dogCorridor.back()) && dogCorridor.back().point == 3, "the corridor does not end in a point portal at the goal");
		test::require(dogCorridor.back().lx == 50.0 && dogCorridor.back().lz == 150.0, "the final point portal is not on the goal");
		std::uint32_t last = 0;
		for (const auto& p : dogCorridor)
		{
			test::require(p.point >= last, "the portals' route points go backwards");
			last = p.point;
			test::require(p.clearLeft >= 0.0 && p.clearRight >= 0.0, "a negative clearance");
		}
		const auto crossings = std::count_if(dogCorridor.begin(), dogCorridor.end(), [](const auto& p) { return !isPoint(p); });
		std::cout << "dog-leg: " << dogCorridor.size() << " portals, " << crossings << " crossings\n";

		// A leg through the wall cannot be walked by a ray: a point portal at its end (today's walk of that leg). The join after it is a point portal too.
		const Points through{ { 50.0, 0.5, 50.0 }, { 50.0, 0.5, 150.0 }, { 150.0, 0.5, 150.0 } };
		const Portals broken = nav->corridorFor(through, body);
		test::require(!broken.empty() && isPoint(broken.front()) && broken.front().point == 1 && broken.front().lx == 50.0 && broken.front().lz == 150.0,
			"a leg through the wall did not end in a point portal");
		test::require(isPoint(broken.back()) && broken.back().point == 2, "the last point has no point portal");
		requireSigns(broken, through);

		// A leg off the searched corridor (the Longhouse shortcut shape): a hand-picked straight line across open floor that no search would
		// choose, still walked by the ray, its crossings behind / in front by the sign.
		const Points shortcut{ { 20.0, 0.5, 20.0 }, { 180.0, 0.5, 80.0 } };
		const Portals cut = nav->corridorFor(shortcut, body);
		test::require(!cut.empty() && isPoint(cut.back()), "the shortcut has no goal portal");
		requireSigns(cut, shortcut);

		// Joins in one polygon: two legs inside the open south-west floor add no point portal between them.
		const Points inside{ { 20.0, 0.5, 20.0 }, { 40.0, 0.5, 25.0 }, { 60.0, 0.5, 40.0 } };
		const Portals joined = nav->corridorFor(inside, body);
		pointPortalNone(joined, "a join inside one polygon laid a point portal");
		test::require(isPoint(joined.back()) && joined.back().point == 2, "no goal portal after the join");

		// A hairpin: out through the gap and straight back. Both legs cross the same edges, opposite ways.
		const Points hairpin{ { 150.0, 0.5, 60.0 }, { 170.0, 0.5, 140.0 }, { 150.0, 0.5, 60.0 } };
		const Portals pin = nav->corridorFor(hairpin, body);
		requireSigns(pin, hairpin);
		std::size_t out = 0;
		std::size_t back = 0;
		for (const auto& p : pin)
			if (!isPoint(p))
				++(p.point == 1 ? out : back);
		test::require(out > 0 && out == back, "the hairpin's two legs do not cross the same polygons");

		// setRoute: the route, the corridor and portalNext in step with routeNext. No navigator: a point portal at every point after the first.
		state::Fighter f;
		setRoute(f, dog, nav.get(), body);
		test::require(f.route.size() == 4 && f.routeNext == 0 && f.portalNext == 0 && f.portals.size() == dogCorridor.size(), "setRoute did not install");
		f.routeNext = 3;
		syncPortalNext(f);
		test::require(f.portalNext < f.portals.size() && f.portals[f.portalNext].point >= 3 && (f.portalNext == 0 || f.portals[f.portalNext - 1].point < 3),
			"portalNext is not the first portal of route point 3");
		f.routeNext = 4;
		syncPortalNext(f);
		test::require(f.portalNext == f.portals.size(), "portalNext is not past the corridor at the route's end");
		setRoute(f, dog, nullptr, body);
		test::require(f.portals.size() == 3 && std::all_of(f.portals.begin(), f.portals.end(), isPoint) && f.portals[0].point == 1, "no navigator: not a point portal per point");
		f.clearRoute();
		test::require(f.portals.empty() && f.portalNext == 0, "clearRoute left a corridor");
		std::cout << "ksim_path_corridor: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_path_corridor: FAIL: " << e.what() << "\n";
		return 1;
	}
}
