// A walker pushed or teleported off its route re-requests the path after 5 s and arrives (kenshi-k16.8.2; HAVOKCHAR_EXE.md 3 step 6, C100-C102).
// The route is a street walk of the Hub; 3 s into it the walker is set down 40 u to the side, off the corridor (more than 3 footprints from the
// route's window), and set down there again every tick (a push that keeps shoving). Off the route for 5 s the walk re-plans (routeVersion bumps once, 5-6.5 s after the move, counted by
// CombatArena::offPathRepaths) and arrives. Held off for 2 s and let go, it steers back onto the route and the timer resets: no re-plan.
// Needs the install (KENSHI_DATA); exit 77 without it.

#include "ksim/combat/combat_data.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

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

constexpr double kTick = 0.05;
constexpr int kLimitTicks = 20 * 90;

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_path_offroute: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
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
		const double radius = data->pathRadius(data->makeFighter(gd, sid, "", rng).raceSid);

		// A long street walk: the first goal 250-500 u from the street spot with a route that long.
		const auto start = nav->walker().snap({ -51320.0, 1567.16, 2700.0 }, 8.0);
		test::require(start.has_value(), "the street spot is off the mesh");
		Route route;
		for (const double dx : { 300.0, -300.0, 400.0, -400.0 })
		{
			const Route cand = nav->routeFrom({ start->x, start->y, start->z }, start->x + dx, start->z, std::nullopt, false, radius, true);
			if (!cand.found || cand.points.size() < 3)
				continue;
			double len = 0.0;
			for (std::size_t i = 0; i + 1 < cand.points.size(); ++i)
				len += std::hypot(cand.points[i + 1].x - cand.points[i].x, cand.points[i + 1].z - cand.points[i].z);
			if (len >= 250.0)
			{
				route = cand;
				break;
			}
		}
		test::require(route.found, "no street walk of 250 u or more from the street spot");

		const auto run = [&](bool comeBack)
		{
			state::Fighter f = data->makeFighter(gd, sid, "", rng);
			f.playerControlled = true;
			f.x = start->x;
			f.y = start->y;
			f.z = start->z;
			const std::uint64_t id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
			sim.enqueue(Command::fighterMoveTo(state::Id{id}, route.points, route.heights, 1));
			const auto fighter = [&] { return const_cast<state::Fighter*>(sim.combat()->find(id)); };
			for (int tick = 0; tick < 60; ++tick) // 3 s of the walk
				sim.run(1);
			// Set down to the side of the route's next stretch: 40 u, snapped to the mesh, and off the corridor.
			state::Fighter* me = fighter();
			const std::size_t at = std::min<std::size_t>(me->routeNext, me->route.size() - 2);
			const double ax = me->route[at + 1].x - me->route[at].x;
			const double az = me->route[at + 1].z - me->route[at].z;
			const double len = std::hypot(ax, az);
			test::require(len > 1.0 && !me->route.empty(), "the walk ended or has no leg 3 s in");
			const double fx = me->x, fy = me->y, fz = me->z; // the walker's own spot: the side is measured from it
			double hx = 0.0, hy = 0.0, hz = 0.0;
			bool moved = false;
			for (const double side : { 40.0, -40.0, 60.0, -60.0 })
			{
				const auto there = nav->walker().snap({ fx - az / len * side, fy, fz + ax / len * side }, 8.0);
				if (there && std::hypot(there->x - fx, there->z - fz) > 3.0 * radius)
				{
					hx = there->x;
					hy = there->y;
					hz = there->z;
					moved = true;
					break;
				}
			}
			test::require(moved, "no mesh spot 40-60 u to the side of the route to set the walker on");
			const std::uint32_t version = me->routeVersion;
			// Held there, as a push that keeps shoving (a walker set down 40 u aside steers back onto its route within 2 s, and the timer
			// would reset): set down again before every tick until the re-plan, or for 2 s when it is then let go to steer back (it rejoins in under 2 s).
			const std::uint64_t repaths = sim.combat()->offPathRepaths();
			int replanTick = -1;
			for (int tick = 0; tick < kLimitTicks; ++tick)
			{
				me = fighter();
				if (replanTick < 0 && tick < (comeBack ? 40 : 130)) {
					me->x = hx;
					me->y = hy;
					me->z = hz;
				}
				sim.run(1);
				me = fighter();
				if (replanTick < 0 && me->walking() && me->routeVersion != version)
					replanTick = tick;
				if (!me->walking())
					break;
			}
			me = fighter();
			const bool arrived = std::hypot(me->x - route.points.back().x, me->z - route.points.back().z) < 4.0;
			if (comeBack)
			{
				test::require(replanTick < 0 && sim.combat()->offPathRepaths() == repaths, "off the route for 2 s then let go: the walk re-planned (tick " + std::to_string(replanTick) + ", off-route re-plans " + std::to_string(sim.combat()->offPathRepaths() - repaths) + ", reroutes " + std::to_string(sim.combat()->reroutes()) + ")");
				test::require(arrived, "off the route for 2 s then let go: did not arrive");
			} else {
				test::require(replanTick >= 0, "5 s off the route: no re-plan");
				const double seconds = (replanTick + 1) * kTick;
				test::require(seconds >= 4.9 && seconds <= 6.5, "the re-plan came " + std::to_string(seconds) + " s after the move, not at 5 s");
				test::require(sim.combat()->offPathRepaths() == repaths + 1, "expected one off-route re-plan");
				test::require(arrived, "the re-planned walk did not arrive (at " + std::to_string(me->x) + ", " + std::to_string(me->z) + ")");
			}
			sim.removeFighter(id); // its body would stand on the next run's goal: the arrival there settles R = 8 off it (k16.6.231)
		};
		run(false);
		run(true);
		std::cout << "ksim_path_offroute: off the route 5 s re-plans and arrives; back after 2 s does not\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_path_offroute: " << e.what() << "\n";
		return 1;
	}
}
