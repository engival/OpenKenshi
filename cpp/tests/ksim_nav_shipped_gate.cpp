// The shipped tile as a standing two-sided gate for the patch bake (kenshi-k16.7.66; docs/formats/NAVMESH_CACHE.md "Shipped-tile gate").
// The runtime mesh is the original's tiles where the stale-tile check passes and OUR bake where it does not (1,051 of 3,995 zones:
// most town interiors). The island test and the door sweep see only under-connection; over-walk joins the main mesh unseen. This test
// holds our rebake of every such zone against the same zone's shipped tile, on both sides:
//  - covered %: the original's surface (beyond kRadius of its walls) that our bake covers (knav/compare.hpp, knav_compare's method);
//  - surplus %: our surface the original has no surface under (over-walk: a roof, a wall top, ground under a building);
//  - connectivity: pairs of points sampled on either mesh, kept when both meshes have a polygon at both points; reachable in one mesh
//    and not in the other is "under" (shipped yes, ours no) or "over" (ours yes, shipped no). Within the zone and its four neighbours.
// Also measured in the same run (printed, in the report files; NAVMESH_CACHE.md records the numbers):
//  - why each zone is stale: which term of the zone hash differs, as far as the hash lets us tell (docs/formats/NAVHASH.md);
//  - how many of a stale zone's 324 tiles no building shape (nor a shipped interior) touches: pure terrain, candidates to keep shipped;
//  - the unlinked portal length of each seam of a rebaked zone with a shipped / rebaked neighbour (and a shipped | shipped control).
// Gated against tests/data/nav_shipped_gate_baseline.tsv: the world totals may not get worse on either side, nor connectivity
// disagreements grow, nor a zone lose more than kZoneSlack percentage points. A recipe change that improves is checked in with
// `--write-baseline FILE` (the new baseline) after the run is read.
// argv: [1] output directory (report files and the nav cache; default "."), [2] baseline file (default the checked-in one),
//       `--write-baseline FILE`, `--zones X0 Z0 X1 Z1` (a window: only those zones, no world-total gate), `--cell C` (default 4 u).
// Exit 77 without the navtiles.

#include "ksim/nav/navigator.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/collision_area.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/loadorder.hpp"
#include "kworld/navhash.hpp"
#include "kworld/navhash_source.hpp"

#include "knav/compare.hpp"
#include "knav/native.hpp"
#include "knav/navmesh.hpp"
#include "knav/travel.hpp"

#include "DetourNavMesh.h"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace
{

namespace compare = knav::compare;
using ksim::MeshNavigator;

constexpr double kRadius = 4.0; // knav_compare's --radius: the original's surface within this of a wall is not asked of us (the Edge band)
constexpr double kTol = 30.0; // knav_compare's --tol
constexpr double kZoneSlack = 2.0; // percentage points a single zone may lose against the baseline
constexpr double kWorldSlack = 0.005; // percentage points the world totals may lose
constexpr int kZoneTiles = 18;
constexpr int kPointsPerSet = 160; // sampled polygons per mesh and zone
constexpr double kPairMin = 40.0, kPairMax = 500.0; // xz distance of a connectivity pair
constexpr std::size_t kThreads = 6;
constexpr int kControlSeams = 70; // shipped | shipped seams measured as the control
constexpr double kSeamClimb = 20.0; // a seam edge pair this near in height is a portal that should link

struct Counts
{
	std::size_t kept = 0, hit = 0, ours = 0, surplus = 0, pairs = 0, under = 0, over = 0;
	double covered() const { return kept == 0 ? 100.0 : 100.0 * static_cast<double>(hit) / static_cast<double>(kept); }
	double surplusPct() const { return ours == 0 ? 0.0 : 100.0 * static_cast<double>(surplus) / static_cast<double>(ours); }
	void add(const Counts& o)
	{
		kept += o.kept; hit += o.hit; ours += o.ours; surplus += o.surplus; pairs += o.pairs; under += o.under; over += o.over;
	}
};

struct Zone
{
	kworld::ZoneCell cell;
	Counts counts;
	std::string reason;
	int terrainOnly = 0; // of 324 tiles: no building shape and no shipped interior touches it
	int foliageOnly = 0; // ... touched by foliage shapes only
	int tiles = 0;
};

double msSince(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

int tileOf(double v)
{
	return static_cast<int>(std::floor((v - kworld::kWorldMin) / 256.0));
}

// ---- baseline ----
struct Baseline
{
	Counts world;
	std::size_t zones = 0;
	std::map<int, Counts> byZone;
};

Baseline readBaseline(const std::string& path)
{
	Baseline out;
	std::ifstream in(path);
	test::require(static_cast<bool>(in), "cannot open the baseline " + path);
	std::string line;
	while (std::getline(in, line))
	{
		if (line.empty() || line[0] == '#')
			continue;
		std::istringstream f(line);
		std::string kind;
		f >> kind;
		Counts c;
		if (kind == "world")
		{
			f >> out.zones >> c.kept >> c.hit >> c.ours >> c.surplus >> c.pairs >> c.under >> c.over;
			out.world = c;
		} else if (kind == "zone") {
			int x = 0, z = 0;
			f >> x >> z >> c.kept >> c.hit >> c.ours >> c.surplus >> c.pairs >> c.under >> c.over;
			out.byZone[kworld::zoneIndexOf({ x, z })] = c;
		}
	}
	test::require(out.zones != 0, "the baseline has no world line: " + path);
	return out;
}

// ---- seams ----
struct EdgeSpan
{
	double t0, t1, y;
	bool linked;
};

// The boundary edges of ground polygons of the tile column (`east`: x, else z) `column`, on the line `border`: spans along the other
// axis, tiles limited to t0..t1 along it.
void sideSpans(const dtNavMesh& nav, bool east, double border, int column, int t0, int t1, std::vector<EdgeSpan>& out)
{
	const std::uint16_t flags = knav::TravelFilter{}.allowed;
	for (int i = 0; i < nav.getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = nav.getTile(i);
		if (tile == nullptr || tile->header == nullptr || (east ? tile->header->x : tile->header->y) != column)
			continue;
		const int along = east ? tile->header->y : tile->header->x;
		if (along < t0 || along > t1)
			continue;
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			const dtPoly& poly = tile->polys[p];
			if (poly.getType() != DT_POLYTYPE_GROUND || (poly.flags & flags) == 0)
				continue;
			for (int e = 0; e < poly.vertCount; ++e)
			{
				if ((poly.neis[e] & DT_EXT_LINK) == 0)
					continue;
				const float* va = &tile->verts[poly.verts[e] * 3];
				const float* vb = &tile->verts[poly.verts[(e + 1) % poly.vertCount] * 3];
				if (std::fabs((east ? va[0] : va[2]) - border) > 0.5 || std::fabs((east ? vb[0] : vb[2]) - border) > 0.5)
					continue;
				bool linked = false;
				for (unsigned int l = poly.firstLink; l != DT_NULL_LINK && !linked; l = tile->links[l].next)
					linked = tile->links[l].edge == e;
				const double ta = east ? va[2] : va[0], tb = east ? vb[2] : vb[0];
				out.push_back({ std::min(ta, tb), std::max(ta, tb), 0.5 * (va[1] + vb[1]), linked });
			}
		}
	}
}

// Of side A's spans, the length that faces a walkable span of side B at a compatible height (a portal) and the part of it with no link.
void seamLength(const std::vector<EdgeSpan>& sideA, const std::vector<EdgeSpan>& sideB, double& portal, double& unlinked)
{
	std::vector<EdgeSpan> b = sideB;
	std::sort(b.begin(), b.end(), [](const EdgeSpan& l, const EdgeSpan& r) { return l.t0 < r.t0; });
	for (const EdgeSpan& a : sideA)
	{
		double covered = 0.0;
		for (const EdgeSpan& o : b)
		{
			if (o.t0 >= a.t1)
				break;
			const double overlap = std::min(a.t1, o.t1) - std::max(a.t0, o.t0);
			if (overlap > 0.01 && std::fabs(a.y - o.y) <= kSeamClimb)
				covered += overlap;
		}
		covered = std::min(covered, a.t1 - a.t0);
		portal += covered;
		if (!a.linked)
			unlinked += covered;
	}
}

struct SeamRow
{
	kworld::ZoneCell a, b;
	std::string kind; // "rebaked|shipped" ...
	double portal = 0.0, unlinked = 0.0;
};

// The seam between zone `a` and its east (`east`: along z) or south neighbour `b`, both resident in `mesh`.
SeamRow measureSeam(const knav::NavMesh& mesh, kworld::ZoneCell a, kworld::ZoneCell b, bool east)
{
	SeamRow row{ a, b, {}, 0.0, 0.0 };
	const double border = kworld::kWorldMin + kworld::kZoneSize * (east ? a.x + 1 : a.z + 1);
	const int t0 = (east ? a.z : a.x) * kZoneTiles, t1 = t0 + kZoneTiles - 1;
	const int tileA = (east ? a.x + 1 : a.z + 1) * kZoneTiles - 1;
	std::vector<EdgeSpan> sideA, sideB;
	sideSpans(mesh.detour(), east, border, tileA, t0, t1, sideA);
	sideSpans(mesh.detour(), east, border, tileA + 1, t0, t1, sideB);
	seamLength(sideA, sideB, row.portal, row.unlinked);
	return row;
}

// ---- connectivity ----
struct Spot
{
	knav::Point at;
};

// Up to kPointsPerSet polygon centroids of `mesh` inside the zone, evenly strided.
std::vector<Spot> samplePolys(const knav::NavMesh& mesh, kworld::ZoneCell zone)
{
	std::vector<Spot> all;
	const dtNavMesh& nav = mesh.detour();
	for (int i = 0; i < nav.getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = nav.getTile(i);
		if (tile == nullptr || tile->header == nullptr)
			continue;
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			const dtPoly& poly = tile->polys[p];
			if (poly.getType() != DT_POLYTYPE_GROUND || (poly.flags & knav::TravelFilter{}.allowed) == 0)
				continue;
			double x = 0.0, y = 0.0, z = 0.0;
			for (int v = 0; v < poly.vertCount; ++v)
			{
				x += tile->verts[poly.verts[v] * 3];
				y += tile->verts[poly.verts[v] * 3 + 1];
				z += tile->verts[poly.verts[v] * 3 + 2];
			}
			x /= poly.vertCount;
			y /= poly.vertCount;
			z /= poly.vertCount;
			const kworld::ZoneCell at = kworld::zoneCellOf(x, z);
			if (at.x == zone.x && at.z == zone.z)
				all.push_back({ { static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) } });
		}
	}
	std::vector<Spot> out;
	const std::size_t step = std::max<std::size_t>(1, all.size() / kPointsPerSet);
	for (std::size_t k = step / 2; k < all.size(); k += step)
		out.push_back(all[k]);
	return out;
}

struct Reach
{
	const knav::NavMesh& mesh;
	knav::NavQuery query;
	knav::PolyComponents components;
	explicit Reach(const knav::NavMesh& m) : mesh(m), query(m, 64), components(m, knav::TravelFilter{}.allowed) {}
	// The component of the polygon at the point (tight in y: the same floor), kNone when there is none.
	std::uint32_t label(const knav::Point& p) const
	{
		knav::PolyLoc loc;
		if (!query.locate(p, { 2.0f, 6.0f, 2.0f }, loc))
			return knav::PolyComponents::kNone;
		return components.at(loc.tileX, loc.tileZ, loc.poly);
	}
};

// Pairs inside one sample set (a spot and the next one 40-500 u away), judged in both meshes.
void judgePairs(const std::vector<Spot>& spots, const Reach& shipped, const Reach& ours, Counts& counts)
{
	std::vector<std::array<std::uint32_t, 2>> sh(spots.size()), our(spots.size());
	for (std::size_t i = 0; i < spots.size(); ++i)
	{
		sh[i] = { shipped.label(spots[i].at), 0 };
		our[i] = { ours.label(spots[i].at), 0 };
	}
	for (std::size_t i = 0; i < spots.size(); ++i)
	{
		for (std::size_t j = i + 1; j < spots.size(); ++j)
		{
			const double d = std::hypot(spots[i].at[0] - spots[j].at[0], spots[i].at[2] - spots[j].at[2]);
			if (d < kPairMin || d > kPairMax)
				continue;
			if (sh[i][0] == knav::PolyComponents::kNone || sh[j][0] == knav::PolyComponents::kNone
				|| our[i][0] == knav::PolyComponents::kNone || our[j][0] == knav::PolyComponents::kNone)
				break; // not on both meshes: the cover figure already counts it; one partner per spot
			++counts.pairs;
			const bool inShipped = sh[i][0] == sh[j][0], inOurs = our[i][0] == our[j][0];
			if (inShipped && !inOurs)
				++counts.under;
			else if (inOurs && !inShipped)
				++counts.over;
			break;
		}
	}
}

// ---- one zone's score, run off the main thread ----
struct ZoneJob
{
	std::vector<compare::Tri> nativeTris, ourTris;
	std::vector<compare::Wall> walls;
	std::vector<bool> interior;
	compare::Grid grid;
};

Counts runJob(const ZoneJob& job)
{
	const compare::Score s = compare::score(job.nativeTris, job.walls, job.interior, job.ourTris, job.grid, kTol, kRadius);
	Counts c;
	c.kept = s.nativeKept;
	c.hit = s.nativeHit;
	c.ours = s.oursTotal;
	c.surplus = s.oursNoNative;
	return c;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const std::string navtiles = knav::native::navtilesDir(KENSHI_DATA);
		if (!std::filesystem::is_directory(navtiles))
		{
			std::cerr << "ksim_nav_shipped_gate: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		std::string outDir = ".", baselinePath = KSIM_SHIPPED_GATE_BASELINE, writeBaseline;
		double cell = 4.0;
		int window[4] = { 0, 0, kworld::kZoneCellsPerAxis - 1, kworld::kZoneCellsPerAxis - 1 };
		bool windowed = false;
		int positional = 0;
		for (int i = 1; i < argc; ++i)
		{
			const std::string arg = argv[i];
			if (arg == "--write-baseline" && i + 1 < argc)
				writeBaseline = argv[++i];
			else if (arg == "--cell" && i + 1 < argc)
				cell = std::stod(argv[++i]);
			else if (arg == "--zones" && i + 4 < argc)
			{
				for (int k = 0; k < 4; ++k)
					window[k] = std::stoi(argv[i + 1 + k]);
				i += 4;
				windowed = true;
			}
			else if (positional++ == 0)
				outDir = arg;
			else
				baselinePath = arg;
		}
		std::filesystem::create_directories(outDir);
		const auto start = std::chrono::steady_clock::now();

		const auto terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gameData = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		ksim::NativeCacheOptions cache;
		cache.dir = outDir + "/navcache"; // the island test's tree (nav_islands/navcache) is the same cache when pointed at it
		cache.worldKey = kworld::dataHash(order);
		const auto logSink = [](const std::string&) {};
		const std::shared_ptr<MeshNavigator> ours = MeshNavigator::openNative(KENSHI_DATA, { { 20, 32 } }, logSink, gameData.get(), cache);
		ksim::NativeCacheOptions shippedCache = cache;
		shippedCache.shippedOnly = true;
		const std::shared_ptr<MeshNavigator> shipped = MeshNavigator::openNative(KENSHI_DATA, { { 20, 32 } }, logSink, gameData.get(), shippedCache);
		kworld::AreaGatherer gatherer(KENSHI_DATA, terrain, *gameData);
		const kworld::AreaSources sources = gatherer.sources();
		kworld::NavHashSource hashes(KENSHI_DATA, sources);

		// ---- the shipped tiles, and which of them the loader rebakes (the loader's own verdict function) ----
		std::map<int, bool> rebaked; // zone index -> the zone mesh is ours
		std::vector<kworld::ZoneCell> shippedZones;
		for (const auto& entry : std::filesystem::directory_iterator(navtiles))
		{
			kworld::ZoneCell c{};
			if (std::sscanf(entry.path().filename().string().c_str(), "tile%d.%d.hkt", &c.x, &c.z) == 2)
				shippedZones.push_back(c);
		}
		std::sort(shippedZones.begin(), shippedZones.end(), [](kworld::ZoneCell a, kworld::ZoneCell b)
			{ return std::make_tuple(a.z / knav::kRegionZones, a.x / knav::kRegionZones, a.z, a.x) < std::make_tuple(b.z / knav::kRegionZones, b.x / knav::kRegionZones, b.z, b.x); });
		std::vector<Zone> zones;
		std::map<std::string, std::size_t> reasons;
		std::size_t stale = 0, unverifiable = 0;
		for (const kworld::ZoneCell c : shippedZones)
		{
			knav::native::StoredHashes stored;
			knav::native::loadTile(navtiles, c.x, c.z, &stored);
			const kworld::NavHashSource::ZoneHashes h = hashes.zone(c);
			const kworld::StaleVerdict verdict = kworld::staleVerdict(stored.hasZone ? std::optional(stored.zone) : std::nullopt, h.hash);
			rebaked[kworld::zoneIndexOf(c)] = verdict != kworld::StaleVerdict::Fresh;
			if (verdict == kworld::StaleVerdict::Fresh)
				continue;
			++(verdict == kworld::StaleVerdict::Stale ? stale : unverifiable);
			Zone zone;
			zone.cell = c;
			// Which hash input differs. The zone hash folds the buildings (the fold) and then the forest word; a stored hash cannot be split
			// into its two terms, so what is told is what the terms allow (NAVHASH.md): no folded building -> only the forest word can differ;
			// the stored hash equal to the fold or to comb(fold, 0) -> the buildings agree and the shipped bake saw no trees; else either.
			const std::string grass = h.treesGrass != 0 ? ", grass-limited trees" : "";
			if (!stored.hasZone || !h.hash)
				zone.reason = "no verdict (no stored hash / zone unreadable)";
			else if (h.buildings == 0)
				zone.reason = h.trees == 0 ? "forest only: no folded building, none of ours trees" : "forest only: no folded building" + grass;
			else if (stored.zone == h.fold || stored.zone == kworld::comb(h.fold, 0))
				zone.reason = "forest only: buildings agree (the shipped bake saw no trees)" + grass;
			else
				zone.reason = h.trees == 0 ? "buildings (no trees of ours: the forest term is empty)" : "buildings and/or forest (undecidable)" + grass;
			++reasons[zone.reason];
			if (c.x >= window[0] && c.z >= window[1] && c.x <= window[2] && c.z <= window[3])
				zones.push_back(zone);
		}
		const std::size_t staleWorld = stale + unverifiable;
		std::cout << "shipped tiles " << shippedZones.size() << ": rebaked " << staleWorld << " (stale " << stale << ", unverifiable " << unverifiable
			<< "); " << std::llround(msSince(start) / 1000.0) << " s\n";
		for (const auto& [reason, n] : reasons)
			std::cout << "  stale reason: " << n << " zones: " << reason << "\n";

		// ---- per zone ----
			std::deque<std::pair<std::size_t, std::future<Counts>>> running;
		std::vector<SeamRow> seams;
		std::set<std::array<int, 3>> seamSeen;
		Counts world;
		std::size_t done = 0;
		const auto finish = [&]()
		{
			Zone& zone = zones[running.front().first];
			const Counts c = running.front().second.get();
			zone.counts.kept = c.kept;
			zone.counts.hit = c.hit;
			zone.counts.ours = c.ours;
			zone.counts.surplus = c.surplus;
			running.pop_front();
		};
		const auto resident = [&](MeshNavigator& nav, kworld::ZoneCell z)
		{
			std::vector<kworld::ZoneCell> want{ z };
			for (const kworld::ZoneCell n : { kworld::ZoneCell{ z.x + 1, z.z }, kworld::ZoneCell{ z.x, z.z + 1 },
				kworld::ZoneCell{ z.x - 1, z.z }, kworld::ZoneCell{ z.x, z.z - 1 } })
				if (n.x >= 0 && n.z >= 0 && n.x < kworld::kZoneCellsPerAxis && n.z < kworld::kZoneCellsPerAxis && nav.canCover(n))
					want.push_back(n);
			nav.setCoverage(want);
		};
		for (std::size_t zi = 0; zi < zones.size(); ++zi)
		{
			Zone& zone = zones[zi];
			const kworld::ZoneCell z = zone.cell;
			resident(*ours, z);
			resident(*shipped, z);
			const MeshNavigator::NativeTileReport* report = ours->nativeReport(z);
			test::require(report != nullptr && report->decision.choice != MeshNavigator::NativeChoice::Shipped,
				"zone " + std::to_string(z.x) + "," + std::to_string(z.z) + ": the loader does not rebake what the gate's verdict rebakes");

			// the score job: the shipped zone (and its neighbours' interiors that reach in) against our rebaked zone
			ZoneJob job;
			const double x0 = kworld::kWorldMin + kworld::kZoneSize * z.x, z0 = kworld::kWorldMin + kworld::kZoneSize * z.z;
			job.grid = { x0, z0, cell, static_cast<int>(std::lround(kworld::kZoneSize / cell)), static_cast<int>(std::lround(kworld::kZoneSize / cell)) };
			std::vector<knav::native::Mesh> meshes;
			for (int j = z.z - 1; j <= z.z + 1; ++j)
				for (int i = z.x - 1; i <= z.x + 1; ++i)
					for (knav::native::Mesh& m : knav::native::loadTile(navtiles, i, j))
						meshes.push_back(std::move(m));
			compare::nativeGeometry(meshes, job.nativeTris, job.walls);
			for (const knav::native::Mesh& m : meshes)
				job.interior.push_back(m.interior());
			for (const compare::Tri& t : compare::ourTriangles(ours->mesh()))
			{
				const double lo = std::min({ t.a[0], t.b[0], t.c[0] }), hi = std::max({ t.a[0], t.b[0], t.c[0] });
				const double lz = std::min({ t.a[2], t.b[2], t.c[2] }), hz = std::max({ t.a[2], t.b[2], t.c[2] });
				if (hi >= x0 && lo <= x0 + kworld::kZoneSize && hz >= z0 && lz <= z0 + kworld::kZoneSize)
					job.ourTris.push_back(t);
			}
			// the nearest native tris need only the box too
			std::erase_if(job.nativeTris, [&](const compare::Tri& t)
				{
					const double lo = std::min({ t.a[0], t.b[0], t.c[0] }), hi = std::max({ t.a[0], t.b[0], t.c[0] });
					const double lz = std::min({ t.a[2], t.b[2], t.c[2] }), hz = std::max({ t.a[2], t.b[2], t.c[2] });
					return !(hi >= x0 - 1 && lo <= x0 + kworld::kZoneSize + 1 && hz >= z0 - 1 && lz <= z0 + kworld::kZoneSize + 1);
				});
			running.emplace_back(zi, std::async(std::launch::async, [job = std::move(job)]() { return runJob(job); }));

			// connectivity: points on either mesh, judged in both
			{
				const Reach sh(shipped->mesh()), ou(ours->mesh());
				judgePairs(samplePolys(shipped->mesh(), z), sh, ou, zone.counts);
				judgePairs(samplePolys(ours->mesh(), z), sh, ou, zone.counts);
			}

			// terrain-only tiles: tiles no building shape (and no shipped interior) touches
			{
				const knav::NavConfig config = knav::configForWorldZones(z, z);
				const kworld::AreaGeometry area = gatherer.gather(z, z);
				std::set<std::pair<int, int>> building, foliage;
				for (const kworld::AreaBuilding& b : area.buildings)
					if (b.shapes)
						for (const kworld::WorldShape& shape : *b.shapes)
							for (const knav::TileCoord& t : knav::tilesTouching(config, shape))
								building.insert({ t.x, t.z });
				if (area.foliage)
					for (const kworld::WorldShape& shape : *area.foliage)
						for (const knav::TileCoord& t : knav::tilesTouching(config, shape))
							foliage.insert({ t.x, t.z });
				for (const knav::native::Mesh& m : meshes)
					if (m.interior() && !m.vertices.empty())
					{
						double mnx = 1e30, mxx = -1e30, mnz = 1e30, mxz = -1e30;
						for (const auto& v : m.vertices)
						{
							mnx = std::min(mnx, v[0]); mxx = std::max(mxx, v[0]);
							mnz = std::min(mnz, v[2]); mxz = std::max(mxz, v[2]);
						}
						for (int tz = tileOf(mnz); tz <= tileOf(mxz); ++tz)
							for (int tx = tileOf(mnx); tx <= tileOf(mxx); ++tx)
								building.insert({ tx, tz });
					}
				for (int tz = z.z * kZoneTiles; tz < (z.z + 1) * kZoneTiles; ++tz)
					for (int tx = z.x * kZoneTiles; tx < (z.x + 1) * kZoneTiles; ++tx)
					{
						++zone.tiles;
						if (building.count({ tx, tz }) == 0)
							++(foliage.count({ tx, tz }) == 0 ? zone.terrainOnly : zone.foliageOnly);
					}
			}

			// seams: the four edges of the zone against resident neighbours (each seam once)
			for (int dir = 0; dir < 4; ++dir)
			{
				const kworld::ZoneCell n{ z.x + (dir == 0) - (dir == 2), z.z + (dir == 1) - (dir == 3) };
				if (n.x < 0 || n.z < 0 || n.x >= kworld::kZoneCellsPerAxis || n.z >= kworld::kZoneCellsPerAxis || !rebaked.count(kworld::zoneIndexOf(n)))
					continue;
				const bool east = dir == 0 || dir == 2;
				const kworld::ZoneCell lo = dir < 2 ? z : n, hi = dir < 2 ? n : z;
				const int index = kworld::zoneIndexOf(lo);
				if (!seamSeen.insert({ index, east ? 0 : 1, 0 }).second)
					continue;
				SeamRow row = measureSeam(ours->mesh(), lo, hi, east);
				const bool loOurs = rebaked[kworld::zoneIndexOf(lo)], hiOurs = rebaked[kworld::zoneIndexOf(hi)];
				row.kind = loOurs && hiOurs ? "rebaked|rebaked" : "shipped|rebaked"; // a seam is the same whichever side is ours
				seams.push_back(row);
			}
			if (running.size() >= kThreads)
				finish();
			if (++done % 50 == 0)
				std::cout << done << " / " << zones.size() << " zones, " << std::llround(msSince(start) / 1000.0) << " s" << std::endl;
		}
		while (!running.empty())
			finish();

		// control: shipped | shipped seams, evenly spread
		if (!windowed)
		{
			std::vector<kworld::ZoneCell> candidates;
			for (const kworld::ZoneCell c : shippedZones)
			{
				const auto east = rebaked.find(kworld::zoneIndexOf({ c.x + 1, c.z }));
				if (!rebaked[kworld::zoneIndexOf(c)] && east != rebaked.end() && !east->second && ours->canCover(c) && ours->canCover({ c.x + 1, c.z }))
					candidates.push_back(c);
			}
			const std::size_t step = std::max<std::size_t>(1, candidates.size() / kControlSeams);
			for (std::size_t k = 0; k < candidates.size(); k += step)
			{
				ours->setCoverage({ candidates[k], { candidates[k].x + 1, candidates[k].z } });
				SeamRow row = measureSeam(ours->mesh(), candidates[k], { candidates[k].x + 1, candidates[k].z }, true);
				row.kind = "shipped|shipped (control)";
				seams.push_back(row);
			}
		}

		// ---- reports ----
		std::ofstream tsv(outDir + "/shipped_gate_zones.tsv");
		tsv << "zx\tzz\tkept\thit\tours\tsurplus\tcovered_pct\tsurplus_pct\tpairs\tunder\tover\ttiles\tterrain_only\tfoliage_only\treason\n";
		std::size_t terrainOnly = 0, foliageOnly = 0, tilesAll = 0, zonesAllTerrain = 0;
		for (const Zone& zone : zones)
		{
			world.add(zone.counts);
			terrainOnly += static_cast<std::size_t>(zone.terrainOnly);
			foliageOnly += static_cast<std::size_t>(zone.foliageOnly);
			tilesAll += static_cast<std::size_t>(zone.tiles);
			zonesAllTerrain += zone.terrainOnly + zone.foliageOnly == zone.tiles ? 1 : 0;
			tsv << std::fixed << std::setprecision(2) << zone.cell.x << "\t" << zone.cell.z << "\t" << zone.counts.kept << "\t" << zone.counts.hit << "\t"
				<< zone.counts.ours << "\t" << zone.counts.surplus << "\t" << zone.counts.covered() << "\t" << zone.counts.surplusPct() << "\t"
				<< zone.counts.pairs << "\t" << zone.counts.under << "\t" << zone.counts.over << "\t" << zone.tiles << "\t" << zone.terrainOnly << "\t"
				<< zone.foliageOnly << "\t" << zone.reason << "\n";
		}
		tsv.close();
		std::ofstream seamTsv(outDir + "/shipped_gate_seams.tsv");
		seamTsv << "kind\tzone_a\tzone_b\tportal_u\tunlinked_u\n";
		std::map<std::string, std::array<double, 3>> seamTotals; // kind -> portal, unlinked, count
		for (const SeamRow& row : seams)
		{
			seamTsv << std::fixed << std::setprecision(1) << row.kind << "\t" << row.a.x << "," << row.a.z << "\t" << row.b.x << "," << row.b.z << "\t"
				<< row.portal << "\t" << row.unlinked << "\n";
			auto& t = seamTotals[row.kind];
			t[0] += row.portal;
			t[1] += row.unlinked;
			t[2] += 1.0;
		}
		std::cout << std::fixed << std::setprecision(3);
		std::cout << "gate: " << zones.size() << " rebaked zones: covered " << world.covered() << "% (" << world.hit << " of " << world.kept
			<< " samples at cell " << cell << "), surplus " << world.surplusPct() << "% (" << world.surplus << " of " << world.ours << "); connectivity pairs "
			<< world.pairs << ", under " << world.under << ", over " << world.over << "\n";
		std::cout << "tiles of the rebaked zones: " << tilesAll << "; no building shape and no shipped interior touches " << terrainOnly + foliageOnly << " ("
			<< foliageOnly << " of them take foliage shapes: rocks, trees), pure terrain " << terrainOnly << "; zones with no building tile at all: " << zonesAllTerrain
			<< "\n";
		for (const auto& [kind, t] : seamTotals)
			std::cout << "seams " << kind << ": " << static_cast<long>(t[2]) << " seams, portal length " << std::llround(t[0]) << " u, unlinked " << std::llround(t[1])
				<< " u (" << (t[0] > 0 ? 100.0 * t[1] / t[0] : 0.0) << "%)\n";
		std::cout << "reports: " << outDir << "/shipped_gate_zones.tsv, shipped_gate_seams.tsv; " << std::llround(msSince(start) / 1000.0) << " s\n";

		if (!writeBaseline.empty())
		{
			std::ofstream out(writeBaseline);
			out << "# The shipped-tile gate's baseline (ksim_nav_shipped_gate, docs/formats/NAVMESH_CACHE.md \"Shipped-tile gate\"): per rebaked zone, our bake against\n"
				   "# the shipped tile at cell " << cell << " u. world: zones kept hit ours surplus pairs under over; zone: x z kept hit ours surplus pairs under over.\n"
				   "# covered % = hit/kept, surplus % = surplus/ours; under = reachable in the shipped mesh not in ours, over = the reverse.\n";
			out << "world\t" << zones.size() << "\t" << world.kept << "\t" << world.hit << "\t" << world.ours << "\t" << world.surplus << "\t" << world.pairs << "\t"
				<< world.under << "\t" << world.over << "\n";
			for (const Zone& zone : zones)
				out << "zone\t" << zone.cell.x << "\t" << zone.cell.z << "\t" << zone.counts.kept << "\t" << zone.counts.hit << "\t" << zone.counts.ours << "\t"
					<< zone.counts.surplus << "\t" << zone.counts.pairs << "\t" << zone.counts.under << "\t" << zone.counts.over << "\n";
			std::cout << "baseline written to " << writeBaseline << "\n";
			return 0;
		}

		// ---- the gate ----
		const Baseline base = readBaseline(baselinePath);
		std::vector<std::string> failures;
		for (const Zone& zone : zones)
		{
			const auto found = base.byZone.find(kworld::zoneIndexOf(zone.cell));
			if (found == base.byZone.end())
			{
				failures.push_back("zone " + std::to_string(zone.cell.x) + "," + std::to_string(zone.cell.z) + " is rebaked now and not in the baseline");
				continue;
			}
			const Counts& b = found->second;
			const auto fmt = [](double v) { std::ostringstream s; s << std::fixed << std::setprecision(2) << v; return s.str(); };
			if (zone.counts.covered() < b.covered() - kZoneSlack)
				failures.push_back("zone " + std::to_string(zone.cell.x) + "," + std::to_string(zone.cell.z) + " covers less: " + fmt(zone.counts.covered()) + "% against " + fmt(b.covered()) + "%");
			if (zone.counts.surplusPct() > b.surplusPct() + kZoneSlack)
				failures.push_back("zone " + std::to_string(zone.cell.x) + "," + std::to_string(zone.cell.z) + " walks more over nothing: surplus " + fmt(zone.counts.surplusPct()) + "% against " + fmt(b.surplusPct()) + "%");
		}
		if (!windowed)
		{
			if (zones.size() > base.zones)
				failures.push_back(std::to_string(zones.size()) + " zones rebaked, the baseline has " + std::to_string(base.zones));
			if (world.covered() < base.world.covered() - kWorldSlack)
				failures.push_back("world cover fell: " + std::to_string(world.covered()) + "% against " + std::to_string(base.world.covered()) + "%");
			if (world.surplusPct() > base.world.surplusPct() + kWorldSlack)
				failures.push_back("world surplus rose: " + std::to_string(world.surplusPct()) + "% against " + std::to_string(base.world.surplusPct()) + "%");
			if (world.under > base.world.under)
				failures.push_back("under-connection pairs rose: " + std::to_string(world.under) + " against " + std::to_string(base.world.under));
			if (world.over > base.world.over)
				failures.push_back("over-walk pairs rose: " + std::to_string(world.over) + " against " + std::to_string(base.world.over));
		}
		for (const std::string& f : failures)
			std::cerr << "gate: " << f << "\n";
		test::require(failures.empty(), std::to_string(failures.size()) + " gate failure(s) against " + baselinePath);
		std::cout << "ksim_nav_shipped_gate ok\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_nav_shipped_gate: " << e.what() << "\n";
		return 1;
	}
}
