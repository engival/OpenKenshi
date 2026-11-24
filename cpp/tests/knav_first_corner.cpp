// PATH_WALK.md 3b step 1 (kenshi-e9s9.5): knav::firstCorner on hand-made portals (x east, z north, walking +z: left = -x, right = +x).
#include "knav/walk.hpp"

#include "test_util.hpp"

#include <iostream>
#include <vector>

namespace {

struct P
{
	double lx, lz, rx, rz;
};

} // namespace

int main()
{
	try
	{
		// A straight corridor 20 wide: the goal is visible from the start.
		const std::vector<P> straight = {{-10, 10, 10, 10}, {-10, 20, 10, 20}, {-10, 30, 10, 30}};
		const knav::FunnelCorner a = knav::firstCorner(straight, 0, 0.0, 0.0, 0.0, 40.0);
		test::require(a.goal, "straight: goal visible");

		// A right-hand turn round a wall end at (10, 20): the passage runs +z, then east. Portals: across the first leg at z 20 (x -10..10), then the
		// opening east at x 30 (z 20..40, left end = the far (north) one). The goal (60, 30) is east.
		const std::vector<P> turn = {{-10, 20, 10, 20}, {30, 40, 30, 20}};
		const knav::FunnelCorner b = knav::firstCorner(turn, 0, 0.0, 0.0, 60.0, 30.0);
		test::require(!b.goal && !b.left, "turn: the wall end (right) is the corner");
		test::require(b.x == 10.0 && b.z == 20.0 && b.index == 0, "turn: the corner is the wall end");
		// Past the wall end the goal is visible.
		const knav::FunnelCorner c = knav::firstCorner(turn, 1, 20.0, 25.0, 60.0, 30.0);
		test::require(c.goal, "turn: goal visible past the corner");

		// Point portals (a route corner the ray could not cross): the corner is the point.
		const std::vector<P> points = {{0, 20, 0, 20}, {20, 20, 20, 20}};
		const knav::FunnelCorner d = knav::firstCorner(points, 0, 0.0, 0.0, 20.0, 40.0);
		test::require(!d.goal && d.x == 0.0 && d.z == 20.0, "points: the first point");
		const knav::FunnelCorner e = knav::firstCorner(points, 1, 0.0, 20.0, 20.0, 40.0);
		test::require(e.x == 20.0 && e.z == 20.0, "points: the second point");
		std::cout << "knav_first_corner: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "knav_first_corner: FAIL: " << e.what() << "\n";
		return 1;
	}
}
