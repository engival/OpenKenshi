// kenshi-h2v.15.3: scenarios/shackled_escape.kscen puts three squadmates in locked shackles (Picker and Spare: lockpicking 40, strength 20, a hacksaw; Strong: strength 95, no
// tool) and orders nothing. The test gives the orders the inventory menu would (order 60 actions 15 / 16 / 17, Simulation::orderShackleAction) and checks: a break by a weak
// character is refused (the shackles stay locked); a cut by Picker drains with a kind 7 bar and takes the shackles off; a pick by Spare shows a kind 1 bar and leaves the shackles
// worn and open; a break by Strong leaves them worn and open at once. The control reruns it with Strong's strength and Picker's tool taken out of the file: the break and the cut
// then fail (the check that fails before the scenario had them).
// Needs the world navmesh bake (argv[1], exit 77 without the Heft region).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>
#include "nav_source.hpp"
#include "ksim/dev/build_paths.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

namespace {

constexpr int kPickSeconds = 600; // the pick is a roll every interval at the shown chance
constexpr int kCutSeconds = 600; // a weak cutter drains slowly (door_plan.cpp cutRate)

struct Outcome
{
	bool weakBreakRefused = false; // Picker's break: accepted by the order, the shackles still locked after 5 s
	double cutSeconds = 0.0; // how long Picker's cut ran
	bool cutDone = false; // Picker's cut took the shackles off
	bool cutBar = false; // kind 7 row for Picker while it cut
	double pickSeconds = 0.0;
	bool pickOpen = false; // Spare's pick opened the shackles
	bool pickWorn = false; // ... and they are still worn
	bool pickBar = false; // kind 1 row for Spare
	bool strongBroke = false; // Strong's break opened them at once
	bool strongWorn = false;
};

Outcome run(Simulation& sim, const kdata::GameData& gd, const CombatData& data, simlink::ServerContext& context, const scenario::File& file)
{
	sim.newWorld();
	sim.run(1);
	const scenario::Placed placed = scenario::apply(sim, gd, data, context, file, 5);
	const std::uint64_t picker = placed.idOf.at(2);
	const std::uint64_t spare = placed.idOf.at(3);
	const std::uint64_t strong = placed.idOf.at(4);
	test::require(sim.shacklesLocked(picker) && sim.shacklesLocked(spare) && sim.shacklesLocked(strong), "the three start in locked shackles");
	const auto bar = [&](std::uint64_t who, std::uint8_t kind)
	{
		for (const Simulation::TaskProgressRow& row : sim.taskProgress(true))
			if (row.id == who && row.kind == kind)
				return true;
		return false;
	};
	Outcome out;
	// A break by the weak: the order is taken, the run refuses it.
	test::require(sim.orderShackleAction(picker, picker, 17).empty(), "Picker's break order is accepted");
	sim.run(100);
	out.weakBreakRefused = sim.shacklesLocked(picker);
	// A cut by Picker.
	test::require(sim.orderShackleAction(picker, picker, 16).empty(), "Picker's cut order is accepted");
	int cutTicks = 0;
	for (; cutTicks < kCutSeconds * 20 && sim.shacklesWorn(picker); ++cutTicks)
	{
		sim.tick();
		out.cutBar = out.cutBar || bar(picker, 7);
	}
	out.cutDone = !sim.shacklesWorn(picker);
	out.cutSeconds = cutTicks * 0.05;
	// A pick by Spare, on its own.
	test::require(sim.orderShackleAction(spare, spare, 15).empty(), "Spare's pick order is accepted");
	int pickTicks = 0;
	for (; pickTicks < kPickSeconds * 20 && sim.shacklesLocked(spare); ++pickTicks)
	{
		sim.tick();
		out.pickBar = out.pickBar || bar(spare, 1);
	}
	out.pickSeconds = pickTicks * 0.05;
	out.pickOpen = !sim.shacklesLocked(spare);
	out.pickWorn = sim.shacklesWorn(spare);
	// A break by Strong.
	test::require(sim.orderShackleAction(strong, strong, 17).empty(), "Strong's break order is accepted");
	sim.run(40);
	out.strongBroke = !sim.shacklesLocked(strong);
	out.strongWorn = sim.shacklesWorn(strong);
	return out;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/shackled_escape.kscen");
		const kworld::ZoneCell zone = kworld::zoneCellOf(file.centreX, file.centreZ);
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), zone.x, zone.z))
		{
			std::cout << "ksim_shackled_escape_scen: SKIPPED: needs the world navmesh bake with the scenario's region (pass its directory)\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		const std::string world = argv[1];
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		// A fresh simulation per run: lock orders and the rest of one run's state must not carry into the next.
		std::shared_ptr<const kdata::GameData> gameData;
		std::shared_ptr<const CombatData> combatData;
		const auto fresh = [&]()
		{
			SpawnOptions spawn;
			spawn.seed = 7;
			Simulation made = Simulation::newGame(KENSHI_DATA, order, spawn);
			made.markPristine();
			gameData = made.gameData();
			combatData = CombatData::build(*gameData, KENSHI_DATA);
			CombatSettings settings;
			settings.substeps = 2;
			made.enableCombat(combatData, settings);
			made.enableNavigation(test::openWorldNav(world, zones));
			AiSystem::Config ai;
			ai.registry = std::make_shared<BehaviourRegistry>(made.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
			ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
			ai.factions = FactionBook::build(made.gameData());
			made.enableAi(std::move(ai));
			return made;
		};
		simlink::ServerContext context;
		context.squadLimit = 50;

		Simulation first = fresh();
		context.gameData = gameData;
		scenario::check(*gameData, file);
		const Outcome full = run(first, *gameData, *combatData, context, file);
		test::require(full.weakBreakRefused, "a break by the weak Picker was refused (the shackles stay locked)");
		test::require(full.cutDone, "Picker's cut took the shackles off within " + std::to_string(kCutSeconds) + " s");
		test::require(full.cutBar, "Picker's cut showed a kind 7 bar");
		test::require(full.pickOpen && full.pickWorn && full.pickBar, "Spare's pick opened the shackles, still worn, with a kind 1 bar");
		test::require(full.strongBroke && full.strongWorn, "Strong's break opened the shackles at once, still worn");
		std::cout << "pick, cut, refused break and break: ok (the cut took " << full.cutSeconds << " s, the pick " << full.pickSeconds << " s)\n";

		// The control: Strong without strength cannot break, Picker without the hacksaw cannot cut.
		scenario::File without = file;
		for (scenario::Char& c : without.chars)
		{
			if (c.id == 4)
				c.stats.erase("strength");
			if (c.id == 2)
				c.items.clear();
		}
		Simulation other = fresh();
		const Outcome weaker = run(other, *gameData, *combatData, context, without);
		test::require(!weaker.strongBroke, "Strong's break still works without its strength");
		test::require(!weaker.cutDone, "Picker's cut still works without a tool");
		std::cout << "ksim_shackled_escape_scen ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
