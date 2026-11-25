#pragma once

// A playtest run (task CE, docs/reference/PLAYTEST.md 3): the scenario a session loaded (SIMLINK SCENARIO_LOAD 103), its seed and its
// start tick, and the counters the end-of-run metrics read (SCENARIO_END 104 -> SCENARIO_METRICS 56). The server samples it after every tick
// (SimLinkServer::run) while a run is active; a run never changes the world, so a scenario plays the same with and without a client.
//
// The metric names are the vocabulary SIMLINK.md "SCENARIO" lists; a scenario's header (`scenario ... metrics=a,b`) picks from it, in order.
// Every time is in game seconds since the run's start (tickDelta x Simulation::kTickSeconds). A metric the run never produced is `valid` 0.

#include "ksim/core/simulation.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ksim::simlink {

// The build's git hash (the SCENARIO_STARTED simHash), defined in the build-time generated build_info.cpp, "unknown" outside a git build.
std::string_view buildHash();

// The metrics SIMLINK.md lists, in the order a header may name them; false when `name` is not one of them.
bool isScenarioMetric(std::string_view name);

struct ScenarioRun
{
	bool active = false;
	std::string name;
	std::uint64_t fileHash = 0;
	std::uint32_t seed = 0;
	std::uint64_t startTick = 0;
	std::vector<std::string> metrics; // the header's list, in order

	// Sampled state (each value is the first/last tick it happened at).
	std::set<std::uint64_t> players; // player-controlled fighters seen during the run
	std::set<std::uint64_t> downNow; // arena members down at the last sample (bodies_down; the NPCs too, so a player-less file counts)
	std::set<std::uint64_t> everEngaged; // fighters that ever had a watch: engaged was non-empty, or its target was a player
	std::set<std::uint64_t> everTargetedPlayer; // non-players that ever targeted a player character
	std::uint64_t provokeTick = 0; // first tick a player character had a target or an engaged list
	std::uint64_t crewTick = 0; // first tick a bystander fought a character that ever targeted a player (the bar's crew)
	std::uint64_t firstEngageTick = 0;
	std::uint64_t lastEngageTick = 0;
	// The earliest engage trigger seen (CHAR_STATS row 34's tick; the lowest id breaks a tie): its basis and the AI_TASK goal. Tick 0 = none.
	std::uint64_t firstTriggerTick = 0;
	std::uint8_t firstTriggerBasis = 0;
	std::uint16_t firstTriggerGoal = 0;
	std::uint64_t firstTriggerId = 0;
	std::uint64_t firstDownTick = 0; // first tick a player character was down
	bool playerCarried = false; // a player character was carried or shackled at some tick

	// Starts a run: the world has just been swapped to the scenario (the clock jumped, the Ids are new).
	void reset(std::string scenarioName, std::uint64_t scenarioFileHash, std::uint32_t runSeed, std::uint64_t start,
		std::vector<std::string> metricNames);
	// One tick's sample, after Simulation::tick. Cheap enough for every tick: one walk of the arena.
	void sample(const Simulation& sim, std::uint64_t tick);
	// The metric's value at `tick` (the end of the run); `valid` 0 when the run never produced it. An unknown name is never valid.
	double value(std::string_view metric, std::uint64_t tick, std::uint8_t& valid) const;
	double elapsedSeconds(std::uint64_t tick) const;
};

} // namespace ksim::simlink
