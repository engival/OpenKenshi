// The navmesh cutover (kenshi-k16.7.189): a zone's tiles from knav::ground + knav::surfgen (knav::bakeZone), no Recast, no shipped tile.
// Zone 46,16 (Heft, 12 x tops) and the Hub's zone: every building is generated or reported, the tiles over the polygon cap are listed, the tiles
// exist, the bake is the same twice, and the wall time of a cold bake is printed.

#include "knav/area.hpp"
#include "knav/navmesh.hpp"
#include "knav/seeds.hpp"
#include "knav/zone_gen.hpp"
#include "kworld/coords.hpp"
#include "kworld/heightmap.hpp"

#include <DetourNavMesh.h>
#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <iostream>

namespace
{

void useNodeCensus(const std::string& dataDir, const kworld::Heightmap& terrain, const char* name, kworld::ZoneCell cell, std::vector<knav::BakedTile> tiles);

// Final-mesh fold check (kenshi-k16.7.207, user 2026-11-25 "the final mesh should never fold in on itself"): two polygons of one tile that lie over each other in
// xz (shared area above `eps`) at heights within the climb are a fold. Counts the pairs and the worst overlap. Polygons are convex (Detour), so the overlap is a
// Sutherland-Hodgman clip.
struct FoldCensus
{
	std::size_t pairs = 0;
	double worst = 0.0;
	std::string where;
};

double clippedArea(const std::vector<std::array<double, 2>>& subject, const std::vector<std::array<double, 2>>& clip, std::array<double, 2>& centre)
{
	std::vector<std::array<double, 2>> out = subject;
	for (std::size_t i = 0; i < clip.size() && !out.empty(); ++i)
	{
		const auto& a = clip[i];
		const auto& b = clip[(i + 1) % clip.size()];
		const auto side = [&](const std::array<double, 2>& p) { return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]); };
		std::vector<std::array<double, 2>> in;
		for (std::size_t j = 0; j < out.size(); ++j)
		{
			const auto& p = out[j];
			const auto& q = out[(j + 1) % out.size()];
			const double sp = side(p), sq = side(q);
			if (sp >= 0)
				in.push_back(p);
			if ((sp >= 0) != (sq >= 0))
			{
				const double t = sp / (sp - sq);
				in.push_back({ p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t });
			}
		}
		out = std::move(in);
	}
	double area = 0.0;
	centre = { 0.0, 0.0 };
	for (const auto& p : out)
		centre = { centre[0] + p[0] / out.size(), centre[1] + p[1] / out.size() };
	for (std::size_t j = 0; j < out.size(); ++j)
		area += out[j][0] * out[(j + 1) % out.size()][1] - out[(j + 1) % out.size()][0] * out[j][1];
	return std::abs(area) * 0.5;
}

FoldCensus foldCensus(const knav::NavMesh& mesh, double eps)
{
	FoldCensus census;
	for (const knav::BakedTile& baked : mesh.tiles())
	{
		const dtMeshTile* tile = mesh.detour().getTileAt(baked.x, baked.z, 0);
		if (!tile)
			continue;
		struct Poly
		{
			std::vector<std::array<double, 2>> xz;
			std::vector<double> ys;
			double y = 0.0, x0 = 1e30, x1 = -1e30, z0 = 1e30, z1 = -1e30;
		};
		std::vector<Poly> polys(static_cast<std::size_t>(tile->header->polyCount));
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			const dtPoly& poly = tile->polys[p];
			Poly& out = polys[static_cast<std::size_t>(p)];
			for (int k = 0; k < poly.vertCount; ++k)
			{
				const float* v = &tile->verts[poly.verts[k] * 3];
				out.xz.push_back({ v[0], v[2] });
				out.ys.push_back(v[1]);
				out.y += v[1] / poly.vertCount;
				out.x0 = std::min<double>(out.x0, v[0]); out.x1 = std::max<double>(out.x1, v[0]);
				out.z0 = std::min<double>(out.z0, v[2]); out.z1 = std::max<double>(out.z1, v[2]);
			}
			double twice = 0.0;
			for (std::size_t j = 0; j < out.xz.size(); ++j)
				twice += out.xz[j][0] * out.xz[(j + 1) % out.xz.size()][1] - out.xz[(j + 1) % out.xz.size()][0] * out.xz[j][1];
			if (twice < 0)
			{
				std::reverse(out.xz.begin(), out.xz.end());
				std::reverse(out.ys.begin(), out.ys.end());
			}
		}
		for (std::size_t a = 0; a < polys.size(); ++a)
			for (std::size_t b = a + 1; b < polys.size(); ++b)
			{
				const Poly& pa = polys[a];
				const Poly& pb = polys[b];
				if (pa.x1 <= pb.x0 || pb.x1 <= pa.x0 || pa.z1 <= pb.z0 || pb.z1 <= pa.z0 || std::abs(pa.y - pb.y) > 40.0)
					continue;
				std::array<double, 2> at{};
				const double area = clippedArea(pa.xz, pb.xz, at);
				if (area <= eps)
					continue;
				// Same layer: the two surfaces within 0.3 u of each other over the overlap's centre (a bridge deck or a roof over the ground is another layer).
				const auto heightAt = [&](const Poly& poly)
				{
					for (std::size_t k = 1; k + 1 < poly.xz.size(); ++k)
					{
						const auto& p0 = poly.xz[0]; const auto& p1 = poly.xz[k]; const auto& p2 = poly.xz[k + 1];
						const double det = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
						if (std::abs(det) < 1e-12)
							continue;
						const double u = ((at[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (at[1] - p0[1])) / det;
						const double v = ((p1[0] - p0[0]) * (at[1] - p0[1]) - (at[0] - p0[0]) * (p1[1] - p0[1])) / det;
						if (u >= -1e-6 && v >= -1e-6 && u + v <= 1 + 1e-6)
							return poly.ys[0] + u * (poly.ys[k] - poly.ys[0]) + v * (poly.ys[k + 1] - poly.ys[0]);
					}
					return poly.y;
				};
				if (std::abs(heightAt(pa) - heightAt(pb)) > 0.3)
					continue;
				++census.pairs;
				if (area > census.worst)
				{
					census.worst = area;
					census.where = "tile " + std::to_string(baked.x) + "," + std::to_string(baked.z) + " polys " + std::to_string(a) + "/" + std::to_string(b) + " at "
						+ std::to_string(pa.xz[0][0]) + "," + std::to_string(pa.xz[0][1]);
				}
			}
	}
	return census;
}

// The polygons within `radius` of a point (the 828 spot): index, vertex count, mean y, and the edges with no neighbour (wall or open).
void spotReport(const knav::NavMesh& mesh, const char* label, double x, double z, double radius)
{
	std::size_t listed = 0;
	for (const knav::BakedTile& baked : mesh.tiles())
	{
		const dtMeshTile* tile = mesh.detour().getTileAt(baked.x, baked.z, 0);
		if (!tile)
			continue;
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			const dtPoly& poly = tile->polys[p];
			double cy = 0, near = 1e30;
			for (int k = 0; k < poly.vertCount; ++k)
			{
				const float* v = &tile->verts[poly.verts[k] * 3];
				cy += v[1] / poly.vertCount;
				near = std::min<double>(near, std::hypot(v[0] - x, v[2] - z));
			}
			if (near > radius || listed >= 40)
				continue;
			++listed;
			std::cout << "    " << label << " tile " << baked.x << "," << baked.z << " poly " << p << " area " << int(poly.getArea()) << " y " << cy << " verts";
			int walls = 0;
			for (int k = 0; k < poly.vertCount; ++k)
			{
				const float* v = &tile->verts[poly.verts[k] * 3];
				std::cout << " (" << v[0] << "," << v[2] << ")";
				walls += poly.neis[k] == 0;
			}
			std::cout << " open/wall edges " << walls << "\n";
		}
	}
}

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
		<< report.doorFaces << ", seam prune dropped " << report.seamPruned << " tops, weld " << report.welded << " merged / " << report.weldSplit << " split / " << report.weldPaired << " paired / " << report.weldDropped << " dropped (" << report.weldThrough << " joined across; " << report.weldOrphanOpen << " neighbour edges still open, " << report.weldOrphanGap << " of them across a gap; dropped by neighbours 0/1/2/3+: " << report.weldByLive[0] << "/" << report.weldByLive[1] << "/" << report.weldByLive[2] << "/" << report.weldByLive[3] << "), open links " << report.openLinks << ", open shared " << report.openShared << ", max polygons per tile " << report.maxPolys << ", over the cap " << report.overfullTiles << "; surfaces " << static_cast<long>(report.msSurf)
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
	{
		const knav::NavMesh whole = knav::NavMesh::fromTiles(knav::configForWorldZones(cell, cell), tiles);
		const FoldCensus fold = foldCensus(whole, 0.01);
		std::cout << "  fold census " << name << ": " << fold.pairs << " overlapping polygon pairs (> 0.01 u^2), worst " << fold.worst << " u^2 " << fold.where << "\n";
		if (cell.x == 46 && cell.z == 16)
		{
			spotReport(whole, "828a", 67037, -72177, 6);
			spotReport(whole, "828b", 67095, -72169, 6);
			spotReport(whole, "828c", 67086, -72160, 4);
		}
	}
	useNodeCensus(dataDir, terrain, name, cell, tiles);
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

// The use nodes (bed, seat, machine, counter: where a user stands) of every building of the zone against the new mesh (kenshi-k16.7.189, ksim 815): a node
// is off the mesh when no walkable point lies within 0.75 u of it in the xz plane. Counts per zone and the worst ones by building.
void useNodeCensus(const std::string& dataDir, const kworld::Heightmap& terrain, const char* name, kworld::ZoneCell cell, std::vector<knav::BakedTile> tiles)
{
	const knav::AreaGeometry area = knav::gatherArea(dataDir, terrain, cell, cell);
	const knav::NavMesh mesh = knav::NavMesh::fromTiles(knav::configForWorldZones(cell, cell), std::move(tiles));
	const knav::NavQuery query(mesh);
	struct Off
	{
		double distance;
		std::string id;
		knav::Point at;
	};
	std::vector<Off> off;
	std::size_t nodes = 0, buildings = 0, buildingsOff = 0;
	for (const auto& building : area.buildings)
	{
		if (!building.shapes || building.shapes->empty() || !building.shapes->front().useNodes)
			continue;
		++buildings;
		std::size_t offHere = 0;
		for (const std::array<float, 3>& node : *building.shapes->front().useNodes)
		{
			++nodes;
			knav::Point hit{};
			const double far = query.nearestPointXZ(node, 8.0f, hit) ? std::hypot(hit[0] - node[0], hit[2] - node[2]) : 1e9;
			if (far > 0.75)
			{
				off.push_back({ far, building.id, node });
				++offHere;
			}
		}
		buildingsOff += offHere != 0;
	}
	std::sort(off.begin(), off.end(), [](const Off& a, const Off& b) { return a.distance > b.distance; });
	std::cout << "  use-node census " << name << " " << cell.x << "," << cell.z << ": " << nodes << " nodes in " << buildings << " buildings, " << off.size() << " off the mesh (" << buildingsOff
		<< " buildings)\n";
	for (std::size_t i = 0; i < off.size() && i < 20; ++i)
		std::cout << "    " << off[i].id << " node (" << off[i].at[0] << " " << off[i].at[1] << " " << off[i].at[2] << ") nearest walkable " << off[i].distance << " u\n";
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
		zone(dataDir, terrain, "Hub east", { 20, 33 });
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "knav_cutover ok\n";
	return 0;
}
