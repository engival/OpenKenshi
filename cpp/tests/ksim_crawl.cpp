// Task BF: a conscious down character crawls on a move order (user play: the character is down with a collapsed leg,
// not KO'd, and cannot move at all; in the original it crawls). The cause was not the run speed (gaitOf / knav::runSpeed
// never look at the legs) and not an order refusal (only dead is refused): CombatArena::step's one branch for
// `unconscious || down` said "the body lies where it fell", zeroed the forward speed and returned before the order walk.
// Now a down body takes a move order, a drive or a player's attack order and moves at CombatArena::kCrawlSpeed with
// `down` still set; a KO'd body still never moves. Kinspect bf_crawl_down.md 9a539ac: the crawl speed is a constant
// (CharStats::getSpeed's `if (down) speed = 5.0`, overriding athletics/legs/blood/encumbrance; live 4.97-5.0), a down
// player's attack order crawls to the target but never lands a blow, and a down NPC does not move by itself.
//
// Player orders only, Hub navmesh: collapse the player's Right Leg (the same rule ksim_ko_long pins), order a walk about
// 40 u away, and check it arrives with down still 1 and never faster than 5 u/s; order an attack and check it crawls
// toward the servant without landing a blow; then a KO'd player with a move order must stay put.
//
// argv[1]: the Hub navmesh cache (ctest fixture knav_cache).

#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

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

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_crawl HUB.knav");
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
		const CombatRules& rules = sim.combat()->rules();

		// A collapsed Right Leg: its effective health below the character's KO point puts the body down, and the collapse
		// fires the forced KO writer (task BL, fb0248d): 3.0 medical timer units, then awake with down still set. No bandage
		// pool, so it stays down while the crawl runs.
		{
			state::FighterPart* leg = partNamed(*fighterOf(sim, duel.player), "Right Leg");
			test::require(leg != nullptr, "the player has no Right Leg");
			leg->health = rules.koPoint(*fighterOf(sim, duel.player)) - 1.0;
		}
		sim.tick();
		{
			const state::Fighter* player = fighterOf(sim, duel.player);
			test::require(player->down, "the collapsed leg did not put the player down");
			test::require(player->unconscious, "the collapse's forced KO did not start");
			// 3.0 medical units, less the one substep that runs after the KO is set (0.05 game s x the medical clock).
			const double oneSubstep = kTickSeconds * settings.medicalClock;
			test::require(player->koTimer > 3.0 - oneSubstep - 0.01 && player->koTimer <= 3.0,
				"the collapse's KO timer is not 3.0 medical units: " + std::to_string(player->koTimer));
			std::printf("down at tick 1: leg health %.1f, ko point %.1f, KO timer %.2f\n",
				partNamed(*fighterOf(sim, duel.player), "Right Leg")->health, rules.koPoint(*player), player->koTimer);
		}
		// 3.0 medical units at the medical clock: 1.5 s = 30 ticks at the old 2.0, ~63 at the synced ~0.96; the body wakes
		// and down stays (it crawls).
		sim.run(static_cast<int>(std::ceil(3.0 / (kTickSeconds * settings.medicalClock))) + 2);
		test::require(!fighterOf(sim, duel.player)->unconscious && fighterOf(sim, duel.player)->down,
			"the body did not wake from the collapse's KO still down");

		// A walk about 50 u out, away from the servant (so the attack order below starts outside the engage range), order
		// as the client would: a route from the server's own search, then the order.
		const state::Fighter* start = fighterOf(sim, duel.player);
		const state::Fighter* enemy = fighterOf(sim, duel.enemy);
		const double startX = start->x;
		const double startZ = start->z;
		const double away = std::atan2(start->x - enemy->x, start->z - enemy->z);
		Route route;
		for (const double d : {50.0, 70.0, 40.0})
		{
			for (int k = 0; k < 16 && !route.found; ++k)
			{
				const double a = away + k * 0.392699;
				const Route tryRoute = sim.navigation()->routeFrom({start->x, start->y, start->z},
					start->x + std::sin(a) * d, start->z + std::cos(a) * d);
				if (tryRoute.found && tryRoute.points.size() >= 2)
					route = tryRoute;
			}
			if (route.found)
				break;
		}
		test::require(route.found, "no routable destination near the player");
		const state::GroundPoint goal = route.points.back();
		sim.enqueue(Command::fighterMoveTo(state::Id{ duel.player }, route.points, route.heights, 7));

		int ticks = 0;
		double moved = 0.0;
		double fastest = 0.0;
		double lastX = startX;
		double lastZ = startZ;
		while (ticks < 6000)
		{
			sim.tick();
			++ticks;
			const state::Fighter* player = fighterOf(sim, duel.player);
			test::require(player->down, "the player stood up mid-crawl (down cleared) at tick " + std::to_string(ticks));
			test::require(!player->unconscious, "the player became unconscious mid-crawl");
			const double step = std::hypot(player->x - lastX, player->z - lastZ);
			moved += step;
			fastest = std::max(fastest, step / kTickSeconds);
			lastX = player->x;
			lastZ = player->z;
			if (!player->walking())
				break;
		}
		const state::Fighter* player = fighterOf(sim, duel.player);
		test::require(!player->walking(), "the crawl did not finish within 6000 ticks");
		test::require(moved > 20.0, "the player barely moved (" + std::to_string(moved) + " u): the crawl is not wired");
		test::require(fastest <= CombatArena::kCrawlSpeed * 1.2,
			"the crawler went faster than the crawl cap (fastest " + std::to_string(fastest) + " u/s)");
		test::require(std::hypot(player->x - goal.x, player->z - goal.z) <= 2.0, "the crawl did not arrive at the route's end");
		std::printf("crawled %.1f u in %d ticks (fastest %.2f u/s, cap %.1f), down still %d\n", moved, ticks, fastest,
			CombatArena::kCrawlSpeed, player->down ? 1 : 0);

		// A down player's attack order is taken too: it crawls toward the target and never lands a blow (kinspect
		// bf_crawl_down.md 9a539ac: ~200 u crawled, entered combat, no blow in 10 s).
		{
			state::Fighter* servant = fighterOf(sim, duel.enemy);
			enemy = servant;
			test::require(enemy != nullptr, "the servant is gone");
			state::Fighter* f = fighterOf(sim, duel.player);
			// The crawler only moves while the target is out of the walk's engage range, and the servant has walked up to the
			// down player during the crawl above: put it back 80 u out on the mesh first (task BL re-pin; the loss of the
			// 32 ticks the collapse's KO costs moved the approach).
			if (std::hypot(f->x - enemy->x, f->z - enemy->z) < 40.0)
			{
				for (int k = 0; k < 16; ++k)
				{
					const double a = k * 0.392699;
					const auto spot = sim.navigation()->walker().snap({f->x + std::sin(a) * 80.0, f->y, f->z + std::cos(a) * 80.0}, 100.0);
					if (!spot)
						continue;
					servant->x = spot->x;
					servant->y = spot->y;
					servant->z = spot->z;
					break;
				}
			}
			const double before = std::hypot(f->x - enemy->x, f->z - enemy->z);
			double enemyStun = 0.0;
			for (const state::FighterPart& p : enemy->parts)
				enemyStun += p.stun;
			sim.enqueue(Command::fighterAttack(state::Id{ duel.player }, state::Id{ duel.enemy }));
			sim.tick();
			std::printf("attack order: orderTarget %llu target %llu distance %.1f\n",
				static_cast<unsigned long long>(fighterOf(sim, duel.player)->orderTarget),
				static_cast<unsigned long long>(fighterOf(sim, duel.player)->target), before);
			for (int i = 0; i < 599; ++i) // 30 s: far more than the crawl needs
			{
				sim.tick();
				const state::Fighter* crawler = fighterOf(sim, duel.player);
				test::require(crawler->down, "the player stood up under an attack order (down cleared)");
				const double speed = std::hypot(crawler->x - lastX, crawler->z - lastZ) / kTickSeconds;
				test::require(speed <= CombatArena::kCrawlSpeed * 1.2, "the attacking crawler exceeded the crawl speed");
				lastX = crawler->x;
				lastZ = crawler->z;
			}
			const state::Fighter* after = fighterOf(sim, duel.player);
			const state::Fighter* enemyAfter = fighterOf(sim, duel.enemy);
			test::require(std::hypot(after->x - enemyAfter->x, after->z - enemyAfter->z) < before,
				"the attack order did not crawl toward the target");
			double enemyStunAfter = 0.0;
			for (const state::FighterPart& p : enemyAfter->parts)
				enemyStunAfter += p.stun;
			test::require(enemyStunAfter <= enemyStun + 1e-9, "a down attacker landed a blow");
			std::printf("attack order: crawled from %.1f u to %.1f u, no blow\n", before,
				std::hypot(after->x - enemyAfter->x, after->z - enemyAfter->z));
		}

		// KO'd = no movement, even with a move order (the user's other half: only a conscious down body crawls).
		{
			state::Fighter* f = fighterOf(sim, duel.player);
			f->koTimer = 60.0; // 30 s of KO at dt 0.1 per tick, well past the 5 s probe
			f->unconscious = true;
			sim.tick();
			const Route koRoute = sim.navigation()->routeFrom({f->x, f->y, f->z}, f->x + 30.0, f->z);
			test::require(koRoute.found, "no route for the KO case");
			sim.enqueue(Command::fighterMoveTo(state::Id{ duel.player }, koRoute.points, koRoute.heights, 8));
			const double x0 = f->x;
			const double z0 = f->z;
			for (int i = 0; i < 100; ++i)
				sim.tick();
			f = fighterOf(sim, duel.player);
			test::require(f->unconscious, "the KO'd player woke inside the 5 s test");
			test::require(std::hypot(f->x - x0, f->z - z0) < 1.0, "a KO'd body moved on a move order");
		}
		std::printf("ksim_crawl: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_crawl: FAIL: " << e.what() << "\n";
		return 1;
	}
}
