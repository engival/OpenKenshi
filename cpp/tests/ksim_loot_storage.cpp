// 184 PUT_LOOT_IN_STORAGE (task ds/loot-storage; kinspect dt24_184): a world-squad member that carries excess loot in a town walks to
// the nearest storage of its side that accepts it and puts it in through the container transfer path.
//  1. outside a town the goal is not taken (the exe's resolver returns 0 with no town);
//  2. in a town the loot goes into the box, counts and stolen marks intact, and the food the member carries stays;
//  3. a locked nearer box, a nearer box of another side and a stand that refuses the item type take nothing: the loot goes to the
//     farther box that accepts it, and with no such box the member keeps it.
// Bare worlds (the dt27 rig).

#include "dt27_rig.hpp"

#include "ksim/combat_data.hpp"
#include "ksim/inventory_grid.hpp"
#include "ksim/routines.hpp"

namespace {

using namespace ksim;
using namespace dt27;

const char* const kFaction = "loot-storage-test-faction";
const char* const kOwner = "loot-storage-victim-faction";
constexpr std::uint64_t kBoxBase = 777200;

std::string firstItem(Rig& rig, std::int32_t function, int skip = 0)
{
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("ITEM")))
		if (const ItemInfo* info = rig.data->item(record->stringID); info != nullptr && info->function == function && skip-- == 0)
			return record->stringID;
	return {};
}

// A BUILDING whose `itemtype limit` is 3: a stand that takes armour only.
std::string armourStand(Rig& rig)
{
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("BUILDING")))
		if (record->intOr("itemtype limit", 0) == 3)
			return record->stringID;
	return {};
}

void joinFaction(Rig& rig)
{
	state::Allegiance side;
	side.factionSid = kFaction;
	for (const std::uint64_t id : {rig.leader, rig.member})
		rig.entityOf(id).set<state::Allegiance>(side);
}

flecs::entity makeBox(Rig& rig, std::uint64_t id, double dx, const std::string& owner, bool locked, const std::string& recordSid = {})
{
	state::Container container;
	container.ownerSid = owner;
	container.recordSid = recordSid;
	container.locked = locked;
	container.x = container.useX = kX + dx;
	container.z = container.useZ = kZ;
	container.y = container.useY = rig.fighterOf(rig.member).y;
	state::Inventory stock;
	stock.version = 1;
	stock.gridW = 8;
	stock.gridH = 10;
	flecs::entity box = ecsOf(rig.sim).entity();
	box.set<state::Id>(state::Id{id});
	box.set<state::Container>(std::move(container));
	box.set<state::Inventory>(std::move(stock));
	return box;
}

// The member's pack: `count` of `loot` marked stolen from kOwner, and one food.
void stockPack(Rig& rig, const std::string& loot, std::uint32_t count, const std::string& food)
{
	state::Inventory pack;
	if (const state::Inventory* packed = rig.entityOf(rig.member).get<state::Inventory>())
		pack = *packed;
	const grid::Rules rules(*rig.gd);
	state::Inventory::Entry stolen;
	stolen.itemSid = loot;
	stolen.count = count;
	stolen.stolen = kOwner;
	test::require(grid::autoplace(pack, rules, stolen) == 0, "the loot did not fit the pack");
	state::Inventory::Entry bread;
	bread.itemSid = food;
	bread.count = 1;
	test::require(grid::autoplace(pack, rules, bread) == 0, "the food did not fit the pack");
	++pack.version;
	rig.entityOf(rig.member).set<state::Inventory>(std::move(pack));
}

std::uint32_t carriedBy(Rig& rig, const std::string& item)
{
	const state::Inventory* pack = rig.entityOf(rig.member).get<state::Inventory>();
	return pack != nullptr ? pack->carried(item) : 0;
}

// Whether every unit of `item` in the box carries the stolen mark.
bool allMarked(const flecs::entity& box, const std::string& item)
{
	std::uint32_t units = 0;
	for (const state::Inventory::Entry& entry : box.get<state::Inventory>()->items)
		if (entry.itemSid == item && entry.slot == state::Inventory::kCarried)
		{
			if (entry.stolen != kOwner)
				return false;
			units += entry.count;
		}
	return units > 0;
}

void run(Rig& rig, const flecs::entity& box, const std::string& loot, int ticks)
{
	for (int i = 0; i < ticks && box.get<state::Inventory>()->carried(loot) == 0; ++i)
		rig.tick(1);
}

void outsideATown()
{
	Rig rig({{routines::task::PutLootInStorage}}, 0);
	joinFaction(rig);
	const std::string loot = firstItem(rig, 0);
	const std::string food = firstItem(rig, kItemFunctionFood);
	test::require(!loot.empty() && !food.empty(), "no loot or food ITEM in the data");
	const flecs::entity box = makeBox(rig, kBoxBase, 6.0, kFaction, false);
	stockPack(rig, loot, 3, food);
	rig.tick(200);
	test::require(rig.goalOf(rig.member) != routines::task::PutLootInStorage, "184: taken outside a town");
	test::require(box.get<state::Inventory>()->carried(loot) == 0, "184: loot went into a box outside a town");
	std::cout << "  no town: the goal is not taken\n";
}

void putsItAway()
{
	Rig rig({{routines::task::PutLootInStorage}}, 0);
	joinFaction(rig);
	makeTown(rig, 2, kX, kZ);
	const std::string loot = firstItem(rig, 0);
	const std::string food = firstItem(rig, kItemFunctionFood);
	const flecs::entity box = makeBox(rig, kBoxBase, 6.0, kFaction, false);
	stockPack(rig, loot, 3, food);
	state::Routine seen;
	for (int i = 0; i < 400 && box.get<state::Inventory>()->carried(loot) == 0; ++i)
	{
		rig.tick(1);
		if (const state::Routine* r = rig.entityOf(rig.member).get<state::Routine>(); r != nullptr && r->goal == routines::task::PutLootInStorage)
			seen = *r;
	}
	test::require(seen.goal == routines::task::PutLootInStorage, "184: the member did not take the goal in a town");
	test::require(seen.chainTarget == kBoxBase, "184: the plan's hand is not the box");
	test::require(box.get<state::Inventory>()->carried(loot) == 3, "184: the box does not hold all three");
	test::require(carriedBy(rig, loot) == 0, "184: the member kept some loot");
	test::require(allMarked(box, loot), "184: the stolen marks did not survive the put");
	test::require(carriedBy(rig, food) == 1 && box.get<state::Inventory>()->carried(food) == 0, "184: the food was stashed");
	test::require(rig.logged("store 184 member"), "184: no store line was logged");
	rig.tick(200);
	test::require(rig.goalOf(rig.member) != routines::task::PutLootInStorage, "184: still on the goal with nothing left to put away");
	std::cout << "  in a town: 3 stolen units in the box, the food kept\n";
}

void refusals()
{
	Rig rig({{routines::task::PutLootInStorage}}, 0);
	joinFaction(rig);
	makeTown(rig, 2, kX, kZ);
	const std::string loot = firstItem(rig, 0);
	const std::string food = firstItem(rig, kItemFunctionFood);
	const std::string stand = armourStand(rig);
	test::require(!stand.empty(), "no armour stand BUILDING in the data");
	const flecs::entity locked = makeBox(rig, kBoxBase + 1, 2.0, kFaction, true);
	const flecs::entity foreign = makeBox(rig, kBoxBase + 2, 3.0, kOwner, false);
	const flecs::entity refuser = makeBox(rig, kBoxBase + 3, 4.0, kFaction, false, stand);
	stockPack(rig, loot, 2, food);
	// Only boxes that refuse: the member keeps the loot and none of them gets any.
	rig.tick(300);
	for (const flecs::entity* box : {&locked, &foreign, &refuser})
		test::require(box->get<state::Inventory>()->carried(loot) == 0, "184: a refusing box took the loot");
	test::require(carriedBy(rig, loot) == 2, "184: the member lost its loot to nothing");
	// The farther box that accepts it gets it.
	const flecs::entity open = makeBox(rig, kBoxBase + 4, 9.0, kFaction, false);
	run(rig, open, loot, 400);
	test::require(open.get<state::Inventory>()->carried(loot) == 2, "184: the accepting box did not get the loot");
	for (const flecs::entity* box : {&locked, &foreign, &refuser})
		test::require(box->get<state::Inventory>()->carried(loot) == 0, "184: a refusing box took the loot");
	std::cout << "  locked, foreign and refusing boxes skipped; the open one took 2\n";
}

} // namespace

int main()
{
	try
	{
		outsideATown();
		putsItAway();
		refusals();
		std::cout << "ksim_loot_storage: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_loot_storage: FAILED: " << e.what() << "\n";
		return 1;
	}
}
