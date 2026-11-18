// The jam goals and the storable-loot goal (task ds/machines-2; kinspect dt24_113, dt24_emptymachine_source, dt24_cage_loot_play):
// 89 / 90 / 91 run the machine picker over the JAMMED machines (work state +0x468 = Producer::prodState 1, output full) and clear the
// jam through 92's pull; 241 waits 3 s at a body and takes every unit of the first resource type my storage holds. The jam state is
// the saved prodState, so a round trip must keep it and the state hash.

#include "dt27_rig.hpp"

#include "ksim/combat/combat_data.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/ai/routines.hpp"

#include <algorithm>
#include <sstream>

namespace {

using namespace ksim;
using namespace dt27;

constexpr std::uint64_t kJammedId = 778101;
constexpr std::uint64_t kFreeId = 778102;
constexpr std::uint64_t kBoxId = 778103;
const char* const kFaction = "machines2-test-faction";

std::string planText(const state::Routine& routine)
{
	std::string out;
	for (const std::uint16_t task : routine.plan)
		out += (out.empty() ? "" : " ") + std::to_string(task);
	return out;
}

std::vector<std::string> resourceItems(Rig& rig, std::size_t want)
{
	std::vector<std::string> items;
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("ITEM")))
		if (const ItemInfo* info = rig.data->item(record->stringID); info != nullptr && info->function == 0 && items.size() < want)
			items.push_back(record->stringID);
	test::require(items.size() == want, "the data lacks plain resource ITEMs");
	return items;
}

void joinFaction(Rig& rig)
{
	state::Allegiance side;
	side.factionSid = kFaction;
	for (const std::uint64_t id : {rig.leader, rig.member})
		rig.entityOf(id).set<state::Allegiance>(side);
}

flecs::entity makeMachine(Rig& rig, const std::string& item, std::uint32_t count, std::uint32_t cap, std::uint8_t state, double dx, std::uint64_t id)
{
	state::Producer producer;
	producer.recordSid = "machines2-test-machine";
	producer.ownerSid = kFaction;
	producer.output = item;
	producer.cap = cap;
	producer.maxOperators = 1;
	producer.prodState = state;
	producer.x = producer.useX = kX + dx;
	producer.z = producer.useZ = kZ;
	producer.y = producer.useY = rig.fighterOf(rig.member).y;
	state::Inventory stock;
	stock.stackLimit = cap;
	stock.version = 1;
	const grid::Size slot = grid::Rules(*rig.gd).footprint(item);
	stock.gridW = static_cast<std::uint8_t>(slot.w);
	stock.gridH = static_cast<std::uint8_t>(slot.h);
	stock.add(item, count);
	flecs::entity machine = ecsOf(rig.sim).entity();
	machine.set<state::Id>(state::Id{id});
	machine.set<state::Producer>(std::move(producer));
	machine.set<state::Inventory>(std::move(stock));
	return machine;
}

std::uint32_t carriedBy(Rig& rig, const std::string& item)
{
	const state::Inventory* pack = rig.entityOf(rig.member).get<state::Inventory>();
	return pack != nullptr ? pack->carried(item) : 0;
}

void unjams(std::uint16_t task, const char* name, const std::vector<std::uint16_t>& plan)
{
	{
		// No jammed machine in the world: the goal is not taken.
		Rig rig({{task}}, 0);
		joinFaction(rig);
		const std::string item = resourceItems(rig, 1)[0];
		makeMachine(rig, item, 6, 20, 2, 4.0, kFreeId);
		rig.tick(160);
		test::require(rig.goalOf(rig.member) != task, std::string(name) + ": taken with no machine jammed");
	}
	Rig rig({{task}}, 0);
	joinFaction(rig);
	const std::string item = resourceItems(rig, 1)[0];
	// The free machine is nearer: the picker must still pass it over for the jammed one (state 1, output at its cap).
	const flecs::entity free = makeMachine(rig, item, 6, 20, 2, 4.0, kFreeId);
	const flecs::entity jammed = makeMachine(rig, item, 6, 6, 1, 8.0, kJammedId);
	state::Routine seen;
	bool cleared = false;
	for (int i = 0; i < 400 && !cleared; ++i)
	{
		rig.tick(1);
		if (const state::Routine* r = rig.entityOf(rig.member).get<state::Routine>(); r != nullptr && r->goal == task)
			seen = *r;
		cleared = jammed.get<state::Inventory>()->carried(item) < 6;
	}
	test::require(seen.goal == task, std::string(name) + ": the member did not take the goal for a jammed machine");
	test::require(seen.chainTarget == kJammedId, std::string(name) + ": the picked machine is not the jammed one");
	test::require(seen.plan == plan, std::string(name) + ": the plan is wrong (" + planText(seen) + ")");
	test::require(cleared && carriedBy(rig, item) > 0, std::string(name) + ": the output was not taken");
	test::require(free.get<state::Inventory>()->carried(item) == 6, std::string(name) + ": the free machine was emptied");
	std::cout << "  " << name << ": took " << carriedBy(rig, item) << " from the jammed machine (plan " << planText(seen) << ")\n";
}

// The jam is saved: prodState 1 survives a save / reload and the state hash is the same.
void jamIsSaved()
{
	Rig rig({}, 0);
	const std::string item = resourceItems(rig, 1)[0];
	const flecs::entity jammed = makeMachine(rig, item, 6, 6, 1, 8.0, kJammedId);
	(void)jammed;
	const std::uint64_t before = rig.sim.stateHash();
	std::stringstream bytes;
	rig.sim.save(bytes);
	rig.sim.reload(bytes);
	test::require(rig.sim.stateHash() == before, "the state hash changed over a save / reload with a jammed machine");
	std::uint8_t state = 0;
	std::uint32_t stored = 0;
	ecsOf(rig.sim).each([&](flecs::entity entity, const state::Id& id, const state::Producer& producer)
	{
		if (id.value == kJammedId)
		{
			state = producer.prodState;
			stored = entity.get<state::Inventory>() != nullptr ? entity.get<state::Inventory>()->carried(producer.output) : 0;
		}
	});
	test::require(state == 1 && stored == 6, "the jammed machine did not keep its work state and stock over a save / reload");
	std::cout << "  jam: prodState 1 and its stock survive a save / reload, state hash equal\n";
}

void lootsStorable()
{
	Rig rig({{routines::task::LootStorable}}, 0);
	joinFaction(rig);
	const std::vector<std::string> items = resourceItems(rig, 2);
	const std::string& storable = items[0];
	const std::string& other = items[1];
	// My box holds `storable` only.
	state::Container container;
	container.ownerSid = kFaction;
	container.x = container.useX = kX - 30.0;
	container.z = container.useZ = kZ;
	container.y = container.useY = rig.fighterOf(rig.member).y;
	state::Inventory boxStock;
	boxStock.version = 1;
	boxStock.gridW = 8;
	boxStock.gridH = 10;
	boxStock.add(storable, 1);
	flecs::entity box = ecsOf(rig.sim).entity();
	box.set<state::Id>(state::Id{kBoxId});
	box.set<state::Container>(std::move(container));
	box.set<state::Inventory>(std::move(boxStock));
	// A corpse carrying `other` only: not storable, so no goal.
	const std::uint64_t corpse = rig.spawn(rig.body, kX + 12.0, kZ + 4.0);
	rig.fighterOf(corpse).dead = true;
	state::Inventory pack;
	if (const state::Inventory* packed = rig.entityOf(corpse).get<state::Inventory>())
		pack = *packed;
	state::Inventory::Entry entry;
	entry.itemSid = other;
	entry.count = 2;
	test::require(grid::autoplace(pack, grid::Rules(*rig.gd), entry) == 0, "the other resource did not fit the corpse's pack");
	++pack.version;
	rig.entityOf(corpse).set<state::Inventory>(pack);
	rig.tick(160);
	test::require(rig.goalOf(rig.member) != routines::task::LootStorable, "241: taken for a body with nothing my storage holds");
	// Now it also carries the storable type (and the other one stays).
	entry.itemSid = storable;
	entry.count = 4;
	test::require(grid::autoplace(pack, grid::Rules(*rig.gd), entry) == 0, "the storable resource did not fit the corpse's pack");
	++pack.version;
	rig.entityOf(corpse).set<state::Inventory>(pack);
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == routines::task::LootStorable; }),
		"241: the member did not take LOOT_RESOURCE_ITEMS_WE_HAVE_STORAGE_FOR for a body carrying a storable resource");
	const state::Routine seen = *rig.entityOf(rig.member).get<state::Routine>();
	test::require(seen.plan == std::vector<std::uint16_t>{routines::task::LootStorable} && seen.chainTarget == corpse, "241: the plan or the target is wrong");
	// A bare world has no navigator to walk the member to the body: stand it beside the corpse.
	state::Fighter& looter = rig.fighterOf(rig.member);
	looter.x = rig.fighterOf(corpse).x + 1.0;
	looter.z = rig.fighterOf(corpse).z;
	looter.y = rig.sim.groundHeight(looter.x, looter.z).value_or(looter.y);
	looter.clearRoute();
	test::require(rig.until(15.0, [&] { return rig.logged("loot 241 member"); }), "241: nothing was looted");
	test::require(carriedBy(rig, storable) == 4, "241: the whole storable stack did not move");
	test::require(carriedBy(rig, other) == 0, "241: the non-storable type was taken");
	test::require(rig.entityOf(corpse).get<state::Inventory>()->carried(other) == 2, "241: the corpse lost its other resource");
	std::cout << "  241: 4 x the storable type taken after the 3 s wait, the other type left\n";
}

} // namespace

int main()
{
	try
	{
		unjams(routines::task::UnjamMachine, "91", {routines::task::EmptyMachine});
		unjams(routines::task::UnjamAllMachines, "90", {91, routines::task::EmptyMachine});
		unjams(routines::task::KeepEverythingRunning, "89", {90, 91, routines::task::EmptyMachine});
		jamIsSaved();
		lootsStorable();
		std::cout << "ksim_machines2: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_machines2: FAILED: " << e.what() << "\n";
		return 1;
	}
}
