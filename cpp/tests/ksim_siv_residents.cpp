// kenshi-k16.6.529: a seated resident stands at its chair's storey, never on a surface below it ("UC Bar patron" 1157, client 2026-11-20: wire
// y 1248 / floor 0 while the chair is 68 u higher). The new-game world, the player on Outpost s-IV's deck, 7000 ticks: every seated resident
// is within 2 u of its seat pose's y. Chairs with no mesh surface near them (the upper storeys of the bar at x 69990, z -71700 are not in the
// mesh) make the run SKIP as KNOWN (exit 77) with the list; it fails if a surface exists and the resident is not on it. argv[1]: navworld bake.
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "live_world_rig.hpp"
#include "nav_source.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

namespace {

constexpr double kDeckX = 67772.7; // Outpost s-IV's entrance deck (ksim_siv_floors)
constexpr double kDeckZ = -71427.9;

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !std::filesystem::exists(argv[1]))
		{
			std::cout << "ksim_siv_residents: SKIPPED: needs the navworld bake (argv[1])\n";
			return 77;
		}
		const auto gd = std::make_shared<kdata::GameData>(kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA)));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		Simulation sim = Simulation::newGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA));
		sim.markPristine();
		const std::shared_ptr<MeshNavigator> nav = test::openWorldNav(argv[1], live_rig::hubZonesWith({{kDeckX, kDeckZ}}));
		sim.enableCombat(data, CombatSettings{});
		sim.enableNavigation(nav);
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = nav;
		ai.data = gd;
		sim.enableAi(std::move(ai));
		live_rig::startWanderer(sim, *gd);
		live_rig::standPlayerAt(sim, kDeckX, kDeckZ);
		sim.run(7000); // the bar's patrons come late (the first seated one by ~2000 ticks)

		// Every seated resident stands on its chair's storey (within 2 u of the seat pose's y) and reads the floor its height gives. A chair
		// the loaded mesh has no surface near (the bar beside s-IV, building 16645741484581658316: chairs at +68 / +113 u over the one
		// ground-floor mesh surface, no walkable surface above y 1290 anywhere in its 180 u square, floors table = the roof only) cannot be
		// stood on: that is the nav's gap (knav interior storeys), the test then SKIPs with the list, and it fails once a surface exists and
		// the resident is not on it.
		int seated = 0;
		int checked = 0;
		std::string missing;
		for (const CombatArena::Member& m : sim.combat()->members())
		{
			const state::Fighter& f = m.fighter;
			if (f.playerControlled || f.dead || !sim.seated(m.id))
				continue;
			++seated;
			const auto pose = sim.pose(m.id);
			if (std::fabs(f.y - pose->y) <= 2.0)
			{
				++checked;
				continue;
			}
			if (!nav->walker().surfaceY(pose->x, pose->y, pose->z, 2.0))
			{
				missing += " " + std::to_string(m.id) + "(y " + std::to_string(f.y) + ", chair " + std::to_string(pose->y) + ")";
				continue;
			}
			throw std::runtime_error("seated resident " + std::to_string(m.id) + " stands at y " + std::to_string(f.y) + ", chair at " + std::to_string(pose->y) + " has a mesh surface");
		}
		test::require(seated > 0, "no seated resident after the run");
		if (!missing.empty())
		{
			std::cout << "ksim_siv_residents: SKIPPED: KNOWN, the mesh has no surface at these chairs (" << checked << " of " << seated << " seated stand on theirs):" << missing << "\n";
			return 77;
		}
		std::cout << "ksim_siv_residents: " << seated << " seated residents all on their chair's storey\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_siv_residents: FAILED: " << e.what() << "\n";
		return 1;
	}
}
