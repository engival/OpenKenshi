// The lock tasks (task ds/shackles-locks; kinspect dt24_shackles / dt24_picklock / item28 b2 b6 / dt24_207): one Lock model for a cage and for worn
// shackles. 76 PICK_LOCK and 285 CUT_LOCK are the planner's steps in front of 207 (a locked cage, a skilled or equipped prisoner); 286 BRUTE_FORCE_LOCK,
// 201 / 185 / 186 on shackles, 219, 110 and 133 are player orders (orderLockTask). A bare world: the cage building is the first with a cage layout.

#include "dt27_rig.hpp"

#include "ksim/door_plan.hpp"
#include "ksim/goap.hpp"
#include "ksim/routines.hpp"

namespace {

using namespace ksim;
using namespace dt27;

void caged(Rig& rig, std::uint64_t id, const BuildingSite& site, bool slave = true)
{
	state::Fighter& f = rig.fighterOf(id);
	f.cageBuilding = site.key;
	f.cageSlot = 0;
	f.slaveState = slave ? 1 : 0;
}

// A character standing on the ground at (x, z) (the peeler test's way: a spawn at the default height falls and slides).
std::uint64_t standAt(Rig& rig, double x, double z)
{
	const std::uint64_t id = rig.spawn(rig.body, x, z);
	state::Fighter& f = rig.fighterOf(id);
	f.y = rig.sim.groundHeight(x, z).value_or(f.y);
	return id;
}

// The test world has no navigation, so the actor is kept beside its target by hand (the walk is the order driver's, not under test).
void keepNear(Rig& rig, std::uint64_t who, std::uint64_t other)
{
	state::Fighter& f = rig.fighterOf(who);
	const state::Fighter& o = rig.fighterOf(other);
	f.x = o.x + 1.0;
	f.z = o.z;
	f.y = o.y;
}

// Runs the lock task step by step beside its target (stepLockTask called directly; the sim between steps is not ticked, so nothing slides away).
Simulation::LockStep runBeside(Rig& rig, std::uint64_t who, std::uint16_t task, std::uint64_t other, int maxSteps)
{
	Simulation::LockStep step = Simulation::LockStep::Working;
	for (int i = 0; i < maxSteps && step == Simulation::LockStep::Working; ++i)
	{
		keepNear(rig, who, other);
		step = rig.sim.stepLockTask(who, task, other);
	}
	return step;
}

void give(Rig& rig, std::uint64_t id, const std::string& sid, int slot)
{
	flecs::entity entity = rig.entityOf(id);
	state::Inventory pack = *entity.get<state::Inventory>();
	state::Inventory::Entry entry;
	entry.itemSid = sid;
	entry.count = 1;
	entry.slot = slot;
	pack.items.push_back(std::move(entry));
	++pack.version;
	entity.set<state::Inventory>(std::move(pack));
}

std::string sidOf(Rig& rig, const char* name)
{
	const kdata::Record* record = rig.gd->find(name);
	test::require(record != nullptr, std::string("no record ") + name);
	return record->stringID;
}

// A tool: an ITEM of function 9 (the hasTools stand-in).
std::string toolSid(Rig& rig)
{
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("ITEM")))
		if (record->intOr("item function", 0) == 9 && rig.data->item(record->stringID) != nullptr)
			return record->stringID;
	throw std::runtime_error("no ITEM of function 9");
}

void shackle(Rig& rig, std::uint64_t id)
{
	give(rig, id, sidOf(rig, "SHACKLES"), state::Inventory::kArmourBase + 9);
	rig.fighterOf(id).slaveState = 1;
}

void registry()
{
	const auto check = [](std::uint16_t task, bool orderOnly, std::uint8_t pres, goap::Sym effect, bool effValue, const char* what)
	{
		const goap::TaskRow* row = goap::rowOf(task);
		test::require(row != nullptr && row->orderOnly == orderOnly && row->preCount == pres && row->effCount == 1
				&& row->eff[0].sym == effect && row->eff[0].value == effValue,
			what);
	};
	check(76, false, 4, goap::Sym::IsDoorLocked, false, "76: arm, lock skill, at location, no weapon -> isDoorLocked = 0");
	check(285, false, 4, goap::Sym::IsDoorLocked, false, "285: arm, tools, at location, no weapon -> isDoorLocked = 0");
	check(286, true, 2, goap::Sym::IsDoorLocked, false, "286: arm, at location -> isDoorLocked = 0 (a player order)");
	check(201, false, 4, goap::Sym::ShacklesLocked, false, "201: shackles lock skill -> isWearingShackles_locked = 0");
	check(185, false, 4, goap::Sym::ShacklesLocked, false, "185: tools -> isWearingShackles_locked = 0");
	check(186, true, 2, goap::Sym::ShacklesLocked, false, "186: a player order");
	check(219, false, 1, goap::Sym::IsChained, false, "219: isWearingShackles_locked = 0 -> isChained = 0");
	check(110, false, 3, goap::Sym::IsTargetImprisoned, false, "110: conscious, no free slot, at location -> TARGET_IS_IMPRISONED = 0");
	check(133, false, 1, goap::Sym::IAmImprisoned, false, "133: isDoorLocked = 0 -> I_AM_IMPRISONED = 0");
	check(134, false, 2, goap::Sym::MachineOccupantIsAlive, false, "134: weapon, at location -> MACHINE_OCCUPANT_IS_ALIVE = 0");
	// The model's numbers (dt24_shackles sections 6-7, item28 b2).
	test::require(std::abs(cutRate(0.0) - 0.1) < 1e-9 && std::abs(cutRate(100.0) - 0.4) < 1e-9, "cut rate: 2 x lerp(0.05, 0.2) = 0.1 .. 0.4 per second");
	test::require(bruteBreaks(91.0, 30.0) && !bruteBreaks(90.0, 30.0), "brute force: strength above 3 x hardness");
	test::require(sufficientLockSkill(40.0, 60.0, true) && !sufficientLockSkill(40.0, 1.0, true) && !sufficientLockSkill(40.0, 45.0, false) == false,
		"lock skill: chance above 0.05, a caged picker at skill - 10");
	std::cout << "  registry ok\n";
}

// 76 (and 285) as the plan steps in front of 207: the locked cage is opened by the prisoner, then it leaves.
void autonomous(bool cut)
{
	Rig rig({{routines::task::GetOutOfCageEscape, 0, 24, 0, 0}}, 0);
	const BuildingSite* site = findCageBuilding(rig);
	test::require(site != nullptr, "no building with a cage layout exists");
	caged(rig, rig.member, *site);
	state::Fighter& f = rig.fighterOf(rig.member);
	if (cut)
	{
		f.skills.strength = 100.0;
		give(rig, rig.member, toolSid(rig), state::Inventory::kCarried);
	} else {
		f.skills.lockpicking = 100.0;
	}
	test::require(rig.sim.cageDoorLocked(site->key), "a cage with a prisoner and no warden is locked");
	const std::string name = cut ? "285" : "76";
	const double hardness = rig.sim.cageLockHardness(site->key);
	int ticks = 0;
	test::require(rig.until(cut ? hardness / 0.4 + 30.0 : 90.0, [&] { ++ticks; return rig.fighterOf(rig.member).cageBuilding == 0; }),
		name + ": the prisoner never got out of its locked cage");
	test::require(rig.logged("goal GET_OUT_OF_CAGE_ESCAPE") || rig.logged("goal"), name + ": no goal logged");
	test::require(rig.logged(cut ? "lock 285 member" : "lock 76 member"), name + ": the lock step did not run");
	test::require(rig.fighterOf(rig.member).slaveState == 2, name + ": a chained slave is not ESCAPING after");
	// The first pick roll comes after 5 game seconds (the lock timer); a cut takes hardness / rate seconds.
	test::require(ticks * kTickSeconds >= (cut ? hardness / 0.4 - 1.0 : 5.0), name + ": out faster than the lock allows");
	// An unskilled, unequipped prisoner has no plan: it stays (the existing escape test covers the default).
	std::cout << "  " << name << ": locked cage opened by the prisoner after " << ticks * kTickSeconds << " s\n";
}

void bruteForceLock()
{
	Rig rig({}, 0);
	const BuildingSite* site = findCageBuilding(rig);
	test::require(site != nullptr, "no building with a cage layout exists");
	const std::uint64_t prisoner = standAt(rig, site->cx, site->cz);
	caged(rig, prisoner, *site);
	rig.tick(3); // the sim puts a caged character at its cage spot
	const std::uint64_t rescuer = standAt(rig, rig.fighterOf(prisoner).x + 1.0, rig.fighterOf(prisoner).z);
	const double hardness = rig.sim.cageLockHardness(site->key);
	rig.fighterOf(rescuer).skills.strength = hardness * 3.0;
	using LockStep = Simulation::LockStep;
	test::require(runBeside(rig, rescuer, routines::task::BruteForceLock, prisoner, 4) == LockStep::Refused, "286: strength = 3 x hardness did not refuse");
	test::require(rig.sim.cageDoorLocked(site->key), "286: strength = 3 x hardness broke the lock");
	test::require(rig.logged("is not strong enough"), "286: no 'not strong enough' line");
	rig.fighterOf(rescuer).skills.strength = hardness * 3.0 + 1.0;
	test::require(runBeside(rig, rescuer, routines::task::BruteForceLock, prisoner, 4) == LockStep::Done, "286: strength above 3 x hardness did not finish");
	test::require(!rig.sim.cageDoorLocked(site->key), "286: strength above 3 x hardness did not break the lock");
	test::require(rig.sim.dialogueEventCount(0x0d) >= 1, "286: the prisoner got no EV 13 (UNLOCK_MY_CAGE_OR_SHACKLES)");
	// 133 GET_OUT_OF_CAGE_LEGIT: the open door lets the prisoner leave, without the slave state change of an escape.
	test::require(rig.sim.stepLockTask(prisoner, routines::task::GetOutOfCageLegit, 0) == LockStep::Done, "133: the open cage was not left");
	test::require(rig.fighterOf(prisoner).cageBuilding == 0, "133: the prisoner did not leave the open cage");
	test::require(rig.fighterOf(prisoner).slaveState == 1, "133: the plain leave changed the slave state");
	std::cout << "  286 / 133: brute force refused at 3 x hardness, breaks above it; the legit leave keeps slave state 1\n";
}

void lockedRefusesLegit()
{
	Rig rig({}, 0);
	const BuildingSite* site = findCageBuilding(rig);
	test::require(site != nullptr, "no building with a cage layout exists");
	const std::uint64_t prisoner = standAt(rig, site->cx, site->cz);
	caged(rig, prisoner, *site);
	test::require(rig.sim.stepLockTask(prisoner, routines::task::GetOutOfCageLegit, 0) == Simulation::LockStep::Refused, "133: not refused on a locked door");
	test::require(rig.fighterOf(prisoner).cageBuilding == site->key, "133: left a locked cage (its precondition is the open door)");
	std::cout << "  133: refused while the door is locked\n";
}

void shackleTasks()
{
	Rig rig({}, 0);
	// 201: the wearer picks its own shackles (skill 100 against level 20: chance 0.9 per roll, first roll after 5 s).
	const std::uint64_t picker = standAt(rig, 20.0, 20.0);
	shackle(rig, picker);
	rig.fighterOf(picker).skills.lockpicking = 100.0;
	test::require(rig.sim.shacklesLocked(picker), "a worn pair of shackles starts locked");
	rig.sim.orderLockTask(picker, routines::task::PickLockOnShackles, 0);
	test::require(rig.until(120.0, [&] { return !rig.sim.shacklesLocked(picker); }), "201: the shackles never came open");
	test::require(rig.sim.shacklesWorn(picker), "201: the picked shackles are gone (219 takes them off)");
	// 219: instant once unlocked; the slave becomes ESCAPING.
	rig.sim.orderLockTask(picker, routines::task::TakeOffMyShackles, 0);
	rig.tick(4);
	test::require(!rig.sim.shacklesWorn(picker), "219: the unlocked shackles are still worn");
	test::require(rig.fighterOf(picker).slaveState == 2, "219: the unchained slave is not ESCAPING");
	// 219 refuses locked shackles.
	const std::uint64_t locked = standAt(rig, 24.0, 20.0);
	shackle(rig, locked);
	rig.sim.orderLockTask(locked, routines::task::TakeOffMyShackles, 0);
	rig.tick(4);
	test::require(rig.sim.shacklesWorn(locked), "219: took off locked shackles");
	// 185: the wearer cuts them with a tool at 2 x lerp(0.05, 0.2) per second; the cut shackles are gone.
	const std::uint64_t cutter = standAt(rig, 28.0, 20.0);
	shackle(rig, cutter);
	rig.fighterOf(cutter).skills.strength = 100.0;
	rig.sim.orderLockTask(cutter, routines::task::CutShackles, 0);
	rig.tick(40);
	test::require(rig.sim.shacklesWorn(cutter), "185: cut without a tool");
	rig.logged("has no tools");
	test::require(rig.logged("has no tools"), "185: no 'has no tools' line");
	give(rig, cutter, toolSid(rig), state::Inventory::kCarried);
	rig.sim.orderLockTask(cutter, routines::task::CutShackles, 0);
	test::require(rig.until(30.0 / 0.4 + 20.0, [&] { return !rig.sim.shacklesWorn(cutter); }), "185: the shackles were never cut");
	test::require(rig.fighterOf(cutter).slaveState == 2, "185: the freed slave is not ESCAPING");
	// 186: instant when strength > 3 x hardness (30); the shackles stay on, open.
	const std::uint64_t weak = standAt(rig, 32.0, 20.0);
	shackle(rig, weak);
	rig.fighterOf(weak).skills.strength = 90.0;
	rig.sim.orderLockTask(weak, routines::task::BruteForceShackles, 0);
	rig.tick(4);
	test::require(rig.sim.shacklesLocked(weak), "186: strength 90 broke hardness 30");
	rig.fighterOf(weak).skills.strength = 91.0;
	rig.sim.orderLockTask(weak, routines::task::BruteForceShackles, 0);
	rig.tick(4);
	test::require(rig.sim.shacklesWorn(weak) && !rig.sim.shacklesLocked(weak), "186: strength 91 did not break the shackles (and leave them on)");
	std::cout << "  201 / 219 / 185 / 186: pick, take off, cut, brute force on worn shackles\n";
}

void releasePrisoner()
{
	Rig rig({}, 0);
	const BuildingSite* site = findCageBuilding(rig);
	test::require(site != nullptr, "no building with a cage layout exists");
	const std::uint64_t prisoner = standAt(rig, site->cx, site->cz);
	caged(rig, prisoner, *site);
	rig.tick(3);
	const std::uint64_t jailer = standAt(rig, rig.fighterOf(prisoner).x + 1.0, rig.fighterOf(prisoner).z);
	// rwh.100: the release clears the cage's lock itself, unconditionally (Task_ReleasePrisoner::vf1); a refused one (an unconscious prisoner) leaves it.
	rig.sim.setScenarioLocked(site->key);
	test::require(rig.sim.cageDoorLocked(site->key), "110: the cage starts locked");
	{
		const std::uint64_t sleeper = standAt(rig, site->cx, site->cz + 3.0);
		caged(rig, sleeper, *site);
		rig.fighterOf(sleeper).unconscious = true;
		test::require(runBeside(rig, jailer, routines::task::ReleasePrisoner, sleeper, 4) == Simulation::LockStep::Refused, "110: not refused for an unconscious prisoner");
		test::require(rig.sim.cageDoorLocked(site->key), "110: a refused release cleared the lock");
		rig.fighterOf(sleeper).cageBuilding = 0;
	}
	test::require(runBeside(rig, jailer, routines::task::ReleasePrisoner, prisoner, 4) == Simulation::LockStep::Done, "110: the release did not finish");
	test::require(!rig.sim.cageDoorLocked(site->key), "110: the release left the cage locked (the lock record and the scenario's mark)");
	test::require(rig.fighterOf(prisoner).cageBuilding == 0, "110: the prisoner was not released");
	test::require(rig.fighterOf(prisoner).slaveState == 1, "110: the release changed the slave state");
	test::require(rig.sim.dialogueEventCount(0x2e) >= 1, "110: no event 0x2e");
	// An unconscious prisoner is not released (TARGET_IS_CONSCIOUS).
	const std::uint64_t out = standAt(rig, site->cx, site->cz + 2.0);
	caged(rig, out, *site);
	rig.fighterOf(out).unconscious = true;
	test::require(runBeside(rig, jailer, routines::task::ReleasePrisoner, out, 4) == Simulation::LockStep::Refused, "110: not refused for an unconscious prisoner");
	test::require(rig.fighterOf(out).cageBuilding == site->key, "110: released an unconscious prisoner");
	std::cout << "  110: a conscious prisoner is freed (slave state kept, event 0x2e), an unconscious one is not\n";
}

}

int main()
{
	try
	{
		registry();
		autonomous(false);
		autonomous(true);
		bruteForceLock();
		lockedRefusesLegit();
		shackleTasks();
		releasePrisoner();
		std::cout << "ksim_locks: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_locks: FAILED: " << e.what() << "\n";
		return 1;
	}
}
