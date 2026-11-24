// Automatic machines (BUILDING `max operators` 0: Well II, the drills) produce with nobody operating them (kenshi-gzto,
// kenshi-k16.5.24): a player-owned one's output moves over game time at rate x richness, to its cap, then reads output-full.
// Lists the automatic producer records of the merged data. No bake is needed.

#include "ksim/combat/combat_data.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/inventory_grid.hpp"
#include "ksim/economy/producers.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

int main()
{
	using namespace ksim;
	SpawnOptions options;
	options.seed = 5;
	options.wildernessByDensity = false;
	RegionConfig regions;
	regions.workerThreads = 1;
	Simulation sim = Simulation::newGame(KENSHI_DATA, kworld::resolveLoadOrder(KENSHI_DATA), options, nullptr, regions);
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(CombatData::build(*sim.gameData(), KENSHI_DATA), settings);
	flecs::world& ecs = const_cast<flecs::world&>(sim.world().ecs());
	const kdata::GameData& data = *sim.gameData();

	const kdata::Record* well = nullptr;
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("BUILDING")))
	{
		producers::Spec spec;
		if (!producers::specOf(data, *record, spec) || spec.maxOperators != 0)
			continue;
		std::printf("automatic producer: %s -> %s, cap %u, %.4f per game day\n", record->name.c_str(), spec.output.c_str(), spec.cap,
			producers::baseProgressPerTick(spec) * kTicksPerGameDay);
		if (record->name == "Well II")
			well = record;
	}
	test::require(well != nullptr, "no automatic Well II producer record");
	producers::Spec spec;
	producers::specOf(data, *well, spec);

	const std::string player = "auto-producer-test-faction";
	state::FactionState owner;
	owner.templateSid = player;
	ecs.entity().set<state::FactionState>(std::move(owner)).set<state::PlayerFaction>(state::PlayerFaction{});

	state::Producer producer;
	producer.recordSid = well->stringID;
	producer.ownerSid = player;
	producer.output = spec.output;
	producer.cap = spec.cap;
	producer.maxOperators = 0;
	producer.useRange = spec.useRange;
	producer.progress = 0.01; // not pristine: an idle empty producer far from a player character is dropped
	state::Inventory stock;
	stock.stackLimit = spec.cap;
	stock.version = 1;
	const grid::Size slot = grid::Rules(data).footprint(spec.output);
	stock.gridW = static_cast<std::uint8_t>(slot.w);
	stock.gridH = static_cast<std::uint8_t>(slot.h);
	flecs::entity entity = ecs.entity();
	entity.set<state::Id>(state::Id{987654321});
	entity.set<state::Producer>(std::move(producer));
	entity.set<state::Inventory>(std::move(stock));

	const auto held = [&] { return entity.get<state::Inventory>()->carried(spec.output); };
	test::require(held() == 0, "starts empty");
	const double perHour = producers::baseProgressPerTick(spec) * kTicksPerGameHour;
	sim.run(kTicksPerGameHour);
	const std::uint32_t one = held();
	sim.run(2 * kTicksPerGameHour);
	const std::uint32_t three = held();
	std::printf("%s: %u after 1 game hour, %u after 3 (data: %.3f per hour, cap %u)\n", well->name.c_str(), one, three, perHour, spec.cap);
	test::require(three > one && one > 0, "the automatic producer's stock did not move with nobody operating it");
	test::require(std::fabs(one - perHour) <= 1.0 && std::fabs(three - 3 * perHour) <= 1.0, "output is not the record's rate");
	test::require(entity.get<state::Producer>()->operators.empty(), "nobody operates it");
	test::require(entity.get<state::Producer>()->prodState == 2, "status is Working, not No workers");
	sim.run(kTicksPerGameDay);
	test::require(held() == spec.cap && entity.get<state::Producer>()->prodState == 1, "it stops at its cap (output full)");
	return 0;
}
