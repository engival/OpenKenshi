// Squads span regions: one group step per squad tick, assigned by squad id,
// reading members from frozen state wherever they are. Every member receives
// its order regardless of region, and the result matches a single-region
// reference run (ARCHITECTURE.md "Threading").

#include "ksim/region.hpp"
#include "ksim/simulation.hpp"

#include "ksim_region_test_util.hpp"
#include "test_util.hpp"

#include <cstdint>
#include <iostream>
#include <set>
#include <vector>

namespace {

struct Member
{
	flecs::entity entity;
	std::int32_t spawnRegion = -1;
};

struct Run
{
	std::vector<Member> members;
	std::uint64_t content = 0;
	std::uint64_t squadSteps = 0;
	std::uint64_t orderMessages = 0;
	double centreX = 0.0;
	double centreZ = 0.0;
	std::size_t distinctRegions = 0;
};

Run run(double regionSize, int workers)
{
	ksim::RegionConfig config = region_test::config(workers, true);
	config.regionSize = regionSize;
	config.squadIntervalTicks = 20;
	config.perceiveRadius = 300.0;
	config.moveSpeed = 5.0;
	config.squadSpeed = 10.0;
	ksim::Simulation sim = region_test::simulation(31415, config);
	const ksim::RegionEngine& engine = sim.regions();

	const double centreX = -147456.0 + 4608.0 * 10.5;
	const double centreZ = -147456.0 + 4608.0 * 10.5;
	Run result;
	std::set<std::int32_t> regions;
	for (int dz = -1; dz <= 1; ++dz)
	{
		for (int dx = -1; dx <= 1; ++dx)
		{
			const flecs::entity entity = sim.spawnMover(centreX + dx * 4608.0,
				centreZ + dz * 4608.0, 0);
			const std::int32_t region = engine.view(entity).region;
			result.members.push_back(Member{entity, region});
			regions.insert(region);
		}
	}
	result.distinctRegions = regions.size();

	sim.run(19);
	test::require(engine.squadSteps(0) == 0,
		"squad stepped before its 1 s tick");
	sim.tick(); // tick 20: first squad step
	test::require(engine.squadSteps(0) == 1, "missing group step at tick 20");
	sim.run(20); // tick 40: second squad step
	test::require(engine.squadSteps(0) == 2,
		"not exactly one group step per squad tick");

	result.centreX = engine.squadCentreX(0);
	result.centreZ = engine.squadCentreZ(0);
	for (const Member& member : result.members)
	{
		const ksim::EntityView view = engine.view(member.entity);
		test::require(view.hasOrder,
			"a member in some region did not receive its order");
		test::require(view.orderX == result.centreX
			&& view.orderZ == result.centreZ,
			"member order does not point at the squad centre");
	}
	result.squadSteps = engine.stats().squadSteps;
	result.orderMessages = engine.stats().orderMessages;
	result.content = sim.stateHash();
	return result;
}

} // namespace

int main()
{
	try
	{
		const Run regioned = run(4608.0, 8);
		const Run reference = run(294912.0, 1);

		test::require(regioned.distinctRegions >= 3,
			"the squad does not span 3+ regions");
		test::require(regioned.squadSteps == 2,
			"wrong total squad step count");
		test::require(regioned.orderMessages == 9 * 2,
			"not every member got exactly one order per group step");
		test::require(regioned.members.size() == reference.members.size(),
			"member count differs");
		for (std::size_t i = 0; i < regioned.members.size(); ++i)
		{
			test::require(regioned.members[i].entity.id()
				== reference.members[i].entity.id(),
				"entity ids differ between regioned and reference runs");
		}
		test::require(regioned.content == reference.content,
			"squad results differ from the single-region reference");

		std::cout << "ksim_regions_squads: regions "
			<< regioned.distinctRegions << ", steps " << regioned.squadSteps
			<< ", orders " << regioned.orderMessages << std::hex << ", hash "
			<< regioned.content << std::dec << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_regions_squads: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
