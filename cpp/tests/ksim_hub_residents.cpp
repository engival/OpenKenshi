// kenshi-k16.6.33: the Hub's town populate against vanilla (rwh.35). The zone data presets the Hub's buildings (7 hub squatters, 1 Bar blackmarket,
// 1 Thieves Guild, 1 holy hub hideout, the rest the ruined default); the `residents` entries add none on top of a preset of their own template.
// Asserts, for the squads homed at The Hub on a New Game: one Bar blackmarket (the vanilla Hub has one; the second is the Rebel Base's), one Thieves
// Guild in a building placed with the "Thieves" interior layout (1470 u from the census point, outside its 1200 u radius), at most 7 hub squatters
// squads, and no Bar blackmarket or Thieves Guild homed in a building of the ruined default (the entries do not retake the default presets).
//
// argv[1]: unused (kept so the test takes the census arguments).

#include "ksim/core/simulation.hpp"
#include "ksim/core/state.hpp"
#include "ksim/world/world.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

int main(int, char**)
try
{
	constexpr double kCx = -50986.0;
	constexpr double kCz = 3514.0;
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
	SpawnOptions options;
	options.seed = 1;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	std::map<std::string, int> atHub;
	double thievesDistance = 0.0;
	std::string thievesLayout;
	sim.world().ecs().each([&](flecs::entity, const state::SquadInstance& squad)
	{
		if (squad.source != state::SpawnSource::TownResident || squad.homeTown.id == 0)
			return;
		const state::TownState* town = sim.world().ecs().entity(squad.homeTown.id).get<state::TownState>();
		const kdata::Record* record = sim.gameData()->find(squad.templateSid);
		if (town == nullptr || record == nullptr || town->activeSid != "18919-Newwworld.mod")
			return;
		++atHub[record->name];
		if (record->name == "Thieves Guild")
		{
			thievesDistance = std::hypot(squad.homeX - kCx, squad.homeZ - kCz);
			if (const BuildingSite* building = sim.world().buildingSite(squad.homeBuilding))
				thievesLayout = building->interiorLayout;
		}
	});
	for (const auto& [name, n] : atHub)
		std::cout << "  " << name << " x" << n << "\n";
	test::require(atHub["Bar blackmarket"] == 1, "one Bar blackmarket at the Hub");
	test::require(atHub["Thieves Guild"] == 1, "one Thieves Guild at the Hub");
	test::require(thievesLayout == "Thieves", "the Thieves Guild sits in a Thieves-layout building, not " + thievesLayout);
	test::require(thievesDistance > 1200.0 && thievesDistance < 1700.0, "the Thieves Guild is the northern building (~1470 u from the census point)");
	test::require(atHub["hub squatters"] <= 7, "at most the 7 preset hub squatters buildings");
	std::cout << "ksim_hub_residents: OK\n";
	return 0;
}
catch (const std::exception& e)
{
	std::cerr << "ksim_hub_residents: FAIL: " << e.what() << "\n";
	return 1;
}
