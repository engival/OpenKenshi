// 176 HUNTING_BLOODSMELL (task dt27; kinspect dt27_eat_hunt_slavetrade.md section 4). Task_HuntingSmell walks the member straight at the
// platoon Blackboard's smell position (Blackboard+0xf8). Nothing in the exe's decompiled set writes it (U), so the sim's hook is
// Simulation::setSquadSmell; with no smell the goal is not offered (the exe would hold an idle task, which ours never does), and
// withdrawing the smell ends the running task so the member re-scores. A bare world: the live selector does the rest.

#include "dt27_rig.hpp"

#include "ksim/ai/routines.hpp"

#include <sstream>

namespace {

using namespace ksim;
using namespace dt27;


void huntsTheSmell()
{
	Rig rig({{routines::task::HuntingBloodsmell}}, 2); // the Leader list, as the Blood Hunter packages carry it; the member is not the follow leader
	test::require(dataHasRow(*rig.gd, routines::task::HuntingBloodsmell), "no AI_PACKAGE carries 176");
	rig.tick(160); // 8 s with no smell: the row has nothing to walk at
	test::require(rig.goalOf(rig.member) != routines::task::HuntingBloodsmell, "176: the goal was taken with no smell on the Blackboard");

	test::require(rig.sim.setSquadSmell(rig.squad, kX + 300.0, kZ), "the squad took no smell");
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == routines::task::HuntingBloodsmell; }),
		"176: the member did not take HUNTING_BLOODSMELL for a smell on the platoon's Blackboard");
	test::require(rig.logged("goal HUNTING_BLOODSMELL member " + std::to_string(rig.member)), "176: the goal commit was not logged");
	test::require(rig.logged("at " + [] { std::ostringstream out; out << kX + 300.0; return out.str(); }()), "176: the walk is not aimed at the smell's position");
	std::cout << "  176: the smell was taken as a goal and aimed at\n";

	rig.sim.clearSquadSmell(rig.squad);
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) != routines::task::HuntingBloodsmell; }),
		"176: the member kept hunting a smell that is gone");
	rig.tick(80);
	test::require(rig.goalOf(rig.member) != routines::task::HuntingBloodsmell, "176: the goal came back with no smell");
	std::cout << "  176: withdrawing the smell ended the hunt\n";
}

} // namespace

int main()
{
	try
	{
		huntsTheSmell();
		std::cout << "ksim_hunting_smell: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_hunting_smell: FAILED: " << e.what() << "\n";
		return 1;
	}
}
