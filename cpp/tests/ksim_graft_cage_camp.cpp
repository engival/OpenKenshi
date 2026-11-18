// Task k16.6.140 (k16.6.116 unsure a): the Obedient Slave graft (rwh.99 section 3) through a cage-camp staging. The captor squad's package lists 172
// SLAVE_GET_IN_MY_MASTERS_CAGE, 183 SLAVE_WATCHING and 184 PUT_LOOT_IN_STORAGE on its Slave list (no shipped package lists 183 or 184 there: the rows
// are the test's own, so the executors are reached through the graft's selector pass); a slave held by a member of that squad (state 1, the 187 row)
// takes each goal and its executor runs:
//  1. 172: a free cage of the captor's faction in the captor's town; the slave walks in and puts itself in;
//  2. 183: another slave still to be processed in sight; the slave walks to it, processes it (state 1, the watcher its hand);
//  3. 184: loot stolen from another faction in the slave's pack, a storage of its side in the town; the loot goes into the box.
// Bare worlds (the dt27 rig).

#include "dt27_rig.hpp"

#include "ksim/combat_data.hpp"
#include "ksim/inventory_grid.hpp"
#include "ksim/routines.hpp"

namespace {

using namespace ksim;
using namespace dt27;

constexpr int kSlaveList = 3;
const char* const kSide = "graft-cage-camp-faction";
const char* const kOwner = "graft-cage-camp-victim";
constexpr std::uint64_t kBoxId = 777300;

struct Camp
{
	std::uint64_t slave = 0;
	std::string factionSid;
	flecs::entity_t owner = 0; // the faction's entity
};

// The captor's faction: the player faction of the rig. The member (the captor hand) holds a slave that carries the faction as its captor.
Camp stage(Rig& rig, double dx, double dz)
{
	Camp camp;
	ecsOf(rig.sim).each([&](flecs::entity entity, const state::FactionState& faction)
	{
		if (faction.templateSid == recordSid(*rig.gd, "FACTION", "Nameless"))
		{
			camp.owner = entity.id();
			camp.factionSid = faction.templateSid;
		}
	});
	test::require(camp.owner != 0, "no player faction");
	camp.slave = rig.spawn(rig.body, kX + dx, kZ + dz);
	// A captive keeps its own squad (the player's, the one it was taken from): the executors run only for a character of a squad.
	const flecs::entity own = rig.sim.spawnMover(kX + dx, kZ + dz);
	rig.entityOf(camp.slave).add<state::MemberOf>(own);
	rig.fighterOf(camp.slave).squad = own.get<state::Id>()->value;
	rig.fighterOf(camp.slave).captorSid = camp.factionSid;
	rig.sim.setSlaveState(camp.slave, 1, rig.member);
	rig.tick(2);
	test::require(rig.fighterOf(camp.slave).obedient && rig.fighterOf(camp.slave).slaveState == 1, "the slave holds its 187 row");
	return camp;
}

void joinSide(Rig& rig, std::uint64_t id)
{
	state::Allegiance side;
	side.factionSid = kSide;
	rig.entityOf(id).set<state::Allegiance>(side);
}

void cageGoal172()
{
	Rig rig({{routines::task::SlaveGetInCage, 0, 24, 0, kSlaveList}}, 0);
	test::require(dataHasRow(*rig.gd, routines::task::SlaveGetInCage), "no AI_PACKAGE carries 172");
	const BuildingSite* cage = findCageBuilding(rig);
	test::require(cage != nullptr, "no building with a cage layout exists");
	const Town town = makeTown(rig, 2, cage->cx, cage->cz);
	for (const std::uint64_t id : {rig.leader, rig.member})
	{
		state::Fighter& f = rig.fighterOf(id);
		f.x = cage->cx + (id == rig.member ? 2.0 : -8.0);
		f.z = cage->cz + 1.5;
		f.y = rig.sim.groundHeight(f.x, f.z).value_or(f.y);
	}
	const Camp camp = stage(rig, 0.0, 0.0);
	state::TownState owned = *town.entity.get<state::TownState>();
	owned.owner.id = camp.owner; // the cage's faction is the town owner's: the captor's
	town.entity.set<state::TownState>(owned);
	state::Fighter& slave = rig.fighterOf(camp.slave);
	slave.x = cage->cx + 4.0;
	slave.z = cage->cz + 1.5;
	slave.y = rig.sim.groundHeight(slave.x, slave.z).value_or(slave.y);
	test::require(rig.until(10.0, [&] { return rig.goalOf(camp.slave) == routines::task::SlaveGetInCage; }), "172: the grafted slave never took the goal");
	// The bare world has no navmesh, so the walk (67) cannot be run here: the slave stands at the cage's spot when the goal is taken and the executor
	// puts it in (107). The walk to a cage is ksim_slave_goals' (the Heft world).
	const state::Routine* routine = rig.entityOf(camp.slave).get<state::Routine>();
	test::require(routine != nullptr && routine->target.kind == static_cast<std::uint8_t>(routines::SiteKind::Cage), "172: the routine holds a cage site");
	const routines::SiteEntry* spot = rig.sim.routineSite(routine->target);
	test::require(spot != nullptr, "172: the cage site is unknown");
	slave.x = spot->x;
	slave.z = spot->z;
	slave.y = rig.sim.groundHeight(slave.x, slave.z).value_or(slave.y);
	const bool caged = rig.until(20.0, [&] { return rig.fighterOf(camp.slave).cageBuilding == cage->key; });
	test::require(caged, "172: the slave never put itself in the cage");
	test::require(rig.logged("cage 172 member " + std::to_string(camp.slave)), "172: no cage line for the slave");
	test::require(rig.fighterOf(camp.slave).slaveState == 1, "172: the caged slave is still a slave");
	std::cout << "  172: the grafted slave walked into the captor's free cage\n";
}

void watching183()
{
	Rig rig({{routines::task::SlaveWatching, 0, 24, 0, kSlaveList}}, 0);
	test::require(dataHasRow(*rig.gd, routines::task::SlaveWatching), "no AI_PACKAGE carries 183");
	const Camp camp = stage(rig, 3.0, 0.0);
	const std::uint64_t other = rig.spawn(rig.body, kX + 3.0, kZ + 3.0); // within reach: the bare world has no navmesh to walk on
	rig.sim.setSlaveState(other, 2); // an escaped slave with no captor: still to be processed
	rig.fighterOf(other).unconscious = true; // it stays where it lies (an escaping slave would run)
	test::require(rig.until(10.0, [&] { return rig.goalOf(camp.slave) == routines::task::SlaveWatching; }), "183: the grafted slave never took the goal");
	const bool done = rig.until(30.0, [&]
	{
		// The bare world has no navmesh to walk on and the two drift apart: the body lies next to the watcher.
		state::Fighter& body = rig.fighterOf(other);
		body.x = rig.fighterOf(camp.slave).x + 2.0;
		body.z = rig.fighterOf(camp.slave).z;
		return body.slaveState == 1;
	});
	test::require(done, "183: the watcher never processed the other slave");
	test::require(rig.sim.slaveCaptor(other) == camp.slave, "183: the processed slave's hand is the watcher");
	std::cout << "  183: the grafted slave processed another slave\n";
}

void storing184()
{
	Rig rig({{routines::task::PutLootInStorage, 0, 24, 0, kSlaveList}}, 0);
	const Camp camp = stage(rig, 3.0, 0.0);
	joinSide(rig, camp.slave);
	makeTown(rig, 2, kX, kZ);
	std::string loot, food;
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("ITEM")))
		if (const ItemInfo* info = rig.data->item(record->stringID); info != nullptr)
		{
			if (loot.empty() && info->function == 0)
				loot = record->stringID;
			if (food.empty() && info->function == kItemFunctionFood)
				food = record->stringID;
		}
	test::require(!loot.empty() && !food.empty(), "no loot or food ITEM in the data");
	state::Container container;
	container.ownerSid = kSide;
	container.x = container.useX = kX + 9.0;
	container.z = container.useZ = kZ;
	container.y = container.useY = rig.fighterOf(camp.slave).y;
	state::Inventory stock;
	stock.version = 1;
	stock.gridW = 8;
	stock.gridH = 10;
	flecs::entity box = ecsOf(rig.sim).entity();
	box.set<state::Id>(state::Id{kBoxId});
	box.set<state::Container>(std::move(container));
	box.set<state::Inventory>(std::move(stock));
	state::Inventory pack;
	if (const state::Inventory* packed = rig.entityOf(camp.slave).get<state::Inventory>())
		pack = *packed;
	const grid::Rules rules(*rig.gd);
	state::Inventory::Entry stolen;
	stolen.itemSid = loot;
	stolen.count = 3;
	stolen.stolen = kOwner;
	test::require(grid::autoplace(pack, rules, stolen) == 0, "the loot did not fit the pack");
	state::Inventory::Entry bread;
	bread.itemSid = food;
	bread.count = 1;
	test::require(grid::autoplace(pack, rules, bread) == 0, "the food did not fit the pack");
	++pack.version;
	rig.entityOf(camp.slave).set<state::Inventory>(std::move(pack));
	test::require(rig.until(30.0, [&] { return box.get<state::Inventory>()->carried(loot) == 3; }), "184: the loot never went into the box");
	test::require(rig.logged("store 184 member " + std::to_string(camp.slave)), "184: no store line for the slave");
	test::require(rig.fighterOf(camp.slave).slaveState == 1, "184: still a slave");
	std::cout << "  184: the grafted slave put its loot in the storage\n";
}

} // namespace

int main()
{
	try
	{
		cageGoal172();
		watching183();
		storing184();
		std::cout << "ksim_graft_cage_camp: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_graft_cage_camp: FAILED: " << e.what() << "\n";
		return 1;
	}
}
