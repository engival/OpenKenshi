// The machine-work goals (task ds/machines; kinspect dt24_113, dt24_plantasks section 4): 113 EMPTY_MACHINE_OUTPUTS and 152
// OPERATE_AUTOMATIC_MACHINERY share the machine picker and run 92's pull; 93 FILL_MACHINE, 124 OPERATE_STORAGE, 88 DELIVER_RESOURCES,
// 114 GET_RID_OF_RESOURCES_IN_MY_INVENTORY and 242 DITCH_ALL_RESOURCES put the resources the member carries into one of my boxes.
// Bare worlds: a package squad with the row takes the goal and runs its plan.

#include "dt27_rig.hpp"

#include "ksim/combat/combat_data.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/ai/routines.hpp"

#include <algorithm>

namespace {

using namespace ksim;
using namespace dt27;

constexpr std::uint64_t kBoxId = 777101;
constexpr std::uint64_t kMachineId = 777102;
const char* const kFaction = "machines-test-faction";

std::string planText(const state::Routine& routine)
{
	std::string out;
	for (const std::uint16_t task : routine.plan)
		out += (out.empty() ? "" : " ") + std::to_string(task);
	return out;
}

// A plain resource: an ITEM record with no function (not food, not a kit).
std::string resourceItem(Rig& rig)
{
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("ITEM")))
		if (const ItemInfo* info = rig.data->item(record->stringID); info != nullptr && info->function == 0)
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

flecs::entity makeBox(Rig& rig, const std::string& item, std::uint32_t count)
{
	state::Container container;
	container.ownerSid = kFaction;
	container.x = container.useX = kX + 6.0;
	container.z = container.useZ = kZ;
	container.y = container.useY = rig.fighterOf(rig.member).y;
	state::Inventory stock;
	stock.version = 1;
	stock.gridW = 8;
	stock.gridH = 10;
	if (count > 0)
		stock.add(item, count);
	flecs::entity box = ecsOf(rig.sim).entity();
	box.set<state::Id>(state::Id{kBoxId});
	box.set<state::Container>(std::move(container));
	box.set<state::Inventory>(std::move(stock));
	return box;
}

flecs::entity makeMachine(Rig& rig, const std::string& item, std::uint8_t maxOperators, double dx, std::uint64_t id)
{
	state::Producer producer;
	producer.recordSid = "machines-test-drill";
	producer.ownerSid = kFaction;
	producer.output = item;
	producer.cap = 20;
	producer.maxOperators = maxOperators;
	producer.x = producer.useX = kX + dx;
	producer.z = producer.useZ = kZ;
	producer.y = producer.useY = rig.fighterOf(rig.member).y;
	state::Inventory stock;
	stock.stackLimit = producer.cap;
	stock.version = 1;
	const grid::Size slot = grid::Rules(*rig.gd).footprint(item);
	stock.gridW = static_cast<std::uint8_t>(slot.w);
	stock.gridH = static_cast<std::uint8_t>(slot.h);
	stock.add(item, 6);
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

void pullsMachine(std::uint16_t task, const char* name)
{
	Rig rig({{task}}, 0);
	joinFaction(rig);
	const std::string item = resourceItem(rig);
	test::require(!item.empty(), "no plain resource ITEM in the data");
	// 152 works the automatic machinery only: a machine that takes an operator is left alone, the drill is emptied.
	const flecs::entity manned = makeMachine(rig, item, 1, 4.0, kMachineId + 1);
	const flecs::entity drill = makeMachine(rig, item, 0, 8.0, kMachineId);
	state::Routine seen;
	bool took = false;
	for (int i = 0; i < 400 && !took; ++i)
	{
		rig.tick(1);
		if (const state::Routine* r = rig.entityOf(rig.member).get<state::Routine>(); r != nullptr && r->goal == task)
			seen = *r;
		took = carriedBy(rig, item) > 0;
	}
	test::require(seen.goal == task, std::string(name) + ": the member did not take the goal for a machine with stock");
	test::require(seen.plan == std::vector<std::uint16_t>{routines::task::EmptyMachine}, std::string(name) + ": the plan is not [92] (" + planText(seen) + ")");
	test::require(took, std::string(name) + ": nothing was taken");
	if (task == routines::task::OperateAutomaticMachinery)
	{
		test::require(seen.chainTarget == kMachineId, "152: the picked machine is not the automatic one");
		test::require(manned.get<state::Inventory>()->carried(item) == 6, "152: the manned machine was emptied");
	}
	test::require(drill.get<state::Inventory>()->carried(item) < 6 || manned.get<state::Inventory>()->carried(item) < 6, "the machine kept its stock");
	test::require(rig.logged("empty " + std::to_string(task) + " member"), std::string(name) + ": no pull line was logged");
	std::cout << "  " << name << ": took " << carriedBy(rig, item) << " from the machine (plan " << planText(seen) << ")\n";
}

void storesResources(std::uint16_t task, const char* name)
{
	Rig rig({{task}}, 0);
	joinFaction(rig);
	const std::string item = resourceItem(rig);
	test::require(!item.empty(), "no plain resource ITEM in the data");
	// Nothing carried: no goal.
	const flecs::entity box = makeBox(rig, item, 0);
	rig.tick(160);
	test::require(rig.goalOf(rig.member) != task, std::string(name) + ": taken with nothing to put away");
	state::Inventory pack;
	if (const state::Inventory* packed = rig.entityOf(rig.member).get<state::Inventory>())
		pack = *packed;
	const grid::Size slot = grid::Rules(*rig.gd).footprint(item);
	(void)slot;
	state::Inventory::Entry entry;
	entry.itemSid = item;
	entry.count = 3;
	test::require(grid::autoplace(pack, grid::Rules(*rig.gd), entry) == 0, "the resource did not fit the pack");
	++pack.version;
	rig.entityOf(rig.member).set<state::Inventory>(std::move(pack));
	state::Routine seen;
	bool stored = false;
	for (int i = 0; i < 400 && !stored; ++i)
	{
		rig.tick(1);
		if (const state::Routine* r = rig.entityOf(rig.member).get<state::Routine>(); r != nullptr && r->goal == task)
			seen = *r;
		stored = box.get<state::Inventory>()->carried(item) > 0;
	}
	test::require(seen.goal == task, std::string(name) + ": the member did not take the goal while carrying a resource");
	test::require(seen.plan == std::vector<std::uint16_t>{task}, std::string(name) + ": the plan is wrong (" + planText(seen) + ")");
	test::require(seen.chainTarget == kBoxId, std::string(name) + ": the plan's hand is not the box");
	test::require(stored, std::string(name) + ": nothing was put into the box");
	test::require(carriedBy(rig, item) + box.get<state::Inventory>()->carried(item) == 3, std::string(name) + ": items were lost");
	test::require(rig.logged("store " + std::to_string(task) + " member"), std::string(name) + ": no store line was logged");
	std::cout << "  " << name << ": put " << box.get<state::Inventory>()->carried(item) << " into the box (plan " << planText(seen) << ")\n";
}

} // namespace

int main()
{
	try
	{
		pullsMachine(routines::task::EmptyMachineOutputs, "113");
		pullsMachine(routines::task::OperateAutomaticMachinery, "152");
		storesResources(routines::task::FillMachine, "93");
		storesResources(routines::task::OperateStorage, "124");
		storesResources(routines::task::DeliverResources, "88");
		storesResources(routines::task::GetRidOfResources, "114");
		storesResources(routines::task::DitchAllResources, "242");
		std::cout << "ksim_machines: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_machines: FAILED: " << e.what() << "\n";
		return 1;
	}
}
