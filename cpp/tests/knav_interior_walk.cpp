// The sim's y on a sunk Longhouse's floor (kenshi-k16.7.31, user playtest 2026-11-14 cage_prisoner: "the character is floating up in the air"). The Longhouse
// (4028-TwoStorey.mod, Heft, floor 1172.9-1175) lies under a dune that rises to 1195 at its north end. The sim's y is right: its walkers take the navmesh floor's
// height; the 1194.9 the client drew is the client's own ground snap (simview.cpp: the terrain unless the sim's y is over 2 u above it). This pins the sim's y:
// the Wanderer of scenarios/cage_prisoner.kscen walks player move_to routes (the wire's: routeFrom, clickGoal) to the capture point (68349.1, -71139.1: the
// sim's y 1175 where the dune is 1194.9), a floor point where the dune crosses 1171, the middle, out of the east door and back in. Asserted, every tick a
// character is inside the building: y within 3 u of the floor (1174.75), no tick moves it by more than a stair step; the door's ramp (outside the interior
// mask) is only bounded. It passes on the base too: it guards the sim's y, it does not discriminate.
// Needs the world navmesh bake (argv[1], exit 77 without the Heft region).
#include "ksim/net/simlink.hpp"
#include "ksim/core/simulation.hpp"

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

namespace {

constexpr double kFloor = 1174.75; // the floor polygons lie at 1172.9-1175
constexpr double kBand = 3.0;
constexpr double kStair = 1.5; // u per tick inside; the floor's own slope is 0.3
constexpr double kRamp = 7.0; // the door's ramp outside the mask runs 1:1 down the dune's face: 3.4 u per tick, 5.3-6.5 where a tick crosses its foot (terrain, as the original)

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(ksim::dev::srcPath("../scenarios/cage_prisoner.kscen"));
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "knav_interior_walk: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world, scenario_rig::Window{4, 7, 6, 9, 4});
		Simulation& sim = rig.sim;
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
		const std::uint64_t walker = placed.idOf.at(1);
		const state::Fighter* f = sim.combat()->find(walker);
		test::require(f != nullptr && f->building != 0, "the Wanderer starts inside the Longhouse");

		// {x, z, inside}: the capture point, a floor point where the dune crosses 1171 (the base dipped there), the middle, out of the east door, back in
		const double legs[][3] = { { 68349.1, -71139.1, 1 }, { 68359.8, -71198.5, 1 }, { 68390.0, -71275.0, 1 }, { 68450.0, -71260.0, 0 }, { 68359.8, -71198.5, 1 } };
		int leg = 0;
		for (const auto& goal : legs)
		{
			const Route route = sim.navigation()->routeFrom({f->x, f->y, f->z}, goal[0], goal[1], std::nullopt, false, 4.0, true, true);
			test::require(route.found, "a route to leg " + std::to_string(leg));
			sim.enqueue(Command::fighterMoveTo(state::Id{walker}, route.points, route.heights, 1 + leg, false, route.approach));
			double lowest = 1.0e9;
			double highest = -1.0e9;
			double stair = 0.0;
			double ramp = 0.0;
			bool wasIn = f->building != 0;
			for (int tick = 0; tick < 500; ++tick)
			{
				const double before = f->y;
				sim.tick();
				f = sim.combat()->find(walker);
				const bool in = f->building != 0;
				const double step = std::fabs(f->y - before);
				if (in && wasIn)
				{
					lowest = std::min(lowest, f->y);
					highest = std::max(highest, f->y);
					stair = std::max(stair, step);
				} else {
					ramp = std::max(ramp, step);
				}
				wasIn = in;
			}
			std::cout << "leg " << leg << " to (" << goal[0] << ", " << goal[1] << "): ended at (" << f->x << ", " << f->y << ", " << f->z << "), inside y " << lowest
				<< " .. " << highest << ", max step inside " << stair << ", on the ramp " << ramp << "\n";
			test::require(std::hypot(f->x - goal[0], f->z - goal[1]) < 10.0, "leg " + std::to_string(leg) + " arrives");
			if (goal[2] != 0.0)
				test::require(f->y > kFloor - kBand && f->y < kFloor + kBand, "leg " + std::to_string(leg) + " ends on the floor, y " + std::to_string(f->y));
			if (lowest < 1.0e8)
			{
				test::require(lowest > kFloor - kBand && highest < kFloor + kBand,
					"leg " + std::to_string(leg) + ": inside the Longhouse y stays within 3 u of the floor (" + std::to_string(lowest) + " .. " + std::to_string(highest) + ")");
				test::require(stair <= kStair, "leg " + std::to_string(leg) + ": no tick inside moves y by more than a stair step (" + std::to_string(stair) + ")");
			}
			test::require(ramp <= kRamp, "leg " + std::to_string(leg) + ": the door ramp has no pop (" + std::to_string(ramp) + ")");
			++leg;
		}
		std::cout << "knav_interior_walk ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
