// G5 sensor list, the world wiring (task CF): Simulation::sensorContext's relation closure. A fighter's `Fighter::faction` is a
// faction INSTANCE's state::Id value (residents.cpp), NOT an ECS entity id. The base commit resolved the two ids as entity ids, so
// every pair read 0 and a record of a relation-hostile faction never carried kHostile; resolved through the faction map (the one
// updateHostility builds) the flags follow the data. The scenario here sets the relations itself, so the test pins the wiring, not
// the vanilla relation table (that one is ksim_sensor_bits' staged table and ksim_factionrel's).

#include "ksim/ai/sensor.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/state.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

state::Fighter body(std::uint64_t faction)
{
	state::Fighter f;
	f.faction = faction;
	return f;
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		SpawnOptions options;
		options.seed = 7;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options);

		std::vector<flecs::entity> factions;
		sim.world().ecs().each([&](flecs::entity entity, const state::FactionState&, const state::Id&)
		{
			if (factions.size() < 2)
				factions.push_back(entity);
		});
		test::require(factions.size() == 2, "two faction instances in the fresh world");
		factions[0].set<state::Relation>(factions[1], state::Relation{-100.0}); // hostile
		factions[1].set<state::Relation>(factions[0], state::Relation{0.0}); // neutral

		const std::uint64_t from = factions[0].get<state::Id>()->value;
		const std::uint64_t to = factions[1].get<state::Id>()->value;
		test::require(from != to, "two faction ids");

		const sensor::Context ctx = sim.sensorContext();
		test::require(ctx.relation != nullptr, "the context carries the relation function");
		test::require(ctx.relation(from, to) == -100.0, "the observer's own relation is read through its state::Id");
		test::require(ctx.relation(to, from) == 0.0, "the other direction reads its own entry");
		test::require(ctx.relation(from, from) == 100.0, "the same faction is 100");

		state::SightRecord record;
		record.target = 2;
		record.seen = true;
		record.sinceSensed = 0.0;
		const std::uint32_t hostile = sensor::flags(ctx, body(from), body(to), record);
		test::require((hostile & sensor::kHostile) != 0,
			"a record of a faction the observer's faction counts <= -30 carries kHostile (bits " + std::to_string(hostile) + ")");
		test::require((hostile & (sensor::kAlly | sensor::kNeutral)) == 0, "hostile is neither ally nor neutral");
		const std::uint32_t neutral = sensor::flags(ctx, body(to), body(from), record);
		test::require((neutral & sensor::kNeutral) != 0 && (neutral & sensor::kHostile) == 0,
			"the reverse direction is neutral, not hostile (bits " + std::to_string(neutral) + ")");

		std::cout << "ksim_sensor_context OK (factions " << from << " -> " << to << ")\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_sensor_context FAILED: " << e.what() << "\n";
		return 1;
	}
}
