// Fixed-tick scheduling: Full every tick, Squad every 20 ticks (~1 sim
// second), Strategic every 2182 ticks (one game-hour, clock.hpp), including the exact
// boundary ticks (ARCHITECTURE.md "Simulation").

#include "ksim/simulation.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

void requireSystem(const ksim::Simulation& simulation, std::size_t index,
	std::uint64_t runs, std::uint64_t lastTick)
{
	const ksim::SystemState& system = simulation.systems()[index];
	test::require(system.runs == runs,
		system.name + " runs " + std::to_string(system.runs) + " != "
		+ std::to_string(runs) + " at tick "
		+ std::to_string(simulation.clockTick()));
	test::require(system.lastRunTick == lastTick,
		system.name + " last tick " + std::to_string(system.lastRunTick)
		+ " != " + std::to_string(lastTick));
}

} // namespace

int main()
{
	try
	{
		ksim::Simulation simulation = ksim::Simulation::load(KENSHI_DATA);

		test::require(simulation.systems().size() == 3,
			"expected three detail-level systems");
		test::require(ksim::Simulation::kFullIntervalTicks == 1,
			"Full detail is not per tick");
		test::require(ksim::Simulation::kSquadIntervalTicks == 20,
			"Squad detail is not 20 ticks");
		test::require(ksim::Simulation::kStrategicIntervalTicks == 2182,
			"Strategic detail is not 2182 ticks");
		test::require(std::fabs(ksim::Simulation::kSquadIntervalTicks
			* ksim::Simulation::kTickSeconds - 1.0) < 1e-9,
			"Squad interval is not one sim second");
		test::require(std::fabs(ksim::Simulation::kStrategicIntervalTicks
			* ksim::Simulation::kTickSeconds - ksim::kGameHourSeconds) < 0.05,
			"Strategic interval is not one game-hour");

		requireSystem(simulation, 0, 0, 0);
		requireSystem(simulation, 1, 0, 0);
		requireSystem(simulation, 2, 0, 0);

		simulation.tick();
		test::require(simulation.clockTick() == 1, "first tick missing");
		requireSystem(simulation, 0, 1, 1);
		requireSystem(simulation, 1, 0, 0);
		requireSystem(simulation, 2, 0, 0);

		simulation.run(18); // tick 19
		test::require(simulation.clockTick() == 19, "tick count after 19 ticks");
		requireSystem(simulation, 0, 19, 19);
		requireSystem(simulation, 1, 0, 0);
		requireSystem(simulation, 2, 0, 0);

		simulation.tick(); // tick 20: first Squad run
		requireSystem(simulation, 0, 20, 20);
		requireSystem(simulation, 1, 1, 20);
		requireSystem(simulation, 2, 0, 0);
		test::require(std::fabs(simulation.simSeconds() - 1.0) < 1e-9,
			"20 ticks are not one sim second");

		simulation.run(180); // tick 200: ten Squad runs
		requireSystem(simulation, 0, 200, 200);
		requireSystem(simulation, 1, 10, 200);
		requireSystem(simulation, 2, 0, 0);

		simulation.run(1981); // tick 2181: Strategic has not run
		test::require(simulation.clockTick() == 2181, "tick count at 2181");
		requireSystem(simulation, 0, 2181, 2181);
		requireSystem(simulation, 1, 109, 2180);
		requireSystem(simulation, 2, 0, 0);

		simulation.tick(); // tick 2182: first Strategic run
		requireSystem(simulation, 0, 2182, 2182);
		requireSystem(simulation, 1, 109, 2180);
		requireSystem(simulation, 2, 1, 2182);

		simulation.run(2182); // tick 4364: second Strategic run
		requireSystem(simulation, 2, 2, 4364);

		std::cout << "ksim_schedule: Full " << simulation.systems()[0].runs
			<< ", Squad " << simulation.systems()[1].runs
			<< ", Strategic " << simulation.systems()[2].runs << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_schedule: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
