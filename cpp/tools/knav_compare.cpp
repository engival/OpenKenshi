// knav_compare: our navmesh bake against the original's baked one (knav/native.hpp), over a zone or a box.
//   knav_compare --navworld DIR (--zone ZX ZZ | --box X0 Z0 X1 Z1) [--cell 2] [--tol 30] [--radius 4] [--y Y0 Y1] [--ppm OUT.ppm] [--navtiles DIR]
//   knav_compare --navworld DIR --at X Z [X Z ...]
//   knav_compare --list (--zone ZX ZZ | --box X0 Z0 X1 Z1): the original's meshes reaching into the box, with their floor heights
// DIR is a `knav_bake --world` bake; --navtiles defaults to the game's (KENSHI_DATA); --live FILE.knm grades against a
// kinspect export of a live game's meshes instead (the Hub's shipped zone is stale: NAVMESH_CACHE "Conversion"). The method is the kinspect session's
// knative.py compare (findings/navmesh.md "Native vs our Detour bake"): both meshes are sampled at the centres of a grid of
// cells, one sample per surface layer, and two samples match when they share a cell and lie within --tol in height (the
// original's faces are coarse in height, so a tolerance of 30 measures where, not how high). Prints our cover of the original's
// surface by distance from its walls, the original's surface we lack beyond --radius (our bake's erosion), each interior's
// cover, and our surface the original has not. --ppm writes a map, one pixel per cell, z downward: red the original's surface
// beyond --radius that we lack, dark grey the same inside --radius, blue ours where the original has no surface, yellow ours
// where the original's is at another height, light grey both.
// --y keeps only the surface between those heights on both sides (one floor of a building, its stairs).
// Heavy for a whole zone at --cell 1: run it on the dev box (docs/RUNBOOK.md "Dev box").

#include "knav/compare.hpp"
#include "knav/native.hpp"
#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"

#include <DetourNavMesh.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace knav::compare;

// Our bake over the zones zx0..zx1 x zz0..zz1, only the tiles that meet the box.
knav::NavMesh loadOurs(const std::string& dir, int zx0, int zz0, int zx1, int zz1, double x0, double z0, double x1, double z1)
{
	std::vector<std::string> paths;
	for (int rz = zz0 / knav::kRegionZones; rz <= zz1 / knav::kRegionZones; ++rz)
	{
		for (int rx = zx0 / knav::kRegionZones; rx <= zx1 / knav::kRegionZones; ++rx)
		{
			const std::string path = dir + "/" + knav::regionFileName(rx, rz);
			if (std::filesystem::exists(path))
				paths.push_back(path);
		}
	}
	if (paths.empty())
		throw std::runtime_error("no region file of " + dir + " covers the box");
	const knav::NavConfig grid = knav::configForWorldZones({0, 0}, {0, 0});
	knav::TileRect keep;
	keep.x0 = static_cast<int>(std::floor((x0 - grid.originX) / grid.tileSize));
	keep.z0 = static_cast<int>(std::floor((z0 - grid.originZ) / grid.tileSize));
	keep.x1 = static_cast<int>(std::floor((x1 - 1e-6 - grid.originX) / grid.tileSize));
	keep.z1 = static_cast<int>(std::floor((z1 - 1e-6 - grid.originZ) / grid.tileSize));
	return knav::NavMesh::load(paths, keep);
}

int zoneOf(double v)
{
	return static_cast<int>(std::floor((v - kworld::kWorldMin) / kworld::kZoneSize));
}

// The native meshes of the zones round the box, one zone further each way (an interior stored next door reaches in).
// `source` is a navtiles directory, or a KNM1 file (a live export, --live): all of its meshes.
std::vector<knav::native::Mesh> loadNative(const std::string& source, int zx0, int zz0, int zx1, int zz1)
{
	if (source.ends_with(".knm"))
		return knav::native::loadKnm(source);
	const std::string& dir = source;
	std::vector<knav::native::Mesh> meshes;
	for (int j = zz0 - 1; j <= zz1 + 1; ++j)
	{
		for (int i = zx0 - 1; i <= zx1 + 1; ++i)
		{
			std::vector<knav::native::Mesh> tile = knav::native::loadTile(dir, i, j);
			meshes.insert(meshes.end(), std::make_move_iterator(tile.begin()), std::make_move_iterator(tile.end()));
		}
	}
	return meshes;
}

double percent(std::size_t part, std::size_t whole)
{
	return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

int compare(const std::string& navworld, const std::string& navtiles, double x0, double z0, double x1, double z1, double cell, double tol,
	double radius, double y0, double y1, const std::string& ppm)
{
	Grid g{x0, z0, cell, static_cast<int>(std::lround((x1 - x0) / cell)), static_cast<int>(std::lround((z1 - z0) / cell))};
	const int zx0 = zoneOf(x0), zz0 = zoneOf(z0), zx1 = zoneOf(x1 - 1e-6), zz1 = zoneOf(z1 - 1e-6);
	const std::vector<knav::native::Mesh> meshes = loadNative(navtiles, zx0, zz0, zx1, zz1);
	std::vector<Tri> ntris;
	std::vector<Wall> walls;
	nativeGeometry(meshes, ntris, walls);
	const knav::NavMesh ours = loadOurs(navworld, zx0, zz0, zx1, zz1, x0, z0, x1, z1);
	const std::vector<Tri> otris = ourTriangles(ours);
	std::cout << std::fixed << std::setprecision(1);
	std::cout << "box " << x0 << " " << z0 << " .. " << x1 << " " << z1 << ", grid " << g.nx << " x " << g.nz << " cell " << cell << ", tol " << tol
		<< ", radius " << radius << "\n";
	std::size_t interiors = 0;
	for (const auto& m : meshes)
		interiors += m.interior() ? 1 : 0;
	std::cout << "native: " << meshes.size() << " meshes (" << interiors << " interiors), " << ntris.size() << " triangles, " << walls.size()
		<< " wall edges; ours: " << otris.size() << " triangles\n";

	std::vector<Sample> ns = raster(ntris, g);
	std::vector<Sample> os = raster(otris, g);
	if (y0 < y1)
	{
		const auto outside = [&](const Sample& s) { return s.h < y0 || s.h > y1; };
		ns.erase(std::remove_if(ns.begin(), ns.end(), outside), ns.end());
		os.erase(std::remove_if(os.begin(), os.end(), outside), os.end());
	}
	const std::vector<double> n2o = match(ns, os);
	const std::vector<double> o2n = match(os, ns);

	// Each native sample's distance (xz) to the nearest native wall at its height (within tol), up to kFar.
	constexpr double kFar = 24.0;
	const std::vector<double> wallDistance = wallDistances(ns, g, walls, tol, kFar);

	std::size_t nInterior = 0;
	for (const Sample& s : ns)
		nInterior += meshes[static_cast<std::size_t>(ntris[static_cast<std::size_t>(s.tri)].source)].interior() ? 1 : 0;
	std::cout << "samples: native " << ns.size() << " (interior " << nInterior << "), ours " << os.size() << "\n";

	const std::vector<double> rings{1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 24};
	std::cout << "our cover of the native surface by distance from its wall:";
	double inner = 0.0;
	for (const double r : rings)
	{
		std::size_t all = 0, hit = 0;
		for (std::size_t k = 0; k < ns.size(); ++k)
		{
			if (wallDistance[k] >= inner && wallDistance[k] < r)
			{
				++all;
				hit += std::abs(n2o[k]) <= tol ? 1 : 0;
			}
		}
		std::cout << " <" << r << " " << percent(hit, all) << "% (" << all << ")";
		inner = r;
	}
	{
		std::size_t all = 0, hit = 0;
		for (std::size_t k = 0; k < ns.size(); ++k)
		{
			if (wallDistance[k] >= kFar)
			{
				++all;
				hit += std::abs(n2o[k]) <= tol ? 1 : 0;
			}
		}
		std::cout << " >=" << kFar << " " << percent(hit, all) << "% (" << all << ")\n";
	}

	std::size_t kept = 0, keptHit = 0, missWorld = 0, missInterior = 0;
	std::map<std::string, std::pair<std::size_t, std::size_t>> perInterior; // id -> (kept, hit)
	for (std::size_t k = 0; k < ns.size(); ++k)
	{
		if (wallDistance[k] < radius)
			continue;
		const knav::native::Mesh& mesh = meshes[static_cast<std::size_t>(ntris[static_cast<std::size_t>(ns[k].tri)].source)];
		const bool hit = std::abs(n2o[k]) <= tol;
		++kept;
		keptHit += hit ? 1 : 0;
		if (!hit)
			(mesh.interior() ? missInterior : missWorld) += 1;
		if (mesh.interior())
		{
			++perInterior[mesh.info].first;
			perInterior[mesh.info].second += hit ? 1 : 0;
		}
	}
	std::cout << "native beyond " << radius << " of a wall: " << kept << ", ours covers " << percent(keptHit, kept) << "%, missing world "
		<< missWorld << " interior " << missInterior << " (" << (missWorld + missInterior) * cell * cell << " u2)\n";
	std::cout << "interiors, our cover beyond " << radius << ":";
	for (const auto& [id, counts] : perInterior)
		std::cout << " " << id << " " << percent(counts.second, counts.first) << "% of " << counts.first * cell * cell << " u2;";
	std::cout << "\n";

	std::size_t none = 0, otherHeight = 0;
	std::map<int, std::size_t> byArea;
	std::vector<double> dys;
	for (std::size_t k = 0; k < os.size(); ++k)
	{
		if (std::abs(o2n[k]) <= tol)
			continue;
		++none;
		++byArea[otris[static_cast<std::size_t>(os[k].tri)].source];
		if (std::isfinite(o2n[k]))
		{
			++otherHeight;
			dys.push_back(std::abs(o2n[k]));
		}
	}
	std::sort(dys.begin(), dys.end());
	std::cout << "ours with no native surface within tol: " << none << " of " << os.size() << " (" << percent(none, os.size()) << "%), by area:";
	for (const auto& [area, n] : byArea)
		std::cout << " " << area << "=" << n;
	std::cout << "; at another height in the cell: " << otherHeight;
	if (!dys.empty())
		std::cout << " (|dy| p50 " << dys[dys.size() / 2] << ", p95 " << dys[dys.size() * 95 / 100] << ")";
	std::cout << "\n";

	if (!ppm.empty())
	{
		// Worst first per cell: 5 red, 4 blue, 3 yellow, 2 dark grey, 1 light grey.
		std::vector<std::uint8_t> category(static_cast<std::size_t>(g.nx) * static_cast<std::size_t>(g.nz), 0);
		const auto raise = [&](std::int64_t c, std::uint8_t v) { category[static_cast<std::size_t>(c)] = std::max(category[static_cast<std::size_t>(c)], v); };
		for (std::size_t k = 0; k < ns.size(); ++k)
		{
			const bool hit = std::abs(n2o[k]) <= tol;
			raise(ns[k].cell, hit ? 1 : wallDistance[k] < radius ? 2 : 5);
		}
		for (std::size_t k = 0; k < os.size(); ++k)
		{
			if (std::abs(o2n[k]) > tol)
				raise(os[k].cell, std::isfinite(o2n[k]) ? 3 : 4);
		}
		static const std::uint8_t palette[6][3] = {{0, 0, 0}, {170, 170, 170}, {70, 70, 70}, {230, 200, 0}, {40, 90, 255}, {230, 30, 30}};
		std::ofstream out(ppm, std::ios::binary);
		out << "P6\n" << g.nx << " " << g.nz << "\n255\n";
		for (const std::uint8_t c : category)
			out.write(reinterpret_cast<const char*>(palette[c]), 3);
		std::cout << "wrote " << ppm << "\n";
	}
	return 0;
}

// The surface heights of both meshes at world points.
int probe(const std::string& navworld, const std::string& navtiles, const std::vector<std::pair<double, double>>& points)
{
	for (const auto& [x, z] : points)
	{
		const int zx = zoneOf(x), zz = zoneOf(z);
		std::vector<Tri> ntris;
		std::vector<Wall> walls;
		const std::vector<knav::native::Mesh> meshes = loadNative(navtiles, zx, zz, zx, zz);
		nativeGeometry(meshes, ntris, walls);
		const knav::NavMesh ours = loadOurs(navworld, zx, zz, zx, zz, x - 1.0, z - 1.0, x + 1.0, z + 1.0);
		const Grid g{x - 0.5, z - 0.5, 1.0, 1, 1};
		std::cout << std::fixed << std::setprecision(1) << x << " " << z << " native";
		for (const Sample& s : raster(ntris, g))
			std::cout << " " << s.h << (meshes[static_cast<std::size_t>(ntris[static_cast<std::size_t>(s.tri)].source)].interior() ? "i" : "");
		std::cout << " | ours";
		for (const Sample& s : raster(ourTriangles(ours), g))
			std::cout << " " << s.h;
		std::cout << "\n";
	}
	return 0;
}

// The original's meshes that reach into the box: id, base, faces, and its floors (face area by the face's mean height, 5 u steps).
int list(const std::string& navtiles, double x0, double z0, double x1, double z1)
{
	const std::vector<knav::native::Mesh> meshes = loadNative(navtiles, zoneOf(x0), zoneOf(z0), zoneOf(x1 - 1e-6), zoneOf(z1 - 1e-6));
	std::cout << std::fixed << std::setprecision(1);
	for (const knav::native::Mesh& mesh : meshes)
	{
		double mx0 = 1e30, mz0 = 1e30, mx1 = -1e30, mz1 = -1e30;
		for (const Vec& v : mesh.vertices)
		{
			mx0 = std::min(mx0, v[0]);
			mx1 = std::max(mx1, v[0]);
			mz0 = std::min(mz0, v[2]);
			mz1 = std::max(mz1, v[2]);
		}
		if (mx1 < x0 || mx0 > x1 || mz1 < z0 || mz0 > z1)
			continue;
		std::map<long, double> floors; // 5 u height step -> area
		std::size_t doors = 0;
		for (const knav::native::Face& face : mesh.faces)
		{
			double y = 0.0, area = 0.0;
			const Vec& a = mesh.vertices[mesh.edges[face.startEdge].a];
			for (int k = 0; k < face.numEdges; ++k)
			{
				const Vec& b = mesh.vertices[mesh.edges[face.startEdge + k].a];
				const Vec& c = mesh.vertices[mesh.edges[face.startEdge + (k + 1) % face.numEdges].a];
				y += b[1];
				area += 0.5 * ((b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]));
			}
			floors[std::lround(y / std::max<int>(face.numEdges, 1) / 5.0)] += std::abs(area);
			doors += face.data == 4 || face.data == 5 ? 1 : 0;
		}
		std::cout << (mesh.interior() ? mesh.info : "world " + std::to_string(mesh.tileX) + "." + std::to_string(mesh.tileZ)) << " base " << mesh.base[0]
			<< " " << mesh.base[1] << " " << mesh.base[2] << ", x " << mx0 << ".." << mx1 << " z " << mz0 << ".." << mz1 << ", " << mesh.faces.size()
			<< " faces, " << doors << " door faces";
		if (mesh.interior())
		{
			std::cout << "; floors (y: u2):";
			for (const auto& [step, area] : floors)
			{
				if (area >= 1.0)
					std::cout << " " << step * 5 << ": " << std::lround(area);
			}
		}
		std::cout << "\n";
	}
	return 0;
}

int usage()
{
	std::cerr << "usage: knav_compare --navworld DIR (--zone ZX ZZ | --box X0 Z0 X1 Z1) [--cell C] [--tol T] [--radius R] [--y Y0 Y1] [--ppm OUT.ppm] [--navtiles DIR | --live FILE.knm]\n"
		"       knav_compare --navworld DIR --at X Z [X Z ...] [--navtiles DIR]\n"
		"       knav_compare --list (--zone ZX ZZ | --box X0 Z0 X1 Z1) [--navtiles DIR]\n";
	return 2;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		std::string navworld, ppm;
		std::string navtiles = knav::native::navtilesDir(KENSHI_DATA);
		double cell = 2.0, tol = 30.0, radius = 4.0, y0 = 0.0, y1 = 0.0;
		std::vector<double> box;
		std::vector<std::pair<double, double>> points;
		bool listing = false;
		for (int i = 1; i < argc; ++i)
		{
			const std::string arg = argv[i];
			const auto next = [&]() -> double
			{
				if (i + 1 >= argc)
					throw std::runtime_error(arg + " needs a value");
				return std::stod(argv[++i]);
			};
			if (arg == "--list")
				listing = true;
			else if (arg == "--navworld" && i + 1 < argc)
				navworld = argv[++i];
			else if ((arg == "--navtiles" || arg == "--live") && i + 1 < argc)
				navtiles = argv[++i];
			else if (arg == "--ppm" && i + 1 < argc)
				ppm = argv[++i];
			else if (arg == "--cell")
				cell = next();
			else if (arg == "--tol")
				tol = next();
			else if (arg == "--radius")
				radius = next();
			else if (arg == "--y")
			{
				y0 = next();
				y1 = next();
			}
			else if (arg == "--zone")
			{
				const double zx = next(), zz = next();
				box = {kworld::kWorldMin + kworld::kZoneSize * zx, kworld::kWorldMin + kworld::kZoneSize * zz,
					kworld::kWorldMin + kworld::kZoneSize * (zx + 1), kworld::kWorldMin + kworld::kZoneSize * (zz + 1)};
			}
			else if (arg == "--box")
			{
				box.clear();
				for (int k = 0; k < 4; ++k)
					box.push_back(next());
			}
			else if (arg == "--at")
			{
				while (i + 2 < argc && std::string_view(argv[i + 1]).substr(0, 2) != "--")
				{
					const double x = std::stod(argv[++i]);
					const double z = std::stod(argv[++i]);
					points.emplace_back(x, z);
				}
			}
			else
				return usage();
		}
		if (listing && box.size() == 4)
			return list(navtiles, box[0], box[1], box[2], box[3]);
		if (navworld.empty() || (box.empty() == points.empty()))
			return usage();
		if (!points.empty())
			return probe(navworld, navtiles, points);
		return compare(navworld, navtiles, box[0], box[1], box[2], box[3], cell, tol, radius, y0, y1, ppm);
	}
	catch (const std::exception& error)
	{
		std::cerr << "knav_compare: " << error.what() << "\n";
		return 1;
	}
}
