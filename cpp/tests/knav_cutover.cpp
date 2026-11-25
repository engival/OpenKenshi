// The navmesh cutover (kenshi-k16.7.189): a zone's tiles from knav::ground + knav::surfgen (knav::bakeZone), no Recast, no shipped tile.
// Zone 46,16 (Heft, 12 x tops) and the Hub's zone: every building is generated or reported, the tiles over the polygon cap are listed, the tiles
// exist, the bake is the same twice, and the wall time of a cold bake is printed.

#include "knav/area.hpp"
#include "knav/seeds.hpp"
#include "knav/zone_gen.hpp"
#include "kworld/coords.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <iostream>

namespace
{

std::vector<knav::BakedTile> bake(const std::string& dataDir, const kworld::Heightmap& terrain, kworld::ZoneCell zone, knav::ZoneGenReport& report, double& ms)
{
	const knav::AreaGeometry area = knav::gatherArea(dataDir, terrain, zone, zone);
	const knav::NavConfig config = knav::configForWorldZones(zone, zone);
	const std::vector<std::array<double, 2>> seeds = knav::zoneBakeSeeds(terrain, area, knav::readSeedsDef(dataDir + "/navtiles"), zone, config.waterLevel);
	const auto start = std::chrono::steady_clock::now();
	std::vector<knav::BakedTile> tiles = knav::bakeZone(config, terrain, area, seeds, 4, &report);
	ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	return tiles;
}

void zone(const std::string& dataDir, const kworld::Heightmap& terrain, const char* name, kworld::ZoneCell cell)
{
	knav::ZoneGenReport report;
	double ms = 0;
	const std::vector<knav::BakedTile> tiles = bake(dataDir, terrain, cell, report, ms);
	std::cout << name << " zone " << cell.x << "," << cell.z << ": " << tiles.size() << " tiles, " << report.buildings << " buildings generated, " << report.buildingsFailed
		<< " failed, ground " << report.groundFaces << " faces (" << report.topFaces << " tops), stitched " << report.stitched << ", doors " << report.doorFacesPainted << "/"
		<< report.doorFaces << ", weld " << report.welded << " merged / " << report.weldSplit << " split / " << report.weldPaired << " paired / " << report.weldDropped << " dropped, open links " << report.openLinks << ", open shared " << report.openShared << ", max polygons per tile " << report.maxPolys << ", over the cap " << report.overfullTiles << "; surfaces " << static_cast<long>(report.msSurf)
		<< " ms, ground " << static_cast<long>(report.msGround) << " ms, tiles " << static_cast<long>(report.msTiles) << " ms, total " << static_cast<long>(ms) << " ms\n";
	std::vector<std::array<int, 3>> polys = report.tilePolys;
	std::sort(polys.begin(), polys.end(), [](const auto& a, const auto& b) { return a[2] > b[2]; });
	std::cout << "  polygons per tile, most first:";
	for (std::size_t i = 0; i < polys.size() && i < 12; ++i)
		std::cout << " (" << polys[i][0] << "," << polys[i][1] << ")=" << polys[i][2];
	std::size_t over = 0;
	for (const auto& tile : polys)
		over += tile[2] > knav::kMaxPolysPerTile;
	std::cout << "; " << over << " of " << polys.size() << " tiles over " << knav::kMaxPolysPerTile << "\n";
	std::vector<std::pair<double, std::string>> kept;
	for (const auto& [label, area] : report.topKept)
		kept.push_back({ area, label });
	std::sort(kept.rbegin(), kept.rend());
	std::cout << "  top area by source, largest:";
	for (std::size_t i = 0; i < kept.size() && i < 8; ++i)
		std::cout << " [" << kept[i].second << " " << static_cast<long>(kept[i].first) << "]";
	std::cout << "\n";
	for (const std::string& id : report.failedIds)
		std::cout << "  building not generated: " << id << "\n";
	test::require(!tiles.empty(), std::string(name) + ": the zone has tiles");
	// The 512-polygon cap is REPORTED, not asserted (kenshi-k16.7.189, ksim 2026-11-25 F6: tops are 92 % of Heft's faces): tiles over it are left out of the mesh.
	if (report.overfullTiles != 0)
		std::cout << "  OVER THE CAP: " << report.overfullTiles << " tiles left out of the mesh\n";
	knav::ZoneGenReport again;
	double ms2 = 0;
	const std::vector<knav::BakedTile> second = bake(dataDir, terrain, cell, again, ms2);
	test::require(second.size() == tiles.size(), std::string(name) + ": the bake is the same twice (tile count)");
	for (std::size_t i = 0; i < tiles.size() && i < second.size(); ++i)
		test::require(tiles[i].x == second[i].x && tiles[i].z == second[i].z && tiles[i].data == second[i].data, std::string(name) + ": the bake is the same twice (tile bytes)");
}

} // namespace

int main()
{
	try
	{
		const std::string dataDir = KENSHI_DATA;
		const kworld::Heightmap terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		zone(dataDir, terrain, "Heft", { 46, 16 });
		zone(dataDir, terrain, "Hub", kworld::zoneCellOf(-51181.133, 2638.970));
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "knav_cutover ok\n";
	return 0;
}
