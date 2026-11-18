// Task BH: the legs scale the run speed (kinspect `140884970`, run_speed findings; BF live side fact). The leg factor is
// min(left, right leg effective health / max health) clamped to 0..1, and it multiplies the excess over the 11 u/s floor
// together with the encumbrance ratio: a healthy leg is the plain run speed, a leg at half health gives 11 + (base - 11)/2,
// and a leg at 0 - still above the KO point, so the body stands - gives exactly 11 u/s.
//
// Player orders only, Hub navmesh: remove the duel's servant, order a long walk, and measure the top speed of the tick
// displacements for a healthy body, a half leg (the other leg healthy: the MIN wins) and a leg at 0.
//
// argv[1]: the Hub navmesh cache (ctest fixture knav_cache).

#include "ksim/clock.hpp"
#include "ksim/combat.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

state::Fighter* fighterOf(Simulation& sim, std::uint64_t id)
{
	return const_cast<CombatArena*>(sim.combat())->find(id);
}

state::FighterPart* partNamed(state::Fighter& f, std::string_view name)
{
	for (state::FighterPart& p : f.parts)
	{
		if (p.name == name)
			return &p;
	}
	return nullptr;
}

// The same min the gait uses.
double legsRatio(const state::Fighter& f)
{
	double worst = 1.0;
	bool any = false;
	for (const state::FighterPart& p : f.parts)
	{
		if (p.name != "Left Leg" && p.name != "Right Leg")
			continue;
		any = true;
		worst = std::min(worst, p.maxHealth > 0.0 ? CombatRules::partEffective(p) / p.maxHealth : 1.0);
	}
	return any ? std::clamp(worst, 0.0, 1.0) : 1.0;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_leg_speed HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		test::require(nav != nullptr, "the Hub mesh did not load");

		Simulation sim = Simulation::load(KENSHI_DATA, 91);
		sim.setGameData(gd);
		simlink::ServerContext context;
		context.gameData = gd;
		context.dataHash = simlink::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		const simlink::Duel duel = simlink::spawnDuel(sim, *gd, *data, context);
		test::require(const_cast<CombatArena*>(sim.combat())->remove(duel.enemy), "the servant did not leave the arena");
		sim.tick();

		state::Fighter* player = fighterOf(sim, duel.player);
		state::FighterPart* left = partNamed(*player, "Left Leg");
		state::FighterPart* right = partNamed(*player, "Right Leg");
		test::require(left != nullptr && right != nullptr, "the duel player has no named legs");

		const auto lengthOf = [](const Route& r) {
			double sum = 0.0;
			for (std::size_t i = 1; i < r.points.size(); ++i)
				sum += std::hypot(r.points[i].x - r.points[i - 1].x, r.points[i].z - r.points[i - 1].z);
			return sum;
		};
		// One long walk from the player's spot; returns the top speed of the tick displacements.
		std::uint32_t seq = 1;
		const auto walkTop = [&](const char* what) {
			const state::Fighter& f = *fighterOf(sim, duel.player);
			Route route;
			for (const double d : {400.0, 300.0, 200.0})
			{
				for (int k = 0; k < 16 && !route.found; ++k)
				{
					const double a = k * 0.392699;
					const Route tryRoute = sim.navigation()->routeFrom({f.x, f.y, f.z},
						f.x + std::sin(a) * d, f.z + std::cos(a) * d);
					if (tryRoute.found && lengthOf(tryRoute) > 120.0)
						route = tryRoute;
				}
				if (route.found)
					break;
			}
			test::require(route.found, std::string(what) + ": no long route from the player");
			sim.enqueue(Command::fighterMoveTo(state::Id{ duel.player }, route.points, route.heights, seq++));
			double top = 0.0;
			int ticks = 0;
			while (ticks++ < 20000)
			{
				const state::Fighter* before = fighterOf(sim, duel.player);
				const double x = before->x;
				const double z = before->z;
				sim.tick();
				const state::Fighter* now = fighterOf(sim, duel.player);
				// The commanded speed, not the displacement: a navmesh correction snap on one tick would read as a spike
				// (seen: 90 u/s on a floor-speed walk). forwardSpeed is the gait speed the walker aims for.
				top = std::max(top, std::fabs(now->forwardSpeed));
				(void)x;
				(void)z;
				if (!now->walking())
					break;
			}
			const state::Fighter* now = fighterOf(sim, duel.player);
			test::require(!now->walking(), std::string(what) + ": the walk did not finish");
			test::require(!now->down && !now->unconscious, std::string(what) + ": the body went down");
			return top;
		};

		// 1. Healthy: the plain run speed (the arena's own gait, legs factor 1).
		const double healthy = walkTop("healthy");
		const double expectedHealthy = sim.combat()->runSpeed(*fighterOf(sim, duel.player));
		std::printf("healthy: top %.2f u/s (run speed %.2f, ratio %.3f)\n", healthy, expectedHealthy, legsRatio(*player));
		test::require(std::fabs(legsRatio(*player) - 1.0) < 1e-9, "the healthy legs are not ratio 1");
		test::require(std::fabs(healthy - expectedHealthy) <= 0.01 * expectedHealthy + 0.5,
			"the healthy walk reached " + std::to_string(healthy) + ", the run speed is " + std::to_string(expectedHealthy));

		// 2. Half a leg, the other healthy: the MIN wins, so 11 + (base - 11) / 2.
		left->health = 0.5 * left->maxHealth;
		left->stun = 0.0;
		left->bonus = 0.0;
		right->health = right->maxHealth;
		right->stun = 0.0;
		right->bonus = 0.0;
		sim.tick();
		const double ratioHalf = legsRatio(*fighterOf(sim, duel.player));
		test::require(std::fabs(ratioHalf - 0.5) < 1e-3, "a half leg is not ratio 0.5: " + std::to_string(ratioHalf));
		const double half = walkTop("half leg");
		const double expectedHalf = 11.0 + (healthy - 11.0) * ratioHalf;
		std::printf("half leg: top %.2f u/s, expected %.2f\n", half, expectedHalf);
		test::require(std::fabs(half - expectedHalf) <= 0.02 * expectedHalf + 0.5,
			"a half leg went at " + std::to_string(half) + ", expected " + std::to_string(expectedHalf));

		// 3. A leg at 0: the floor exactly, and the body still stands (0 is above the negative KO point).
		left->health = 0.0;
		left->stun = 0.0;
		sim.tick();
		test::require(legsRatio(*fighterOf(sim, duel.player)) <= 1e-9, "a leg at 0 is not ratio 0");
		const double floor = walkTop("leg at 0");
		std::printf("leg at 0: top %.2f u/s (the 11 u/s floor)\n", floor);
		test::require(std::fabs(floor - 11.0) < 0.05, "a leg at 0 went at " + std::to_string(floor) + ", not the 11 u/s floor");

		std::printf("ksim_leg_speed: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_leg_speed: FAIL: " << e.what() << "\n";
		return 1;
	}
}
