// The pieces of a live-world test that opens the whole new-game world on the navworld bake (ksim_raid_goals, ksim_raid_occupy,
// ksim_raid_victory_stay, ksim_raid_phases): the fixture towns, the zone window, the Wanderer start and the player as the detail anchor.
// Each of those tests carries its own Game fixture on these; a new one starts here. Needs KENSHI_DATA.
#pragma once

#include "ksim/core/clock.hpp"
#include "ksim/core/command.hpp"
#include "ksim/save/newgame.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/world/towncircle.hpp"

#include "kworld/coords.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"

#include <cmath>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace live_rig {

// The town circles (world_tasks.cpp: every TownState, towncircle::of, in ecs order) - the selector's own myTown geometry.
inline std::vector<ksim::towncircle::Circle> townCircles(ksim::Simulation& sim, const kdata::GameData& gd)
{
	std::vector<ksim::towncircle::Circle> circles;
	ecsOf(sim).each([&](flecs::entity, const ksim::state::TownState& town) { circles.push_back(ksim::towncircle::of(gd, town)); });
	return circles;
}

// Whether a point is a fixture town's: inside a town circle and away from the Hub (the new-game start town).
inline bool fixtureTownAt(const std::vector<ksim::towncircle::Circle>& circles, double x, double z)
{
	return ksim::towncircle::at(circles, x, z) >= 0 && std::hypot(x - kHubX, z - kHubZ) > 6000.0;
}

// The Hub's 3 x 3 zones (where the start point is picked) plus the zones of the given points.
inline std::vector<kworld::ZoneCell> hubZonesWith(const std::vector<std::pair<double, double>>& points)
{
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			zones.push_back({ksim::simlink::kHubZoneX + dx, ksim::simlink::kHubZoneZ + dz});
	for (const auto& [x, z] : points)
		zones.push_back(kworld::zoneCellOf(x, z));
	return zones;
}

// The new-game start as the first Wanderer: the player character, the detail anchor and the "a player exists" test the promotions need
// (never ordered). Runs the New Game command, then noon: every goal's window is open. The sim is markPristine()d first.
inline void startWanderer(ksim::Simulation& sim, const kdata::GameData& gd)
{
	const ksim::StartCatalog catalog = ksim::StartCatalog::build(gd, KENSHI_DATA);
	std::string start;
	for (const ksim::StartOff& off : catalog.all())
		if (off.name.find("Wanderer") != std::string::npos)
		{
			start = off.sid;
			break;
		}
	test::require(!start.empty(), "no Wanderer start");
	std::string outcome;
	sim.addNewGameObserver(&outcome, [&](const ksim::NewGameOutcome& result) { outcome = result.reason; });
	sim.enqueue(ksim::Command::newGame(start, "Tester", 1));
	sim.run(20);
	sim.removeNewGameObserver(&outcome);
	sim.setClockTick(static_cast<std::uint64_t>(ksim::kTicksPerGameDay * 0.5));
	test::require(playerOf(sim) != nullptr, "the new game has no player character (" + outcome + ")");
}

// The player character stands at (x, z) (the area it carries is the input to the resident pass; it is never ordered).
inline void standPlayerAt(ksim::Simulation& sim, double x, double z)
{
	if (ksim::state::Fighter* player = const_cast<ksim::state::Fighter*>(playerOf(sim)))
	{
		player->x = x;
		player->z = z;
		player->y = sim.groundHeight(x, z).value_or(player->y);
		player->clearRoute();
		player->building = 0;
	}
}

} // namespace live_rig
