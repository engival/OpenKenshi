// Doors and gates as navmesh links (docs/formats/NAVMESH_CACHE.md "Doors"): loads the Hub cache
// (argv[1]) and checks that door state and per-class access change path cost and reachability
// without rebuilding any tile, in the detailed query and in the coarse graph.

#include "knav/area.hpp"
#include "knav/coarse.hpp"
#include "knav/door.hpp"
#include "knav/navmesh.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <DetourNavMesh.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>

namespace {

using knav::DoorKey;
using knav::DoorState;
using knav::Point;

const Point kReach = { 40.0f, 60.0f, 40.0f };
constexpr int kResident = 0;
constexpr int kStranger = 1; // a class the door refuses

// A house door the street reaches an interior floor through (the Hub's gates are ruins: no doors, user-checked 2026-11-04).
struct Doorway
{
	DoorKey key = 0;
	Point outside{};
	Point inside{};
};

Point groundPoint(const kworld::Heightmap& terrain, double x, double z)
{
	return { static_cast<float>(x), static_cast<float>(terrain.height(x, z)), static_cast<float>(z) };
}

bool crosses(const std::vector<DoorKey>& doors, DoorKey door)
{
	return std::find(doors.begin(), doors.end(), door) != doors.end();
}

knav::TravelFilter asClass(int doorClass)
{
	knav::TravelFilter filter;
	filter.doorClass = static_cast<std::uint8_t>(doorClass);
	return filter;
}

// The tile slots of the Detour mesh: a rebuilt or re-added tile gets another salt, so equal refs
// before and after mean no tile was touched.
std::vector<dtTileRef> tileRefs(const knav::NavMesh& mesh)
{
	std::vector<dtTileRef> refs;
	for (const knav::BakedTile& tile : mesh.tiles())
		refs.push_back(mesh.detour().getTileRefAt(tile.x, tile.z, 0));
	return refs;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: knav_door CACHE");
		knav::NavMesh mesh = knav::NavMesh::load(argv[1]);
		const std::string dataDir = KENSHI_DATA;
		const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const knav::AreaGeometry area = knav::gatherArea(dataDir, terrain, { 19, 31 }, { 21, 33 });

		// Census: the doors the Hub placements carry, and which got polygons in the mesh. A door whose
		// doorway is filled by a building has none (nothing to link).
		std::vector<Doorway> ways;
		std::size_t gatePolys = 0;
		std::size_t doors = 0;
		std::size_t linked = 0;
		std::size_t buried = 0;
		for (const knav::AreaBuilding& building : area.buildings)
		{
			for (const DoorKey key : building.nav.doors)
			{
				++doors;
				linked += !mesh.doorPolys(key).empty();
				if (mesh.doorPolys(key).empty())
				{
					// Why: the doorway stands in the building's solid fill. The collision of a house is a closed
					// hull without the opening, so its footprint prism removes the ground under the door mat, and a
					// closed shape covers the doorway: nothing walkable within the mat's height window (the sill
					// minus the climb to the sill plus the agent height) near the door.
					for (const kworld::WorldShape& s : *building.shapes)
					{
						if (s.use != kworld::NavUse::Door || s.door != key)
							continue;
						const double cx = (s.min[0] + s.max[0]) / 2;
						const double cz = (s.min[2] + s.max[2]) / 2;
						const double ground = terrain.height(cx, cz);
						knav::Point ignored;
						const bool bare = !knav::NavQuery(mesh).nearestPoint(
							{ static_cast<float>(cx), s.min[1] + 6.5f, static_cast<float>(cz) }, { 12.0f, 11.5f, 12.0f }, ignored);
						bool filled = false;
						for (const kworld::WorldShape& o : *building.shapes)
							filled = filled || (o.use != kworld::NavUse::Door && o.closed && o.min[0] <= cx && o.max[0] >= cx
								&& o.min[2] <= cz && o.max[2] >= cz && o.min[1] <= ground && o.max[1] >= ground);
						buried += bare && filled;
						if (bare && filled)
							std::cout << "  no door polygons, doorway inside the fill of " << building.id << "\n";
					}
				}
				gatePolys += building.nav.gateway && !mesh.doorPolys(key).empty();
			}
		}
		std::cout << "hub doors " << doors << ", with polygons " << linked << ", gates with polygons " << gatePolys << "\n";
		std::cout << "doors without polygons because the building fills their doorway: " << buried << "\n";
		// 24 linked before the tilted-hull fix (2026-11-01); 25 until ruins lost their doors (7d6ac14: 21 of the Hub's buildings are
		// ruins, as live; the live Hub mesh has 3 door faces, NAVMESH_CACHE "Runtime mesh").
		test::require(doors == 4 && linked == 4, "the Hub door census changed");
		test::require(buried == doors - linked, "a Hub door has no polygons for a reason other than the building fill");
		test::require(gatePolys == 0, "a ruined Hub gate has door polygons");

		// The leaf frame the client's door pick moves a leaf by (kinspect door_pick.md): unit, orthogonal
		// axes; a box's corners at its centre +- the half extents along them; the actor origin nearby.
		std::size_t boxes = 0;
		for (const knav::AreaBuilding& building : area.buildings)
		{
			for (const kworld::WorldShape& s : *building.shapes)
			{
				if (s.use != kworld::NavUse::Door)
					continue;
				const auto dot = [&](int a, int b)
				{
					return s.axes[3 * a] * s.axes[3 * b] + s.axes[3 * a + 1] * s.axes[3 * b + 1] + s.axes[3 * a + 2] * s.axes[3 * b + 2];
				};
				for (int a = 0; a < 3; ++a)
					for (int b = 0; b < 3; ++b)
						test::require(std::fabs(dot(a, b) - (a == b ? 1.0f : 0.0f)) < 1e-3f, "a door shape's axes are not orthonormal");
				test::require(std::hypot(s.actor[0] - building.placed.x, s.actor[2] - building.placed.z) < 3000.0,
					"a door shape's actor origin is far from its placement");
				if (s.half[0] <= 0.0f)
					continue;
				++boxes;
				const std::size_t count = s.vertices.size() / 3;
				std::array<float, 3> centre{};
				for (std::size_t i = 0; i < count; ++i)
					for (int axis = 0; axis < 3; ++axis)
						centre[axis] += s.vertices[i * 3 + axis] / static_cast<float>(count);
				for (std::size_t i = 0; i < count; ++i)
					for (int j = 0; j < 3; ++j)
					{
						float along = 0.0f;
						for (int axis = 0; axis < 3; ++axis)
							along += (s.vertices[i * 3 + axis] - centre[axis]) * s.axes[3 * j + axis];
						test::require(std::fabs(std::fabs(along) - s.half[j]) < 0.05f * s.half[j] + 0.5f,
							"a door box's corner is not at its half extent along its axis");
					}
			}
		}
		std::cout << "door box shapes with a checked frame: " << boxes << "\n";
		test::require(boxes > 0, "no Hub door is a box shape");
		test::require(mesh.doorKeys().size() == linked, "doorKeys disagrees with the census");

		// Door polygons are walkable passage: same ground, only a different area.
		for (const DoorKey key : mesh.doorKeys())
			for (const knav::PolyLoc& at : mesh.doorPolys(key))
			{
				const dtMeshTile* tile = mesh.detour().getTileAt(at.tileX, at.tileZ, 0);
				test::require(tile->polys[at.poly].getArea() == static_cast<int>(knav::Area::DoorOpen),
					"a door polygon is not baked open");
			}

		// Houses: interiors are in the bake, so a door leads somewhere. From the street, through the door,
		// to a floor inside the building (not terrain), and where one exists up to a higher floor or roof.
		{
			const knav::NavQuery walk(mesh);
			const Point tight = { 3.0f, 3.0f, 3.0f };
			std::size_t houseDoors = 0;
			std::size_t entered = 0;
			std::size_t climbed = 0;
			for (const knav::AreaBuilding& building : area.buildings)
			{
				if (building.nav.gateway)
					continue;
				std::array<float, 3> lo = { 1e30f, 1e30f, 1e30f };
				std::array<float, 3> hi = { -1e30f, -1e30f, -1e30f };
				for (const kworld::WorldShape& o : *building.shapes)
					for (int a = 0; a < 3; ++a)
					{
						lo[a] = std::min(lo[a], o.min[a]);
						hi[a] = std::max(hi[a], o.max[a]);
					}
				const auto indoors = [&](const Point& p)
				{
					return p[0] > lo[0] + 4 && p[0] < hi[0] - 4 && p[2] > lo[2] + 4 && p[2] < hi[2] - 4
						&& p[1] > terrain.height(p[0], p[2]) + 4;
				};
				for (const DoorKey key : building.nav.doors)
				{
					if (mesh.doorPolys(key).empty())
						continue;
					++houseDoors;
					const kworld::WorldShape* slab = nullptr;
					for (const kworld::WorldShape& s : *building.shapes)
						if (s.use == kworld::NavUse::Door && s.door == key)
							slab = &s;
					const double cx = (slab->min[0] + slab->max[0]) / 2;
					const double cz = (slab->min[2] + slab->max[2]) / 2;
					Point mat;
					if (!walk.nearestPoint({ static_cast<float>(cx), slab->min[1] + 6.0f, static_cast<float>(cz) }, { 12.0f, 12.0f, 12.0f }, mat))
						continue;
					// The street: terrain level, outside the building, nearest to the door, on the mesh.
					bool haveStreet = false;
					Point street{};
					float streetDistance = 1e30f;
					for (int step = 0; step < 24; ++step)
						for (const float radius : { 40.0f, 70.0f })
						{
							const double angle = step * 0.2617994;
							const Point at = groundPoint(terrain, cx + std::cos(angle) * radius, cz + std::sin(angle) * radius);
							Point onMesh;
							if (at[0] > lo[0] && at[0] < hi[0] && at[2] > lo[2] && at[2] < hi[2])
								continue;
							if (!walk.nearestPoint(at, { 2.0f, 3.0f, 2.0f }, onMesh) || std::fabs(onMesh[1] - at[1]) > 3.0f)
								continue;
							if (radius < streetDistance)
							{
								street = onMesh;
								streetDistance = radius;
								haveStreet = true;
							}
						}
					if (!haveStreet)
						continue;
					bool in = false;
					Point inside{};
					bool up = false;
					int tries = 0;
					for (float ring : { 20.0f, 35.0f, 55.0f })
						for (int step = 0; step < 16 && !in; ++step)
						{
							const double angle = step * 0.3926991;
							Point floor;
							if (!walk.nearestPoint({ mat[0] + static_cast<float>(std::cos(angle)) * ring, mat[1],
								mat[2] + static_cast<float>(std::sin(angle)) * ring }, tight, floor) || !indoors(floor))
								continue;
							const knav::PathResult path = walk.findPath(street, floor, kReach, asClass(kResident));
							in = path.complete && crosses(path.doors, key);
							inside = floor;
						}
					if (in)
						ways.push_back({ key, street, inside });
					for (float gx = lo[0]; gx <= hi[0] && !up && tries < 12; gx += 8.0f)
						for (float gz = lo[2]; gz <= hi[2] && !up && tries < 12; gz += 8.0f)
							for (float gy = mat[1] + 25.0f; gy <= hi[1] + 4.0f && !up && tries < 12; gy += 25.0f)
							{
								Point high;
								if (!walk.nearestPoint({ gx, gy, gz }, { 1.0f, 3.0f, 1.0f }, high) || high[1] < mat[1] + 20.0f
									|| !indoors({ high[0], mat[1] + 10.0f, high[2] }))
									continue;
								++tries;
								const knav::PathResult path = walk.findPath(street, high, kReach, asClass(kResident));
								up = path.complete && crosses(path.doors, key);
							}
					entered += in;
					climbed += up;
					std::cout << "  " << building.id << ": door floor y " << mat[1] << ", inside " << in << ", up " << up << "\n";
				}
			}
			std::cout << "house doors walked: " << houseDoors << ", reach an interior floor " << entered
				<< ", reach a higher floor or roof " << climbed << "\n";
			test::require(entered > 0, "no Hub house door leads to an interior floor");
			test::require(ways.size() >= 2, "the door state checks below need two house doors that lead indoors");
		}

		const knav::NavQuery query(mesh);
		const std::vector<dtTileRef> refs = tileRefs(mesh);
		const std::vector<knav::BakedTile> baked = mesh.tiles();

		// Open: a straight way in through the door. The checks below compare costs, so they take the doors whose way in costs about
		// its length: inside a small house the wall band (Area::Edge) can cost more than the door, which costs nothing.
		std::vector<knav::PathResult> open;
		std::vector<Doorway> plain;
		for (const Doorway& way : ways)
		{
			const knav::PathResult path = query.findPath(way.outside, way.inside, kReach, asClass(kResident));
			std::cout << "open: length " << path.length << " cost " << path.cost << " doors " << path.doors.size() << "\n";
			test::require(path.complete, "no path from outside a door to its interior floor");
			test::require(crosses(path.doors, way.key), "the way in does not go through the door's door polygons");
			if (std::fabs(path.cost - path.length) >= path.length * 0.2f)
				continue;
			open.push_back(path);
			plain.push_back(way);
		}
		// A door crossing costs about +5 over the walk (PATHFINDING_STATIC.md 2.3 / 6.3); with the door priced like land it costs nothing.
		for (std::size_t i = 0; i < plain.size(); ++i)
		{
			knav::TravelFilter landPriced = asClass(kResident);
			landPriced.cost[static_cast<std::size_t>(knav::Area::DoorOpen)] = 1.0f;
			landPriced.cost[static_cast<std::size_t>(knav::Area::DoorClosed)] = 1.0f;
			const knav::PathResult flat = query.findPath(plain[i].outside, plain[i].inside, kReach, landPriced);
			const float extra = open[i].cost - flat.cost;
			std::cout << "door cost: +" << extra << " over a land-priced door\n";
			test::require(extra > 2.0f && extra < 8.0f, "crossing a door should cost about +5 in the search");
		}
		ways = plain;
		test::require(ways.size() >= 2, "fewer than two Hub doors have a way in that costs about its length");
		const std::size_t polyCount = mesh.polyCount();

		// The graph is built once, doors open and open to all; every state below is read from the book.
		const knav::TileRect clusters = knav::CoarseGraph::clustersOf(mesh.config().window);
		const knav::CoarseGraph graph = knav::CoarseGraph::build(mesh, clusters, 4);
		test::require(graph == knav::CoarseGraph::build(mesh, clusters, 1), "the door graph depends on the thread count");
		test::require(graph == knav::CoarseGraph::deserialize(graph.serialize()), "door edges do not survive serialisation");
		std::size_t doorEdges = 0;
		for (const auto& [id, cluster] : graph.clusters())
			doorEdges += cluster.doorEdges.size();
		std::cout << "graph: " << graph.stats().clusters << " clusters, " << doorEdges << " portal pairs through doors\n";
		const knav::CoarsePath coarseOpen = graph.findPath(mesh, ways[0].outside, ways[0].inside, kReach, {});
		test::require(coarseOpen.found && crosses(coarseOpen.doors, ways[0].key), "the open coarse route misses the door");

		const Doorway& near = ways[0];
		knav::DoorBook book;

		// Closed, allowed: the same way in at the same cost. Changed 2026-10-31 (DOORS.md section 6): the original's planner adds an open
		// step for an unlocked door, it is not a path cost, so kDoorClosedCost is 1 (it was 11, "dearer by the opening wait").
		book.setState(near.key, DoorState::Closed);
		mesh.applyDoors(book);
		const knav::PathResult closed = query.findPath(near.outside, near.inside, kReach, asClass(kResident));
		std::cout << "closed: length " << closed.length << " cost " << closed.cost << "\n";
		test::require(closed.complete && crosses(closed.doors, near.key), "an allowed traveller should still go through the closed door");
		test::require(std::fabs(closed.cost - open[0].cost) < 1.0f && std::fabs(closed.length - open[0].length) < 1.0f,
			"a closed door should cost what an open one does");
		const knav::CoarsePath coarseClosed = graph.findPath(mesh, near.outside, near.inside, kReach, { &book, kResident });
		std::cout << "coarse open " << coarseOpen.cost << " closed " << coarseClosed.cost << "\n";
		test::require(coarseClosed.found && crosses(coarseClosed.doors, near.key), "the coarse route should go through the closed door too");
		test::require(std::fabs(coarseClosed.cost - coarseOpen.cost) < 1.0f, "the coarse route charges the closed door");

		// Refused: a stranger is kept out of an open door, the resident is not. Same state, two answers.
		book.setState(near.key, DoorState::Open);
		book.setAccess(near.key, kStranger, false);
		mesh.applyDoors(book);
		const knav::PathResult resident = query.findPath(near.outside, near.inside, kReach, asClass(kResident));
		const knav::PathResult stranger = query.findPath(near.outside, near.inside, kReach, asClass(kStranger));
		std::cout << "resident length " << resident.length << ", stranger "
			<< (stranger.complete ? stranger.length : -1.0f) << " (doors " << stranger.doors.size() << ")\n";
		test::require(resident.complete && crosses(resident.doors, near.key) && resident.length < open[0].length + 1.0f,
			"the allowed class should still walk through");
		test::require(!stranger.complete || (!crosses(stranger.doors, near.key) && stranger.length > resident.length * 1.2f),
			"the refused class went through the door");

		const knav::CoarsePath coarseResident = graph.findPath(mesh, near.outside, near.inside, kReach, { &book, kResident });
		const knav::CoarsePath coarseStranger = graph.findPath(mesh, near.outside, near.inside, kReach, { &book, kStranger });
		std::cout << "coarse resident " << coarseResident.length << ", stranger "
			<< (coarseStranger.found ? coarseStranger.length : -1.0f) << "\n";
		test::require(coarseResident.found && crosses(coarseResident.doors, near.key), "the coarse resident route misses the door");
		// complete, not found: a house door has no way round, and a partial path (found) ends outside it (as the locked check below).
		test::require(coarseStranger.found == stranger.complete,
			"coarse and detailed disagree on whether the refused class can get in");
		test::require(!coarseStranger.found || !crosses(coarseStranger.doors, near.key), "the coarse refused route goes through the door");
		if (coarseStranger.found)
			test::require(coarseStranger.length > coarseResident.length * 1.2f, "the coarse refused route did not go round");

		// Locked: nobody. Both classes go round or fail cleanly.
		book.setAccess(near.key, kStranger, true);
		book.setState(near.key, DoorState::Locked);
		mesh.applyDoors(book);
		for (const int doorClass : { kResident, kStranger })
		{
			const knav::PathResult locked = query.findPath(near.outside, near.inside, kReach, asClass(doorClass));
			const knav::CoarsePath coarse = graph.findPath(mesh, near.outside, near.inside, kReach, { &book, doorClass });
			test::require(!locked.complete || !crosses(locked.doors, near.key), "a path goes through a locked door");
			test::require(!coarse.found || !crosses(coarse.doors, near.key), "a coarse route goes through a locked door");
			test::require(locked.complete == coarse.found, "coarse and detailed disagree about the locked door");
			std::cout << "locked, class " << doorClass << ": " << (locked.complete ? locked.length : -1.0f) << " (coarse "
				<< (coarse.found ? coarse.length : -1.0f) << ")\n";
		}
		// The other door is unaffected: from outside it, the way in is still direct.
		const knav::PathResult other = query.findPath(ways[1].outside, ways[1].inside, kReach, asClass(kResident));
		test::require(other.complete && crosses(other.doors, ways[1].key) && other.length < open[1].length + 1.0f,
			"locking one door changed the other");

		// Both doors locked: whatever is left (a wall gap, a ramp) is what the two searches must agree on.
		book.setState(ways[1].key, DoorState::Locked);
		mesh.applyDoors(book);
		const knav::PathResult shut = query.findPath(near.outside, near.inside, kReach, asClass(kResident));
		const knav::CoarsePath coarseShut = graph.findPath(mesh, near.outside, near.inside, kReach, { &book, kResident });
		std::cout << "both locked: detailed " << (shut.complete ? shut.length : -1.0f) << ", coarse "
			<< (coarseShut.found ? coarseShut.length : -1.0f) << "\n";
		test::require(shut.complete == coarseShut.found, "coarse and detailed disagree with both doors locked");
		test::require(!crosses(shut.doors, near.key) && !crosses(shut.doors, ways[1].key), "a path goes through a locked door");
		test::require(!crosses(coarseShut.doors, near.key) && !crosses(coarseShut.doors, ways[1].key),
			"a coarse route goes through a locked door");

		// Through every state above no tile was removed, added or rebuilt.
		test::require(tileRefs(mesh) == refs, "door state changes rebuilt or re-added tiles");
		test::require(mesh.polyCount() == polyCount && mesh.tiles().size() == baked.size(),
			"the mesh changed shape with a door state");

		// A tile that holds door polygons is replaced (as NavWorld::apply does): the state follows it.
		const knav::PolyLoc doorPoly = mesh.doorPolys(near.key).front();
		const knav::BakedTile* source = mesh.tile(doorPoly.tileX, doorPoly.tileZ);
		// The clearance cache is keyed by polygon ref and link index, which a re-link reuses: nothing may survive a replace.
		const std::size_t cachedBefore = query.clearanceEntries();
		mesh.replaceTiles({}, { *source });
		std::cout << "clearance entries before replaceTiles " << cachedBefore << ", after " << query.clearanceEntries() << "\n";
		test::require(query.clearanceEntries() == 0, "a stale clearance entry survived replaceTiles");
		const knav::NavQuery requery(mesh);
		const knav::PathResult afterReplace = requery.findPath(near.outside, near.inside, kReach, asClass(kResident));
		test::require(!afterReplace.complete || !crosses(afterReplace.doors, near.key), "a replaced tile forgot the locked door");

		// Back to open: the original paths return, and through all of it no tile was rebuilt.
		book = {};
		mesh.applyDoors(book);
		const knav::NavQuery fresh(mesh);
		const knav::PathResult reopened = fresh.findPath(near.outside, near.inside, kReach, asClass(kResident));
		test::require(reopened.complete && std::fabs(reopened.length - open[0].length) < 0.01f
			&& std::fabs(reopened.cost - open[0].cost) < 0.01f, "reopening the door did not restore the path");
		for (std::size_t i = 0; i < baked.size(); ++i)
			test::require(mesh.tiles()[i].data == baked[i].data && mesh.tiles()[i].doors == baked[i].doors,
				"the baked bytes of a tile changed with a door state");
	} catch (const std::exception& e) {
		std::cerr << "knav_door: " << e.what() << "\n";
		return 1;
	}
	std::cout << "knav_door ok\n";
	return 0;
}
