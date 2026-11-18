// kenshi-k16.6.65: Simulation::reload (LOAD_GAME, NEW_WORLD, SCENARIO_LOAD) puts a world in the place of one that ran, so it must leave nothing of the old
// world behind: a scenario played, replayed in the same process (newWorld + apply) and played from a fresh Simulation gives the same state hash every
// 20 ticks; so does a save loaded into the running Simulation against the same save restored into a fresh one. Also: a scenario clock before a new game's
// start is refused (the pristine stamps would be in the future), and ScenarioRun::elapsedSeconds never wraps.
// Needs the world navmesh bake with the Hub region (argv[1], exit 77 without it).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/save/scenario_run.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "nav_source.hpp"

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

constexpr int kRunTicks = 3000; // 150 game s: the player is downed and the capture chain (strip, restrain, carry) runs
constexpr int kHashEvery = 20;

struct Rig
{
	std::string world;
	std::vector<kworld::ZoneCell> zones;
	std::shared_ptr<const CombatData> combatData;
	scenario::File file;
	simlink::ServerContext context;

	// What a server does to a Simulation before it plays: combat, the world mesh and the AI.
	void enable(Simulation& sim) const
	{
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combatData, settings);
		sim.enableNavigation(test::openWorldNav(world, zones));
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim.gameData());
		sim.enableAi(std::move(ai));
	}
};

// The state hash every kHashEvery ticks over kRunTicks, the first one before any tick.
std::vector<std::uint64_t> play(Simulation& sim, int ticks)
{
	std::vector<std::uint64_t> hashes{sim.stateHash()};
	for (int tick = 1; tick <= ticks; ++tick)
	{
		sim.tick();
		if (tick % kHashEvery == 0)
			hashes.push_back(sim.stateHash());
	}
	return hashes;
}

// The scenario's placed world with the player attacking the Slaver boss (slavers_downed: the capture chain follows, whose world tasks keep blocks and carry stamps).
scenario::Placed begin(Simulation& sim, Rig& rig)
{
	sim.newWorld();
	sim.run(1);
	const scenario::Placed placed = scenario::apply(sim, *sim.gameData(), *rig.combatData, rig.context, rig.file, 3);
	sim.enqueue(Command::fighterAttack(state::Id{placed.idOf.at(1)}, state::Id{placed.idOf.at(2)}));
	return placed;
}

void requireSame(const std::vector<std::uint64_t>& a, const std::vector<std::uint64_t>& b, const std::string& what)
{
	test::require(a.size() == b.size(), what + ": same number of samples");
	for (std::size_t i = 0; i < a.size(); ++i)
		test::require(a[i] == b[i], what + ": the state hashes differ at tick " + std::to_string(i * kHashEvery));
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		Rig rig;
		rig.file = scenario::parseFile(KSIM_SCENARIO);
		const kworld::ZoneCell zone = kworld::zoneCellOf(rig.file.centreX, rig.file.centreZ);
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), zone.x, zone.z))
		{
			std::cout << "ksim_reload_hygiene: SKIPPED: needs the world navmesh bake with the Hub region (pass its directory)\n";
			return 77;
		}
		rig.world = argv[1];
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				rig.zones.push_back({zone.x + dx, zone.z + dz});
		}
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions spawn;
		spawn.seed = 7;
		const auto fresh = [&]
		{
			auto sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, spawn));
			sim->markPristine();
			return sim;
		};
		const std::unique_ptr<Simulation> first = fresh();
		Simulation& sim = *first;
		rig.combatData = CombatData::build(*sim.gameData(), KENSHI_DATA);
		rig.context.gameData = sim.gameData();
		rig.context.squadLimit = 50;
		rig.enable(sim);

		// ---- SCENARIO_LOAD / NEW_WORLD: the replay is the first play is a fresh process's play ----
		const scenario::Placed placedNow = begin(sim, rig);
		const std::vector<std::uint64_t> firstPlay = play(sim, kRunTicks);
		// The orders (arrest, lock) and the dialogue throttle are runtime only: one is given now, and the reload must drop it with the world.
		test::require(sim.orderArrest(placedNow.idOf.at(2), placedNow.idOf.at(1), 240), "an arrest order is accepted");
		test::require(sim.worldRuntimeEntries() > 0, "the world holds derived runtime state");
		sim.newWorld();
		test::require(sim.worldRuntimeEntries() == 0, "a reload leaves none of the replaced world's derived state");
		begin(sim, rig); // the second play, in the same process: its clock goes back, its ids come round again
		const std::vector<std::uint64_t> replay = play(sim, kRunTicks);
		const std::unique_ptr<Simulation> other = fresh();
		rig.enable(*other);
		begin(*other, rig);
		const std::vector<std::uint64_t> freshPlay = play(*other, kRunTicks);
		test::require(freshPlay.front() != freshPlay.back(), "the scenario changes the world over the run");
		requireSame(replay, freshPlay, "the replay against a fresh process");
		requireSame(firstPlay, freshPlay, "the first play against a fresh process");
		std::cout << "scenario replay: " << freshPlay.size() << " samples equal\n";

		// ---- LOAD_GAME: a save loaded into the running Simulation is one loaded into a fresh process ----
		begin(sim, rig);
		for (int tick = 0; tick < 400; ++tick)
			sim.tick();
		std::ostringstream saved;
		sim.save(saved);
		const std::string bytes = saved.str();
		for (int tick = 0; tick < 600; ++tick)
			sim.tick(); // the world moves on: every stamp it made is ahead of the save's clock
		{
			std::istringstream in(bytes);
			sim.reload(in);
		}
		const std::vector<std::uint64_t> reloaded = play(sim, kRunTicks);
		std::istringstream in(bytes);
		Simulation loaded = Simulation::restore(World::load(KENSHI_DATA), in, {}, false, nullptr, sim.gameData());
		rig.enable(loaded);
		const std::vector<std::uint64_t> restored = play(loaded, kRunTicks);
		requireSame(reloaded, restored, "a save reloaded in-process against restored into a fresh Simulation");
		std::cout << "load_game: " << restored.size() << " samples equal\n";

		// ---- a scenario clock before a new game's start is refused ----
		{
			scenario::File early = rig.file;
			early.day = 0;
			early.hour = 8;
			early.minute = 0;
			sim.newWorld();
			sim.run(1);
			bool refused = false;
			try
			{
				scenario::apply(sim, *sim.gameData(), *rig.combatData, rig.context, early, 3);
			} catch (const std::exception& e) {
				refused = std::string(e.what()).find("before a new game's start") != std::string::npos;
			}
			test::require(refused, "a scenario clock before day 1 10:00 is refused");
		}

		// ---- the run clock never wraps ----
		{
			simlink::ScenarioRun run;
			run.reset("x", 0, 0, 100, {});
			test::require(run.elapsedSeconds(50) == 0.0 && run.elapsedSeconds(140) == 40 * kTickSeconds, "ScenarioRun::elapsedSeconds is 0 below its start");
			test::require(ticksSince(5, 9) == 0 && ticksSince(9, 5) == 4, "ticksSince saturates at 0");
		}
		std::cout << "ksim_reload_hygiene ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
