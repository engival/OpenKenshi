// Wandering traders (docs/reference/TOWN_LIFE_SLICE.md phase T2, kinspect item 53 b53t and item 54 T2):
//   - the pure rules: the eligible-town filter, the 4-nearest pick, the U(5, 8) stay, the leave window 06:00-21:00;
//   - pack animals on the real data: the worn backpack, the animal's own grid, the backpack's grid, the spawn fill; a Caravan is never refreshed;
//   - a refresh fills the members' backpacks and the next one wipes what a player sold in;
//   - a stepped trader on the real data: arrives, stays U(5, 8) h, leaves only in the window, picks another town, moves at 25 u/s;
//   - a dead squad costs its faction 10 prosperity, which regenerates, and nothing replaces it;
//   - save and load in the middle of a stay give the uninterrupted run's state hash.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trade.hpp"
#include "ksim/economy/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
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

// ---- the pure rules ----

trader::TownFacts goodTown(std::uint64_t key, double x)
{
	trader::TownFacts town;
	town.key = key;
	town.x = x;
	town.z = 0.0;
	town.isPublic = true;
	town.relation = 0.0;
	return town;
}

void pureRules()
{
	// The stay is U(5, 8).
	test::require(trader::stayHours(0.0) == 5.0 && std::fabs(trader::stayHours(0.999999) - 8.0) < 1e-5 && trader::stayHours(0.5) == 6.5, "stay U(5, 8)");

	// The window: global hour 6..20.
	test::require(!trader::inLeaveWindow(5.99) && trader::inLeaveWindow(6.0) && trader::inLeaveWindow(20.99) && !trader::inLeaveWindow(21.0)
			&& !trader::inLeaveWindow(0.0) && trader::inLeaveWindow(24.0 * 7 + 12.5) && !trader::inLeaveWindow(24.0 * 7 + 23.0),
		"leave window 06:00 .. 20:59");
	const double day = 24.0 * 5;
	const auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
	test::require(near(trader::departureTime(day + 10.0, 5.0), day + 15.0), "a stay that ends at 15:00 leaves at 15:00");
	test::require(near(trader::departureTime(day + 18.0, 5.0), day + 24.0 + 6.0), "ends 23:00: waits for 06:00 the next day");
	test::require(near(trader::departureTime(day + 0.5, 5.0), day + 6.0), "ends 05:30: waits for 06:00 the same day");
	test::require(near(trader::departureTime(day + 16.0, 5.0), day + 24.0 + 6.0), "ends exactly 21:00: waits");
	test::require(near(trader::departureTime(day + 15.5, 5.0), day + 20.5), "ends 20:30: leaves");
	test::require(near(trader::departureTime(day + 7.0, 8.0), day + 15.0), "ends 15:00 after 8 h");

	// The filter, one rule at a time.
	trader::TownFacts base = goodTown(1, 100.0);
	test::require(trader::eligibleTown(base), "a public town");
	{
		trader::TownFacts t = base;
		t.isCurrent = true;
		test::require(!trader::eligibleTown(t), "not the current town");
		t = base;
		t.isPublic = false;
		test::require(!trader::eligibleTown(t), "a private town of another faction");
		t.ownedByOwnFaction = true;
		test::require(trader::eligibleTown(t), "a private town of its own faction");
		t = base;
		t.relation = -30.0;
		test::require(!trader::eligibleTown(t), "relation -30 is hostile");
		t.relation = -29.0;
		test::require(trader::eligibleTown(t), "relation -29 is not");
		t = base;
		t.ownerNotReal = true;
		test::require(!trader::eligibleTown(t), "not real");
		t = base;
		t.ownerBandit = true;
		test::require(!trader::eligibleTown(t), "a bandit town for a non-bandit");
		t.squadBandit = true;
		test::require(trader::eligibleTown(t), "a bandit town for a bandit");
		t = base;
		t.ownerAttitude = -50.0;
		t.relation = 20.0;
		test::require(!trader::eligibleTown(t), "a hostile-by-default owner needs relation 50");
		t.relation = 50.0;
		test::require(trader::eligibleTown(t), "...and gets it at 50");
		t = base;
		t.owned = false;
		test::require(!trader::eligibleTown(t), "a marker town has no owner");
		for (const int type : {0, 1, 4, 7, 8, 9})
		{
			t = base;
			t.type = type;
			test::require(!trader::eligibleTown(t), "town type " + std::to_string(type) + " is never visited");
		}
		for (const int type : {2, 3, 5, 6})
		{
			t = base;
			t.type = type;
			test::require(trader::eligibleTown(t), "town type " + std::to_string(type) + " is visited");
		}
		t = base;
		t.type = 1;
		t.ownerIsPlayer = true;
		test::require(trader::eligibleTown(t), "a player faction's town passes the type test");
	}

	// The 4 nearest eligible towns, nearest first; the current and the ineligible ones are skipped.
	std::vector<trader::TownFacts> towns;
	for (int i = 0; i < 8; ++i)
		towns.push_back(goodTown(10 + i, 100.0 * (i + 1)));
	towns[0].isCurrent = true; // the nearest is where it stands
	towns[1].relation = -80.0; // hostile
	towns[5].type = 4; // ruins
	const std::vector<std::size_t> nearest = trader::nearestEligible(towns, 0.0, 0.0);
	test::require(nearest == std::vector<std::size_t>({2, 3, 4, 6}), "the 4 nearest eligible are towns 2, 3, 4, 6");
	std::map<std::size_t, int> counts;
	RngStream dice(streamSeed(7, 3));
	constexpr int kDraws = 20000;
	for (int i = 0; i < kDraws; ++i)
		++counts[trader::pickTown(towns, 0.0, 0.0, dice.uniform01())];
	test::require(counts.size() == 4, "the pick uses exactly the 4 nearest");
	for (const auto& [index, count] : counts)
	{
		test::require(index != 0 && index != 1 && index != 5 && index != 7, "never the current, a hostile, a ruin or the fifth");
		test::require(std::fabs(count / static_cast<double>(kDraws) - 0.25) < 0.02, "each of the four about a quarter");
	}
	test::require(trader::pickTown({}, 0.0, 0.0, 0.5) == 0, "no town: nothing picked");
	std::vector<trader::TownFacts> none = {towns[0], towns[1]};
	test::require(trader::pickTown(none, 0.0, 0.0, 0.5) == none.size(), "no eligible town: nothing picked");
	// A tie in distance is broken by the key.
	std::vector<trader::TownFacts> tie = {goodTown(9, 50.0), goodTown(3, -50.0)};
	test::require(trader::nearestEligible(tie, 0.0, 0.0) == std::vector<std::size_t>({1, 0}), "ties go to the lower key");
}

// ---- the world ----

Simulation newGame(const kworld::LoadOrder& order, bool full)
{
	SpawnOptions options;
	options.seed = 12;
	options.wildernessByDensity = false;
	RegionConfig config;
	config.fullDetailAll = full;
	return Simulation::newGame(KENSHI_DATA, order, options, nullptr, config);
}

void enable(Simulation& sim)
{
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<StraightNavigator>();
	config.factions = FactionBook::build(sim.gameData());
	sim.enableAi(std::move(config));
}

struct Caravan
{
	flecs::entity squad;
	std::uint64_t id = 0;
};

std::vector<Caravan> squadsOf(Simulation& sim, const std::string& templateSid)
{
	std::vector<Caravan> found;
	ecsOf(sim).each([&](flecs::entity entity, const state::Id& id, const state::SquadInstance& squad)
	{
		if (squad.templateSid == templateSid)
			found.push_back({entity, id.value});
	});
	std::sort(found.begin(), found.end(), [](const Caravan& a, const Caravan& b) { return a.id < b.id; });
	return found;
}

std::uint32_t units(const std::vector<state::InventoryEntry>& list)
{
	std::uint32_t total = 0;
	for (const auto& entry : list)
		total += entry.count;
	return total;
}

// ---- pack animals ----

void packAnimals(const kworld::LoadOrder& order)
{
	Simulation sim = newGame(order, false);
	const kdata::GameData& data = *sim.gameData();
	const grid::Rules rules(data);
	std::size_t animals = 0;
	std::size_t filled = 0;
	std::size_t withVendors = 0;
	ecsOf(sim).each([&](flecs::entity member, const state::CharacterInstance& character, const state::Inventory& inventory)
	{
		const kdata::Record* record = data.find(character.templateSid);
		test::require(record != nullptr, "animal record");
		const std::string backpack = trader::packBackpack(data, *record);
		test::require(!backpack.empty(), "only pack animals got an Inventory (" + character.templateSid + ")");
		++animals;
		// The animal's own grid is `inventory w` x `inventory h` (the Pack Beast 4 x 8), not the human 8 x 10.
		test::require(inventory.gridW == record->intOr("inventory w", 4) && inventory.gridH == record->intOr("inventory h", 8), "the animal's own grid");
		const state::InventoryEntry* worn = inventory.wornBackpack();
		test::require(worn != nullptr && worn->itemSid == backpack, "the backpack is worn, with no roll");
		const grid::Size bag = rules.bagSize(backpack);
		test::require(bag.w == data.find(backpack)->intOr("storage size width", 0)
				&& bag.h == data.find(backpack)->intOr("storage size height", 0) && bag.w > 0,
			"the backpack's grid is the item's storage size");
		for (const auto& entry : worn->contents)
			test::require(entry.x + rules.footprint(entry.itemSid).w <= bag.w && entry.y + rules.footprint(entry.itemSid).h <= bag.h, "the fill lies inside the bag");
		// The fill comes from the squad template's `vendors`; a squad without them (and an animal with no `inventory` list) stays empty.
		const flecs::entity squad = member.target<state::MemberOf>();
		const kdata::Record* squadRecord = data.find(squad.get<state::SquadInstance>()->templateSid);
		const bool vendors = squadRecord->findRefs("vendors") != nullptr && !squadRecord->findRefs("vendors")->entries.empty();
		if (vendors)
		{
			++withVendors;
			filled += units(worn->contents) > 0 ? 1 : 0;
			if (units(worn->contents) == 0)
				std::cerr << "unfilled: " << character.templateSid << " in " << squadRecord->stringID << " items " << inventory.items.size() << " bag " << worn->contents.size() << " id " << member.get<state::Id>()->value << " members " << membersOf(const_cast<Simulation&>(sim), squad).size() << "\n";
		} else {
			test::require(units(worn->contents) == 0, "no vendors, no fill");
		}
	});
	test::require(animals >= 6, "the default world has pack animals (" + std::to_string(animals) + ")");
	test::require(withVendors >= 4 && filled == withVendors, "every pack animal of a squad with vendors has the spawn fill (" + std::to_string(filled) + " of " + std::to_string(withVendors) + ")");

	// The Pack Beast is 4 x 8 and the Garru Backpack 20 x 20 in the merged data (kinspect item 54, live).
	const std::vector<Caravan> caravans = squadsOf(sim, "1161-gamedata.base");
	test::require(!caravans.empty(), "the Caravan squad exists");
	bool beast = false;
	for (const flecs::entity member : membersOf(sim, caravans.front().squad))
	{
		const state::Inventory* inventory = member.get<state::Inventory>();
		if (inventory == nullptr)
			continue;
		beast = true;
		const kdata::Record& animal = *data.find(member.get<state::CharacterInstance>()->templateSid);
		if (animal.stringID == "3999-gamedata.base")
			test::require(inventory->gridW == 4 && inventory->gridH == 8, "Pack Beast 4 x 8");
		test::require(inventory->gridW == animal.intOr("inventory w", 4) && inventory->gridH == animal.intOr("inventory h", 8), "the animal's grid");
		// Another pack animal in the merged data (a Pack Bull) wears a different bag; whatever the animal is, the bag is its record's.
		const std::string& worn = inventory->wornBackpack()->itemSid;
		test::require(worn == trader::packBackpack(data, *data.find(member.get<state::CharacterInstance>()->templateSid)), "the worn backpack is the Pack Beast record's");
		const grid::Size bag = rules.bagSize(worn);
		if (animal.stringID == "3999-gamedata.base")
			test::require(bag.w == 20 && bag.h == 20, "Pack Beast carries a 20 x 20 bag in the merged data (kinspect item 54)");
		test::require(bag.w > 0 && bag.h > 0, "the backpack has a grid");
		test::require(units(inventory->wornBackpack()->contents) > 0, "the Caravan's pack beast carries a vendor-list fill");
	}
	test::require(beast, "the Caravan has pack beasts");
	// TQ4: a Caravan is not `is trader`: no wallet, no timer, never refreshed.
	for (const Caravan& caravan : caravans)
		test::require(caravan.squad.get<state::TraderStock>() == nullptr, "a Caravan has no TraderStock");
	Simulation again = newGame(order, false);
	test::require(again.stateHash() == sim.stateHash(), "the pack animals are deterministic");
}

// ---- a refresh fills the members' backpacks ----

void refreshBackpacks(const kworld::LoadOrder& order)
{
	Simulation sim = newGame(order, true);
	const kdata::GameData& data = *sim.gameData();
	const grid::Rules rules(data);
	const Caravan caravan = squadsOf(sim, "1161-gamedata.base").front();
	std::vector<flecs::entity> carriers;
	for (const flecs::entity member : membersOf(sim, caravan.squad))
	{
		if (member.get<state::Inventory>() != nullptr)
			carriers.push_back(member);
	}
	test::require(carriers.size() >= 2, "two or more pack beasts");
	// Make it a trader by hand (the real Hiver caravans are, the plain Caravan is not): the refresh works on the backpacks.
	const double now = trader::gameHours(sim.clockTick());
	const trader::Outcome first = trader::refresh(caravan.squad, data, sim.tradeBook(), rules, 5, now, true);
	const state::TraderStock stock = *caravan.squad.get<state::TraderStock>();
	test::require(first.drawn > 0 && first.placed > 0 && stock.refreshes == 1 && stock.seeded, "the refresh drew and placed stock");
	test::require(std::fabs(stock.nextRefresh - (now + 24.0)) < 1e-9, "re-armed to now + P");
	test::require(stock.money >= trader::kMoneyFloor && stock.money <= trader::kMoneyCap, "a backpack trader's wallet is the clamped quarter of the stock");
	test::require(caravan.squad.get<state::Inventory>() == nullptr, "no squad store: the stock is in the backpacks");
	std::uint32_t total = 0;
	for (const flecs::entity carrier : carriers)
		total += units(carrier.get<state::Inventory>()->wornBackpack()->contents);
	test::require(total == first.placed, "every placed unit is in a member's backpack (" + std::to_string(total) + " vs " + std::to_string(first.placed) + ")");

	// A player sold something into a backpack: the next refresh wipes it.
	state::Inventory inventory = *carriers.back().get<state::Inventory>();
	state::InventoryEntry sold;
	sold.itemSid = data.byType(kworld::itemTypeByName("ITEM")).front()->stringID;
	sold.count = 1;
	sold.quality.maker = "player-sold";
	sold.quality.level = 0.321;
	sold.slot = state::Inventory::kCarried;
	std::vector<state::InventoryEntry> bag = inventory.wornBackpack()->contents;
	state::Inventory view;
	view.gridW = 20;
	view.gridH = 20;
	view.items = bag;
	grid::autoplace(view, rules, sold);
	inventory.wornBackpack()->contents = view.items;
	carriers.back().set<state::Inventory>(inventory);
	const auto hasSold = [&]()
	{
		for (const flecs::entity carrier : carriers)
		{
			for (const auto& entry : carrier.get<state::Inventory>()->wornBackpack()->contents)
			{
				if (entry.quality.maker == "player-sold")
					return true;
			}
		}
		return false;
	};
	test::require(hasSold(), "the sale is in the backpack");
	trader::refresh(caravan.squad, data, sim.tradeBook(), rules, 5, now + 24.0, false);
	test::require(!hasSold() && caravan.squad.get<state::TraderStock>()->refreshes == 2, "the second refresh wipes it");

	// The timer path: due and at full detail, the step refreshes the backpacks once.
	state::TraderStock due = *caravan.squad.get<state::TraderStock>();
	due.nextRefresh = trader::gameHours(sim.clockTick()) - 1.0;
	caravan.squad.set<state::TraderStock>(due);
	sim.run(trader::kIntervalTicks + 1);
	test::require(caravan.squad.get<state::TraderStock>()->refreshes == 3, "the timer refresh of a wandering trader ran");

	// Dead members hold no stock: with every carrier dead nothing refreshes and the timer is as it was.
	for (const flecs::entity carrier : carriers)
	{
		state::Fighter fighter;
		fighter.dead = true;
		carrier.set<state::Fighter>(fighter);
	}
	const state::TraderStock before = *caravan.squad.get<state::TraderStock>();
	const trader::Outcome none = trader::refresh(caravan.squad, data, sim.tradeBook(), rules, 5, now + 100.0, false);
	test::require(none.drawn == 0 && caravan.squad.get<state::TraderStock>()->refreshes == before.refreshes
			&& caravan.squad.get<state::TraderStock>()->nextRefresh == before.nextRefresh,
		"no living container: no refresh");
}

// ---- the circuit ----

struct Visit
{
	double arrivedAt = 0.0;
	double stay = 0.0;
	double leftAt = 0.0;
	std::uint64_t arriveTick = 0;
	std::uint64_t leaveTick = 0;
	double fromX = 0.0, fromZ = 0.0;
	flecs::entity_t town = 0;
};

void circuit(const kworld::LoadOrder& order)
{
	Simulation sim = newGame(order, false);
	enable(sim);
	{
		const BehaviourRegistry& registry = sim.ai()->registry();
		const Coverage coverage = registry.coverage(sim.world().ecs());
		std::size_t circuits = 0;
		for (const auto& [name, count] : coverage.byBehaviour)
			circuits += name == "trade_circuit" ? count : 0;
		test::require(circuits >= 1, "the coverage report shows trade_circuit squads (" + std::to_string(circuits) + ")");
	}
	while (sim.clockTick() % 20 != 0)
		sim.run(1);
	sim.run(20);
	const Caravan caravan = squadsOf(sim, "1161-gamedata.base").front();
	test::require(caravan.squad.get<state::TraderTrip>() != nullptr, "the caravan runs the trade circuit");
	const kdata::GameData& data = *sim.gameData();

	std::vector<Visit> visits;
	bool staying = false;
	flecs::entity_t lastTown = 0;
	bool forced = false;
	double forcedLeave = 0.0;
	// Up to 12 visits in 12 days: a pair of towns ~12 h apart locks the caravan into evening arrivals (every stay ends at night),
	// so a short run can see no plain stay (2026-11-04: the f86d2f2 populate re-rolled the caravan into one such pair).
	for (int step = 0; step < 52364 * 12 / 20 && visits.size() < 12; ++step)
	{
		sim.run(20);
		const state::TraderTrip trip = *caravan.squad.get<state::TraderTrip>();
		const std::uint64_t tick = sim.clockTick();
		const double hours = trader::gameHours(tick);
		// The caravan's step is on its own tick of the second (id % 20, Simulation::inSlice, k16.6.320): the latest one up to this tick.
		const double stepHours = trader::gameHours(tick - (tick % 20 + 20 - caravan.squad.get<state::Id>()->value % 20) % 20);
		const state::Position position = *caravan.squad.get<state::Position>();
		if (trip.staying && !staying)
		{
			Visit visit;
			visit.arrivedAt = trip.arrivedAt;
			visit.stay = trip.stayHours;
			visit.arriveTick = tick;
			visit.town = trip.town.id;
			test::require(std::fabs(trip.arrivedAt - stepHours) < 1e-9, "the arrival is stamped at the step that found it");
			test::require(trip.stayHours >= 5.0 && trip.stayHours <= 8.0, "the stay is U(5, 8)");
			const state::TownState& town = *flecs::entity(ecsOf(sim), trip.town.id).get<state::TownState>();
			test::require(std::hypot(position.x - town.x, position.z - town.z) <= UtilityGroupLayer::kHomeRadius + 1.0, "it stands in the town");
			const kdata::Record* record = data.find(town.activeSid);
			const int type = record != nullptr ? record->intOr("type", 2) : 2;
			test::require(type == 2 || type == 3 || type == 5 || type == 6, "a visited town is a TOWN, VILLAGE, SLAVE_CAMP or MILITARY");
			test::require(trip.town.id != lastTown || lastTown == 0, "it never picks the town it just left");
			if (!visits.empty())
			{
				// 25 u/s along the straight route.
				const Visit& previous = visits.back();
				const double distance = std::hypot(town.x - previous.fromX, town.z - previous.fromZ);
				const double seconds = static_cast<double>(tick - previous.leaveTick) * kTickSeconds;
				test::require(std::fabs(seconds - distance / trader::kWanderSpeed) <= 2.5,
					"the trip took " + std::to_string(seconds) + " s for " + std::to_string(distance) + " u (expected " + std::to_string(distance / trader::kWanderSpeed) + ")");
			}
			visits.push_back(visit);
			staying = true;
			// Once, push the end of a stay into the night: it must wait for 06:00.
			if (!forced && visits.size() == 2)
			{
				forced = true;
				state::TraderTrip moved = trip;
				const double dayStart = std::floor(hours / 24.0) * 24.0;
				moved.arrivedAt = dayStart + 24.0 + 1.0 - moved.stayHours; // the stay ends at 01:00 tomorrow
				caravan.squad.set<state::TraderTrip>(moved);
				visits.back().arrivedAt = moved.arrivedAt;
				forcedLeave = dayStart + 24.0 + 6.0;
				state::GroupAI ai = *caravan.squad.get<state::GroupAI>();
				ai.until = tick + 1; // look again at once
				caravan.squad.set<state::GroupAI>(ai);
			}
		} else if (!trip.staying && staying) {
			Visit& visit = visits.back();
			visit.leftAt = hours;
			visit.leaveTick = tick;
			visit.fromX = position.x;
			visit.fromZ = position.z;
			staying = false;
			lastTown = visit.town;
			test::require(trader::inLeaveWindow(hours), "it leaves only between 06:00 and 21:00 (left at hour " + std::to_string(hours) + ")");
			const double expected = trader::departureTime(visit.arrivedAt, visit.stay);
			test::require(hours >= expected - 1e-9 && hours - expected < 0.02, "it leaves at the end of the stay or the next 06:00 (left " + std::to_string(hours) + ", expected " + std::to_string(expected) + ")");
			if (visits.size() == 2)
				test::require(std::fabs(hours - forcedLeave) < 0.02, "a stay that ends at night waits for 06:00");
			test::require(trip.town.id != visit.town, "the next town is not the one it leaves");
		}
	}
	test::require(visits.size() >= 4, "it arrived at several towns (" + std::to_string(visits.size()) + ")");
	test::require(caravan.squad.get<state::TraderTrip>()->arrivals >= visits.size(), "arrivals are counted");
	bool plain = false;
	for (std::size_t i = 0; i + 1 < visits.size(); ++i)
		plain = plain || std::fabs(visits[i].leftAt - (visits[i].arrivedAt + visits[i].stay)) < 0.02;
	test::require(plain, "some stays end inside the window and the squad leaves at once");
}

// ---- death costs prosperity ----

void death(const kworld::LoadOrder& order)
{
	Simulation sim = newGame(order, false);
	enable(sim);
	const Caravan caravan = squadsOf(sim, "1161-gamedata.base").front();
	const std::string templateSid = caravan.squad.get<state::SquadInstance>()->templateSid;
	const flecs::entity faction(ecsOf(sim), caravan.squad.get<state::SquadInstance>()->faction.id);
	test::require(faction.is_valid(), "the Caravan has a faction");
	const kdata::GameData& data = *sim.gameData();
	const double cap = factionMaxProsperity(data, faction.get<state::FactionState>()->templateSid);
	test::require(factionProsperity(data, faction) == cap, "a faction starts at its cap");
	const std::size_t count = squadsOf(sim, templateSid).size();
	sim.enqueue(Command::removeSquad(state::Id{caravan.id}, state::Id{0}));
	sim.run(1);
	test::require(squadsOf(sim, templateSid).size() == count - 1, "the squad is gone");
	test::require(faction.get<state::FactionState>()->prosperityLost == 10.0 && factionProsperity(data, faction) == cap - 10.0, "prosperity fell by exactly 10");
	// Regenerates 0.1 per faction update, and nothing replaces the caravan (no slot timer for an ordinary squad).
	sim.run(3 * kTicksPerGameHour + 5);
	const double lost = faction.get<state::FactionState>()->prosperityLost;
	test::require(lost < 10.0 && lost >= 9.0, "prosperity regenerates a little (" + std::to_string(lost) + ")");
	sim.run(static_cast<std::uint64_t>(2 * kTicksPerGameDay));
	test::require(squadsOf(sim, templateSid).size() == count - 1, "no replacement timer for an ordinary squad");
}

// ---- save and load in the middle of a stay ----

void saveLoad(const kworld::LoadOrder& order)
{
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_trader_circuit_test.ksav";
	Simulation straight = newGame(order, false);
	enable(straight);
	while (straight.clockTick() % 20 != 0)
		straight.run(1);
	const Caravan caravan = squadsOf(straight, "1161-gamedata.base").front();
	bool found = false;
	for (int step = 0; step < 52364 * 3 / 20 && !found; ++step)
	{
		straight.run(20);
		const state::TraderTrip* trip = caravan.squad.get<state::TraderTrip>();
		found = trip != nullptr && trip->staying && trip->arrivals >= 1 && trader::gameHours(straight.clockTick()) < trip->arrivedAt + trip->stayHours - 3.0;
	}
	test::require(found, "a caravan in the middle of a stay");
	const std::uint64_t split = straight.stateHash();
	straight.saveGame(path);
	const std::uint64_t after = 6 * kTicksPerGameHour + 41;
	straight.run(after);
	test::require(caravan.squad.get<state::TraderTrip>()->arrivals >= 1, "still circuiting");
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == split, "load gives the saved hash");
	enable(loaded);
	loaded.run(after);
	test::require(loaded.stateHash() == straight.stateHash(), "load and continue gives the straight run's hash");
	std::remove(path.c_str());
}

} // namespace

int main()
{
	try
	{
		pureRules();
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		packAnimals(order);
		refreshBackpacks(order);
		circuit(order);
		death(order);
		saveLoad(order);
		std::cout << "ksim_trader_circuit: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_trader_circuit: FAILED: " << e.what() << "\n";
		return 1;
	}
}
