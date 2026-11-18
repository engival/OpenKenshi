// The town shopping trip (task CH group 3, docs/reference/TOWN_LIFE_SLICE.md 5.26; kinspect dt24_shop / dt24_118 / TQ24):
//   - the list: a CHARACTER's `shopping` refs (ITEM, want, chance, priority) rolled for the day on the shopping stream, merged per
//     ITEM `item function`, kept in priority order; a chance the roll misses leaves the ref out; a template with no refs has no list
//     and never shops;
//   - the 118 resolver: uniform among the shops that stock a wanted function, else the town's first shop (the counter fallback);
//     a shop with no counter scores kShoppingNoCounterScore;
//   - 119's transaction: one unit, at random among the stock units of the first unmet function, out of the shop and into the
//     member's inventory; a failed pulse after a successful one ends the goal;
//   - the trip runs in the real tick (score -> walk -> transaction), survives a save / load and the loaded run makes the same
//     decisions as the straight one (the v86 fields: the list, its day, the timer and the latch).

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/routines.hpp"
#include "ksim/shopping.hpp"
#include "ksim/simulation.hpp"
#include "ksim/templates.hpp"
#include "ksim/trader.hpp"

#include "../src/ksim/world_resolvers.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;


class StraightNavigator final : public Navigator
{
public:
	bool covers(double, double) const override { return true; }
	Route route(double fromX, double fromZ, double toX, double toZ) const override
	{
		Route route;
		route.found = true;
		route.points = {{fromX, fromZ}, {toX, toZ}};
		return route;
	}
};

Simulation newGame(const kworld::LoadOrder& order)
{
	SpawnOptions options;
	options.seed = 1; // the Hub's bar squads (as ksim_town_bar)
	options.wildernessByDensity = false;
	return Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
}

void enable(Simulation& sim, const std::shared_ptr<const CombatData>& combat)
{
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<StraightNavigator>();
	sim.enableAi(std::move(config));
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
}

void centreOn(Simulation& sim, double x, double z)
{
	sim.enqueue(Command::setPlayerAreas({{x, z}}));
	sim.run(1);
}

std::uint64_t idOf(flecs::entity e)
{
	return e.get<state::Id>()->value;
}

void runUntil(Simulation& sim, const std::function<bool()>& done, std::uint64_t maxTicks, const std::string& what)
{
	for (std::uint64_t i = 0; i < maxTicks; ++i)
	{
		if (done())
			return;
		sim.run(1);
	}
	test::require(done(), what);
}

// The star of the transaction test: a squad holding a merchant's stock in a building its members call home (the same link the
// scorer's shop walk and stepTraders use).
struct Shop
{
	flecs::entity squad;
	std::uint64_t building = 0;
	double x = 0.0;
	double z = 0.0;
};

std::optional<Shop> merchantShop(Simulation& sim)
{
	std::optional<Shop> found;
	ecsOf(sim).each([&](flecs::entity squad, const state::TraderStock&, const state::Inventory&) {
		if (found.has_value())
			return;
		for (const flecs::entity member : membersOf(sim, squad))
		{
			const state::Fighter* f = member.get<state::Fighter>();
			if (f == nullptr || f->dead || f->homeBuilding == 0)
				continue;
			found = Shop{squad, f->homeBuilding, f->x, f->z};
			return;
		}
	});
	return found;
}

std::uint64_t unitsOf(const state::Inventory& inventory, const CombatData& items, std::uint16_t function)
{
	std::uint64_t units = 0;
	for (const state::InventoryEntry& entry : inventory.items)
	{
		const ItemInfo* info = items.item(entry.itemSid);
		if (info != nullptr && static_cast<std::uint16_t>(std::max(0, info->function)) == function)
			units += entry.count;
	}
	return units;
}

// The first stock item whose ITEM function is not 0 (a functionless record cannot show that the filter works).
const state::InventoryEntry* stockItem(const state::Inventory& stock, const CombatData& items, std::uint16_t* function)
{
	for (const state::InventoryEntry& entry : stock.items)
	{
		const ItemInfo* info = items.item(entry.itemSid);
		if (info == nullptr || info->function <= 0 || entry.count == 0)
			continue;
		*function = static_cast<std::uint16_t>(info->function);
		return &entry;
	}
	return nullptr;
}

const tmpl::Character* characterTemplate(const World& world, std::string_view sid)
{
	const auto prefab = world.prefabs().find(std::string(sid));
	return prefab != world.prefabs().end() ? prefab->second.get<tmpl::Character>() : nullptr;
}

// ---- the list (buildList) ----

void testListBuild(const Simulation& probe, const std::shared_ptr<const CombatData>& combat)
{
	// Caravan trader carries 7 refs, several at 50 % (gamedata.base and rebirth.mod both; the merged record decides).
	const tmpl::Character* record = characterTemplate(probe.world(), "1090-gamedata.base");
	test::require(record != nullptr && !record->shopping.empty(), "Caravan trader carries shopping refs");
	std::cout << "list: Caravan trader has " << record->shopping.size() << " refs\n";

	const std::vector<state::ShoppingWant> list = shopping::buildList(record->shopping, *combat, 7, 11, 0);
	const std::vector<state::ShoppingWant> again = shopping::buildList(record->shopping, *combat, 7, 11, 0);
	test::require(list == again, "the list is a pure function of (refs, seed, dice key, day)");
	test::require(shopping::unmet(list),
		"the day's list wants something (a 100 % ref with a wanted count is always on it)");

	// Every ref the roll always keeps (chance 100) is on the list: the want of its function is at least the ref's.
	for (const tmpl::ShoppingRef& ref : record->shopping)
	{
		const ItemInfo* info = combat->item(ref.itemSid);
		if (ref.chance < 100 || ref.want <= 0 || info == nullptr)
			continue;
		const std::uint16_t function = static_cast<std::uint16_t>(std::max(0, info->function));
		const auto found = std::find_if(list.begin(), list.end(), [&](const state::ShoppingWant& entry) { return entry.function == function; });
		test::require(found != list.end() && found->want >= ref.want,
			"a 100 % ref is on the list with its want (function " + std::to_string(function) + ")");
		test::require(found->taken == 0, "a fresh list has bought nothing");
	}

	// A chance of 0 never passes the roll: a ref with want 0 is never on it either.
	const tmpl::ShoppingRef& sample = record->shopping.front();
	std::vector<tmpl::ShoppingRef> none = {{sample.itemSid, 1, 0, 0}};
	test::require(shopping::buildList(none, *combat, 7, 11, 0).empty(), "a 0 % ref never reaches the list");
	none = {{sample.itemSid, 0, 100, 0}};
	test::require(shopping::buildList(none, *combat, 7, 11, 0).empty(), "a want of 0 never reaches the list");

	// Priority: the list is kept in the refs' priority order (v2, lower first), whatever the data order.
	std::vector<tmpl::ShoppingRef> two;
	std::set<std::uint16_t> distinct;
	for (const tmpl::ShoppingRef& ref : record->shopping)
	{
		const ItemInfo* info = combat->item(ref.itemSid);
		if (info == nullptr || ref.want <= 0)
			continue;
		const std::uint16_t function = static_cast<std::uint16_t>(std::max(0, info->function));
		if (!distinct.insert(function).second)
			continue;
		two.push_back({ref.itemSid, 1, 100, static_cast<std::int32_t>(two.size())});
		if (two.size() == 2)
			break;
	}
	test::require(two.size() == 2, "the record names two items of distinct functions");
	two[0].priority = 5;
	two[1].priority = 0;
	const std::vector<state::ShoppingWant> ordered = shopping::buildList(two, *combat, 7, 11, 0);
	test::require(ordered.size() == 2 && ordered[0].function == static_cast<std::uint16_t>(combat->item(two[1].itemSid)->function)
		&& ordered[1].function == static_cast<std::uint16_t>(combat->item(two[0].itemSid)->function), "the lower priority comes first");
	// One function twice: the wants add (and the lower priority wins).
	std::vector<tmpl::ShoppingRef> same = {{two[0].itemSid, 2, 100, 5}, {two[0].itemSid, 1, 100, 0}};
	const std::vector<state::ShoppingWant> merged = shopping::buildList(same, *combat, 7, 11, 0);
	test::require(merged.size() == 1 && merged[0].want == 3, "refs of one function merge their wants");

	// The daily roll: over ten days the chance-50 refs come and go (the 100 % ones always stay).
	std::set<std::size_t> sizes;
	for (std::int64_t day = 0; day < 10; ++day)
		sizes.insert(shopping::buildList(record->shopping, *combat, 7, 11, day).size());
	test::require(sizes.size() > 1, "the day's rolls change the list (the chance refs)");
}

// ---- the resolver (1409997f0) ----

void testResolver()
{
	using worldres::Shop;
	std::vector<Shop> shops(3);
	shops[0] = {100, false, true};
	shops[1] = {200, true, true};
	shops[2] = {300, true, false};
	double score = 0.0;
	test::require(worldres::shopping118(shops, 0.0, &score) == 1 && score == 1.0, "roll 0 picks the first stocking shop (a counter)");
	test::require(worldres::shopping118(shops, 0.99, &score) == 2 && score == routines::kShoppingNoCounterScore,
		"roll ~1 picks the last stocking shop (no counter: the stall score)");
	// Uniform over the stockers only: the non-stocking shop 0 is never picked while one stocks.
	for (int i = 0; i < 20; ++i)
		test::require(worldres::shopping118(shops, static_cast<double>(i) / 20.0) != 0, "a non-stocking shop is not picked while another stocks");
	// Nothing stocks: the town's first shop, the counter fallback (1409fc1b0).
	std::vector<Shop> cold = {{100, false, true}, {200, false, false}};
	test::require(worldres::shopping118(cold, 0.7, &score) == 0 && score == 1.0, "with no stocker the first shop is the fallback");
	std::vector<Shop> empty;
	test::require(worldres::shopping118(empty, 0.0) == 0, "no shop: the caller's no-pick");
}

// ---- the transaction (takeOne) ----

void testTakeOne(const std::shared_ptr<const CombatData>& combat)
{
	// Two stock entries of one function: a stack of two and a single.
	std::string sid;
	std::uint16_t function = 0;
	for (const auto& [candidate, info] : combat->items)
	{
		if (info.function > 0)
		{
			sid = candidate;
			function = static_cast<std::uint16_t>(info.function);
			break;
		}
	}
	test::require(!sid.empty(), "the data names an item with a function");
	state::Inventory stock;
	state::InventoryEntry stack;
	stack.itemSid = sid;
	stack.count = 2;
	stack.slot = state::Inventory::kCarried;
	stock.items.push_back(stack);
	stack.count = 1;
	stock.items.push_back(stack);
	state::InventoryEntry other;
	other.itemSid = "2-gamedata.base"; // a record with another function (or none: it is not matched either way)
	other.count = 5;
	stock.items.push_back(other);

	state::InventoryEntry piece;
	const std::uint32_t stockVersion = stock.version;
	test::require(shopping::takeOne(stock, *combat, function, 0.0, piece), "the transaction takes a unit");
	test::require(stock.items.size() == 3 && stock.items[0].count == 1 && stock.items[1].count == 1,
		"roll 0 splits the first stack (2 -> 1)");
	test::require(piece.itemSid == sid && piece.count == 1 && piece.section == state::InventoryEntry::kUnplaced,
		"the piece is one unplaced unit");
	test::require(stock.version == stockVersion + 1, "the stock's version moves");
	// Two units of the function left (one per entry): roll ~1 takes the single-object entry, which leaves.
	test::require(shopping::takeOne(stock, *combat, function, 0.99, piece), "the transaction takes the next unit");
	test::require(stock.items.size() == 2 && stock.items[0].count == 1, "the single-object entry of that function is gone");
	// The last unit: its stack leaves too, and then there is nothing left to take.
	test::require(shopping::takeOne(stock, *combat, function, 0.0, piece), "the transaction takes the last unit");
	test::require(stock.items.size() == 1, "no stack of the function is left");
	test::require(!shopping::takeOne(stock, *combat, function, 0.5, piece), "no unit of the function left: no transaction");
	test::require(!shopping::takeOne(stock, *combat, static_cast<std::uint16_t>(function + 997), 0.5, piece),
		"a function no entry carries: no transaction");
	test::require(unitsOf(stock, *combat, function) == 0 && stock.items[0].count == 5, "the entries of other functions are untouched");
}

// ---- the trip in the real tick, across a save / load ----

void testTrip(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation straight = newGame(order);
	enable(straight, combat);
	centreOn(straight, kHubX, kHubZ);
	// Some bar squad promoted: its members are Fighters with a routine, like the shopper of a real town.
	flecs::entity shopper;
	runUntil(straight, [&] {
		ecsOf(straight).each([&](flecs::entity member, const state::Routine&, const state::Fighter&) {
			if (!shopper.is_valid() && member.get<state::CharacterInstance>() != nullptr && member.get<state::Inventory>() != nullptr)
				shopper = member;
		});
		return shopper.is_valid();
	}, 4000, "a promoted town member before the trip");
	test::require(shopper.is_valid(), "the Hub has a promoted member");

	const std::optional<Shop> shop = merchantShop(straight);
	test::require(shop.has_value(), "the Hub has a merchant squad with a home building");
	std::uint16_t function = 0;
	const state::InventoryEntry* item = stockItem(*shop->squad.get<state::Inventory>(), *combat, &function);
	test::require(item != nullptr, "the shop's stock carries an item with an item function");
	const std::string stocked = item->itemSid;
	std::cout << "trip: shop building " << shop->building << ", function " << function << " (" << stocked << ")\n";

	// The shopper stands at the counter (the scorer walks it there; the test places it). The arena's Fighter is the live copy and the
	// entity's state::Fighter is the save's, so both move (mirrorFighter is the tick's own copy).
	state::Fighter& f = *const_cast<state::Fighter*>(straight.combat()->find(idOf(shopper)));
	f.x = shop->x;
	f.z = shop->z;
	f.clearRoute();
	f.target = 0;
	f.asleep = false;
	shopper.set<state::Fighter>(f);
	const std::uint64_t shopBefore = unitsOf(*shop->squad.get<state::Inventory>(), *combat, function);
	test::require(shopper.get<state::Inventory>() != nullptr, "the shopper has an inventory");
	const std::uint64_t mineBefore = unitsOf(*shopper.get<state::Inventory>(), *combat, function);

	// The adopted goal, as the scorer writes it: the shop's counter, the day's list, the fresh timer.
	state::Routine routine = *shopper.get<state::Routine>();
	const double hours = trader::gameHours(straight.clockTick());
	const std::int32_t day = static_cast<std::int32_t>(std::floor(hours / 24.0));
	routine.goal = routines::task::Shopping;
	routine.since = straight.clockTick();
	routine.holdUntil = 0;
	routine.target = {};
	routine.nextScore = straight.clockTick() + 1000000; // the scorer leaves the injected goal alone
	routine.plan = {67, routines::task::BuyStuff, routines::task::Shopping};
	routine.step = 0;
	routine.shopping = {{function, 1, 0}};
	routine.shoppingDay = day;
	routine.shoppingTimer = 0.0;
	routine.shoppingLatch = false;
	routine.shoppingShop = shop->building;
	routine.shoppingX = shop->x;
	routine.shoppingZ = shop->z;
	shopper.set<state::Routine>(routine);

	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_shopping.save";
	const std::uint64_t split = straight.stateHash();
	straight.saveGame(path);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == split, "load gives the saved hash");
	enable(loaded, combat);
	test::require(loaded.stateHash() == split, "enabling AI and combat does not change the hash");
	flecs::entity reloadedEntity;
	ecsOf(loaded).each([&](flecs::entity e, const state::Id& id) {
		if (id.value == idOf(shopper))
			reloadedEntity = e;
	});
	const state::Routine* reloaded = reloadedEntity.get<state::Routine>();
	test::require(reloaded != nullptr && reloaded->goal == routines::task::Shopping && reloaded->shopping.size() == 1
		&& reloaded->shopping[0].function == function && reloaded->shoppingDay == day && reloaded->shoppingShop == shop->building,
		"the list, its day and the shop survive the save (v86)");

	// The two runs make the same decisions: chunks of 50 ticks, hashes equal.
	for (int chunk = 0; chunk < 8; ++chunk)
	{
		straight.run(50);
		loaded.run(50);
		if (loaded.stateHash() != straight.stateHash())
		{
			const state::Routine* a = shopper.get<state::Routine>();
			const state::Fighter* fa = straight.combat()->find(idOf(shopper));
			flecs::entity le;
			ecsOf(loaded).each([&](flecs::entity e, const state::Id& id) { if (id.value == idOf(shopper)) le = e; });
			const state::Routine* b = le.get<state::Routine>();
			const state::Fighter* fb = loaded.combat()->find(idOf(shopper));
			std::cout << "chunk " << chunk << ": straight " << std::hex << straight.stateHash() << " loaded " << loaded.stateHash() << std::dec << "\n"
				<< "  straight: goal " << (a ? a->goal : 0) << " taken " << (a && !a->shopping.empty() ? a->shopping[0].taken : 0) << " timer " << (a ? a->shoppingTimer : 0)
				<< " step " << (a ? a->step : 0) << " at " << (fa ? fa->x : 0) << "," << (fa ? fa->z : 0) << " route " << (fa ? fa->route.size() : 0) << "\n"
				<< "  loaded:   goal " << (b ? b->goal : 0) << " taken " << (b && !b->shopping.empty() ? b->shopping[0].taken : 0) << " timer " << (b ? b->shoppingTimer : 0)
				<< " step " << (b ? b->step : 0) << " at " << (fb ? fb->x : 0) << "," << (fb ? fb->z : 0) << " route " << (fb ? fb->route.size() : 0) << "\n";
		}
		test::require(loaded.stateHash() == straight.stateHash(), "the loaded run equals the straight one (chunk " + std::to_string(chunk) + ")");
	}

	// The transaction happened: one unit of the function left the shop and joined the shopper; the trip ended (a failed pulse after
	// the success latches the goal done) and the list is satisfied.
	const std::uint64_t shopAfter = unitsOf(*shop->squad.get<state::Inventory>(), *combat, function);
	const std::uint64_t mineAfter = unitsOf(*shopper.get<state::Inventory>(), *combat, function);
	test::require(shopAfter == shopBefore - 1, "the shop lost one unit of the function (" + std::to_string(shopBefore) + " -> " + std::to_string(shopAfter) + ")");
	test::require(mineAfter == mineBefore + 1, "the shopper gained one unit of the function (" + std::to_string(mineBefore) + " -> " + std::to_string(mineAfter) + ")");
	const state::Routine* after = shopper.get<state::Routine>();
	test::require(after != nullptr && after->shopping.size() == 1 && after->shopping[0].taken == 1, "the list counts the purchase");
	test::require(!shopping::unmet(after->shopping), "the day's list is satisfied");
	test::require(after->goal != routines::task::Shopping, "a failed pulse after the purchase ends the trip");
	std::remove(path.c_str());
	std::cout << "trip ok: shop " << shopBefore << " -> " << shopAfter << ", shopper " << mineBefore << " -> " << mineAfter << "\n";
}

} // namespace

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation probe = newGame(order);
		const std::shared_ptr<const CombatData> combat = CombatData::build(*probe.gameData(), KENSHI_DATA);
		testListBuild(probe, combat);
		testResolver();
		testTakeOne(combat);
		testTrip(order, combat);
	} catch (const std::exception& e) {
		std::cerr << "ksim_shopping: FAILED: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_shopping OK\n";
	return 0;
}
