// Item provenance (task ds/provenance, save v89, ARCHITECTURE.md "Item provenance"): planted is silent (concatenated on a merge, taken first in,
// first out by a split), stolen is part of the merge key, and both survive a save with the same stateHash, and the previous version's save too.

#include "dt27_rig.hpp"

#include "ksim/inventory_grid.hpp"
#include "ksim/save.hpp"

#include <cstdio>

namespace {

using namespace ksim;
using namespace dt27;

std::uint32_t plantedUnits(const state::Inventory::Entry& entry)
{
	std::uint32_t total = 0;
	for (const state::Planting& p : entry.planted)
		total += p.units;
	return total;
}

std::string stackableSid(const kdata::GameData& gd, const grid::Rules& rules)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("ITEM")))
		if (rules.stackCap(record->stringID, nullptr) >= 8)
			return record->stringID;
	throw std::runtime_error("no stackable ITEM");
}

state::Inventory::Entry stack(const std::string& sid, std::uint32_t count)
{
	state::Inventory::Entry entry;
	entry.itemSid = sid;
	entry.count = count;
	entry.slot = state::Inventory::kCarried;
	return entry;
}

void mergeAndSplit(const kdata::GameData& gd)
{
	const grid::Rules rules(gd);
	const std::string sid = stackableSid(gd, rules);
	state::Inventory inv;
	inv.gridW = 10;
	inv.gridH = 10;

	// Two plantings merge into one stack: the lists concatenate, in order.
	state::Inventory::Entry a = stack(sid, 2);
	a.planted = {{2, 7, 100}};
	state::Inventory::Entry b = stack(sid, 3);
	b.planted = {{3, 8, 200}};
	test::require(grid::autoplace(inv, rules, a) == 0 && grid::autoplace(inv, rules, b) == 0, "autoplace left units over");
	test::require(inv.items.size() == 1 && inv.items[0].count == 5, "planted stacks did not merge (planted is silent)");
	test::require(inv.items[0].planted.size() == 2 && inv.items[0].planted[0].planter == 7 && inv.items[0].planted[1].planter == 8, "the merge did not concatenate the lists in order");

	// A clean stack merges into a planted one too, and adds no planting.
	test::require(grid::autoplace(inv, rules, stack(sid, 1)) == 0 && inv.items.size() == 1 && inv.items[0].count == 6 && plantedUnits(inv.items[0]) == 5, "a clean stack did not join a planted one");

	// A split takes units first in, first out: 3 units are the 2 of planter 7 and 1 of planter 8.
	const std::vector<state::Inventory::Entry> taken = grid::takeFrom(inv, 0, 3);
	test::require(taken.size() == 1 && taken[0].planted.size() == 2 && taken[0].planted[0].planter == 7 && taken[0].planted[0].units == 2
		&& taken[0].planted[1].planter == 8 && taken[0].planted[1].units == 1, "the split did not take planter 7 first");
	test::require(inv.items[0].count == 3 && inv.items[0].planted.size() == 1 && inv.items[0].planted[0].planter == 8 && inv.items[0].planted[0].units == 2, "the rest of the split lost units");

	// Stolen is in the merge key: a stolen stack never joins a clean one, nor another victim's.
	state::Inventory kits;
	kits.gridW = 10;
	kits.gridH = 10;
	state::Inventory::Entry clean = stack(sid, 1);
	state::Inventory::Entry stolen = stack(sid, 1);
	stolen.stolen = "faction_a";
	state::Inventory::Entry other = stack(sid, 1);
	other.stolen = "faction_b";
	test::require(!clean.sameKind(stolen) && !stolen.sameKind(other) && stolen.sameKind(stolen), "sameKind ignores the stolen mark");
	grid::autoplace(kits, rules, clean);
	grid::autoplace(kits, rules, stolen);
	grid::autoplace(kits, rules, other);
	grid::autoplace(kits, rules, stolen);
	test::require(kits.items.size() == 3 && kits.items[1].count == 2, "stacks did not separate by stolen mark");
	std::cout << "  merge key and split ok\n";
}

void saveRoundTrip()
{
	Rig rig({}, 0);
	const grid::Rules rules(*rig.gd);
	const std::string sid = stackableSid(*rig.gd, rules);
	const std::uint64_t holder = rig.spawn(rig.body, 30.0, 30.0);
	flecs::entity entity = rig.entityOf(holder);
	state::Inventory pack = *entity.get<state::Inventory>();
	state::Inventory::Entry planted = stack(sid, 4);
	planted.planted = {{3, 777, 123}, {1, 778, 456}};
	state::Inventory::Entry stolen = stack(sid, 2);
	stolen.stolen = "victim_faction";
	grid::autoplace(pack, rules, planted);
	grid::autoplace(pack, rules, stolen);
	++pack.version;
	entity.set<state::Inventory>(std::move(pack));
	rig.tick(3);

	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_provenance.save";
	const std::uint64_t held = rig.sim.stateHash();
	rig.sim.saveGame(path);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == held, "load gives the saved hash");
	const state::Inventory* back = nullptr;
	loaded.world().ecs().each([&](const state::Id& id, const state::Inventory& inv)
	{
		if (id.value == holder)
			back = &inv;
	});
	test::require(back != nullptr, "the holder is gone after load");
	bool sawPlanted = false;
	bool sawStolen = false;
	for (const state::Inventory::Entry& entry : back->items)
	{
		if (entry.itemSid != sid)
			continue;
		if (entry.planted.size() == 2 && entry.planted[0].planter == 777 && entry.planted[0].units == 3 && entry.planted[0].tick == 123 && entry.planted[1].planter == 778)
			sawPlanted = true;
		if (entry.stolen == "victim_faction" && entry.count == 2)
			sawStolen = true;
	}
	test::require(sawPlanted && sawStolen, "the planted list or the stolen mark changed in the save");
	std::remove(path.c_str());

	// The previous version (v89, the one that introduced provenance) is the only older one a build reads, and it carries the marks.
	const std::string old = std::string(KSIM_BINARY_DIR) + "/ksim_provenance_prev.save";
	writeSaveFile(old, rig.sim.world().ecs(), rig.sim.world().loadOrder(), {}, kSaveVersion - 1);
	Simulation legacy = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), old, false, nullptr, RegionConfig{});
	std::size_t marked = 0;
	legacy.world().ecs().each([&](const state::Inventory& inv)
	{
		for (const state::Inventory::Entry& entry : inv.items)
			marked += entry.planted.size() + (entry.stolen.empty() ? 0 : 1);
	});
	test::require(marked > 0, "the previous version's save lost the provenance marks");
	std::remove(old.c_str());
	std::cout << "  save v89 round trip (stateHash asserted) and the previous version load ok\n";
}

} // namespace

int main()
{
	try
	{
		Rig rig({}, 0);
		mergeAndSplit(*rig.gd);
		saveRoundTrip();
		std::cout << "ksim_provenance: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_provenance: FAILED: " << e.what() << "\n";
		return 1;
	}
}
