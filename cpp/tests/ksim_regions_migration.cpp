// Entities crossing a region border must end up owned by the correct region
// at the tick boundary, with none lost or duplicated (ARCHITECTURE.md
// "Threading": ownership changes only at the tick boundary, as a message).

#include "ksim/region.hpp"
#include "ksim/simulation.hpp"

#include "ksim_region_test_util.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <vector>

int main()
{
	try
	{
		ksim::RegionConfig config = region_test::config(8, true);
		config.squadIntervalTicks = 1;
		config.perceiveRadius = 100.0;
		config.keepRadius = 500.0;
		config.moveSpeed = 200.0;
		config.squadSpeed = 0.0;
		ksim::Simulation sim = region_test::simulation(17, config);
		const ksim::RegionEngine& engine = sim.regions();

		const std::vector<std::pair<double, double>> positions
			= region_test::scattered(200, 90210, 60000.0);
		std::vector<flecs::entity> entities;
		std::vector<std::int32_t> spawnRegions;
		for (std::size_t i = 0; i < positions.size(); ++i)
		{
			entities.push_back(sim.spawnMover(positions[i].first,
				positions[i].second, static_cast<std::int32_t>(i / 10)));
			spawnRegions.push_back(engine.view(entities.back()).region);
		}

		// Pull each squad across the map; the order is a boundary message.
		for (std::int32_t squad = 0; squad < 20; ++squad)
			sim.enqueue(ksim::Command::setSquadCentre(squad,
				-147456.0 + 4608.0 * (squad + 8), -80000.0));

		const std::size_t count = engine.entityCount();
		std::vector<std::int32_t> lastRegion = spawnRegions;
		std::vector<std::int32_t> visits(engine.entityCount(), 1);
		std::size_t crossings = 0;
		std::int32_t maxVisits = 0;
		for (std::uint64_t tick = 1; tick <= 120; ++tick)
		{
			sim.tick();
			std::size_t memberships = 0;
			for (std::int32_t region = 0; region < engine.regionCount(); ++region)
				memberships += engine.regionEntityCount(region);
			test::require(memberships == count,
				"membership count changed at tick "
				+ std::to_string(sim.clockTick()));
			test::require(engine.entityCount() == count,
				"entity count changed during migration");

			for (std::size_t i = 0; i < entities.size(); ++i)
			{
				const ksim::EntityView view = engine.view(entities[i]);
				test::require(view.region == engine.regionOf(view.x, view.z),
					"owner does not match position at tick "
					+ std::to_string(sim.clockTick()));
				const ksim::Region* component = entities[i].get<ksim::Region>();
				test::require(component != nullptr
					&& component->index == view.region,
					"flecs Region component is stale");
				if (view.region != lastRegion[i])
				{
					++crossings;
					lastRegion[i] = view.region;
					++visits[i];
					maxVisits = std::max(maxVisits, visits[i]);
				}
			}
		}

		std::set<flecs::entity_t> ids;
		for (const flecs::entity entity : entities)
			ids.insert(entity.id());
		test::require(ids.size() == count, "an entity id was lost or duplicated");
		test::require(crossings > 0, "no entity crossed a border");
		test::require(maxVisits >= 3,
			"no entity crossed more than one region border");

		std::cout << "ksim_regions_migration: crossings " << crossings
			<< ", max regions visited " << maxVisits << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_regions_migration: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
