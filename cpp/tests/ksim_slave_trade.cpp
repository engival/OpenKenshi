// The slave trade's town steps (task dt27 part B; kinspect dt27_eat_hunt_slavetrade.md section 6, dt27_town_slave_role_i2.md):
// 192 GO_TO_SOMEWHERE_FOR_DELIVERING_SLAVES (its pre list atDestinationTown is the goal; the walk is 53 / 130's), 217 GATHER_SLAVES_FROM_
// CAGES_IF_ITS_AN_EXPORT_TOWN (173's gather over my town's cages, the occupant must stand) and 218 / 194 GIVE_ALL_MY_SLAVES_TO(_IF_ITS_
// AN_IMPORT_TOWN) (the keeper of my town's nearest allied slave-storage building, designation 10). The "export" / "import" of the names
// is in neither the code nor the data (sections 6.4 / 6.5): the cages with a standing occupant and the slave-storage building ARE the
// qualifiers. A bare world: the live selector and executors do the rest, no orders to NPCs.

#include "dt27_rig.hpp"

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
		f.y = rig.sim.groundHeight(f.x, f.z).value_or(f.y);
	}
}

// 192: outside the package town the goal is taken and aimed at the town; inside, it ends and is not offered.
void goesToDestination()
{
	Rig rig({{routines::task::GoToSomewhereForDeliveringSlaves}}, 2);
	test::require(dataHasRow(*rig.gd, routines::task::GoToSomewhereForDeliveringSlaves), "no AI_PACKAGE carries 192");
	const Town home = makeTown(rig, 2, kX + 8000.0, kZ);
	rig.squad.get_mut<state::SquadInstance>()->homeTown.id = home.entity.id();
	place(rig, home.circle.x + home.circle.radius + 600.0, home.circle.z);
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == routines::task::GoToSomewhereForDeliveringSlaves; }),
		"192: the member outside the destination town did not take the goal");
	test::require(rig.logged("at " + fmt(home.circle.x)), "192: the walk is not aimed at the destination town");
	place(rig, home.circle.x, home.circle.z);
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) != routines::task::GoToSomewhereForDeliveringSlaves; }),
		"192: the goal outlived the arrival");
	rig.tick(160);
	test::require(rig.goalOf(rig.member) != routines::task::GoToSomewhereForDeliveringSlaves, "192: offered inside the destination town");
	std::cout << "  192: taken outside the destination town, ended on arrival\n";
}

// A cage building with a town round it and a captive in the cage; the member (Leader list) stands beside it.
struct CageScene
{
	const BuildingSite* cage = nullptr;
	std::uint64_t captive = 0;
};

CageScene setUpCage(Rig& rig)
{
	CageScene scene;
	scene.cage = findCageBuilding(rig);
	test::require(scene.cage != nullptr, "no building with a cage layout exists");
	makeTown(rig, 2, scene.cage->cx, scene.cage->cz); // my town: the circle the cage and the member stand in
	// The taker within the executor's reach of the captive (a bare world has no navigator to walk it there); the leader clear of it.
	place(rig, scene.cage->cx - 8.0, scene.cage->cz);
	rig.fighterOf(rig.member).x = scene.cage->cx + 2.0;
	rig.fighterOf(rig.member).z = scene.cage->cz + 1.5;
	scene.captive = rig.spawn(rig.body, scene.cage->cx, scene.cage->cz);
	state::Fighter& victim = rig.fighterOf(scene.captive);
	victim.y = rig.sim.groundHeight(victim.x, victim.z).value_or(victim.y);
	victim.cageBuilding = scene.cage->key;
	victim.cageSlot = 0;
	victim.slaveState = 1;
	return scene;
}

// 217: a cage of my town with a standing occupant is gathered (the occupant leaves it and follows the taker); a knocked-out occupant is not.
void gathersStandingOccupant()
{
	{
		Rig rig({{routines::task::GatherSlavesFromCagesExport}}, 2);
		test::require(dataHasRow(*rig.gd, routines::task::GatherSlavesFromCagesExport), "no AI_PACKAGE carries 217");
		const CageScene scene = setUpCage(rig);
		rig.fighterOf(scene.captive).unconscious = true;
		rig.fighterOf(scene.captive).koTimer = 1.0e6; // stays down for the whole case
		rig.tick(240);
		test::require(rig.goalOf(rig.member) != routines::task::GatherSlavesFromCagesExport, "217: a knocked-out occupant was gathered");
		test::require(rig.fighterOf(scene.captive).cageBuilding == scene.cage->key, "217: a knocked-out occupant left its cage");
	}
	Rig rig({{routines::task::GatherSlavesFromCagesExport}}, 2);
	const CageScene scene = setUpCage(rig);
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == routines::task::GatherSlavesFromCagesExport; }),
		"217: the member did not take the goal for a standing occupant of my town's cage");
	const bool left = rig.until(20.0, [&] { return rig.fighterOf(scene.captive).cageBuilding == 0; });
	test::require(left, "217: the occupant never left its cage");
	test::require(rig.fighterOf(scene.captive).followTarget == rig.member, "217: the gathered slave does not follow its taker");
	std::cout << "  217: a standing occupant of my town's cage was taken; a knocked-out one was left\n";
}

// 218 / 194: the member's slaves are handed to the keeper of the slave-storage building (designation 10) of its town.
void deliversToStorageKeeper(std::uint16_t task, const char* name)
{
	Rig rig({{task}}, 2);
	test::require(dataHasRow(*rig.gd, task), std::string(name) + ": no AI_PACKAGE carries the row");
	const BuildingSite* storage = nullptr;
	for (const BuildingSite& site : rig.sim.world().buildingSites())
		if (!site.squadTemplate.empty())
			if (const kdata::Record* squad = rig.gd->find(site.squadTemplate))
				if (squad->intOr("building designation", 0) == 10)
				{
					storage = &site;
					break;
				}
	test::require(storage != nullptr, "no building of designation 10 (slave storage) is placed");
	makeTown(rig, 2, storage->cx, storage->cz);
	place(rig, storage->cx - 20.0, storage->cz + 20.0); // the leader, clear of the scene
	rig.fighterOf(rig.member).x = storage->cx + 7.0; // the member within the executor's reach of the keeper (no navigator in a bare world)
	rig.fighterOf(rig.member).z = storage->cz + 4.0;
	const std::uint64_t keeper = rig.spawn(rig.body, storage->cx, storage->cz);
	rig.fighterOf(keeper).homeBuilding = storage->key;
	rig.fighterOf(keeper).y = rig.sim.groundHeight(storage->cx, storage->cz).value_or(rig.fighterOf(keeper).y);

	// No slaves: nothing to give, the goal is not offered.
	rig.tick(160);
	test::require(rig.goalOf(rig.member) != task, std::string(name) + ": the goal was taken with no slave to give");

	std::vector<std::uint64_t> slaves;
	for (int i = 0; i < 2; ++i)
	{
		const std::uint64_t slave = rig.spawn(rig.body, storage->cx + 12.0 + 4.0 * i, storage->cz + 14.0);
		state::Fighter& s = rig.fighterOf(slave);
		s.slaveState = 1;
		s.followTarget = rig.member;
		slaves.push_back(slave);
	}
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == task; }),
		std::string(name) + ": the member with slaves did not take the goal for its town's slave-storage keeper");
	test::require(rig.until(20.0, [&] { return rig.fighterOf(slaves[0]).followTarget == keeper && rig.fighterOf(slaves[1]).followTarget == keeper; }),
		std::string(name) + ": the slaves were not handed to the keeper");
	test::require(rig.logged("deliver 218 member"), std::string(name) + ": no delivery line was logged");
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) != task; }), std::string(name) + ": the goal outlived the delivery");
	std::cout << "  " << name << ": the slaves were given to the slave-storage keeper\n";
}

} // namespace

// 281 FIND_CAGE_AND_PUT_DEADGUY_IN (dt26 section 0, dt24_172): 112's cage finder with a dead body allowed. A kidnapper (254) carrying a man
// to a cage whose victim dies in its arms is replanned as 281 and the corpse still goes into the cage (112's row, isCarriedDudeDead = 0,
// would have let it go). The world selector never offers either to a member already carrying, so 254's chain is the way in.
void cagesTheDead()
{
	Rig rig({{routines::task::FinalKidnapperCageJob}, {routines::task::FindCageAndPutDeadGuyIn}}, 2); // the package carries the caging goals with 254, as the kidnapper template does for 112
	test::require(dataHasRow(*rig.gd, routines::task::FindCageAndPutDeadGuyIn), "no AI_PACKAGE carries 281");
	const BuildingSite* cage = findCageBuilding(rig);
	test::require(cage != nullptr, "no building with a cage layout exists");
	makeTown(rig, 2, cage->cx, cage->cz);
	place(rig, cage->cx - 8.0, cage->cz);
	rig.fighterOf(rig.member).x = cage->cx + 2.0;
	rig.fighterOf(rig.member).z = cage->cz + 1.5;
	rig.fighterOf(rig.member).homeBuilding = cage->key;
	const std::uint64_t victim = rig.spawn(recordSid(*rig.gd, "CHARACTER", "Fogman"), cage->cx + 4.0, cage->cz + 1.5); // not an ally
	rig.fighterOf(victim).unconscious = true;
	rig.fighterOf(victim).down = true;
	{
		state::Inventory empty = *rig.entityOf(victim).get<state::Inventory>();
		empty.items.clear(); // an unarmed victim: the lift needs no strip first
		empty.version = 1;
		rig.entityOf(victim).set<state::Inventory>(std::move(empty));
	}
	rig.fighterOf(victim).weapon = rig.data->unarmed;
	rig.fighterOf(victim).faction = 123456789; // the rig gives every spawn the same faction: an ally the sensor excludes
	{
		state::SightRecord record; // the observer's sight list is what the sensor reads: the scan would take longer than the case
		record.target = victim;
		record.seen = true;
		record.sinceSensed = 0.0;
		rig.fighterOf(rig.member).sight.push_back(record);
	}
	rig.fighterOf(victim).koTimer = 1.0e6;
	{
		// The package row is the goal window; the town's PATROL outranks a Fluff-tier 254 in a bare world, so the pick is made here, as
		// the selector would make it (the downed non-ally in sight, the lift plan).
		rig.fighterOf(rig.member).clearRoute(); // the town patrol the member took first is dropped with its walk
		rig.fighterOf(rig.member).forwardSpeed = 0.0;
		rig.fighterOf(rig.member).lateralSpeed = 0.0;
		state::Routine routine;
		routine.goal = routines::task::FinalKidnapperCageJob;
		routine.since = rig.sim.clockTick();
		routine.nextScore = routine.since;
		routine.chainTarget = victim;
		routine.plan = {67, 68, 132, 254};
		rig.fighterOf(rig.member).carrying = victim; // the lift (ksim_slave_goals) is done: the chain is at its cage half
		rig.fighterOf(victim).carriedBy = rig.member;
		rig.entityOf(rig.member).set<state::Routine>(std::move(routine));
	}
	test::require(rig.until(15.0, [&] { return rig.fighterOf(rig.member).carrying == victim; }), "254: the kidnapper never lifted the victim [" + [&] { std::string a; for (const std::string& l : rig.log) a += l + " | "; return a + " goal " + std::to_string(rig.goalOf(rig.member)); }() + "]");
	rig.fighterOf(victim).dead = true; // dies in the arms
	test::require(rig.until(10.0, [&] { return rig.goalOf(rig.member) == routines::task::FindCageAndPutDeadGuyIn; }),
		"281: the chain was not replanned for the corpse");
	// The put-in is 112's executor (ksim_slave_goals runs it to the cage on the Heft world); the bare rig's cage cannot be walked to, so
	// the case is that the chain keeps the corpse (112's chain drops a dead body: "capture end ... the body died").
	rig.tick(60);
	test::require(rig.fighterOf(rig.member).carrying == victim, "281: the carrier let the corpse go");
	test::require(rig.goalOf(rig.member) == routines::task::FindCageAndPutDeadGuyIn, "281: the chain ended with the corpse in the arms [" + [&] { std::string a; for (const std::string& l : rig.log) a += l + " | "; return a + " goal " + std::to_string(rig.goalOf(rig.member)); }() + "]");
	test::require(!rig.logged("capture end"), "281: the chain ended");
	std::cout << "  281: a victim that died in the arms is carried on to the cage as a corpse\n";
}

int main()
{
	try
	{
		goesToDestination();
		gathersStandingOccupant();
		deliversToStorageKeeper(routines::task::GiveAllMySlavesToImport, "218");
		deliversToStorageKeeper(routines::task::GiveAllMySlavesTo, "194");
		cagesTheDead();
		std::cout << "ksim_slave_trade: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_slave_trade: FAILED: " << e.what() << "\n";
		return 1;
	}
}
