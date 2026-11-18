// Task ds/locks-save part 2: the planner rows of the lift twins (170 / 213 / 225 / 240 / 246), 203 ARREST_TARGET, 187 _SLAVE_OBEDIENCE and 272
// SMASH_BUILDING. Pure: a staged world of symbols and the real task registry (the lift twins' executor is the 68 lift: carry.cpp isLiftTask).

#include "ksim/goap.hpp"
#include "ksim/routines.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace ksim;
using goap::Sym;

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

std::vector<std::uint16_t> tasks(const goap::Plan& plan)
{
	std::vector<std::uint16_t> out;
	for (const goap::Step& step : plan.steps)
		out.push_back(step.task);
	return out;
}

const std::set<Sym> kReady{Sym::HasOneWorkingArm, Sym::IsMovementAllowed, Sym::IsTargetDown};

void liftTwins()
{
	using V = std::vector<std::uint16_t>;
	for (const std::uint16_t lift : {goap::kLiftPersonSnatching, goap::kLiftPersonPlayerOrder, goap::kKidnapOrder})
	{
		const goap::Plan plan = goap::plan(Stage(kReady), lift, 7, 500.0, 500.0);
		test::require(plan.found() && tasks(plan) == V{lift}, "lift twin " + std::to_string(lift) + " does not plan itself beside a downed body");
	}
	// 240 is the in-town arrest: it needs atATown (the registry's town test; UNSURE which town).
	test::require(!goap::plan(Stage(kReady), goap::kLiftPersonSnatchingInTown, 7, 500.0, 500.0).found(), "240 planned outside a town");
	std::set<Sym> inTown = kReady;
	inTown.insert(Sym::AtATown);
	test::require(tasks(goap::plan(Stage(inTown), goap::kLiftPersonSnatchingInTown, 7, 500.0, 500.0)) == V{240}, "240 does not plan in a town");
	// 170 heals first: needsFirstAid = 0 is pre, and 25 is the only task that clears it.
	test::require(tasks(goap::plan(Stage(kReady), goap::kLiftObjectHealFirst, 7, 500.0, 500.0)) == V{170}, "170 with nothing to heal is not [170]");
	std::set<Sym> hurt = kReady;
	hurt.insert(Sym::NeedsFirstAid);
	test::require(tasks(goap::plan(Stage(hurt), goap::kLiftObjectHealFirst, 7, 500.0, 500.0)) == V{goap::kFirstAidOrder, 170},
		"170 on a wounded body is not [25, 170]");
	// Orders never displace 68 as the planner's provider: 203's lift step is 68 when the body is not in the arms yet.
	std::set<Sym> down = kReady;
	down.insert(Sym::TargetIsUnarmed);
	const goap::Plan arrest = goap::plan(Stage(down), routines::task::ArrestTarget, 7, 500.0, 500.0);
	test::require(arrest.found() && tasks(arrest) == V{goap::kLiftPerson, 203}, "203 beside an unarmed downed body is not [68, 203]");
	std::set<Sym> carried = down;
	carried.insert(Sym::IsCarryingTarget);
	test::require(tasks(goap::plan(Stage(carried), routines::task::ArrestTarget, 7, 500.0, 500.0)) == V{203}, "203 with the body in the arms is not [203]");
	std::cout << "  lift twins 170 / 213 / 225 / 240 / 246, 203 over the 68 lift\n";
}

void blankNodes()
{
	using V = std::vector<std::uint16_t>;
	test::require(tasks(goap::plan(Stage({}), routines::task::SlaveObedience, 0, 500.0, 500.0)) == V{187}, "187 is not a bare plan node");
	test::require(tasks(goap::plan(Stage({}), routines::task::SmashBuilding, 0, 500.0, 500.0)) == V{272}, "272 is not a bare plan node");
	std::cout << "  187 / 272 plan nodes\n";
}

} // namespace

int main()
{
	try
	{
		liftTwins();
		blankNodes();
		std::cout << "ksim_locks2: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_locks2: FAILED: " << e.what() << "\n";
		return 1;
	}
}
