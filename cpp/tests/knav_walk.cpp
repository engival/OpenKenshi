// The movement step (knav/walk.hpp) on the baked Hub: stays on the mesh, slides along edges,
// reports blocked, follows a route to its end exactly, y comes from the surface and agrees with
// the terrain on open ground, and it is deterministic.

#include "knav/navmesh.hpp"
#include "knav/walk.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"
#include "hub_point.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using knav::Drive;
using knav::Position;

constexpr double kPi = 3.14159265358979323846;

struct Rng
{
	std::uint64_t s = 88172645463325252ull;
	double next() // [0, 1)
	{
		s ^= s << 13;
		s ^= s >> 7;
		s ^= s << 17;
		return static_cast<double>(s >> 11) / 9007199254740992.0;
	}
};

struct Pt
{
	double x;
	double z;
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: knav_walk CACHE");
		const auto mesh = knav::NavMesh::load(argv[1]);
		const auto terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");
		const knav::Walker walker(mesh);

		const auto start = walker.snap({ kHubX, terrain.terrainHeight(kHubX, kHubZ), kHubZ }, 200.0);
		test::require(start.has_value(), "the Hub centre has no mesh within 200 units");

		// Strafe (k16.8.17): on open ground from the Hub centre, heading 0.7. A strafe alone moves perpendicular to the heading at
		// kStrafeFactor x speed; W+A covers exactly speed x dt (normalised), S+A 0.707 x; the old no-strafe call is unchanged.
		{
			const double heading = 0.7, speed = 10.0, dt = 0.05;
			const double fx = std::sin(heading), fz = std::cos(heading); // the facing
			const auto move = [&](Drive mode, int strafe)
			{
				const knav::StepResult r = walker.drive(*start, mode, heading, speed, dt, true, strafe);
				return Pt{ r.at.x - start->x, r.at.z - start->z };
			};
			const double full = speed * dt;
			const Pt side = move(Drive::Stop, 1);
			test::require(std::fabs(side.x * fx + side.z * fz) < 2e-3, "a strafe moved along the facing");
			test::require(std::fabs(std::hypot(side.x, side.z) - knav::kStrafeFactor * full) < 2e-3, "a strafe is not kStrafeFactor x speed");
			test::require(side.x * std::cos(heading) - side.z * std::sin(heading) > 0.0, "strafe +1 is not to the right of the heading");
			const Pt left = move(Drive::Stop, -1);
			test::require(std::fabs(left.x + side.x) < 2e-3 && std::fabs(left.z + side.z) < 2e-3, "strafe -1 is not the mirror of +1");
			const Pt wa = move(Drive::Forward, -1);
			test::require(std::fabs(std::hypot(wa.x, wa.z) - full) < 2e-3, "W+A is not the gait x dt");
			test::require(wa.x * fx + wa.z * fz > 0.0, "W+A does not go forward");
			const Pt sa = move(Drive::Backward, -1);
			test::require(std::fabs(std::hypot(sa.x, sa.z) - 0.70710678 * full) < 2e-3, "S+A is not 0.707 x the gait x dt");
			test::require(sa.x * fx + sa.z * fz < 0.0, "S+A does not go back");
			const Pt fwd = move(Drive::Forward, 0);
			test::require(std::fabs(std::hypot(fwd.x, fwd.z) - full) < 2e-3 && std::fabs(fwd.x - fx * full) < 2e-3, "forward changed");
			const Pt back = move(Drive::Backward, 0);
			test::require(std::fabs(std::hypot(back.x, back.z) - knav::kBackpedalFactor * full) < 2e-3, "backward changed");
			// The ramped overload: the speed along the move direction ramps to gait x |local|; a stop with no strafe zeroes it.
			double fs = 0.0;
			for (int i = 0; i < 200; ++i)
				walker.drive(*start, Drive::Stop, heading, speed, fs, dt, 60.0, true, 1);
			test::require(std::fabs(fs - knav::kStrafeFactor * speed) < 1e-6, "the strafe ramp did not reach kStrafeFactor x gait");
			walker.drive(*start, Drive::Stop, heading, speed, fs, dt, 60.0, true, 0);
			test::require(fs == 0.0, "a stop with no strafe left a speed");
		}

		// Random drive: never leaves the mesh, y tracks the terrain, moves are never longer than asked.
		Rng rng;
		Position p = *start;
		long steps = 0;
		long blockedSteps = 0;
		double overshoot = 0.0;
		constexpr double kWallEndSlack = 0.5; // units; the worst blocked step of the seed's 6000 is printed below
		double worstY = 0.0;
		long nearTerrain = 0;
		for (int i = 0; i < 6000; ++i)
		{
			const double heading = rng.next() * 2.0 * kPi;
			const Drive mode = rng.next() < 0.2 ? Drive::Backward : Drive::Forward;
			const double speed = 40.0 + rng.next() * 60.0; // up to the run speeds of the combat data
			const knav::StepResult r = walker.drive(p, mode, heading, speed, 0.05);
			test::require(r.onMesh, "a driven step started off the mesh at step " + std::to_string(i));
			const double asked = speed * 0.05 * (mode == Drive::Backward ? knav::kBackpedalFactor : 1.0);
			// A step that ends against a wall may move a little more than asked: Detour's moveAlongSurface returns the wall edge's
			// point nearest the target, which at the end of an edge is its corner, not a point on the asked line. Until 18289478 the
			// bake eroded the mesh a body width off the walls, so the random drive never reached a border edge; with the wall band kept
			// as Area::Edge the border edges are the real walls and the drive slides along them (step 1589 of this seed: 4.99 asked
			// 4.68, blocked). An unblocked step stays strict.
			overshoot = std::max(overshoot, r.moved - asked);
			test::require(r.moved <= asked + (r.blocked ? kWallEndSlack : 0.02), "moved more than asked: " + std::to_string(r.moved) + " vs "
				+ std::to_string(asked) + " at step " + std::to_string(i) + (r.blocked ? " (blocked)" : ""));
			test::require(walker.onMesh(r.at), "ended off the mesh at step " + std::to_string(i));
			// Open ground (no building deck within reach): the surface agrees with the terrain.
			const double dy = std::fabs(r.at.y - terrain.terrainHeight(r.at.x, r.at.z));
			worstY = std::max(worstY, dy);
			nearTerrain += dy <= 3.0 ? 1 : 0;
			blockedSteps += r.blocked ? 1 : 0;
			++steps;
			p = r.at;
		}
		std::cout << "random drive: " << steps << " steps, " << blockedSteps << " blocked, worst |y - terrain| "
			<< worstY << ", within 3 units of the terrain: " << nearTerrain << ", worst overshoot of a step " << overshoot << "\n";
		test::require(nearTerrain * 10 >= steps * 8, "the surface is mostly not at the terrain height");
		test::require(blockedSteps > 0, "6000 random steps in the Hub never met an edge");
		// 60, was 40: with the wall band kept in the mesh (18289478, no bake-time erosion) the random drive climbs a raised surface it
		// never reached before, at about (-50709, 2937): 45 u above the terrain, reached by a steady rise (about 2 u a step) and with no
		// mesh at ground level under it (a roof or wall slope inside the 40 degree limit; UNSURE which structure). The bound still
		// catches a surface height that is plainly wrong.
		test::require(worstY < 60.0, "surface height is far from the terrain (decks aside)");

		// Blocked and sliding: run into an edge, then move along it.
		bool slid = false;
		int trials = 0;
		for (int t = 0; t < 400 && !slid; ++t)
		{
			Position q = *start;
			const double heading = rng.next() * 2.0 * kPi;
			knav::StepResult r;
			for (int i = 0; i < 400; ++i)
			{
				r = walker.drive(q, Drive::Forward, heading, 100.0, 0.05);
				q = r.at;
				if (r.blocked)
					break;
			}
			if (!r.blocked)
				continue;
			++trials;
			// A step along the edge: one of the two sideways headings must make progress.
			for (const double turn : { 0.6, -0.6, 1.2, -1.2 })
			{
				const knav::StepResult along = walker.drive(q, Drive::Forward, heading + turn, 100.0, 0.05);
				test::require(along.onMesh && walker.onMesh(along.at), "the slide left the mesh");
				const double wanted = 5.0;
				if (along.moved > 0.5 * wanted * std::cos(turn) && along.blocked)
				{
					slid = true; // moved a good part of the way while the wall still stopped some of it
					break;
				}
				if (along.moved > 0.25 * wanted)
					slid = true;
			}
		}
		std::cout << "edge trials: " << trials << ", slid: " << slid << "\n";
		test::require(slid, "never slid along an edge after hitting one");

		// Stop does not move; backward goes the other way.
		const knav::StepResult still = walker.drive(*start, Drive::Stop, 1.0, 100.0, 0.05);
		test::require(still.onMesh && still.moved == 0.0 && still.at.x == start->x, "stop moved");

		// A route is followed to its end exactly.
		const knav::NavQuery query(mesh);
		const knav::Point from = { static_cast<float>(start->x), static_cast<float>(start->y), static_cast<float>(start->z) };
		int routes = 0;
		for (int t = 0; t < 40 && routes < 8; ++t)
		{
			const knav::Point to = { from[0] + static_cast<float>(rng.next() * 600.0 - 300.0), from[1],
				from[2] + static_cast<float>(rng.next() * 600.0 - 300.0) };
			const knav::PathResult path = query.findPath(from, to, { 40.0f, 80.0f, 40.0f });
			if (!path.complete || path.points.size() < 3)
				continue;
			++routes;
			std::vector<Pt> route;
			for (const knav::Point& w : path.points)
				route.push_back({ w[0], w[2] });
			Position at = { path.points.front()[0], path.points.front()[1], path.points.front()[2] };
			std::size_t next = 0;
			double travelled = 0.0;
			int guard = 0;
			int vertexRefusals = 0;
			while (next < route.size() && ++guard < 4000)
			{
				const knav::StepResult r = walker.follow(at, route, next, 100.0 * 0.05);
				test::require(r.onMesh, "route follow left the mesh");
				// Detour's moveAlongSurface can refuse a short hop that starts exactly on a polygon vertex (the
				// waypoint a hop has just reached; the Hub's storm house ramp corner (-51090, 3154) does it for
				// some hop lengths): the walk is only blocked there if the next full hop from the vertex is too.
				// A hop that ENDS on a vertex is not refused any more (NavQuery::slide retries a hair short, k16.8.6).
				// Nothing else may block a planned route.
				if (r.blocked)
				{
					test::require(next > 0 && std::hypot(r.at.x - route[next - 1].x, r.at.z - route[next - 1].z) < 1e-3,
						"a planned route was blocked");
					++vertexRefusals;
				}
				travelled += r.moved;
				at = r.at;
			}
			test::require(vertexRefusals <= 2, "a planned route was refused at a vertex repeatedly");
			test::require(next == route.size(), "route not finished");
			test::require(std::hypot(at.x - route.back().x, at.z - route.back().z) < 0.01, "route end not reached exactly");
			test::require(travelled < path.length * 1.02 + 1.0, "walked far more than the route is long");
		}
		test::require(routes >= 3, "too few routable pairs found");

		// Determinism: the same drive twice gives the same positions.
		Position a = *start;
		Position b = *start;
		Rng r1;
		Rng r2;
		for (int i = 0; i < 500; ++i)
		{
			const double h1 = r1.next() * 6.28;
			const double h2 = r2.next() * 6.28;
			a = walker.drive(a, Drive::Forward, h1, 90.0, 0.05).at;
			b = walker.drive(b, Drive::Forward, h2, 90.0, 0.05).at;
		}
		test::require(a.x == b.x && a.y == b.y && a.z == b.z, "not deterministic");
		// A cliff the polygon links cross (k16.8.27): the Hub's upper floor edge at (-51159, 2582), y 1646.6, links to the floor 66 u below. A
		// push over it, in one step or in small ones, stops at the edge: no hop changes height by more than a stair's rise.
		{
			const auto edge = walker.snap({ -51159.0, 1646.6, 2582.0 }, 2.0);
			if (edge && std::fabs(edge->y - 1646.6) < 4.0)
			{
				for (const double push : { 0.16, 1.0, 8.0, 30.0 })
				{
					Position at = *edge;
					for (int i = 0; i < 400 && std::hypot(at.x - edge->x, at.z - edge->z) < 40.0; ++i)
						at = walker.step(at, 0.0, -push).at;
					test::require(std::fabs(at.y - edge->y) < 4.0, "a push over the upper floor edge dropped the body to the floor below");
				}
			}
		}
		std::cout << "knav_walk ok\n";
	}
	catch (const std::exception& e)
	{
		std::cerr << "knav_walk: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
