// Line of sight, phase 1 (docs/reference/LINE_OF_SIGHT.md, task AZ): the placed buildings' collision as per-region bounding-
// volume trees, the segment query against the hulls and the terrain, the memory, the load/unload cycle, rays across a region
// border, and the cost. The geometry is the real Hub: the Bar (3384-TwoStorey) and its neighbours.
//
//  1. the BVH agrees with a brute-force scan of the same triangles;
//  2. a wall blocks (two eye points on opposite sides of a Bar wall);
//  3. an opening passes: the same line through a window or doorway is clear at some heights and blocked at others;
//  4. a floor blocks (a point under a slab, one above: no sight either way);
//  5. a low obstacle: blocked at body height, seen over at eye height;
//  6. memory: the Hub's regions and the biggest town region;
//  7. a region is freed when it leaves Full detail, and a ray across a border consults both regions' trees;
//  8. cost: ns per ray in the Hub, and the estimate for bar_crowd's eye-to-eye pairs.
//
// Reads the install at run time (KENSHI_DATA); nothing game-derived is built in.

#include "kworld/collision.hpp"
#include "kworld/features.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/hull_tree.hpp"
#include "kworld/towns.hpp"
#include "kworld/zones.hpp"

#include "ksim/world/interact.hpp"
#include "ksim/nav/los.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kBarX = -51181.0;
constexpr double kBarZ = 2639.0;
// Work per ray (tree boxes and triangles tested), pinned at ~1.5x the measured 103 / 80 street and 12 / 0 above the roofs.
constexpr double kStreetNodes = 155.0, kStreetTris = 120.0, kRoofNodes = 19.0, kRoofTris = 5.0;

std::string num(double v)
{
	char buffer[64];
	std::snprintf(buffer, sizeof buffer, "%.3f", v);
	return buffer;
}

struct Tri
{
	std::array<double, 3> a{}, b{}, c{}, n{}, centre{};
	double area = 0.0;
};

std::vector<Tri> trianglesOf(const std::vector<kworld::WorldShape>& shapes)
{
	std::vector<Tri> out;
	for (const kworld::WorldShape& shape : shapes)
	{
		for (std::size_t i = 0; i + 2 < shape.indices.size(); i += 3)
		{
			const auto point = [&](std::uint32_t index)
			{
				return std::array<double, 3>{ shape.vertices[index * 3], shape.vertices[index * 3 + 1], shape.vertices[index * 3 + 2] };
			};
			Tri tri;
			tri.a = point(shape.indices[i]);
			tri.b = point(shape.indices[i + 1]);
			tri.c = point(shape.indices[i + 2]);
			std::array<double, 3> u{ tri.b[0] - tri.a[0], tri.b[1] - tri.a[1], tri.b[2] - tri.a[2] };
			std::array<double, 3> v{ tri.c[0] - tri.a[0], tri.c[1] - tri.a[1], tri.c[2] - tri.a[2] };
			std::array<double, 3> cross{ u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
			const double length = std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
			if (length < 1.0e-6)
				continue; // degenerate
			tri.n = { cross[0] / length, cross[1] / length, cross[2] / length };
			tri.area = 0.5 * length;
			tri.centre = { (tri.a[0] + tri.b[0] + tri.c[0]) / 3.0, (tri.a[1] + tri.b[1] + tri.c[1]) / 3.0,
				(tri.a[2] + tri.b[2] + tri.c[2]) / 3.0 };
			out.push_back(tri);
		}
	}
	return out;
}

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::unique_ptr<Simulation> sim;

	Rig()
		: gd(std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA)))
		, sim(std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91)))
	{
		sim->setGameData(gd);
	}

	void areas(std::vector<state::GroundPoint> centres)
	{
		sim->enqueue(Command::setPlayerAreas(std::move(centres)));
		sim->tick();
	}
	const LineOfSight& los() const { return *sim->lineOfSight(); }
};

// The placement nearest a point, within `reach`.
const kworld::PlacedInstance* nearest(const std::vector<kworld::PlacedInstance>& placements, double x, double z, double reach)
{
	const kworld::PlacedInstance* best = nullptr;
	double bestDistance = reach;
	for (const kworld::PlacedInstance& placed : placements)
	{
		const double d = std::hypot(placed.x - x, placed.z - z);
		if (d < bestDistance)
		{
			bestDistance = d;
			best = &placed;
		}
	}
	return best;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_los HUB.knav");
		// ---- 0. the segment is origin + t * (b - a): an analytic triangle far from the origin ----
		{
			// At the Hub's coordinates (a |x| of ~51000 is what made "the direction is b" answer wrongly: the endpoint read as a
			// direction is a huge vector, and a triangle near the start then "hits" a fraction of a unit along the segment; task AZ).
			kworld::HullTree one;
			one.buildTriangles(std::vector<float>{ -51000.0f, 1500.0f, 2600.0f, -50990.0f, 1500.0f, 2600.0f, -51000.0f, 1510.0f, 2600.0f },
				{ 0, 1, 2 });
			double t = 0.0;
			// Straight down through the triangle's inside (x = base + 2, y = base + 2): the plane at z 2600 is the segment's midpoint.
			test::require(one.trace({ -50998.0, 1502.0, 2610.0 }, { -50998.0, 1502.0, 2590.0 }, t), "a segment through a triangle did not hit");
			test::require(std::abs(t - 0.5) < 1e-6, "the hit is not at the segment's midpoint");
			test::require(one.traceBrute({ -50998.0, 1502.0, 2610.0 }, { -50998.0, 1502.0, 2590.0 }, t) && std::abs(t - 0.5) < 1e-6,
				"the brute scan disagrees with the analytic hit");
			// Beside the triangle, and a segment that stops before its plane: no hit either time.
			test::require(!one.trace({ -50980.0, 1502.0, 2610.0 }, { -50980.0, 1502.0, 2590.0 }, t), "a segment beside the triangle hit it");
			test::require(!one.trace({ -50998.0, 1502.0, 2610.0 }, { -50998.0, 1502.0, 2605.0 }, t), "a segment ending before the plane hit it");
			std::printf("segment: a triangle far from the origin is hit at its analytic t and missed beside it\n");
		}
		Rig rig;
		// The doors the navmesh furnishing spawns give the doorway the opening case casts through.
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		test::require(nav != nullptr, "the Hub mesh did not load");
		rig.sim->enableNavigation(nav);
		spawnMeshDoors(*rig.sim, *nav); // the Hub's doors: a doorway is the opening case's gap
		rig.sim->enableLineOfSight();
		test::require(rig.sim->lineOfSightEnabled(), "line of sight did not enable");
		const auto builtFrom = std::chrono::steady_clock::now();
		rig.areas({{ kBarX, kBarZ }});
		const double builtMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - builtFrom).count();
		const LineOfSight& los = rig.los();
		const std::int32_t barRegion = los.regionOf(kBarX, kBarZ);
		test::require(los.loaded(barRegion), "the Bar's region is not loaded for Full detail");
		const std::size_t hubTriangles = los.triangles(barRegion);
		test::require(hubTriangles > 0, "the Bar's region has no collision triangles");
		std::printf("hub: %zu regions, the Bar's has %zu triangles, %.1f KiB, built in %.0f ms (buildings, doors, foliage)\n", los.loadedRegions(),
			hubTriangles, static_cast<double>(los.memoryBytes()) / 1024.0, builtMs);

		// The Bar's own collision, for the geometry of the cases below.
		const kworld::PlacedInstance* bar = nearest(rig.sim->world().placements(), kBarX, kBarZ, 60.0);
		test::require(bar != nullptr, "no Bar placement near the Hub Bar");
		const double ground = bar->y;
		kworld::CollisionLibrary library(KENSHI_DATA, *rig.gd);
		kworld::CollisionCoverage coverage;
		std::vector<kworld::WorldShape> barShapes;
		library.addBuilding(*bar, barShapes, coverage);
		const std::vector<Tri> barTris = trianglesOf(barShapes);
		test::require(!barTris.empty(), "the Bar has no collision triangles");
		std::printf("bar: %s, %zu shapes, %zu triangles, ground y %.1f\n", bar->buildingID.c_str(), barShapes.size(), barTris.size(), ground);

		// ---- 1. the tree agrees with a brute-force scan ----
		{
			const kworld::HullTree* tree = los.tree(barRegion);
			test::require(tree != nullptr, "the Bar's region has no tree");
			std::uint64_t rng = 0x9e3779b97f4a7c15ull;
			const auto next = [&rng]()
			{
				rng ^= rng << 13;
				rng ^= rng >> 7;
				rng ^= rng << 17;
				return static_cast<double>(rng >> 11) / 9007199254740992.0; // [0, 1)
			};
			int hits = 0;
			for (int i = 0; i < 400; ++i)
			{
				std::array<double, 3> a{ kBarX + (next() - 0.5) * 200.0, ground + next() * 220.0, kBarZ + (next() - 0.5) * 200.0 };
				std::array<double, 3> b{ kBarX + (next() - 0.5) * 200.0, ground + next() * 220.0, kBarZ + (next() - 0.5) * 200.0 };
				double fast = 0.0;
				double slow = 0.0;
				const bool byTree = tree->trace(a, b, fast);
				const bool brute = tree->traceBrute(a, b, slow);
				test::require(byTree == brute && (!byTree || std::fabs(fast - slow) < 1.0e-9),
					"the tree and the brute-force scan disagree on a random ray");
				hits += byTree ? 1 : 0;
			}
			test::require(hits > 5, "the random rays found almost nothing (" + std::to_string(hits) + ")");
			std::printf("tree: 400 random rays match the brute-force scan (%d blocked)\n", hits);
		}

		// ---- 2. a wall blocks ----
		const Tri* wall = nullptr;
		for (const Tri& tri : barTris)
		{
			if (std::fabs(tri.n[1]) < 0.3 && tri.centre[1] > ground + 6.0 && tri.area > 300.0
				&& (wall == nullptr || tri.area > wall->area))
				wall = &tri;
		}
		test::require(wall != nullptr, "the Bar has no large vertical wall triangle");
		const std::array<double, 3> normal{ wall->n[0], 0.0, wall->n[2] };
		const auto across = [&](double height)
		{
			// Well clear of the wall's own face on the near side of the pair's eye points (the Bar has other parts close by).
			return std::pair<std::array<double, 3>, std::array<double, 3>>{
				std::array<double, 3>{ wall->centre[0] + normal[0] * 20.0, height, wall->centre[2] + normal[2] * 20.0 },
				std::array<double, 3>{ wall->centre[0] - normal[0] * 20.0, height, wall->centre[2] - normal[2] * 20.0 } };
		};
		{
			const auto [a, b] = across(wall->centre[1]);
			const LineOfSight::Hit hit = los.trace(a, b);
			test::require(hit.blocked && hit.hull, "a Bar wall does not block the eye line across it");
			test::require(hit.t > 0.0 && hit.t < 1.0, "the wall hit is at an end (" + num(hit.t) + ")");
			std::printf("wall: blocked at t %.3f (centre y %.1f)\n", hit.t, wall->centre[1]);
		}

		// ---- 3. an opening passes: a window, a doorway ----
		// Hoisted for section 9 (the same opening as a perception outcome).
		bool foundWall = false;
		double passX = 0.0;
		double passY = 0.0;
		double passAlong = 0.0;
		std::array<double, 3> openingNormal{};
		std::array<double, 3> passPoint{}; // the first passing line's point on the wall's plane (sections 9, 10)
		{
			// Find a wall of the Bar with an opening in it: scan short lines through each large wall triangle's plane, along the
			// wall and up the building. One that passes somewhere and blocks elsewhere has a window or a doorway (the slab itself
			// is not collision, so a doorway is a hole).
			int open = 0;
			int blocked = 0;
			for (const Tri& tri : barTris)
			{
				if (std::fabs(tri.n[1]) > 0.3 || tri.area < 300.0 || tri.centre[1] < ground + 6.0)
					continue;
				const std::array<double, 3> n{ tri.n[0], 0.0, tri.n[2] };
				const std::array<double, 3> tangent{ -n[2], 0.0, n[0] };
				int localOpen = 0;
				int localBlocked = 0;
				for (double along = -40.0; along <= 40.0; along += 2.0)
					for (double height = tri.centre[1] - 30.0; height < tri.centre[1] + 60.0; height += 6.0)
					{
						const std::array<double, 3> a{ tri.centre[0] + tangent[0] * along + n[0] * 6.0, height,
							tri.centre[2] + tangent[2] * along + n[2] * 6.0 };
						const std::array<double, 3> b{ tri.centre[0] + tangent[0] * along - n[0] * 6.0, height,
							tri.centre[2] + tangent[2] * along - n[2] * 6.0 };
						if (los.trace(a, b).blocked)
							++localBlocked;
						else {
							if (localOpen == 0)
							{
								passX = tri.centre[0] + tangent[0] * along;
								passY = height;
								passAlong = along;
								passPoint = { passX, height, tri.centre[2] + tangent[2] * along };
							}
							++localOpen;
						}
					}
				if (localOpen > 0 && localBlocked > 0)
				{
					foundWall = true;
					openingNormal = n;
					open += localOpen;
					blocked += localBlocked;
					break;
				}
			}
			test::require(foundWall, "no Bar wall has an opening (a passing and a blocked line through its plane)");
			std::printf("opening: a wall with an opening passes %d lines and blocks %d (first pass at %.1f along, y %.1f, x %.1f)\n",
				open, blocked, passAlong, passY, passX);
		}

		// ---- 4. a floor blocks ----
		const Tri* slab = nullptr; // hoisted for section 9
		{
			for (const Tri& tri : barTris)
			{
				if (std::fabs(tri.n[1]) > 0.85 && tri.centre[1] > ground + 25.0 && tri.area > 300.0 && (slab == nullptr || tri.area > slab->area))
					slab = &tri;
			}
			test::require(slab != nullptr, "the Bar has no large horizontal slab");
			const std::array<double, 3> below{ slab->centre[0], slab->centre[1] - 25.0, slab->centre[2] };
			const std::array<double, 3> above{ slab->centre[0], slab->centre[1] + 25.0, slab->centre[2] };
			const LineOfSight::Hit up = los.trace(below, above);
			const LineOfSight::Hit down = los.trace(above, below);
			test::require(up.blocked && up.hull && down.blocked && down.hull, "a Bar floor does not block up and down");
			std::printf("floor: blocked up t %.3f and down t %.3f (slab y %.1f)\n", up.t, down.t, slab->centre[1]);
		}

		// ---- 5. a low obstacle: blocked at its own height, seen over above it ----
		std::array<double, 3> lowBodyA{}, lowBodyB{}, lowEyeA{}, lowEyeB{}; // kept for section 9
		{
			// A low solid of the Bar (a floor ledge, a sill, a table): a line through it at its own height is blocked, and the
			// same line a little above its top is clear (the air over it).
			bool checked = false;
			double lowTop = 0.0;
			double bodyT = 0.0;
			double seenAt = 0.0;
			double width = 0.0;
			for (const kworld::WorldShape& shape : barShapes)
			{
				if (checked)
					break;
				const double height = shape.max[1] - shape.min[1];
				const double span = std::max(shape.max[0] - shape.min[0], shape.max[2] - shape.min[2]);
				if (height <= 1.0 || height > 30.0 || span < 6.0 || span > 250.0)
					continue;
				const double cx = 0.5 * (shape.min[0] + shape.max[0]);
				const double cz = 0.5 * (shape.min[2] + shape.max[2]);
				const bool wide = (shape.max[0] - shape.min[0]) >= (shape.max[2] - shape.min[2]);
				const double half = 0.25 * (wide ? (shape.max[0] - shape.min[0]) : (shape.max[2] - shape.min[2]));
				const auto at = [&](double y)
				{
					return wide ? std::pair<std::array<double, 3>, std::array<double, 3>>{ { cx - half, y, cz }, { cx + half, y, cz } }
								: std::pair<std::array<double, 3>, std::array<double, 3>>{ { cx, y, cz - half }, { cx, y, cz + half } };
				};
				const auto [ba, bb] = at(shape.min[1] + 0.5 * height);
				const LineOfSight::Hit bodyHit = los.trace(ba, bb);
				if (!bodyHit.blocked || !bodyHit.hull)
					continue;
				for (double y = shape.max[1] + 6.0; y < shape.max[1] + 50.0 && !checked; y += 3.0)
				{
					const auto [ea, eb] = at(y);
					if (!los.trace(ea, eb).blocked)
					{
						checked = true;
						lowTop = shape.max[1];
						bodyT = bodyHit.t;
						seenAt = y;
						width = span;
						lowBodyA = ba;
						lowBodyB = bb;
						lowEyeA = ea;
						lowEyeB = eb;
					}
				}
			}
			test::require(checked, "no low part of the Bar's collision blocks at its height and is seen over");
			std::printf("low: a Bar part %.1f wide, top %.1f, blocks at its own height t %.2f and is clear %.1f above its top\n", width,
				lowTop, bodyT, seenAt - lowTop);
		}

		// ---- 6. memory: the Hub's regions and the biggest town region ----
		{
			std::map<std::int32_t, std::size_t> placementsPerRegion;
			for (const kworld::PlacedInstance& placed : rig.sim->world().placements())
				++placementsPerRegion[los.regionOf(placed.x, placed.z)];
			std::vector<std::pair<std::size_t, std::int32_t>> ranked; // placements, region
			for (const auto& [region, count] : placementsPerRegion)
				ranked.emplace_back(count, region);
			std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
			test::require(!ranked.empty(), "no region has a placement");
			// The biggest town: of the busiest regions, the one whose buildings carry the most collision (a wasteland region's
			// placements are resource markers with no geometry).
			LineOfSight measure(KENSHI_DATA, *rig.gd, rig.sim->world().placements(), los.regionsPerAxis(), los.regionSize());
			std::int32_t biggest = -1;
			std::size_t most = 0;
			std::size_t biggestTriangles = 0;
			for (std::size_t i = 0; i < std::min<std::size_t>(ranked.size(), 5); ++i)
			{
				const std::int32_t region = ranked[i].second;
				measure.ensure(region);
				if (measure.triangles(region) > biggestTriangles)
				{
					biggestTriangles = measure.triangles(region);
					biggest = region;
					most = ranked[i].first;
				}
			}
			test::require(biggest >= 0 && biggestTriangles > 0, "none of the busiest regions has any collision");
			std::printf("memory: the Hub %zu regions %.1f KiB; the biggest town region (%d, %zu placements) %.1f KiB, %zu triangles\n",
				los.loadedRegions(), static_cast<double>(los.memoryBytes()) / 1024.0, biggest, most,
				static_cast<double>(measure.triangles(biggest) * 9 * sizeof(float) + measure.triangles(biggest) / 8 * 2 * sizeof(float) * 8) / 1024.0,
				biggestTriangles);
		}

		// ---- 7. a region is freed when it leaves Full detail; across a border both trees answer ----
		{
			rig.areas({{ 20000.0, -20000.0 }});
			test::require(!rig.los().loaded(barRegion), "the Bar's region was not freed after the player left it");
			const LineOfSight& moved = rig.los();
			std::int32_t far = moved.regionOf(20000.0, -20000.0);
			test::require(moved.loaded(far), "the new area's region is not loaded");
			// Two centres in adjacent regions around the Hub.
			double x0 = 0.0, z0 = 0.0, x1 = 0.0, z1 = 0.0;
			moved.regionBounds(barRegion, x0, z0, x1, z1);
			const std::int32_t neighbour = barRegion + 1; // the region one column east
			double nx0 = 0.0, nz0 = 0.0, nx1 = 0.0, nz1 = 0.0;
			moved.regionBounds(neighbour, nx0, nz0, nx1, nz1);
			rig.areas({{ kBarX, kBarZ }, { 0.5 * (nx0 + nx1), 0.5 * (nz0 + nz1) }});
			const LineOfSight& both = rig.los();
			test::require(both.loaded(barRegion) && both.loaded(neighbour), "the two adjacent regions are not both loaded");
			const std::uint64_t beforeA = both.regionQueries(barRegion);
			const std::uint64_t beforeB = both.regionQueries(neighbour);
			const double boundary = x1;
			const std::array<double, 3> west{ boundary - 40.0, ground + 40.0, kBarZ };
			const std::array<double, 3> east{ boundary + 40.0, ground + 40.0, kBarZ };
			both.trace(west, east);
			test::require(both.regionQueries(barRegion) > beforeA && both.regionQueries(neighbour) > beforeB,
				"a ray across the border did not consult both regions' trees");
			std::printf("regions: the Bar's freed on unload, a cross-border ray consulted both trees\n");
		}

		// ---- 8. cost ----
		{
			rig.areas({{ kBarX, kBarZ }});
			const LineOfSight& back = rig.los();
			std::uint64_t rng = 0x243f6a8885a308d3ull;
			const auto next = [&rng]()
			{
				rng ^= rng << 13;
				rng ^= rng >> 7;
				rng ^= rng << 17;
				return static_cast<double>(rng >> 11) / 9007199254740992.0;
			};
			// Eye height over the local ground: the terrain blocks a ray that starts underground.
			const kworld::Heightmap terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");
			const int rays = 5000;
			int rays_used = 0;
			std::vector<std::pair<std::array<double, 3>, std::array<double, 3>>> samples;
			samples.reserve(rays);
			// The rays of perception in the bar_crowd scale: eye to eye between characters standing on the walkable ground.
			std::vector<std::array<double, 3>> eyes;
			for (int tried = 0; tried < rays * 20 && eyes.size() < static_cast<std::size_t>(rays) * 2; ++tried)
			{
				const double x = kBarX + (next() - 0.5) * 400.0;
				const double z = kBarZ + (next() - 0.5) * 400.0;
				if (const auto spot = nav->walker().snap({ x, terrain.terrainHeight(x, z) + 40.0, z }, 60.0))
					eyes.push_back({ spot->x, spot->y + 18.0, spot->z });
			}
			test::require(eyes.size() >= 100, "too few walkable sample points near the Bar");
			for (int i = 0; i + 1 < static_cast<int>(eyes.size()) && static_cast<int>(samples.size()) < rays; i += 2)
				samples.push_back({ eyes[static_cast<std::size_t>(i)], eyes[static_cast<std::size_t>(i + 1)] });
			rays_used = static_cast<int>(samples.size());
			std::pair<double, double> lastWork{ 0.0, 0.0 }; // nodes, triangles tested per ray by the last run
			const auto run = [&](const std::vector<std::pair<std::array<double, 3>, std::array<double, 3>>>& set)
			{
				int blocked = 0;
				kworld::HullTree::work() = {};
				const auto start = std::chrono::steady_clock::now();
				for (const auto& [a, b] : set)
					blocked += back.trace(a, b).blocked ? 1 : 0;
				const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
				const double perRay = seconds / static_cast<double>(set.size()) * 1.0e9;
				const kworld::HullTree::Work done = kworld::HullTree::work();
				const double n = static_cast<double>(set.size());
				lastWork = { static_cast<double>(done.nodes) / n, static_cast<double>(done.triangles) / n };
				return std::pair<double, int>{ perRay, blocked };
			};
			const int streetRays = rays_used;
			const auto [perRay, blocked] = run(samples);
			const auto streetWork = lastWork;
			// Above the roofs: rays that cross the whole region, the longest traversal a sight line can have.
			std::vector<std::pair<std::array<double, 3>, std::array<double, 3>>> high;
			high.reserve(rays);
			for (const auto& [a, b] : samples)
				high.push_back({ { a[0], a[1] + 200.0, a[2] }, { b[0], b[1] + 200.0, b[2] } });
			const auto [perRayHigh, blockedHigh] = run(high);
			const auto roofWork = lastWork;
			// bar_crowd: 28 bandits against the bar's residents (~12), indoor sight 500: every pair is a candidate.
			const int crowd = 40;
			const double pairs = crowd * (crowd - 1) / 2.0;
			std::printf("cost: street level %.0f ns/ray (%.2f us), %d of %d blocked; above the roofs %.0f ns/ray (%d blocked)\n", perRay,
				perRay / 1000.0, blocked, streetRays, perRayHigh, blockedHigh);
			std::printf("cost: bar_crowd %d characters -> %.0f pairs, %.2f ms/tick at street level\n", crowd, pairs,
				pairs * perRay / 1.0e6);
			std::printf("cost: work per ray, street %.2f nodes %.2f triangles; roofs %.2f nodes %.2f triangles\n", streetWork.first,
				streetWork.second, roofWork.first, roofWork.second);
			// The gate is the deterministic work (box and triangle tests per ray), not host time: the us/ray above is info only.
			test::require(streetWork.first < kStreetNodes && streetWork.second < kStreetTris && roofWork.first < kRoofNodes && roofWork.second < kRoofTris,
				"a ray's work: street " + num(streetWork.first) + " nodes / " + num(streetWork.second) + " tris, roofs " + num(roofWork.first)
				+ " / " + num(roofWork.second));
		}

		// ---- 9. perception: the same geometry as sight outcomes (LINE_OF_SIGHT 4 tests 1-4) ----
		{
			// An eye (+18 over the feet) to eye ray through `lineOfSight`, one scan of one observer aimed at one target; then
			// the perception answer. Fresh ids per call keep the pair cache out of the cases.
			std::uint64_t nextId = 100;
			const auto sees = [&](const std::array<double, 3>& eyeA, const std::array<double, 3>& eyeB)
			{
				state::Fighter observer;
				observer.x = eyeA[0];
				observer.y = eyeA[1] - LineOfSight::kEyeHeight;
				observer.z = eyeA[2];
				const double dx = eyeB[0] - eyeA[0];
				const double dz = eyeB[2] - eyeA[2];
				observer.yaw = std::atan2(dx, dz);
				state::Fighter target;
				target.x = eyeB[0];
				target.y = eyeB[1] - LineOfSight::kEyeHeight;
				target.z = eyeB[2];
				target.yaw = std::atan2(-dx, -dz);
				perception::Environment env;
				env.lineOfSight = [&los](std::uint64_t a, const state::Fighter& fa, std::uint64_t b, const state::Fighter& fb)
				{
					return los.visible(a, fa, b, fb);
				};
				const std::uint64_t observerId = nextId++;
				const std::uint64_t targetId = nextId++;
				std::vector<perception::Noticed> noticed;
				perception::scan(observerId, observer, 1.0, 1.0, {{ targetId, &target }}, env, noticed);
				return perception::aware(observer, targetId);
			};
			// 1. a wall: the pair of section 2, at eye height on opposite sides of the Bar's wall - no sight.
			{
				const auto [a, b] = across(wall->centre[1]);
				test::require(!sees(a, b), "a Bar wall does not stop perception's sight");
				std::printf("perception: a wall at eye height stops sight\n");
			}
			// 2. an opening: the pair of section 3, in the opening's line - sight holds.
			{
				test::require(foundWall, "the opening case needs section 3's gap");
				const std::array<double, 3> a{ passPoint[0] + openingNormal[0] * 6.0, passPoint[1], passPoint[2] + openingNormal[2] * 6.0 };
				const std::array<double, 3> b{ passPoint[0] - openingNormal[0] * 6.0, passPoint[1], passPoint[2] - openingNormal[2] * 6.0 };
				test::require(sees(a, b), "an opening in the wall does not let perception see through");
				std::printf("perception: an opening passes sight\n");
			}
			// 3. a floor: the roof/floor pair of section 4 - no sight either way.
			{
				const std::array<double, 3> below{ slab->centre[0], slab->centre[1] - 25.0, slab->centre[2] };
				const std::array<double, 3> above{ slab->centre[0] + 4.0, slab->centre[1] + 25.0, slab->centre[2] };
				test::require(!sees(below, above) && !sees(above, below), "a Bar floor does not stop perception up and down");
				std::printf("perception: a floor stops sight both ways\n");
			}
			// 4. a low obstacle: the pair of section 5, standing over it - sight holds (the body-height line is section 5's block).
			{
				test::require(sees(lowEyeA, lowEyeB), "a standing pair does not see over a low obstacle");
				std::printf("perception: a standing pair sees over a low obstacle\n");
			}
		}
		// ---- 10. the pair cache and the shot query ----
		{
			state::Fighter a;
			a.x = passPoint[0] + openingNormal[0] * 6.0;
			a.y = passPoint[1] - LineOfSight::kEyeHeight;
			a.z = passPoint[2] + openingNormal[2] * 6.0;
			state::Fighter b = a;
			b.x = passPoint[0] - openingNormal[0] * 6.0;
			b.z = passPoint[2] - openingNormal[2] * 6.0;
			const std::uint64_t casts0 = los.pairCasts();
			const std::uint64_t hits0 = los.pairHits();
			const bool first = los.visible(900, a, 901, b);
			const std::uint64_t casts1 = los.pairCasts();
			test::require(casts1 == casts0 + 1, "the first sight pair is not cast");
			test::require(los.visible(900, a, 901, b) == first && los.pairCasts() == casts1 && los.pairHits() == hits0 + 1,
				"a stationary pair is re-cast (the cache did not answer)");
			state::Fighter moved = b;
			moved.x += 3.0; // past kPairMove
			test::require(los.visible(900, a, 901, moved) == first && los.pairCasts() == casts1 + 1,
				"a pair that moved more than kPairMove is not re-cast");
			los.setTick(11); // older than kPairTicks
			los.visible(900, a, 901, b);
			test::require(los.pairCasts() == casts1 + 2, "a pair older than kPairTicks is not re-cast");
			std::printf("cache: %llu casts, %llu hits; move and TTL re-cast\n",
				static_cast<unsigned long long>(los.pairCasts()), static_cast<unsigned long long>(los.pairHits()));
			// The shot query (ranged combat is not implemented: FEATURES "Crossbows"): a wall blocks a muzzle-to-body line,
			// an opening passes it, and a body away from Full detail is never blocked (coarse).
			{
				const auto [ma, mb] = across(wall->centre[1]);
				state::Fighter target;
				target.x = mb[0];
				target.y = mb[1] - LineOfSight::kEyeHeight;
				target.z = mb[2];
				test::require(los.shotBlocked(ma, target), "a Bar wall does not block a shot");
				const LineOfSight::Hit stop = los.firstBlocker(ma, target);
				test::require(stop.blocked && stop.hull && stop.t > 0.0 && stop.t < 1.0, "the first blocker of a walled-off shot is not a hull short of the target");
				target.x = b.x;
				target.y = b.y;
				target.z = b.z;
				test::require(!los.shotBlocked({ a.x, a.y + 18.0, a.z }, target), "an opening does not pass a shot");
				test::require(!los.firstBlocker({ a.x, a.y + 18.0, a.z }, target).blocked, "an opening has a first blocker");
				state::Fighter far;
				far.x = 20000.0;
				far.z = -20000.0;
				test::require(!los.loaded(los.regionOf(far.x, far.z)), "the coarse case's region is unexpectedly loaded");
				test::require(!los.shotBlocked(ma, far), "a body away from Full detail is blocked");
				std::printf("shot: a wall blocks, an opening passes, a coarse region never blocks\n");
			}
		}

		// ---- 11. the Simulation turns it on and gives the arena's perception the hook ----
		{
			Simulation autoSim = Simulation::load(KENSHI_DATA, 92);
			autoSim.setGameData(rig.gd); // the fixtures' path: the first setGameData enables it
			test::require(autoSim.lineOfSightEnabled(), "a world with placements did not enable line of sight by itself");
			autoSim.enableCombat(CombatData::build(*rig.gd, KENSHI_DATA));
			autoSim.enqueue(Command::setPlayerAreas({{ kBarX, kBarZ }}));
			autoSim.tick();
			const perception::Environment& env = autoSim.combat()->environment();
			test::require(static_cast<bool>(env.lineOfSight), "the arena's perception has no line-of-sight hook");
			const auto [a, b] = across(wall->centre[1]);
			state::Fighter near;
			near.x = a[0];
			near.y = a[1] - LineOfSight::kEyeHeight;
			near.z = a[2];
			state::Fighter far;
			far.x = b[0];
			far.y = b[1] - LineOfSight::kEyeHeight;
			far.z = b[2];
			test::require(!env.lineOfSight(1, near, 2, far), "the arena's hook lets sight through the Bar's wall");
			// Away from Full detail (a region without a tree) the hook answers visible: coarse perception is unchanged.
			near.x = 300000.0;
			near.z = 300000.0;
			far.x = 300040.0;
			far.z = 300000.0;
			test::require(env.lineOfSight(3, near, 4, far), "a pair away from Full detail is blocked");
			std::printf("wiring: the simulation enables it, the arena hook blocks across the wall and passes away from Full detail\n");
		}

		// ---- 12. a doorway's leaf blocks while its door is shut and passes while it is open or broken (k16.13.4) ----
		{
			// The Bar's own door (the one the Hub furnishing spawns near it): its leaf from the collision library, the eye line along the
			// door's way-in normal through the middle of the leaf.
			flecs::entity doorEntity;
			rig.sim->world().ecs().each([&](flecs::entity e, const state::Door& d)
			{
				if (std::hypot(d.x - kBarX, d.z - kBarZ) < 400.0 && los.doorLeafTriangles(d.key) > 0 && !doorEntity.is_valid())
					doorEntity = e;
			});
			test::require(doorEntity.is_valid(), "no Hub door with a leaf in the tree near the Bar");
			const state::Door door = *doorEntity.get<state::Door>();
			std::array<double, 3> lo{ 1e30, 1e30, 1e30 };
			std::array<double, 3> hi{ -1e30, -1e30, -1e30 };
			for (const kworld::PlacedInstance& candidate : rig.sim->world().placements())
			{
				if (std::hypot(candidate.x - door.x, candidate.z - door.z) > 120.0)
					continue;
				std::vector<kworld::WorldShape> leafShapes;
				kworld::CollisionCoverage leafCoverage;
				library.addBuilding(candidate, leafShapes, leafCoverage);
				for (const kworld::WorldShape& shape : leafShapes)
					if (shape.use == kworld::NavUse::Door && shape.door == door.key)
						for (int k = 0; k < 3; ++k)
						{
							lo[k] = std::min<double>(lo[k], shape.min[k]);
							hi[k] = std::max<double>(hi[k], shape.max[k]);
						}
			}
			test::require(lo[0] < hi[0], "the door's leaf has no bounds");
			const std::array<double, 3> middle{ 0.5 * (lo[0] + hi[0]), 0.5 * (lo[1] + hi[1]), 0.5 * (lo[2] + hi[2]) };
			const double reach = 0.5 * (hi[0] - lo[0] + hi[2] - lo[2]) + 12.0;
			const std::array<double, 3> near{ middle[0] + door.inX * reach, middle[1], middle[2] + door.inZ * reach };
			const std::array<double, 3> far{ middle[0] - door.inX * reach, middle[1], middle[2] - door.inZ * reach };
			state::Fighter inside;
			inside.x = near[0];
			inside.y = near[1] - LineOfSight::kEyeHeight;
			inside.z = near[2];
			state::Fighter outside = inside;
			outside.x = far[0];
			outside.z = far[2];
			const auto setDoor = [&](std::uint8_t value)
			{
				doorEntity.get_mut<state::Door>()->state = value;
				rig.sim->tick(); // syncLineOfSight reads the doors
			};
			// The shut door: its leaf stops the line, as a wall does.
			setDoor(1);
			test::require(!los.doorOpen(door.key), "a shut door reads open for sight");
			const LineOfSight::Hit shut = los.trace(near, far);
			test::require(shut.blocked && shut.hull && shut.t > 0.0 && shut.t < 1.0, "a shut door's leaf does not block the eye line");
			test::require(!los.visible(950, inside, 951, outside), "perception sees through a shut door");
			setDoor(2);
			test::require(!los.visible(950, inside, 951, outside), "perception sees through a locked door");
			// Open (0) and broken (3) pass it; the pair cache answers afresh the same tick (it is dropped when the door changes).
			setDoor(0);
			test::require(los.doorOpen(door.key), "an open door reads shut for sight");
			test::require(!los.trace(near, far).blocked, "an open door's leaf still blocks the eye line");
			test::require(los.visible(950, inside, 951, outside), "perception does not see through an open door (or the pair cache kept the shut answer)");
			setDoor(1);
			test::require(!los.visible(950, inside, 951, outside), "a door that shut again does not block a warm pair");
			setDoor(3);
			test::require(los.visible(950, inside, 951, outside), "perception does not see through a broken door");
			setDoor(1);
			std::printf("door: leaf %zu triangles; shut/locked block, open/broken pass, the pair cache follows (first hit t %.3f)\n",
				los.doorLeafTriangles(door.key), shut.t);
		}

		// ---- 13. a foliage rock between two characters (k16.13.4) ----
		{
			// The biggest instance of the Bar's zone cell with collision, tested on a tree of its own shapes first, so the case does not
			// depend on the region tree it checks: a line across its middle that its own hull stops.
			const kworld::Heightmap terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");
			const kworld::BiomeMap biomes = kworld::BiomeMap::load(std::string(KENSHI_DATA) + "/newland/land/biomemap.png");
			const kworld::Features features = kworld::Features::join(*rig.gd, KENSHI_DATA);
			const kworld::Towns towns = kworld::Towns::join(*rig.gd, KENSHI_DATA);
			const kworld::ZoneCell cell = kworld::zoneCellOf(kBarX, kBarZ);
			struct Rock
			{
				std::string record;
				std::array<double, 3> from{}, to{};
				double volume = 0.0;
			};
			Rock best;
			int tried = 0;
			double x0 = 0.0, z0 = 0.0, x1 = 0.0, z1 = 0.0;
			los.regionBounds(barRegion, x0, z0, x1, z1);
			for (const kworld::FoliageInstance& tree : kworld::generateFoliage(cell, terrain, biomes, *rig.gd, features, towns))
			{
				std::vector<kworld::WorldShape> shapes;
				kworld::CollisionCoverage rockCoverage;
				if (!library.addFoliage(tree, shapes, rockCoverage) || shapes.empty() || shapes.front().use == kworld::NavUse::Projected)
					continue;
				if (tree.x < x0 + 50.0 || tree.x > x1 - 50.0 || tree.z < z0 + 50.0 || tree.z > z1 - 50.0)
					continue;
				std::array<double, 3> lo{ 1e30, 1e30, 1e30 };
				std::array<double, 3> hi{ -1e30, -1e30, -1e30 };
				for (const kworld::WorldShape& shape : shapes)
					for (int k = 0; k < 3; ++k)
					{
						lo[k] = std::min<double>(lo[k], shape.min[k]);
						hi[k] = std::max<double>(hi[k], shape.max[k]);
					}
				const double volume = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
				if (volume <= best.volume || hi[1] - lo[1] < 12.0)
					continue;
				++tried;
				kworld::HullTree own;
				own.build(shapes);
				const double mid = 0.5 * (lo[1] + hi[1]);
				const double cx = 0.5 * (lo[0] + hi[0]);
				const double cz = 0.5 * (lo[2] + hi[2]);
				const double reachX = 0.5 * (hi[0] - lo[0]) + 10.0;
				const std::array<double, 3> from{ cx - reachX, mid, cz };
				const std::array<double, 3> to{ cx + reachX, mid, cz };
				double t = 0.0;
				if (own.trace(from, to, t) && terrain.terrainHeight(from[0], from[2]) < mid - 1.0 && terrain.terrainHeight(to[0], to[2]) < mid - 1.0)
					best = Rock{ tree.recordID, from, to, volume };
			}
			test::require(best.volume > 0.0, "no foliage rock with collision in the Bar's zone cell (" + std::to_string(tried) + " tried)");
			state::Fighter west;
			west.x = best.from[0];
			west.y = best.from[1] - LineOfSight::kEyeHeight;
			west.z = best.from[2];
			state::Fighter east = west;
			east.x = best.to[0];
			east.z = best.to[2];
			test::require(los.loaded(los.regionOf(west.x, west.z)), "the rock's region is not loaded");
			const LineOfSight::Hit stop = los.trace(best.from, best.to);
			test::require(stop.blocked && stop.hull && stop.t > 0.0 && stop.t < 1.0, "a foliage rock (" + best.record + ") does not block the eye line across it");
			test::require(!los.visible(960, west, 961, east), "perception sees through a foliage rock");
			test::require(los.shotBlocked({ best.from[0], best.from[1], best.from[2] }, east), "a shot passes through a foliage rock");
			std::printf("rock: %s (volume %.0f) blocks sight and a shot\n", best.record.c_str(), best.volume);
		}

		std::cout << "ksim_los: ok\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_los: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
