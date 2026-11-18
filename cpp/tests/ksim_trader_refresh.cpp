// The merchant refresh (docs/reference/TOWN_LIFE_SLICE.md phase T1, kinspect item 53 / b53v):
//   - the pure rules against worked values: the N range and mean, the v0 split with the remainder to the last list, the level
//     weights, the half-fill placement by value (overflow destroyed and counted), the wallet formula with its 3000 / 25000 clamp;
//   - a stepped trader on the real data: the spawn refresh arms the timer and fills the store, the timer fires once after days
//     away at full detail (no catch-up) and re-arms to exactly now + P, a squad that is not at full detail changes nothing,
//     unique special items survive every wipe and are never re-created, what a player sold in is wiped;
//   - save and load in the middle of a timer give the uninterrupted run's state hash.

#include "ksim/rng.hpp"
#include "ksim/save.hpp"
#include "ksim/simulation.hpp"
#include "ksim/trade.hpp"
#include "ksim/trader.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;

void requireNear(double value, double expected, double tolerance, const std::string& what)
{
	test::require(std::fabs(value - expected) <= tolerance, what + ": " + std::to_string(value) + " vs " + std::to_string(expected));
}

// ---- the pure rules ----

void pureRules()
{
	// N = max(1, round(U(0.8, 1.2) F)); F < 1 means 100.
	test::require(trader::stockCount(15, 0.0) == 12 && trader::stockCount(15, 0.999999) == 18, "N for F = 15 spans 12 .. 18");
	test::require(trader::stockCount(0, 0.0) == 80 && trader::stockCount(-4, 0.999999) == 120, "F < 1 is 100: N spans 80 .. 120");
	test::require(trader::stockCount(1, 0.0) == 1 && trader::stockCount(1, 0.5) == 1, "N is at least 1");
	for (const int fill : {15, 0})
	{
		RngStream dice(streamSeed(11, 99));
		double sum = 0.0;
		int least = 1 << 30;
		for (int i = 0; i < 10000; ++i)
		{
			const int n = trader::stockCount(fill, dice.uniform01());
			sum += n;
			least = std::min(least, n);
		}
		requireNear(sum / 10000.0, fill == 0 ? 100.0 : 15.0, fill == 0 ? 1.0 : 0.15, "mean N for F = " + std::to_string(fill));
		test::require(least >= (fill == 0 ? 80 : 12), "least N for F = " + std::to_string(fill));
	}

	// The split: round(share) for all but the last, the remainder to the last (Scraphouse: 100 / 7 / 30 of 200).
	const std::vector<int> scraphouse = trader::splitCounts(200, {100.0, 7.0, 30.0});
	test::require(scraphouse == std::vector<int>({146, 10, 44}), "split 200 over 100 / 7 / 30 is 146 / 10 / 44");
	test::require(trader::splitCounts(100, {100.0, 100.0, 100.0}) == std::vector<int>({33, 33, 34}), "equal thirds: the remainder goes to the last list");
	test::require(trader::splitCounts(7, {50.0}) == std::vector<int>({7}), "one list takes all");
	test::require(trader::splitCounts(5, {0.0, 0.0}) == std::vector<int>({0, 5}), "no weight: the last list takes all");
	test::require(trader::splitCounts(3, {100.0, 100.0, 100.0, 100.0}) == std::vector<int>({1, 1, 1, 0}), "an overshoot leaves the last list empty, never negative");
	test::require(trader::entryWeight(0) == 100.0 && trader::entryWeight(30) == 30.0, "v0 0 weighs 100");

	// The weighted pick: the first entry whose running weight exceeds the roll.
	const std::vector<double> weights = {10.0, 0.0, 30.0};
	test::require(trader::pickWeighted(weights, 0.0) == 0 && trader::pickWeighted(weights, 0.249) == 0 && trader::pickWeighted(weights, 0.251) == 2
			&& trader::pickWeighted(weights, 0.999) == 2,
		"pickWeighted by running weight");
	test::require(trader::pickWeighted({0.0, 0.0}, 0.5) == 2, "an empty pool picks nothing");

	// Level weights, empirically (20,000 draws, 2 % tolerance).
	const auto frequencies = [](int level, bool crossbow)
	{
		RngStream dice(streamSeed(5, 77));
		std::map<int, double> seen;
		for (int i = 0; i < 20000; ++i)
			seen[trader::pickLevel(level, crossbow, dice.uniform01())] += 1.0 / 20000.0;
		return seen;
	};
	{
		const auto seen = frequencies(3, true); // 1: 20, 2: 33, 3: 100, 4: 33, 5: 5 of 191
		requireNear(seen.at(1), 20.0 / 191.0, 0.02, "crossbow L3 -> 1");
		requireNear(seen.at(2), 33.0 / 191.0, 0.02, "crossbow L3 -> 2");
		requireNear(seen.at(3), 100.0 / 191.0, 0.02, "crossbow L3 -> 3");
		requireNear(seen.at(4), 33.0 / 191.0, 0.02, "crossbow L3 -> 4");
		requireNear(seen.at(5), 5.0 / 191.0, 0.02, "crossbow L3 -> 5");
		const auto robots = frequencies(3, false); // 22 / 44 / 100 / 44 / 22 of 232
		requireNear(robots.at(1), 22.0 / 232.0, 0.02, "robotics L3 -> 1");
		requireNear(robots.at(2), 44.0 / 232.0, 0.02, "robotics L3 -> 2");
		requireNear(robots.at(3), 100.0 / 232.0, 0.02, "robotics L3 -> 3");
		const auto low = frequencies(1, false); // only 1..3: 100 / 44 / 22
		test::require(low.count(4) == 0 && low.count(5) == 0, "robotics L1 never reaches 4");
		requireNear(low.at(1), 100.0 / 166.0, 0.02, "robotics L1 -> 1");
		const auto top = frequencies(5, true);
		test::require(top.size() == 1 && top.count(5) == 1, "a level-5 crossbow list stays at 5");
	}

	// The wallet.
	test::require(trader::walletFor(0, true, 1000.0, 0.5) == 3000, "the floor");
	test::require(trader::walletFor(0, true, 40000.0, 0.5) == 10000, "a quarter of the stock value");
	test::require(trader::walletFor(0, true, 400000.0, 0.5) == 25000, "the cap");
	test::require(trader::walletFor(1000, true, 1.0, 0.0) == 600 && trader::walletFor(1000, true, 1.0, 0.999999) == 1400 && trader::walletFor(1000, true, 1.0, 0.5) == 1000,
		"vendor money V +- 0.4 V");
	test::require(trader::walletFor(1000, false, 40000.0, 0.5) == 10000, "a backpack trader ignores vendor money");
	test::require(trader::refreshPeriod(24.0f) == 24.0 && trader::refreshPeriod(0.0f) == 24000.0 && trader::refreshPeriod(-3.0f) == 24000.0, "refresh period, 24000 h = never");
	test::require(trader::refreshSeed(1, 5, 0) != trader::refreshSeed(1, 5, 1) && trader::refreshSeed(1, 5, 0) != trader::refreshSeed(1, 6, 0)
			&& trader::refreshSeed(1, 5, 0) == trader::refreshSeed(1, 5, 0),
		"refresh dice are keyed by squad and count");
}

// ---- placement ----

void placement(const kdata::GameData& data)
{
	const grid::Rules rules(data);
	// Three distinct 2 x 2 records (a 4 x 4 store holds 16 cells; half full = 8 = two of them).
	std::vector<std::string> sids;
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("ITEM")))
	{
		const grid::Size size = rules.footprint(record->stringID);
		if (size.w == 2 && size.h == 2 && rules.stackCap(record->stringID, nullptr) == 1 && sids.size() < 3)
			sids.push_back(record->stringID);
	}
	test::require(sids.size() == 3, "three 2 x 2 unstackable items in the data");
	state::Inventory store;
	store.gridW = 4;
	store.gridH = 4;
	std::vector<trader::Piece> pieces;
	for (std::size_t i = 0; i < 3; ++i)
	{
		trader::Piece piece;
		piece.entry.itemSid = sids[i];
		piece.entry.count = 1;
		piece.entry.slot = state::Inventory::kCarried;
		piece.value = i == 0 ? 10.0 : i == 1 ? 50.0 : 30.0;
		pieces.push_back(piece);
	}
	const trader::Placement result = trader::placeStock(store, rules, pieces);
	test::require(result.placed == 2 && result.destroyed == 1, "two placed, one destroyed once the store is half full");
	test::require(result.placedValue == 80.0, "the highest values (50 and 30) were placed");
	std::vector<std::string> kept;
	for (const auto& entry : store.items)
		kept.push_back(entry.itemSid);
	std::sort(kept.begin(), kept.end());
	std::vector<std::string> expected = {sids[1], sids[2]};
	std::sort(expected.begin(), expected.end());
	test::require(kept == expected, "the 10-value piece is the one destroyed");
	requireNear(trader::occupancy(store, rules), 0.5, 1e-9, "occupancy after two 2 x 2");

	// A stack too big for the store: the units that do not fit are destroyed and counted.
	state::Inventory tiny;
	tiny.gridW = 2;
	tiny.gridH = 2;
	trader::Piece big;
	big.entry.itemSid = sids[0];
	big.entry.count = 3;
	big.entry.slot = state::Inventory::kCarried;
	big.value = 9.0;
	const trader::Placement over = trader::placeStock(tiny, rules, {big});
	test::require(over.placed + over.destroyed == 3 && over.destroyed >= 2, "a stack that does not fit loses its overflow");
}

// ---- the stepped trader ----

struct Squad
{
	flecs::entity entity;
	std::uint64_t id = 0;
	float period = 24.0f;
};

std::vector<Squad> merchants(Simulation& sim)
{
	std::vector<Squad> found;
	ecsOf(sim).each([&](flecs::entity entity, const state::Id& id, const state::SquadInstance& squad, const state::TraderStock&)
	{
		found.push_back({entity, id.value, sim.gameData()->find(squad.templateSid)->floatOr("vendors refresh time", 24.0f)});
	});
	std::sort(found.begin(), found.end(), [](const Squad& a, const Squad& b) { return a.id < b.id; });
	return found;
}

std::uint32_t units(const state::Inventory& store, bool unique)
{
	std::uint32_t total = 0;
	for (const auto& entry : store.items)
		total += entry.unique == unique ? entry.count : 0;
	return total;
}

RegionConfig fullDetail()
{
	RegionConfig config;
	config.fullDetailAll = true;
	return config;
}

std::uint64_t dayTicks(double days)
{
	return static_cast<std::uint64_t>(days * kTicksPerGameDay);
}

// Runs until the squad's refresh count changes (at most `limit` ticks); the tick it fired on is the clock now.
bool runUntilRefresh(Simulation& sim, flecs::entity squad, std::uint32_t before, int limit)
{
	for (int i = 0; i < limit; ++i)
	{
		sim.run(1);
		if (squad.get<state::TraderStock>()->refreshes != before)
			return true;
	}
	return false;
}

void steppedTrader(const kworld::LoadOrder& order)
{
	SpawnOptions options;
	options.seed = 31;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, fullDetail());
	const kdata::GameData& data = *sim.gameData();
	const grid::Rules rules(data);
	std::vector<Squad> shops = merchants(sim);
	test::require(shops.size() >= 20, "the default world has merchant squads (" + std::to_string(shops.size()) + ")");
	const double start = trader::gameHours(sim.clockTick());
	std::size_t stocked = 0;
	for (const Squad& shop : shops)
	{
		const state::TraderStock& stock = *shop.entity.get<state::TraderStock>();
		test::require(stock.seeded && stock.refreshes == 1, "the spawn refresh ran once");
		requireNear(stock.nextRefresh, start + trader::refreshPeriod(shop.period), 1e-9, "the spawn refresh arms now + P");
		// A squad with pack animals (a wandering trader, T2) keeps its stock in its members' backpacks, not in a store of its own.
		std::uint32_t held = 0;
		if (trader::hasPackAnimals(data, *data.find(shop.entity.get<state::SquadInstance>()->templateSid)))
		{
			test::require(shop.entity.get<state::Inventory>() == nullptr, "a wandering trader has no squad store");
			sim.world().ecs().query_builder<const state::Inventory>().with<state::MemberOf>(shop.entity).build().each([&](const state::Inventory& pack)
			{
				if (const state::Inventory::Entry* bag = pack.wornBackpack())
				{
					for (const auto& entry : bag->contents)
						held += entry.count;
				}
			});
		} else {
			const state::Inventory* store = shop.entity.get<state::Inventory>();
			test::require(store != nullptr, "a merchant has a store");
			held = units(*store, false);
		}
		stocked += held > 0 ? 1 : 0;
		if (held > 0)
			test::require(stock.money > 0, "a stocked shop has a wallet");
	}
	test::require(stocked * 10 >= shops.size() * 8, "most merchants have stock after the spawn refresh");
	test::require(sim.traderStats().forced == shops.size(), "one forced refresh per merchant");

	// Pick the first shop with a plain 24 h timer and `vendors`.
	const Squad* picked = nullptr;
	for (const Squad& shop : shops)
	{
		if (shop.period == 24.0f && !trader::hasPackAnimals(data, *data.find(shop.entity.get<state::SquadInstance>()->templateSid)) && data.find(shop.entity.get<state::SquadInstance>()->templateSid)->intOr("vendor money", 0) == 0 && units(*shop.entity.get<state::Inventory>(), false) > 0 && picked == nullptr)
			picked = &shop;
	}
	test::require(picked != nullptr, "a 24 h merchant with stock");
	const flecs::entity shop = picked->entity;
	const kdata::Record& templ = *data.find(shop.get<state::SquadInstance>()->templateSid);
	const int fill = templ.intOr("vendors fill total amount", 15);
	const int mostDrawn = trader::stockCount(fill, 0.999999);

	// A player sold something in: it carries a mark no draw can produce.
	state::ItemQuality mark;
	mark.maker = "player-sold";
	mark.level = 0.123;
	const std::string soldSid = data.byType(kworld::itemTypeByName("ITEM")).front()->stringID;
	const auto sell = [&]()
	{
		state::Inventory store = *shop.get<state::Inventory>();
		state::Inventory::Entry sold;
		sold.itemSid = soldSid;
		sold.count = 1;
		sold.slot = state::Inventory::kCarried;
		sold.quality = mark;
		grid::autoplace(store, rules, sold);
		shop.set<state::Inventory>(store);
	};
	const auto soldPresent = [&]()
	{
		for (const auto& entry : shop.get<state::Inventory>()->items)
		{
			if (entry.quality.maker == "player-sold")
				return true;
		}
		return false;
	};
	sell();
	test::require(soldPresent(), "the player's sale is in the store");

	// Before the timer: nothing happens.
	sim.run(30);
	test::require(shop.get<state::TraderStock>()->refreshes == 1 && soldPresent(), "no refresh before the timer");

	// Away: the squad is not loaded, 30 game-days pass, nothing changes. Back: exactly one refresh.
	shop.set<Detail>(Detail{DetailLevel::Strategic});
	const state::Inventory awayStock = *shop.get<state::Inventory>();
	const state::TraderStock awayState = *shop.get<state::TraderStock>();
	sim.setClockTick(sim.clockTick() + dayTicks(30.0));
	sim.run(25);
	test::require(shop.get<state::TraderStock>()->refreshes == awayState.refreshes && shop.get<state::TraderStock>()->nextRefresh == awayState.nextRefresh
			&& shop.get<state::TraderStock>()->money == awayState.money && shop.get<state::Inventory>()->items.size() == awayStock.items.size() && soldPresent(),
		"a squad that is not at full detail changes nothing, 30 game-days later");
	shop.set<Detail>(Detail{DetailLevel::Full});
	const std::uint32_t before = shop.get<state::TraderStock>()->refreshes;
	test::require(runUntilRefresh(sim, shop, before, 40), "the timer fires on return");
	const state::TraderStock& after = *shop.get<state::TraderStock>();
	test::require(after.refreshes == before + 1, "one refresh");
	requireNear(after.nextRefresh, trader::gameHours(sim.clockTick()) + 24.0, 1e-9, "re-armed to exactly now + P");
	test::require(!soldPresent(), "the player's sale was wiped");
	test::require(units(*shop.get<state::Inventory>(), false) > 0 && units(*shop.get<state::Inventory>(), false) <= static_cast<std::uint32_t>(mostDrawn),
		"the new stock is at most N units");
	test::require(after.money >= trader::kMoneyFloor && after.money <= trader::kMoneyCap, "the wallet was re-seeded in the 3000 .. 25000 band");
	sim.run(400);
	test::require(shop.get<state::TraderStock>()->refreshes == before + 1, "no catch-up: no second refresh for the 30 days it was away");

	// A merchant is never refreshed at a lower detail level while the days pass, and a non-merchant squad has no state at all.
	std::size_t withState = 0;
	std::size_t traders = 0;
	ecsOf(sim).each([&](const state::SquadInstance& squad)
	{
		const kdata::Record* record = data.find(squad.templateSid);
		traders += record != nullptr && trader::isMerchant(*record) ? 1 : 0;
	});
	withState = merchants(sim).size();
	test::require(traders == withState, "exactly the merchant squads carry a TraderStock");
}

// ---- unique special items ----

void uniqueItems(const kworld::LoadOrder& order)
{
	SpawnOptions options;
	options.seed = 5;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, fullDetail());
	const kdata::GameData& data = *sim.gameData();
	const grid::Rules rules(data);
	const kdata::Record* special = nullptr;
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
	{
		const kdata::RefCategory* refs = record->findRefs("special items");
		// (a template with pack animals stocks the backpacks of its members, T2; this hand-made squad has none)
		if (special == nullptr && refs != nullptr && !refs->entries.empty() && data.find(refs->entries.front().targetID) != nullptr && !trader::hasPackAnimals(data, *record))
			special = record;
	}
	test::require(special != nullptr, "a squad template with special items");
	std::uint32_t wanted = 0;
	for (const kdata::RefEntry& entry : special->findRefs("special items")->entries)
		wanted += data.find(entry.targetID) != nullptr ? static_cast<std::uint32_t>(entry.v0) : 0;

	// A hand-made squad of that template (with its vendors, if it has any, it stocks too).
	flecs::world& ecs = ecsOf(sim);
	flecs::entity squad = state::createInstance(ecs);
	squad.set<state::SquadInstance>(state::SquadInstance{special->stringID, {}, {}, state::SpawnSource::TownResident});
	const trader::Outcome first = trader::refresh(squad, data, sim.tradeBook(), rules, 9, 100.0, true);
	test::require(first.specials == wanted, "the spawn refresh makes every special unit (the template may also stock and overflow) (" + std::to_string(first.specials) + " of " + std::to_string(wanted) + ", destroyed " + std::to_string(first.destroyed) + ", " + special->stringID + ")");
	test::require(units(*squad.get<state::Inventory>(), true) == wanted, "...flagged unique");
	test::require(squad.get<state::TraderStock>()->seeded, "seeded");

	for (int wipe = 0; wipe < 5; ++wipe)
	{
		state::Inventory store = *squad.get<state::Inventory>();
		state::Inventory::Entry sold;
		sold.itemSid = data.byType(kworld::itemTypeByName("ITEM")).front()->stringID;
		sold.count = 1;
		sold.slot = state::Inventory::kCarried;
		sold.quality.maker = "player-sold";
		sold.quality.level = 0.5;
		grid::autoplace(store, rules, sold);
		squad.set<state::Inventory>(store);
		const trader::Outcome again = trader::refresh(squad, data, sim.tradeBook(), rules, 9, 100.0 + 24.0 * (wipe + 1), false);
		test::require(again.specials == 0, "a timer refresh never re-creates specials");
		test::require(units(*squad.get<state::Inventory>(), true) == wanted, "the unique items survive wipe " + std::to_string(wipe + 1));
		bool soldStays = false;
		for (const auto& entry : squad.get<state::Inventory>()->items)
			soldStays = soldStays || entry.quality.maker == "player-sold";
		test::require(!soldStays, "a player-sold item is wiped");
	}
	test::require(squad.get<state::TraderStock>()->refreshes == 6, "six refreshes counted");
	requireNear(squad.get<state::TraderStock>()->nextRefresh, 100.0 + 24.0 * 5 + 24.0, 1e-9, "the last timer");

	// A clone keeps the flag; a unique stack never merges into a plain one of the same record.
	state::Inventory::Entry unique;
	unique.itemSid = "x";
	unique.unique = true;
	state::Inventory::Entry plain = unique;
	plain.unique = false;
	test::require(!unique.sameKind(plain) && state::Inventory::Entry(unique).unique, "unique is part of the merge key and copies");
}

// ---- save and load in the middle of a timer ----

void saveLoad(const kworld::LoadOrder& order)
{
	SpawnOptions options;
	options.seed = 77;
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_trader_refresh_test.ksav";
	constexpr std::uint64_t kBefore = 30;
	constexpr std::uint64_t kAfter = 60;
	const auto prepare = [&](Simulation& sim)
	{
		// Every timer falls due 45 ticks in: after the save point (30), before the end (90).
		const double due = trader::gameHours(sim.clockTick() + 45) - 1e-9;
		for (const Squad& shop : merchants(sim))
		{
			state::TraderStock stock = *shop.entity.get<state::TraderStock>();
			stock.nextRefresh = due;
			shop.entity.set<state::TraderStock>(stock);
		}
		sim.run(kBefore);
	};
	Simulation straight = Simulation::newGame(KENSHI_DATA, order, options, nullptr, fullDetail());
	prepare(straight);
	test::require(straight.traderStats().refreshes == 0, "no timer has fired at the save point");
	const std::uint64_t splitHash = straight.stateHash();
	straight.run(kAfter);
	test::require(straight.traderStats().refreshes > 0, "the timers fired after the save point");
	const std::uint64_t endHash = straight.stateHash();

	{
		Simulation first = Simulation::newGame(KENSHI_DATA, order, options, nullptr, fullDetail());
		prepare(first);
		test::require(first.stateHash() == splitHash, "the prepared run is deterministic");
		first.saveGame(path);
	}
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, fullDetail());
	test::require(loaded.stateHash() == splitHash, "load gives the saved hash");
	loaded.run(kAfter);
	test::require(loaded.stateHash() == endHash, "load and continue gives the straight run's hash");
	const std::vector<Squad> a = merchants(straight);
	const std::vector<Squad> b = merchants(loaded);
	test::require(a.size() == b.size() && !a.empty(), "the same merchants");
	for (std::size_t i = 0; i < a.size(); ++i)
	{
		const state::TraderStock& x = *a[i].entity.get<state::TraderStock>();
		const state::TraderStock& y = *b[i].entity.get<state::TraderStock>();
		test::require(x.refreshes == y.refreshes && x.nextRefresh == y.nextRefresh && x.money == y.money && x.destroyed == y.destroyed && x.seeded == y.seeded,
			"the timer, count and wallet survive the save");
	}
	std::remove(path.c_str());
}

} // namespace

int main()
{
	try
	{
		pureRules();
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		{
			const kdata::GameData data = kworld::loadMergedGameData(order);
			placement(data);
		}
		steppedTrader(order);
		uniqueItems(order);
		saveLoad(order);
		std::cout << "ksim_trader_refresh: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_trader_refresh: FAILED: " << e.what() << "\n";
		return 1;
	}
}
