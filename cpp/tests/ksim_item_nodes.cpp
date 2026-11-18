// Placement item nodes (state::ItemNode, WORLD_LAYOUT.md "Placement item nodes"): the ITEM_PLACEMENT_GROUP instances of the Hub's
// interior layouts on the Hub navmesh.
//   - spawnMeshItemNodes makes one node per layout instance on the mesh, each with an owned GroundItem from its group's lists, at
//     the node, inside its building's footprint; a second call makes none;
//   - a taken item is timed at the next hour and refills within [min, max] hours of its group, a new pick;
//   - a save keeps the nodes, the timer and the item.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/core/clock.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;

// The node's GroundItem, nullptr when none lies.
const state::GroundItem* itemOf(Simulation& sim, const state::ItemNode& node)
{
	const state::GroundItem* found = nullptr;
	ecsOf(sim).each([&](const state::Id& id, const state::GroundItem& item) { if (id.value == node.item) found = &item; });
	return found;
}

// Ticks to the first tick of game hour `hour` (stepItemNodes runs on the hour).
void toHour(Simulation& sim, std::uint64_t hour)
{
	sim.setClockTick(hour * kTicksPerGameHour - 1);
	sim.tick();
}

// The entries of an ITEM_PLACEMENT_GROUP the node may pick.
std::set<std::string> listOf(const kdata::GameData& gd, const std::string& groupSid)
{
	std::set<std::string> out;
	const kdata::Record* group = gd.find(groupSid);
	for (const char* category : {"items", "weapons", "armours", "clothing", "containers", "blueprints", "crossbows", "robotics"})
	{
		const kdata::RefCategory* refs = group != nullptr ? group->findRefs(category) : nullptr;
		for (std::size_t i = 0; refs != nullptr && i < refs->entries.size(); ++i)
			out.insert(refs->entries[i].targetID);
	}
	return out;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_item_nodes HUB.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation sim = Simulation::load(KENSHI_DATA, 91);
		sim.setGameData(gd);
		sim.enableCombat(data, CombatSettings{});
		sim.enableNavigation(nav);

		// ---- spawn ----
		const std::size_t made = spawnMeshItemNodes(sim, *nav);
		std::cout << "item nodes on the Hub mesh: " << made << "\n";
		test::require(made >= 10, "too few Hub item nodes: " + std::to_string(made));
		test::require(spawnMeshItemNodes(sim, *nav) == 0, "a second spawn made nodes again");
		std::size_t owned = 0;
		std::map<std::string, int> groups;
		std::set<std::uint64_t> keys;
		ecsOf(sim).each([&](const state::ItemNode& node)
		{
			test::require(keys.insert(node.key).second, "two nodes share a key");
			const state::GroundItem* item = itemOf(sim, node);
			test::require(item != nullptr, "a new node has no item out");
			test::require(listOf(*gd, node.groupSid).count(item->itemSid) != 0, "an item not from its group's lists: " + item->itemSid);
			test::require(item->x == node.x && item->z == node.z && item->ownerSid == node.ownerSid, "the item is not the node's");
			test::require(std::hypot(node.useX - node.x, node.useZ - node.z) <= kContainerSnap, "the use point is off the node");
			owned += node.ownerSid.empty() ? 0 : 1;
			const kdata::Record* group = gd->find(node.groupSid);
			++groups[group != nullptr ? group->name : node.groupSid];
		});
		for (const auto& [name, n] : groups)
			std::cout << "  " << name << ": " << n << "\n";
		test::require(owned * 2 >= made, "fewer than half the Hub nodes are owned: " + std::to_string(owned));

		// ---- take one, it refills ----
		flecs::entity nodeEntity;
		ecsOf(sim).each([&](flecs::entity e, const state::ItemNode&) { if (!nodeEntity.is_valid()) nodeEntity = e; });
		const state::ItemNode before = *nodeEntity.get<state::ItemNode>();
		ecsOf(sim).each([&](flecs::entity e, const state::Id& id, const state::GroundItem&) { if (id.value == before.item) e.destruct(); });
		toHour(sim, 10);
		const state::ItemNode timed = *nodeEntity.get<state::ItemNode>();
		test::require(timed.item == 0 && timed.refillTick > 10 * kTicksPerGameHour, "a taken item was not timed");
		const kdata::Record* group = gd->find(before.groupSid);
		const std::int64_t minHours = group->intOr("min respawn time", 24);
		const std::int64_t maxHours = std::max<std::int64_t>(minHours, group->intOr("max respawn time", 48));
		const double hours = static_cast<double>(timed.refillTick - 10 * kTicksPerGameHour) / static_cast<double>(kTicksPerGameHour);
		test::require(hours >= static_cast<double>(minHours) - 1e-6 && hours <= static_cast<double>(maxHours) + 1e-6,
			"the refill is " + std::to_string(hours) + " h away, not in [min, max]");

		// A save while it waits keeps the timer.
		const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_item_nodes.save";
		sim.saveGame(path);
		{
			Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path);
			std::size_t nodes = 0;
			std::uint64_t refill = 0;
			ecsOf(loaded).each([&](const state::ItemNode& node)
			{
				++nodes;
				if (node.key == before.key)
					refill = node.refillTick;
			});
			test::require(nodes == made, "the save lost nodes");
			test::require(refill == timed.refillTick, "the save lost the refill timer");
		}

		toHour(sim, 10 + static_cast<std::uint64_t>(maxHours) + 1);
		const state::ItemNode refilled = *nodeEntity.get<state::ItemNode>();
		test::require(refilled.item != 0 && refilled.item != before.item && itemOf(sim, refilled) != nullptr, "the node never refilled");
		test::require(refilled.picks == before.picks + 1, "the refill is not a new pick");
		std::cout << "ksim_item_nodes: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_item_nodes: " << e.what() << "\n";
		return 1;
	}
}
