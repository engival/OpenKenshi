// kenshi-k16.6.468: a scenario's player squad keeps its full-detail area (and the nav tiles under it) wherever it walks. gate_heft puts the
// Wanderer at Heft's gate with no start-off faction, so stepPlayerAreas used to leave the area at the file's start: 6k u away the squad dropped
// to strategic detail, undrawn, with no mesh under it. The Wanderer is moved in 1k u hops (as a walk would) to 10k u south of the start; at
// every hop, after the sim has run a second, an area covers its region and the navigator covers its spot. Needs the world bake (exit 77 without).
#include "ksim/core/simulation.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/world/region.hpp"

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <iostream>
#include <string>

using namespace ksim;

namespace {

bool areaCovers(const Simulation& sim, double x, double z)
{
	const std::int32_t region = sim.regions().regionOf(x, z);
	for (const Area& area : sim.regions().areas())
	{
		if (std::binary_search(area.regions.begin(), area.regions.end(), region))
			return true;
	}
	return false;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(ksim::dev::srcPath("../scenarios/gate_heft.kscen"));
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "ksim_player_detail_follow: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world);
		Simulation& sim = rig.sim;
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 1);
		test::require(!placed.players.empty(), "gate_heft places no player");
		const std::uint64_t id = placed.players.front();
		sim.run(40);
		test::require(areaCovers(sim, fighterOf(sim, id)->x, fighterOf(sim, id)->z), "no area over the Wanderer at the start");
		for (int hop = 1; hop <= 10; ++hop)
		{
			state::Fighter* fighter = fighterOf(sim, id);
			fighter->z += 1000.0;
			sim.run(80); // past the residents' one-second interval and the tile load
			fighter = fighterOf(sim, id);
			const std::string where = " at hop " + std::to_string(hop) + " (" + std::to_string(fighter->x) + ", " + std::to_string(fighter->z) + ")";
			test::require(areaCovers(sim, fighter->x, fighter->z), "no full-detail area over the player squad" + where);
			test::require(sim.navigation()->covers(fighter->x, fighter->z), "no nav tiles under the player squad" + where);
		}
		std::cout << "ksim_player_detail_follow: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_player_detail_follow: " << e.what() << '\n';
		return 1;
	}
}
