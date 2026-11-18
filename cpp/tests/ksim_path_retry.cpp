// The failed-path ladder (kenshi-k16.8.2; HAVOKCHAR_EXE.md 2.2, C57, C166-C168, C171-C172; LOCOMOTION.md 9, D8).
// Part 1, the ladder itself: each request after a failed one for the same goal is retry n, offset sideways by 100 n (1 + 0.2 n) with the sign
// alternating (a goal that fails direct goes through 16 sideways asks, one road-network ask, then "stuck": no more asks); a new goal, a success
// of the goal itself, a removed character and a new navmesh start over.
// Part 2 (needs the install, KENSHI_DATA; skipped without): Simulation::routeRetry for a walker whose goal has no route (outside the loaded
// window): the owner gets a failed route (NoRoute) on every ask, never a hang, and routeStuck() says when the ladder is used up (the exe's vf10).

#include "ksim/combat_data.hpp"
#include "ksim/navigator.hpp"
#include "ksim/path_retry.hpp"
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

bool near(double a, double b) { return std::fabs(a - b) < 1.0e-6; }

void ladder()
{
	PathRetries ladder;
	// Walker at the origin, goal 1000 u along +z: the sideways axis is x (perpendicular, to the left of the heading).
	PathRetries::Ask a = ladder.ask(7, 0.0, 0.0, 0.0, 1000.0);
	test::require(a.step == PathRetries::Step::Direct && near(a.x, 0.0) && near(a.z, 1000.0) && a.retries == 0, "the first ask is the goal itself");
	ladder.result(7, false);
	for (int n = 1; n <= PathRetries::kSideways; ++n)
	{
		a = ladder.ask(7, 0.0, 0.0, 0.0, 1000.0);
		test::require(a.step == PathRetries::Step::Sideways && a.retries == n, "retry " + std::to_string(n) + " is a sideways ask");
		const double d = 100.0 * n * (1.0 + 0.2 * n);
		test::require(near(std::fabs(a.x), d) && near(a.z, 1000.0), "retry " + std::to_string(n) + " offset is " + std::to_string(d) + ", got x " + std::to_string(a.x));
		test::require((a.x < 0.0) == (n % 2 == 1), "the sign of retry " + std::to_string(n) + " follows the parity of n");
		ladder.result(7, false);
		test::require(!ladder.stuck(7), "stuck before the ladder is used up");
	}
	test::require(near(PathRetries::offset(1), 120.0) && near(PathRetries::offset(16), 6720.0), "offset(1) = 120, offset(16) = 6720");
	a = ladder.ask(7, 0.0, 0.0, 0.0, 1000.0);
	test::require(a.step == PathRetries::Step::RoadNet, "after 16 retries comes the road network");
	ladder.result(7, false);
	test::require(!ladder.stuck(7), "the road network ask is not the last");
	for (int i = 0; i < 5; ++i)
	{
		a = ladder.ask(7, 0.0, 0.0, 0.0, 1000.0);
		test::require(a.step == PathRetries::Step::Stuck, "after the road network: completely stuck, no asks");
		ladder.result(7, false);
	}
	test::require(ladder.stuck(7), "stuck() is the owner's failure");
	// A new goal starts over.
	a = ladder.ask(7, 0.0, 0.0, 50.0, 1000.0);
	test::require(a.step == PathRetries::Step::Direct && a.retries == 0 && !ladder.stuck(7), "a new goal resets the counter");
	ladder.result(7, false);
	// A success on a sideways goal keeps the count; the owner's next ask is for the goal itself again (the exe: not FAILED, so no bump), and
	// if that fails the ask after it is retry n + 1.
	a = ladder.ask(7, 0.0, 0.0, 50.0, 1000.0);
	test::require(a.step == PathRetries::Step::Sideways && a.retries == 1, "after a failed goal the next ask is retry 1");
	ladder.result(7, true);
	a = ladder.ask(7, 0.0, 0.0, 50.0, 1000.0);
	test::require(a.step == PathRetries::Step::Direct && a.retries == 1, "after a sideways success the next ask is the goal itself, count kept");
	ladder.result(7, false);
	a = ladder.ask(7, 0.0, 0.0, 50.0, 1000.0);
	test::require(a.step == PathRetries::Step::Sideways && a.retries == 2 && a.x > 50.0, "the ask after that failure is retry 2, the other side (retry 1 went to -x)");
	// The goal itself found with no retries on the count ends the ladder.
	ladder.result(7, true);
	ladder.reset(7);
	ladder.ask(7, 0.0, 0.0, 50.0, 1000.0);
	ladder.result(7, true);
	test::require(ladder.retries(7) == 0, "a found goal leaves nothing behind");
	// invalidate / removal.
	ladder.result(7, false);
	ladder.ask(7, 0.0, 0.0, 50.0, 1000.0);
	ladder.resetAll();
	test::require(ladder.retries(7) == 0, "resetAll (a new navmesh, C57) zeroes the counter");
	ladder.ask(8, 0.0, 0.0, 0.0, 10.0);
	ladder.result(8, false);
	ladder.reset(8);
	test::require(ladder.ask(8, 0.0, 0.0, 0.0, 10.0).retries == 0, "reset(id) (the character left) starts over");
}

void simulation()
{
	if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
	{
		std::cerr << "ksim_path_retry: part 2 skipped: no shipped navtiles in " << KENSHI_DATA << "\n";
		return;
	}
	std::vector<kworld::ZoneCell> hub;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			hub.push_back({ simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz });
	const std::shared_ptr<MeshNavigator> nav = MeshNavigator::openNative(KENSHI_DATA, hub);
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
	Simulation sim = Simulation::load(KENSHI_DATA, 77);
	sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
	sim.enableCombat(data, CombatSettings{});
	sim.enableNavigation(nav);
	std::string sid;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		if (r->name == "Escaped Servant")
			sid = r->stringID;
	std::uint64_t rng = 12345;
	state::Fighter f = data->makeFighter(gd, sid, "", rng);
	const auto spot = nav->walker().snap({ -51320.0, 1567.16, 2700.0 }, 8.0);
	test::require(spot.has_value(), "the street spot is off the mesh");
	f.x = spot->x;
	f.y = spot->y;
	f.z = spot->z;
	const std::uint64_t id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	const state::Fighter& me = *sim.combat()->find(id);
	// A goal far outside the loaded window: no route for the goal itself or for any sideways offset (up to 6.7 km).
	const double goalX = spot->x + 400000.0;
	const double goalZ = spot->z;
	test::require(!nav->routeFrom({ me.x, me.y, me.z }, goalX, goalZ, std::nullopt, true, 4.0).found, "the far goal must have no route");
	for (int ask = 1; ask <= 25; ++ask)
	{
		const Route route = sim.routeRetry(id, me, goalX, goalZ, std::nullopt, true);
		test::require(ask < 19 || route.failure == state::AiReason::NoRoute, "ask " + std::to_string(ask) + ": the stuck ask answers NoRoute");
		test::require(!route.found, "ask " + std::to_string(ask) + ": found a route to nowhere");
		// Asks 1-18 are the goal, 16 sideways retries and the road net; the 19th and later are not made and the ladder is used up.
		test::require(sim.routeStuck(id) == (ask >= 19), "ask " + std::to_string(ask) + ": routeStuck is " + (sim.routeStuck(id) ? "true" : "false"));
	}
	// The ladder used up on the far goal is reset by a door book (a door or gate changed state: routability changed, C57's invalidate).
	test::require(sim.routeStuck(id), "the far goal's ladder is used up");
	nav->applyDoors(knav::DoorBook{});
	sim.routeRetry(id, me, goalX, goalZ, std::nullopt, true);
	test::require(!sim.routeStuck(id), "a door book applied: the next ask starts the ladder over");
	// A door changes routability: a goal behind a locked door ladders up to stuck; the door opens (a new door book on the navigator) and the next
	// ask is Direct and finds the route. The door: the first of the mesh's doors whose two sides (60 u either way across it) route to each other
	// open and not when locked.
	{
		const knav::DoorBook none;
		struct Pair { double ax, ay, az, bx, by, bz; knav::DoorKey key; };
		Pair pair{};
		bool found = false;
		for (const knav::DoorKey key : nav->mesh().doorKeys())
			for (const double reach : { 15.0, 30.0, 60.0 })
			{
				knav::Point centre;
				knav::NavMesh::DoorFootprint fp;
				if (found || !nav->mesh().doorCentre(key, centre, &fp))
					continue;
				const double nx = -fp.axisZ, nz = fp.axisX; // across the door
				const auto a = nav->walker().snap({ centre[0] + nx * reach, centre[1], centre[2] + nz * reach }, 20.0);
				const auto b = nav->walker().snap({ centre[0] - nx * reach, centre[1], centre[2] - nz * reach }, 20.0);
				if (!a || !b)
					continue;
				knav::DoorBook locked;
				locked.setState(key, knav::DoorState::Locked);
				nav->applyDoors(none);
				const Route open = nav->routeFrom({ a->x, a->y, a->z }, b->x, b->z, b->y, false, 4.0);
				nav->applyDoors(locked);
				const Route shut = nav->routeFrom({ a->x, a->y, a->z }, b->x, b->z, b->y, false, 4.0);
				nav->applyDoors(none);
				if (open.found && !shut.found)
				{
					pair = Pair{ a->x, a->y, a->z, b->x, b->y, b->z, key };
					found = true;
				}
			}
		test::require(found, "no door in the Hub whose two sides do not route to each other when it is locked");
		state::Fighter g = data->makeFighter(gd, sid, "", rng);
		g.x = pair.ax;
		g.y = pair.ay;
		g.z = pair.az;
		const std::uint64_t gid = sim.spawnFighter(std::move(g)).get<state::Id>()->value;
		const state::Fighter& walker = *sim.combat()->find(gid);
		knav::DoorBook locked;
		locked.setState(pair.key, knav::DoorState::Locked);
		nav->applyDoors(locked);
		// A retry's sideways goal may well be reachable (the door's own side), so the locked door fails the first ask and the second is a retry; the
		// door opening before the second makes it the goal itself again, not a retry offset 120 u.
		test::require(!sim.routeRetry(gid, walker, pair.bx, pair.bz, pair.by, false).found, "the locked door's far side has a route");
		nav->applyDoors(none); // the door opens
		const Route through = sim.routeRetry(gid, walker, pair.bx, pair.bz, pair.by, false);
		test::require(through.found, "the door opened: the next ask finds the route");
		test::require(std::hypot(through.points.back().x - pair.bx, through.points.back().z - pair.bz) < 2.0, "the route through the open door ends at the goal, not at an offset");
	}
	// The owner's act on the failure: a new goal asks again at once. Find the walker again: the second spawn above may have moved the arena's
	// members, so `me` can dangle (it did, k16.6.235's hashed race() read its raceSid).
	sim.routeRetry(id, *sim.combat()->find(id), spot->x + 100.0, spot->z, std::nullopt, true);
	test::require(!sim.routeStuck(id), "a new goal clears the stuck ladder");
}

} // namespace

int main()
{
	try
	{
		ladder();
		simulation();
		std::cout << "ksim_path_retry: the ladder goes sideways 16 times, tries the road network, then gives up\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_path_retry: " << e.what() << "\n";
		return 1;
	}
}
