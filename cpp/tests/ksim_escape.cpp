// 207 GET_OUT_OF_CAGE_ESCAPE and 256 GET_OUT_OF_CAGE_OPPORTUNISTIC (task ds/escape; kinspect dt24_207): one class, Task_GetOutOfCage, with the
// registry's pre isDoorLocked = 0 and effect I_AM_IMPRISONED = 0. A caged member whose cage is not locked takes the goal and leaves the cage
// (a chained slave becomes ESCAPING, slave state 2); with the warden squad's cages locked the goal is not taken, and the member is offered
// it again once they open - an escapee never ends stuck. 202 TOTAL_ESCAPE is a data-only row and is never offered. A bare world.

#include "dt27_rig.hpp"

#include "ksim/crime.hpp"
#include "ksim/goap.hpp"
#include "ksim/routines.hpp"

namespace {

using namespace ksim;
using namespace dt27;

// The warden squad of `building`: a squad instance whose home is it, with its cages locked or not.
flecs::entity wardenSquad(Rig& rig, std::uint64_t building, bool locked)
{
	flecs::entity warden = state::createInstance(ecsOf(rig.sim));
	state::SquadInstance instance;
	instance.homeBuilding = building;
	warden.set<state::SquadInstance>(std::move(instance));
	state::SquadRoutine routine;
	routine.cagesLocked = locked ? 1 : 0;
	warden.set<state::SquadRoutine>(routine);
	return warden;
}

state::SquadRoutine lockedRoutine()
{
	state::SquadRoutine routine;
	routine.cagesLocked = 1;
	return routine;
}

void cage(Rig& rig, std::uint64_t building)
{
	state::Fighter& f = rig.fighterOf(rig.member);
	f.cageBuilding = building;
	f.cageSlot = 0;
	f.slaveState = 1;
}

void registry()
{
	for (const std::uint16_t task : {routines::task::GetOutOfCageEscape, routines::task::GetOutOfCageOpportunistic})
	{
		const goap::TaskRow* row = goap::rowOf(task);
		test::require(row != nullptr && !row->hasGoalState && row->preCount == 1 && row->pre[0].sym == goap::Sym::IsDoorLocked
				&& !row->pre[0].value && row->effCount == 1 && row->eff[0].sym == goap::Sym::IAmImprisoned && !row->eff[0].value,
			"207 / 256: pre isDoorLocked = 0, effect I_AM_IMPRISONED = 0");
	}
	const goap::TaskRow* total = goap::rowOf(routines::task::TotalEscape);
	test::require(total != nullptr && total->preCount == 2 && total->effCount == 0 && !total->hasGoalState,
		"202: pre isChained = 0 and I_AM_IMPRISONED = 0, no effect");
	std::cout << "  registry ok\n";
}

void escapes(std::uint16_t task, const char* name)
{
	Rig rig({{task, 0, 24, 0, 0}}, 0);
	test::require(dataHasRow(*rig.gd, task), std::string(name) + ": no AI_PACKAGE carries the row");
	const BuildingSite* building = findCageBuilding(rig);
	test::require(building != nullptr, "no building with a cage layout exists");
	// A free member does not take it: nothing to get out of.
	rig.tick(80);
	test::require(rig.goalOf(rig.member) != task, std::string(name) + ": offered to a member in no cage");

	// No promoted warden (the camp, nest or jail away from the player: no SquadRoutine at all): a cage with a prisoner is locked by
	// default, so the prisoner stays across several selector passes.
	cage(rig, building->key);
	rig.fighterOf(rig.member).captorSid = "cage-owner-test"; // the victim of the escape crime when the cage records no owner
	rig.tick(160);
	test::require(rig.fighterOf(rig.member).cageBuilding == building->key, std::string(name) + ": left a cage nobody opened (no warden)");
	test::require(rig.goalOf(rig.member) != task, std::string(name) + ": taken with no warden squad (a cage is locked by default)");

	// A promoted warden that has locked its cages: not taken, and the member stays caged (not stuck: the goal never started).
	const flecs::entity warden = wardenSquad(rig, building->key, true);
	for (int i = 0; i < 120; ++i)
	{
		// The sim drops a SquadRoutine of a squad with nobody promoted; the warden squad of a bare world is kept locked by hand.
		warden.set<state::SquadRoutine>(lockedRoutine());
		rig.tick(1);
	}
	test::require(rig.fighterOf(rig.member).cageBuilding == building->key, std::string(name) + ": left a locked cage");
	test::require(rig.goalOf(rig.member) != task, std::string(name) + ": taken with the door locked");

	// The warden opens: the goal is offered again and the member leaves.
	test::require(rig.until(10.0, [&]
		{
			warden.set<state::SquadRoutine>(state::SquadRoutine{}); // promoted, its lock task never run (kept by hand, as above)
			return rig.fighterOf(rig.member).cageBuilding == 0;
		}),
		std::string(name) + ": did not leave the unlocked cage");
	test::require(rig.fighterOf(rig.member).slaveState == 2, std::string(name) + ": a chained slave is not ESCAPING after");
	{
		// Task_GetOutOfCage with the escape flag records ESCAPE_PRISON against the cage's owner once the occupant is out.
		const state::CrimeRecord* record = rig.entityOf(rig.member).get<state::CrimeRecord>();
		test::require(record != nullptr && record->crime == crime::kEscapingPrison && !record->victimFaction.empty(), std::string(name) + ": the escape recorded no crime");
	}
	test::require(rig.logged("escape " + std::to_string(task) + " member"), std::string(name) + ": the escape is not logged");
	// Out: the effect holds, the goal is over and the member is not stuck in it.
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) != task; }), std::string(name) + ": the goal outlived the escape");
	std::cout << "  " << name << ": locked cage refused, unlocked cage left (slave state 2)\n";
}

// A cage whose building's every door is destroyed (state 3, open for ever) is open even with no warden promoted.
void brokenDoor()
{
	Rig rig({{routines::task::GetOutOfCageEscape, 0, 24, 0, 0}}, 0);
	const BuildingSite* building = findCageBuilding(rig);
	test::require(building != nullptr, "no building with a cage layout exists");
	cage(rig, building->key);
	rig.tick(80);
	test::require(rig.fighterOf(rig.member).cageBuilding == building->key, "207: left a cage nobody opened");
	flecs::entity door = state::createInstance(ecsOf(rig.sim));
	state::Door broken;
	broken.building = building->key;
	broken.state = 3;
	door.set<state::Door>(std::move(broken));
	test::require(rig.until(10.0, [&] { return rig.fighterOf(rig.member).cageBuilding == 0; }), "207: did not leave a cage whose door is destroyed");
	std::cout << "  207: a destroyed door opens the cage\n";
}

} // namespace

int main()
{
	try
	{
		registry();
		escapes(routines::task::GetOutOfCageEscape, "207");
		escapes(routines::task::GetOutOfCageOpportunistic, "256");
		brokenDoor();
		std::cout << "ksim_escape: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_escape: FAILED: " << e.what() << "\n";
		return 1;
	}
}
