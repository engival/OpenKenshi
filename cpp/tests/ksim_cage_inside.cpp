// kenshi-k16.6.370 (playtest cage_prisoner, third report): the caged Prisoner is in its cage. cage_prisoner loaded as scenario
// files load it; since k16.6.731 the sim holds no cage centre / floor height: the prisoner stays in the same cage machine slot (the machine tail the client
// draws from) at its plain stand point at the spawn and every second of 60 game s of the fight. Needs the world navmesh bake
// (argv[1], exit 77 without the Heft region).
#include "ksim/net/simlink.hpp"
#include "ksim/core/simulation.hpp"

#include "scenario_rig.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cmath>
#include <iostream>
#include <string>


using namespace ksim;

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(ksim::dev::srcPath("../scenarios/cage_prisoner.kscen"));
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "ksim_cage_inside: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world, scenario_rig::Window{4, 7, 6, 9, 4});
		Simulation& sim = rig.sim;
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
		const std::uint64_t prisoner = placed.idOf.at(2);
		const state::Fighter* held = sim.combat()->find(prisoner);
		test::require(held != nullptr && held->cageBuilding != 0, "the prisoner is caged");
		test::require(held->machineUse.in() && held->machineUse.building == held->cageBuilding, "the caged prisoner is in the cage machine (k16.6.730)");
		// k16.6.731: the sim holds no cage centre or floor height any more; the machine tail places the drawing. The body stays at its plain
		// stand point and in the same cage machine slot for the whole fight.
		const double x0 = held->x;
		const double z0 = held->z;
		const state::FighterMachineUse use0 = held->machineUse;
		std::size_t bad = 0;
		const auto check = [&](const std::string& when)
		{
			const state::Fighter* f = sim.combat()->find(prisoner);
			if (!f->machineUse.in() || f->machineUse.building != use0.building || f->machineUse.piece != use0.piece || f->machineUse.slot != use0.slot
				|| f->cageBuilding != use0.building || std::hypot(f->x - x0, f->z - z0) > 0.5)
			{
				std::cout << when << ": the prisoner left its cage machine or stand point (" << f->x << ", " << f->y << ", " << f->z << ")\n";
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
		test::require(bad == 0, std::to_string(bad) + " samples with the prisoner out of its cage machine");
		std::cout << "ksim_cage_inside ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
