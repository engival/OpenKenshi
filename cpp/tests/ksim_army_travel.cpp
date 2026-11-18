// 215 TRAVEL_TO_TARGET_PACKAGE (task army-travel; kinspect dt24_215): Task_TravelToMissionTarget_ArmyFormation, pre not crouching / lying,
// effect atPackageTarget, no finder. The walk goes to the package's target point (ours the squad's home town centre, as 53's but with no nest
// diversion) and ends on arrival. The formation slot (speed modes, the in-place stop) is not modelled. A target nobody can walk to ends the goal.
// A bare world: the live selector does the rest, and a bare world has no navmesh, so no walk can be set.

#include "dt27_rig.hpp"

#include "ksim/routines.hpp"
#include "ksim/towncircle.hpp"

#include <sstream>

namespace {

using namespace ksim;
using namespace dt27;

constexpr std::uint16_t kTask = routines::task::TravelToTargetPackage;

void place(Rig& rig, double x, double z)
{
	for (const std::uint64_t id : {rig.leader, rig.member})
	{
		state::Fighter& f = rig.fighterOf(id);
		f.x = x + (id == rig.member ? 3.0 : 0.0);
		f.z = z;
	}
}

// A squad whose package carries 215 takes it outside the package target, aims at the point, and the arrival ends it.
void travels()
{
	Rig rig({{kTask}}, 2);
	test::require(dataHasRow(*rig.gd, kTask), "no AI_PACKAGE carries the 215 row");
	const Town home = makeTown(rig, 2, kX + 8000.0, kZ);
	test::require(home.circle.radius > 0.0, "the fixture town has no radius");
	rig.squad.get_mut<state::SquadInstance>()->homeTown.id = home.entity.id();
	place(rig, home.circle.x + home.circle.radius + 600.0, home.circle.z);
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == kTask; }), "the member outside the target did not take 215");
	test::require(rig.logged("at " + fmt(home.circle.x)), "the walk is not aimed at the package target point");

	place(rig, home.circle.x, home.circle.z);
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) != kTask; }), "215 outlived the arrival");
	rig.tick(160);
	test::require(rig.goalOf(rig.member) != kTask, "215 was offered at the target");
	std::cout << "  215: taken outside the package target, ended on arrival\n";
}

// No way to walk (a bare world has no mesh): the goal is taken and then ends by itself, and the member is free - it never holds 215 for good.
void unreachable()
{
	Rig rig({{kTask}}, 2);
	const Town home = makeTown(rig, 2, kX + 8000.0, kZ);
	rig.squad.get_mut<state::SquadInstance>()->homeTown.id = home.entity.id();
	place(rig, home.circle.x + home.circle.radius + 600.0, home.circle.z);
	rig.tick(20 * 20);
	// The goal ended (no walk could be set) and the selector took it afresh: more than one pick of the same target in the log.
	int picks = 0;
	for (const std::string& line : rig.log)
		picks += line.find("at " + fmt(home.circle.x)) != std::string::npos ? 1 : 0;
	test::require(picks >= 2, "an unreachable package target left the member holding 215 for ever (" + std::to_string(picks) + " pick)");
	std::cout << "  215: an unreachable target ends the goal\n";
}

} // namespace

int main()
{
	try
	{
		travels();
		unreachable();
		std::cout << "ksim_army_travel: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_army_travel: FAILED: " << e.what() << "\n";
		return 1;
	}
}
