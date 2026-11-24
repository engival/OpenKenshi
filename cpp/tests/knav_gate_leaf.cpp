// A pivot gate locks with bodies on and around its leaf (kenshi-k16.6.562; findings/rwh231_door_close_on_character_live.md c3a c4 c6 c11,
// rwh232, rwh244): the lock refuses a PLAN, not a body. On Heft's gate (67157, -72071):
//  1. a body on the leaf when it locks routes out to the INSIDE and to the OUTSIDE (rwh.231 c4/c6: the occupant is not caged; the original's
//     close has no occupancy rule and the router starts from the shut band);
//  2. a walker planned through the gate while it was open, crossing as it locks, reaches its goal (c3a, rwh.244: no stop, re-plan or deflection),
//     through Walker::follow with the doors the route was planned through; with none given (a route planned after the lock) it is stopped;
//  3. a walker that plans after the lock is refused (c11: no route through the shut gate) as before.
// argv[1]: the world bake dir (region_11_4.knav). Exit 77 without it.

#include "knav/door.hpp"
#include "knav/navmesh.hpp"
#include "knav/walk.hpp"
#include "test_util.hpp"

#include <DetourNavMesh.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <vector>

namespace {

using knav::Point;

const Point kReach = { 6.0f, 12.0f, 6.0f };
constexpr double kHeftGateX = 67157.30;
constexpr double kHeftGateZ = -72071.85;

} // namespace

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	try
	{
		if (argc < 2 || !std::filesystem::exists(std::string(argv[1]) + "/region_11_4.knav"))
		{
			std::printf("no region_11_4.knav: the test needs the world bake\n");
			return 77;
		}
		knav::NavMesh mesh = knav::NavMesh::loadRegion(argv[1], 11, 4, 1);
		const knav::NavQuery query(mesh);
		// The gate: the door whose centre is nearest the user's reported spot.
		knav::DoorKey gate = 0;
		Point centre{};
		knav::NavMesh::DoorFootprint foot;
		double best = 150.0;
		for (const knav::DoorKey key : mesh.doorKeys())
		{
			Point c;
			knav::NavMesh::DoorFootprint f;
			if (!mesh.doorCentre(key, c, &f))
				continue;
			const double d = std::hypot(c[0] - kHeftGateX, c[2] - kHeftGateZ);
			if (d < best)
			{
				best = d;
				gate = key;
				centre = c;
				foot = f;
			}
		}
		test::require(gate != 0, "no door near Heft's gate");
		std::printf("gate %llu centre %.1f %.1f %.1f axis %.2f %.2f long %.1f deep %.1f (%.1f u from the reported spot)\n", static_cast<unsigned long long>(gate), centre[0], centre[1], centre[2], foot.axisX, foot.axisZ,
			foot.halfLong, foot.halfDeep, best);
		// The crossing direction is across the slab's long axis.
		const float nx = -foot.axisZ;
		const float nz = foot.axisX;
		knav::DoorBook book;
		mesh.applyDoors(book);
		// Ground points on each side of the doorway, reachable from each other while it is open.
		std::vector<Point> sides[2];
		for (const float d : { foot.halfDeep + 10.0f, foot.halfDeep + 20.0f, foot.halfDeep + 35.0f })
			for (const float lateral : { -10.0f, 0.0f, 10.0f })
				for (int s = 0; s < 2; ++s)
				{
					const float sign = s == 0 ? 1.0f : -1.0f;
					const Point at = { centre[0] + sign * nx * d + foot.axisX * lateral, centre[1], centre[2] + sign * nz * d + foot.axisZ * lateral };
					Point on;
					if (query.nearestPoint(at, { 2.0f, 8.0f, 2.0f }, on) && std::fabs(on[1] - centre[1]) < 8.0f)
						sides[s].push_back(on);
				}
		test::require(!sides[0].empty() && !sides[1].empty(), "no ground on a side of the gate");
		std::printf("ground points: side A %zu, side B %zu\n", sides[0].size(), sides[1].size());
		// Open: a route from A to B through the gate and no other door.
		bool crossed = false;
		Point a, b;
		for (const Point& pa : sides[0])
			for (const Point& pb : sides[1])
			{
				const knav::PathResult open = query.findPath(pa, pb, kReach);
				if (!crossed && open.complete && open.doors.size() == 1 && open.doors[0] == gate && std::hypot(pa[0] - pb[0], pa[2] - pb[2]) > 30.0f)
				{
					crossed = true;
					a = pa;
					b = pb;
				}
			}
		test::require(crossed, "no route through the open gate between the sides");

		// 1. The occupant: on the leaf when it locks.
		knav::TravelFilter any;
		any.throughLocks = true;
		const std::vector<knav::PolyLoc> polys = mesh.doorPolys(gate);
		test::require(!polys.empty(), "the gate has no polygons");
		const dtMeshTile* tile = mesh.detour().getTileAt(polys[0].tileX, polys[0].tileZ, 0);
		const dtPoly& poly = tile->polys[polys[0].poly];
		Point stand{};
		for (int v = 0; v < poly.vertCount; ++v)
			for (int k = 0; k < 3; ++k)
				stand[k] += tile->verts[poly.verts[v] * 3 + k] / static_cast<float>(poly.vertCount);
		book.setState(gate, knav::DoorState::Locked);
		mesh.applyDoors(book);
		int out[2] = { 0, 0 };
		for (int s = 0; s < 2; ++s)
			for (const Point& target : sides[s])
				out[s] += query.findPath(stand, target, kReach).complete ? 1 : 0;
		std::printf("occupant routes out: side A %d of %zu, side B %d of %zu\n", out[0], sides[0].size(), out[1], sides[1].size());
		test::require(out[0] > 0, "a body on the locked leaf has no route out to one side");
		test::require(out[1] > 0, "a body on the locked leaf has no route out to the other side");
		// ... and the walker leaves it either way.
		knav::Walker walker(mesh);
		for (int s = 0; s < 2; ++s)
		{
			knav::Position at{ stand[0], stand[1], stand[2] };
			const Point& to = sides[s].front();
			const knav::StepResult r = walker.step(at, to[0] - at.x, to[2] - at.z);
			test::require(r.moved > 0.5, "the walker cannot leave the locked leaf to a side");
		}

		// 3. A plan after the lock is refused.
		for (const Point& pa : sides[0])
			for (const Point& pb : sides[1])
			{
				const knav::PathResult shut = query.findPath(pa, pb, kReach);
				bool through = false;
				for (const knav::DoorKey door : shut.doors)
					through = through || door == gate;
				if (through)
					std::printf("through: complete %d doors %zu len %.1f\n", shut.complete, shut.doors.size(), shut.length);
				test::require(!through, "a route planned after the lock goes through the shut gate");
			}

		// 2. A walker planned while open, crossing as it locks.
		book.setState(gate, knav::DoorState::Open);
		mesh.applyDoors(book);
		const knav::PathResult plan = query.findPath(a, b, kReach);
		test::require(plan.complete && plan.doors.size() == 1, "the open route is gone");
		book.setState(gate, knav::DoorState::Locked);
		mesh.applyDoors(book);
		std::vector<knav::Position> route;
		for (const Point& p : plan.points)
			route.push_back({ p[0], p[1], p[2] });
		const auto walk = [&](const std::vector<knav::DoorKey>& crossing)
		{
			knav::Position at = route.front();
			std::size_t next = 1;
			for (int tick = 0; tick < 200 && next < route.size(); ++tick)
				at = walker.follow(at, route, next, 3.0, 0.0, crossing).at;
			return std::hypot(at.x - b[0], at.z - b[2]);
		};
		const double with = walk({ gate });
		const double without = walk({});
		std::printf("walker planned open, gate locked: %.1f u from the goal through the route's doors, %.1f u without\n", with, without);
		test::require(with < 3.0, "a walker crossing as the gate locks does not reach its goal");
		test::require(without > 10.0, "the locked gate does not stop a walker that was not planned through it");
		return 0;
	} catch (const std::exception& e)
	{
		std::cerr << e.what() << "\n";
		return 1;
	}
}
