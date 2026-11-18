// The Hub copper node's navmesh cut and its stand point (kenshi-k16.19.4 / .19.5). The node's collision is a pile of plates (a cooked
// shell); the bake must cut the xz cover of its triangles, not the hull of the whole pile, so the gaps and the ground beside it stay walkable and
// the miner stands against the rocks. Checks, on the native Hub mesh (MeshNavigator::openNative, the one the server and the playtest run on; exit 77 without the navtiles):
//   - no walkable ground inside the collision's cover (a miner never stands inside the drawn rock);
//   - every unwalkable point near the node lies within a few units of that cover (the cut follows the collision);
//   - the miner's approach route for OPERATE_MACHINERY 87 ends at the original's approach point (65 u from the origin), beside the rocks;
// The cut area and the cover area are printed.

#include "ksim/world/interact.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/state.hpp"

#include "knav/native.hpp"

#include "kdata/format.hpp"
#include "kworld/collision.hpp"
#include "kworld/features.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/loadorder.hpp"
#include "kworld/towns.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kNodeX = -52734.0; // the playtest's copper node (journey_mine v1, kclient capture)
constexpr double kNodeZ = 1879.0;
constexpr double kStep = 2.0; // the sample grid
constexpr double kEdge = 3.0; // a point this deep inside the cover counts as inside it (the cut is cell-sized: the mesh bakes no erosion)
constexpr double kSlack = 4.0; // "within a few units": the cut's extra reach beyond the cover (cells, polygon edges)
constexpr double kAgentRadius = 4.0; // the route's clearance radius (CombatData::pathRadius, human)

struct Tri
{
	double x[3];
	double z[3];
};

bool inside(const Tri& t, double px, double pz)
{
	const auto side = [&](int a, int b) { return (t.x[b] - t.x[a]) * (pz - t.z[a]) - (t.z[b] - t.z[a]) * (px - t.x[a]); };
	const double s0 = side(0, 1);
	const double s1 = side(1, 2);
	const double s2 = side(2, 0);
	return (s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0);
}

double segmentDistance(double px, double pz, double ax, double az, double bx, double bz)
{
	const double dx = bx - ax;
	const double dz = bz - az;
	const double length2 = dx * dx + dz * dz;
	const double t = length2 > 0.0 ? std::clamp(((px - ax) * dx + (pz - az) * dz) / length2, 0.0, 1.0) : 0.0;
	return std::hypot(px - (ax + t * dx), pz - (az + t * dz));
}

// 0 inside the cover of the triangles, else the distance to the nearest triangle edge.
double distanceToCover(const std::vector<Tri>& tris, double px, double pz)
{
	double best = std::numeric_limits<double>::max();
	for (const Tri& t : tris)
	{
		if (inside(t, px, pz))
			return 0.0;
		for (int k = 0; k < 3; ++k)
			best = std::min(best, segmentDistance(px, pz, t.x[k], t.z[k], t.x[(k + 1) % 3], t.z[(k + 1) % 3]));
	}
	return best;
}

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cout << "ksim_mine_node_cut: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const kdata::GameData data = kworld::loadMergedGameData(order);
		// The mesh the server runs on (k16.7.2): the shipped tiles, the stale Hub zone rebaked with our recipe (the playtest's capture: "nav tiles 2899").
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::openNative(KENSHI_DATA, {kworld::zoneCellOf(kNodeX, kNodeZ)}, [](const std::string&) {}, &data);

		// The node: the producer foliage instance nearest the capture's spot, its collision placed as the bake places it.
		const std::string dir = KENSHI_DATA;
		const kworld::Heightmap terrain = kworld::Heightmap::open(dir + "/newland/land/fullmap.tif");
		const kworld::BiomeMap biomes = kworld::BiomeMap::load(dir + "/newland/land/biomemap.png");
		const kworld::Features features = kworld::Features::join(data, dir);
		const kworld::Towns towns = kworld::Towns::join(data, dir);
		kworld::CollisionLibrary collision(dir, data);
		const kworld::ZoneCell cell = kworld::zoneCellOf(kNodeX, kNodeZ);
		std::vector<kworld::WorldShape> shapes;
		double originX = 0, originY = 0, originZ = 0, best = 1e30;
		for (const kworld::FoliageInstance& tree : kworld::generateFoliage(cell, terrain, biomes, data, features, towns))
		{
			const kdata::Record* mesh = data.find(tree.recordID);
			if (mesh == nullptr || mesh->findRefs("building type") == nullptr || std::hypot(tree.x - kNodeX, tree.z - kNodeZ) >= best)
				continue;
			std::vector<kworld::WorldShape> own;
			kworld::CollisionCoverage coverage;
			if (!collision.addFoliage(tree, own, coverage) || own.empty())
				continue;
			best = std::hypot(tree.x - kNodeX, tree.z - kNodeZ);
			shapes = std::move(own);
			originX = tree.x;
			originY = tree.y;
			originZ = tree.z;
		}
		test::require(best < 60.0, "the copper node is in the Hub zone's foliage (nearest " + std::to_string(best) + " u)");

		std::vector<Tri> tris;
		double minX = 1e30, maxX = -1e30, minZ = 1e30, maxZ = -1e30;
		for (const kworld::WorldShape& shape : shapes)
		{
			for (std::size_t t = 0; t + 2 < shape.indices.size(); t += 3)
			{
				Tri tri;
				for (int k = 0; k < 3; ++k)
				{
					tri.x[k] = shape.vertices[shape.indices[t + k] * 3];
					tri.z[k] = shape.vertices[shape.indices[t + k] * 3 + 2];
					minX = std::min(minX, tri.x[k]);
					maxX = std::max(maxX, tri.x[k]);
					minZ = std::min(minZ, tri.z[k]);
					maxZ = std::max(maxZ, tri.z[k]);
				}
				tris.push_back(tri);
			}
		}
		test::require(!tris.empty(), "the node's collision has triangles");

		// The cut, sampled: a point is cut when no walkable ground lies within half a step.
		double coverArea = 0, cutArea = 0, farCut = 0, standingOnRock = 0, worstFar = 0;
		for (double z = minZ - 40; z <= maxZ + 40; z += kStep)
		{
			for (double x = minX - 40; x <= maxX + 40; x += kStep)
			{
				const double d = distanceToCover(tris, x, z);
				const bool walkable = nav->nearestWalkable(x, z, kStep * 0.5).has_value();
				coverArea += d == 0.0 ? kStep * kStep : 0.0;
				cutArea += walkable ? 0.0 : kStep * kStep;
				// Deep inside: the cover on every side too (a point at the cover's edge is within a cell of walkable ground).
				bool deep = d == 0.0 && walkable;
				for (int k = 0; k < 8 && deep; ++k)
					deep = distanceToCover(tris, x + kEdge * std::cos(k * 0.785398), z + kEdge * std::sin(k * 0.785398)) == 0.0;
				standingOnRock += deep ? kStep * kStep : 0.0;
				if (!walkable && d > kSlack)
				{
					farCut += kStep * kStep;
					worstFar = std::max(worstFar, d);
				}
			}
		}
		std::cout << "copper node at " << originX << "," << originZ << ": collision cover " << coverArea << " u2, unwalkable " << cutArea << " u2 (within " << kSlack
			<< " u of the cover: all but " << farCut << " u2, worst " << worstFar << " u off it); walkable inside the cover: " << standingOnRock << " u2\n";
		test::require(standingOnRock == 0.0, "no walkable ground inside the collision's cover");
		test::require(farCut == 0.0, "the cut stays within " + std::to_string(kSlack) + " u of the collision (" + std::to_string(farCut) + " u2 beyond, worst " + std::to_string(worstFar) + ")");

		// The miner's approach for OPERATE_MACHINERY 87 (the Use action): ordered from 150 u south of the node.
		InteractTarget target;
		target.kind = InteractKind::Producer;
		target.x = originX;
		target.y = originY;
		target.z = originZ;
		target.reach = 100.0;
		target.reachStopped = target.reach; // what findInteractTarget gives a producer
		target.reachY = kProducerReachY;
		target.boxMinX = minX;
		target.boxMaxX = maxX;
		target.boxMinZ = minZ;
		target.boxMaxZ = maxZ;
		state::Fighter miner;
		const auto start = nav->nearestWalkable(originX, originZ - 150.0, 40.0);
		test::require(start.has_value(), "walkable ground 150 u south of the node");
		miner.x = start->x;
		miner.z = start->z;
		miner.y = originY;
		const Route route = routeToInteract(*nav, miner, target, static_cast<std::uint8_t>(InteractAction::Use), false, kAgentRadius);
		test::require(route.found && !route.points.empty(), "the miner routes to the node");
		const double endX = route.points.back().x;
		const double endZ = route.points.back().z;
		const double gap = distanceToCover(tris, endX, endZ);
		std::cout << "collision box x " << minX << ".." << maxX << " z " << minZ << ".." << maxZ << ", origin " << originX << "," << originZ << " (" << distanceToCover(tris, originX, originZ)
			<< " u from the cover); miner from " << miner.x << "," << miner.z << " ends " << endX << "," << endZ << "\n";
		std::cout << "the miner's route ends " << std::hypot(endX - originX, endZ - originZ) << " u from the origin, " << gap << " u from the collision\n";
		// The original's rule (item59 MQ9): the approach point is 65 u from the origin toward the actor, so it lies kProducerApproach minus the rock's
		// reach on that side from the collision (the pile spans ~45 u about its origin: ~20 u). Not the brief's ~10 u: deferred: playtesting.
		test::require(std::fabs(std::hypot(endX - originX, endZ - originZ) - kProducerApproach) < 1.0 && gap <= 25.0,
			"the miner stands at the approach point, " + std::to_string(gap) + " u from the collision");
		test::require(target.nearEnough(endX, route.heights.back(), endZ, target.reachStopped), "and counts as arrived");
		std::cout << "ksim_mine_node_cut ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_mine_node_cut FAILED: " << e.what() << "\n";
		return 1;
	}
}
