// kenshi-k16.6.442 (playtest cage_prisoner, 3rd regression): the Heft slave shop's own watchers (goal 183, Slaver Boss / Guards) stand at DISTINCT
// spots near the cage once they hold the goal, not stacked on one point (the prisoner's old spawn spot beside the cage). cage_prisoner loaded as
// scenario files load it, 90 game s; every member of the world on goal 183 within 80 u of the prisoner is sampled each second. Needs the world
// navmesh bake (argv[1], exit 77 without the Heft region).
#include "ksim/net/simlink.hpp"
#include "ksim/core/simulation.hpp"

#include "scenario_rig.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace ksim;

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(ksim::dev::srcPath("../scenarios/cage_prisoner.kscen"));
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "ksim_cage_watchers: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world, scenario_rig::Window{4, 7, 6, 9, 4});
		Simulation& sim = rig.sim;
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
		const state::Fighter* prisoner = sim.combat()->find(placed.idOf.at(2));
		test::require(prisoner != nullptr && prisoner->cageBuilding != 0, "the prisoner is caged");
		const double px = prisoner->x;
		const double pz = prisoner->z;
		const double spawnX = file.chars.at(1).x;
		const double spawnZ = file.chars.at(1).z;
		struct Watcher { std::uint64_t id; double x; double z; };
		std::vector<Watcher> watchers;
		for (int tick = 1; tick <= 90 * 20; ++tick)
		{
			sim.tick();
			if (tick % 20 != 0)
				continue;
			watchers.clear();
			const_cast<flecs::world&>(sim.world().ecs()).each([&](flecs::entity e, const state::Fighter& f)
			{
				const state::Routine* r = e.get<state::Routine>();
				if (r != nullptr && r->goal == routines::task::SlaveWatching && std::hypot(f.x - px, f.z - pz) < 80.0)
					watchers.push_back({e.id(), f.x, f.z});
			});
		}
		for (const Watcher& w : watchers)
			std::cout << "watcher " << w.id << " at (" << w.x << ", " << w.z << ") " << std::hypot(w.x - px, w.z - pz) << " u from the cage\n";
		test::require(watchers.size() >= 2, "at least two watchers hold goal 183 near the cage (" + std::to_string(watchers.size()) + ")");
		std::size_t bad = 0;
		for (std::size_t i = 0; i < watchers.size(); ++i)
		{
			if (std::hypot(watchers[i].x - spawnX, watchers[i].z - spawnZ) < 2.0 || std::hypot(watchers[i].x - px, watchers[i].z - pz) > 30.0)
				++bad;
			for (std::size_t j = i + 1; j < watchers.size(); ++j)
			{
				if (std::hypot(watchers[i].x - watchers[j].x, watchers[i].z - watchers[j].z) < 6.0)
					++bad;
			}
		}
		test::require(bad == 0, std::to_string(bad) + " watcher pairs closer than a body width (6 u), far from the cage (30 u), or on the prisoner's spawn spot");
		std::cout << "ksim_cage_watchers ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
