// kenshi-k16.6.370 (playtest cage_prisoner, third report): the caged Prisoner stands INSIDE its cage, not beside it. cage_prisoner loaded as scenario
// files load it; the prisoner's position, read from the sim (the Fighter the CHAR_DYNAMIC snapshot is written from), is inside a solid prop's collision
// (the cage's closed box, prop_clip.hpp) at the spawn and every second of 60 game s of the fight, and never moves. Needs the world navmesh bake
// (argv[1], exit 77 without the Heft region).
#include "ksim/simlink.hpp"
#include "ksim/simulation.hpp"

#include "prop_clip.hpp"
#include "scenario_rig.hpp"
#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <string>

#ifndef KSIM_SCENARIO
#error KSIM_SCENARIO must be defined
#endif

using namespace ksim;

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(KSIM_SCENARIO);
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "ksim_cage_inside: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world, scenario_rig::Window{4, 7, 6, 9, 4});
		Simulation& sim = rig.sim;
		prop_clip::PropClip clip(*rig.gameData);
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
		const std::uint64_t prisoner = placed.idOf.at(2);
		const state::Fighter* held = sim.combat()->find(prisoner);
		test::require(held != nullptr && held->cageBuilding != 0, "the prisoner is caged");
		const double x0 = held->x;
		const double z0 = held->z;
		std::cout << "prisoner at (" << x0 << ", " << held->y << ", " << z0 << ")" << clip.near(x0, z0, 12.0) << "\n";
		std::size_t bad = 0;
		const auto check = [&](const std::string& when)
		{
			const state::Fighter* f = sim.combat()->find(prisoner);
			const std::string in = clip.inside(f->x, f->y + 1.0, f->z);
			if (in.empty() || std::hypot(f->x - x0, f->z - z0) > 0.5)
			{
				std::cout << when << ": the prisoner at (" << f->x << ", " << f->y << ", " << f->z << ") is in no cage solid" << clip.near(f->x, f->z, 12.0) << "\n";
				++bad;
			}
		};
		check("spawn");
		sim.enqueue(Command::fighterAttack(state::Id{placed.idOf.at(1)}, state::Id{placed.idOf.at(3)}));
		for (int tick = 1; tick <= 1200; ++tick)
		{
			sim.tick();
			if (tick % 20 == 0)
				check("t=" + std::to_string(tick / 20) + "s");
		}
		test::require(bad == 0, std::to_string(bad) + " samples with the prisoner outside its cage's collision");
		std::cout << "ksim_cage_inside ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
