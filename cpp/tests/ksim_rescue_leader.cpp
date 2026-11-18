// 142 FIND_AND_RESCUE_LEADER and 148 FIND_AND_RESCUE_IF_THERES_BEDS (task siblings; kinspect dt24_142.md); 105 FIND_AND_RESCUE runs 148's finder and executor.
// 142: while the platoon's leader is down (pre isLeaderDown) a member lifts it (the plan's 68, as 254's) and carries it away. 148: the nearest
// downed ally is lifted the same way. Where the casualty is taken is in no finding (UNSURE): ours is the squad's home town, where it is put
// down; when a bed is in reach the carried casualty becomes 106 and ends in a bed. A leader on its feet is never rescued. A bare world: the squad and the town are fixtures, the live selector and executors do the rest.

#include "dt27_rig.hpp"

#include "ksim/ai/routines.hpp"


namespace {

using namespace ksim;
using namespace dt27;


// A home town with the squad just inside its circle (a bare world's mesh gives no walk a route, so the carry-home walk is not exercised: the
// hand-over is); the squad's members within reach of each other.
Town setUpHome(Rig& rig)
{
	const Town home = makeTown(rig, 2, kX + 4000.0, kZ);
	rig.squad.get_mut<state::SquadInstance>()->homeTown.id = home.entity.id();
	const double x = home.circle.x - home.circle.radius + 50.0;
	for (const std::uint64_t id : {rig.leader, rig.member})
	{
		state::Fighter& f = rig.fighterOf(id);
		f.x = x + (id == rig.member ? 3.0 : 0.0);
		f.z = home.circle.z;
		f.y = rig.sim.groundHeight(f.x, f.z).value_or(f.y);
	}
	return home;
}

void knockOut(Rig& rig, std::uint64_t id)
{
	state::Fighter& f = rig.fighterOf(id);
	f.unconscious = true;
	f.koTimer = 1.0e6; // down for the whole case
}

// The member (Squad list, task) rescues the casualty: lifted, carried to the home town, put down there.
void rescues(std::uint16_t task, bool casualtyIsLeader)
{
	Rig rig({{task, 0, 24, 0, 0}}, 0);
	test::require(task == routines::task::FindAndRescue || dataHasRow(*rig.gd, task), "no AI_PACKAGE carries the rescue goal"); // no package names 105 (it is 148's twin, a plan or order goal)
	const Town home = setUpHome(rig);
	// The first conscious member without a routine is the squad's follow leader and stays out of the selector (OURS, world_tasks.cpp), and
	// a downed leader leaves that to rig.member: the rescuer is a third member.
	std::uint64_t rescuer = rig.spawn(rig.body, rig.fighterOf(rig.member).x + 3.0, rig.fighterOf(rig.member).z);
	rig.join(rescuer, 0);
	std::uint64_t casualty = rig.leader;
	if (!casualtyIsLeader)
	{
		casualty = rig.spawn(rig.body, rig.fighterOf(rig.member).x + 6.0, rig.fighterOf(rig.member).z);
		rig.join(casualty, 0);
		// The leader stays on its feet, so 148's pick is this ally and not the leader.
	}
	knockOut(rig, casualty);
	// Within the lift reach of the casualty (a bare world's mesh gives the approach walk no route, as in ksim_fog_eat).
	for (const std::uint64_t id : {rescuer, rig.member})
	{
		rig.fighterOf(id).x = rig.fighterOf(casualty).x + (id == rescuer ? 3.0 : -3.0);
		rig.fighterOf(id).z = rig.fighterOf(casualty).z;
	}
	// Either free member may take the goal (the first in id order does); the one that did is the rescuer.
	rig.until(10.0, [&] { return rig.goalOf(rig.member) == task || rig.goalOf(rescuer) == task; });
	if (rig.goalOf(rig.member) == task)
		rescuer = rig.member;
	test::require(rig.until(10.0, [&] { return rig.goalOf(rescuer) == task; }), "the member did not take the rescue goal for a downed casualty [" + [&] { std::string a; for (const std::string& l : rig.log) a += l + " | "; return a + " goal " + std::to_string(rig.goalOf(rescuer)); }() + "]");
	test::require(rig.until(15.0, [&] { return rig.fighterOf(rescuer).carrying == casualty; }), "the casualty was never lifted [" + fmt(std::hypot(rig.fighterOf(rescuer).x - rig.fighterOf(casualty).x, rig.fighterOf(rescuer).z - rig.fighterOf(casualty).z)) + " u, walking " + std::to_string(rig.fighterOf(rescuer).walking()) + ", log " + [&] { std::string a; for (const std::string& l : rig.log) a += l + " | "; return a; }() + "]");
	test::require(rig.logged("rescue " + std::to_string(task) + " member"), "no rescue line was logged");
	test::require(rig.until(120.0, [&] { return rig.logged(" down"); }), "the casualty was never put down [dist to town " + fmt(std::hypot(rig.fighterOf(rescuer).x - home.circle.x, rig.fighterOf(rescuer).z - home.circle.z)) + " r " + fmt(home.circle.radius) + " walking " + std::to_string(rig.fighterOf(rescuer).walking()) + " goal " + std::to_string(rig.goalOf(rescuer)) + " carrying " + std::to_string(rig.fighterOf(rescuer).carrying) + " log " + [&] { std::string a; for (const std::string& l : rig.log) a += l + " | "; return a; }() + "]");
	test::require(rig.fighterOf(rescuer).carrying == 0, "the rescuer still carries the casualty after the put-down [carrying " + std::to_string(rig.fighterOf(rescuer).carrying) + " carriedBy " + std::to_string(rig.fighterOf(casualty).carriedBy) + " goal " + std::to_string(rig.goalOf(rescuer)) + " interact " + std::to_string(rig.entityOf(rescuer).has<state::InteractTask>()) + "]");
	const state::Fighter& body = rig.fighterOf(casualty);
	test::require(std::hypot(body.x - home.circle.x, body.z - home.circle.z) <= home.circle.radius + 10.0,
		"the casualty was put down away from the home town");
	test::require(rig.until(3.0, [&] { return rig.goalOf(rescuer) != task; }), "the rescuer kept the goal after the hand-over");
	std::cout << "  " << task << ": the downed " << (casualtyIsLeader ? "leader" : "ally") << " was lifted and carried to the home town\n";
}

// The rescuer lifts the casualty beside a building with beds: the lift becomes 106 FIND_BED_AND_PUT_IN at a bed of that building.
void takenToABed(std::uint16_t task)
{
	Rig rig({{task, 0, 24, 0, 0}, {routines::task::FindBedAndPutIn, 0, 24, 0, 0}}, 0);
	setUpHome(rig);
	const state::Fighter& at = rig.fighterOf(rig.member);
	// The bed building nearest the squad, within the 2,000 u the resolver's candidates reach.
	const BuildingSite* house = findLayoutBuilding(rig, routines::SiteKind::Bed, true, at.x, at.z);
	test::require(house != nullptr && std::hypot(house->cx - at.x, house->cz - at.z) < 1800.0, "no building with a bed within reach of the squad");
	std::uint64_t rescuer = rig.spawn(rig.body, at.x + 3.0, at.z);
	rig.join(rescuer, 0);
	std::uint64_t casualty = rig.leader;
	if (task == routines::task::FindAndRescueIfBeds)
	{
		casualty = rig.spawn(rig.body, at.x + 6.0, at.z);
		rig.join(casualty, 0);
	}
	knockOut(rig, casualty);
	for (const std::uint64_t id : {rescuer, rig.member})
	{
		rig.fighterOf(id).x = rig.fighterOf(casualty).x + (id == rescuer ? 3.0 : -3.0);
		rig.fighterOf(id).z = rig.fighterOf(casualty).z;
	}
	for (const std::uint64_t id : {rescuer, rig.member, rig.leader, casualty})
		rig.fighterOf(id).homeBuilding = house->key;
	const auto logText = [&] { std::string a; for (const std::string& l : rig.log) a += l + " | "; return a; };
	test::require(rig.until(30.0, [&] { return rig.logged("takes " + std::to_string(casualty) + " to bed"); }), "the rescue never handed the casualty to 106 [" + logText() + "]");
	const std::uint64_t carrier = rig.fighterOf(rig.member).carrying == casualty ? rig.member : rescuer;
	test::require(rig.until(5.0, [&] { return rig.goalOf(carrier) == routines::task::FindBedAndPutIn; }), "the carrier is not running 106 [" + logText() + "]");
	// The put-in itself (the walk to the bed and the laying down) is ksim_bed_choice's: a bare world gives the walk no route and the test
	// has no accessor for a bed's position. Here the hand-off: 106's Routine, aimed at a bed of the squad's building, on the carried casualty.
	const state::Routine* routine = rig.entityOf(carrier).get<state::Routine>();
	test::require(routine != nullptr && routine->target.kind == static_cast<std::uint8_t>(routines::SiteKind::Bed) && routine->target.object == house->key
		&& routine->chainTarget == casualty && rig.fighterOf(carrier).carrying == casualty, "106 is not aimed at a bed of the home with the casualty in the arms [" + logText() + "]");
	std::cout << "  " << task << ": the lifted casualty became 106 aimed at a bed of the home\n";
}

// 142 needs isLeaderDown: a leader on its feet is not rescued, and neither is a downed member that is not the leader.
void leaderOnItsFeet()
{
	Rig rig({{routines::task::FindAndRescueLeader, 0, 24, 0, 0}}, 0);
	setUpHome(rig);
	const std::uint64_t rescuer = rig.spawn(rig.body, rig.fighterOf(rig.member).x + 3.0, rig.fighterOf(rig.member).z);
	rig.join(rescuer, 0);
	rig.tick(240);
	test::require(rig.goalOf(rescuer) != routines::task::FindAndRescueLeader, "142: taken with the leader on its feet");
	const std::uint64_t other = rig.spawn(rig.body, rig.fighterOf(rig.member).x + 6.0, rig.fighterOf(rig.member).z);
	rig.join(other, 0);
	knockOut(rig, other);
	rig.tick(240);
	test::require(rig.goalOf(rescuer) != routines::task::FindAndRescueLeader, "142: a downed non-leader was rescued as the leader");
	std::cout << "  142: not taken for a leader on its feet or a downed non-leader\n";
}

} // namespace

int main()
{
	try
	{
		rescues(routines::task::FindAndRescueLeader, true);
		rescues(routines::task::FindAndRescueIfBeds, false);
		rescues(routines::task::FindAndRescue, false); // 105 FIND_AND_RESCUE (port-talk): 148's finder, the same executor
		leaderOnItsFeet();
		takenToABed(routines::task::FindAndRescueIfBeds);
		std::cout << "ksim_rescue_leader: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_rescue_leader: FAILED: " << e.what() << "\n";
		return 1;
	}
}
