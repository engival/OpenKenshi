// The supply board and its delivery caravans (TOWN_LIFE_SLICE 7.4, phase T4c). The board and caravan kinds are the v69 layout, so
// this phase takes no save version. This pins:
//  - the board: a line over its target is an offer, a line under it a need, a live claim expires kBoardEntryHours after its post and is
//    re-posted from the stock, and a delivery on the road keeps its destination's shortfall reserved (not re-posted);
//  - a match: a trade-circuit squad staying in the surplus town loads the goods (the source stock loses them at the match) and the T2
//    circuit walks the order to the needing town (its own TravelTo and arrival stamp), where the unload moves the stock;
//  - a caravan killed on the road loses the goods: the destination stays short and its need is re-posted;
//  - a stale order (T4d, save v73's `CaravanOrder::postedAt`): past kOrderMaxHours the goods return to the source (or are lost with a
//    vanished source), the order is dropped and both boards re-post — a claim can never lie on the road forever;
//  - `mode = original` has no board, no order and no T4 state.
//
// OURS where 7.4 is open: the delivery caravan is the T2 trade-circuit squad (7.4's overlay template family is not built), the surplus
// is the town's stock over its target (T4a/T4b feed the stock directly; `EconomyProducer::buffer` stays unused), and one delivery
// carries at most kCaravanCapacity. The killed caravan is removed by the RemoveSquad command (the same path a real death takes); no
// player character is ordered to fight it.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/economy/economy.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
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

Simulation newGame(EconomyMode mode)
{
	SpawnOptions options;
	options.seed = 12;
	options.wildernessByDensity = false;
	options.economy = mode;
	RegionConfig regions;
	regions.workerThreads = 1;
	return Simulation::newGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), options, nullptr, regions);
}

void enable(Simulation& sim)
{
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<StraightNavigator>();
	config.factions = FactionBook::build(sim.gameData());
	sim.enableAi(std::move(config));
}

struct Town
{
	flecs::entity entity;
	state::TownState state;
};

std::vector<Town> townsOf(Simulation& sim)
{
	std::vector<Town> towns;
	ecsOf(sim).each([&](flecs::entity entity, const state::TownState& town) { towns.push_back({entity, town}); });
	std::sort(towns.begin(), towns.end(), [](const Town& left, const Town& right) { return left.entity.id() < right.entity.id(); });
	return towns;
}

// The closest pair of towns: a short delivery keeps the walk small.
std::pair<std::size_t, std::size_t> closestPair(const std::vector<Town>& towns)
{
	std::pair<std::size_t, std::size_t> best{0, 1};
	double nearest = 1e30;
	for (std::size_t i = 0; i < towns.size(); ++i)
		for (std::size_t j = i + 1; j < towns.size(); ++j)
		{
			const double distance = (towns[i].state.x - towns[j].state.x) * (towns[i].state.x - towns[j].state.x)
				+ (towns[i].state.z - towns[j].state.z) * (towns[i].state.z - towns[j].state.z);
			if (distance < nearest)
			{
				nearest = distance;
				best = {i, j};
			}
		}
	return best;
}

std::vector<flecs::entity> caravansOf(Simulation& sim)
{
	std::vector<std::pair<std::uint64_t, flecs::entity>> found;
	ecsOf(sim).each([&](flecs::entity entity, const state::SquadInstance& squad)
	{
		if (squad.templateSid != "1161-gamedata.base")
			return;
		const state::Id* id = entity.get<state::Id>();
		found.emplace_back(id != nullptr ? id->value : entity.id(), entity);
	});
	std::sort(found.begin(), found.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
	std::vector<flecs::entity> out;
	for (const auto& [id, entity] : found)
		out.push_back(entity);
	return out;
}

// A town stock with one line (the fixture's item; no real town stocks it, so no other board posts it).
void setStock(flecs::entity town, const std::string& item, double amount, double target)
{
	state::TownStock& stock = *town.get_mut<state::TownStock>();
	stock.lines.clear();
	stock.lines.push_back(state::StockLine{item, amount, target});
	stock.prosperity = economy::prosperity(stock);
}

double amountOf(flecs::entity town, const std::string& item)
{
	const state::StockLine* line = economy::findLine(*town.get<state::TownStock>(), item);
	return line != nullptr ? line->amount : -1.0;
}

const state::BoardEntry* boardEntry(flecs::entity town, std::uint8_t kind, const std::string& item)
{
	return economy::findEntry(*town.get<state::SupplyBoard>(), kind, item);
}

// Fixture setup: the squad stands in the town and its circuit stamps a stay of `stay` hours from now (no command orders the squad).
void armStay(Simulation& sim, flecs::entity squad, const Town& town, double stay)
{
	squad.set<state::Position>(state::Position{town.state.x, town.state.z});
	state::GroupAI ai = squad.get<state::GroupAI>() != nullptr ? *squad.get<state::GroupAI>() : state::GroupAI{};
	ai.goal = state::GoalKind::None;
	ai.route.clear();
	ai.length = 0.0;
	ai.along = 0.0;
	ai.until = 0;
	ai.since = 0;
	squad.set<state::GroupAI>(ai);
	state::TraderTrip trip;
	trip.town = state::EntityRef{town.entity.id()};
	trip.staying = true;
	trip.arrivedAt = trader::gameHours(sim.clockTick());
	trip.stayHours = stay;
	squad.set<state::TraderTrip>(trip);
}

// A trade-circuit squad with no AI step: the fixture's own TraderTrip (matching reads it, nothing moves it).
void markStaying(Simulation& sim, flecs::entity squad, const Town& town, double stay)
{
	state::TraderTrip trip;
	trip.town = state::EntityRef{town.entity.id()};
	trip.staying = true;
	trip.arrivedAt = trader::gameHours(sim.clockTick());
	trip.stayHours = stay;
	squad.set<state::TraderTrip>(trip);
}

flecs::entity orderHolder(Simulation& sim, const std::string& item)
{
	flecs::entity holder;
	ecsOf(sim).each([&](flecs::entity entity, const state::CaravanOrder& order)
	{
		if (order.itemSid == item)
			holder = entity;
	});
	return holder;
}

// ---- the pure rules ----

void pureRules()
{
	const state::StockLine surplus{"a", 150.0, 100.0};
	const state::StockLine shortfall{"b", 50.0, 100.0};
	test::require(economy::findEntry(state::SupplyBoard{}, state::kBoardOffer, "a") == nullptr, "an empty board has no entry");
	test::require(std::fabs(economy::deliveryAmount(surplus, 50.0) - 50.0) < 1e-9, "a delivery takes the smaller side");
	test::require(std::fabs(economy::deliveryAmount(surplus, 10.0) - 10.0) < 1e-9, "the need caps it");
	test::require(std::fabs(economy::deliveryAmount(surplus, 500.0) - 50.0) < 1e-9, "the surplus caps it");
	test::require(std::fabs(economy::deliveryAmount(state::StockLine{"a", 400.0, 100.0}, 500.0) - economy::kCaravanCapacity) < 1e-9,
		"the caravan's capacity caps it");
	test::require(economy::deliveryAmount(state::StockLine{"a", 100.0, 100.0}, 50.0) == 0.0, "no surplus, no delivery");

	state::TownStock stock;
	stock.lines = {state::StockLine{"a", 150.0, 100.0}, state::StockLine{"b", 50.0, 100.0}, state::StockLine{"c", 100.0, 100.0}};
	state::SupplyBoard board;
	economy::postBoard(board, 7, stock, 100.0, {});
	test::require(board.entries.size() == 2, "two entries: the surplus and the shortfall (" + std::to_string(board.entries.size()) + ")");
	const state::BoardEntry* offer = economy::findEntry(board, state::kBoardOffer, "a");
	const state::BoardEntry* need = economy::findEntry(board, state::kBoardNeed, "b");
	test::require(offer != nullptr && offer->amount == 50.0 && offer->from.id == 7 && offer->postedAt == 100.0
			&& offer->expires == 100.0 + economy::kBoardEntryHours,
		"the surplus offers the excess");
	test::require(need != nullptr && need->amount == 50.0, "the shortfall needs the rest");
	test::require(economy::findEntry(board, state::kBoardOffer, "c") == nullptr && economy::findEntry(board, state::kBoardNeed, "c") == nullptr,
		"a line at its target posts nothing");
	// An inbound delivery covers the shortfall: the need is not posted (the destination reservation).
	economy::postBoard(board, 7, stock, 100.5, {{"b", 50.0}});
	test::require(economy::findEntry(board, state::kBoardNeed, "b") == nullptr, "a reserved shortfall is not posted");
	test::require(economy::findEntry(board, state::kBoardNeed, "b") == nullptr && board.entries.size() == 1, "only the offer remains");
	// The claim lives kBoardEntryHours, then it is re-posted from the stock with a fresh clock.
	economy::postBoard(board, 7, stock, 100.0 + economy::kBoardEntryHours + 0.5, {{"b", 25.0}});
	offer = economy::findEntry(board, state::kBoardOffer, "a");
	need = economy::findEntry(board, state::kBoardNeed, "b");
	test::require(offer != nullptr && offer->postedAt == 100.0 + economy::kBoardEntryHours + 0.5, "the expired offer is re-posted");
	test::require(need != nullptr && need->amount == 25.0, "the half-reserved need posts the other half");
	// The board stays sorted by (kind, itemSid): offers before needs, items ascending.
	test::require(board.entries[0].kind == state::kBoardOffer && board.entries[1].kind == state::kBoardNeed, "offers sort before needs");
	std::printf("pure: posting, reservation and expiry\n");
}

// ---- a delivery arrives and moves the stock ----

void delivery()
{
	Simulation sim = newGame(EconomyMode::Real);
	enable(sim);
	const std::vector<Town> towns = townsOf(sim);
	test::require(towns.size() > 2, "the world has towns");
	const auto [a, b] = closestPair(towns);
	flecs::entity src = towns[a].entity;
	flecs::entity dst = towns[b].entity;
	const double distance = std::hypot(towns[a].state.x - towns[b].state.x, towns[a].state.z - towns[b].state.z);
	test::require(!caravansOf(sim).empty(), "the world has a Caravan squad");
	armStay(sim, caravansOf(sim).front(), towns[a], 3.0);
	setStock(src, "test-x", 150.0, 100.0);
	setStock(dst, "test-x", 50.0, 100.0);

	flecs::entity courier;
	for (int step = 0; step < 6000 && !courier.is_valid(); ++step)
	{
		sim.run(20);
		courier = orderHolder(sim, "test-x");
	}
	test::require(courier.is_valid(), "no delivery was posted");
	const state::CaravanOrder order = *courier.get<state::CaravanOrder>();
	test::require(order.from.id == src.id() && order.to.id == dst.id(), "the order runs from the surplus town to the short one");
	test::require(std::fabs(order.amount - 50.0) < 1e-9, "it carries the shortfall (" + std::to_string(order.amount) + ")");
	test::require(std::fabs(amountOf(src, "test-x") - 100.0) < 1e-9, "the goods left the source stock");
	test::require(std::fabs(amountOf(dst, "test-x") - 50.0) < 1e-9, "nothing has arrived yet");
	test::require(boardEntry(src, state::kBoardOffer, "test-x") == nullptr, "the offer is claimed");
	test::require(boardEntry(dst, state::kBoardNeed, "test-x") == nullptr, "the need is claimed");
	// The destination half of the reservation: an hour later the shortfall is still not re-posted (the goods are on the road).
	sim.run(kTicksPerGameHour + 5);
	test::require(boardEntry(dst, state::kBoardNeed, "test-x") == nullptr, "an in-transit delivery keeps the need reserved");
	test::require(std::fabs(amountOf(dst, "test-x") - 50.0) < 1e-9, "still nothing at the destination");
	// The T2 circuit walks it: the caravan's own TravelTo, its arrival stamp, its 25 u/s.
	std::uint64_t departure = 0;
	for (int step = 0; step < 3 * 24 * 60 && !departure; ++step)
	{
		sim.run(20);
		const state::TraderTrip* trip = courier.get<state::TraderTrip>();
		if (trip != nullptr && !trip->staying && trip->town.id == dst.id())
			departure = sim.clockTick();
	}
	test::require(departure != 0, "the caravan never left for the needing town");
	for (int step = 0; step < 3 * 24 * 60; ++step)
	{
		const state::TraderTrip* trip = courier.get<state::TraderTrip>();
		if (trip != nullptr && trip->staying && trip->town.id == dst.id())
			break;
		sim.run(20);
	}
	const state::TraderTrip* trip = courier.get<state::TraderTrip>();
	test::require(trip != nullptr && trip->staying && trip->town.id == dst.id(), "the delivery did not arrive");
	const state::Position position = *courier.get<state::Position>();
	test::require(std::hypot(position.x - towns[b].state.x, position.z - towns[b].state.z)
			<= UtilityGroupLayer::kHomeRadius + 1.0, "the caravan does not stand in the needing town");
	const double seconds = static_cast<double>(sim.clockTick() - departure) * kTickSeconds;
	// The unload at the next economy tick.
	sim.run(2 * kTicksPerGameHour + 10);
	test::require(courier.get<state::CaravanOrder>() == nullptr, "the order is not finished");
	test::require(std::fabs(amountOf(dst, "test-x") - 100.0) < 1e-6,
		"the delivered stock is not at its target (" + std::to_string(amountOf(dst, "test-x")) + ")");
	std::printf("delivery: %g u over %g u in %.0f s, the town moved 50 -> 100\n", order.amount, distance, seconds);
}

// ---- a killed caravan leaves the shortfall; an expired claim is re-posted ----

void lossAndExpiry()
{
	Simulation sim = newGame(EconomyMode::Real);
	const std::vector<Town> towns = townsOf(sim);
	const auto [a, b] = closestPair(towns);
	flecs::entity src = towns[a].entity;
	flecs::entity dst = towns[b].entity;
	std::size_t c = 0;
	while (c == a || c == b)
		++c;
	flecs::entity third = towns[c].entity;
	// No AI in this fixture: the caravan's stay is the fixture's own TraderTrip, so only it can be matched.
	const std::vector<flecs::entity> caravans = caravansOf(sim);
	test::require(!caravans.empty(), "the world has a Caravan squad");
	const flecs::entity caravan = caravans.front();
	markStaying(sim, caravan, towns[a], 8.0);
	setStock(src, "test-x", 150.0, 100.0);
	setStock(dst, "test-x", 50.0, 100.0);

	flecs::entity courier;
	for (int step = 0; step < 6000 && !courier.is_valid(); ++step)
	{
		sim.run(20);
		courier = orderHolder(sim, "test-x");
	}
	test::require(courier.is_valid(), "the board did not post the delivery");
	test::require(std::fabs(amountOf(src, "test-x") - 100.0) < 1e-9, "the goods left the source");
	test::require(std::fabs(amountOf(dst, "test-x") - 50.0) < 1e-9, "nothing arrived");
	// Killed on the road: a direct effect (RemoveSquad, the path a real death takes).
	const state::Id* id = courier.get<state::Id>();
	test::require(id != nullptr, "the caravan has an Id");
	sim.enqueue(Command::removeSquad(state::Id{id->value}, state::Id{0}));
	sim.run(1);
	test::require(!orderHolder(sim, "test-x").is_valid(), "the dead caravan still holds an order");
	// The goods are lost with it: the destination is still short and its need is re-posted, the source does not get them back.
	sim.run(kTicksPerGameHour + 10);
	test::require(std::fabs(amountOf(dst, "test-x") - 50.0) < 1e-9,
		"the destination stock moved without a delivery (" + std::to_string(amountOf(dst, "test-x")) + ")");
	const state::BoardEntry* need = boardEntry(dst, state::kBoardNeed, "test-x");
	test::require(need != nullptr && std::fabs(need->amount - 50.0) < 1e-9, "the lost delivery did not re-post the shortfall");
	test::require(std::fabs(amountOf(src, "test-x") - 100.0) < 1e-9, "the source recovered the lost goods");

	// A surplus nobody carries: the claim expires and is re-posted from the stock.
	setStock(third, "test-y", 150.0, 100.0);
	sim.run(kTicksPerGameHour + 10);
	const state::BoardEntry* offer = boardEntry(third, state::kBoardOffer, "test-y");
	test::require(offer != nullptr && std::fabs(offer->amount - 50.0) < 1e-9, "the surplus is not offered");
	const double posted = offer->postedAt;
	sim.run((economy::kBoardEntryHours + 1.0) * static_cast<double>(kTicksPerGameHour) + 10);
	offer = boardEntry(third, state::kBoardOffer, "test-y");
	test::require(offer != nullptr && std::fabs(offer->amount - 50.0) < 1e-9 && offer->postedAt > posted,
		"an expired claim was not re-posted (" + std::to_string(posted) + " -> "
			+ (offer != nullptr ? std::to_string(offer->postedAt) : std::string("none")) + ")");

	// The open board is saved with the town (the v69 SupplyBoard kind); a load keeps it and the run continues to the same hash.
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_economy_caravan_test.ksav";
	const std::uint64_t split = sim.stateHash();
	sim.saveGame(path);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == split, "load gives the saved hash");
	test::require(economy::findEntry(*flecs::entity(ecsOf(loaded), third.id()).get<state::SupplyBoard>(), state::kBoardOffer, "test-y")
			!= nullptr,
		"the board did not survive the load");
	sim.run(2 * kTicksPerGameHour);
	loaded.run(2 * kTicksPerGameHour);
	test::require(loaded.stateHash() == sim.stateHash(), "load and continue gives the straight run's hash");
	std::remove(path.c_str());
	std::printf("loss: the lost delivery left the town short and re-posted it; an expired claim re-posted\n");
}

// ---- a stale order is released (T4d) ----

void staleOrder()
{
	Simulation sim = newGame(EconomyMode::Real);
	const std::vector<Town> towns = townsOf(sim);
	const auto [a, b] = closestPair(towns);
	flecs::entity src = towns[a].entity;
	flecs::entity dst = towns[b].entity;
	const flecs::entity caravan = caravansOf(sim).front();
	markStaying(sim, caravan, towns[a], 8.0);
	setStock(src, "test-x", 150.0, 100.0);
	setStock(dst, "test-x", 50.0, 100.0);

	flecs::entity courier;
	for (int step = 0; step < 6000 && !courier.is_valid(); ++step)
	{
		sim.run(20);
		courier = orderHolder(sim, "test-x");
	}
	test::require(courier.is_valid(), "the board did not post the delivery");
	const state::CaravanOrder held = *courier.get<state::CaravanOrder>();
	// The age clock saves with the order (v73, the T4d field).
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_economy_caravan_test.ksav";
	const std::uint64_t split = sim.stateHash();
	sim.saveGame(path);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == split, "load gives the saved hash");
	state::CaravanOrder reloaded;
	bool found = false;
	ecsOf(loaded).each([&](flecs::entity, const state::CaravanOrder& order)
	{
		if (order.itemSid == "test-x")
		{
			reloaded = order;
			found = true;
		}
	});
	test::require(found && reloaded.postedAt == held.postedAt && reloaded.amount == held.amount,
		"the order's age did not survive the load");
	std::remove(path.c_str());

	// The carrier stops wandering (the fixture's own state; no AI steps in this world): the order is all that is left. Past the limit
	// the goods return to the source, the order is dropped and both boards re-post.
	courier.remove<state::TraderTrip>();
	sim.run((economy::kOrderMaxHours + 2.0) * static_cast<double>(kTicksPerGameHour) + 20);
	test::require(!orderHolder(sim, "test-x").is_valid(), "a stale order was never released");
	test::require(std::fabs(amountOf(src, "test-x") - 150.0) < 1e-9,
		"the goods did not return to the source (" + std::to_string(amountOf(src, "test-x")) + ")");
	test::require(std::fabs(amountOf(dst, "test-x") - 50.0) < 1e-9, "the destination moved without a delivery");
	const state::BoardEntry* offer = boardEntry(src, state::kBoardOffer, "test-x");
	const state::BoardEntry* need = boardEntry(dst, state::kBoardNeed, "test-x");
	test::require(offer != nullptr && std::fabs(offer->amount - 50.0) < 1e-9, "the returned surplus is not re-offered");
	test::require(need != nullptr && std::fabs(need->amount - 50.0) < 1e-9, "the released shortfall is not re-posted");

	// A second order whose source is gone: the stale release loses the goods instead of returning them.
	markStaying(sim, caravan, towns[a], 8.0);
	setStock(src, "test-x", 150.0, 100.0);
	setStock(dst, "test-x", 50.0, 100.0);
	courier = flecs::entity();
	for (int step = 0; step < 6000 && !courier.is_valid(); ++step)
	{
		sim.run(20);
		courier = orderHolder(sim, "test-x");
	}
	test::require(courier.is_valid(), "the board did not post the second delivery");
	courier.get_mut<state::CaravanOrder>()->from.id = 0; // the source town is not there
	courier.remove<state::TraderTrip>();
	sim.run((economy::kOrderMaxHours + 2.0) * static_cast<double>(kTicksPerGameHour) + 20);
	test::require(!orderHolder(sim, "test-x").is_valid(), "the sourceless stale order was never released");
	test::require(std::fabs(amountOf(src, "test-x") - 100.0) < 1e-9, "a vanished source got the goods back");
	test::require(std::fabs(amountOf(dst, "test-x") - 50.0) < 1e-9, "the destination moved without a delivery");
	test::require(boardEntry(dst, state::kBoardNeed, "test-x") != nullptr, "the released shortfall is not re-posted");
	std::printf("stale: %g h releases an order, goods home (source gone: lost), the boards re-post\n", economy::kOrderMaxHours);
}

// ---- mode = original ----

void original()
{
	Simulation sim = newGame(EconomyMode::Original);
	sim.run(kTicksPerGameHour * 2);
	flecs::world& ecs = ecsOf(sim);
	test::require(ecs.get<state::EconomySettings>() == nullptr, "original mode created the economy singleton");
	std::size_t t4 = 0;
	ecs.each([&](flecs::entity, const state::TownStock&) { ++t4; });
	ecs.each([&](flecs::entity, const state::SupplyBoard&) { ++t4; });
	ecs.each([&](flecs::entity, const state::CaravanOrder&) { ++t4; });
	test::require(t4 == 0, "original mode created T4 state");
	std::printf("original: no board, no order and no T4 state\n");
}

} // namespace

int main()
{
	try
	{
		pureRules();
		delivery();
		lossAndExpiry();
		staleOrder();
		original();
		std::cout << "ksim_economy_caravan: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_economy_caravan: FAILED: " << e.what() << "\n";
		return 1;
	}
}
