// kenshi-k16.6.36: a scenario character's `cage=` key puts it in a cage at load through Simulation::imprison (scenarios/cage_prisoner.kscen: a prisoner of
// the player's squad in the Heft slave shop's cage, the Wanderer beside it, a Slaver boss and three crossbow guards 40 u away). Asserted: the prisoner is in
// the cage (cageBuilding, at its slot, locked: CHAR_STATS state 5 with the cage's lock level, the value the wire sends); over 60 game seconds nothing
// shoots, hits, knocks out or targets it, while the slavers do fight the free Wanderer; a `cage=` the world has no free cage for is refused at load.
// Needs the world navmesh bake (argv[1], exit 77 without the Heft region).
#include "ksim/simlink.hpp"
#include "ksim/simulation.hpp"

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_SCENARIO
#error KSIM_SCENARIO must be defined
#endif

using namespace ksim;

namespace {

constexpr int kRunTicks = 1200; // 60 game s
constexpr std::uint64_t kHeftCage = 3893074017808827376ull; // the slave shop's placement key (ksim_arrest's Heft)

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(KSIM_SCENARIO);
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "ksim_cage_scenario: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		// The Hub's window (the default is the 3 x 3 regions around the file's centre).
		scenario_rig::Rig rig(file, world, scenario_rig::Window{4, 7, 6, 9, 4});
		Simulation& sim = rig.sim;

		// A cage the world has none free for is refused at load, never skipped (the Hub has no cage building).
		{
			scenario::File bad = file;
			for (scenario::Char& c : bad.chars)
				c.cage = c.cage.empty() ? c.cage : "1";
			sim.newWorld();
			sim.run(1);
			bool refused = false;
			try
			{
				scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, bad, 1);
			} catch (const std::exception& e) {
				refused = std::string(e.what()).find("cage=1") != std::string::npos;
			}
			test::require(refused, "cage=<a key with no cage> is refused at load");
		}

		for (const std::string& which : {std::string("near"), std::to_string(kHeftCage)})
		{
			scenario::File variant = file;
			for (scenario::Char& c : variant.chars)
				c.cage = c.cage.empty() ? c.cage : which;
			sim.newWorld();
			sim.run(1);
			const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, variant, 3);
			const std::uint64_t wanderer = placed.idOf.at(1);
			const std::uint64_t prisoner = placed.idOf.at(2);
			const state::Fighter* held = sim.combat()->find(prisoner);
			test::require(held != nullptr && held->cageBuilding == kHeftCage, "cage=" + which + ": the prisoner is in the Heft slave shop's cage");
			test::require(sim.combat()->find(wanderer)->cageBuilding == 0, "the Wanderer is free");
			const double cageX = held->x;
			const double cageZ = held->z;
			const Simulation::CharStats stats = sim.charStats(prisoner);
			test::require(stats.state == 5, "CHAR_STATS state 5 (a locked cage): " + std::to_string(stats.state));
			std::cout << "cage=" << which << ": prisoner at (" << cageX << ", " << cageZ << "), state " << static_cast<int>(stats.state) << ", lock level "
				<< static_cast<int>(stats.lockLevel) << "\n";

			std::vector<std::string> hits;
			std::size_t pickedWanderer = 0;
			std::size_t pickedPrisoner = 0;
			std::size_t shotsAtWanderer = 0;
			const_cast<CombatArena*>(sim.combat())->setObserver([&](const CombatEvent& e)
			{
				if (e.who == prisoner && (e.kind == CombatEvent::Kind::Hit || e.kind == CombatEvent::Kind::Shot || e.kind == CombatEvent::Kind::Knockout
					|| e.kind == CombatEvent::Kind::Death || e.kind == CombatEvent::Kind::StealthKo))
					hits.push_back("event kind " + std::to_string(static_cast<int>(e.kind)) + " by " + std::to_string(e.other));
				if (e.who == wanderer && e.kind == CombatEvent::Kind::Shot)
					++shotsAtWanderer;
			});
			// The player attacks the Slaver boss (char 3), as a player would (the scenario's own look_for): the slavers answer and the fight is on
			// while the prisoner sits in its cage.
			sim.enqueue(Command::fighterAttack(state::Id{wanderer}, state::Id{placed.idOf.at(3)}));
			for (int tick = 0; tick < kRunTicks; ++tick)
			{
				sim.tick();
				for (const CombatArena::Member& m : sim.combat()->members())
				{
					if (m.id == wanderer || m.id == prisoner)
						continue;
					pickedWanderer += m.fighter.target == wanderer ? 1 : 0;
					pickedPrisoner += m.fighter.target == prisoner || m.fighter.orderTarget == prisoner ? 1 : 0;
				}
			}
			const state::Fighter* after = sim.combat()->find(prisoner);
			test::require(after != nullptr && !after->dead && !after->unconscious && !after->down, "the prisoner is alive, up and conscious after 60 s");
			test::require(after->cageBuilding == kHeftCage && std::hypot(after->x - cageX, after->z - cageZ) < 1.0, "the prisoner never left its cage");
			test::require(pickedPrisoner == 0, "no slaver picked the prisoner as a target (" + std::to_string(pickedPrisoner) + " ticks)");
			test::require(hits.empty(), "no hit, shot, KO or technique reached the prisoner: " + (hits.empty() ? std::string() : hits.front()));
			test::require(pickedWanderer > 0, "the slavers fight the free Wanderer (target picks: " + std::to_string(pickedWanderer) + ", shots " + std::to_string(shotsAtWanderer) + ")");
			std::cout << "cage=" << which << ": " << pickedWanderer << " target-ticks on the Wanderer, " << shotsAtWanderer << " shots at it, none on the prisoner\n";
		}
		std::cout << "ksim_cage_scenario ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
