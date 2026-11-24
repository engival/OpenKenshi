// kenshi-k16.6.627: the negative route cache. A goal behind a wall answers NoRoute once and from the cache after; a tile patch that opens
// the wall (mesh revision change) drops the cache and the route is found. Mesh built in the test, no game data.

#include "ksim/nav/navigator.hpp"

#include "knav/navmesh.hpp"
#include "knav/travel.hpp"

#include "test_util.hpp"

#include <DetourNavMeshBuilder.h>

#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace {

using namespace ksim;

// A tile of the grid xs x zs (world units, even) with the cells in `omit` (column, row) left out; all Land, y = 0
// (the helper of ksim_chase_stuck).
knav::BakedTile gridTile(const std::vector<int>& xs, const std::vector<int>& zs, const std::set<std::pair<int, int>>& omit)
{
	const int nx = static_cast<int>(xs.size());
	const int nz = static_cast<int>(zs.size());
	std::vector<unsigned short> verts;
	for (int r = 0; r < nz; ++r)
		for (int c = 0; c < nx; ++c)
		{
			verts.push_back(static_cast<unsigned short>(xs[c] / 2));
			verts.push_back(0);
			verts.push_back(static_cast<unsigned short>(zs[r] / 2));
		}
	std::map<std::pair<int, int>, int> index;
	for (int r = 0; r + 1 < nz; ++r)
		for (int c = 0; c + 1 < nx; ++c)
			if (omit.count({ c, r }) == 0)
				index.emplace(std::make_pair(c, r), static_cast<int>(index.size()));
	const auto neighbour = [&](int c, int r) -> unsigned short
	{
		const auto it = index.find({ c, r });
		return it == index.end() ? static_cast<unsigned short>(0xffff) : static_cast<unsigned short>(it->second);
	};
	std::vector<unsigned short> polys;
	std::vector<unsigned char> areas;
	std::vector<unsigned short> flags;
	for (int r = 0; r + 1 < nz; ++r)
		for (int c = 0; c + 1 < nx; ++c)
		{
			if (omit.count({ c, r }) != 0)
				continue;
			const unsigned short corners[4] = { static_cast<unsigned short>(r * nx + c), static_cast<unsigned short>((r + 1) * nx + c),
				static_cast<unsigned short>((r + 1) * nx + c + 1), static_cast<unsigned short>(r * nx + c + 1) };
			for (const unsigned short v : corners)
				polys.push_back(v);
			polys.push_back(neighbour(c - 1, r));
			polys.push_back(neighbour(c, r + 1));
			polys.push_back(neighbour(c + 1, r));
			polys.push_back(neighbour(c, r - 1));
			areas.push_back(static_cast<unsigned char>(knav::Area::Land));
			flags.push_back(knav::areaFlag(knav::Area::Land));
		}
	dtNavMeshCreateParams params{};
	params.verts = verts.data();
	params.vertCount = nx * nz;
	params.polys = polys.data();
	params.polyAreas = areas.data();
	params.polyFlags = flags.data();
	params.polyCount = static_cast<int>(areas.size());
	params.nvp = 4;
	params.walkableHeight = 18.0f;
	params.walkableRadius = 4.0f;
	params.walkableClimb = 5.0f;
	params.bmax[0] = 256.0f;
	params.bmax[1] = 20.0f;
	params.bmax[2] = 256.0f;
	params.cs = 2.0f;
	params.ch = 1.0f;
	params.buildBvTree = true;
	unsigned char* data = nullptr;
	int size = 0;
	test::require(dtCreateNavMeshData(&params, &data, &size), "dtCreateNavMeshData failed");
	knav::BakedTile tile;
	tile.x = 0;
	tile.z = 0;
	tile.data.assign(data, data + size);
	dtFree(data);
	return tile;
}

}

int main()
{
	try
	{
		std::vector<int> xs;
		for (int v = 0; v <= 88; v += 4)
			xs.push_back(v);
		std::set<std::pair<int, int>> wall;
		for (int c = 0; c < 22; ++c)
			wall.insert({ c, 10 });
		knav::NavConfig config;
		config.tilesX = 1;
		config.tilesZ = 1;
		config.window = { 0, 0, 0, 0 };
		std::vector<knav::BakedTile> tiles;
		tiles.push_back(gridTile(xs, xs, wall));
		const MeshNavigator nav(knav::NavMesh::fromTiles(config, std::move(tiles)));

		const auto hits = [&]() { return nav.stats().routeNegativeHits; };
		const auto ask = [&](double fx, double radius) { return nav.routeFrom({ fx, 0.0, 10.0 }, 20.0, 70.0, 0.0, false, Body{ radius, std::nullopt }); };
		// Radius 0: not topological-keyed, so an exact repeat hits and a start 2 u over (same cell) does not.
		test::require(!ask(21.0, 0.0).found && hits() == 0, "a goal behind a full wall has a route, or the first ask was cached");
		test::require(!ask(21.0, 0.0).found && hits() == 1, "an exact repeat of a failed ask was not served from the cache");
		test::require(!ask(23.0, 0.0).found && hits() == 1, "a radius-0 failure was served to a different start in the same cell");
		// With a radius the radius-0 probe fails too: the failure is topological and serves any start and body in those polys.
		test::require(!ask(21.0, 3.0).found && hits() == 1, "the first radius-3 ask was served from the cache");
		test::require(!ask(23.0, 3.0).found && hits() == 2, "a topological failure was not served to another start in the same cell");
		test::require(!ask(22.0, 2.0).found && hits() == 3, "a topological failure was not served to another body in the same cell");

		// Clearance only: a 4 u gap in the wall, a body wider than it. The plain probe finds the way, so the failure is not topological and
		// is not served to a start 2 u over in the same cell.
		{
			std::set<std::pair<int, int>> gapWall;
			for (int c = 0; c < 22; ++c)
				if (c != 10)
					gapWall.insert({ c, 10 });
			std::vector<knav::BakedTile> gapTiles;
			gapTiles.push_back(gridTile(xs, xs, gapWall));
			const MeshNavigator gap(knav::NavMesh::fromTiles(config, std::move(gapTiles)));
			const auto askGap = [&](double fx, double radius) { return gap.routeFrom({ fx, 0.0, 10.0 }, 20.0, 70.0, 0.0, false, Body{ radius, std::nullopt }); };
			test::require(askGap(21.0, 0.0).found, "the gap has no plain route");
			const std::uint64_t before = gap.stats().routeNegativeHits;
			const bool wide = askGap(21.0, 6.0).found;
			if (!wide)
			{
				test::require(!askGap(23.0, 6.0).found && gap.stats().routeNegativeHits == before,
					"a clearance-only failure was served to another start in the same cell");
				test::require(!askGap(21.0, 6.0).found && gap.stats().routeNegativeHits == before + 1, "an exact clearance failure was not cached");
			}
			std::cout << "clearance-only case: wide body " << (wide ? "found a route (case not exercised)" : "failed") << "\n";
			test::require(!wide, "a 6 u body fits a 4 u gap: the clearance-only case is not exercised");
		}

		// The patch: the same tile with the wall's row open.
		nav.replaceTiles({ { 0, 0 } }, { gridTile(xs, xs, {}) });
		const Route open = ask(21.0, 3.0);
		test::require(open.found, "a patched tile that opens the wall still has no route: the cache survived the patch");
		test::require(hits() == 3, "a stale cache entry answered after the patch");
		std::cout << "ksim_route_negative_cache: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
