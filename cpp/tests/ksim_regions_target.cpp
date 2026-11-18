// Target persistence across borders: an archer locks a runner by id, the
// runner crosses several region borders, the lock never drops, and every hit
// is delivered to the region owning the target that tick, exactly once
// (ARCHITECTURE.md "Threading": references are IDs; effects are messages with
// one tick of delay).

#include "ksim/world/region.hpp"
#include "ksim/core/simulation.hpp"

#include "ksim_region_test_util.hpp"
#include "test_util.hpp"

#include <cstdint>
#include <iostream>
#include <set>
#include <vector>

int main()
{
	try
	{
		ksim::RegionConfig config = region_test::config(4, true);
		config.squadIntervalTicks = 1;
		config.perceiveRadius = 2000.0;
		config.keepRadius = 100000.0;
		config.moveSpeed = 200.0;
		config.squadSpeed = 0.0;
		ksim::Simulation sim = region_test::simulation(2718, config);
		const ksim::RegionEngine& engine = sim.regions();

		const double startX = -147456.0 + 4608.0 * 10.0 + 200.0;
		const double z = -80000.0;
		const flecs::entity archer = sim.spawnMover(startX, z, 0);
		const flecs::entity runner = sim.spawnMover(startX - 50.0, z, 1);

		// The runner's squad is told to march east; the archer's squad centre
		// stays on top of the archer, so the archer holds position.
		sim.enqueue(ksim::Command::setSquadCentre(1,
			startX + 15000.0, z));

		std::vector<ksim::HitEvent> events;
		sim.setHitObserver([&events](const ksim::HitEvent& event)
		{
			events.push_back(event);
		});

		std::set<std::int32_t> hitRegions;
		std::uint64_t archerHits = 0;
		for (std::uint64_t tick = 1; tick <= 100; ++tick)
		{
			const std::int32_t archerRegion = engine.view(archer).region;
			const std::int32_t runnerRegion = engine.view(runner).region;
			const std::size_t before = events.size();
			sim.tick();

			// The lock survives every border crossing.
			test::require(engine.view(archer).target == runner.id(),
				"the archer lost its lock at tick " + std::to_string(tick));
			test::require(engine.view(runner).region
				== engine.regionOf(engine.view(runner).x,
					engine.view(runner).z),
				"the runner's owner is wrong at tick " + std::to_string(tick));

			std::uint64_t pairHits = 0;
			std::uint64_t allHits = 0;
			for (std::size_t i = before; i < events.size(); ++i)
			{
				const ksim::HitEvent& event = events[i];
				test::require(event.tick == tick, "hit delivered at wrong tick");
				const std::int32_t owner = event.target == archer.id()
					? archerRegion : runnerRegion;
				test::require(event.region == owner,
					"hit delivered to a region that did not own the target");
				++allHits;
				if (event.archer == archer.id() && event.target == runner.id())
				{
					++pairHits;
					hitRegions.insert(event.region);
				}
			}
			test::require(pairHits == 1,
				"archer hit was lost or applied twice at tick "
				+ std::to_string(tick));
			test::require(allHits == 2,
				"expected one hit in each direction at tick "
				+ std::to_string(tick));
			archerHits += pairHits;
		}

		test::require(engine.view(runner).hits == archerHits,
			"runner hit counter does not match delivered archer hits");
		test::require(engine.stats().hitsDelivered == engine.stats().hitMessages,
			"a hit message was lost");
		test::require(hitRegions.size() >= 3,
			"the target did not cross enough region borders");

		std::cout << "ksim_regions_target: hits " << archerHits
			<< ", regions " << hitRegions.size() << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_regions_target: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
