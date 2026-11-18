// kenshi-h2v.21.3: scenarios/food_raid.kscen itself, loaded as the server loads it (Heft handed to the player, a wooden chest of 30 Bread, a Sand
// ninja raid force 700 u out already carrying the raid order through the `town` / `container` / `raid` records). Asserted within 3 game minutes: the
// force reaches the town (its order is Staying), a raider's pack holds Bread from the chest, and the force leaves (Retreating, Returning, Done or gone).
// Needs the world navmesh bake (argv[1], exit 77 without the Heft region).
#include "ksim/simulation.hpp"

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_SCENARIO
#error KSIM_SCENARIO must be defined
#endif

using namespace ksim;

namespace {

constexpr int kRunTicks = 3 * 60 * 20; // 3 game minutes
constexpr const char* kBread = "1946-gamedata.base";

} // namespace

// Runs the scenario `file` for 3 game minutes and asserts the raid: false when the bake lacks the file's region.
static bool runRaid(const scenario::File& file, const std::string& world, const std::string& label)
{
	if (!scenario_rig::hasRegion(file, world))
		return false;
	scenario_rig::Rig rig(file, world);
	Simulation& sim = rig.sim;

	sim.newWorld();
	sim.run(1);
	const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
	std::vector<std::uint64_t> raiders;
	// The characters of the file's raid squads, not a hard-coded id list.
	for (const scenario::Raid& raid : file.raids)
		for (const scenario::Char& c : file.chars)
			if (std::find(raid.squads.begin(), raid.squads.end(), c.squad) != raid.squads.end())
				raiders.push_back(placed.idOf.at(c.id));
	test::require(!raiders.empty(), "the file's raid has no characters");

	flecs::world& ecs = ecsOf(sim);
	const auto breadCarried = [&]()
	{
		std::uint32_t total = 0;
		ecs.each([&](const state::Id& id, const state::Inventory& pack)
		{
			if (std::find(raiders.begin(), raiders.end(), id.value) != raiders.end())
				total += pack.carried(kBread);
		});
		return total;
	};
	state::AiReason failure = state::AiReason::None;
	const auto status = [&]()
	{
		state::OrderStatus found = state::OrderStatus::None;
		ecs.each([&](const state::GroupAI& group)
		{
			if (group.order.campaign.empty())
				return;
			found = group.order.status;
			failure = group.order.failure;
		});
		return found;
	};

	int arrived = -1;
	int looted = -1;
	int left = -1;
	for (int tick = 0; tick < kRunTicks; ++tick)
	{
		sim.run(1);
		if (tick % 20 != 0)
			continue;
		const state::OrderStatus now = status();
		if (tick % 200 == 0)
		{
			const state::Fighter* lead = sim.combat()->find(raiders.front());
			std::cout << "  " << tick / 20 << " s: order " << static_cast<int>(now) << " (" << state::aiReasonName(failure) << "), lead ninja "
				<< (lead != nullptr ? std::to_string(lead->x) + ", " + std::to_string(lead->z) : std::string("gone")) << ", bread carried " << breadCarried() << "\n";
		}
		if (arrived < 0 && now == state::OrderStatus::Staying)
			arrived = tick / 20;
		if (looted < 0 && breadCarried() > 0)
			looted = tick / 20;
		if (arrived >= 0 && left < 0 && (now == state::OrderStatus::Retreating || now == state::OrderStatus::Returning || now == state::OrderStatus::Done || now == state::OrderStatus::None))
			left = tick / 20;
	}
	std::cout << "arrived at " << arrived << " s, bread in a raider's pack at " << looted << " s (" << breadCarried() << " carried at the end), left at " << left << " s\n";
	test::require(arrived >= 0, "the raid force never reached the town (its order never read Staying)");
	test::require(looted >= 0, "no raider's pack ever held Bread from the chest");
	test::require(left >= 0, "the raid force never left (no Retreating / Returning / Done)");
	std::cout << label << " ok\n";
	return true;
}

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(KSIM_SCENARIO);
		if (argc < 2 || !runRaid(file, argv[1], "Old Village"))
		{
			std::cout << "ksim_food_raid_scenario: SKIPPED: needs the world navmesh bake with the Old Village region (pass its directory)\n";
			return 77;
		}
		if (!runRaid(scenario_rig::inHeft(file), argv[1], "Heft (gated)"))
			std::cout << "ksim_food_raid_scenario: the Heft variant is skipped: no bake region\n";

		std::cout << "ksim_food_raid_scenario ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_food_raid_scenario: FAILED: " << e.what() << "\n";
		return 1;
	}
}
