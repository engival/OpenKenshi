#pragma once

// G1, the AI package stack (kinspect dt19_package_stack.md; RUNTIME_SYSTEMS G1). Each platoon (a squad in the sim) holds one
// Blackboard: a priority table `priority -> package` (highest wins), the applied package, and one overriding timed contract.
// Every tick a ladder walks the table from the highest registered priority down; the first priority that yields a running
// package wins, and its merged goal rows (the PackageTable's lists for that record) are what the goal selector scores.
//
// There is no LIFO stack and no "current package per character": the character reads its platoon's board (dt19 section 0).
// Contracts do not touch orders; a contract is a package that overrides the table while unfinished (dt19 section 8). The
// package's `signal func` (a subclass id, dt19 section 5) is what can finish it: Contract_Timed at its expiry and
// Contract_HiredAlly at its expiry unless its "ended" flag is set. The F classes (dt41_F_packages.md: slavers, patrols, assault,
// occupy, trader, war, gathering) have their own should-start / finished / can-lead tests, reading a `Situation`; every other id is the
// base AIPackage, which starts and never finishes.
//
// Kept faithful, quirks included: a finished package whose `Leads to` is not registered (or which may not follow it - the base
// class always may) drops the whole priority level rather than trying the bucket's other packages; a priority-0 bucket with one
// entry applies it and still returns "not handled", so the ladder carries on below; `change AI` does not end an active
// contract, so the contract keeps overriding the installed package until it finishes. `clears existing jobs` is never read by
// the original (the literal is not in the exe) and is not modelled here.

#include "ksim/core/state.hpp"

#include "kdata/gamedata.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ksim::blackboard {

// BlackboardSignalFunctions ids this slice models (dt19 section 5, dt41 F). Every other id behaves as the base class: it starts, never
// finishes.
inline constexpr std::int32_t kContractHiredAlly = 0xc;
inline constexpr std::int32_t kContractTimed = 0x21;
// The F classes (dt41_F_packages.md, factory 140285ba0): the signal classes with their own lifecycle slots.
inline constexpr std::int32_t kCannibalGoOutOnPatrol = 4;
inline constexpr std::int32_t kRetreatAndRefresh = 5;
inline constexpr std::int32_t kGoOutOnPatrol = 6;
inline constexpr std::int32_t kPrisonerGathering = 0xa;
inline constexpr std::int32_t kSlaverCamps = 0xb;
inline constexpr std::int32_t kWanderingTrader = 0x10;
inline constexpr std::int32_t kWarBase = 0x13;
inline constexpr std::int32_t kWarAssault = 0x14;
inline constexpr std::int32_t kGoOutOnPatrolRunning = 0x17;
inline constexpr std::int32_t kPrisonerGatheringUnholy = 0x18;
inline constexpr std::int32_t kSlaverAnywhere = 0x1c;
inline constexpr std::int32_t kPassingByTownAssault = 0x1d;
inline constexpr std::int32_t kOccupyTown = 0x1e;
inline constexpr std::int32_t kWarAssaultCannibal = 0x20;
// GoOutOnPatrol finishes after this long (DAT_1416adc00 = 12.0, F section 10; the unit is the exe's game time, read as hours, UNSURE).
inline constexpr double kPatrolHours = 12.0;
// PassingByTownAssault may not finish before this long (DAT_1416aeda0 = 0.25) and finishes, leading on to OccupyTown, once more than one member
// is engaged (rwh.53 section 2.4: vf13 reads each member's vf+0x118 > 0; kinspect dt41 F read "fewer than 2 capable members", which no raid
// reaches, so the stay would never start).
inline constexpr double kAssaultMinHours = 0.25;
inline constexpr int kAssaultEngaged = 2;
// OccupyTown finishes once no living, standing member has been in its town for this long (DAT_14168bf24 = 4.0 s of frame time, rwh.53 section 3;
// the board's hours are game hours: 4 s of play, kGameDaySeconds in a day).
inline constexpr double kOccupyHours = 4.0 * 24.0 / 2618.1819;
// PrisonerGathering: finished when the mission count is at most members x this (DAT_14168a254 = 3.0) and no gather target is found.
inline constexpr double kGatherPerMember = 3.0;
// WanderingTrader keeps wandering by day (F section 9): the hour is above the first and below the second.
inline constexpr double kDaylightFrom = 6.0;
inline constexpr double kDaylightTo = 21.0;
// A contract pushed with `hours < 1` lasts 10 min (DAT_1416ad5b8, dt19 section 9).
inline constexpr double kShortContractHours = 1.0 / 6.0;
// A campaign phase package is pushed as a 24 h contract (dt19 section 2 step 1).
inline constexpr double kCampaignHours = 24.0;

// One registered package: the exe's AIPackage "signal object", reduced to what the ladder and the goal selector read.
struct Package
{
	std::string sid; // the AI_PACKAGE record
	std::int32_t priority = 0;
	std::int32_t signalFunc = 0; // its `signal func`
	std::string leadsTo; // its `Leads to` record, kept only when that record is registered too (resolved at registration)
};

struct Board
{
	std::map<std::int32_t, std::vector<Package>> table; // priority -> packages, registration (data) order
	std::int32_t maxPriority = -1; // the ladder's start
	std::string replacement; // the `change AI` package ("" = the table is the data's); the one thing in the table that is saved
	std::string current; // the applied package ("" = none)
	// Per-package runtime state of the F classes (not saved: a load restarts them, like the HiredAlly flag).
	double startedAt = 0.0; // game hours at which `current` was applied (GoOutOnPatrol +0xb0, PassingByTownAssault +0xb8)
	int traderState = 0; // Package_WanderingTrader +0xe0: 0 -> 1 -> 2 (finished) on arrivals at a town
	double occupyHeld = 0.0; // Package_OccupyTown +0xc0: game hours since a living, standing member was last in the town
	// The town OccupyTown holds (package +0x08, the TownState's entity id): set at the package's start from the platoon's current town,
	// cleared when it stops. Derived, never saved (a loaded occupation re-reads the town the leader stands in).
	std::uint64_t occupyTown = 0;
	// PassingByTownAssault +0xb4, "finished once": set the first tick two members are engaged, so a fight that is over before the minimum run
	// (kAssaultMinHours) still ends the assault, leading on, when it is up. Not saved: a loaded assault waits for its next fight.
	bool assaultEngaged = false;
	std::int32_t currentPriority = 0;
	Package contract; // the overriding timed contract (empty sid = none)
	double contractExpiry = 0.0; // game hours
	bool reapply = false; // a full re-merge is due (the exe's +0x130; our lists are compiled per record, so the flag is informational)
	bool runningAway = false; // +0x14c: freezes the ladder (the "Run Away!" state, dt19 section 7)
	// The package's sticky cage hand (AIPackage+0x88, dt24_killprisoner section 7): the cage 135 picked, kept while it still holds an
	// occupant. Saved (BoardStates). 0 = none (the hand's type 0xb).
	std::uint64_t stickyCageBuilding = 0;
	std::uint16_t stickyCageSlot = 0;
	// The platoon's ownership set (Platoon+0x148, kinspect rwh.8): the characters the squad owns, filled at the cuff, emptied by the hand-over.
	// Ascending fighter ids. The slaver packages (0xb / 0x1c) start on its size >= 1 and finish on 0. Saved (BoardStates, v95).
	std::vector<std::uint64_t> owned;
	// The slaver packages' target hand (Blackboard+0xf0): the state::Id of the town the finder picked at the package's start, 0 = none. Fixed
	// for the package's life (the exe never re-selects); `depotStamp` is the `startedAt` the finder last ran for (-1 = it has not run).
	std::uint64_t depot = 0;
	double depotStamp = -1.0;
	// The platoon's blood smell (Blackboard+0xf8 -> the first ZoneSmell's position, dt27 section 4.2): the world position 176 walks at.
	// Nothing in the exe's decompiled set writes it (U), so ours is a hook for the smell system (Simulation::setSquadSmell). Saved (BoardStates).
	bool hasSmell = false;
	double smellX = 0.0;
	double smellZ = 0.0;
};

// The campaign hook (dt19 section 2 step 1): the phase package a platoon's campaign pushes as a 24 h contract before any
// priority is considered. FACTION_CAMPAIGN is not modelled yet; the caller passes an empty one.
struct Campaign
{
	bool present = false;
	Package package;
};

// What the package classes read of the platoon and the world (dt41 F). The defaults are the neutral answers: a platoon with no
// mission, ready, with enough fighters, nothing to assault, no arrival at a town. A caller sets only what it knows.
struct Situation
{
	int owned = 0; // Platoon+0x148, the Ownerships set's size (kinspect rwh.8): the slaver packages start at >= 1 and finish at 0
	bool platoonReady = true; // 1407ac7b0, the platoon condition (meaning UNSURE: read as "fit to act"); patrol and assault end when false
	int engagedMembers = 0; // living members with a target (vf+0x118 > 0): PassingByTownAssault finishes, leading on, at kAssaultEngaged
	bool assaultTown = false; // PassingByTownAssault should-start: a town not allied, past its cooldown, leader alive, objects near
	bool targetsGone = true; // every object the assault selected is gone
	bool leaderAlive = true; // OccupyTown: the platoon leader is alive
	bool occupying = false; // OccupyTown: a living, standing member is in the occupied town this tick (140289180)
	bool atTown = false; // WanderingTrader: the platoon stands at its target town (1405944e0)
	bool townOwned = false; // WanderingTrader: the current town has an owner while the platoon's own record has none
	bool gatherTarget = true; // PrisonerGathering: its finder (vf19) returns a target
	int missionCount = 0; // PrisonerGathering: the mission's count (Platoon::vf19 +0x18)
	int members = 0;
	bool warInstance = true; // WarBase / WarAssault: the faction's campaign map still has an instance for the platoon
	double stepHours = 0.0; // the game time one tick advances (DAT_142133794)
	bool advance = true; // false: a re-read of the ladder between the owner's ticks; the classes' own tick state (trader, occupy) does not move
};

// The two slaver shipping classes (SIG_SLAVER_SHIP_TO_WORKCAMPS 11, ..._ANYWHERE_CLOSE 28): the ones that run the depot finder at their start.
constexpr bool slaverShipping(std::int32_t signalFunc) noexcept { return signalFunc == kSlaverCamps || signalFunc == kSlaverAnywhere; }

// Contract_Timed / Contract_HiredAlly: the only signal classes that finish on their own.
bool timedSignal(std::int32_t signalFunc) noexcept;

// Whether the package is finished at `nowHours`. Table packages with a timed signal read the board's single contract expiry,
// as the original's signal classes do; the base class never finishes.
bool finished(const Board&, const Package&, double nowHours, const Situation& = {}) noexcept;
// vf12 (should-start): the base class answers 1. The overrides: the slaver packages need a member list and an owned slave, OccupyTown never starts by itself,
// RetreatAndRefresh starts when the platoon is not ready, PassingByTownAssault needs a town to assault, the trader reads its state.
bool shouldStart(const Board&, const Package&, double nowHours, const Situation& = {}) noexcept;
// vf18 (may follow `Leads to`): the base class answers 1; PassingByTownAssault leads on only while ready with too few fighters.
bool canLead(const Package&, const Situation& = {}) noexcept;

// 140290450's town test (rwh.53 section 2.4): a squad assaults the town it stands in when the town is not allied to it AND the town is
// the player's (the exe asks nothing more of a player-owned town: no hostility, no war), or the squad attacks neutrals, or the town's
// owner is an enemy of it (the caravan-in-a-neutral-town exclusion). `enemy` is only read for another owner's town.
constexpr bool assaultsTown(bool allied, bool attacksNeutrals, bool playerOwned, bool enemy) noexcept
{
	return !allied && (attacksNeutrals || playerOwned || enemy);
}

// Builds a Package from its AI_PACKAGE record: `signal func` and the first `Leads to` (the record id). False when the record is
// missing or is not an AI_PACKAGE.
bool readPackage(const kdata::GameData&, std::string_view sid, std::int32_t priority, Package& out);

// 14026c160: a record already registered at the priority is skipped (the signal object is per record), else appended in data
// order; max_priority grows. The 140271620 tail (`Leads to`) is resolveLeads.
void registerPackage(Board&, Package pkg);
// Every registered package's `Leads to`, matched against the packages already registered: a successor that is not in the table
// is dropped.
void resolveLeads(Board&);
// 140272240: a contract is either already running (same record) or replaces the slot; its expiry is `hours` game-hours away
// (10 min when `hours < 1`), a missing `signal func` becomes Contract_Timed, and it becomes the applied package.
void pushContract(Board&, Package pkg, double hours, double nowHours);
// 140272430 (dialogue `change AI`): wipe the table, install one package at priority 0 and apply it. An active contract is not
// ended (the original leaves it; it keeps overriding until it finishes).
void changeAI(Board&, Package pkg);

// 1402732b0: walk the priorities from max down (unless running away), then tick the applied package (a no-op for the base
// class). The contract's own re-mark hook (Contract_HiredAlly vf9) is not modelled.
void tick(Board&, double nowHours, const Campaign& campaign = {}, const Situation& = {});
// 140272570: the campaign push, the contract override, then the priority's bucket. True when the level handled the tick.
bool consider(Board&, std::int32_t priority, double nowHours, const Campaign& campaign = {}, const Situation& = {});

// The board's runtime state for the save (state::BoardStates::Entry), and its inverse: `restore` runs on a board whose table was just rebuilt
// from the data; it re-installs the `change AI` package (read from `data`; a record gone from the data leaves the data's table), the
// contract, the applied package and the flags. The table itself is never saved.
state::BoardStates::Entry capture(const Board&, std::uint64_t squadId);
void restore(Board&, const state::BoardStates::Entry&, const kdata::GameData& data);

// The registered package with this id (its table entry or the contract), or null.
const Package* find(const Board&, std::string_view sid) noexcept;

} // namespace ksim::blackboard
