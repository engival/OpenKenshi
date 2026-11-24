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
#include "knav/world.hpp"
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

void setSeeds(knav::NavWorld& world, const std::vector<knav::Point>& seedsDef, const kworld::Heightmap& terrain,
	const knav::AreaGeometry& area, kworld::ZoneCell min, kworld::ZoneCell max)
{
	std::vector<knav::Point> seeds = knav::zoneSeeds(terrain, area, seedsDef, min, max, world.mesh().config().waterLevel);
	const std::vector<knav::Point> origins = knav::buildingSeeds(area);
	seeds.insert(seeds.end(), origins.begin(), origins.end());
	world.setSeeds(std::move(seeds), knav::doorSeedsOf(area));
}

// --trace: the seeds that keep the surface nearest `at` (a seed keeps the connected set of its nearest polygon).
void traceSeeds(const knav::NavMesh& mesh, const knav::Point& at, const std::vector<knav::Point>& seedsDef,
	const kworld::Heightmap& terrain, const knav::AreaGeometry& area, kworld::ZoneCell min, kworld::ZoneCell max)
{
	const knav::PolyComponents components(mesh, 0xffff);
	const knav::NavQuery query(mesh, 65535);
	knav::PolyLoc where;
	if (!query.locate(at, { 4.0f, 50.0f, 4.0f }, where))
	{
		std::cout << "trace: no surface near the point\n";
		return;
	}
	const std::uint32_t label = components.at(where.tileX, where.tileZ, where.poly);
	std::cout << "trace: surface at " << where.point[1] << ", connected set " << label << "\n";
	const std::pair<const char*, std::vector<knav::Point>> sources[] = {
		{ "zone", knav::zoneSeeds(terrain, area, seedsDef, min, max, mesh.config().waterLevel) },
		{ "building origin", knav::buildingSeeds(area) },
		{ "door", knav::doorSeeds(mesh, knav::doorSeedsOf(area), mesh.config().waterLevel) },
	};
	std::optional<knav::Point> nearest;
	const auto flat = [&](const knav::Point& p) { return std::hypot(p[0] - at[0], p[2] - at[2]); };
	for (const auto& [name, seeds] : sources)
	{
		for (const knav::Point& seed : seeds)
		{
			if (!query.locate(seed, knav::kSeedReach, where) || components.at(where.tileX, where.tileZ, where.poly) != label)
				continue;
			std::cout << "  " << name << " " << seed[0] << " " << seed[1] << " " << seed[2] << "\n";
			if (!nearest || flat(seed) < flat(*nearest))
				nearest = where.point;
		}
	}
	if (!nearest)
		return;
	// The way up from the nearest seed: where the surface climbs is where it joins what the original keeps apart.
	const knav::PathResult path = query.findPath(*nearest, at, { 4.0f, 50.0f, 4.0f });
	std::cout << "path from the nearest seed (" << (path.complete ? "complete" : "partial") << "):\n";
	for (const knav::Point& p : path.points)
	{
		std::cout << "  " << p[0] << " " << p[1] << " " << p[2] << "\n";
		// The building shapes under the point: the part that carries it.
		for (const knav::AreaBuilding& building : area.buildings)
		{
			for (std::size_t i = 0; building.shapes && i < building.shapes->size(); ++i)
			{
				const kworld::WorldShape& shape = (*building.shapes)[i];
				if (p[0] < shape.min[0] - 1 || p[0] > shape.max[0] + 1 || p[2] < shape.min[2] - 1 || p[2] > shape.max[2] + 1
					|| p[1] < shape.min[1] - 2 || p[1] > shape.max[1] + 2)
					continue;
				std::cout << "    " << building.id << " " << building.placed.buildingID << " shape " << i << " use "
					<< static_cast<int>(shape.use) << (shape.solid ? " solid" : "") << (shape.closed ? " closed" : "")
					<< " y " << shape.min[1] << ".." << shape.max[1] << ", " << shape.indices.size() / 3 << " tris\n";
				if (shape.half != std::array<float, 3>{})
					std::cout << "      box at " << shape.actor[0] << " " << shape.actor[1] << " " << shape.actor[2] << " half " << shape.half[0]
						<< " " << shape.half[1] << " " << shape.half[2] << " axes " << shape.axes[0] << " " << shape.axes[1] << " " << shape.axes[2]
						<< " / " << shape.axes[3] << " " << shape.axes[4] << " " << shape.axes[5] << " / " << shape.axes[6] << " " << shape.axes[7]
						<< " " << shape.axes[8] << "\n";
				if (std::getenv("KNAV_TRACE_VERTS"))
				{
					for (std::size_t v = 0; v + 2 < shape.vertices.size(); v += 3)
						std::cout << "      v " << shape.vertices[v] << " " << shape.vertices[v + 1] << " " << shape.vertices[v + 2] << "\n";
				}
			}
		}
	}
	// The mask hulls over the target: how far outside each plane it is (positive = clipped).
	for (const knav::AreaBuilding& building : area.buildings)
	{
		for (std::size_t i = 0; building.shapes && i < building.shapes->size(); ++i)
		{
			const kworld::WorldShape& shape = (*building.shapes)[i];
			if (!shape.keepInside || at[0] < shape.min[0] || at[0] > shape.max[0] || at[2] < shape.min[2] || at[2] > shape.max[2])
				continue;
			std::cout << "mask of " << building.id << " at " << building.placed.x << " " << building.placed.y << " " << building.placed.z
				<< " scale " << building.placed.scale << ", shape " << i << " y " << shape.min[1] << ".." << shape.max[1] << ":";
			for (const auto& plane : *shape.keepInside)
				std::cout << " " << plane[0] * at[0] + plane[1] * at[1] + plane[2] * at[2] - plane[3];
			std::cout << "\n";
			break;
		}
	}
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
int bakeWorld(const std::string& dataDir, const std::string& dir, const int (&regions)[4], int threads, float radius, bool prune,
	const std::optional<knav::Point>& trace)
{
	std::filesystem::create_directories(dir);
	const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
	const auto start = Clock::now();
	knav::AreaGatherer gatherer(dataDir, terrain);
	const std::vector<knav::Point> seedsDef = prune ? knav::readSeedsDef(knav::native::navtilesDir(dataDir)) : std::vector<knav::Point>{};
	std::cout << "load: " << secondsSince(start) << " s\n";

	std::size_t tiles = 0;
	std::size_t polys = 0;
	std::uintmax_t bytes = 0;
	double gatherSeconds = 0.0;
	double bakeSeconds = 0.0;
	for (int rz = regions[1]; rz <= regions[3]; ++rz)
	{
		for (int rx = regions[0]; rx <= regions[2]; ++rx)
		{
			const kworld::ZoneCell min{ rx * knav::kRegionZones, rz * knav::kRegionZones };
			const kworld::ZoneCell max{ min.x + knav::kRegionZones - 1, min.z + knav::kRegionZones - 1 };
			auto step = Clock::now();
			const knav::AreaGeometry area = gatherer.gather(min, max);
			const double gathered = secondsSince(step);

			step = Clock::now();
			knav::NavConfig config = knav::configForWorldZones(min, max);
			config.agentRadius = radius;
			knav::NavWorld world(config, terrain, area.foliage);
			for (const knav::AreaBuilding& building : area.buildings)
				world.setBuilding(building.id, building.shapes, building.nav);
			if (prune)
				setSeeds(world, seedsDef, terrain, area, min, max);
			world.bakeAll(threads);
			const double baked = secondsSince(step);
			if (trace)
				traceSeeds(world.mesh(), *trace, seedsDef, terrain, area, min, max);

			const std::string path = dir + "/" + knav::regionFileName(rx, rz);
			std::filesystem::remove(path); // may be a read-only hard link into the bake cache: never write through it
			world.mesh().save(path);
			tiles += world.mesh().tiles().size();
			polys += world.mesh().polyCount();
			bytes += std::filesystem::file_size(path);
			gatherSeconds += gathered;
			bakeSeconds += baked;
			std::cout << path << ": " << world.mesh().tiles().size() << " tiles, " << world.mesh().polyCount()
				<< " polys, gather " << std::fixed << std::setprecision(1) << gathered << " s, bake " << baked
				<< " s\n  " << areaTotals(world.mesh()) << "\n" << std::flush;
		}
	}
	std::cout << "total: " << tiles << " tiles, " << polys << " polys, " << bytes << " bytes, gather "
		<< gatherSeconds << " s, bake " << bakeSeconds << " s on " << threads << " thread(s)\n";
	knav::printBakeProfile(std::cout);
	return 0;
}

// The Hub-sized single-file bake.
int bakeSingle(const std::string& dataDir, const std::string& out, kworld::ZoneCell min, kworld::ZoneCell max, int threads, float radius,
	bool prune)
{
	const auto seconds = secondsSince;
auto start = Clock::now();
	const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
	const knav::AreaGeometry area = knav::gatherArea(dataDir, terrain, min, max);
	std::cout << "gather: " << seconds(start) << " s, " << area.buildings.size() << " buildings, "
		<< area.foliage->size() << " foliage shapes\n";

	knav::NavConfig config = knav::configForZones(min, max);
	config.agentRadius = radius;
	start = Clock::now();
	knav::NavWorld world(config, terrain, area.foliage);
	for (const knav::AreaBuilding& building : area.buildings)
		world.setBuilding(building.id, building.shapes, building.nav);
	if (prune)
		setSeeds(world, knav::readSeedsDef(knav::native::navtilesDir(dataDir)), terrain, area, min, max);
	world.bakeAll(threads);
	const double bakeSeconds = seconds(start);
	const knav::NavMesh& mesh = world.mesh();
	std::filesystem::remove(out); // may be a read-only hard link into the bake cache: never write through it
	mesh.save(out);

	const auto& c = area.coverage;
	std::cout << "bake: " << bakeSeconds << " s on " << threads << " thread(s)\n"
		<< "tiles: " << mesh.tiles().size() << " of " << config.tilesX * config.tilesZ
		<< ", polys: " << mesh.polyCount() << ", cache: " << std::filesystem::file_size(out)
		<< " bytes (" << out << ")\n"
		<< "building placements: " << c.placements << ": " << c.placementsWithCollision
		<< " with collision, " << c.placementsRenderOnly << " render mesh only, "
		<< c.placementsNoGeometry << " no geometry, " << c.placementsMissingTarget << " missing target\n"
		<< "parts: " << c.parts << ": " << c.partsWithCollision << " collision, "
		<< c.partsRenderOnly << " render only (skipped), " << c.partsNoGeometry << " none, "
		<< c.partsFlagged << " passable/no-collision, " << c.partsFileUnreadable << " unreadable file, "
		<< c.partsRuinRemoved << " ruin upper floors\n"
		<< "path modes: ignore " << c.pathModes[0] << ", projected " << c.pathModes[1]
		<< ", obstacle " << c.pathModes[2] << ", walkable " << c.pathModes[3]
		<< "; gateways " << c.gatewayPlacements << "\n"
		<< "doors: " << c.doors << " placed, " << c.doorsWithoutCollision << " without collision; "
		<< mesh.doorKeys().size() << " have polygons in the mesh\n"
		<< "foliage instances: " << c.foliageInstances << ", with collision: " << c.foliageWithCollision << " (walkable " << c.foliageWalkable
		<< "), navmesh cutters " << c.foliageCutters << "\n"
		<< "interior furniture: " << c.furniture << " pieces, " << c.furnitureWithCollision << " with collision\n";
	knav::printBakeProfile(std::cout);
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	std::string out = "hub.knav";
	std::string worldDir;
	int regions[4] = { 0, 0, kworld::kZoneCellsPerAxis / knav::kRegionZones - 1, kworld::kZoneCellsPerAxis / knav::kRegionZones - 1 };
	int threads = 4;
	float radius = knav::NavConfig{}.agentRadius;
	bool prune = true;
	std::string cacheRoot = std::getenv("KNAV_BAKE_CACHE") ? std::getenv("KNAV_BAKE_CACHE") : KNAV_BAKE_CACHE_DEFAULT;
	std::optional<knav::Point> trace;
	knav::BakeTuning tuning;
	kworld::ZoneCell min{ 19, 31 };
	kworld::ZoneCell max{ 21, 33 };
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--out" && i + 1 < argc)
			out = argv[++i];
		else if (arg == "--threads" && i + 1 < argc)
			threads = std::atoi(argv[++i]);
		else if (arg == "--radius" && i + 1 < argc)
			radius = static_cast<float>(std::atof(argv[++i]));
		else if (arg == "--trace" && i + 3 < argc)
		{
			trace = knav::Point{ static_cast<float>(std::atof(argv[i + 1])), static_cast<float>(std::atof(argv[i + 2])),
				static_cast<float>(std::atof(argv[i + 3])) };
			i += 3;
		}
		else if (arg == "--cache" && i + 1 < argc)
			cacheRoot = argv[++i];
		else if (arg == "--no-cache")
			cacheRoot.clear();
		else if (arg == "--no-prune")
			prune = false;
		else if (arg == "--no-flat")
			tuning.flatTiles = false;
		else if (arg == "--ledge" && i + 1 < argc)
		{
			const std::string name = argv[++i];
			tuning.ledge = name == "off" ? knav::BakeLedge::Off : name == "drops" ? knav::BakeLedge::Drops : knav::BakeLedge::Recast;
		}
		else if (arg == "--partition" && i + 1 < argc)
		{
			const std::string name = argv[++i];
			if (name == "watershed")
				tuning.partition = knav::BakePartition::Watershed;
			else if (name == "monotone")
				tuning.partition = knav::BakePartition::Monotone;
			else if (name == "layers")
				tuning.partition = knav::BakePartition::Layers;
			else
			{
				std::cerr << "knav_bake: unknown partition " << name << "\n";
				return 2;
			}
		}
		else if (arg == "--detail-cells" && i + 1 < argc)
			tuning.detailSampleCells = static_cast<float>(std::atof(argv[++i]));
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
				"       --radius R  erosion radius (default 4; the original bakes at 0 and keeps clearance at query time)\n"
				"       --no-prune  keep the regions no seed reaches (roofs, sealed rooms; knav/seeds.hpp)\n"
				"       --trace X Y Z  (world bake) list the seeds that keep the surface nearest that point\n"
				"  speed switches (default: the reference bake, flat tiles on):\n"
				"       --no-flat  --partition watershed|monotone|layers  --detail-cells N (0 = off)  --ledge recast|drops|off\n"
				"  KNAV_PROFILE=1 prints per-stage CPU time\n";
			return 2;
		}
	}

	try
	{
		knav::setBakeTuning(tuning);
		const std::string dataDir = KENSHI_DATA;

#ifdef _WIN32
		if (!cacheRoot.empty())
			std::cerr << "knav_bake: the result cache is POSIX-only; baking uncached\n";
#else
		if (!cacheRoot.empty() && !trace)
		{
			// Thread count and the output path do not change the bytes (knav_bake_threads).
			std::ostringstream key;
			key << "recipe " << kBakeRecipeVersion << "\nsrc " << KNAV_BAKE_SRC_HASH << "\ndata " << dataDir << " "
				<< knavcache::dataStamp(dataDir) << "\nradius " << radius << " prune " << prune << "\ntuning " << tuning.flatTiles << " "
				<< static_cast<int>(tuning.ledge) << " " << static_cast<int>(tuning.partition) << " " << tuning.detailSampleCells << "\n";
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
					return worldDir.empty() ? bakeSingle(dataDir, (data / "bake.knav").string(), min, max, threads, radius, prune)
						: bakeWorld(dataDir, data.string(), regions, threads, radius, prune, trace);
				},
				[&](const std::filesystem::path& data) {
					if (!worldDir.empty())
						knavcache::linkTree(data, worldDir);
					else
						knavcache::linkFile(data / "bake.knav", out);
				});
		}
#endif
		return worldDir.empty() ? bakeSingle(dataDir, out, min, max, threads, radius, prune)
			: bakeWorld(dataDir, worldDir, regions, threads, radius, prune, trace);
	} catch (const std::exception& e) {
		std::cerr << "knav_bake: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
