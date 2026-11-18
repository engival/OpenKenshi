// G4 of the native AI runtime, phase D (task CB): the GOAP planner (kinspect dt18_goap_search.md). Pure: a stage world of
// symbols, the real task registry, and the slaver chain's expected plans.

#include "ksim/goap.hpp"
#include "ksim/routines.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace ksim;

// A staged world: the symbols that hold, everything else false. `x, z` is the actor's position; the plan position is the body's.
class Stage final : public goap::World
{
public:
	explicit Stage(std::set<goap::Sym> held) : held_(std::move(held)) {}
	bool holds(goap::Sym sym, std::uint64_t, double x, double z) const override
	{
		if (sym == goap::Sym::AtLocation)
			return std::hypot(x - atX_, z - atZ_) <= 4.0;
		return held_.count(sym) != 0;
	}
	void at(double x, double z)
	{
		atX_ = x;
		atZ_ = z;
	}

private:
	std::set<goap::Sym> held_;
	double atX_ = 500.0;
	double atZ_ = 500.0;
};

std::vector<std::uint16_t> tasks(const goap::Plan& plan)
{
	std::vector<std::uint16_t> out;
	for (const goap::Step& step : plan.steps)
		out.push_back(step.task);
	return out;
}

std::string show(const std::vector<std::uint16_t>& list)
{
	std::string out;
	for (std::uint16_t task : list)
	{
		if (!out.empty())
			out += " -> ";
		out += std::to_string(task);
	}
	return out;
}

void requireTasks(const goap::Plan& plan, const std::vector<std::uint16_t>& expected, const std::string& what)
{
	const std::vector<std::uint16_t> got = tasks(plan);
	if (got != expected)
		throw std::runtime_error(what + ": expected " + show(expected) + ", got " + show(got));
}

// A body that is down, wears a weapon, and the member stands away from it: the full chain. 167's goal state is isChained only
// (s58): the capture ends at the cuff, the lift is the caging goal's.
void testChain()
{
	Stage world({goap::Sym::IsMovementAllowed, goap::Sym::HasOneWorkingArm, goap::Sym::IsTargetDown});
	const goap::Plan plan = goap::plan(world, routines::task::CaptureSlaves, 42, 0.0, 0.0);
	test::require(plan.found(), "167: a plan");
	requireTasks(plan, {goap::kRunning, goap::kStripWeapons, goap::kChainTarget, routines::task::CaptureSlaves},
		"167: the chain strip -> chain -> the goal (no lift)");
	test::require(std::fabs(plan.score - 1.0) < 1.0e-9, "167: the weakest link's score is 1.0");

	// The caging goal with the person in arms: the walk to the cage, the put-in, the goal.
	Stage carrying({goap::Sym::IsMovementAllowed, goap::Sym::HasOneWorkingArm, goap::Sym::IsTargetDown, goap::Sym::TargetIsUnarmed,
		goap::Sym::IsChained, goap::Sym::IsCarryingTargetHealthy});
	const goap::Plan cage = goap::plan(carrying, routines::task::FindCageAndPutIn, 42, 0.0, 0.0);
	test::require(cage.found(), "112: a plan");
	requireTasks(cage, {goap::kRunning, goap::kPutInCage, routines::task::FindCageAndPutIn}, "112: walk -> put in");

	// The caging goal without the load: 112 plans the lift in front of itself - walk to the body, lift, walk to the cage, put in.
	Stage empty({goap::Sym::IsMovementAllowed, goap::Sym::HasOneWorkingArm, goap::Sym::IsChained});
	const goap::Plan lift = goap::plan(empty, routines::task::FindCageAndPutIn, 42, 0.0, 0.0);
	test::require(lift.found(), "112 without a load: a plan");
	requireTasks(lift, {goap::kRunning, routines::task::CarryWoundedSlaves, goap::kRunning, goap::kPutInCage,
		routines::task::FindCageAndPutIn}, "112 without a load: walk -> lift -> walk -> put in");
}

// Task CH (dt24_223): the intruder eviction. 223's goal state IS_CARRYING_TARGET is provided by 68 LIFT_PERSON - not by 168, whose
// registry goal is IS_CARRYING_TARGET_HEALTHY (the slaver lift; the two symbols are split for exactly this reason). 224's outside
// move plans once the body is in the arms.
void testIntruderChain()
{
	Stage away({goap::Sym::IsMovementAllowed, goap::Sym::HasOneWorkingArm, goap::Sym::IsTargetDown});
	const goap::Plan plan = goap::plan(away, routines::task::PickupIntruders, 42, 0.0, 0.0);
	test::require(plan.found(), "223: a plan");
	requireTasks(plan, {goap::kRunning, goap::kLiftPerson, routines::task::PickupIntruders}, "223: walk -> lift -> the goal node");

	// At the body: no 67.
	Stage at({goap::Sym::IsMovementAllowed, goap::Sym::HasOneWorkingArm, goap::Sym::IsTargetDown});
	at.at(1.0, 1.0);
	requireTasks(goap::plan(at, routines::task::PickupIntruders, 42, 0.0, 0.0),
		{goap::kLiftPerson, routines::task::PickupIntruders}, "223 at the body: no run");

	// 224 carrying: the walk out, then the take-out step (its tick is the moveTo).
	Stage carrying({goap::Sym::IsMovementAllowed, goap::Sym::IsCarryingSomething});
	requireTasks(goap::plan(carrying, routines::task::TakeIntruderOutside, 42, 0.0, 0.0),
		{goap::kRunning, routines::task::TakeIntruderOutside}, "224: walk out -> the take-out");

	// 224 without the body in the arms: no plan (IS_CARRYING_SOMETHING is live and no task provides it).
	Stage empty({goap::Sym::IsMovementAllowed});
	requireTasks(goap::plan(empty, routines::task::TakeIntruderOutside, 42, 0.0, 0.0), {}, "224 not carrying: no plan");
}

// Task crime-rows (kinspect ai_enum_map 204 / 209 / 210 / 211 / 212): the goal-state rows. 209 / 210 (Task_InvestigateAlarms) need atATown
// and plan to themselves; the other three are Task_Blank rows with no preconditions.
void testCrimeGoalRows()
{
	Stage inTown({goap::Sym::AtATown, goap::Sym::AlarmsInTheVicinity});
	requireTasks(goap::plan(inTown, routines::task::InvestigateAlarms, 0, 0.0, 0.0), {routines::task::InvestigateAlarms}, "209 in a town: the goal node");
	requireTasks(goap::plan(inTown, routines::task::InvestigateAlarmsAlliesOnly, 0, 0.0, 0.0), {routines::task::InvestigateAlarmsAlliesOnly},
		"210 in a town: the goal node");
	Stage outside({goap::Sym::AlarmsInTheVicinity});
	requireTasks(goap::plan(outside, routines::task::InvestigateAlarms, 0, 0.0, 0.0), {}, "209 outside a town: no plan (atATown)");
	Stage bare({goap::Sym::IsMovementAllowed});
	for (const std::uint16_t task : {routines::task::HuntBounties, routines::task::PoliceFreePrisoners, routines::task::LootStolenGoods})
		requireTasks(goap::plan(bare, task, 42, 0.0, 0.0), {task}, "a Task_Blank goal row: the goal node alone");
}

// Task CH group 3 (dt24_273 section 1): 273 PICKUP_INTRUDERS_TOWN is 223's registry row under the town's scope - the same goal state
// IS_CARRYING_TARGET from 68 LIFT_PERSON - so the plan is the same walk -> lift -> goal. 274's goal state INTRUDER_IS_OUTSIDE_GATES is
// a stage symbol: unheld it plans the run, held it is the take-out step alone.
void testIntruderTownChain()
{
	Stage away({goap::Sym::IsMovementAllowed, goap::Sym::HasOneWorkingArm, goap::Sym::IsTargetDown});
	const goap::Plan plan = goap::plan(away, routines::task::PickupIntrudersTown, 42, 0.0, 0.0);
	test::require(plan.found(), "273: a plan");
	requireTasks(plan, {goap::kRunning, goap::kLiftPerson, routines::task::PickupIntrudersTown}, "273: walk -> lift -> the goal node");

	Stage carrying({goap::Sym::IsMovementAllowed, goap::Sym::IsCarryingSomething});
	requireTasks(goap::plan(carrying, routines::task::TakeIntruderOutsideTown, 42, 0.0, 0.0),
		{goap::kRunning, routines::task::TakeIntruderOutsideTown}, "274: run to the gate point -> the take-out");

	// Without the body in the arms: no plan (IS_CARRYING_SOMETHING is live and no task provides it).
	Stage empty({goap::Sym::IsMovementAllowed});
	requireTasks(goap::plan(empty, routines::task::TakeIntruderOutsideTown, 42, 0.0, 0.0), {}, "274 not carrying: no plan");

	// Already outside the gates, at the point (the evaluator's state): the take-out step alone.
	Stage out({goap::Sym::IsMovementAllowed, goap::Sym::IsCarryingSomething, goap::Sym::IntruderIsOutsideGates});
	out.at(1.0, 1.0);
	requireTasks(goap::plan(out, routines::task::TakeIntruderOutsideTown, 42, 1.0, 1.0), {routines::task::TakeIntruderOutsideTown},
		"274 outside the gates: the step alone");
}

// Task CH group 3 (dt24_118 section 1, dt24_shop section 1): 118 SHOPPING is the registry's blank goal node - goal state `?local_2e60`
// (the day's list is satisfied) and precondition atATown. 119 BUY_SHIT is the Task_Shopping action: it provides that goal state from
// isAtLocation (the counter), so the plan is walk -> 119 -> 118. In town with nothing left to buy the goal is already true.
void testShoppingChain()
{
	Stage inTown({goap::Sym::IsMovementAllowed, goap::Sym::HasOneWorkingArm, goap::Sym::AtATown});
	const goap::Plan plan = goap::plan(inTown, routines::task::Shopping, 42, 0.0, 0.0);
	test::require(plan.found(), "118: a plan");
	requireTasks(plan, {goap::kRunning, routines::task::BuyStuff, routines::task::Shopping}, "118: walk to the counter -> 119 -> the goal");

	// At the counter (the plan position within the stage's reach): no walk.
	inTown.at(1.0, 1.0);
	requireTasks(goap::plan(inTown, routines::task::Shopping, 42, 1.0, 1.0),
		{routines::task::BuyStuff, routines::task::Shopping}, "118 at the counter: 119 -> the goal, no walk");

	// Not in a town: atATown has no provider, so no plan (the goal scores 0 this update).
	Stage outside({goap::Sym::IsMovementAllowed, goap::Sym::HasOneWorkingArm});
	requireTasks(goap::plan(outside, routines::task::Shopping, 42, 0.0, 0.0), {}, "118 outside a town: no plan");
}

// The machine and storage pull chains (dt24_emptymachine_dismantle; the registry rows of dt24_113 / dt24_115 / dt24_131): the
// Task_Blank goal node's state is what the pull task writes, so a plan walks (67) and pulls (92 / 247 / 284), then the goal.
void testPullChains()
{
	Stage away({goap::Sym::GotInventoryRoomForResources, goap::Sym::IsMovementAllowed});
	requireTasks(goap::plan(away, goap::kWantToFillMachine, 7, 0.0, 0.0),
		{goap::kRunning, routines::task::EmptyMachine, goap::kWantToFillMachine}, "94: walk -> 92 -> the goal");
	requireTasks(goap::plan(away, goap::kFindSomeBuildMats, 7, 0.0, 0.0),
		{goap::kRunning, routines::task::BuildMats, goap::kFindSomeBuildMats}, "115: walk -> 247 -> the goal");
	requireTasks(goap::plan(away, goap::kLootFoodAndStuff, 7, 0.0, 0.0),
		{goap::kRunning, routines::task::LootContainer, goap::kLootFoodAndStuff}, "131: walk -> 284 -> the goal");

	// At the building: no walk (AtLocation holds live).
	Stage at({goap::Sym::GotInventoryRoomForResources, goap::Sym::IsMovementAllowed});
	at.at(1.0, 1.0);
	requireTasks(goap::plan(at, goap::kWantToFillMachine, 7, 0.0, 0.0), {routines::task::EmptyMachine, goap::kWantToFillMachine},
		"94 at the machine: no run");

	// A full inventory: gotInventoryRoomForResources does not hold and no task provides it, so the goal scores 0.
	Stage full({});
	test::require(!goap::plan(full, goap::kLootFoodAndStuff, 7, 0.0, 0.0).found(), "no room: no pull, not a terminal state");
}

// The worker at a machine (dt24_emptymachine_source section 3): 87 OPERATE_MACHINERY needs isMachineOutputFull = 0 and
// isMachineInputInvalid = 0, and 92 COLLECT_OUTPUT_RESOURCE is the action that supplies them. So an NPC whose goal state is
// IS_WORKING (214 / 189) plans the collector in front of the operate step - the planner inserts it, nothing scripts it.
void testMachineWorkChains()
{
	// A full machine, the worker at it: the pull, then the operate step, then the goal.
	Stage fullMachine({goap::Sym::GotInventoryRoomForResources, goap::Sym::IsMachineOutputFull});
	fullMachine.at(1.0, 1.0);
	requireTasks(goap::plan(fullMachine, goap::kAutoLabouringMines, 7, 0.0, 0.0),
		{routines::task::EmptyMachine, goap::kOperateMachinery, goap::kAutoLabouringMines}, "189 full machine: 92 -> 87 -> the goal");
	// 214's registry row also needs atATown (dt24_214): the same machine in a town plans it, outside one there is no plan.
	test::require(!goap::plan(fullMachine, goap::kRelaxInTownPackage, 7, 0.0, 0.0).found(), "214 outside a town: no plan");
	Stage fullMachineInTown({goap::Sym::GotInventoryRoomForResources, goap::Sym::IsMachineOutputFull, goap::Sym::AtATown});
	fullMachineInTown.at(1.0, 1.0);
	requireTasks(goap::plan(fullMachineInTown, goap::kRelaxInTownPackage, 7, 0.0, 0.0),
		{routines::task::EmptyMachine, goap::kOperateMachinery, goap::kRelaxInTownPackage},
		"214 full machine: 92 -> 87 -> the goal");

	// Away from the machine the task loop walks first (67).
	Stage away({goap::Sym::GotInventoryRoomForResources, goap::Sym::IsMachineOutputFull, goap::Sym::IsMovementAllowed});
	requireTasks(goap::plan(away, goap::kAutoLabouringMines, 7, 0.0, 0.0),
		{goap::kRunning, routines::task::EmptyMachine, goap::kOperateMachinery, goap::kAutoLabouringMines},
		"189 away: walk -> 92 -> 87 -> the goal");

	// Not full: the operate step alone fits.
	Stage running({goap::Sym::GotInventoryRoomForResources});
	running.at(1.0, 1.0);
	requireTasks(goap::plan(running, goap::kAutoLabouringMines, 7, 0.0, 0.0),
		{goap::kOperateMachinery, goap::kAutoLabouringMines}, "189 not full: 87 -> the goal");

	// A full carrier cannot pull, so a full machine has no plan: the NPC works nothing rather than stalls on a take-all.
	Stage noRoom({goap::Sym::IsMachineOutputFull});
	noRoom.at(1.0, 1.0);
	test::require(!goap::plan(noRoom, goap::kAutoLabouringMines, 7, 0.0, 0.0).found(), "a full carrier at a full machine: no plan");
}

// A precondition already true in the live world drops its subtree (dt18 section 4: the evaluator is a live call).
void testTruePreconditionsDrop()
{
	// The body already stands at the member: no 67.
	Stage near({goap::Sym::IsMovementAllowed, goap::Sym::HasOneWorkingArm, goap::Sym::IsTargetDown});
	near.at(1.0, 1.0);
	requireTasks(goap::plan(near, routines::task::CaptureSlaves, 42, 0.0, 0.0),
		{goap::kStripWeapons, goap::kChainTarget, routines::task::CaptureSlaves}, "at the body: no run");

	// Unarmed as well: no 181 either.
	Stage unarmed({goap::Sym::IsMovementAllowed, goap::Sym::HasOneWorkingArm, goap::Sym::IsTargetDown, goap::Sym::TargetIsUnarmed});
	unarmed.at(1.0, 1.0);
	requireTasks(goap::plan(unarmed, routines::task::CaptureSlaves, 42, 0.0, 0.0),
		{goap::kChainTarget, routines::task::CaptureSlaves}, "unarmed: no strip");

	// Chained: the goal has nothing left to plan but itself.
	Stage done({goap::Sym::IsMovementAllowed, goap::Sym::HasOneWorkingArm, goap::Sym::IsTargetDown, goap::Sym::TargetIsUnarmed,
		goap::Sym::IsChained});
	done.at(1.0, 1.0);
	requireTasks(goap::plan(done, routines::task::CaptureSlaves, 42, 0.0, 0.0), {routines::task::CaptureSlaves},
		"chained: the goal alone");
}

// No provider for a needed symbol: score 0, and the next update (a changed world) plans it (dt18 section 6).
void testNoPlan()
{
	// The body cannot be chained: its race has no working arm? No - the ACTOR's arm is missing: hasOneWorkingArm false and no task
	// provides it.
	Stage noArm({goap::Sym::IsMovementAllowed, goap::Sym::IsTargetDown});
	const goap::Plan none = goap::plan(noArm, routines::task::CaptureSlaves, 42, 0.0, 0.0);
	test::require(!none.found(), "no working arm: no chain, not a terminal state");
	test::require(none.steps.empty() && none.score == 0.0, "no plan: empty, score 0");

	// A dead body: 112 has no plan (isCarriedDudeDead must be false and nothing provides it).
	Stage dead({goap::Sym::IsCarryingTargetHealthy, goap::Sym::CarriedDudeIsDead});
	const goap::Plan deadPlan = goap::plan(dead, routines::task::FindCageAndPutIn, 42, 0.0, 0.0);
	test::require(!deadPlan.found(), "a dead carried body: the caging goal scores 0");

	// Every cage full: same, and the next update frees one and the plan is found again.
	Stage full({goap::Sym::IsCarryingTargetHealthy, goap::Sym::CagesAreAllFull});
	test::require(!goap::plan(full, routines::task::FindCageAndPutIn, 42, 0.0, 0.0).found(), "cages full: score 0");
	Stage free({goap::Sym::IsCarryingTargetHealthy, goap::Sym::IsMovementAllowed});
	const goap::Plan again = goap::plan(free, routines::task::FindCageAndPutIn, 42, 0.0, 0.0);
	test::require(again.found(), "a cage freed: the next update plans it");
	requireTasks(again, {goap::kRunning, goap::kPutInCage, routines::task::FindCageAndPutIn}, "re-planned caging chain");
}

// The cycle guard (dt18 section 5): two tasks that only each other can satisfy are not a plan, and the search ends.
void testCycle()
{
	std::vector<goap::TaskRow> table;
	{
		goap::TaskRow a;
		a.task = 166;
		a.name = "A";
		a.pre[0] = {goap::Sym::TargetIsUnarmed, true};
		a.preCount = 1;
		a.eff[0] = {goap::Sym::IsChained, true};
		a.effCount = 1;
		table.push_back(a);
	}
	{
		goap::TaskRow b;
		b.task = 181;
		b.name = "B";
		b.pre[0] = {goap::Sym::IsChained, true};
		b.preCount = 1;
		b.eff[0] = {goap::Sym::TargetIsUnarmed, true};
		b.effCount = 1;
		table.push_back(b);
	}
	{
		goap::TaskRow goal;
		goal.task = routines::task::CaptureSlaves;
		goal.name = "goal";
		goal.goalState = {goap::Sym::IsChained, true};
		goal.hasGoalState = true;
		table.push_back(goal);
	}
	Stage world({});
	const goap::Plan plan = goap::planWith(world, routines::task::CaptureSlaves, 42, 0.0, 0.0, table);
	test::require(!plan.found(), "a cycle gives no plan");
}

} // namespace

int main()
{
	try
	{
		testChain();
		testIntruderChain();
		testIntruderTownChain();
		testCrimeGoalRows();
		testShoppingChain();
		testPullChains();
		testMachineWorkChains();
		testTruePreconditionsDrop();
		testNoPlan();
		testCycle();
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_goap_plan OK\n";
	return 0;
}
