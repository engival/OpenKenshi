// A player character with a move order keeps running it under attack (kenshi-k16.6.343, user playtest 2026-11-17, kenshi-k16.6.342: shoved ~30 u
// along the wall by three attackers, never back on its route). The original (docs/reference/COMBAT_MOVE_LIVE.md, rwh.172): bodies do not block,
// a hit zeroes the velocity for ~0.2 s and nothing else, no knockback, no re-plan; a crowd costs the runner within ~5% of the baseline run.
// One Hub bar door run, once alone and then with three attackers on it (several seeds): the runner reaches the goal, is never carried backwards
// along its route, and the crowd run costs no more than the baseline plus the original's price per landed hit.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/combat/combat.hpp"
#include "ksim/nav/navigator.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;
constexpr int kEngageSteps = 20; // 1 s of fight before the order
// The Storm House bar of the playtest: a spot inside, the open ground outside its door.
constexpr double kStartX = -50620.0;
constexpr double kStartZ = -1135.0;
constexpr double kGoalX = -50606.0;
constexpr double kGoalZ = -1066.0;
// The original's price of one landed hit on a runner (rwh.172 section 2: ~0.2 s lost) and of a heavy stagger (~0.45 s), and the crowd's own: 5%.
constexpr double kHitCost = 0.25;
constexpr double kStaggerCost = 0.45;
constexpr double kCrowdSlack = 0.05;

struct Result
{
	bool arrived = false;
	double seconds = 0.0;
	double worst = 0.0; // the largest step taken away from the goal along the route's direction, in one step (u)
	double drift = 0.0; // the largest distance from the route's segment
	double peak = 0.0; // the top speed reached: a wounded leg lowers it, and the run is then judged against that gait
	int hits = 0;
	int stumbles = 0;
	std::uint32_t version = 0;
};

Result run(const kdata::GameData& gd, const std::shared_ptr<const CombatData>& data, const std::shared_ptr<MeshNavigator>& nav,
	const kdata::Record* servant, int attackers, std::uint64_t seed)
{
	CombatArena arena(data, CombatSettings{}, seed);
	arena.setNavigation(nav);
	std::uint64_t rng = seed;
	Result out;
	const Route route = nav->routeFrom({kStartX, 1080.0, kStartZ}, kGoalX, kGoalZ);
	test::require(route.found, "no route from the bar to the goal");
	std::printf("route: %zu points, first (%.0f, %.0f) last (%.0f, %.0f)\n", route.points.size(), route.points.front().x, route.points.front().z, route.points.back().x, route.points.back().z);
	const auto make = [&](double x, double z, int team, bool player)
	{
		state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
		f.ranged = state::FighterRanged{};
		f.x = x;
		f.z = z;
		f.y = nav->walker().snap({x, route.heights.front(), z}, 10.0).value_or(knav::Position{x, route.heights.front(), z}).y;
		f.team = team;
		f.playerControlled = player;
		return f;
	};
	state::Fighter runner = make(route.points.front().x, route.points.front().z, 0, true);
	for (std::size_t i = 0; i < route.points.size(); ++i)
		runner.route.push_back({route.points[i].x, route.heights[i], route.points[i].z});
	runner.routeNext = 0;
	runner.routeSource = state::Fighter::PlanSource::Order;
	runner.routeApproach = route.approach;
	runner.orderStop = true;
	// The order is given after the fight has started (the playtest's): the route is held back until the attackers have engaged.
	std::vector<state::RoutePoint> held;
	held.swap(runner.route);
	arena.add(1, std::move(runner));
	// The attackers stand around the runner's first steps, as the playtest's three did at the door.
	for (int i = 0; i < attackers; ++i)
	{
		const double a = 0.8 + 2.1 * i;
		arena.add(2 + i, make(route.points.front().x + 9.0 * std::sin(a), route.points.front().z + 9.0 * std::cos(a), 1, false));
	}
	arena.setObserver([&](const CombatEvent& e)
	{
		if (e.who != 1)
			return;
		out.hits += e.kind == CombatEvent::Kind::Hit ? 1 : 0;
		out.stumbles += e.kind == CombatEvent::Kind::Stumble ? 1 : 0;
	});
	const state::Fighter* me = arena.find(1);
	for (int step = 0; step < kEngageSteps; ++step)
		arena.step(kDt);
	out.hits = 0;
	out.stumbles = arena.find(1)->state == state::CombatState::Stumble ? 1 : 0; // staggered when the order comes: it costs a stagger too
	arena.find(1)->route = held;
	arena.find(1)->routeNext = 0;
	++arena.find(1)->routeVersion;
	out.version = me->routeVersion;
	double lastX = me->x;
	double lastZ = me->z;
	const double gx = route.points.back().x;
	const double gz = route.points.back().z;
	double goalDistance = std::hypot(gx - me->x, gz - me->z);
	for (int step = 0; step < 600 && !out.arrived; ++step)
	{
		arena.step(kDt);
		me = arena.find(1);
		out.seconds = (step + 1) * kDt;
		if (std::getenv("MUA_TRACE") != nullptr)
			std::printf("t %.2f pos (%.1f, %.1f) fwd %.1f state %d walking %d routeNext %zu/%zu target %llu stumbleT %.2f\n", out.seconds, me->x, me->z, me->forwardSpeed,
				static_cast<int>(me->state), me->walking() ? 1 : 0, static_cast<std::size_t>(me->routeNext), me->route.size(), static_cast<unsigned long long>(me->target), me->stumbleTimer);
		const double d = std::hypot(gx - me->x, gz - me->z);
		out.worst = std::max(out.worst, d - goalDistance);
		out.peak = std::max(out.peak, me->forwardSpeed);
		goalDistance = d;
		lastX = me->x;
		lastZ = me->z;
		out.arrived = me->route.empty() && d < 6.0;
	}
	(void)lastX;
	(void)lastZ;
	out.version = me->routeVersion - out.version;
	return out;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_move_under_attack HUB.knav");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		test::require(nav != nullptr, "the Hub mesh did not load");
		const kdata::Record* servant = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r;
		}
		test::require(servant != nullptr, "no Escaped Servant");

		const Result base = run(gd, data, nav, servant, 0, 5);
		std::printf("baseline: arrived %d in %.2f s, worst step back %.2f u\n", base.arrived, base.seconds, base.worst);
		test::require(base.arrived, "the unattacked runner reaches the goal");
		for (std::uint64_t seed = 5; seed < 11; ++seed)
		{
			const Result r = run(gd, data, nav, servant, 3, seed);
			std::printf("seed %llu: arrived %d in %.2f s, hits %d, stumbles %d, peak %.0f u/s, worst step back %.2f u, re-routes %u\n",
				static_cast<unsigned long long>(seed), r.arrived, r.seconds, r.hits, r.stumbles, r.peak, r.worst, r.version);
			test::require(r.arrived, "the attacked runner reaches the goal");
			const double gait = r.peak > 1.0 ? std::max(1.0, base.peak / r.peak) : 1.0;
			test::require(r.seconds <= base.seconds * gait * (1.0 + kCrowdSlack) + kStaggerCost * r.stumbles + kHitCost * r.hits + kDt,
				"the crowd costs no more than the original's: 5% and the hits");
			test::require(r.worst < 1.0, "no hit or body carries the runner back from the goal");
		}
		std::printf("ksim_move_under_attack: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
