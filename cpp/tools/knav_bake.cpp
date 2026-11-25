// Offline navmesh bake: knav_bake [--out FILE] [--threads N] [--zones X0 Z0 X1 Z1]
// Defaults to the Hub and its neighbours (zone cells 19..21 x 31..33).
// Results are cached by input (tools/knav_bake_cache.hpp): KNAV_BAKE_CACHE=DIR or --cache DIR (--no-cache = off).
// World bake: knav_bake --world DIR [--regions RX0 RZ0 RX1 RZ1] [--threads N]
// writes DIR/region_RX_RZ.knav for each region of 4 x 4 zones (default all 16 x 16).

#include "knav_bake_cache.hpp"
#include "knav_bake_srchash.h"

#include "knav/area.hpp"
#include "knav/native.hpp"
#include "knav/navmesh.hpp"
#include "knav/seeds.hpp"
#include "knav/zone_gen.hpp"
#include "kworld/heightmap.hpp"

#include <DetourNavMesh.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

// Bump when the bake's output can change without a source change (an input outside the hashed sources).
constexpr int kBakeRecipeVersion = 2;

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point from)
{
	return std::chrono::duration<double>(Clock::now() - from).count();
}

// The tiles of zone cells min..max, one zone at a time through the cutover bake (knav::bakeZone: knav::ground + surfgen).
std::vector<knav::BakedTile> bakeRange(knav::AreaGatherer& gatherer, const kworld::Heightmap& terrain, const std::vector<knav::Point>& seedsDef,
	kworld::ZoneCell min, kworld::ZoneCell max, int threads, double& gatherSeconds)
{
	std::vector<knav::BakedTile> tiles;
	for (int z = min.z; z <= max.z; ++z)
		for (int x = min.x; x <= max.x; ++x)
		{
			const kworld::ZoneCell zone{ x, z };
			const auto step = Clock::now();
			const knav::AreaGeometry area = gatherer.gather(zone, zone);
			gatherSeconds += secondsSince(step);
			const knav::NavConfig zoneConfig = knav::configForWorldZones(zone, zone);
			knav::ZoneGenReport report;
			for (knav::BakedTile& tile : knav::bakeZone(zoneConfig, terrain, area, knav::zoneBakeSeeds(terrain, area, seedsDef, zone, zoneConfig.waterLevel), threads, &report))
				tiles.push_back(std::move(tile));
			for (const std::string& id : report.failedIds)
				std::cerr << "knav_bake: zone " << x << "," << z << ": building not generated: " << id << "\n";
			if (report.overfullTiles != 0)
				std::cerr << "knav_bake: zone " << x << "," << z << ": " << report.overfullTiles << " tiles over " << knav::kMaxPolysPerTile << " polygons left out\n";
		}
	return tiles;
}

// Walkable area by polygon area id (the last bucket: every door's own id), pruned polygons left out.
std::string areaTotals(const knav::NavMesh& mesh)
{
	double totals[static_cast<int>(knav::kAreaCount) + 1] = {};
	const dtNavMesh& detour = mesh.detour();
	for (int t = 0; t < detour.getMaxTiles(); ++t)
	{
		const dtMeshTile* tile = detour.getTile(t);
		if (tile == nullptr || tile->header == nullptr)
			continue;
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			const dtPoly& poly = tile->polys[p];
			if (poly.flags == 0 || poly.getType() != DT_POLYTYPE_GROUND)
				continue;
			double area = 0.0;
			for (int v = 2; v < poly.vertCount; ++v)
			{
				const float* a = &tile->verts[poly.verts[0] * 3];
				const float* b = &tile->verts[poly.verts[v - 1] * 3];
				const float* c = &tile->verts[poly.verts[v] * 3];
				area += std::abs((b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2])) / 2.0;
			}
			totals[std::min<int>(poly.getArea(), static_cast<int>(knav::kAreaCount))] += area;
		}
	}
	std::string out = "area u2 by id:";
	for (int a = 0; a <= static_cast<int>(knav::kAreaCount); ++a)
		out += " " + (a < static_cast<int>(knav::kAreaCount) ? std::to_string(a) : std::string("doors")) + "="
			+ std::to_string(std::lround(totals[a]));
	return out;
}

// One file per region, gathered from game data loaded once.
int bakeWorld(const std::string& dataDir, const std::string& dir, const int (&regions)[4], int threads)
{
	std::filesystem::create_directories(dir);
	const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
	const auto start = Clock::now();
	knav::AreaGatherer gatherer(dataDir, terrain);
	const std::vector<knav::Point> seedsDef = knav::readSeedsDef(knav::native::navtilesDir(dataDir));
	std::cout << "load: " << secondsSince(start) << " s\n";
	std::size_t tiles = 0, polys = 0;
	std::uintmax_t bytes = 0;
	double gatherSeconds = 0.0, bakeSeconds = 0.0;
	for (int rz = regions[1]; rz <= regions[3]; ++rz)
	{
		for (int rx = regions[0]; rx <= regions[2]; ++rx)
		{
			const kworld::ZoneCell min{ rx * knav::kRegionZones, rz * knav::kRegionZones };
			const kworld::ZoneCell max{ min.x + knav::kRegionZones - 1, min.z + knav::kRegionZones - 1 };
			const auto step = Clock::now();
			double gathered = 0.0;
			std::vector<knav::BakedTile> baked = bakeRange(gatherer, terrain, seedsDef, min, max, threads, gathered);
			const double seconds = secondsSince(step);
			const knav::NavMesh mesh = knav::NavMesh::fromTiles(knav::configForWorldZones(min, max), std::move(baked));
			const std::string path = dir + "/" + knav::regionFileName(rx, rz);
			std::filesystem::remove(path); // may be a read-only hard link into the bake cache: never write through it
			mesh.save(path);
			tiles += mesh.tiles().size();
			polys += mesh.polyCount();
			bytes += std::filesystem::file_size(path);
			gatherSeconds += gathered;
			bakeSeconds += seconds - gathered;
			std::cout << path << ": " << mesh.tiles().size() << " tiles, " << mesh.polyCount() << " polys, gather " << std::fixed << std::setprecision(1)
				<< gathered << " s, bake " << seconds - gathered << " s\n  " << areaTotals(mesh) << "\n" << std::flush;
		}
	}
	std::cout << "total: " << tiles << " tiles, " << polys << " polys, " << bytes << " bytes, gather " << gatherSeconds << " s, bake " << bakeSeconds
		<< " s on " << threads << " thread(s)\n";
	return 0;
}

// The Hub-sized single-file bake.
int bakeSingle(const std::string& dataDir, const std::string& out, kworld::ZoneCell min, kworld::ZoneCell max, int threads)
{
	const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
	knav::AreaGatherer gatherer(dataDir, terrain);
	const std::vector<knav::Point> seedsDef = knav::readSeedsDef(knav::native::navtilesDir(dataDir));
	const auto start = Clock::now();
	double gathered = 0.0;
	std::vector<knav::BakedTile> tiles = bakeRange(gatherer, terrain, seedsDef, min, max, threads, gathered);
	const knav::NavMesh mesh = knav::NavMesh::fromTiles(knav::configForWorldZones(min, max), std::move(tiles));
	std::filesystem::remove(out); // may be a read-only hard link into the bake cache: never write through it
	mesh.save(out);
	std::cout << "bake: " << secondsSince(start) << " s (gather " << gathered << " s) on " << threads << " thread(s)\n"
		<< "tiles: " << mesh.tiles().size() << ", polys: " << mesh.polyCount() << ", cache: " << std::filesystem::file_size(out) << " bytes (" << out << ")\n"
		<< "doors: " << mesh.doorKeys().size() << " have polygons in the mesh\n";
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	std::string out = "hub.knav";
	std::string worldDir;
	int regions[4] = { 0, 0, kworld::kZoneCellsPerAxis / knav::kRegionZones - 1, kworld::kZoneCellsPerAxis / knav::kRegionZones - 1 };
	int threads = 4;
	std::string cacheRoot = std::getenv("KNAV_BAKE_CACHE") ? std::getenv("KNAV_BAKE_CACHE") : KNAV_BAKE_CACHE_DEFAULT;
	kworld::ZoneCell min{ 19, 31 };
	kworld::ZoneCell max{ 21, 33 };
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--out" && i + 1 < argc)
			out = argv[++i];
		else if (arg == "--threads" && i + 1 < argc)
			threads = std::atoi(argv[++i]);
		else if (arg == "--cache" && i + 1 < argc)
			cacheRoot = argv[++i];
		else if (arg == "--no-cache")
			cacheRoot.clear();
		else if (arg == "--world" && i + 1 < argc)
			worldDir = argv[++i];
		else if (arg == "--regions" && i + 4 < argc)
		{
			for (int& region : regions)
				region = std::atoi(argv[++i]);
		}
		else if (arg == "--zones" && i + 4 < argc)
		{
			min = { std::atoi(argv[i + 1]), std::atoi(argv[i + 2]) };
			max = { std::atoi(argv[i + 3]), std::atoi(argv[i + 4]) };
			i += 4;
		} else {
			std::cerr << "usage: knav_bake [--out FILE] [--threads N] [--zones X0 Z0 X1 Z1]\n"
				"       knav_bake --world DIR [--regions RX0 RZ0 RX1 RZ1] [--threads N]\n"
				"  a zone is knav::bakeZone: knav::ground + surfgen (kenshi-k16.7.189)\n";
			return 2;
		}
	}

	try
	{
		const std::string dataDir = KENSHI_DATA;

#ifdef _WIN32
		if (!cacheRoot.empty())
			std::cerr << "knav_bake: the result cache is POSIX-only; baking uncached\n";
#else
		if (!cacheRoot.empty())
		{
			// Thread count and the output path do not change the bytes (knav_bake_threads).
			std::ostringstream key;
			key << "recipe " << kBakeRecipeVersion << "\nsrc " << KNAV_BAKE_SRC_HASH << "\ndata " << dataDir << " "
				<< knavcache::dataStamp(dataDir) << "\n";
			if (!worldDir.empty())
				key << "world " << regions[0] << " " << regions[1] << " " << regions[2] << " " << regions[3] << "\n";
			else
				key << "zones " << min.x << " " << min.z << " " << max.x << " " << max.z << "\n";
#if KSIM_DEV
			// Test hook for the .failed path (kenshi-k16.15.45): KNAV_BAKE_TEST_FAIL=throw|exit makes the bake fail after some output.
			const char* const injected = std::getenv("KNAV_BAKE_TEST_FAIL");
			if (injected)
				key << "test-fail " << injected << "\n";
#endif
			return knavcache::cached(cacheRoot, key.str(),
				[&](const std::filesystem::path& data) {
#if KSIM_DEV
					if (injected)
					{
						for (int i = 0; i < 60; ++i)
							std::cout << "bake progress line " << i << "\n";
						std::cerr << "injected failure\n";
						if (std::string(injected) == "throw")
							throw std::runtime_error("injected bake error");
						return 3;
					}
#endif
					return worldDir.empty() ? bakeSingle(dataDir, (data / "bake.knav").string(), min, max, threads)
						: bakeWorld(dataDir, data.string(), regions, threads);
				},
				[&](const std::filesystem::path& data) {
					if (!worldDir.empty())
						knavcache::linkTree(data, worldDir);
					else
						knavcache::linkFile(data / "bake.knav", out);
				});
		}
#endif
		return worldDir.empty() ? bakeSingle(dataDir, out, min, max, threads)
			: bakeWorld(dataDir, worldDir, regions, threads);
	} catch (const std::exception& e) {
		std::cerr << "knav_bake: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
