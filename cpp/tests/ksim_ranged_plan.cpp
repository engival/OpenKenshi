// Task ds/ranged step 1: the planner rows of the Task_Blank combat nodes 9 / 10 / 11, 59 GET_READY_FOR_ACTION (between the equip and the melee
// swing), 64 MAKE_TARGET_STAND_STILL, 236 WORSHIP_TARGET and the splint pair 249 / 250. Pure: a staged world of symbols and the real registry.

#include "ksim/ai/goap.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace ksim;
using goap::Sym;
using V = std::vector<std::uint16_t>;

class Stage final : public goap::World
{
public:
	explicit Stage(std::set<Sym> held) : held_(std::move(held)) {}
	bool holds(Sym sym, std::uint64_t, double x, double z) const override
	{
		if (sym == Sym::AtLocation)
			return std::hypot(x - 500.0, z - 500.0) <= 4.0;
		return held_.count(sym) != 0;
	}

private:
	std::set<Sym> held_;
};

V tasks(const goap::Plan& plan)
{
	V out;
	for (const goap::Step& step : plan.steps)
		out.push_back(step.task);
	return out;
}

V planOf(const std::set<Sym>& held, std::uint16_t task)
{
	return tasks(goap::plan(Stage(held), task, 7, 500.0, 500.0));
}

} // namespace

int main()
{
	try
	{
		for (const std::uint16_t blank : {goap::kChooseEnemyAndAttack, goap::kChooseAttackerOfAlly, goap::kAttackCharactersAttacker})
			test::require(planOf({}, blank) == V{blank}, "Task_Blank node " + std::to_string(blank) + " is not a bare plan node");

		// 59 sits between the equip and the swing: a melee step without isReadyForAction plans the get-ready step in front of it.
		const std::set<Sym> armed{Sym::IsMovementAllowed, Sym::HasWeaponEquipped, Sym::IsWithin50Meters};
		test::require(planOf(armed, goap::kGetReadyForAction) == V{goap::kGetReadyForAction}, "59 with a weapon in hand is not [59]");
		test::require(planOf(armed, goap::kMeleeAttack) == (V{goap::kGetReadyForAction, goap::kMeleeAttack}), "4 on a drawn weapon is not [59, 4]");
		std::set<Sym> ready = armed;
		ready.insert(Sym::ReadyForAction);
		test::require(planOf(ready, goap::kMeleeAttack) == V{goap::kMeleeAttack}, "4 when already ready is not [4]");
		std::set<Sym> lying = armed;
		lying.insert(Sym::IsCrouchingOrLying);
		test::require(planOf(lying, goap::kGetReadyForAction).empty(), "59 planned while crouching or lying");

		test::require(planOf({}, goap::kMakeTargetStandStill) == V{goap::kMakeTargetStandStill}, "64 is not a bare plan node");
		test::require(planOf({}, goap::kWorshipTarget) == V{goap::kWorshipTarget}, "236 is not a bare plan node");

		// 250 is the job goal (needsSplint = 0), 249 the order that clears it: the pair mirrors 58 / 25.
		test::require(planOf({Sym::NeedsSplint}, goap::kSplintJob) == (V{goap::kSplintOrder, goap::kSplintJob}), "250 on a splintable patient is not [249, 250]");
		test::require(planOf({}, goap::kSplintJob) == V{goap::kSplintJob}, "250 with nothing to rig is not [250]");
		test::require(planOf({Sym::NeedsSplint}, goap::kSplintOrder) == V{goap::kSplintOrder}, "249 is not a bare order");
		std::cout << "ksim_ranged_plan: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_ranged_plan: FAILED: " << e.what() << "\n";
		return 1;
	}
}
