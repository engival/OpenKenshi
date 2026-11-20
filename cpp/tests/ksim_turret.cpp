// Task ds/turret S3: the turret mount and its gunner (dt41_B). A gunner mounted on a turret shoots with the turret's GUN_DATA numbers
// and its Turrets skill, in range and in the arc, not outside either; it lets go when moved off the post or when told to. And the wiring: a
// resident whose running goal is 149 on a turret building, standing at its post, mounts it, shoots, and dismounts when the goal moves on.

#include "ksim/combat/combat.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/collision.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

namespace { constexpr double kPi = 3.14159265358979323846; } // M_PI is not in MinGW's strict -std=c++20 <cmath>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
using Shot = CombatArena::Shot;

void require(bool ok, const std::string& what)
{
	test::require(ok, what);
	std::printf("ok  %s\n", what.c_str());
}

const kdata::Record* recordNamed(const kdata::GameData& gd, const char* type, const char* name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
		if (r->name == name)
			return r;
	throw std::runtime_error(std::string("no ") + type + " " + name);
}

bool harmed(const state::Fighter& f, double bloodBefore)
{
	return f.dead || f.unconscious || f.blood < bloodBefore - 1.0e-6;
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const kdata::Record* bandit = recordNamed(gd, "CHARACTER", "Dust Bandit");
		const kdata::Record* turretGun = recordNamed(gd, "GUN_DATA", "Turret 00");

		// ---- the turret's gun: the GUN_DATA row's numbers, not a crossbow's ----
		const state::FighterRanged gun = data->makeTurretGun(gd, *turretGun);
		require(gun.armed() && gun.range == turretGun->intOr("range", -1) && gun.deviation == turretGun->floatOr("accuracy deviation at 0 skill", -1.0f)
			&& gun.perfectSkill == turretGun->intOr("accuracy perfect skill", -1) && gun.numShots == turretGun->intOr("num shots", -1)
			&& gun.reloadMin == turretGun->floatOr("reload time min", -1.0f) && gun.reloadMax == turretGun->floatOr("reload time max", -1.0f)
			&& gun.pierceMax > 0.0, "Turret 00 carries its own GUN_DATA range, accuracy, shots and reload, and a bolt's damage");
		const state::FighterRanged twin = data->makeTurretGun(gd, *recordNamed(gd, "GUN_DATA", "Turret double"));
		require(twin.numShots == 2 && twin.range > 0.0 && twin.range != gun.range, "Turret double fires 2 shots, with a range of its own");

		// ---- the arena: a mounted gunner shoots in the arc and nowhere else ----
		std::uint64_t rng = 5;
		const auto fight = [&](double targetX, double targetZ, double seconds, std::int32_t* fired, std::uint16_t* task, double* moved)
		{
			CombatArena arena(data, CombatSettings{}, 21);
			state::Fighter gunner = data->makeFighter(gd, bandit->stringID, "", rng);
			state::Fighter target = data->makeFighter(gd, bandit->stringID, "", rng);
			gunner.team = 0;
			target.team = 1;
			gunner.x = 0.0;
			gunner.z = 0.0;
			gunner.skills.turrets = 100.0;
			target.x = targetX;
			target.z = targetZ;
			arena.add(1, std::move(gunner));
			arena.add(2, std::move(target));
			require(arena.mountTurret(1, 77, 0.0, 0.0, 0.0, gun), "the gunner mounts");
			require(!arena.mountTurret(1, 78, 0.0, 0.0, 0.0, gun), "a mounted gunner mounts nothing else");
			perception::notice(*arena.find(1), 2);
			perception::notice(*arena.find(2), 1);
			const std::int32_t bolts = arena.find(1)->mount.gun.ammo;
			const double bloodBefore = arena.find(2)->blood;
			*task = 0;
			for (int i = 0; i < static_cast<int>(seconds * 20.0); ++i)
			{
				arena.step(0.05);
				if (arena.find(1)->rangedTask != 0)
					*task = arena.find(1)->rangedTask;
			}
			*fired = bolts - arena.find(1)->mount.gun.ammo;
			*moved = std::hypot(arena.find(1)->x, arena.find(1)->z);
			return harmed(*arena.find(2), bloodBefore);
		};
		std::int32_t fired = 0;
		std::uint16_t task = 0;
		double moved = 0.0;
		require(fight(0.0, 400.0, 20.0, &fired, &task, &moved), "a target 400 u ahead was not hurt");
		require(task == goap::kShootAtTarget && fired > 0, "the gunner did not run 235 / fire");
		require(moved < 1.0, "the gunner left its post");
		require(!fight(0.0, -400.0, 6.0, &fired, &task, &moved) && fired == 0 && task == 0, "a target behind the turret (out of the arc) was shot at");
		require(!fight(0.0, 1500.0, 6.0, &fired, &task, &moved) && fired == 0, "a target beyond the turret's range was shot at");
		// (The negative cases run 6 s: the target walks at the gunner and would, given longer, come round into the arc.)
		// A 400 u target at 80 degrees is inside the 180 degree arc, one at 100 degrees is outside it.
		require(fight(400.0 * std::sin(80.0 * kPi / 180.0), 400.0 * std::cos(80.0 * kPi / 180.0), 20.0, &fired, &task, &moved) && fired > 0,
			"a target 80 degrees off the facing was not shot");
		require(!fight(400.0 * std::sin(100.0 * kPi / 180.0), 400.0 * std::cos(100.0 * kPi / 180.0), 6.0, &fired, &task, &moved) && fired == 0,
			"a target 100 degrees off the facing was shot");

		// ---- let go: told to, or moved off the post ----
		{
			CombatArena arena(data, CombatSettings{}, 21);
			state::Fighter gunner = data->makeFighter(gd, bandit->stringID, "", rng);
			gunner.team = 0;
			arena.add(1, std::move(gunner));
			arena.mountTurret(1, 77, 0.0, 0.0, 0.0, gun);
			arena.dismountTurret(1);
			require(!arena.find(1)->mount.mounted() && !arena.find(1)->gun().armed(), "dismountTurret left the gunner mounted or armed");
			arena.mountTurret(1, 77, 0.0, 0.0, 0.0, gun);
			arena.find(1)->x = 100.0; // knocked off the post
			arena.step(0.05);
			require(!arena.find(1)->mount.mounted(), "a gunner moved 100 u off its post kept the turret");
		}

		// ---- the wiring: a resident on goal 149 at a turret's post mounts it, shoots, and lets go when the goal moves on ----
		{
			SpawnOptions options;
			options.seed = 12;
			options.wildernessByDensity = false;
			Simulation sim = Simulation::newGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), options, nullptr, RegionConfig{});
			sim.setTurretManning(true);
			CombatSettings settings;
			settings.substeps = 2;
			sim.enableCombat(CombatData::build(*sim.gameData(), KENSHI_DATA), settings);
			const kworld::PlacedInstance* placed = nullptr;
			const kdata::Record* building = nullptr;
			for (const kworld::PlacedInstance& candidate : sim.world().placements())
			{
				const kdata::Record* record = gd.find(candidate.buildingID);
				if (record != nullptr && routines::isTurretBuilding(gd, *record))
				{
					placed = &candidate;
					building = record;
					break;
				}
			}
			require(placed != nullptr, "the world places a turret building");
			const std::array<double, 4> q{placed->qw, placed->qx, placed->qy, placed->qz};
			double x = placed->x;
			double z = placed->z;
			if (const std::vector<routines::UseNode> nodes = routines::useNodePoses(gd, *building); !nodes.empty())
			{
				const std::array<double, 3> local = routines::rotate(q, nodes.front().at);
				x += local[0];
				z += local[2];
			}
			const std::uint64_t turretKey = kworld::doorKey(kworld::placementId(*placed));
			const std::array<double, 3> ahead = routines::rotate(q, {0.0, 0.0, 1.0});
			const double yaw = std::atan2(ahead[0], ahead[2]);
			const auto add = [&](std::uint64_t team, double px, double pz)
			{
				state::Fighter f = data->makeFighter(gd, bandit->stringID, "", rng);
				f.team = team;
				f.x = px;
				f.z = pz;
				f.y = 0.0;
				f.attention = 0.5;
				return sim.spawnFighter(std::move(f));
			};
			const flecs::entity gunner = add(0, x, z);
			const flecs::entity enemy = add(1, x + 300.0 * std::sin(yaw), z + 300.0 * std::cos(yaw));
			const std::uint64_t gunnerId = gunner.get<state::Id>()->value;
			const std::uint64_t enemyId = enemy.get<state::Id>()->value;
			state::Routine routine;
			routine.goal = routines::task::ManTurrets;
			routine.target = {static_cast<std::uint8_t>(routines::SiteKind::Turret), turretKey, 0};
			routine.nextScore = ~0ULL; // the scorer leaves the goal alone: this test is the mount, not the finder
			gunner.set<state::Routine>(routine);
			perception::notice(*const_cast<CombatArena*>(sim.combat())->find(gunnerId), enemyId);
			perception::notice(*const_cast<CombatArena*>(sim.combat())->find(enemyId), gunnerId);
			const double enemyBlood = sim.combat()->find(enemyId)->blood;
			bool mounted = false;
			bool shot = false;
			for (int n = 0; n < 600; ++n)
			{
				sim.tick();
				const state::Fighter* g = sim.combat()->find(gunnerId);
				mounted = mounted || (g != nullptr && g->mount.mounted() && g->mount.building == turretKey);
				shot = shot || (g != nullptr && g->rangedTask == goap::kShootAtTarget);
			}
			require(mounted, "the resident at the turret's post did not mount it");
			require(shot, "the mounted resident did not shoot (235)");
			require(harmed(*sim.combat()->find(enemyId), enemyBlood), "the turret's shots did the enemy no harm");
			routine.goal = routines::task::StayInHome; // the goal moved on: the task's stop
			gunner.set<state::Routine>(routine);
			for (int n = 0; n < 20; ++n)
				sim.tick();
			require(!sim.combat()->find(gunnerId)->mount.mounted(), "the gunner kept the turret after its goal moved on");
		}
		std::printf("ksim_turret: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
