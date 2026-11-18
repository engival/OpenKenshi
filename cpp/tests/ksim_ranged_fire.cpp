// Task ds/ranged S1: the ranged fire core (kinspect dt41_A). Drives CombatArena::rangedFire on two Dust Bandits of opposite teams:
// the range test and its height factor, a hit in the open through the medical path, the reload timer and the ammo, a wall that blocks the
// shot (the arena's shotBlocker hook; LineOfSight::firstBlocker itself is in ksim_los), an ally in the line (no shot), an enemy in the line
// (no block), and the skill's effect on the deviation.

#include "ksim/combat/combat.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
using Shot = CombatArena::Shot;

void require(bool ok, const char* what)
{
	test::require(ok, what);
	std::printf("ok  %s\n", what);
}

double health(const state::Fighter& f)
{
	double sum = f.blood;
	for (const state::FighterPart& p : f.parts)
		sum += p.health;
	return sum;
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const kdata::Record* bandit = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Dust Bandit")
				bandit = r;
		}
		const kdata::Record* crossbow = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CROSSBOW")))
		{
			if (r->name == "Eagle's Cross")
				crossbow = r;
		}
		test::require(bandit != nullptr && crossbow != nullptr, "no Dust Bandit / Eagle's Cross in the data");
		std::uint64_t rng = 5;
		const state::FighterRanged gun = data->makeRanged(*crossbow, 0.5);
		const auto make = [&](int team, double x, double z)
		{
			state::Fighter f = data->makeFighter(gd, bandit->stringID, "", rng);
			f.team = team;
			f.x = x;
			f.z = z;
			f.skills.crossbows = 100.0; // past `accuracy perfect skill`: no deviation
			return f;
		};
		const auto fresh = [&](double dist, double dy = 0.0, double skill = 100.0)
		{
			auto arena = std::make_unique<CombatArena>(data, CombatSettings{}, 9);
			state::Fighter shooter = make(0, 0.0, 0.0);
			shooter.ranged = gun;
			shooter.skills.crossbows = skill;
			state::Fighter target = make(1, 0.0, dist);
			target.y = dy;
			arena->add(1, std::move(shooter));
			arena->add(2, std::move(target));
			return arena;
		};
		require(gun.range > 1000.0 && gun.numShots == 1 && gun.ammo > 0, "the Eagle's Cross numbers come from the record (range, shots, ammo)");

		// ---- 1. a shot in the open hits through the medical path; the ammo and the reload timer follow ----
		{
			auto arena = fresh(300.0);
			const double before = health(*arena->find(2));
			test::require(arena->rangedFire(1, 2, 0.1) == Shot::Hit, "a shot at 300 u in the open did not hit");
			require(health(*arena->find(2)) < before, "the hit hurt the target");
			require(arena->find(1)->ranged.ammo == gun.ammo - 1, "a shot costs one bolt");
			// Skill 100 reloads in `reload time min` (1 skill 0 = max): the next calls wait.
			require(std::fabs(arena->find(1)->ranged.timer - gun.reloadMin) < 1e-9, "the reload at skill 100 is `reload time min`");
			require(arena->rangedFire(1, 2, 0.1) == Shot::Waiting, "a second shot inside the reload fired");
			require(arena->rangedFire(1, 2, gun.reloadMin) == Shot::Hit, "the shot after the reload did not fire");
			auto slow = fresh(300.0, 0.0, 0.0);
			slow->rangedFire(1, 2, 0.1);
			require(std::fabs(slow->find(1)->ranged.timer - gun.reloadMax) < 1e-9, "the reload at skill 0 is `reload time max`");
		}
		// ---- 2. the range test: `range` x the height factor (target above: down to 0.4, below: up to 1.5; y 0 is 1) ----
		{
			require(fresh(gun.range + 100.0)->rangedFire(1, 2, 0.1) == Shot::OutOfRange, "beyond `range` still shot");
			require(fresh(gun.range - 100.0)->rangedFire(1, 2, 0.1) == Shot::Hit, "inside `range` did not shoot");
			require(fresh(gun.range * 1.4, -300.0)->rangedFire(1, 2, 0.1) == Shot::Hit, "a target 300 below (x1.5) at 1.4 range did not shoot");
			require(fresh(gun.range * 0.5, 300.0)->rangedFire(1, 2, 0.1) == Shot::OutOfRange, "a target 300 above (x0.4) at half range shot");
			require(fresh(gun.range * 0.35, 300.0)->rangedFire(1, 2, 0.1) == Shot::Hit, "a target 300 above at 0.35 range did not shoot");
		}
		// ---- 3. a wall stops the shot, and the shot goes when it is gone ----
		{
			auto arena = fresh(300.0);
			double at = 0.5;
			arena->environmentMutable().shotBlocker = [&at](const std::array<double, 3>&, const state::Fighter&) { return at; };
			const double before = health(*arena->find(2));
			test::require(arena->rangedFire(1, 2, 0.1) == Shot::Blocked, "a wall between the pair did not block the shot");
			require(health(*arena->find(2)) == before && arena->find(1)->ranged.ammo == gun.ammo, "a blocked shot cost no bolt and hurt nobody");
			require(std::fabs(arena->find(1)->ranged.timer - 0.3) < 1e-9, "a blocked shot retries in 0.3 s");
			at = -1.0;
			test::require(arena->rangedFire(1, 2, 0.3) == Shot::Hit, "the shot did not go once the wall was gone");
			std::printf("ok  the shot goes once the line is clear\n");
		}
		// ---- 4. bodies in the line: an ally aborts the shot, an enemy does not ----
		{
			for (const int team : {0, 1})
			{
				auto arena = fresh(300.0);
				arena->add(3, make(team, 0.0, 150.0));
				const Shot shot = arena->rangedFire(1, 2, 0.1);
				if (team == 0)
					require(shot == Shot::Blocked, "an ally in the line did not stop the shot");
				else
					require(shot == Shot::Hit, "an enemy in the line stopped the shot");
			}
			auto aside = fresh(300.0);
			aside->add(3, make(0, 40.0, 150.0)); // an ally off to the side
			require(aside->rangedFire(1, 2, 0.1) == Shot::Hit, "an ally beside the line stopped the shot");
		}
		// ---- 5. no bolts, no weapon, no target ----
		{
			auto arena = fresh(300.0);
			arena->find(1)->ranged.ammo = 0;
			require(arena->rangedFire(1, 2, 0.1) == Shot::NoAmmo, "an empty crossbow shot");
			require(arena->rangedFire(2, 1, 0.1) == Shot::NoWeapon, "an unarmed fighter shot");
			require(arena->rangedFire(1, 99, 0.1) == Shot::NoTarget, "a shot at nobody");
		}
		// ---- 6. skill sets the deviation: a novice at 1000 u misses most shots, a master none ----
		{
			int hits[2] = {0, 0};
			for (int i = 0; i < 2; ++i)
			{
				auto arena = fresh(1000.0, 0.0, i == 0 ? 1.0 : 100.0);
				for (int shot = 0; shot < 60; ++shot)
				{
					arena->find(1)->ranged.ammo = gun.ammo;
					arena->find(1)->ranged.timer = 0.0;
					arena->find(2)->dead = false; // the volley outlasts a target: stand it up again
					arena->find(2)->unconscious = false;
					arena->find(2)->blood = 1000.0;
					for (state::FighterPart& p : arena->find(2)->parts)
						p.health = p.maxHealth;
					hits[i] += arena->rangedFire(1, 2, 0.1) == Shot::Hit ? 1 : 0;
				}
			}
			std::printf("    1000 u, 60 shots: skill 1 hits %d, skill 100 hits %d\n", hits[0], hits[1]);
			require(hits[1] == 60, "a master (skill >= perfect) missed");
			require(hits[0] < 30 && hits[0] < hits[1], "a novice hit as often as a master");
		}
		// ---- 7. a caged body is nobody's victim (kenshi-k16.6.29): never picked, never kept, never shot; the same body uncaged is ----
		{
			for (const bool caged : {true, false})
			{
				auto arena = fresh(300.0);
				arena->find(2)->cageBuilding = caged ? 77 : 0;
				if (caged)
					require(arena->rangedFire(1, 2, 0.1) == Shot::NoTarget, "a direct shot at a caged body did not return NoTarget");
				ksim::perception::notice(*arena->find(1), 2);
				const double before = health(*arena->find(2));
				for (int i = 0; i < 20 * 10; ++i)
				{
					arena->step(0.05);
					if (caged)
						test::require(arena->find(1)->target == 0, "a crossbowman targeted a caged body");
				}
				if (caged)
					require(health(*arena->find(2)) == before && arena->find(1)->ranged.ammo == gun.ammo, "a crossbowman never targeted or shot a caged body");
				else
					require(arena->find(1)->ranged.ammo < gun.ammo, "a crossbowman shoots the same body uncaged");
			}
		}
		std::printf("ksim_ranged_fire: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
