// The road layer of the original's pathing (kenshi-k16.7.48; PATHFINDING_STATIC.md 1.13 and 2.8; roads_static.md).
// Part 1, kworld::RoadNetwork on hand-made roads: entry and exit on the nearest segment, A* over junctions, the use test
// (legs + roadLength x factor)^2 <= straight^2, hex marker order. Part 2 (needs the install): the vanilla roads load (2109) and
// a long move between two towns takes them. Part 3 (needs the install and its navtiles): Simulation::routeRetry takes a move over
// 1000 u through the road's markers when the use test passes, and the ladder's road rung (roadNetworkRoute) walks them.

#include "ksim/combat/combat_data.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "kworld/roads.hpp"

#include "knav/native.hpp"

#include "sim_access.hpp"
#include "test_util.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
using kworld::RoadNetwork;

RoadNetwork::Road road(int a, int b, std::vector<RoadNetwork::Marker> markers)
{
	RoadNetwork::Road r;
	r.start = a;
	r.end = b;
	r.markers = std::move(markers);
	for (std::size_t i = 0; i + 1 < r.markers.size(); ++i)
		r.length += std::hypot(r.markers[i + 1].x - r.markers[i].x, r.markers[i + 1].z - r.markers[i].z);
	return r;
}

void synthetic()
{
	// Two roads meeting at junction 2: x 0..2000 along z 0, then 2000..4000 bending up to z 300 and back.
	const RoadNetwork net = RoadNetwork::fromRoads({
		road(1, 2, { {0, 0, 0}, {1000, 0, 0}, {2000, 0, 0} }),
		road(2, 3, { {2000, 0, 0}, {3000, 0, 300}, {4000, 0, 0} }),
	});
	test::require(net.roads().size() == 2, "both roads kept");
	const auto path = net.route(-100.0, 50.0, 4100.0, 50.0);
	test::require(path.size() >= 5, "a move from beside the road's start to beside its end takes the road (" + std::to_string(path.size()) + " nodes)");
	test::require(std::fabs(path.front().x - 0.0) < 1.0 && std::fabs(path.back().x - 4000.0) < 1.0, "the entry is the road's start, the exit its end");
	bool bend = false;
	for (const auto& m : path)
		bend = bend || (std::fabs(m.x - 3000.0) < 1.0 && std::fabs(m.z - 300.0) < 1.0);
	test::require(bend, "the markers of the second road are walked in order");
	// The same move backwards walks them reversed.
	const auto back = net.route(4100.0, 50.0, -100.0, 50.0);
	test::require(back.size() == path.size() && std::fabs(back.front().x - 4000.0) < 1.0, "the reverse move walks the roads reversed");
	// Entry and exit mid-road: the markers between only.
	const auto mid = net.route(1200.0, 20.0, 3500.0, 20.0);
	test::require(!mid.empty() && std::fabs(mid.front().x - 1200.0) < 1.0, "an entry mid-segment is the projected point");
	// Use test: a goal off the road's line where the legs cost more than the road saves.
	test::require(net.route(1000.0, 18000.0, 3000.0, 18000.0).empty(), "legs of 18 km each way: the road is not taken");
	// A road that is nowhere near the way: the road runs 4 km one way while the goal is 1.2 km the other.
	test::require(net.route(2000.0, 0.0, 800.0, 3000.0).empty(), "a road that leads away from the goal is not taken");
	// A move under the one-road case: entry and exit on one road, nothing between.
	const auto same = net.route(100.0, 10.0, 900.0, 10.0);
	test::require(same.size() == 2, "entry and exit on one segment: the two points");
	// Not connected: a second road with no shared junction.
	const RoadNetwork split = RoadNetwork::fromRoads({ road(1, 2, { {0, 0, 0}, {2000, 0, 0} }), road(5, 6, { {2000, 0, 100}, {4000, 0, 100} }) });
	test::require(split.route(0.0, 20.0, 4000.0, 20.0).empty(), "roads that do not connect are not a route");
	// The stuck call's factor 0.2 discounts the road more (bigger detours pass): a 1.9x detour passes at 0.2 and fails at 0.6.
	const RoadNetwork bent = RoadNetwork::fromRoads({ road(1, 2, { {0, 0, 0}, {1000, 0, 1500}, {2000, 0, 0} }) });
	test::require(bent.route(0.0, 20.0, 2000.0, 20.0, 0.6).empty(), "a road 1.8x the straight line fails the use test at factor 0.6");
	test::require(!bent.route(0.0, 20.0, 2000.0, 20.0, 0.2).empty(), "and passes it at the stuck fallback's 0.2");
	// The factor comes from the faction's road preference: max(0, 1 - preference). A faction with preference 0.8 (factor 0.2) takes the
	// 1.8x detour that the vanilla 0.4 (factor 0.6) refuses; preference 1.0 or more disables roads.
	test::require(std::fabs(RoadNetwork::factorFor(0.4) - RoadNetwork::kDefaultFactor) < 1e-9, "vanilla preference 0.4 is factor 0.6");
	test::require(bent.route(0.0, 20.0, 2000.0, 20.0, RoadNetwork::factorFor(0.4)).empty(), "preference 0.4: the detour fails the use test");
	test::require(!bent.route(0.0, 20.0, 2000.0, 20.0, RoadNetwork::factorFor(0.8)).empty(), "preference 0.8: the same detour passes");
	test::require(RoadNetwork::factorFor(1.5) == 0.0, "a preference above 1 clamps the factor to 0");
}

void install()
{
	if (!std::filesystem::is_directory(std::string(KENSHI_DATA) + "/newland/leveldata/Newwworld"))
	{
		std::cerr << "ksim_road_route: part 2 skipped: no Newwworld leveldata in " << KENSHI_DATA << "\n";
		return;
	}
	const RoadNetwork net = RoadNetwork::load(KENSHI_DATA);
	test::require(net.roads().size() > 2000 && net.roads().size() <= 2109, "the vanilla roads load: " + std::to_string(net.roads().size()));
	// The Hub (-50979, 2932) to a town 6 km along the road network: some move over 1 km takes the roads.
	int taken = 0;
	for (const double dx : { 3000.0, 6000.0, -6000.0, 12000.0 })
		for (const double dz : { 0.0, 4000.0, -5000.0 })
			if (!net.route(-50979.0, 2932.0, -50979.0 + dx, 2932.0 + dz).empty())
				++taken;
	test::require(taken > 0, "no long move from the Hub takes any road");
	// Heft gate -> stone camp (e9s9.28): the chain holds every marker of the original's walk, in particular the seven (roads 1250 #4,
	// 1248 #1 #0, 383808953 #9..#5) that the walked trail was seen to miss: they are lost after the router, in the follower.
	const auto chain = net.route(67046.8, -72096.9, 65406.1, -50248.6);
	test::require(!chain.empty(), "the Heft walk takes the roads");
	const double want[][2] = {
		{ 68456.1, -71304.4 }, // road 1250 marker 4
		{ 70460.3, -70940.2 }, // road 1248 marker 1
		{ 70585.9, -70841.1 }, // road 1248 marker 0
		{ 70603.4, -69608.4 }, // road 383808953 marker 9
		{ 70579.7, -69576.1 }, // road 383808953 marker 8
		{ 70399.8, -69477.7 }, // road 383808953 marker 7
		{ 70150.7, -69610.9 }, // road 383808953 marker 6
		{ 69883.1, -69457.0 }, // road 383808953 marker 5
	};
	for (const auto& w : want)
	{
		bool found = false;
		for (const auto& m : chain)
			found = found || std::hypot(m.x - w[0], m.z - w[1]) < 2.0;
		test::require(found, "the Heft chain lacks the original's marker at " + std::to_string(w[0]) + "," + std::to_string(w[1]));
	}
}

void simulation()
{
	if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
	{
		std::cerr << "ksim_road_route: part 3 skipped: no shipped navtiles in " << KENSHI_DATA << "\n";
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
	sim.setRoadLookahead(0.0); // this test measures the exe's marker handover (rwh.268); the look-ahead is ksim_road_lookahead's
	sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
	sim.setGameData(std::make_shared<const kdata::GameData>(gd));
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
	const flecs::entity entity = sim.spawnFighter(std::move(f));
	const std::uint64_t id = entity.get<state::Id>()->value;

	// The factor is the walker's faction's: two factions with different road preferences give different factors.
	{
		const kdata::Record* other = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("FACTION")))
			if (r->floatOr("road preference", 0.4f) != 0.4f && !other)
				other = r;
		const double own = sim.roadFactor(id);
		test::require(std::fabs(own - 0.0) < 1e-9 || own >= 0.0, "roadFactor is non-negative");
		if (other != nullptr)
		{
			const std::string was = entity.get<state::Allegiance>() != nullptr ? entity.get<state::Allegiance>()->factionSid : std::string();
			entity.set<state::Allegiance>({ other->stringID });
			const double got = sim.roadFactor(id);
			test::require(std::fabs(got - RoadNetwork::factorFor(other->floatOr("road preference", 0.4f))) < 1e-6, "roadFactor follows the faction's road preference");
			test::require(std::fabs(got - own) > 1e-6 || was == other->stringID, "two factions with different preferences give different factors");
			entity.set<state::Allegiance>({ was });
		}
	}

	// A goal 1.2-2 km away on walkable ground with a plain route no longer than 1.3x the straight line, and a detour point M beside the
	// middle that the plain route does not pass (so a route through M can only come from a road).
	const knav::Position from{ spot->x, spot->y, spot->z };
	const state::Fighter& me = *sim.combat()->find(id);
	bool placed = false;
	double gx = 0.0, gz = 0.0, mx = 0.0, mz = 0.0;
	for (int a = 0; a < 16 && !placed; ++a)
		for (const double reach : { 1500.0, 1800.0, 1300.0 })
		{
			const double ang = a * 3.14159265358979 / 8.0;
			const auto goal = nav->walker().snap({ spot->x + std::cos(ang) * reach, spot->y, spot->z + std::sin(ang) * reach }, 60.0);
			if (!goal || std::hypot(goal->x - spot->x, goal->z - spot->z) < 1100.0)
				continue;
			const Route plain = nav->routeFrom(from, goal->x, goal->z, std::nullopt, true, 4.0);
			if (!plain.found || plain.points.size() < 2)
				continue;
			double len = 0.0;
			for (std::size_t i = 1; i < plain.points.size(); ++i)
				len += std::hypot(plain.points[i].x - plain.points[i - 1].x, plain.points[i].z - plain.points[i - 1].z);
			const double straight = std::hypot(goal->x - spot->x, goal->z - spot->z);
			if (len > 1.3 * straight)
				continue;
			// M beside the middle, 250 u to the side.
			const double cx = (spot->x + goal->x) / 2.0 - std::sin(ang) * 250.0, cz = (spot->z + goal->z) / 2.0 + std::cos(ang) * 250.0;
			const auto m = nav->walker().snap({ cx, spot->y, cz }, 40.0);
			if (!m || !nav->routeFrom(from, m->x, m->z, std::nullopt, true, 4.0).found || !nav->routeFrom({ m->x, m->y, m->z }, goal->x, goal->z, std::nullopt, true, 4.0).found)
				continue;
			double nearest = 1e9;
			for (const auto& p : plain.points)
				nearest = std::min(nearest, std::hypot(p.x - m->x, p.z - m->z));
			if (nearest < 60.0)
				continue;
			gx = goal->x;
			gz = goal->z;
			mx = m->x;
			mz = m->z;
			placed = true;
			break;
		}
	test::require(placed, "no walkable 1.1 km move with a free detour point in the Hub window");
	const double straight = std::hypot(gx - spot->x, gz - spot->z);

	const auto passes = [&](const Route& r)
	{
		for (const auto& p : r.points)
			if (std::hypot(p.x - mx, p.z - mz) < 12.0)
				return true;
		return false;
	};
	// Before: no road, the walk is the plain route and misses M.
	sim.setRoadNetwork(std::make_shared<const RoadNetwork>());
	const Route plain = sim.routeRetry(id, me, gx, gz, std::nullopt, true);
	test::require(plain.found && !passes(plain), "without roads the route is the plain search");
	// A road from beside the walker through M to beside the goal.
	const auto net = std::make_shared<const RoadNetwork>(RoadNetwork::fromRoads({
		road(1, 2, { { spot->x + 5.0, 0, spot->z + 5.0 }, { mx, 0, mz }, { gx - 5.0, 0, gz - 5.0 } }) }));
	sim.setRoadNetwork(net);
	sim.forgetPathRetries();
	const Route viaRoad = sim.routeRetry(id, me, gx, gz, std::nullopt, true);
	test::require(viaRoad.found && passes(viaRoad), "a move over 1000 u takes the road: the route passes the marker");
	test::require(std::hypot(viaRoad.points.back().x - gx, viaRoad.points.back().z - gz) < 2.0, "and still ends on the goal");
	// Under 1000 u the road is not asked for: the same road, a goal 600 u along it.
	const double ux = (gx - spot->x) / straight, uz = (gz - spot->z) / straight;
	const auto shortGoal = nav->walker().snap({ spot->x + ux * 600.0, spot->y, spot->z + uz * 600.0 }, 30.0);
	if (shortGoal)
	{
		sim.forgetPathRetries();
		const Route shortRoute = sim.routeRetry(id, me, shortGoal->x, shortGoal->z, std::nullopt, true);
		test::require(shortRoute.found && !passes(shortRoute), "a move under 1000 u does not take the road");
	}
	// The ladder's road rung: the call itself, at the stuck fallback's factor, walks the markers; with no roads it is not found.
	const Route rung = sim.roadNetworkRoute(me, gx, gz);
	test::require(rung.found && passes(rung), "the road-network rung walks the road's markers");
	// The road follower (kenshi-e9s9.10, rwh.268): a hand-made road S - M - G with a 60 degree turn at M and width 100. The walker hands M over
	// within the width and plans on from where it is: it cuts the corner by about the width, keeps its speed, and never stops before the goal.
	{
		const double ux = (gx - spot->x) / straight, uz = (gz - spot->z) / straight;
		double turnX = 0.0, turnZ = 0.0;
		bool found60 = false;
		for (const double side : { 1.0, -1.0 })
		{
			const double off = side * std::tan(30.0 * 3.14159265358979 / 180.0) * straight / 2.0;
			const auto m = nav->walker().snap({ (spot->x + gx) / 2.0 - uz * off, spot->y, (spot->z + gz) / 2.0 + ux * off }, 15.0);
			if (!m || !nav->routeFrom(from, m->x, m->z, std::nullopt, true, 4.0).found || !nav->routeFrom({ m->x, m->y, m->z }, gx, gz, std::nullopt, true, 4.0).found)
				continue;
			turnX = m->x;
			turnZ = m->z;
			found60 = true;
			break;
		}
		if (found60)
		{
			RoadNetwork::Road r = road(1, 2, { { spot->x + 5.0, 0, spot->z + 5.0 }, { turnX, 0, turnZ }, { gx - 5.0, 0, gz - 5.0 } });
			r.width = 100.0;
			sim.setRoadNetwork(std::make_shared<const RoadNetwork>(RoadNetwork::fromRoads({ r })));
			sim.forgetPathRetries();
			sim.enqueue(Command::fighterMoveTo(state::Id{ id }, { { gx, gz } }, { 0.0 }, 9));
			double closest = 1e9, peak = 0.0, speedNear = -1.0;
			std::size_t stops = 0, near540 = 0;
			for (int t = 0; t < 20 * 400; ++t)
			{
				sim.run(1);
				const state::Fighter& w = *sim.combat()->find(id);
				const double toGoal = std::hypot(gx - w.x, gz - w.z);
				const double toTurn = std::hypot(turnX - w.x, turnZ - w.z);
				peak = std::max(peak, w.forwardSpeed);
				closest = std::min(closest, toTurn);
				if (toTurn < 100.0 && speedNear < 0.0)
					speedNear = w.forwardSpeed;
				if (t > 100 && toGoal > 30.0 && w.forwardSpeed < 0.1 * peak)
					++stops;
				if (const auto* held = sim.farOrder(id); held != nullptr && toGoal < 540.0)
				{
					++near540;
					test::require(held->next >= held->markers.size(), "within 547.7 u of the goal a road marker is still being walked");
				}
				if (toGoal < 30.0)
					break;
			}
			std::cout << "ksim_road_route: road follower: closest approach to the 60 degree marker " << closest << " u (width 100), speed at the handover "
				<< speedNear << " of peak " << peak << ", stops " << stops << ", ticks inside 547.7 u with the order held " << near540 << "\n";
			test::require(closest > 20.0 && closest < 130.0, "the walker did not cut the corner by about the width: closest " + std::to_string(closest));
			test::require(speedNear >= 0.7 * peak, "the walker slowed at the handover: " + std::to_string(speedNear) + " of " + std::to_string(peak));
			test::require(stops == 0, std::to_string(stops) + " ticks stopped away from the goal");
			test::require(std::hypot(gx - sim.combat()->find(id)->x, gz - sim.combat()->find(id)->z) < 30.0, "the walker did not reach the goal");
			// A save taken while the walker is on a marker leg (before the handover) and reloaded walks on as the straight run: the same place 400 ticks in.
			const auto leg = [&](bool reload) -> std::pair<double, double>
			{
				state::Fighter* w = fighterOf(sim, id);
				w->x = spot->x;
				w->y = spot->y;
				w->z = spot->z;
				w->forwardSpeed = 0.0;
				w->vx = w->vz = 0.0;
				sim.enqueue(Command::fighterStop(state::Id{ id }, false)); // the walker the last leg left mid-route would carry its heading and route into this one
				sim.run(40);
				sim.enqueue(Command::fighterMoveTo(state::Id{ id }, { { gx, gz } }, { 0.0 }, reload ? 12 : 11));
				sim.run(60);
				if (reload)
				{
					const auto* held = sim.farOrder(id);
					test::require(held != nullptr && held->onMarker, "the walker is not on a marker leg 3 s in");
					std::ostringstream out;
					sim.save(out);
					std::istringstream in(out.str());
					sim.reload(in);
					held = sim.farOrder(id);
					test::require(held != nullptr && held->onMarker && held->markers[held->cur].width == 100.0, "the marker leg state was not restored by the load");
				}
				sim.run(340);
				const state::Fighter* e = sim.combat()->find(id);
				return { e->x, e->z };
			};
			leg(false); // the first run after the walk above starts from a different state: a warm-up, two straight runs from here are alike
			const auto straightEnd = leg(false);
			const auto loadedEnd = leg(true);
			const double apart = std::hypot(straightEnd.first - loadedEnd.first, straightEnd.second - loadedEnd.second);
			std::cout << "ksim_road_route: road follower save: " << apart << " u between the straight run and the reloaded one after 400 ticks\n";
			test::require(apart < 0.01, "a save on a marker leg reloads to a different walk: " + std::to_string(apart) + " u apart");
		} else {
			std::cerr << "ksim_road_route: road follower case skipped: no walkable 60 degree marker\n";
		}
	}
	sim.setRoadNetwork(std::make_shared<const RoadNetwork>());
	test::require(!sim.roadNetworkRoute(me, gx, gz).found, "with no roads the rung finds nothing");
}

} // namespace

int main()
{
	try
	{
		synthetic();
		install();
		simulation();
		std::cout << "ksim_road_route: the road pre-route and the road-network rung take the road when the use test passes\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_road_route: " << e.what() << "\n";
		return 1;
	}
}
