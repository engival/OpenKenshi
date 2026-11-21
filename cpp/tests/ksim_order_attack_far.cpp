// A player's attack order on a far target survives other attackers engaging (kenshi-k16.6.337, kenshi-karw: "I ran away, then ordered an attack on
// someone close to the bar door. I expected my character to run past all the other attackers and go after them"). The original (rwh.165): a click
// order sits in the job queue, which the goal selector considers before the tier-3 combat goals (self preservation among them); being targeted only
// replans and the same order restarts; combat never drops it. Here: the player is ordered onto a target ~30 u away while three hostiles are on the
// player; it keeps the order (orderTarget, target), runs past them and reaches the target.
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
constexpr std::uint64_t kTarget = 9;
constexpr double kReach = 15.0; // in the fight with the target: a duel circles and is knocked apart at 7-25 u, the held crowd case stalled at 29 u (u)
// The Storm House bar of the playtest: a spot inside, the open ground outside its door.
constexpr double kStartX = -50620.0;
constexpr double kStartZ = -1135.0;
constexpr double kGoalX = -50606.0;
constexpr double kGoalZ = -1066.0;

struct Result
{
	bool reached = false;
	bool dropped = false; // the order or the held target went to anyone but the ordered enemy
	double seconds = 0.0;
	int hits = 0;
	std::uint64_t lastTarget = 0;
	std::string end; // the player's state when the run ends (diagnostic)
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
	const double sx = route.points.front().x;
	const double sz = route.points.front().z;
	arena.add(1, make(sx, sz, 0, true));
	// The ordered enemy stands ~30 u off along the route; the attackers (the same team) ring the player.
	arena.add(kTarget, make(route.points.back().x, route.points.back().z, 1, false));
	for (int i = 0; i < attackers; ++i)
	{
		const double a = 0.8 + 2.1 * i;
		arena.add(2 + i, make(sx + 9.0 * std::sin(a), sz + 9.0 * std::cos(a), 1, false));
	}
	arena.setObserver([&](const CombatEvent& e)
	{
		out.hits += e.who == 1 && e.kind == CombatEvent::Kind::Hit ? 1 : 0;
	});
	for (int step = 0; step < kEngageSteps; ++step)
		arena.step(kDt);
	state::Fighter* me = arena.find(1);
	me->orderTarget = kTarget;
	me->clearRoute();
	me->hold = false;
	const state::Fighter* target = arena.find(kTarget);
	for (int step = 0; step < 600 && !out.reached; ++step)
	{
		arena.step(kDt);
		me = arena.find(1);
		out.seconds = (step + 1) * kDt;
		// The crowd must not put the runner down (a vital part under the KO point): the test judges the order, not the fight's price.
		for (state::FighterPart& part : arena.find(1)->parts)
		{
			part.health = part.maxHealth;
			part.stun = 0.0;
		}
		arena.find(1)->blood = 150.0;
		out.lastTarget = me->target;
		if (std::getenv("OAF_TRACE") != nullptr)
			std::printf("t %.2f pos (%.1f, %.1f) d %.1f state %d goal %u order %llu target %llu fwd %.1f hp-hits %d\n", out.seconds, me->x, me->z,
				std::hypot(target->x - me->x, target->z - me->z), static_cast<int>(me->state), static_cast<unsigned>(me->combatGoal),
				static_cast<unsigned long long>(me->orderTarget), static_cast<unsigned long long>(me->target), me->forwardSpeed, out.hits);
		if (!me->dead && me->orderTarget != kTarget && !target->dead)
			out.dropped = true;
		if (step % 40 == 0 || me->dead || me->unconscious)
		{
			char line[200];
			std::snprintf(line, sizeof line, "t %.1f d %.1f dead %d ko %d down %d state %d blood %.0f order %llu target %llu walking %d", out.seconds,
				std::hypot(target->x - me->x, target->z - me->z), me->dead, me->unconscious, me->down, static_cast<int>(me->state), me->blood,
				static_cast<unsigned long long>(me->orderTarget), static_cast<unsigned long long>(me->target), me->walking() ? 1 : 0);
			out.end = line;
		}
		if (me->dead || me->unconscious)
			break;
		out.reached = std::hypot(target->x - me->x, target->z - me->z) < kReach && me->target == kTarget;
	}
	return out;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_order_attack_far HUB.knav");
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
		for (const int attackers : {0, 3})
		{
			for (std::uint64_t seed = 5; seed < 9; ++seed)
			{
				const Result r = run(gd, data, nav, servant, attackers, seed);
				std::printf("attackers %d seed %llu: reached %d in %.2f s, hits %d, dropped %d, target %llu\n", attackers, static_cast<unsigned long long>(seed),
					r.reached, r.seconds, r.hits, r.dropped, static_cast<unsigned long long>(r.lastTarget));
				if (!r.reached)
					std::printf("  end: %s\n", r.end.c_str());
				test::require(!r.dropped, "the attack order is never dropped by the attackers engaging");
				test::require(r.reached, "the ordered character reaches the far target and fights it");
			}
		}
		std::printf("ksim_order_attack_far: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
