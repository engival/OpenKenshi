// Task ds/small-tasks-2: the executors of 28 STAND_UP, 102 / 159 (Task_TalktoPlayer), 164 FOLLOW_WHILE_TALKING and 165 TOWN_STALKER (out of town).
// None is a package goal in the data, so the test sets the Routine itself, as an order or a plan naming the task would. A bare world: the
// package's own goal (188 with no slave of mine) never fires.

#include "dt27_rig.hpp"

#include "ksim/ai/routines.hpp"

namespace {

using namespace ksim;
using namespace dt27;

void give(Rig& rig, std::uint16_t goal, std::uint64_t target)
{
	state::Routine routine;
	routine.goal = goal;
	routine.since = rig.sim.clockTick();
	routine.chainTarget = target;
	routine.nextScore = rig.sim.clockTick() + 100000; // the selector does not re-score the member inside the test
	rig.entityOf(rig.member).set<state::Routine>(std::move(routine));
}

// 102 / 159 / 164 / 165: the order that names the hand (Simulation::orderTalkGoal, the generic path's entry).
void order(Rig& rig, std::uint16_t goal, std::uint64_t target)
{
	test::require(rig.sim.orderTalkGoal(rig.member, goal, target), "the talk order " + std::to_string(goal) + " was refused");
	test::require(rig.goalOf(rig.member) == goal, "the order did not put the goal in the Routine");
}

void standsUp()
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	rig.fighterOf(rig.member).crouched = true;
	give(rig, routines::task::StandUp, 0);
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == 0; }), "28: the task did not end");
	test::require(!rig.fighterOf(rig.member).crouched, "28: the member is still crouched");
	test::require(rig.logged("stand 28 member"), "28: no stand line was logged");
	std::cout << "  28: stood up\n";
}

void talks(std::uint16_t goal)
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	const std::uint64_t target = rig.spawn(rig.body, kX + 10.0, kZ + 4.0);
	order(rig, goal, target);
	test::require(rig.until(25.0, [&] { return rig.logged("talk " + std::to_string(goal) + " member"); }),
		std::to_string(goal) + ": the member never reached the target and spoke");
	const state::Fighter& f = rig.fighterOf(rig.member);
	const state::Fighter& t = rig.fighterOf(target);
	test::require(std::hypot(f.x - t.x, f.z - t.z) <= 20.0, std::to_string(goal) + ": it spoke from afar");
	test::require(rig.sim.dialogueEventCount(1) > 0, std::to_string(goal) + ": no dialogue event fired");
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) == 0; }), std::to_string(goal) + ": the task did not end with the line");
	std::cout << "  " << goal << ": spoke at the target\n";
}

void stalks()
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	const std::uint64_t target = rig.spawn(rig.body, kX + 10.0, kZ + 4.0);
	order(rig, routines::task::TownStalker, target);
	test::require(!rig.sim.orderTalkGoal(rig.member, routines::task::StandUp, target), "orderTalkGoal took a goal that is not a talk goal");
	test::require(rig.until(25.0, [&] { return rig.logged("talk 165 member"); }), "165: the stalker never reached the target and spoke");
	test::require(rig.sim.dialogueEventCount(0x1a) > 0, "165: line 0x1a was not fired");
	test::require(rig.goalOf(rig.member) == routines::task::TownStalker, "165: the stalker stopped at the line");
	// It keeps following until the target is gone (the dt27 rig has no navmesh, so the walk itself is the shared walkTo of 188's and is not run here).
	rig.tick(100);
	test::require(rig.goalOf(rig.member) == routines::task::TownStalker, "165: the stalker gave up while the target stood there");
	// A dead target ends it (never a held state).
	rig.fighterOf(target).dead = true;
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == 0; }), "165: the stalker kept going after the target died");
	std::cout << "  165: followed and spoke once\n";
}

} // namespace

int main()
{
	try
	{
		standsUp();
		for (const std::uint16_t goal : {routines::task::SeekAndTalk, routines::task::SendDialogue, routines::task::FollowWhileTalking})
			talks(goal);
		stalks();
		std::cout << "ksim_small_exec: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_small_exec: FAILED: " << e.what() << "\n";
		return 1;
	}
}
