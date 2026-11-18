// The live world a scenario-file test runs in: a new game on the navworld bake with combat, navigation and the squad AI on (the window of
// regions around the file's centre), ready for newWorld() / scenario::apply(). ksim_cage_scenario, ksim_food_raid_scenario and
// ksim_raid_takeover_scenario each carried a copy; a new scenario test starts here. Needs KENSHI_DATA and KSIM_AI_TABLE.
#pragma once

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"
#include "nav_source.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif

namespace scenario_rig {

// The squad AI's navigator window in regions (rx0 rz0 rx1 rz1) and its last open() argument.
struct Window
{
	int rx0, rz0, rx1, rz1;
	int last = 8;
};

// Whether the world navigation (the runtime mesh, or the bake in `world` with KSIM_NAVWORLD_BAKE) has the region the file's centre is in.
inline bool hasRegion(const ksim::scenario::File& file, const std::string& world)
{
	const kworld::ZoneCell zone = kworld::zoneCellOf(file.centreX, file.centreZ);
	return test::worldRegionReady(world, zone.x, zone.z);
}

struct Rig
{
	ksim::Simulation sim;
	std::shared_ptr<const kdata::GameData> gameData;
	std::shared_ptr<const ksim::CombatData> combatData;
	ksim::simlink::ServerContext context;

	// `window` empty: the 3 x 3 regions around the file's centre (the default 4 7 6 9 window is the Hub's).
	Rig(const ksim::scenario::File& file, const std::string& world, std::optional<Window> window = std::nullopt)
		: sim(ksim::Simulation::newGame(KENSHI_DATA, kworld::resolveLoadOrder(KENSHI_DATA, ""), spawnOptions()))
	{
		const kworld::ZoneCell zone = kworld::zoneCellOf(file.centreX, file.centreZ);
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		}
		sim.markPristine();
		gameData = sim.gameData();
		combatData = ksim::CombatData::build(*gameData, KENSHI_DATA);
		ksim::CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combatData, settings);
		sim.enableNavigation(test::openWorldNav(world, zones));
		ksim::AiSystem::Config ai;
		ai.registry = std::make_shared<ksim::BehaviourRegistry>(sim.world(), ksim::BehaviourRegistry::readRules(KSIM_AI_TABLE));
		const int rx = zone.x / knav::kRegionZones;
		const int rz = zone.z / knav::kRegionZones;
		const Window at = window.value_or(Window{std::max(0, rx - 1), std::max(0, rz - 1), rx + 1, rz + 1});
		ai.navigator = ksim::KnavNavigator::open(KENSHI_DATA, world, at.rx0, at.rz0, at.rx1, at.rz1, at.last);
		ai.factions = ksim::FactionBook::build(sim.gameData());
		sim.enableAi(std::move(ai));
		context.gameData = gameData;
		context.squadLimit = 50;
	}

private:
	static ksim::SpawnOptions spawnOptions()
	{
		ksim::SpawnOptions spawn;
		spawn.seed = 7;
		return spawn;
	}
};

// The scenario moved by (dx, dz) with its characters raised by dy: the file's centre, characters and containers, its towns and its raids' towns
// renamed `town`. A scenario written for one town replayed in another (Old Village -> Heft, the walled one).
inline ksim::scenario::File moveScenario(ksim::scenario::File file, double dx, double dz, double dy, const std::string& town)
{
	file.centreX += dx;
	file.centreZ += dz;
	for (ksim::scenario::Char& c : file.chars)
	{
		c.x += dx;
		c.z += dz;
		c.y += dy;
	}
	for (ksim::scenario::Container& c : file.containers)
	{
		c.x += dx;
		c.z += dz;
	}
	for (ksim::scenario::Town& t : file.towns)
		t.name = town;
	for (ksim::scenario::Raid& r : file.raids)
		r.town = town;
	return file;
}

// A scenario written for Old Village, in Heft: the file's offset between the two towns (Heft's ground is ~1170 u high, Old Village's ~1000).
inline ksim::scenario::File inHeft(const ksim::scenario::File& file)
{
	return moveScenario(file, 68773.0 - 76098.0, -71496.6 + 85826.0, 174.3, "Heft");
}

} // namespace scenario_rig
