// Task ds/ranged S2: the ranged goals. The planner rows 262 / 263 / 266 / 264 / 235 (a crossbow hand plans [262], a melee hand keeps its melee
// plan), and an NPC with a crossbow at range: its attack runs as the ranged task, it shoots from where it stands and never closes to melee.

#include "ksim/combat/combat.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
using goap::Sym;

void require(bool ok, const char* what)
{
	test::require(ok, what);
	std::printf("ok  %s\n", what);
}

class Stage final : public goap::World
{
public:
	explicit Stage(std::set<Sym> held) : held_(std::move(held)) {}
	bool holds(Sym sym, std::uint64_t, double, double) const override { return held_.count(sym) != 0; }

private:
	std::set<Sym> held_;
};

std::vector<std::uint16_t> planOf(const std::set<Sym>& held, std::uint16_t goal)
{
	std::vector<std::uint16_t> out;
	for (const goap::Step& step : goap::plan(Stage(held), goal, 7, 0.0, 0.0).steps)
		out.push_back(step.task);
	return out;
}

} // namespace

int main()
{
	try
	{
		// ---- 1. the planner: a crossbow hand plans the ranged row, a melee hand the melee ones ----
		{
			const std::set<Sym> bow{Sym::IsMovementAllowed, Sym::HasWeaponEquipped, Sym::HasTwoWorkingArms, Sym::HoldsCrossbow};
			require(planOf(bow, 16) == (std::vector<std::uint16_t>{goap::kRangedAttack, 16}), "goal 16 with a crossbow hand does not plan [262, 16]");
			std::set<Sym> melee{Sym::IsMovementAllowed, Sym::ReadyForAction, Sym::HasWeaponEquipped, Sym::HasTwoWorkingArms};
			const std::vector<std::uint16_t> meleePlan = planOf(melee, 16);
			require(std::find(meleePlan.begin(), meleePlan.end(), goap::kMeleeAttack) != meleePlan.end(), "goal 16 with a melee hand left the melee plan");
			// Someone in melee reach: the arena stops answering HoldsCrossbow, so the shooter plans the melee swing (and 262's own
			// iAmBeingMeleeAttacked = 0 fails too).
			std::set<Sym> meleed{Sym::IsMovementAllowed, Sym::HasWeaponEquipped, Sym::HasTwoWorkingArms, Sym::IAmBeingMeleeAttacked};
			require(planOf(meleed, 16).back() == 16 && planOf(meleed, 16).size() > 1, "a meleed crossbow hand has no plan");
			meleed.insert(Sym::HoldsCrossbow);
			require(planOf(meleed, 16).empty(), "a meleed crossbow hand plans 262");
			std::set<Sym> bare{Sym::IsMovementAllowed, Sym::HasOneWorkingArm};
			require(planOf(bare, goap::kEquipCrossbow) == std::vector<std::uint16_t>{goap::kEquipCrossbow}, "264 is not a plan of its own");
			// 235 shoots from a turret: it needs the mount (OperatesTurret); 146 / 234 mount, and only where the town finder has a turret.
			require(planOf({Sym::OperatesTurret}, goap::kShootAtTarget) == std::vector<std::uint16_t>{goap::kShootAtTarget}, "235 is not a plan of its own");
			require(planOf({}, goap::kShootAtTarget).empty(), "235 plans without a mount");
			const std::set<Sym> found{Sym::HasOneWorkingArm, Sym::TurretFound};
			require(planOf(found, goap::kUseTurret) == std::vector<std::uint16_t>{goap::kUseTurret}, "146 is not a plan of its own");
			require(planOf(found, goap::kTurretPlayerJob) == std::vector<std::uint16_t>{goap::kTurretPlayerJob}, "234 is not a plan of its own");
			require(planOf({Sym::HasOneWorkingArm}, goap::kUseTurret).empty(), "146 plans with no turret found");
			// 228 stealth KO: a plan of its own once ordered (the symbol), never reached by a melee goal that did not order it.
			const std::set<Sym> ordered{Sym::HasOneWorkingArm, Sym::IsMovementAllowed, Sym::StealthKoOrdered};
			require(planOf(ordered, goap::kStealthKnockout) == std::vector<std::uint16_t>{goap::kStealthKnockout}, "228 is not a plan of its own");
			require(planOf({Sym::HasOneWorkingArm, Sym::IsMovementAllowed}, goap::kStealthKnockout).empty(), "228 plans with no order");
		}
		// ---- 2. an NPC with a crossbow at range shoots as the ranged task and never closes in ----
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const kdata::Record* bandit = nullptr;
		const kdata::Record* crossbow = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Dust Bandit")
				bandit = r;
		}
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CROSSBOW")))
		{
			if (r->name == "Eagle's Cross")
				crossbow = r;
		}
		test::require(bandit != nullptr && crossbow != nullptr, "no Dust Bandit / Eagle's Cross in the data");
		std::uint64_t rng = 3;
		for (const bool armed : {true, false})
		{
			CombatArena arena(data, CombatSettings{}, 21);
			state::Fighter shooter = data->makeFighter(gd, bandit->stringID, "", rng);
			state::Fighter target = data->makeFighter(gd, bandit->stringID, "", rng);
			shooter.team = 0;
			target.team = 1;
			shooter.x = 0.0;
			shooter.z = 0.0;
			target.x = 0.0;
			target.z = 400.0;
			shooter.skills.crossbows = 100.0;
			if (armed)
				shooter.ranged = data->makeRanged(*crossbow, 0.5);
			const std::int32_t bolts = shooter.ranged.ammo;
			arena.add(1, std::move(shooter));
			arena.add(2, std::move(target));
			perception::notice(*arena.find(1), 2);
			perception::notice(*arena.find(2), 1);
			std::uint16_t task = 0;
			double nearest = 1.0e9;
			for (int i = 0; i < 400; ++i) // 20 s
			{
				arena.step(0.05);
				if (arena.find(1)->rangedTask != 0)
					task = arena.find(1)->rangedTask;
				nearest = std::min(nearest, std::hypot(arena.find(1)->x - arena.find(2)->x, arena.find(1)->z - arena.find(2)->z));
			}
			if (armed)
			{
				require(task == goap::kRangedAttack, "the armed NPC did not run the ranged attack task (262)");
				require(arena.find(1)->ranged.ammo < bolts, "the armed NPC fired no bolt");
				require(nearest > 100.0, "the armed NPC closed to melee instead of shooting");
				require(arena.find(2)->dead || arena.find(2)->unconscious || arena.find(2)->blood < 100.0, "the shots did the target no harm");
			} else {
				require(task == 0, "an unarmed-at-range NPC ran a ranged task");
			}
		}
		std::printf("ksim_ranged_goal: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
