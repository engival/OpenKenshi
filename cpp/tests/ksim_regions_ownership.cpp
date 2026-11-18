// Ownership decides who writes (ARCHITECTURE.md "Threading"). The engine
// audits every entity write: the writing work unit's region must equal the
// entity's owning region. It also checks that the flecs Region component
// value matches the engine's owner after migration.

#include "ksim/world/region.hpp"
#include "ksim/core/simulation.hpp"

#include "ksim_region_test_util.hpp"
#include "test_util.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

int main()
{
	try
	{
		ksim::RegionConfig config = region_test::config(8, true);
		config.perceiveRadius = 300.0;
		config.keepRadius = 900.0;
		config.moveSpeed = 50.0;
		// Hold workers until all have a task: the spread across threads is
		// then guaranteed by the pool, not by machine load.
		config.rendezvousWorkers = true;
		ksim::Simulation sim = region_test::simulation(99, config);
		const ksim::RegionEngine& engine = sim.regions();

		const std::vector<std::pair<double, double>> positions
			= region_test::scattered(300, 4242);
		std::vector<flecs::entity> entities;
		entities.reserve(positions.size());
		for (std::size_t i = 0; i < positions.size(); ++i)
		{
			entities.push_back(sim.spawnMover(positions[i].first,
				positions[i].second, static_cast<std::int32_t>(i / 5)));
		}

		sim.run(50);

		const ksim::RegionStats& stats = engine.stats();
		test::require(stats.writeViolations == 0,
			"an entity was written by a region that does not own it: "
			+ std::to_string(stats.writeViolations));
		test::require(stats.maxActiveWorkers > 1,
			"the 8-worker run never used more than one thread");
		test::require(stats.writes > stats.entityUpdates,
			"no cross-border messages were applied");
		test::require(stats.entityUpdates > 0 && stats.hitsDelivered > 0,
			"the audit saw no writes");
		test::require(stats.hitsDelivered == stats.hitMessages,
			"a hit message was lost");

		std::size_t memberships = 0;
		std::size_t migrated = 0;
		for (std::size_t slot = 0; slot < entities.size(); ++slot)
		{
			const ksim::EntityView view = engine.view(entities[slot]);
			test::require(view.region == engine.regionOf(view.x, view.z),
				"entity owner does not match its position");
			const ksim::Region* component = entities[slot].get<ksim::Region>();
			test::require(component != nullptr,
				"entity has no Region component");
			test::require(component->index == view.region,
				"flecs Region component is stale");
			const ksim::state::Position* position =
				entities[slot].get<ksim::state::Position>();
			test::require(position != nullptr && position->x == view.x
				&& position->z == view.z,
				"flecs Position component is stale");
			if (view.region != engine.regionOf(positions[slot].first,
				positions[slot].second))
				++migrated;
		}
		for (std::int32_t region = 0; region < engine.regionCount(); ++region)
			memberships += engine.regionEntityCount(region);
		test::require(memberships == engine.entityCount(),
			"region membership lost or duplicated entities");
		test::require(migrated > 0, "no entity ever crossed a region border");

		std::cout << "ksim_regions_ownership: writes " << stats.writes
			<< ", violations " << stats.writeViolations << ", migrated "
			<< migrated << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_regions_ownership: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
