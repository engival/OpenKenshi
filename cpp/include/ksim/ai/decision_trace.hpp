#pragma once

// The AI decision trace (docs/formats/SIMLINK.md "DEBUG VIEW", kenshi-k16.6.43): one Pick per member per goal pick that ran, with
// every candidate's tier / utility / weight / crowd factor / score / verdict. The two goalsel callers (the world squads' selector in
// world_tasks.cpp, the town scorer in routine_scorer.cpp) build it only while Simulation::setDecisionTrace has a sink; it is
// observation only: no RNG draw, no state change, nothing saved. The debug view keeps a ring per member; the server's
// --trace-member logs formatPick.

#include "ksim/ai/goalsel.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ksim::trace {

enum class Source : std::uint8_t
{
	WorldSelector, // world_tasks.cpp: a built world squad's member
	TownScorer, // routine_scorer.cpp: a town resident (Routine)
	Combat, // combat.cpp pickTarget (kenshi-k16.6.59): a fighter's combat goal / target pick, recorded only when it changes
};

enum class Hold : std::uint8_t
{
	None, // nothing running
	Inside, // the running goal's minimum duration has not elapsed
	Expired, // it has: the re-scored copy is capped
};

enum class Result : std::uint8_t
{
	Chosen, // a real goal scored
	DefaultIdle, // nothing scored: the default task (IDLE); the member keeps what it is doing
	Fallback, // the town scorer's STAY_IN_HOME (nothing plannable)
};

struct Candidate
{
	std::uint16_t task = 0;
	int tier = 0;
	std::uint64_t target = 0;
	double utility = 0.0;
	double weight = 1.0;
	double crowd = 1.0; // goalsel::crowdFactor(others, decay)
	double score = 0.0;
	goalsel::Why verdict = goalsel::Why::NoTarget;
};

struct Pick
{
	std::uint64_t tick = 0;
	std::uint64_t member = 0;
	Source source = Source::WorldSelector;
	std::uint16_t runningTask = 0; // 0 = none
	std::uint64_t runningTarget = 0;
	Hold hold = Hold::None;
	std::vector<Candidate> candidates; // in offer order
	Result result = Result::DefaultIdle;
	std::uint16_t task = 0;
	std::uint64_t target = 0;
	double score = 0.0;
	int tier = 0;
};

// What Simulation::setDecisionTrace takes. `wants` (null = every member) is asked before any record is built, so a sink that
// follows one member costs the others nothing.
struct Sink
{
	std::function<bool(std::uint64_t member)> wants;
	std::function<void(const Pick&)> record;
};

// The Combat source's rows (Simulation::installFightTrace). The pick is one rule, not goalsel's offers: the picker scores every admitted
// enemy (1 - distance / 200) x 2 - crowd, +5 for the current target (+10 more while fewer than two attack it), +10 for a charge's attacker.
// So `task` = the combat goal that admitted the enemy (5 attack order, 16 / 21 attackers, 34 troublemaker, 143 / 15 protect ...),
// `target` = the enemy, `utility` = the distance term, `crowd` = the engaged-plus-refusals COUNT (not goalsel's factor), `weight` 1,
// `tier` 0, `score` the picker's, `verdict` Chosen / Outscored. `running` = the (goal, target) last reported; `hold` None; `Result`
// Chosen, or DefaultIdle with task 0 when the fighter let go. A player's attack order has no candidates.

// A Pick from the offers and their verdicts (select's out-vector); the caller sets the result.
Pick makePick(std::uint64_t tick, std::uint64_t member, Source source, const goalsel::Current& running,
	const std::vector<goalsel::Offer>& offers, const std::vector<goalsel::Verdict>& verdicts);

const char* sourceName(Source source) noexcept;
const char* holdName(Hold hold) noexcept;
const char* resultName(Result result) noexcept;

// The --trace-member line: one per pick, the candidates after " | " each, e.g.
//   trace tick 1200 member 77 TownScorer running 43 hold Expired -> Chosen 1 target 5 score 1.25 tier 2 | 43 t2 u1 w1 c1 s0.05 HoldCapped | ...
std::string formatPick(const Pick& pick);

} // namespace ksim::trace
