// The real-economy overlay's camps (TOWN_LIFE_SLICE 7.3, T4b): EconomyProducers seeded from the placed production buildings, the
// staffing rule (a camp runs only while its town has a living staff squad) and the real-converter path at Full detail (a camp's
// output comes from its state::Producer machine, never the aggregate, so load and unload never double-count). This pins:
//  - the seed: the world's camps with their rates, converters carrying a machine, producers without one;
//  - the CRAFTING_SLICE 7.3 rate from the merged data (Iron Refinery II 48 plates per game day, Iron Refinery I 43.2) and the CQ34
//    ingredient rule (one Raw Iron per plate; the Bread Oven's Water 0.25 / Strawflour 1.0);
//  - a chain over a game day: a seeded converter camp takes its input from its town's stock and makes the data's rate;
//  - the staff: killing the camp's staff stops its output;
//  - Full detail: the camp's machine makes the same rate as the aggregate, and a day loaded then a day unloaded add up (no double
//    counting).
// No bake is needed.

#include "ksim/core/clock.hpp"
#include "ksim/core/command.hpp"
#include "ksim/economy/economy.hpp"
#include "ksim/economy/producers.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

Simulation newGame()
{
	SpawnOptions options;
	options.seed = 5;
	options.wildernessByDensity = false;
	options.economy = EconomyMode::Real;
	RegionConfig regions;
	regions.workerThreads = 1;
	return Simulation::newGame(KENSHI_DATA, kworld::resolveLoadOrder(KENSHI_DATA), options, nullptr, regions);
}

// One game hour of ticks.
std::uint64_t ticks(std::uint64_t hours)
{
	return hours * kTicksPerGameHour;
}

flecs::world& ecsOf(Simulation& simulation)
{
	return const_cast<flecs::world&>(simulation.world().ecs());
}

const kdata::Record* recordNamed(const kdata::GameData& data, std::string_view name)
{
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("BUILDING")))
		if (record->name == name)
			return record;
	return nullptr;
}

std::string nameOf(const kdata::GameData& data, const std::string& sid)
{
	const kdata::Record* record = data.find(sid);
	return record != nullptr ? record->name : sid;
}

double lineAmount(const state::TownStock& stock, std::string_view sid)
{
	const state::StockLine* line = economy::findLine(stock, sid);
	return line != nullptr ? line->amount : -1.0;
}

// A town built by the test: no populating world behind it, its stock set here. The camp under test is re-homed to it so nothing
// else moves its lines.
struct Town
{
	flecs::entity entity;
	std::string item;
	std::string input;
};

Town makeTown(flecs::world& ecs, Simulation& simulation, const std::string& item, const std::string& input)
{
	Town town;
	town.item = item;
	town.input = input;
	town.entity = ecs.entity();
	town.entity.set<state::Id>(state::Id{1ull << 60});
	town.entity.set<state::TownState>(state::TownState{"", "", state::EntityRef{}, 0.0, 0.0});
	state::TownStock stock;
	stock.lines.push_back(state::StockLine{input, 5000.0, 5000.0});
	stock.lines.push_back(state::StockLine{item, 0.0, 5000.0});
	std::sort(stock.lines.begin(), stock.lines.end(), [](const auto& left, const auto& right) { return left.itemSid < right.itemSid; });
	stock.prosperity = economy::prosperity(stock);
	stock.lastTick = trader::gameHours(simulation.clockTick());
	town.entity.set<state::TownStock>(std::move(stock));
	town.entity.set<state::SupplyBoard>(state::SupplyBoard{});
	return town;
}

// One staff squad at a town: a squad instance homed there with one member (no Fighter: an unloaded member counts as effective).
flecs::entity makeStaff(flecs::world& ecs, const Town& town)
{
	const flecs::entity squad = ecs.entity();
	squad.set<state::SquadInstance>(state::SquadInstance{"", state::EntityRef{}, state::EntityRef{town.entity.id()},
		state::SpawnSource::TownResident});
	const flecs::entity member = ecs.entity();
	member.add<state::MemberOf>(squad);
	return member;
}

// The first seeded camp (in Id order) that produces `item` and carries a machine (a converter with inputs): the world's Dried Meat
// campfires are the usual pick.
flecs::entity findCamp(flecs::world& ecs, const kdata::GameData& data, const std::string& item)
{
	std::vector<std::pair<std::uint64_t, flecs::entity>> found;
	ecs.each([&](flecs::entity entity, const state::Id& id, const state::EconomyProducer& producer)
	{
		if (economy::isCamp(id.value) && entity.has<state::Producer>() && nameOf(data, producer.itemSid) == item)
			found.emplace_back(id.value, entity);
	});
	if (found.empty())
		return flecs::entity();
	std::sort(found.begin(), found.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
	return found.front().second;
}

void seed()
{
	Simulation simulation = newGame();
	flecs::world& ecs = ecsOf(simulation);
	std::size_t camps = 0;
	std::size_t converters = 0;
	std::size_t machineMissing = 0;
	std::size_t machineWrong = 0;
	std::size_t producerCampsWithMachine = 0;
	std::size_t badRate = 0;
	std::size_t noTown = 0;
	ecs.each([&](flecs::entity entity, const state::Id& id, const state::EconomyProducer& producer)
	{
		if (!economy::isCamp(id.value))
			return;
		++camps;
		noTown += producer.town.id == 0 ? 1 : 0;
		badRate += producer.ratePerHour <= 0.0 ? 1 : 0;
		const state::Producer* machine = entity.get<state::Producer>();
		if (producer.consumes.empty())
		{
			producerCampsWithMachine += machine != nullptr ? 1 : 0; // a producer (a mine, a well): no machine
			return;
		}
		++converters;
		machineMissing += machine == nullptr ? 1 : 0;
		if (machine != nullptr)
			machineWrong += machine->output != producer.itemSid || machine->recordSid.empty() || machine->cap == 0 ? 1 : 0;
	});
	test::require(camps > 100, "the world seeded too few camps: " + std::to_string(camps));
	test::require(converters > 0 && machineMissing == 0 && machineWrong == 0, "no converter camp with a sound machine");
	test::require(producerCampsWithMachine == 0, "a producer camp carries a converter machine");
	test::require(noTown == 0 && badRate == 0, "a camp has no town or no rate");
	std::printf("seed: %zu camps, %zu converters with a machine\n", camps, converters);
}

void rates()
{
	// The CRAFTING_SLICE 7.3 converter rate on the merged data, and the CQ34 ingredient rule.
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA);
	const std::shared_ptr<const kdata::GameData> data = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
	const auto check = [&](const char* name, double perDay, const std::vector<std::pair<std::string, double>>& inputs)
	{
		const kdata::Record* building = recordNamed(*data, name);
		test::require(building != nullptr, std::string("no BUILDING record named ") + name);
		producers::Spec spec;
		test::require(producers::productionSpecOf(*data, *building, spec), std::string(name) + " is not a production building");
		const kdata::Record* function = producers::productionFunctionality(*data, *building);
		test::require(function != nullptr, std::string(name) + " has no functionality");
		const double perHour = economy::campRatePerHour(spec);
		test::require(std::abs(perHour * 24.0 - perDay) < 0.05,
			std::string(name) + ": " + std::to_string(perHour * 24.0) + " a day, the data says " + std::to_string(perDay));
		const std::vector<state::ConsumeLine> consumes = economy::campConsumes(*data, *function, spec.output, perHour);
		test::require(consumes.size() == inputs.size(), std::string(name) + " has a different input list");
		for (const auto& [item, perOutput] : inputs)
		{
			const state::ConsumeLine* line = nullptr;
			for (const state::ConsumeLine& candidate : consumes)
				if (nameOf(*data, candidate.itemSid) == item)
					line = &candidate;
			test::require(line != nullptr, std::string(name) + " does not consume " + item);
			// perHour is perOutput x perHour: compare as items per output.
			test::require(std::abs(line->perHour - perOutput * perHour) < 1e-6,
				std::string(name) + ": " + item + " is not " + std::to_string(perOutput) + " per output");
		}
	};
	// Iron Refinery I: 48 x 0.9 a game day; CQ34: one Raw Iron per plate (the `consumes` rate 200 is the slot's cap, not 2:1).
	check("Iron Refinery", 43.2, {{"Raw Iron", 1.0}});
	// Iron Refinery II: 48 x 1.0.
	check("Iron Refinery II", 48.0, {{"Raw Iron", 1.0}});
	// The Bread Oven: automatic (skill 1.0), 24 a day; its item's ingredients win: 1 Strawflour and 0.25 Water per bread.
	check("Bread Oven", 24.0, {{"Strawflour", 1.0}, {"Water", 0.25}});
	std::printf("rates: Iron Refinery II 48 plates a game day, the Bread Oven 24 bread (1 Strawflour, 0.25 Water each)\n");
}

struct Chain
{
	explicit Chain(Simulation&& world) : simulation(std::move(world)) {}

	Simulation simulation;
	Town town;
	flecs::entity camp;
	flecs::entity member;
	double rate = 0.0;
};

std::unique_ptr<Chain> chainRig()
{
	SpawnOptions options;
	options.seed = 5;
	options.wildernessByDensity = false;
	options.economy = EconomyMode::Real;
	RegionConfig regions;
	regions.workerThreads = 1;
	auto rig = std::make_unique<Chain>(Simulation::newGame(KENSHI_DATA, kworld::resolveLoadOrder(KENSHI_DATA), options, nullptr, regions));
	flecs::world& ecs = ecsOf(rig->simulation);
	const kdata::GameData& data = *rig->simulation.gameData();
	rig->camp = findCamp(ecs, data, "Dried Meat");
	test::require(rig->camp.is_valid(), "no seeded Dried Meat camp in the world");
	state::EconomyProducer& producer = *rig->camp.get_mut<state::EconomyProducer>();
	test::require(producer.consumes.size() == 1, "the camp does not take one input");
	rig->rate = producer.ratePerHour;
	rig->town = makeTown(ecs, rig->simulation, producer.itemSid, producer.consumes.front().itemSid);
	producer.town = state::EntityRef{rig->town.entity.id()};
	rig->member = makeStaff(ecs, rig->town);
	return rig;
}

double campAmount(const Town& town)
{
	return lineAmount(*town.entity.get<state::TownStock>(), town.item);
}

void chain()
{
	std::unique_ptr<Chain> rig = chainRig();
	Simulation& simulation = rig->simulation;
	const double rateDay = rig->rate * 24.0;
	test::require(rig->rate > 0.0, "the camp's rate is zero");

	// A game day of aggregate flow: the output is the CRAFTING rate and the input is drawn at 1 per output (the CQ34 rule).
	simulation.run(ticks(24));
	const double made = campAmount(rig->town);
	const state::TownStock& stock = *rig->town.entity.get<state::TownStock>();
	test::require(std::abs(made - rateDay) < 1.5,
		"the chain did not make the CRAFTING rate: " + std::to_string(made) + " of " + std::to_string(rateDay));
	test::require(std::abs(lineAmount(stock, rig->town.input) - (5000.0 - made)) < 1.5,
		"the input was not drawn one per output");
	std::printf("chain: %.1f a day at %.3f an hour, %.1f of the input drawn\n", made, rig->rate, made);

	// Killing the staff stops the output (7.3).
	rig->member.destruct();
	const double before = campAmount(rig->town);
	simulation.run(ticks(3));
	test::require(std::abs(campAmount(rig->town) - before) < 1e-9, "an unstaffed camp kept producing");
	std::printf("staff: killed, the camp stopped\n");
}

void machine()
{
	std::unique_ptr<Chain> rig = chainRig();
	Simulation& simulation = rig->simulation;
	const state::Producer* campMachine = rig->camp.get<state::Producer>();
	test::require(campMachine != nullptr, "the converter camp has no machine");

	// A day at Full detail: the camp's output comes from its machine's progress, not the aggregate.
	simulation.enqueue(Command::setPlayerAreas({{campMachine->x, campMachine->z}}));
	simulation.run(ticks(24));
	const double loaded = campAmount(rig->town);
	const state::Producer* machine = rig->camp.get<state::Producer>();
	test::require(machine->prodState == 2, "a staffed camp at Full detail is not working: " + std::to_string(machine->prodState));
	test::require(machine->progress >= 0.0 && machine->progress < 1.0, "the machine's progress is out of range");
	test::require(std::abs(loaded - rig->rate * 24.0) < 2.0,
		"the loaded machine did not make its rate: " + std::to_string(loaded));
	const double progress = machine->progress;
	std::printf("machine: %.1f a day at Full detail (progress %.3f)\n", loaded, progress);

	// Unloaded for a day: the aggregate takes over from the stock; the total is two days' rate, not more (no double counting).
	simulation.enqueue(Command::setPlayerAreas({}));
	simulation.run(ticks(24));
	const double both = campAmount(rig->town);
	test::require(std::abs(both - rig->rate * 48.0) < 3.0,
		"load then unload double-counted: " + std::to_string(both) + " of " + std::to_string(rig->rate * 48.0));
	const state::Producer* after = rig->camp.get<state::Producer>();
	test::require(after->progress == progress, "the machine advanced while unloaded");
	std::printf("machine: a day loaded then a day unloaded made %.1f in two days\n", both);
}

} // namespace

int main()
{
	try
	{
		seed();
		rates();
		chain();
		machine();
		std::printf("ksim_economy_prod: ok\n");
	} catch (const test::Failure& failure) {
		std::fprintf(stderr, "ksim_economy_prod: %s\n", failure.what());
		return 1;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "ksim_economy_prod: %s\n", error.what());
		return 1;
	}
	return 0;
}
