// Border strip perception: an entity sees a neighbour across a region border
// exactly as it would in a single-region run. The strip is a spatial-query
// acceleration, not a visibility limit (ARCHITECTURE.md "Threading").

#include "ksim/region.hpp"
#include "ksim/simulation.hpp"

#include "ksim_region_test_util.hpp"
#include "test_util.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

struct Point
{
	double x = 0.0;
	double z = 0.0;
};

struct Run
{
	std::vector<std::uint64_t> hashes;
	std::vector<ksim::EntityView> views;
	std::vector<std::int32_t> regions;
	std::uint64_t finalContent = 0;
};

// One region covering the world is the no-border reference; 4,608-unit
// regions with 8 workers is the regioned run.
Run run(double regionSize, int workers)
{
	ksim::RegionConfig config = region_test::config(workers, true);
	config.regionSize = regionSize;
	config.perceiveRadius = 500.0;
	config.keepRadius = 2000.0;
	config.moveSpeed = 5.0;
	ksim::Simulation sim = region_test::simulation(555, config);
	const ksim::RegionEngine& engine = sim.regions();

	constexpr double border = -147456.0 + 4608.0 * 11.0;
	const std::vector<Point> points{{border - 10.0, -80000.0},
		{border + 10.0, -80000.0}, {border + 1200.0, -80000.0}};
	std::vector<flecs::entity> entities;
	for (const Point& point : points)
		entities.push_back(sim.spawnMover(point.x, point.z));

	Run run;
	for (std::uint64_t tick = 1; tick <= 50; ++tick)
	{
		sim.tick();
		if (tick == 1 || tick == 50)
			run.hashes.push_back(sim.stateHash());
	}
	for (const flecs::entity entity : entities)
	{
		run.views.push_back(engine.view(entity));
		run.regions.push_back(engine.regionOf(
			engine.view(entity).x, engine.view(entity).z));
	}
	run.finalContent = sim.stateHash();
	return run;
}

} // namespace

int main()
{
	try
	{
		const Run reference = run(294912.0, 1);
		const Run regioned = run(4608.0, 8);

		test::require(reference.regions.size() == regioned.regions.size(), "spawn mismatch");
		// The two perceived entities really are on different sides of a border.
		test::require(regioned.regions[0] != regioned.regions[1],
			"the archer and neighbour are not in different regions");
		test::require(reference.regions[0] == reference.regions[1],
			"the single-region reference split the pair");

		// The archer locks the entity across the border, exactly as without
		// regions. Entity ids are identical because the spawn order is.
		for (std::size_t i = 0; i < reference.views.size(); ++i)
		{
			test::require(reference.views[i].id == regioned.views[i].id,
				"entity ids differ between runs");
			test::require(reference.views[i].target == regioned.views[i].target,
				"target lock differs across a border at entity "
				+ std::to_string(i));
		}
		test::require(regioned.views[0].target == regioned.views[1].id,
			"the archer did not lock the neighbour across the border");
		test::require(reference.hashes[0] == regioned.hashes[0]
			&& reference.hashes[1] == regioned.hashes[1],
			"border perception changed the content hash");
		test::require(reference.finalContent == regioned.finalContent,
			"regioned run diverged from the single-region reference");

		std::cout << "ksim_regions_perception: archer locked cross-border target "
			<< std::hex << regioned.finalContent << std::dec << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_regions_perception: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
