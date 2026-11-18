// Walks up and down the Hub's stairs arrive (kenshi-k16.8.3). A survey of every stacked floor of the Hub's 3x3 zones (about 20 upper-floor
// spots, thinned to one per 60 u) walked from the street and back by a player character with the sim's walker: every walk with a route
// arrived. The survey's first "stalls" (bar floor 1, two spots) were the walker pausing at a shut door on the stair (the door step takes the route,
// opens it and gives the route back), not a stall; a walk that stands still for 20 s fails here.
// The walks below are the bar's first floor behind a door and upper floors 80-110 u over the street, both ways.
// Needs the install (KENSHI_DATA); exit 77 without it.

#include "ksim/combat_data.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "knav/native.hpp"

#include "test_util.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

struct Spot
{
	const char* what;
	double x, y, z;
};

constexpr Spot kStreet = { "the street", -51320.0, 1567.16, 2700.0 };

const Spot kStairs[] = {
	{ "bar floor 1 (door)", -51168.0, 1647.3, 2612.0 },
	{ "bar floor 1, west (door)", -51216.0, 1648.1, 2660.0 },
	{ "upper floor, south-east", -50736.0, 1653.97, 2611.4 },
	{ "upper floor, east", -50760.0, 1663.97, 2681.6 },
	{ "upper floor, north", -51072.0, 1671.46, 2464.4 },
	{ "upper floor, mid", -50976.0, 1657.09, 2516.0 },
};

constexpr int kLimitTicks = 20 * 60; // a minute: the longest walk (740 u at 70 u/s, plus a door) takes 15 s

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_stair_walks: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> hub;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hub.push_back({ simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz });
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::openNative(KENSHI_DATA, hub);
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		Simulation sim = Simulation::load(KENSHI_DATA, 77);
		CombatSettings settings;
		settings.substeps = 2;
		sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		std::string sid;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
			if (r->name == "Escaped Servant")
				sid = r->stringID;
		std::uint64_t rng = 12345;
		const auto walk = [&](const Spot& from, const Spot& to, const std::string& tag)
		{
			const auto spot = nav->walker().snap({ from.x, from.y, from.z }, 8.0);
			test::require(spot.has_value(), tag + ": the start is off the mesh");
			state::Fighter f = data->makeFighter(gd, sid, "", rng);
			f.playerControlled = true;
			f.x = spot->x;
			f.y = spot->y;
			f.z = spot->z;
			const Route route = nav->routeFrom({ f.x, f.y, f.z }, to.x, to.z, to.y, false, data->pathRadius(f.raceSid), true);
			test::require(route.found && route.points.size() >= 2, tag + ": no route");
			const std::uint64_t id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
			sim.enqueue(Command::fighterMoveTo(state::Id{id}, route.points, route.heights, 1));
			double lastX = spot->x, lastZ = spot->z;
			int still = 0;
			bool arrived = false;
			for (int tick = 0; tick < kLimitTicks && !arrived; ++tick)
			{
				sim.run(1);
				const state::Fighter& me = *sim.combat()->find(id);
				arrived = std::hypot(me.x - route.points.back().x, me.z - route.points.back().z) < 2.0;
				still = std::hypot(me.x - lastX, me.z - lastZ) < 0.02 ? still + 1 : 0;
				lastX = me.x;
				lastZ = me.z;
				test::require(still < 20 * 20, tag + ": stands still for 20 s at (" + std::to_string(me.x) + ", " + std::to_string(me.y) + ", " + std::to_string(me.z) + ")");
			}
			test::require(arrived, tag + ": did not arrive in a minute");
			const state::Fighter& me = *sim.combat()->find(id);
			test::require(std::fabs(me.y - to.y) < 6.0, tag + ": arrived on another floor (y " + std::to_string(me.y) + ")");
			sim.removeFighter(id); // no idle body left standing on the next walk's start: each walk is the bare walker
		};
		for (const Spot& stair : kStairs)
		{
			walk(kStreet, stair, std::string("up to ") + stair.what);
			walk(stair, kStreet, std::string("down from ") + stair.what);
		}
		std::cout << "ksim_stair_walks: " << 2 * std::size(kStairs) << " stair walks arrived\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_stair_walks: " << e.what() << "\n";
		return 1;
	}
}
