// 184 PUT_LOOT_IN_STORAGE (kinspect dt24_184): the CARRYING_EXCESS_LOOT evaluator, the resolver for the storage the loot goes to,
// and the put. One resolver (lootStorage184) serves the world squads' selector and the residents' goal pass.
//
// Kept from the exe: no town, no search; a character with a home looks only at the home's storages, one without at its town's storages
// of its own side; the nearest box that accepts at least one carried item within the reach cap. The rest is OURS, marked UNSURE: the
// per-item accept test (vf+0x4a0) is a box's lock, its `itemtype limit` and room in its grid; the item filter (140748d40) is "a carried
// stack that is not food, a kit or a tool"; the home's function 0xd / 2 slots are the containers inside the home building; the town
// role field (+0xd8 == 2, which sends a homed character to the town search too) is not modelled; the exe's distance_mode is the
// machine picker's mode 1 (kStockReach).

#include "ksim/combat/combat.hpp"
#include "ksim/economy/inventory_grid.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/towncircle.hpp"

#include "world/world_resolvers.hpp"

#include <map>

namespace ksim {

namespace {

constexpr std::int32_t kFunctionTool = 9; // ITEM `item function` TOOL (lock_tasks.cpp's)

// A carried stack the goal moves: anything but food, a medical or robot kit, or a tool (the member keeps what it uses).
bool isLoot(const ItemInfo* info)
{
	return info != nullptr && info->function != kItemFunctionMedkit && info->function != kItemFunctionFood
		&& info->function != kItemFunctionFoodRestricted && info->function != kItemFunctionRobotKit && info->function != kFunctionTool;
}

state::Inventory::Entry unit(const std::string& item)
{
	state::Inventory::Entry one;
	one.itemSid = item;
	one.count = 1;
	return one;
}

} // namespace

bool Simulation::carryingExcessLoot(std::uint64_t id) const
{
	if (combat_ == nullptr)
		return false;
	const state::Inventory* held = fighterEntity(id).get<state::Inventory>();
	if (held == nullptr)
		return false;
	for (const state::Inventory::Entry& entry : held->items)
		if (entry.slot == state::Inventory::kCarried && entry.count > 0 && isLoot(combat_->data().item(entry.itemSid)))
			return true;
	return false;
}

std::uint64_t Simulation::lootStorage184(std::uint64_t id)
{
	const state::Fighter* f = combat_ != nullptr && gameData_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr || !carryingExcessLoot(id))
		return 0;
	flecs::world& ecs = ecsMutable();
	std::vector<towncircle::Circle> circles;
	ecs.each([&](const state::TownState& town) { circles.push_back(towncircle::of(*gameData_, town)); });
	const int myTown = towncircle::at(circles, f->x, f->z);
	if (myTown < 0)
		return 0; // CharStatus+0x88 == 0: no town, nothing to search
	std::vector<std::string> loot;
	for (const state::Inventory::Entry& entry : fighterEntity(id).get<state::Inventory>()->items)
		if (entry.slot == state::Inventory::kCarried && entry.count > 0 && isLoot(combat_->data().item(entry.itemSid))
			&& std::find(loot.begin(), loot.end(), entry.itemSid) == loot.end())
			loot.push_back(entry.itemSid);
	std::string side;
	if (const state::Allegiance* allegiance = fighterEntity(id).get<state::Allegiance>())
		side = allegiance->factionSid;
	const grid::Rules rules(*gameData_);
	std::vector<worldres::StockBuilding> boxes;
	ecs.each([&](flecs::entity entity, const state::Id& boxId, const state::Container& box)
	{
		const state::Inventory* stock = entity.get<state::Inventory>();
		if (box.locked || stock == nullptr)
			return;
		if (const auto blocked = worldTaskBlocks_.find({id, boxId.value}); blocked != worldTaskBlocks_.end() && blocked->second > clockTick())
			return; // the member's own block: this box refused it a moment ago
		if (f->homeBuilding != 0)
		{
			if (buildingAt(box.x, box.y, box.z) != f->homeBuilding)
				return; // the home's own storage slots
		} else if (side.empty() || box.ownerSid != side || towncircle::at(circles, box.x, box.z) != myTown) {
			return; // the town's storages of my side (the nearest-allies finder)
		}
		for (const std::string& item : loot)
		{
			state::Inventory probe = *stock;
			grid::settle(probe, rules, boxId.value);
			if (containerTakesType(box, item) && grid::autoplace(probe, rules, unit(item)) == 0)
			{
				boxes.push_back({boxId.value, box.x, box.z, box.useX, box.useZ, true, {item}});
				return;
			}
		}
	});
	return worldres::storage184(boxes, *f, worldres::kStockReach);
}

std::uint32_t Simulation::putLoot184(std::uint64_t id, std::uint64_t container)
{
	if (combat_ == nullptr)
		return 0;
	std::map<std::string, std::uint32_t> stacks; // per item: the carried units, in sid order
	if (const state::Inventory* held = fighterEntity(id).get<state::Inventory>())
		for (const state::Inventory::Entry& entry : held->items)
			if (entry.slot == state::Inventory::kCarried && entry.count > 0 && isLoot(combat_->data().item(entry.itemSid)))
				stacks[entry.itemSid] += entry.count;
	std::uint32_t moved = 0;
	for (const auto& [item, total] : stacks)
	{
		const std::string whole = checkTransfer(id, container, item, total, false);
		if (whole.empty())
		{
			transferItem(id, container, item, total, false);
			moved += total;
			continue;
		}
		if (whole != "full")
			continue; // a locked box, a type it refuses, out of reach: this stack stays
		for (std::uint32_t one = 0; one < total && checkTransfer(id, container, item, 1, false).empty(); ++one)
		{
			transferItem(id, container, item, 1, false);
			++moved;
		}
	}
	return moved;
}

} // namespace ksim
