#pragma once

// G2, the original's goal selector (kinspect dt14_goal_selector.md; RUNTIME_SYSTEMS G2). One pass per full AI update over the
// member's candidate goals: the package's goal rows of the member's role, each with its hour window and weight, scored
//   score = utility x weight x crowd decay
// (all of dt14 section 4; utility is the task's utility times its resolver's score, and a resolver with no target is 0). Candidates
// go in strict tiers (3 URGENT, 2 NON_URGENT, 1 FLUFF): the first tier with a candidate wins, the highest score inside it. The
// running goal keeps a 1.25 bonus and, while its hold runs, a 0.4 floor; once the hold has expired its re-scored copy is capped at
// 0.05, so anything that can compete displaces it. Nothing scoring at all: the default task (IDLE) at 1.0 - and even that failing
// returns no goal, and the next update re-scores (dt14 section 8). There is no terminal state.
//
// UNSURE (dt14 section 11): the held-goal byte's exact meaning (ours: the member's current goal, always "held" while it runs), the
// constant the post-hold cap compares against (ours: the cap applies unconditionally, dt9's recorded reading), and the order/job
// scoring bodies, which this slice does not model.
//
// Pure: no Simulation, no flecs, no save. The caller builds Offers (its resolvers) and commits the Choice.

#include <cstdint>
#include <vector>

namespace ksim::goalsel {

// dt14 section 9.
inline constexpr double kCurrentBonus = 1.25; // DAT_14168ba14: the running goal's multiplier
inline constexpr double kHoldFloor = 0.4; // DAT_141696c90: its floor inside the minimum duration
inline constexpr double kHoldCap = 0.05; // DAT_141696c74: its cap once that has expired
inline constexpr double kDefaultScore = 1.0; // DAT_14167c308: the default task's score
inline constexpr std::uint16_t kIdleTask = 0; // AI_TASK IDLE (ORIGINAL_AI 8a: the default task)

// The hold lengths by goal (hours): base + rand x span. OURS: the town scorer's table (routines.hpp holdOf) is the only one read;
// a goal without one has no hold, and then its re-chosen copy is simply the 1.25 bonus.
struct Hold
{
	double base = 0.0;
	double span = 0.0;
	bool none() const noexcept { return base == 0.0 && span == 0.0; }
};

// One candidate: one package row for this member, with its resolver's outcome.
struct Offer
{
	std::uint16_t task = 0;
	int tier = 2; // routines::Tier: 3 URGENT, 2 NON_URGENT, 1 FLUFF
	double utility = 0.0; // the task's utility x its resolver's score; 0 = the resolver found nothing
	double weight = 1.0; // the row's weight (routines::effectiveWeight)
	double crowdDecay = 1.0; // the task's crowd decay (dt9: 0.8 medic / repair / builder / splint, 0 eat / kill-caged, 1 default)
	int others = 0; // squadmates on the same task (AIPackage::countOthersOnTask)
	std::uint16_t start = 0;
	std::uint16_t end = 24;
	std::uint64_t target = 0; // the resolver's target (the same-goal test); 0 for a targetless goal
	bool holds = false; // the goal's hold byte: its minimum duration applies
	Hold hold;
};

// The running goal, for the same-goal test and the hold branch.
struct Current
{
	std::uint16_t task = 0;
	std::uint64_t target = 0;
	bool insideHold = false; // the minimum duration has not elapsed (the caller keeps the timer)
};

// Why one offer did or did not win (the AI decision trace, SIMLINK.md "DEBUG VIEW"). Read only: select fills it on request.
enum class Why : std::uint8_t
{
	Chosen, // the pass's pick
	NoTarget, // scored 0: the resolver found nothing (utility 0), or the crowd decay zeroed it
	OutOfWindow, // the row's hour window is closed
	LowerTier, // a higher tier had a candidate, so this tier was never compared
	Outscored, // a candidate of the same tier scored higher (or tied and came later)
	HoldCapped, // the running goal, its hold expired: re-scored copy capped at kHoldCap, and it lost
};

struct Verdict
{
	Why why = Why::NoTarget;
	double score = 0.0; // scoreOf: the 1.25 bonus, hold floor and cap applied; 0 = not a candidate
};

struct Choice
{
	bool found = false; // a real goal scored
	bool isDefault = false; // nothing scored: default_task (IDLE)
	std::uint16_t task = kIdleTask;
	double score = 0.0;
	int tier = 0;
	std::uint64_t target = 0;
};

// [start, end) in game hours: wraps when start > end; any end > 24 covers the hour; start == end is always on (dt14 section 3).
bool windowHolds(std::uint16_t start, std::uint16_t end, double hour) noexcept;
// decay^n (n = squadmates on the task; n <= 0 = 1).
double crowdFactor(int others, double decay) noexcept;
// The score of one offer, 0 = not a candidate (dt14 section 4).
double scoreOf(const Offer& offer, const Current& current, double hour) noexcept;
// The pass: tiers 3, 2, 1 in order; inside a tier the highest score, a tie to the later offer (the exe's 0.0001 bump); nothing
// scoring = the default task. The caller still has to commit (dt14's commit checks). `verdicts` (null = the plain pass, nothing
// else runs) gets one Verdict per offer, in offer order; it never changes the Choice.
Choice select(const std::vector<Offer>& offers, const Current& current, double hour, std::vector<Verdict>* verdicts = nullptr) noexcept;
// A short name for a Why ("Chosen", "NoTarget", ...).
const char* whyName(Why why) noexcept;

} // namespace ksim::goalsel
