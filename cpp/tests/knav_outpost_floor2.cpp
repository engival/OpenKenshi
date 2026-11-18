// Outpost s-IV, floor 0 -> floor 2 (kenshi-k16.6.260; user play 2026-11-15: three clicks on the floor-2 bunk room were `no_route`).
// The way up is the interior ramp, BUILDING part 42261 (Newwworld.mod `bravo-Interior_Ramp_LEVEL00`, `is stairs`, an `interior`
// entry whose chance is 0). The part roll took 0 as "never", so the ramp was not in the collision, the floor-0 mesh had no polygon
// on it and floor 2 (the slab at y 1246) was an island (selectParts, kworld/collision.cpp). The shipped navtile has the ramp.
// Runs on the runtime mesh (MeshNavigator::openNative: s-IV's zone is stale and is rebaked from our collision); exit 77 without
// the game's navtiles. Also checks that a refused route names the check that refused it (Route::check, the server log's `check=`).

#include "ksim/navigator.hpp"

#include "knav/native.hpp"
#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <filesystem>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "knav_outpost_floor2: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		const kworld::ZoneCell zone = kworld::zoneCellOf(67772.7, -71427.9);
		const auto nav = ksim::MeshNavigator::openNative(KENSHI_DATA, { zone });
		test::require(nav->native(), "not the runtime mesh");
		constexpr double kBody = 4.0; // the Greenlander's pathfind footprint radius

		// The playtest's case: the Wanderer inside s-IV at floor 0, the goal (67724, 1246.29, -71480) in the floor-2 bunk room.
		const ksim::Route up = nav->routeFrom({ 67772.7, 1203.4, -71427.9 }, 67724.0, -71480.0, 1246.29, false, kBody, true, false);
		test::require(up.found, std::string("floor 0 -> floor 2 refused: ") + std::string(up.check));
		test::require(!up.approach, "the route ends short of the bunk room");
		test::require(up.heights.back() > 1240.0 && up.heights.back() < 1252.0, "the route does not end on floor 2");
		test::require(std::hypot(up.points.back().x - 67724.0, up.points.back().z + 71480.0) < 6.0, "the route ends away from the click");
		double top = 0.0;
		for (const double h : up.heights)
			top = std::max(top, h);
		test::require(top < 1262.0, "the route climbs past floor 2 (onto the roof slab)");
		// ... and back down.
		const ksim::Route down = nav->routeFrom({ 67724.0, 1246.3, -71480.0 }, 67772.7, -71427.9, 1203.4, false, kBody, true, false);
		test::require(down.found && down.heights.back() < 1210.0, "floor 2 -> floor 0 has no route");

		// A refused route says which check refused it. The goal is high over every layer: a goal at y 1500 was refused only
		// because an unpruned pocket sat there; the pruned runtime mesh (NAVMESH_CACHE.md "Native source") has none, and the
		// click snaps to the roof slab instead.
		const ksim::Route none = nav->routeFrom({ 67772.7, 1203.4, -71427.9 }, 67724.0, -71480.0, 5000.0, false, kBody, true, false);
		test::require(!none.found && !none.check.empty(), "a refused route names no check");
		std::cout << "knav_outpost_floor2: up " << up.points.size() << " points, top y " << top << "; refused goal: " << none.check << "\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "knav_outpost_floor2: FAILED: " << e.what() << "\n";
		return 1;
	}
}
