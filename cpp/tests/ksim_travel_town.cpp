// 130 TRAVEL_TO_TARGET_TOWN_FAST and 53 TRAVEL_TO_TARGET_TOWN (task dt27; kinspect dt27_eat_hunt_slavetrade.md section 5). Both rows are
// Task_TravelToTargetTown: pre hasDestinationTown, effect atDestinationTown. The destination is the package's target town - ours the
// squad's home town, as 36 PATROL_TOWN's - and a NEST (TownType 0) target is diverted to the nearest other town. A member outside the
// town takes the goal and aims its walk at the town; inside it, the goal is not offered and a running one ends. A bare world: the
// live selector does the rest.

#include "dt27_rig.hpp"

#include "ksim/ai/routines.hpp"
#include "ksim/world/towncircle.hpp"

#include <sstream>

namespace {

using namespace ksim;
using namespace dt27;


void place(Rig& rig, double x, double z)
{
	for (const std::uint64_t id : {rig.leader, rig.member})
	{
		state::Fighter& f = rig.fighterOf(id);
		f.x = x + (id == rig.member ? 3.0 : 0.0);
		f.z = z;
	}
}

void travels(std::uint16_t task, const char* name)
{
	Rig rig({{task}}, 2); // the Leader list, as the data's travel packages carry it
	test::require(dataHasRow(*rig.gd, task), std::string(name) + ": no AI_PACKAGE carries the row");
	const Town homeTown = makeTown(rig, 2, kX + 8000.0, kZ);
	const Town* home = &homeTown;
	test::require(home->circle.radius > 0.0, "the fixture town has no radius");
	rig.squad.get_mut<state::SquadInstance>()->homeTown.id = home->entity.id();

	place(rig, home->circle.x + home->circle.radius + 600.0, home->circle.z);
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == task; }),
		std::string(name) + ": the member outside the package town did not take the travel goal");
	test::require(rig.logged("at " + fmt(home->circle.x)), std::string(name) + ": the walk is not aimed at the town's centre");

	// Arrived: the running task ends and the goal is not offered again.
	place(rig, home->circle.x, home->circle.z);
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) != task; }), std::string(name) + ": the travel goal outlived the arrival");
	rig.tick(160);
	test::require(rig.goalOf(rig.member) != task, std::string(name) + ": the goal was offered inside the destination town");
	std::cout << "  " << name << ": taken outside the package town, ended on arrival\n";
}

void nestDiverts()
{
	Rig rig({{routines::task::TravelToTargetTownFast}}, 2);
	const Town nest = makeTown(rig, 0, kX + 20000.0, kZ);
	const Town near = makeTown(rig, 2, kX + 30000.0, kZ);
	const Town far = makeTown(rig, 2, kX + 60000.0, kZ);
	test::require(nest.circle.radius > 0.0 && near.circle.radius > 0.0, "a fixture town has no radius");
	rig.squad.get_mut<state::SquadInstance>()->homeTown.id = nest.entity.id();
	place(rig, nest.circle.x + nest.circle.radius + 600.0, nest.circle.z);
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == routines::task::TravelToTargetTownFast; }),
		"130: the member of a nest-aimed package did not take the travel goal");
	test::require(rig.logged("at " + fmt(near.circle.x)), "130: a nest target was not diverted to the nearest real town");
	test::require(!rig.logged("at " + fmt(far.circle.x)) && !rig.logged("at " + fmt(nest.circle.x)),
		"130: the walk was aimed at the nest or a farther town");
	std::cout << "  130: a nest target walks to the nearest real town instead\n";
}

} // namespace

int main()
{
	try
	{
		travels(routines::task::TravelToTargetTownFast, "130");
		travels(routines::task::TravelToTargetTown, "53");
		nestDiverts();
		std::cout << "ksim_travel_town: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_travel_town: FAILED: " << e.what() << "\n";
		return 1;
	}
}
