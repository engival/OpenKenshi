// A door shuts on a character standing in its doorway (kenshi-k16.6.552, k16.6.551; findings/rwh231_door_close_on_character_live.md,
// findings/rwh232_door_close_occupancy_static.md): the original's close has NO occupancy rule (it never waits, refuses or pushes) and
// the occupant keeps a route: from the pivot of a shut locked gate a move to the inside walks. So the router and the walker must start
// from a locked door polygon: a locked poly refuses entry (excluded from the search) but not a body already on it.
// Loads the Hub cache (argv[1]); for every door with polygons: stand on its first polygon, lock the door, and require a route out
// to the points around it, the walker's slide to leave the polygon, and a path that ENTERS the door still refused.

#include "knav/door.hpp"
#include "knav/navmesh.hpp"
#include "test_util.hpp"

#include <DetourNavMesh.h>

#include <cmath>
#include <iostream>
#include <vector>

namespace {

using knav::DoorKey;
using knav::DoorState;
using knav::Point;

const Point kReach = { 6.0f, 12.0f, 6.0f };

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: knav_door_occupied CACHE");
		knav::NavMesh mesh = knav::NavMesh::load(argv[1]);
		const knav::NavQuery query(mesh);
		knav::TravelFilter anyDoor;
		anyDoor.throughLocks = true;
		std::size_t doors = 0;
		std::size_t routed = 0;
		std::size_t slid = 0;
		for (const DoorKey key : mesh.doorKeys())
		{
			const std::vector<knav::PolyLoc> polys = mesh.doorPolys(key);
			if (polys.empty())
				continue;
			++doors;
			// The centre of the door's first polygon.
			const dtMeshTile* tile = mesh.detour().getTileAt(polys[0].tileX, polys[0].tileZ, 0);
			const dtPoly& poly = tile->polys[polys[0].poly];
			Point stand{};
			for (int v = 0; v < poly.vertCount; ++v)
				for (int a = 0; a < 3; ++a)
					stand[a] += tile->verts[poly.verts[v] * 3 + a] / static_cast<float>(poly.vertCount);
			// Open: the points around the doorway a body can walk to (on the mesh, off this door's polygons).
			knav::DoorBook book;
			mesh.applyDoors(book);
			std::vector<Point> around;
			for (const float radius : { 8.0f, 14.0f, 24.0f })
				for (int step = 0; step < 16; ++step)
				{
					const double angle = step * 0.39269908;
					const Point at = { stand[0] + static_cast<float>(std::cos(angle)) * radius, stand[1],
						stand[2] + static_cast<float>(std::sin(angle)) * radius };
					Point on;
					if (!query.nearestPoint(at, { 1.0f, 4.0f, 1.0f }, on) || std::fabs(on[1] - stand[1]) > 6.0f)
						continue;
					bool onDoor = false;
					knav::PolyLoc there;
					if (query.locate(on, { 0.5f, 2.0f, 0.5f }, there, anyDoor))
						for (const knav::PolyLoc& p : polys)
							onDoor = onDoor || (p.tileX == there.tileX && p.tileZ == there.tileZ && p.poly == there.poly);
					const knav::PathResult open = query.findPath(stand, on, kReach);
					if (!onDoor && open.complete)
						around.push_back(on);
				}
			std::cout << "door " << key << " polys " << polys.size() << " at " << stand[0] << " " << stand[1] << " " << stand[2] << " around " << around.size() << "\n";
			if (around.empty())
				continue;
			// Locked, with the character still on it: the close does not move anyone (the original).
			book.setState(key, DoorState::Locked);
			mesh.applyDoors(book);
			std::size_t out = 0;
			for (const Point& target : around)
				out += query.findPath(stand, target, kReach).complete ? 1 : 0;
			test::require(out > 0, "a character on a locked door polygon has no route out of it");
			routed += out > 0;
			// The walker: a slide from the polygon toward a reachable point moves.
			bool moved = false;
			for (const Point& target : around)
			{
				Point to;
				bool blocked = false;
				if (query.slide(stand, target, kReach, to, blocked) && std::hypot(to[0] - stand[0], to[2] - stand[2]) > 0.5f)
					moved = true;
			}
			test::require(moved, "the walker cannot leave a locked door polygon it stands on");
			slid += moved;
			// A locked door still refuses entry: a start off it through it is not planned (no door crossing for the traveller).
			for (const Point& target : around)
			{
				const knav::PathResult back = query.findPath(target, stand, kReach);
				test::require(!back.complete || back.doors.empty() || back.doors.back() == key,
					"a path ended on a locked door crosses another door");
			}
			// Unlocked again: as baked.
			book.setState(key, DoorState::Open);
			mesh.applyDoors(book);
		}
		std::cout << "doors " << doors << ", with a route out of the locked leaf " << routed << ", walker left it " << slid << "\n";
		test::require(doors > 0, "no door to test");
		return 0;
	} catch (const std::exception& e)
	{
		std::cerr << e.what() << "\n";
		return 1;
	}
}
