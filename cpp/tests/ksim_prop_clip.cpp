// kenshi-k16.6.253 (playtest cage_prisoner, Heft Longhouse): a character never stands inside a solid prop. cage_prisoner loaded as scenario files load
// it, then run 60 game s of the fight; at the spawn and every second no character (the caged prisoner excepted: its slot is inside its cage by design)
// is inside the collision of a placed Obstacle / Projected solid (prop_clip.hpp). Needs the world navmesh bake (argv[1], exit 77 without the Heft
// region); KSIM_NATIVE_NAV=1 runs it on the runtime mesh (the shipped tiles), else on the bake.
#include "ksim/simlink.hpp"
#include "ksim/simulation.hpp"

#include "prop_clip.hpp"
#include "scenario_rig.hpp"
#include "test_util.hpp"

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
			std::cout << "ksim_prop_clip: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world, scenario_rig::Window{4, 7, 6, 9, 4});
		Simulation& sim = rig.sim;
		prop_clip::PropClip clip(*rig.gameData);
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
		const std::uint64_t prisoner = placed.idOf.at(2);
		std::size_t bad = 0;
		const auto check = [&](const std::string& when)
		{
			for (const CombatArena::Member& m : sim.combat()->members())
			{
				if (m.id == prisoner || m.fighter.dead)
					continue;
				const std::string in = clip.inside(m.fighter.x, m.fighter.y, m.fighter.z);
				if (!in.empty())
				{
					std::cout << when << ": char " << m.id << " at (" << m.fighter.x << ", " << m.fighter.y << ", " << m.fighter.z << ") is inside " << in << "\n";
					++bad;
				}
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
		test::require(bad == 0, std::to_string(bad) + " (character, time) samples stood inside a solid prop");
		std::cout << "ksim_prop_clip ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
