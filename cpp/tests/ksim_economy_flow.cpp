// The real-economy overlay's first phase (TOWN_LIFE_SLICE 7.1-7.5, T4a, save v69): the `economy.mode` world flag, the seeded
// TownStock, the aggregate hourly tick and the soak counters. This pins:
//  - `mode = real` seeds EconomySettings, a TownStock + SupplyBoard on every town, their lines from the town's trade goods with
//    the `trade prices` targets, and the soak's economy counters;
//  - the flow: one producer and one consumer reach the analytic steady state (production == consumption holds the stock), a
//    faster producer grows it, a stopped consumer stops the draw, and a full output stops the producer and its inputs;
//  - `mode = original` (the default) creates no T4 state, its state hash equals a run without the overlay and differs from real.
// No bake is needed.

#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/economy/economy.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/soak.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cmath>
#include <cstdio>
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
	return Simulation::newGame(KENSHI_DATA, kworld::resolveLoadOrder(KENSHI_DATA), options, nullptr, regions);
}

// One game hour of ticks (the clock's rounded hour; each integration spans ~1.00008 h).
std::uint64_t ticks(std::uint64_t hours)
{
	return hours * kTicksPerGameHour;
}

void seed()
{
	Simulation simulation = newGame(EconomyMode::Real);
	flecs::world& ecs = const_cast<flecs::world&>(simulation.world().ecs());
	const state::EconomySettings* settings = ecs.get<state::EconomySettings>();
	test::require(settings != nullptr && settings->mode == state::kEconomyReal, "mode = real has no EconomySettings singleton");

	const kdata::GameData& data = *simulation.gameData();
	std::size_t stocks = 0;
	std::size_t boards = 0;
	std::size_t lines = 0;
	std::size_t towns = 0;
	std::size_t tradePriceTowns = 0;
	std::size_t pricedLines = 0;
	ecs.each([&](flecs::entity entity, const state::TownState& town)
	{
		++towns;
		const state::TownStock* stock = entity.get<state::TownStock>();
		test::require(stock != nullptr, "a town has no TownStock in mode real");
		++stocks;
		test::require(entity.get<state::SupplyBoard>() != nullptr, "a town has no SupplyBoard in mode real");
		++boards;
		for (std::size_t i = 0; i < stock->lines.size(); ++i)
		{
			const state::StockLine& line = stock->lines[i];
			test::require(line.target > 0.0 && line.amount == line.target,
				"a seeded line is not at a positive target");
			test::require(i == 0 || stock->lines[i - 1].itemSid < line.itemSid, "the stock lines are not sorted");
			++lines;
		}
		// The price data: a town with `trade prices` stocks each listed item at that entry's v0.
		const kdata::Record* record = data.find(town.activeSid.empty() ? town.rootSid : town.activeSid);
		if (record != nullptr)
		{
			if (const kdata::RefCategory* prices = record->findRefs("trade prices"))
			{
				++tradePriceTowns;
				for (const kdata::RefEntry& entry : prices->entries)
				{
					const state::StockLine* line = economy::findLine(*stock, entry.targetID);
					test::require(line != nullptr, "a `trade prices` item is not stocked");
					test::require(line->target == static_cast<double>(entry.v0),
						"a `trade prices` item does not use its v0 as the target");
					++pricedLines;
				}
			}
		}
	});
	test::require(towns > 100 && stocks == towns && boards == towns, "the T4 state does not cover every town");
	test::require(lines > 0, "no town has a stock line");
	test::require(tradePriceTowns > 0 && pricedLines > 0, "no town seeded from `trade prices`");
	std::printf("seed: %zu towns, %zu stock lines, %zu towns with trade prices\n", towns, lines, tradePriceTowns);
}

void flow()
{
	Simulation simulation = newGame(EconomyMode::Real);
	flecs::world& ecs = const_cast<flecs::world&>(simulation.world().ecs());
	// One town, with everything else about it ignored: the tick is per town and per line.
	flecs::entity town;
	ecs.each([&](flecs::entity entity, state::TownStock&)
	{
		if (!town.is_valid())
			town = entity;
	});
	test::require(town.is_valid(), "no town with a stock");
	state::TownStock& stock = *town.get_mut<state::TownStock>();
	stock.lines.clear();
	stock.lines.push_back(state::StockLine{"test-x", 50.0, 100.0});
	stock.lines.push_back(state::StockLine{"test-y", 0.0, 100.0});
	stock.prosperity = 1.0;

	const flecs::entity producer = ecs.entity();
	producer.set<state::EconomyProducer>(state::EconomyProducer{state::EntityRef{town.id()}, "test-x", 10.0, 0.0, {}});
	const flecs::entity consumer = ecs.entity();
	consumer.set<state::EconomyProducer>(state::EconomyProducer{state::EntityRef{town.id()}, "test-y", 10.0, 0.0,
		{state::ConsumeLine{"test-x", 10.0}}});
	const auto amount = [&](const char* item)
	{
		const state::StockLine* line = economy::findLine(*town.get<state::TownStock>(), item);
		return line != nullptr ? line->amount : -1.0;
	};
	const auto near = [](double got, double want) { return std::abs(got - want) < 0.02; };

	// Production 10/h, consumption 10/h: the town's stock of test-x holds (the analytic steady state), test-y fills at 10/h.
	simulation.run(ticks(5));
	test::require(near(amount("test-x"), 50.0), "the steady stock moved: " + std::to_string(amount("test-x")));
	test::require(near(amount("test-y"), 50.0), "the consumer's output is not its rate: " + std::to_string(amount("test-y")));

	// A faster producer: the surplus accumulates.
	producer.get_mut<state::EconomyProducer>()->ratePerHour = 20.0;
	simulation.run(ticks(3));
	test::require(near(amount("test-x"), 80.0), "the surplus did not accumulate: " + std::to_string(amount("test-x")));
	test::require(near(amount("test-y"), 80.0), "the consumer's output moved with the surplus: " + std::to_string(amount("test-y")));

	// No consumer: the producer alone still runs at its rate.
	consumer.destruct();
	simulation.run(ticks(2));
	test::require(near(amount("test-x"), 120.0), "the lone producer did not run: " + std::to_string(amount("test-x")));

	// A full output stops the producer, and its input is not consumed (the other producer is turned off so nothing else moves).
	producer.get_mut<state::EconomyProducer>()->ratePerHour = 0.0;
	state::TownStock& after = *town.get_mut<state::TownStock>();
	economy::lineFor(after, "test-y").amount = economy::cap(economy::lineFor(after, "test-y"));
	const flecs::entity consumer2 = ecs.entity();
	consumer2.set<state::EconomyProducer>(state::EconomyProducer{state::EntityRef{town.id()}, "test-y", 10.0, 0.0,
		{state::ConsumeLine{"test-x", 10.0}}});
	const double x = amount("test-x");
	const double y = amount("test-y");
	simulation.run(ticks(2));
	test::require(near(amount("test-x"), x) && near(amount("test-y"), y),
		"a full output kept a producer running (its input was consumed)");
	std::printf("flow: a town reached its steady state and stopped at a full output\n");
}

void original()
{
	// The default mode: no T4 state at all, and the state hash is the one a T3 build makes (the save encodes only what exists, so
	// the absent kinds contribute nothing).
	const Simulation first = newGame(EconomyMode::Original);
	flecs::world& ecs = const_cast<flecs::world&>(first.world().ecs());
	test::require(ecs.get<state::EconomySettings>() == nullptr, "mode = original created the economy singleton");
	std::size_t t4 = 0;
	ecs.each([&](flecs::entity, const state::TownStock&) { ++t4; });
	ecs.each([&](flecs::entity, const state::EconomyProducer&) { ++t4; });
	ecs.each([&](flecs::entity, const state::CaravanOrder&) { ++t4; });
	ecs.each([&](flecs::entity, const state::SupplyBoard&) { ++t4; });
	test::require(t4 == 0, "mode = original created T4 components");

	Simulation one = newGame(EconomyMode::Original);
	Simulation two = newGame(EconomyMode::Original);
	Simulation real = newGame(EconomyMode::Real);
	one.run(ticks(3));
	two.run(ticks(3));
	real.run(ticks(3));
	test::require(one.stateHash() == two.stateHash(), "two original-mode runs of the same world differ");
	test::require(one.stateHash() != real.stateHash(), "mode = real did not change the state hash");
	std::printf("original: no T4 state, hash %016llx (real differs: %016llx)\n",
		static_cast<unsigned long long>(one.stateHash()), static_cast<unsigned long long>(real.stateHash()));
}

void saveLoad()
{
	// The economy state is saved (v69): a load keeps the mode and the stock, and the run continues to the same hash.
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA);
	SpawnOptions options;
	options.seed = 5;
	options.wildernessByDensity = false;
	options.economy = EconomyMode::Real;
	RegionConfig regions;
	regions.workerThreads = 1;
	Simulation straight = Simulation::newGame(KENSHI_DATA, order, options, nullptr, regions);
	straight.run(ticks(3));
	const std::uint64_t mid = straight.stateHash();
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_economy_flow.ksav";
	straight.saveGame(path);
	straight.run(ticks(3));
	const std::uint64_t end = straight.stateHash();
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, regions);
	std::remove(path.c_str());
	test::require(loaded.stateHash() == mid, "the loaded economy world hashes differently");
	const state::EconomySettings* settings = loaded.world().ecs().get<state::EconomySettings>();
	test::require(settings != nullptr && settings->mode == state::kEconomyReal, "the economy mode did not survive a load");
	loaded.run(ticks(3));
	test::require(loaded.stateHash() == end, "the loaded economy run differs from the uninterrupted one");
	std::printf("save: the mode and the stock survive a load (hash %016llx)\n",
		static_cast<unsigned long long>(mid));
}

void counters()
{
	// The soak's economy counters: the towns and lines of the real world, zero in original. A short run needs the AI for the
	// soak's audit.
	SpawnOptions options;
	options.seed = 5;
	options.wildernessByDensity = false;
	options.economy = EconomyMode::Real;
	RegionConfig regions;
	regions.workerThreads = 1;
	Simulation simulation = Simulation::newGame(KENSHI_DATA, kworld::resolveLoadOrder(KENSHI_DATA), options, nullptr, regions);
	AiSystem::Config config;
	config.registry = std::make_shared<const BehaviourRegistry>(simulation.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	simulation.enableAi(std::move(config));
	SoakOptions soak;
	soak.days = 4.0 / 24.0;
	const SoakReport report = runSoak(simulation, soak);
	test::require(report.economyTowns > 100 && report.economyLines > 0, "the soak saw no economy state");
	// T4b seeds the camps, so a real world's stock moves: the counters must hold a sensible fill, not the seeded target everywhere.
	test::require(report.economyProducers > 0 && report.economyFill >= 0.0 && report.economyFill <= 1.0,
		"the soak's economy counters are out of range");
	std::printf("soak: %zu towns, %zu lines, fill %.3f, %zu producers\n", report.economyTowns, report.economyLines,
		report.economyFill, report.economyProducers);

	Simulation off = newGame(EconomyMode::Original);
	AiSystem::Config offConfig;
	offConfig.registry = std::make_shared<const BehaviourRegistry>(off.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	off.enableAi(std::move(offConfig));
	const SoakReport plain = runSoak(off, soak);
	test::require(plain.economyTowns == 0 && plain.economyLines == 0 && plain.economyProducers == 0,
		"the soak counted economy state in original mode");
}

} // namespace

int main()
{
	try
	{
		seed();
		flow();
		original();
		saveLoad();
		counters();
		std::printf("ksim_economy_flow: ok\n");
	} catch (const test::Failure& failure) {
		std::fprintf(stderr, "ksim_economy_flow: %s\n", failure.what());
		return 1;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "ksim_economy_flow: %s\n", error.what());
		return 1;
	}
	return 0;
}
