// T4d (TOWN_LIFE_SLICE 7.4, row 7.7): the town stock's price factor on top of the trade chain, and the T1 draw filter. This pins:
//  - the pure rules: stockFactor is 2 at half stock, 0.5 with a glut, exactly 1 at the target and for an item the town does not stock,
//    clamped both ways; takeFromTown takes at most what the line holds, leaves the fraction and never makes it negative;
//  - the chain: the same entry quotes dearer in a short town and cheaper in a glutted one, both sides scale by the same factor (the
//    spread holds), and at the target the quote is exactly the chain's own — which is also the `original` price, bit for bit;
//  - the draw filter: a real-mode town with no stock fills nothing (the roll still happens), a drawn unit leaves the line, and no line
//    goes negative; `original` mode has no TownStock and fills exactly as T1 does.
//
// OURS / UNSURE: 7.4 says k, lo and hi are data; no data holds them, so economy.hpp has kPriceExponent 1, lo 0.5, hi 2 and an amount
// floor of 1. The factor applies to any item the town stocks (gear included, through the same quote) and to both sides of the spread.

#include "ksim/clock.hpp"
#include "ksim/economy.hpp"
#include "ksim/inventory_grid.hpp"
#include "ksim/simulation.hpp"
#include "ksim/trade.hpp"
#include "ksim/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

Simulation newGame(EconomyMode mode)
{
	SpawnOptions options;
	options.seed = 5;
	options.wildernessByDensity = false;
	options.economy = mode;
	RegionConfig regions;
	regions.workerThreads = 1;
	return Simulation::newGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), options, nullptr, regions);
}

struct Shop
{
	flecs::entity squad;
	flecs::entity town;
	std::uint64_t id = 0;
};

// A merchant with a home town and a store (no pack animals: its stock is the squad's one grid, easy to count).
std::vector<Shop> shopsOf(Simulation& sim)
{
	const kdata::GameData& data = *sim.gameData();
	flecs::world& ecs = const_cast<flecs::world&>(sim.world().ecs());
	std::vector<Shop> shops;
	ecs.each([&](flecs::entity squad, const state::SquadInstance& instance)
	{
		if (instance.homeTown.id == 0)
			return;
		const kdata::Record* record = data.find(instance.templateSid);
		if (record == nullptr || !trader::isMerchant(*record) || trader::hasPackAnimals(data, *record))
			return;
		const flecs::entity town(squad.world().c_ptr(), instance.homeTown.id);
		if (!town.is_valid())
			return;
		const state::Id* id = squad.get<state::Id>();
		shops.push_back({squad, town, id != nullptr ? id->value : squad.id()});
	});
	std::sort(shops.begin(), shops.end(), [](const Shop& left, const Shop& right) { return left.id < right.id; });
	return shops;
}

// The units of `itemSid` in a squad's store.
int stored(flecs::entity squad, const std::string& itemSid)
{
	const state::Inventory* store = squad.get<state::Inventory>();
	if (store == nullptr)
		return 0;
	int units = 0;
	for (const state::Inventory::Entry& entry : store->items)
		units += entry.itemSid == itemSid && !entry.unique ? static_cast<int>(entry.count) : 0;
	return units;
}

// ---- the pure rules ----

void pureRules()
{
	const auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
	state::TownStock stock; // sorted by itemSid: findLine() is a binary search
	stock.lines = {{"empty", 0.0, 100.0}, {"even", 100.0, 100.0}, {"glut", 200.0, 100.0}, {"short", 50.0, 100.0}};
	test::require(near(economy::stockFactor(stock, "short"), 2.0), "half the target: factor 2");
	test::require(near(economy::stockFactor(stock, "glut"), 0.5), "twice the target: factor 0.5");
	test::require(economy::stockFactor(stock, "even") == 1.0, "at the target: exactly 1");
	test::require(near(economy::stockFactor(stock, "empty"), economy::kPriceHigh), "an empty line is the scarce clamp (no division by zero)");
	test::require(economy::stockFactor(stock, "unknown") == 1.0, "an item the town does not stock has no factor");
	state::TownStock clamps;
	clamps.lines = {{"cheap", 200.0, 1.0}, {"dear", 1.0, 1000.0}};
	test::require(economy::stockFactor(clamps, "dear") == economy::kPriceHigh, "the factor is clamped high");
	test::require(economy::stockFactor(clamps, "cheap") == economy::kPriceLow, "the factor is clamped low");

	state::TownStock take;
	take.lines = {{"t", 3.7, 100.0}};
	test::require(economy::takeFromTown(take, "t", 5) == 3, "the draw takes at most what the town holds");
	test::require(near(take.lines[0].amount, 0.7), "the fraction stays");
	test::require(economy::takeFromTown(take, "t", 5) == 0 && near(take.lines[0].amount, 0.7), "an empty town gives nothing");
	test::require(economy::takeFromTown(take, "t", 2) == 0, "and still nothing while under a unit");
	test::require(economy::takeFromTown(take, "missing", 5) == 0, "an item the town does not stock");
	std::printf("pure: the factor's shape and clamps, the draw's floor\n");
}

// ---- the chain: the factor on top of quote(), and the spread ----

void chain(Simulation& sim)
{
	const kdata::GameData& data = *sim.gameData();
	const trade::Book book(data, 5);
	const kdata::Record* item = nullptr;
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("ITEM")))
		if (record->intOr("value", 0) > 0 && book.basePrice(*record) > 0)
		{
			item = record;
			break;
		}
	test::require(item != nullptr, "no priced item in the data");
	const state::Inventory::Entry entry{item->stringID, 1, 0, {}};

	state::TownStock stock;
	stock.lines = {{item->stringID, 100.0, 100.0}};
	trade::Terms terms;
	terms.stock = &stock;
	const trade::Quote base = book.quote(terms, entry);
	test::require(base.buy > 0 && base.sell > 0, "the item has a price pair");
	// At its target the T4 factor is 1: exactly the chain's own price (a hand-built Terms has no stock).
	const trade::Quote plain = book.quote(trade::Terms{}, entry);
	test::require(base.buy == plain.buy && base.sell == plain.sell, "at the target the quote is the chain's own, bit for bit");

	stock.lines[0].amount = 50.0; // half: factor 2
	const trade::Quote scarce = book.quote(terms, entry);
	stock.lines[0].amount = 200.0; // twice: factor 0.5
	const trade::Quote glut = book.quote(terms, entry);
	test::require(scarce.buy > base.buy && glut.buy < base.buy, "scarce costs more, a glut costs less");
	test::require(scarce.sell > base.sell && glut.sell < base.sell, "both sides move with the stock");
	test::require(scarce.buy == 2 * base.buy && scarce.sell == 2 * base.sell, "factor 2 scales both sides: the spread holds");
	test::require(glut.buy == (base.buy + 1) / 2 && glut.sell == (base.sell + 1) / 2, "factor 0.5 scales both sides");
	std::printf("chain: %d/%d at target, %d/%d short, %d/%d glutted\n", base.buy, base.sell, scarce.buy, scarce.sell, glut.buy, glut.sell);
}

// ---- the world: real against original, and the draw filter ----

void worlds(Simulation& original, Simulation& real)
{
	const std::vector<Shop> originals = shopsOf(original);
	const std::vector<Shop> reals = shopsOf(real);
	test::require(!originals.empty(), "no merchant shop in the original world");
	test::require(!reals.empty(), "no merchant shop in the real world");
	// The worlds are built identically up to the overlay: the first shop is the same instance.
	test::require(originals.front().id == reals.front().id, "the two worlds built different shops");
	const Shop& originalShop = originals.front();
	const Shop& realShop = reals.front();
	const kdata::GameData& data = *real.gameData();
	const trade::Book book(data, 5);

	state::TownStock& stock = *realShop.town.get_mut<state::TownStock>();
	// A stocked item with a price, so the factor can move it.
	std::size_t index = 0;
	for (; index < stock.lines.size(); ++index)
	{
		const kdata::Record* record = data.find(stock.lines[index].itemSid);
		if (record != nullptr && book.basePrice(*record) > 0)
			break;
	}
	test::require(index < stock.lines.size(), "the shop's town stocks no priced item");
	const std::string item = stock.lines[index].itemSid;
	const double target = stock.lines[index].target;
	test::require(target > 0.0 && std::fabs(stock.lines[index].amount - target) < 1e-9, "a seeded line is at its target");
	const state::Inventory::Entry entry{item, 1, 0, {}};

	// Seeded: the real price is the original price, bit for bit (factor 1), and original mode has no stock at all.
	const trade::Terms realTerms = book.termsOf(realShop.squad);
	const trade::Terms originalTerms = book.termsOf(originalShop.squad);
	test::require(realTerms.stock != nullptr, "the real world's shop has no market stock");
	test::require(originalTerms.stock == nullptr, "original mode has a stock factor");
	const trade::Quote seeded = book.quote(realTerms, entry);
	const trade::Quote plain = book.quote(originalTerms, entry);
	test::require(seeded.buy == plain.buy && seeded.sell == plain.sell, "at its seeded target the real price is the original one");
	// Scarcity: the same trade is dearer.
	stock.lines[index].amount = target * 0.5;
	const trade::Quote scarce = book.quote(book.termsOf(realShop.squad), entry);
	test::require(scarce.buy > plain.buy && scarce.sell > plain.sell, "a short town does not raise its price");
	test::require(scarce.buy == 2 * plain.buy && scarce.sell == 2 * plain.sell, "half stock doubles both sides");

	// The draw filter. Both worlds are at the same refresh count (the forced spawn fill), so the same dice: the original world's fill
	// (no TownStock, T1 as it was) shows what the roll draws, and the real world repeats it against a town that holds two units of the
	// best-drawn item.
	const grid::Rules rules(data);
	const double now = trader::gameHours(real.clockTick());
	const trader::Outcome t1 = trader::refresh(originalShop.squad, data, book, rules, 5, now, true);
	test::require(t1.placed > 0, "the original world's fill placed nothing");
	std::map<std::string, int> drew;
	for (const state::Inventory::Entry& entry : originalShop.squad.get<state::Inventory>()->items)
		if (!entry.unique)
			drew[entry.itemSid] += static_cast<int>(entry.count);
	const auto best = std::max_element(drew.begin(), drew.end(), [](const auto& left, const auto& right) { return left.second < right.second; });
	test::require(best != drew.end() && best->second >= 2, "the roll drew no item twice");
	const std::string drawnItem = best->first;
	for (state::StockLine& line : stock.lines)
		line.amount = 0.0;
	state::StockLine& chosen = economy::lineFor(stock, drawnItem);
	chosen.amount = 2.0;
	const trader::Outcome two = trader::refresh(realShop.squad, data, book, rules, 5, now, true);
	test::require(two.placed == 2, "the shop did not take exactly the two units the town held (" + std::to_string(two.placed) + ")");
	test::require(stored(realShop.squad, drawnItem) == 2, "the two units are not in the shop");
	test::require(chosen.amount == 0.0, "a drawn unit did not leave the town");
	// And with nothing left, the next roll fills nothing, whatever it draws.
	const trader::Outcome empty = trader::refresh(realShop.squad, data, book, rules, 5, now + 24.0, true);
	test::require(empty.drawn > 0, "the roll still happens");
	test::require(empty.placed == 0, "an empty town filled a shop (" + std::to_string(empty.placed) + ")");
	bool negative = false;
	for (const state::StockLine& line : stock.lines)
		negative = negative || line.amount < 0.0;
	test::require(!negative, "a stock line went negative");
	std::printf("worlds: the seeded real price matches original; the roll drew %d %s, the town held 2, the shop took 2\n",
		best->second, drawnItem.c_str());
}

} // namespace

int main()
{
	try
	{
		pureRules();
		Simulation real = newGame(EconomyMode::Real);
		Simulation original = newGame(EconomyMode::Original);
		chain(real);
		worlds(original, real);
		std::cout << "ksim_economy_price: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_economy_price: FAILED: " << e.what() << "\n";
		return 1;
	}
}
