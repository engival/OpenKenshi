// Tile assembly (knav/assemble.hpp, kenshi-k16.7.144): synthetic cuts, then the Hub bar tile: Recast bake of the zone without the bar, the
// generator's interior mesh cutting it, one nativeTiles call.

#include "kdata/gamedata.hpp"
#include "knav/area.hpp"
#include "knav/assemble.hpp"
#include "knav/surfgen.hpp"
#include "kworld/collision.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace
{

using P3 = std::array<double, 3>;

knav::native::Mesh quad(double x0, double z0, double x1, double z1, double y)
{
	knav::native::Mesh m;
	m.vertices = { { x0, y, z0 }, { x0, y, z1 }, { x1, y, z1 }, { x1, y, z0 } };
	m.faces.push_back({ 0, 4, 0 });
	for (std::uint32_t k = 0; k < 4; ++k)
	{
		knav::native::Edge e;
		e.a = k;
		e.b = (k + 1) % 4;
		m.edges.push_back(e);
	}
	return m;
}

double xzArea(const knav::native::Mesh& m)
{
	double total = 0;
	for (const knav::native::Face& f : m.faces)
	{
		double a2 = 0;
		for (std::uint32_t k = 0; k < f.numEdges; ++k)
		{
			const P3 &a = m.vertices[m.edges[f.startEdge + k].a], &b = m.vertices[m.edges[f.startEdge + (k + 1) % f.numEdges].a];
			a2 += a[0] * b[2] - b[0] * a[2];
		}
		total += 0.5 * std::fabs(a2);
	}
	return total;
}

void synthetic()
{
	knav::NavConfig config;
	config.tilesX = config.tilesZ = 4;
	// a zone-side face 100 x 100 at y 0; terrain: the plane y = 5 over x >= 50 and y = -5 below that, two triangles each side
	const knav::native::Mesh face = quad(0, 0, 100, 100, 0);
	knav::assembly::Input in;
	in.config = &config;
	in.tile = { 0, 0 };
	in.zoneSide = { &face };
	in.terrain = { { { { 50, 5, 0 }, { 50, 5, 200 }, { 200, 5, 200 } } }, { { { 50, 5, 0 }, { 200, 5, 200 }, { 200, 5, 0 } } },
		{ { { -100, -5, 0 }, { -100, -5, 200 }, { 50, -5, 200 } } }, { { { -100, -5, 0 }, { 50, -5, 200 }, { 50, -5, 0 } } } };
	knav::assembly::Report r;
	const knav::native::Mesh world = knav::assembly::assembleWorld(in, {}, &r);
	std::cout << "synthetic terrain cut: " << world.faces.size() << " faces, " << xzArea(world) << " u2\n";
	test::require(std::fabs(xzArea(world) - 5000.0) < 1.0, "the half of the face under the terrain is cut away");

	// the generator's face cuts a Recast-like face: the zone-side quad over the recast quad
	// (no BakedTile here: use the cutter path on an empty recast and check the cutter leaves generated faces alone)
	knav::assembly::Input two = in;
	two.terrain.clear();
	two.cutters = { { { { 40, 40 }, { 40, 60 }, { 60, 60 }, { 60, 40 } }, -100, 100 } };
	const knav::native::Mesh untouched = knav::assembly::assembleWorld(two, {}, nullptr);
	test::require(std::fabs(xzArea(untouched) - 10000.0) < 1.0, "a cutter cuts Recast faces only, not the generated zone side");
}

} // namespace

int main()
{
	try
	{
		synthetic();
		const std::string dataDir = KENSHI_DATA;
		const kworld::Heightmap terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const kdata::GameData gd = kworld::loadMergedGameData(dataDir);
		kworld::CollisionLibrary library{ dataDir, gd };
		const kworld::ZoneCell zone = kworld::zoneCellOf(-51181.133, 2638.970);
		const kworld::AreaGeometry area = knav::gatherArea(dataDir, terrain, zone, zone);
		const kworld::AreaBuilding* bar = nullptr;
		for (const kworld::AreaBuilding& b : area.buildings)
			if (!b.shapes->empty() && b.placed.instanceID.find("4933-Newwworld") != std::string::npos)
				bar = &b;
		test::require(bar != nullptr, "the Hub bar is placed in zone 20,32");
		std::vector<kworld::WorldShape> mask;
		test::require(library.addInteriorMask(bar->placed, mask), "the bar has an interior mask");
		knav::surfgen::Input gen;
		for (const kworld::WorldShape& s : *bar->shapes)
			gen.shapes.push_back(&s);
		for (const kworld::WorldShape& m : mask)
			gen.mask.push_back(&m);
		knav::native::Mesh interior = knav::surfgen::generate(gen);
		interior.info = "bar";
		test::require(!interior.faces.empty(), "the generator makes the bar interior");

		// the Recast pass with the bar's shapes left unfilled
		const std::vector<kworld::WorldShape> all = area.flatShapes();
		std::vector<const kworld::WorldShape*> others;
		for (const kworld::AreaBuilding& b : area.buildings)
			if (&b != bar)
				for (const kworld::WorldShape& s : *b.shapes)
					others.push_back(&s);
		const knav::NavConfig config = knav::configForWorldZones(zone, zone);
		double cx = 0, cz = 0;
		for (const P3& v : interior.vertices)
		{
			cx += v[0];
			cz += v[2];
		}
		cx /= static_cast<double>(interior.vertices.size());
		cz /= static_cast<double>(interior.vertices.size());
		const knav::TileCoord tile{ static_cast<int>(std::floor((cx - config.originX) / config.tileSize)),
			static_cast<int>(std::floor((cz - config.originZ) / config.tileSize)) };
		const std::vector<knav::BakedTile> recast = knav::bakeTileSet(config, terrain, others, { tile }, 1);
		test::require(recast.size() == 1, "the Recast tile bakes");

		knav::assembly::Input in;
		in.config = &config;
		in.tile = tile;
		in.recast = &recast[0];
		in.interiors = { &interior };
		// the terrain inside the building's mask is carved (BUILDING `interior terrain` 0): each mask shape's footprint hull cuts at every height
		for (const kworld::WorldShape& m : mask)
		{
			std::vector<std::array<double, 2>> pts;
			for (std::size_t v = 0; v + 2 < m.vertices.size(); v += 3)
				pts.push_back({ m.vertices[v], m.vertices[v + 2] });
			std::sort(pts.begin(), pts.end());
			std::vector<std::array<double, 2>> hull(2 * pts.size());
			std::size_t k = 0;
			const auto turn = [](const auto& o, const auto& a, const auto& b) { return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]); };
			for (std::size_t i = 0; i < pts.size(); ++i)
			{
				while (k >= 2 && turn(hull[k - 2], hull[k - 1], pts[i]) <= 0.0) --k;
				hull[k++] = pts[i];
			}
			for (std::size_t i = pts.size() - 1, lower = k + 1; i-- > 0;)
			{
				while (k >= lower && turn(hull[k - 2], hull[k - 1], pts[i]) <= 0.0) --k;
				hull[k++] = pts[i];
			}
			hull.resize(k > 1 ? k - 1 : k);
			in.cutters.push_back({ hull, -1e6, 1e6 });
		}
		const double x0 = config.originX + tile.x * config.tileSize, z0 = config.originZ + tile.z * config.tileSize;
		in.terrain = knav::assembly::terrainTriangles(terrain, x0 - 8, z0 - 8, x0 + config.tileSize + 8, z0 + config.tileSize + 8);
		knav::assembly::Report rep;
		knav::NativeTileStats stats;
		const knav::native::Mesh plain = knav::assembly::recastFaces(recast[0]).mesh;
		const knav::native::Mesh world = knav::assembly::assembleWorld(in, {}, &rep);
		const auto tiles = knav::assembly::assembleTile(in, {}, nullptr, {}, &stats);
		std::cout << "hub bar tile " << tile.x << "," << tile.z << ": recast " << plain.faces.size() << " faces " << xzArea(plain) << " u2; assembled world "
			<< world.faces.size() << " faces " << xzArea(world) << " u2 (removed " << rep.recastRemovedArea << "); polys " << stats.polys << ", overfull "
			<< stats.overfull << ", openShared " << stats.openShared << "\n";
		test::require(rep.recastRemovedArea > 1000.0, "the bar's interior cuts the Recast ground under it");
		test::require(xzArea(world) < xzArea(plain), "the assembled world mesh is smaller by the cut");
		test::require(tiles.size() == 1 && !tiles[0].data.empty(), "one nativeTiles call builds the tile");
		test::require(stats.overfull == 0, "the tile is under the polygon cap");
		std::cout << "knav_assemble ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "knav_assemble: " << e.what() << "\n";
		return 1;
	}
}
