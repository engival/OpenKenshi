// The economy's Lua hooks (TOWN_LIFE_SLICE 7.8 / 7.13, task AV; ARCHITECTURE.md "Behaviour layer: Lua"). This pins:
//  - economy.price: a script's factor replaces stockFactor's k / lo / hi rule (the quote moves), the engine rule answers when the script
//    has no price and for a call the script fails (logged, the engine rule for that call only), and the per-quote cost is measured;
//  - economy.demand: the script's target table moves what a town posts and what a delivery caravan orders;
//  - economy.on_delivery / economy.on_loss: each fires once, with the order's item, amount and ends;
//  - `original` mode calls no hook, and a world with no hooks attached is today's engine, bit for bit.
//
// The script is a process setting (Simulation::setEconomyHooks), never saved; the test's script logs its events through ksim.log and the
// hook set's log sink collects them.

#include "ksim/core/clock.hpp"
#include "ksim/core/command.hpp"
#include "ksim/economy/economy.hpp"
#include "ksim/economy/economy_script.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trade.hpp"
#include "ksim/economy/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
using ksim::economy::HookSetup;
using ksim::economy::Hooks;

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

// The town's state::Id (what a hook sees), else its raw entity id.
std::uint64_t townId(flecs::entity town)
{
	const state::Id* id = town.get<state::Id>();
	return id != nullptr ? id->value : static_cast<std::uint64_t>(town.id());
}

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

// Loads a script with the given body and collects what it (and the hook set) says.
std::shared_ptr<Hooks> script(const std::string& body, std::vector<std::string>& log, std::uint64_t seed = 12)
{
	HookSetup setup;
	setup.name = "test-hooks.lua";
	setup.source = body;
	setup.log = [&log](std::string_view message) { log.emplace_back(message); };
	return Hooks::load(setup, seed);
}

bool logged(const std::vector<std::string>& log, std::string_view needle)
{
	return std::any_of(log.begin(), log.end(), [needle](const std::string& line) { return line.find(needle) != std::string::npos; });
}

// ---- economy.price ----

void priceHook()
{
	Simulation sim = newGame(EconomyMode::Real);
	const kdata::GameData& data = *sim.gameData();
	const trade::Book engineBook(data, 12);
	const kdata::Record* item = nullptr;
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("ITEM")))
		if (record->intOr("value", 0) > 0 && engineBook.basePrice(*record) > 0)
		{
			item = record;
			break;
		}
	test::require(item != nullptr, "no priced item in the data");
	const state::Inventory::Entry entry{item->stringID, 1, 0, {}};
	state::TownStock stock;
	stock.lines = {{item->stringID, 50.0, 100.0}}; // half the target: the engine's factor is 2
	trade::Terms terms;
	terms.stock = &stock;
	terms.townId = 4242;
	trade::Terms plainTerms = terms;
	plainTerms.stock = nullptr; // the same chain and town factor, with no stock's T4 factor
	const trade::Quote plain = engineBook.quote(plainTerms, entry);
	const trade::Quote engine = engineBook.quote(terms, entry);
	test::require(engine.buy == 2 * plain.buy, "the engine rule doubles a half-stocked town's price");

	// The script replaces the k / lo / hi rule: a flat 3 instead of the engine's 2.
	std::vector<std::string> log;
	sim.setEconomyHooks(script("return { api = 0, price = function(q) return 3.0 end }\n", log));
	test::require(sim.economyHooks() != nullptr && sim.economyHooks()->hasPrice(), "the price hook did not load");
	terms.hooks = sim.economyHooks();
	const trade::Quote scripted = sim.tradeBook().quote(terms, entry);
	const auto expect = [](std::int32_t base) { return static_cast<std::int32_t>(static_cast<double>(base) * 3.0 + 0.5); };
	test::require(scripted.buy == expect(plain.buy) && scripted.sell == expect(plain.sell),
		"the script's price is not the chain x 3 (" + std::to_string(scripted.buy) + "/" + std::to_string(scripted.sell) + ")");
	test::require(scripted.buy != engine.buy, "the script did not replace the engine factor");
	test::require(log.empty(), "a working price hook logged something");

	// The query the script sees: the item, the town, the stock and the target.
	std::vector<std::string> seen;
	sim.setEconomyHooks(script(
		"return { api = 0, price = function(q) ksim.log(string.format('%s|%d|%g|%g', q.item, q.town, q.stock, q.target)); return 2 end }\n", seen));
	terms.hooks = sim.economyHooks();
	const trade::Quote echoes = sim.tradeBook().quote(terms, entry);
	test::require(echoes.buy == engine.buy, "a script returning the engine's 2 differs from it");
	test::require(!seen.empty() && seen.front().find(item->stringID + "|4242|50|100") != std::string::npos,
		"the price hook's query is not {item, town, stock, target}: " + (seen.empty() ? std::string("nothing") : seen.front()));

	// Hot path: one quote with the script against the engine's (the same book; a hand-built Terms with no hooks is the engine's).
	trade::Terms scriptedTerms = terms;
	trade::Terms engineTerms = terms;
	engineTerms.hooks = nullptr;
	const auto measure = [&](const trade::Terms& t) {
		constexpr int n = 20000;
		std::int32_t sink = 0;
		const auto start = std::chrono::steady_clock::now();
		for (int i = 0; i < n; ++i)
			sink += sim.tradeBook().quote(t, entry).buy;
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / n;
		return std::pair{us, sink};
	};
	const auto [engineUs, engineSink] = measure(engineTerms);
	const auto [scriptedUs, scriptedSink] = measure(scriptedTerms);
	test::require(engineSink > 0 && scriptedSink > 0, "the measurement did nothing");
	std::printf("price: engine %d/%d, script 3 -> %d/%d, engine 2 -> %d/%d; %.3f us/quote engine, %.3f us/quote script\n",
		engine.buy, engine.sell, scripted.buy, scripted.sell, echoes.buy, echoes.sell, engineUs, scriptedUs);
}

// ---- economy.demand, on_delivery and on_loss ----

const char* kDemandScript = R"(local DST = %DST%
return {
	api = 0,
	demand = function(q)
		if q.town == DST then return { { item = "test-x", target = 300.0 } } end
		return {}
	end,
	on_delivery = function(o) ksim.log("delivery " .. o.item .. " " .. o.amount .. " " .. o.from .. " " .. o.to) end,
	on_loss = function(o) ksim.log("loss " .. o.item .. " " .. o.amount) end,
}
)";

// Waits for a delivery order on `item` and returns its carrier.
flecs::entity waitOrder(Simulation& sim, const std::string& item)
{
	flecs::entity courier;
	for (int step = 0; step < 4000 && !courier.is_valid(); ++step)
	{
		sim.run(20);
		courier = orderHolder(sim, item);
	}
	return courier;
}

struct Fixture
{
	Simulation sim;
	std::vector<Town> towns;
	flecs::entity src;
	flecs::entity dst;
	std::uint64_t dstId = 0;

	explicit Fixture(EconomyMode mode = EconomyMode::Real)
		: sim(newGame(mode))
	{
		towns = townsOf(sim);
		test::require(towns.size() > 2, "the world has towns");
		const auto [a, b] = closestPair(towns);
		src = towns[a].entity;
		dst = towns[b].entity;
		dstId = townId(dst);
		test::require(!caravansOf(sim).empty(), "the world has a Caravan squad");
	}
};

void demandAndEvents()
{
	// The engine: the destination's need is 50 (target 100, stock 50), so the order carries 50 of the 300 the source offers.
	{
		Fixture f;
		markStaying(f.sim, caravansOf(f.sim).front(), f.towns[closestPair(f.towns).first], 8.0);
		setStock(f.src, "test-x", 400.0, 100.0);
		setStock(f.dst, "test-x", 50.0, 100.0);
		const flecs::entity courier = waitOrder(f.sim, "test-x");
		test::require(courier.is_valid(), "the engine posted no delivery");
		test::require(std::fabs(courier.get<state::CaravanOrder>()->amount - 50.0) < 1e-9,
			"the engine's order is not the need (" + std::to_string(courier.get<state::CaravanOrder>()->amount) + ")");
	}

	// economy.demand raises the destination's target to 300: its need is 250, so the caravan (capacity 200) carries 200.
	Fixture f;
	std::string body = kDemandScript;
	body.replace(body.find("%DST%"), 5, std::to_string(f.dstId));
	std::vector<std::string> log;
	f.sim.setEconomyHooks(script(body, log));
	test::require(f.sim.economyHooks()->hasDemand(), "the demand hook did not load");
	markStaying(f.sim, caravansOf(f.sim).front(), f.towns[closestPair(f.towns).first], 8.0);
	setStock(f.src, "test-x", 400.0, 100.0);
	setStock(f.dst, "test-x", 50.0, 100.0);
	const flecs::entity courier = waitOrder(f.sim, "test-x");
	test::require(courier.is_valid(), "the demand script posted no delivery");
	const state::CaravanOrder order = *courier.get<state::CaravanOrder>();
	test::require(std::fabs(order.amount - 100.0) < 1e-9,
		"the demand script did not change the order (" + std::to_string(order.amount) + ")");
	test::require(std::fabs(economy::findLine(*f.dst.get<state::TownStock>(), "test-x")->target - 300.0) < 1e-9,
		"the script's target is not on the town's line");

	// The delivery event: the caravan at its destination, one economy tick, exactly one on_delivery with the order.
	const auto destination = std::find_if(f.towns.begin(), f.towns.end(), [&](const Town& town) { return town.entity.id() == order.to.id; });
	test::require(destination != f.towns.end(), "the order's destination is not a town");
	markStaying(f.sim, courier, *destination, 8.0);
	courier.get_mut<state::CaravanOrder>()->phase = state::kCaravanGo;
	f.sim.run(static_cast<int>(kTicksPerGameHour) + 10);
	test::require(logged(log, "delivery test-x 100"), "on_delivery did not fire with the order");
	{
		std::string all;
		for (const std::string& line : log)
			all += "[" + line + "] ";
		test::require(std::count_if(log.begin(), log.end(), [](const std::string& line) { return line.find("delivery ") != std::string::npos; }) == 1,
			"on_delivery fired more than once for one order: " + all);
	}
	test::require(std::fabs(amountOf(f.dst, "test-x") - 150.0) < 1e-9, "the delivery did not move the stock");

	// The loss event: a second order on the same caravan, its carrier removed (the path a real death takes), exactly one on_loss.
	const auto source = std::find_if(f.towns.begin(), f.towns.end(), [&](const Town& town) { return town.entity.id() == order.from.id; });
	test::require(source != f.towns.end(), "the order's source is not a town");
	markStaying(f.sim, courier, *source, 8.0);
	setStock(f.src, "test-x", 400.0, 100.0);
	setStock(f.dst, "test-x", 50.0, 100.0);
	const flecs::entity next = waitOrder(f.sim, "test-x");
	test::require(next.is_valid(), "no second order formed");
	const state::Id* id = next.get<state::Id>();
	test::require(id != nullptr, "the carrier has no Id");
	f.sim.enqueue(Command::removeSquad(state::Id{id->value}, state::Id{0}));
	f.sim.run(1);
	test::require(logged(log, "loss test-x 100"), "on_loss did not fire with the order");
	test::require(std::count_if(log.begin(), log.end(), [](const std::string& line) { return line.find("loss test-x ") != std::string::npos; }) == 1,
		"on_loss fired more than once for one order");
	std::printf("demand: the script's need made the order %g (the engine's 50); delivery and loss each fired once\n", order.amount);
}

// ---- the shipped default writes the engine's rule ----

void defaultScript()
{
	std::vector<std::string> log;
	const std::shared_ptr<Hooks> hooks = Hooks::loadDefault(12, [&log](std::string_view message) { log.emplace_back(message); });
	test::require(hooks != nullptr && hooks->hasPrice() && hooks->hasDemand(), "the shipped default economy script did not load");
	state::TownStock stock;
	stock.lines = {{"empty", 0.0, 100.0}, {"even", 100.0, 100.0}, {"glut", 200.0, 100.0}, {"short", 50.0, 100.0}};
	for (const state::StockLine& line : stock.lines)
	{
		double factor = 0.0;
		test::require(hooks->price(line.itemSid, 1, line.amount, line.target, factor), "the default script's price did not answer for " + line.itemSid);
		test::require(std::fabs(factor - economy::stockFactor(stock, line.itemSid)) < 1e-9,
			"the shipped default's price differs from stockFactor for " + line.itemSid);
	}
	std::printf("default: economy.lua's price is stockFactor's rule\n");
}

// ---- a failing hook falls back for that call, loudly ----

void failingHook()
{
	Simulation sim = newGame(EconomyMode::Real);
	const kdata::GameData& data = *sim.gameData();
	const trade::Book engineBook(data, 12);
	const kdata::Record* item = nullptr;
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("ITEM")))
		if (record->intOr("value", 0) > 0 && engineBook.basePrice(*record) > 0)
		{
			item = record;
			break;
		}
	const state::Inventory::Entry entry{item->stringID, 1, 0, {}};
	state::TownStock stock;
	stock.lines = {{item->stringID, 50.0, 100.0}};
	trade::Terms terms;
	terms.stock = &stock;
	const trade::Quote engine = engineBook.quote(terms, entry);

	std::vector<std::string> log;
	sim.setEconomyHooks(script("return { api = 0, price = function(q) error('boom') end }\n", log));
	terms.hooks = sim.economyHooks();
	const trade::Quote first = sim.tradeBook().quote(terms, entry);
	const trade::Quote second = sim.tradeBook().quote(terms, entry);
	test::require(first.buy == engine.buy && first.sell == engine.sell, "a failing price hook did not fall back to the engine rule");
	test::require(logged(log, "economy hook price failed"), "the failure was not logged");
	test::require(std::count_if(log.begin(), log.end(), [](const std::string& line) { return line.find("economy hook price failed") != std::string::npos; }) >= 2,
		"a failure stopped the hook for the session instead of the call");
	test::require(second.buy == engine.buy, "the second call did not fall back too");

	// A script without price: the engine rule, no error.
	std::vector<std::string> quiet;
	sim.setEconomyHooks(script("return { api = 0, demand = function(q) return {} end }\n", quiet));
	terms.hooks = sim.economyHooks();
	const trade::Quote silent = sim.tradeBook().quote(terms, entry);
	test::require(silent.buy == engine.buy && quiet.empty(), "a script without price changed the quote or logged");
	std::printf("fallback: a failing hook logs and the engine rule answers that call\n");
}

// ---- original mode calls no hook ----

void originalMode()
{
	Fixture f(EconomyMode::Original);
	std::vector<std::string> log;
	f.sim.setEconomyHooks(script("return { api = 0, demand = function(q) ksim.log('called') return {} end, price = function(q) return 9 end }\n", log));
	f.sim.run(3 * static_cast<int>(kTicksPerGameHour));
	test::require(log.empty(), "original mode called a hook");
	trade::Terms terms;
	test::require(f.sim.economyHooks() != nullptr, "the hooks did not load");
	test::require(ecsOf(f.sim).get<state::EconomySettings>() == nullptr, "original mode created the economy singleton");
	std::printf("original: no hook is called\n");
}

} // namespace

int main()
{
	try
	{
		priceHook();
		demandAndEvents();
		defaultScript();
		failingHook();
		originalMode();
		std::cout << "ksim_economy_hooks: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_economy_hooks: FAILED: " << e.what() << "\n";
		return 1;
	}
}
