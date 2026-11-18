#pragma once

// Soak metrics for the squad AI (ARCHITECTURE.md "Validation"): run a
// Simulation with the AI on for N game-days and measure what must stay at
// zero or flat: squads with no goal, goal failures by reason, squads stuck
// (a travel goal whose squad has not moved for N game-hours, measured from
// outside the AI), arrivals.

#include "ksim/ai.hpp"
#include "ksim/profile.hpp"
#include "ksim/simulation.hpp"

#include <array>
#include <cstdint>
#include <iosfwd>

namespace ksim {

struct SoakOptions
{
	double days = 1.0;
	// Game-hours without moving on a travel goal before a squad counts as stuck.
	std::uint64_t stuckHours = 6;
	// Every N game-hours (0 = never) a town changes hands to a faction hostile to its owner and
	// goes back 12 hours later: the owner loses a town (events up, Defend, its gates on Alert).
	// Needs the door system. A stand-in for the conquest that does not exist yet.
	std::uint64_t provokeEveryHours = 0;
	// The living-world CSVs (docs/reference/LIVING_WORLD.md), pure reads written after every game-hour. Null = off.
	//   csv: one row per hour, world totals (kSoakCsvHeader).
	//   factionCsv: one row per (hour, faction) that has a squad (kSoakFactionCsvHeader); factionless squads are "none".
	std::ostream* csv = nullptr;
	std::ostream* factionCsv = nullptr;
	//   playerCsv: one row per (hour, member of a PlayerSquad), squad then member order (kSoakPlayerCsvHeader, the oracle's player.csv).
	std::ostream* playerCsv = nullptr;
};

// Column names. deaths_offscreen_fight and deaths_bled_out count the bodies by CorpseDecay::cause (2 token fight, k16.6.46; 1 bled out off-screen,
// k16.6.52), deaths_detail every other body; each body once, at the first hourly sample that sees it.
inline constexpr const char* kSoakCsvHeader = "hour,day,squads,members,squads_gone,respawns,other_new_squads,deaths_detail,"
	"deaths_offscreen_fight,deaths_bled_out,corpses,raids_started,towns_raided";
inline constexpr const char* kSoakFactionCsvHeader = "hour,faction,squads,members";
// blood, down (a collapsed leg or torso), ko (unconscious) and dead as the oracle writes them (True/False); x, y, z the member's position.
inline constexpr const char* kSoakPlayerCsvHeader = "hour,name,blood,down,ko,dead,x,y,z";

struct SoakReport
{
	std::uint64_t ticks = 0;
	std::uint64_t samples = 0; // one per game-hour
	AiAudit last; // after the final tick
	std::size_t maxGoalless = 0; // worst sample
	std::size_t maxOffRoute = 0;
	std::size_t stuckNow = 0;
	std::uint64_t stuckEpisodes = 0; // squads that crossed the threshold
	// Over the run, from AiStats.
	std::uint64_t selections = 0;
	std::uint64_t arrivals = 0;
	std::array<std::uint64_t, state::kAiReasonCount> failures{};
	std::size_t squadsThatMoved = 0;
	double seconds = 0.0;
	// Town life when no player is near (TOWN_LIFE_SLICE 5.3 LOD, T3f): the peak number of squads in each unloaded phase over the run's
	// hourly samples, and the peak count of window-valid leader rows the scorer does not run.
	std::array<std::size_t, routines::kTownPhaseCount> maxByPhase{};
	std::size_t maxUnmodelled = 0;
	// The real-economy overlay (TOWN_LIFE_SLICE 7, T4a) at the final hourly sample: towns with a TownStock, their lines, lines at
	// zero stock, the mean fill (0..1) and the EconomyProducers. All zero in `original` mode (no T4 state exists).
	std::size_t economyTowns = 0;
	std::size_t economyLines = 0;
	std::size_t economyEmptyLines = 0;
	std::size_t economyProducers = 0;
	double economyFill = 0.0;
#if KSIM_PROFILE
	// The per-system tick timings over the run (Totals, subtracted from the profiler's start) and over its last kWindow ticks (docs/PROFILING.md).
	profile::Totals profile;
	profile::Summary profileWindow;
#endif
};

// Needs sim.ai() (Simulation::enableAi). Runs whole game-hours: the tick count
// is days * 24 * kTicksPerGameHour, rounded.
SoakReport runSoak(Simulation& simulation, const SoakOptions& options);

} // namespace ksim
