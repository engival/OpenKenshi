// Full-detail areas: a set of regions around a player position; overlapping
// areas merge into one. Regions in an area tick every tick, squad-level
// regions at ~1 s (ARCHITECTURE.md "Threading" per-level table and
// "Simulation").

#include "ksim/region.hpp"
#include "ksim/simulation.hpp"

#include "ksim_region_test_util.hpp"
#include "test_util.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool contains(const ksim::Area& area, std::int32_t region)
{
	for (const std::int32_t value : area.regions)
	{
		if (value == region)
			return true;
	}
	return false;
}

} // namespace

int main()
{
	try
	{
		ksim::RegionConfig config = region_test::config(4, false);
		config.regionSize = 4608.0;
		config.areaRadiusRegions = 1;
		config.perceiveRadius = 100.0;
		config.moveSpeed = 1.0;
		ksim::Simulation sim = region_test::simulation(31, config);
		const ksim::RegionEngine& engine = sim.regions();

		const flecs::entity inside = sim.spawnMover(-100000.0, -80000.0);
		const flecs::entity outside = sim.spawnMover(0.0, 60000.0);

		// Players in adjacent regions: the two 3x3 sets overlap and merge.
		// The command applies at the start of tick 1.
		sim.enqueue(ksim::Command::setPlayerAreas({{-100000.0, -80000.0},
			{-96000.0, -80000.0}, {0.0, 0.0}}));
		test::require(engine.areas().empty(), "areas changed before the command tick");
		sim.tick();
		const std::vector<ksim::Area>& areas = engine.areas();
		test::require(areas.size() == 2,
			"overlapping areas did not merge to one");
		test::require(areas[0].regions.size() == 12,
			"merged area is not the union of the two 3x3 sets");
		test::require(contains(areas[0], 9 + 64 * 13)
			&& contains(areas[0], 12 + 64 * 15),
			"merged area does not span both player sets");
		test::require(!contains(areas[0], 32 + 64 * 32),
			"far player area was merged by mistake");

		test::require(inside.get<ksim::Detail>()->level
			== ksim::DetailLevel::Full,
			"entity in a player area is not Full");
		test::require(outside.get<ksim::Detail>()->level
			== ksim::DetailLevel::Squad,
			"entity outside a player area is not Squad");

		sim.run(9);
		test::require(engine.view(inside).steps == 10,
			"full-detail region did not tick every tick");
		test::require(engine.view(outside).steps == 0,
			"squad-level region ticked before ~1 s");

		sim.run(10);
		test::require(engine.view(inside).steps == 20,
			"full-detail region stopped ticking");
		test::require(engine.view(outside).steps == 1,
			"squad-level region did not tick at tick 20");

		std::cout << "ksim_regions_areas: areas " << areas.size()
			<< ", merged regions " << areas[0].regions.size()
			<< ", inside steps " << engine.view(inside).steps
			<< ", outside steps " << engine.view(outside).steps << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_regions_areas: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
