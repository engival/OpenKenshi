// World island test (kenshi-k16.7.40, kenshi-k16.7.43; docs/formats/NAVMESH_CACHE.md "Island test"): walkable regions of the
// runtime mesh that the open world cannot reach. The mesh is the one a playtest loads (MeshNavigator::openNative over KENSHI_DATA:
// shipped tiles, stale zones rebaked with our recipe), streamed through in blocks of 5 x 5 zones (one region plus a zone of overlap
// east and south, so every pair of adjacent zones is in some block). Per block the Detour links give connected polygon sets
// (knav::PolyComponents, every area allowed, doors open); a world-wide union-find over (tile, polygon) joins the sets of the blocks.
// Then:
//  - the main component is the one of the largest area (the open world); every other component is an island;
//  - a polygon is "clipped" when its centre lies more than kClipDepth under the terrain heightmap: it is a bake artefact under the
//    ground, and its area does not count towards an island;
//  - an island whose area (clipped polygons left out) is under kMinArea is dropped; so is one whose dry area (not Shallow / Swim
//    water) is: a sheet of sea or world-edge ocean is swum, not a place to be stuck in;
//  - the rest are candidates, ranked by area into <out>/nav_islands.tsv. Each carries its source: a polygon in a zone the loader
//    rebaked with our recipe (nativeReport: the stored hash differed, or could not be verified) is ours, one in a shipped zone is the
//    original's own mesh (mixed when the island spans both);
//  - gated: those inside a building's interior mask hull (Simulation::buildingAt's rule, docs/reference/INTERIORS.md; the whole
//    footprint for a maskless building with a door: a wall's collision box over terrain is not a building), and open-world ones over
//    kOpenGate (the rest are terrain pockets behind cliffs: report only). A gated one must be in the baseline
//    (tests/data/nav_islands_baseline.tsv) or the test fails. A baseline line is keyed by owner record, floor height (8 u) and area
//    (a factor of 1.3), not by centroid; one line covers one island (kenshi-k16.7.66).
// argv: [1] the output directory (the report and the nav cache tree live there), default ".";
//       [2] the baseline file, default the checked-in one;
//       `--zones X0 Z0 X1 Z1`: only the blocks that hold these zones (a window; the test then gates nothing but --expect[-absent]);
//       `--expect X Z R` anywhere after: instead of the baseline, fail unless a candidate lies within R of (X, Z) (the self-check:
//       the Heft Longhouse on a build without k16.6.229's recipe 5);
//       `--expect-absent X Z R A`: fail if a candidate of A u2 or more lies within R of (X, Z) (the self-check: Outpost s-IV's floor
//       2, a 38,066 u2 island before k16.6.260's selectParts chance-0 rule and connected after it).
// The walk runs in 4 threads (bands of block rows, one navigator each) and the graphs are merged (kenshi-k16.7.155: 324 s -> 121 s).
// Cold (no nav cache) and cached wall times and the peak RSS are printed. Exit 77 without the navtiles.

#include "ksim/nav/navigator.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/collision_area.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/loadorder.hpp"

#include "knav/native.hpp"
#include "knav/navmesh.hpp"
#include "knav/travel.hpp"

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

constexpr double kDefaultMinArea = 10000.0; // u^2: one room's floor (~100 m^2), start value; `--min-area A` overrides (a probe)
double kMinArea = kDefaultMinArea;
constexpr double kOpenGate = 1.0e6; // u^2: an open-world island this big is gated too (smaller ones are terrain pockets: reported only)
constexpr double kClipDepth = 40.0; // u under the terrain at the polygon's centre
constexpr double kBaselineFloorTol = 8.0; // a baseline line matches an island of the same owner whose floor height is this near (u)
constexpr double kBaselineAreaBand = 1.3; // ... and whose area is within this factor (a band: a recipe change moves an island's rim, not its size class)
constexpr double kHullTerrainDepth = 15.0; // u: an island this far under the owner's base is terrain under its hull, not a floor
constexpr double kBuildingHeightSlack = 3.0; // Simulation::buildingAt's height tolerance (ksim/combat/carry.cpp)
constexpr int kBlockZones = 5;
constexpr double kGridCell = 64.0;

double msSince(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::size_t statusKb(const char* key)
{
	std::ifstream status("/proc/self/status");
	std::string line;
	const std::string prefix = std::string(key) + ":";
	while (std::getline(status, line))
		if (line.rfind(prefix, 0) == 0)
			return static_cast<std::size_t>(std::stoul(line.substr(prefix.size())));
	return 0;
}

struct PolyInfo
{
	float area = -1.0f; // < 0: not in the graph (flags allow nothing)
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	int tx = 0; // the Detour tile: the loader's report names the tiles it replaced with our bake
	int tz = 0;
	bool clipped = false;
	bool dry = false; // Land, Edge or a door: ground a walker stands on (not Shallow / Swim water)
	float wallDist = 1e30f; // xz distance from the centroid to the polygon's nearest wall edge (no neighbour): the --oracle's interior test
};

// The world-wide graph: a dense id per (tile, polygon), seen the first time any block loads the tile.
struct World
{
	std::unordered_map<std::uint64_t, std::uint32_t> tileBase;
	std::vector<std::uint32_t> parent;
	std::vector<PolyInfo> info;

	std::uint32_t find(std::uint32_t a)
	{
		while (parent[a] != a)
		{
			parent[a] = parent[parent[a]];
			a = parent[a];
		}
		return a;
	}
	void join(std::uint32_t a, std::uint32_t b)
	{
		a = find(a);
		b = find(b);
		if (a != b)
			parent[std::max(a, b)] = std::min(a, b);
	}
};

std::uint64_t tileKey(int x, int z)
{
	return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z);
}

// Adds the block's polygons and joins what its Detour links join.
void absorb(World& world, const knav::NavMesh& mesh, const kworld::Heightmap& terrain)
{
	const std::uint16_t flags = knav::TravelFilter{}.allowed;
	const knav::PolyComponents components(mesh, flags);
	const dtNavMesh& nav = mesh.detour();
	std::unordered_map<std::uint32_t, std::uint32_t> first; // block label -> world id of its first polygon
	for (int i = 0; i < nav.getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = nav.getTile(i);
		if (tile->header == nullptr)
			continue;
		const int tx = tile->header->x;
		const int tz = tile->header->y;
		const auto [at, fresh] = world.tileBase.try_emplace(tileKey(tx, tz), static_cast<std::uint32_t>(world.parent.size()));
		const std::uint32_t base = at->second;
		if (fresh)
		{
			world.parent.resize(world.parent.size() + static_cast<std::size_t>(tile->header->polyCount));
			std::iota(world.parent.begin() + base, world.parent.end(), base);
			world.info.resize(world.parent.size());
			for (int p = 0; p < tile->header->polyCount; ++p)
			{
				const dtPoly& poly = tile->polys[p];
				PolyInfo& out = world.info[base + static_cast<std::uint32_t>(p)];
				out.tx = tx;
				out.tz = tz;
				if ((poly.flags & flags) == 0)
					continue;
				out.area = 0.0f; // an off-mesh link joins, it has no floor
				if (poly.getType() != DT_POLYTYPE_GROUND)
					continue;
				double sx = 0.0, sy = 0.0, sz = 0.0, twice = 0.0;
				for (int v = 0; v < poly.vertCount; ++v)
				{
					const float* a = &tile->verts[poly.verts[v] * 3];
					const float* b = &tile->verts[poly.verts[(v + 1) % poly.vertCount] * 3];
					sx += a[0];
					sy += a[1];
					sz += a[2];
					twice += static_cast<double>(a[0]) * b[2] - static_cast<double>(b[0]) * a[2];
				}
				out.area = static_cast<float>(std::fabs(twice) * 0.5);
				out.x = static_cast<float>(sx / poly.vertCount);
				out.y = static_cast<float>(sy / poly.vertCount);
				out.z = static_cast<float>(sz / poly.vertCount);
				out.dry = poly.getArea() != static_cast<unsigned char>(knav::Area::Shallow) && poly.getArea() != static_cast<unsigned char>(knav::Area::Swim);
				out.clipped = out.y < terrain.terrainHeight(out.x, out.z) - kClipDepth;
				for (int v = 0; v < poly.vertCount; ++v)
				{
					bool wall = poly.neis[v] == 0;
					if (!wall && (poly.neis[v] & DT_EXT_LINK) != 0) // a tile-border edge is a wall unless a link crosses it
					{
						wall = true;
						for (unsigned int l = poly.firstLink; l != DT_NULL_LINK && wall; l = tile->links[l].next)
							wall = tile->links[l].edge != v;
					}
					if (!wall)
						continue;
					const float* a = &tile->verts[poly.verts[v] * 3];
					const float* b = &tile->verts[poly.verts[(v + 1) % poly.vertCount] * 3];
					const double ex = b[0] - a[0], ez = b[2] - a[2];
					const double len2 = ex * ex + ez * ez;
					const double t = len2 > 0.0 ? std::clamp(((out.x - a[0]) * ex + (out.z - a[2]) * ez) / len2, 0.0, 1.0) : 0.0;
					out.wallDist = std::min(out.wallDist, static_cast<float>(std::hypot(out.x - (a[0] + t * ex), out.z - (a[2] + t * ez))));
				}
			}
		}
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			const std::uint32_t label = components.at(tx, tz, p);
			if (label == knav::PolyComponents::kNone)
				continue;
			const std::uint32_t id = base + static_cast<std::uint32_t>(p);
			const auto [slot, isFirst] = first.try_emplace(label, id);
			if (!isFirst)
				world.join(slot->second, id);
		}
	}
}

struct Island
{
	std::uint32_t root = 0;
	std::size_t polys = 0;
	double area = 0.0; // clipped polygons left out
	double clippedArea = 0.0;
	double dryArea = 0.0; // of `area`: not Shallow or Swim water
	double cx = 0.0, cy = 0.0, cz = 0.0; // area-weighted centroid of the unclipped polygons
	float ymin = 1e30f, ymax = -1e30f;
	float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
};

// --oracle (kenshi-k16.7.53): a gated island's floor sampled in the original's own mesh. The runtime mesh replaces a stale
// zone's tiles with our bake; this asks the shipped tile (MeshNavigator, NativeCacheOptions::shippedOnly) what lies at the same
// points: a polygon within kTightY of the point's height, in the main component or an island of the shipped world, or none.
struct OracleProbe
{
	std::size_t rank = 0; // into the candidates
	bool interior = true; // false: no polygon of the island is kOracleBand deep, the samples are all of its polygons
	std::size_t deepPolys = 0;
	std::vector<std::array<float, 3>> points;
};
constexpr float kOracleTightY = 8.0f; // the first oracle's height rule (k16.7.53), still reported as the verdict_8u column
float oracleBandY = 16.0f; // --oracle-band: a shipped polygon this near the sample's height is "the same floor": half the 32-34 u storey spacing of the 3793 building (k16.7.56)
constexpr float kOracleLooseY = 60.0f; // anything this near in y: reported as "something shipped, at another height"
constexpr std::size_t kOracleSamples = 96;
constexpr float kOracleBand = 4.0f; // samples sit at least this far (xz, centroid to the polygon's own wall edges) inside the island (kenshi-k16.7.57)
constexpr float kOracleXz = 1.5f; // the footprint: about the point itself, so a wall's far side is not "the same floor"

// A line is keyed by what the island is, not where its centroid falls (kenshi-k16.7.66: a recipe change drifted centroids past the old 60 u
// match): the owner record (a building's id, or "open world@ZX,ZZ" in the zone of the centroid), its floor height (above the placement's y;
// the absolute height in the open world) and its area band. One line matches one island: a second island of the same key is NEW.
struct BaselineEntry
{
	std::string record;
	double floorY = 0.0, area = 0.0;
	double x = 0.0, z = 0.0; // where it was seen when the line was written: for a human, not matched
	std::string source; // shipped | rebaked | mixed | shipped?
	std::string status; // accepted | pending
	std::string why;
};

std::vector<BaselineEntry> readBaseline(const std::string& path)
{
	std::vector<BaselineEntry> entries;
	std::ifstream in(path);
	test::require(static_cast<bool>(in), "cannot open the baseline " + path);
	std::string line;
	while (std::getline(in, line))
	{
		if (line.empty() || line[0] == '#')
			continue;
		std::istringstream fields(line);
		BaselineEntry entry;
		std::string floorY, area, x, z;
		std::getline(fields, entry.record, '\t');
		std::getline(fields, floorY, '\t');
		std::getline(fields, area, '\t');
		std::getline(fields, x, '\t');
		std::getline(fields, z, '\t');
		std::getline(fields, entry.source, '\t');
		std::getline(fields, entry.status, '\t');
		std::getline(fields, entry.why);
		entry.floorY = std::stod(floorY);
		entry.area = std::stod(area);
		entry.x = std::stod(x);
		entry.z = std::stod(z);
		test::require(entry.status == "accepted" || entry.status == "bug" || entry.status == "pending", "baseline status must be accepted, bug or pending: " + line);
		entries.push_back(entry);
	}
	return entries;
}

// The extent of a list of shapes along the placement's own axes (ax, az) on the plane and in y: Simulation::buildingAt's
// oriented box (ksim/world/world.cpp addBuildingSite), doors left out.
struct Bounds
{
	double minA = 1e30, maxA = -1e30, minB = 1e30, maxB = -1e30, minY = 1e30, maxY = -1e30;
};

Bounds boundsOf(const std::vector<kworld::WorldShape>& shapes, const kworld::PlacedInstance& instance, double ax, double az)
{
	Bounds bounds;
	for (const kworld::WorldShape& shape : shapes)
	{
		if (shape.use == kworld::NavUse::Door)
			continue;
		for (std::size_t i = 0; i + 2 < shape.vertices.size(); i += 3)
		{
			const double dx = shape.vertices[i] - instance.x;
			const double dz = shape.vertices[i + 2] - instance.z;
			const double a = dx * ax + dz * az;
			const double b = -dx * az + dz * ax;
			bounds.minA = std::min(bounds.minA, a);
			bounds.maxA = std::max(bounds.maxA, a);
			bounds.minB = std::min(bounds.minB, b);
			bounds.maxB = std::max(bounds.maxB, b);
			bounds.minY = std::min(bounds.minY, static_cast<double>(shape.vertices[i + 1]));
			bounds.maxY = std::max(bounds.maxY, static_cast<double>(shape.vertices[i + 1]));
		}
	}
	return bounds;
}

// The placement's local x axis on the plane (the yaw the sites and the bake use). False for a degenerate rotation.
bool placementAxis(const kworld::PlacedInstance& instance, double& ax, double& az)
{
	const double w = instance.qw, x = instance.qx, y = instance.qy, z = instance.qz;
	ax = 1 - 2 * (y * y + z * z);
	az = 2 * (x * z - w * y);
	const double length = std::hypot(ax, az);
	if (length < 1.0e-6)
		return false;
	ax /= length;
	az /= length;
	return true;
}

// One building the island's centroid can be inside: its interior mask hull (the mask's collision, bounds about the placement's
// axes), else the whole footprint of a building with a door (a maskless building whose collision box is a wall would otherwise
// claim the terrain behind it).
struct OwnerBox
{
	std::string record;
	std::string placement;
	double cx = 0.0, cz = 0.0;
	double axisX = 1.0, axisZ = 0.0;
	double halfA = 0.0, halfB = 0.0;
	double minY = 0.0, maxY = 0.0;
	double base = 0.0; // the placement's y: the report's floor height is measured above it
	double top = 0.0; // the top of the building's own collision: an island at it is a roof, not a floor
	double area = 0.0; // halfA x halfB: the smallest box wins, as in buildingAt
};

struct Owner
{
	std::string record;
	std::string placement;
	double floorY = 0.0; // the island's height above the placement's y
	double topY = 0.0; // the building's own top, above the placement's y
	bool found = false;
};

// The source of an island's floor polygons, from the loader's own decisions: a polygon in a tile the loader replaced with our
// bake (nativeReport: a rebaked zone's whole square, an interior's own square in a fresh zone) is ours; in a shipped tile it is
// the original's own mesh.
struct Source
{
	std::size_t shipped = 0;
	std::size_t rebaked = 0;
	std::size_t unknown = 0;
	std::size_t interiorRebakedZones = 0; // shipped zones whose interiors alone were rebaked (reported apart)
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_nav_islands: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		std::string outDir = ".";
		std::string baselinePath = KSIM_ISLANDS_BASELINE;
		bool expect = false;
		double expectX = 0.0, expectZ = 0.0, expectR = 0.0;
		bool expectAbsent = false;
		double absentX = 0.0, absentZ = 0.0, absentR = 0.0, absentArea = 0.0;
		int positional = 0;
		std::string writeBaseline; // --write-baseline FILE: the gated islands not in the baseline, as pending lines
		int zoneBox[4] = { 0, 0, kworld::kZoneCellsPerAxis - 1, kworld::kZoneCellsPerAxis - 1 }; // --zones X0 Z0 X1 Z1: a window of the world
		for (int i = 1; i < argc; ++i)
		{
			if (std::string(argv[i]) == "--expect" && i + 3 < argc)
			{
				expect = true;
				expectX = std::stod(argv[i + 1]);
				expectZ = std::stod(argv[i + 2]);
				expectR = std::stod(argv[i + 3]);
				i += 3;
			}
			else if (std::string(argv[i]) == "--expect-absent" && i + 4 < argc)
			{
				expectAbsent = true;
				absentX = std::stod(argv[i + 1]);
				absentZ = std::stod(argv[i + 2]);
				absentR = std::stod(argv[i + 3]);
				absentArea = std::stod(argv[i + 4]);
				i += 4;
			}
			else if (std::string(argv[i]) == "--write-baseline" && i + 1 < argc)
			{
				writeBaseline = argv[++i];
			}
			else if (std::string(argv[i]) == "--oracle")
				throw std::runtime_error("--oracle is retired: it compared against the shipped navtiles, which are no longer loaded (kenshi-k16.7.189)");
			else if (std::string(argv[i]) == "--oracle-band" && i + 1 < argc)
			{
				oracleBandY = std::stof(argv[++i]);
			}
			else if (std::string(argv[i]) == "--min-area" && i + 1 < argc)
			{
				kMinArea = std::stod(argv[++i]);
			}
			else if (std::string(argv[i]) == "--zones" && i + 4 < argc)
			{
				for (int k = 0; k < 4; ++k)
					zoneBox[k] = std::stoi(argv[i + 1 + k]);
				i += 4;
			}
			else if (positional++ == 0)
				outDir = argv[i];
			else
				baselinePath = argv[i];
		}
		std::filesystem::create_directories(outDir);

		const auto start = std::chrono::steady_clock::now();
		const auto terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gameData = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		ksim::NativeCacheOptions cache;
		cache.dir = outDir + "/navcache";
		cache.worldKey = kworld::dataHash(order);
		cache.keepTrees = 1; // this test's own tree dir: a dead recipe's tree is ~3.4 GB (kenshi-k16.6.814)
		// ---- the walk graph over the whole world ----
		// The blocks of the window: `onBlock` runs with the zones resident (the mesh is the navigator's now). Block rows [row0, row1).
		std::mutex outMutex;
		const auto walkBlocks = [&](ksim::MeshNavigator& walker, World& graph, int row0, int row1, const auto& onBlock)
		{
			std::size_t count = 0;
			for (int bz = row0; bz < row1 && bz * 4 < kworld::kZoneCellsPerAxis; ++bz)
			{
				for (int bx = 0; bx * 4 < kworld::kZoneCellsPerAxis; ++bx)
				{
					std::vector<kworld::ZoneCell> zones;
					for (int dz = 0; dz < kBlockZones; ++dz)
						for (int dx = 0; dx < kBlockZones; ++dx)
						{
							const kworld::ZoneCell zone{ bx * 4 + dx, bz * 4 + dz };
							if (zone.x >= zoneBox[0] && zone.z >= zoneBox[1] && zone.x <= zoneBox[2] && zone.z <= zoneBox[3]
								&& zone.x < kworld::kZoneCellsPerAxis && zone.z < kworld::kZoneCellsPerAxis && walker.canCover(zone))
								zones.push_back(zone);
						}
					if (zones.empty())
						continue;
					walker.setCoverage(zones);
					absorb(graph, walker.mesh(), terrain);
					onBlock(walker.mesh(), zones);
					++count;
				}
				const std::lock_guard<std::mutex> lock(outMutex);
				std::cout << "row " << bz << ": " << graph.parent.size() << " polygons (its band), " << std::llround(msSince(start) / 1000.0) << " s, RSS "
						  << statusKb("VmRSS") / 1024 << " MB" << std::endl;
			}
			return count;
		};
		// The world is walked in kBands bands of block rows, one thread each with its own navigator and graph (the zone hash and the
		// cache reads are most of the time, and a navigator is not thread safe); the graphs are merged below. Every pair of adjacent
		// zones is in some block of one band: a block holds its own zones and one zone east and south.
		struct Band
		{
			std::shared_ptr<ksim::MeshNavigator> nav;
			std::vector<std::string> log;
			World world;
			std::size_t blocks = 0;
		};
		constexpr int kBands = 4;
		constexpr int kRows = kworld::kZoneCellsPerAxis / 4;
		std::vector<Band> bands(kBands);
		std::vector<std::thread> threads;
		std::vector<std::string> failures(kBands);
		for (int t = 0; t < kBands; ++t)
			threads.emplace_back([&, t]()
			{
				try
				{
					Band& band = bands[t];
					band.nav = ksim::MeshNavigator::openNative(KENSHI_DATA, { { 20, 32 } },
						[&band](const std::string& line) { band.log.push_back(line); }, gameData.get(), cache);
					band.blocks = walkBlocks(*band.nav, band.world, t * kRows / kBands, (t + 1) * kRows / kBands,
						[](const knav::NavMesh&, const std::vector<kworld::ZoneCell>&) {});
				} catch (const std::exception& e) {
					failures[t] = e.what();
				}
			});
		for (std::thread& thread : threads)
			thread.join();
		for (int t = 0; t < kBands; ++t)
			test::require(failures[t].empty(), "band " + std::to_string(t) + ": " + failures[t]);
		// Merge: a tile in two bands (the seam rows) is one tile; a band's polygon sets join the world's.
		World world;
		std::size_t blocks = 0;
		for (Band& band : bands)
		{
			blocks += band.blocks;
			const World& local = band.world;
			std::vector<std::uint32_t> globalOf(local.parent.size());
			for (std::uint32_t id = 0; id < local.parent.size();)
			{
				const PolyInfo& first = local.info[id];
				std::uint32_t end = id;
				while (end < local.parent.size() && local.info[end].tx == first.tx && local.info[end].tz == first.tz)
					++end;
				const auto [at, fresh] = world.tileBase.try_emplace(tileKey(first.tx, first.tz), static_cast<std::uint32_t>(world.parent.size()));
				if (fresh)
				{
					world.parent.resize(world.parent.size() + (end - id));
					std::iota(world.parent.begin() + at->second, world.parent.end(), at->second);
					world.info.insert(world.info.end(), local.info.begin() + id, local.info.begin() + end);
				}
				for (std::uint32_t k = id; k < end; ++k)
					globalOf[k] = at->second + (k - id);
				id = end;
			}
			World& mutableLocal = band.world;
			for (std::uint32_t id = 0; id < local.parent.size(); ++id)
			{
				const std::uint32_t root = mutableLocal.find(id);
				if (root != id)
					world.join(globalOf[id], globalOf[root]);
			}
			band.world = World{}; // the merged graph is the one kept
		}
		// A zone's decision lives in the band that loaded it.
		const auto nativeReportOf = [&](kworld::ZoneCell zone) -> const ksim::MeshNavigator::NativeTileReport*
		{
			for (const Band& band : bands)
				if (const auto* report = band.nav->nativeReport(zone))
					return report;
			return nullptr;
		};
		std::size_t regionHits = 0, regionMisses = 0, zoneHits = 0, zoneMisses = 0;
		for (const Band& band : bands)
		{
			for (const std::string& line : band.log)
				std::cout << "loader: " << line << "\n";
			regionHits += band.nav->nativeCacheStats().regionHits;
			regionMisses += band.nav->nativeCacheStats().regionMisses;
			zoneHits += band.nav->nativeCacheStats().zoneHits;
			zoneMisses += band.nav->nativeCacheStats().zoneMisses;
		}
		if (const auto* report = nativeReportOf({ 46, 16 }))
			std::cout << "zone 46,16 (the Heft Longhouse): " << (report->decision.choice == ksim::MeshNavigator::NativeChoice::Shipped ? "shipped tile" : "rebaked with our recipe")
					  << (report->decision.reason.empty() ? "" : " (" + report->decision.reason + ")") << "\n";
		const double walkMs = msSince(start);
		std::cout << "graph: " << blocks << " blocks, " << world.tileBase.size() << " tiles, " << world.parent.size() << " polygons, "
				  << std::llround(walkMs / 1000.0) << " s, peak RSS " << statusKb("VmHWM") / 1024 << " MB; nav cache: "
				  << regionHits << " region hits / " << regionMisses << " misses, " << zoneHits << " zone hits / " << zoneMisses << " misses\n";

		// ---- components ----
		std::map<std::uint32_t, Island> byRoot;
		for (std::uint32_t id = 0; id < world.parent.size(); ++id)
		{
			const PolyInfo& poly = world.info[id];
			if (poly.area < 0.0f)
				continue;
			Island& island = byRoot[world.find(id)];
			island.root = world.find(id);
			++island.polys;
			if (poly.clipped)
			{
				island.clippedArea += poly.area;
				continue;
			}
			island.area += poly.area;
			if (poly.dry)
				island.dryArea += poly.area;
			island.cx += static_cast<double>(poly.x) * poly.area;
			island.cy += static_cast<double>(poly.y) * poly.area;
			island.cz += static_cast<double>(poly.z) * poly.area;
			island.ymin = std::min(island.ymin, poly.y);
			island.ymax = std::max(island.ymax, poly.y);
			island.x0 = std::min(island.x0, poly.x);
			island.x1 = std::max(island.x1, poly.x);
			island.z0 = std::min(island.z0, poly.z);
			island.z1 = std::max(island.z1, poly.z);
		}
		std::vector<Island> all;
		for (auto& [root, island] : byRoot)
		{
			if (island.area > 0.0)
			{
				island.cx /= island.area;
				island.cy /= island.area;
				island.cz /= island.area;
			}
			all.push_back(island);
		}
		std::sort(all.begin(), all.end(), [](const Island& a, const Island& b) { return a.area > b.area; });
		test::require(!all.empty(), "no walkable polygon in the world");
		const Island main = all.front();
		std::size_t belowMin = 0, clippedOnly = 0, waterOnly = 0;
		std::vector<Island> candidates;
		for (std::size_t i = 1; i < all.size(); ++i)
		{
			if (all[i].area <= 0.0)
				++clippedOnly;
			else if (all[i].dryArea < kMinArea)
				++(all[i].area < kMinArea ? belowMin : waterOnly);
			else
				candidates.push_back(all[i]);
		}
		std::cout << "components: " << all.size() << "; main " << std::llround(main.area / 1e6) << "e6 u2 (" << main.polys << " polygons); islands dropped: "
				  << clippedOnly << " entirely clipped under the terrain, " << waterOnly << " water only (dry area under the threshold), " << belowMin << " under " << kMinArea << " u2; candidates: "
				  << candidates.size() << "\n";

		// ---- nearest main polygon: a 64 u grid over the main component's polygon centres ----
		std::vector<std::pair<std::uint64_t, std::uint32_t>> grid;
		const auto cellKey = [](double x, double z)
		{
			return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(x / kGridCell)))) << 32)
				| static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(z / kGridCell)));
		};
		for (std::uint32_t id = 0; id < world.parent.size(); ++id)
			if (world.info[id].area > 0.0f && !world.info[id].clipped && world.find(id) == main.root)
				grid.push_back({ cellKey(world.info[id].x, world.info[id].z), id });
		std::sort(grid.begin(), grid.end());
		std::map<std::uint32_t, std::vector<std::uint32_t>> members; // candidate root -> its ground polygons
		for (const Island& island : candidates)
			members[island.root];
		for (std::uint32_t id = 0; id < world.parent.size(); ++id)
			if (world.info[id].area > 0.0f && !world.info[id].clipped)
			{
				const auto found = members.find(world.find(id));
				if (found != members.end())
					found->second.push_back(id);
			}

		// ---- the owning building: Simulation::buildingAt's rule (docs/reference/INTERIORS.md) ----
		kworld::AreaGatherer gatherer(KENSHI_DATA, terrain, *gameData);
		kworld::CollisionLibrary library(KENSHI_DATA, *gameData);
		std::map<std::pair<int, int>, kworld::AreaGeometry> areas;
		std::map<std::pair<int, int>, std::vector<OwnerBox>> boxes;
		const auto areaFor = [&](const kworld::ZoneCell zone) -> const kworld::AreaGeometry&
		{
			auto found = areas.find({ zone.x, zone.z });
			if (found == areas.end())
				found = areas.emplace(std::make_pair(zone.x, zone.z), gatherer.gather(zone, zone)).first;
			return found->second;
		};
		const auto boxesFor = [&](const kworld::ZoneCell zone) -> const std::vector<OwnerBox>&
		{
			auto found = boxes.find({ zone.x, zone.z });
			if (found != boxes.end())
				return found->second;
			std::vector<OwnerBox> out;
			for (const kworld::AreaBuilding& building : areaFor(zone).buildings)
			{
				double ax = 0.0, az = 0.0;
				if (!placementAxis(building.placed, ax, az))
					continue;
				std::vector<kworld::WorldShape> mask;
				std::vector<kworld::WorldShape> footprint; // a maskless building: the shell, if it has a door
				const std::vector<kworld::WorldShape>* shapes = nullptr;
				if (library.addInteriorMask(building.placed, mask))
					shapes = &mask;
				else
				{
					const kdata::Record* record = gameData->find(building.placed.buildingID);
					const kdata::RefCategory* doors = record != nullptr ? record->findRefs("doors") : nullptr;
					if (doors == nullptr || doors->entries.empty() || !building.shapes)
						continue;
					footprint = *building.shapes;
					shapes = &footprint;
				}
				const Bounds bounds = boundsOf(*shapes, building.placed, ax, az);
				if (bounds.minA > bounds.maxA)
					continue;
				OwnerBox box;
				box.record = building.placed.buildingID;
				box.placement = building.id;
				box.cx = building.placed.x + ax * (bounds.minA + bounds.maxA) * 0.5 - az * (bounds.minB + bounds.maxB) * 0.5;
				box.cz = building.placed.z + az * (bounds.minA + bounds.maxA) * 0.5 + ax * (bounds.minB + bounds.maxB) * 0.5;
				box.axisX = ax;
				box.axisZ = az;
				box.halfA = (bounds.maxA - bounds.minA) * 0.5;
				box.halfB = (bounds.maxB - bounds.minB) * 0.5;
				box.minY = bounds.minY;
				box.maxY = bounds.maxY;
				box.base = building.placed.y;
				if (building.shapes)
					box.top = boundsOf(*building.shapes, building.placed, ax, az).maxY;
				box.area = box.halfA * box.halfB;
				out.push_back(box);
			}
			return boxes.emplace(std::make_pair(zone.x, zone.z), std::move(out)).first->second;
		};
		const auto owner = [&](const Island& island) -> Owner
		{
			const kworld::ZoneCell zone = kworld::zoneCellOf(island.cx, island.cz);
			const OwnerBox* best = nullptr;
			for (const OwnerBox& box : boxesFor(zone))
			{
				if (best != nullptr && box.area >= best->area)
					continue;
				const double dx = island.cx - box.cx;
				const double dz = island.cz - box.cz;
				if (std::fabs(dx * box.axisX + dz * box.axisZ) > box.halfA || std::fabs(-dx * box.axisZ + dz * box.axisX) > box.halfB)
					continue;
				if (island.cy < box.minY - kBuildingHeightSlack || island.cy > box.maxY + kBuildingHeightSlack)
					continue;
				best = &box;
			}
			// An island far under the placement's y is terrain beneath a very large mask hull (5312-TwoStorey's ~300k u2 pockets 20-147 u
			// below its base), not a floor of the building: open world (kenshi-k16.7.53).
			if (best == nullptr || island.cy < best->base - kHullTerrainDepth)
				return Owner{};
			return Owner{ best->record, best->placement, island.cy - best->base, best->top - best->base, true };
		};

		const std::vector<BaselineEntry> baseline = readBaseline(baselinePath);
		std::vector<bool> used(baseline.size(), false);
		std::ofstream tsv(outDir + "/nav_islands.tsv");
		tsv << "rank\tarea_u2\tpolys\tclipped_area_u2\tcx\tcy\tcz\ty_min\ty_max\tbox_x\tbox_z\tbuilding_record\tplacement\tfloor_y_above_base\t"
			   "source\tinterior_rebaked_zones\ttop_y_above_base\tnearest_main_gap_u\tnearest_main_x\tnearest_main_y\tnearest_main_z\tbaseline\n";
		std::vector<std::string> unknown;
		std::ostringstream fresh; // baseline lines for what is not in it (--write-baseline)
		std::size_t pending = 0, bugs = 0;
		bool expected = false;
		std::string absentFound; // --expect-absent: the island that breaks it, empty when none did
		std::vector<OracleProbe> probes;
		std::size_t gatedShipped = 0, gatedRebaked = 0, gatedMixed = 0, gatedBuilding = 0, gatedOpen = 0;
		for (std::size_t rank = 0; rank < candidates.size(); ++rank)
		{
			const Island& island = candidates[rank];
			// Nearest main polygon to any of the island's polygons, searched ring by ring up to 1,024 u.
			double bestGap = -1.0;
			float bestAt[3] = {};
			for (const std::uint32_t id : members.at(island.root))
			{
				const PolyInfo& poly = world.info[id];
				const std::int32_t cx = static_cast<std::int32_t>(std::floor(poly.x / kGridCell));
				const std::int32_t cz = static_cast<std::int32_t>(std::floor(poly.z / kGridCell));
				for (int ring = 0; ring <= 16; ++ring)
				{
					for (int dz = -ring; dz <= ring; ++dz)
					{
						for (int dx = -ring; dx <= ring; ++dx)
						{
							if (std::max(std::abs(dx), std::abs(dz)) != ring)
								continue;
							const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx + dx)) << 32)
								| static_cast<std::uint32_t>(cz + dz);
							for (auto it = std::lower_bound(grid.begin(), grid.end(), std::make_pair(key, std::uint32_t{ 0 }));
								 it != grid.end() && it->first == key; ++it)
							{
								const PolyInfo& near = world.info[it->second];
								const double gap = std::sqrt(std::pow(near.x - poly.x, 2.0) + std::pow(near.y - poly.y, 2.0)
									+ std::pow(near.z - poly.z, 2.0));
								if (bestGap < 0.0 || gap < bestGap)
								{
									bestGap = gap;
									bestAt[0] = near.x;
									bestAt[1] = near.y;
									bestAt[2] = near.z;
								}
							}
						}
					}
					if (bestGap >= 0.0 && bestGap <= (ring + 0.0) * kGridCell)
						break;
				}
			}
			const Owner who = owner(island);
			const bool gated = who.found || island.area >= kOpenGate;
			// The island's source (see Source): the loader's decision on the tiles its polygons are in.
			Source source;
			std::set<int> counted; // zones already counted for the interior-rebaked tally
			for (const std::uint32_t id : members.at(island.root))
			{
				const PolyInfo& poly = world.info[id];
				const kworld::ZoneCell zone = kworld::zoneCellOf(poly.x, poly.z);
				const ksim::MeshNavigator::NativeTileReport* report = nativeReportOf(zone);
				if (report == nullptr)
				{
					++source.unknown;
					continue;
				}
				const knav::TileCoord tile{ poly.tx, poly.tz };
				if (std::find(report->rebaked.begin(), report->rebaked.end(), tile) != report->rebaked.end())
				{
					++source.rebaked;
					continue;
				}
				++source.shipped;
				bool interiorRebaked = false;
				for (const ksim::MeshNavigator::NativeInteriorReport& interior : report->interiors)
					interiorRebaked = interiorRebaked || interior.decision.choice != ksim::MeshNavigator::NativeChoice::Shipped;
				if (interiorRebaked && counted.insert(kworld::zoneIndexOf(zone)).second)
					++source.interiorRebakedZones;
			}
			const std::string origin = source.rebaked == 0 ? (source.unknown != 0 ? "shipped?" : "shipped")
				: source.shipped + source.unknown == 0 ? "rebaked" : "mixed";
			if (gated)
			{
				if (origin == "rebaked")
					++gatedRebaked;
				else if (origin == "mixed")
					++gatedMixed;
				else
					++gatedShipped;
				if (who.found)
					++gatedBuilding;
				else
					++gatedOpen;
			}
			const kworld::ZoneCell centreZone = kworld::zoneCellOf(island.cx, island.cz);
			const std::string ownerKey = who.found ? who.record : "open world@" + std::to_string(centreZone.x) + "," + std::to_string(centreZone.z);
			const double floorKey = who.found ? who.floorY : island.cy;
			std::string note = gated ? "NEW" : "report only";
			if (gated)
			{
				// the closest unused line of the same owner within the floor and area bands
				std::size_t best = baseline.size();
				double bestScore = 1e30;
				for (std::size_t b = 0; b < baseline.size(); ++b)
				{
					const BaselineEntry& entry = baseline[b];
					if (used[b] || entry.record != ownerKey || std::fabs(entry.floorY - floorKey) > kBaselineFloorTol
						|| std::max(entry.area, island.area) > kBaselineAreaBand * std::min(entry.area, island.area))
						continue;
					const double score = std::fabs(entry.floorY - floorKey) / kBaselineFloorTol + std::log(std::max(entry.area, island.area) / std::min(entry.area, island.area));
					if (score < bestScore)
					{
						bestScore = score;
						best = b;
					}
				}
				if (best != baseline.size())
				{
					used[best] = true;
					note = baseline[best].status;
				}
			}
			if (note == "NEW")
			{
				fresh << std::fixed << std::setprecision(0) << ownerKey << "\t" << floorKey << "\t" << island.area << "\t" << island.cx << "\t" << island.cz << "\t" << origin << "\tpending\t" << island.area << " u2, "
					  << (who.found ? who.record : std::string("open world")) << " (unclassified: is it the original's own?)\n";
				std::ostringstream text;
				text << std::fixed << std::setprecision(0) << "island of " << island.area << " u2 at (" << island.cx << ", " << island.cy << ", " << island.cz << ")";
				unknown.push_back(text.str());
			}
			else if (note == "pending")
				++pending;
			else if (note == "bug")
				++bugs;
			if (expect && std::hypot(island.cx - expectX, island.cz - expectZ) <= expectR)
				expected = true;
			if (expectAbsent && island.area >= absentArea && std::hypot(island.cx - absentX, island.cz - absentZ) <= absentR)
			{
				std::ostringstream text;
				text << "an island of " << island.area << " u2 lies within " << absentR << " u of (" << absentX << ", " << absentZ << "), at ("
					 << island.cx << ", " << island.cy << ", " << island.cz << "), " << origin << ", " << (who.found ? who.record : "open world");
				absentFound = text.str();
			}
			tsv << std::fixed << std::setprecision(1) << rank + 1 << "\t" << island.area << "\t" << island.polys << "\t" << island.clippedArea << "\t"
				<< island.cx << "\t" << island.cy << "\t" << island.cz << "\t" << island.ymin << "\t" << island.ymax << "\t"
				<< island.x1 - island.x0 << "\t" << island.z1 - island.z0 << "\t"
				<< (who.found ? who.record : std::string("open world")) << "\t" << (who.found ? who.placement : std::string()) << "\t"
				<< (who.found ? who.floorY : 0.0) << "\t" << origin << "\t" << source.interiorRebakedZones << "\t"
				<< (who.found ? who.topY : 0.0) << "\t" << bestGap << "\t"
				<< bestAt[0] << "\t" << bestAt[1] << "\t" << bestAt[2] << "\t" << note << "\n";
		}
		tsv.close();
		std::cout << "report: " << outDir << "/nav_islands.tsv (" << candidates.size() << " islands: " << pending << " pending and " << bugs << " bug lines in the baseline, "
				  << unknown.size() << " not in it)\ngated: " << gatedBuilding + gatedOpen << " (" << gatedBuilding << " inside a building, " << gatedOpen
				  << " open world); source: " << gatedShipped << " shipped, " << gatedRebaked << " rebaked, " << gatedMixed << " mixed\n"
				  << "total: " << std::llround(msSince(start) / 1000.0) << " s wall, peak RSS " << statusKb("VmHWM") / 1024 << " MB\n";
		if (!writeBaseline.empty())
		{
			std::ofstream(writeBaseline, std::ios::app) << fresh.str();
			std::cout << "baseline lines appended to " << writeBaseline << "\n";
			unknown.clear();
		}
		if (expect)
		{
			test::require(expected, "no island within " + std::to_string(expectR) + " u of the expected (" + std::to_string(expectX) + ", " + std::to_string(expectZ) + ")");
			std::cout << "self-check: an island lies within " << expectR << " u of (" << expectX << ", " << expectZ << ")\n";
		}
		if (expectAbsent)
		{
			test::require(absentFound.empty(), "the expected gap is an island: " + absentFound);
			std::cout << "self-check: no island of " << absentArea << " u2 or more within " << absentR << " u of (" << absentX << ", "
					  << absentZ << ")\n";
		}
		if (!expect && !expectAbsent && !unknown.empty())
		{
			for (const std::string& line : unknown)
				std::cerr << "not in the baseline: " << line << "\n";
			test::require(false, std::to_string(unknown.size()) + " unreachable region(s) not in the baseline (" + outDir + "/nav_islands.tsv)");
		}
		std::cout << "ksim_nav_islands ok\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_nav_islands: " << e.what() << "\n";
		return 1;
	}
}
