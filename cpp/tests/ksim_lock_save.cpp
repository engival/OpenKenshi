// The lock state survives a save (task ds/locks-save, save v87 state::LockStates): a cage opened by a brute force stays open, a half-cut pair of
// shackles keeps its remaining hardness, a half-picked pair keeps its pick timer, and a v86 save (no LockStates) loads every lock untouched.

#include "dt27_rig.hpp"

#include "ksim/routines.hpp"
#include "ksim/save.hpp"

#include <cstdio>

namespace {

using namespace ksim;
using namespace dt27;

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

std::string toolSid(Rig& rig)
{
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("ITEM")))
		if (record->intOr("item function", 0) == 9 && rig.data->item(record->stringID) != nullptr)
			return record->stringID;
	throw std::runtime_error("no ITEM of function 9");
}

std::uint64_t standAt(Rig& rig, double x, double z)
{
	const std::uint64_t id = rig.spawn(rig.body, x, z);
	state::Fighter& f = rig.fighterOf(id);
	f.y = rig.sim.groundHeight(x, z).value_or(f.y);
	return id;
}

std::uint64_t shackled(Rig& rig, double x, double z)
{
	const std::uint64_t id = standAt(rig, x, z);
	give(rig, id, sidOf(rig, "SHACKLES"), state::Inventory::kArmourBase + 9);
	rig.fighterOf(id).slaveState = 1;
	return id;
}

void roundTrip()
{
	Rig rig({}, 0);
	const BuildingSite* site = findCageBuilding(rig);
	test::require(site != nullptr, "no building with a cage layout exists");
	using LockStep = Simulation::LockStep;

	// A cage forced open by a rescuer (286).
	const std::uint64_t prisoner = standAt(rig, site->cx, site->cz);
	state::Fighter& pf = rig.fighterOf(prisoner);
	pf.cageBuilding = site->key;
	pf.cageSlot = 0;
	pf.slaveState = 1;
	rig.tick(3);
	const std::uint64_t rescuer = standAt(rig, rig.fighterOf(prisoner).x + 1.0, rig.fighterOf(prisoner).z);
	rig.fighterOf(rescuer).skills.strength = 100.0;
	test::require(rig.sim.cageDoorLocked(site->key), "the cage starts locked");
	test::require(rig.sim.stepLockTask(rescuer, routines::task::BruteForceLock, prisoner) == LockStep::Done, "286 did not open the cage");
	test::require(!rig.sim.cageDoorLocked(site->key), "the forced cage is not open");

	// A pair of shackles cut halfway (185, 20 s of 75).
	const std::uint64_t cutter = shackled(rig, 40.0, 20.0);
	rig.fighterOf(cutter).skills.strength = 100.0;
	give(rig, cutter, toolSid(rig), state::Inventory::kCarried);
	for (int i = 0; i < 50; ++i)
		test::require(rig.sim.stepLockTask(cutter, routines::task::CutShackles, 0) == LockStep::Working, "185 finished early");
	const double hardness = rig.sim.lockFind(true, cutter)->hardness;
	test::require(hardness > 0.0 && hardness < 30.0, "the half-cut hardness is not between 0 and the start");

	// A pair of shackles picked for 3 s with no skill (no roll yet).
	const std::uint64_t picker = shackled(rig, 44.0, 20.0);
	rig.fighterOf(picker).skills.lockpicking = 0.0;
	for (int i = 0; i < 6; ++i)
		rig.sim.stepLockTask(picker, routines::task::PickLockOnShackles, 0);
	const double elapsed = rig.sim.lockFind(true, picker)->elapsed;
	test::require(elapsed > 0.0 && elapsed < 5.0 && !rig.sim.lockFind(true, picker)->open, "the half-picked timer is not mid-interval");

	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_lock_save.save";
	const std::uint64_t held = rig.sim.stateHash();
	rig.sim.saveGame(path);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == held, "load gives the saved hash");
	test::require(!loaded.cageDoorLocked(site->key), "the forced cage is locked again after load");
	const Simulation::LockRecord* cut = loaded.lockFind(true, cutter);
	test::require(cut != nullptr && cut->hardness == hardness && !cut->open, "the half-cut hardness changed in the save");
	const Simulation::LockRecord* pick = loaded.lockFind(true, picker);
	test::require(pick != nullptr && pick->elapsed == elapsed && pick->attempts == 0, "the half-picked timer changed in the save");
	std::remove(path.c_str());

	// The previous version (v87, the one that introduced LockStates) is the only older one a build reads, and it carries the lock state.
	const std::string old = std::string(KSIM_BINARY_DIR) + "/ksim_lock_save_prev.save";
	writeSaveFile(old, rig.sim.world().ecs(), rig.sim.world().loadOrder(), {}, kSaveVersion - 1);
	Simulation legacy = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), old, false, nullptr, RegionConfig{});
	test::require(legacy.lockFind(true, cutter) != nullptr, "the previous version's save lost the lock state");
	std::remove(old.c_str());
	std::cout << "  lock state: open cage, half-cut and half-picked shackles survive save / load, and the previous version's save too\n";
}

} // namespace

int main()
{
	try
	{
		roundTrip();
		std::cout << "ksim_lock_save: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_lock_save: FAILED: " << e.what() << "\n";
		return 1;
	}
}
