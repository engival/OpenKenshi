#pragma once

// What a character that may not walk through a locked door does about it (DOORS.md section 6 "Bash vs pick vs wait",
// DOORS_LIVE.md section 16). The original decides with its GOAP planner: the plan with the highest score wins, a plan scores by
// its lowest step utility, utility <= 0 is dropped. There is no random roll and no crime check. The measured utilities:
//   UNLOCK_DOOR      the building's faction only (isObjectMine); no skill
//   BASH_DOOR  1.0   a melee weapon in hand (a carried one is equipped first), a door that is not mine and not allied (dt43 G); the
//                    score is ours, from DOORS_LIVE: against a non-hostile
//                    owner only a law, military, slave, slaver or bandit faction scores (traders, civilians, diplomats and
//                    adventurers score 0)
//   BASH_GATE  1.0 / mates   the same, for a gate: divided by the squad-mates already on it; allowed with fewer than 8 and when
//                    not under melee attack. UNSURE: no weapon is needed (raids bash gates unarmed, 0.3 per hit)
//   PICK_LOCK  0.1   empty hands (UNEQUIP_WEAPON sheathes first), any owner (dt43 G: no owner atom); lock level - Lockpicking < 41.7 (`minimum lockpick chance` 5 x 0.01)
//   wait             otherwise, keeping the goal
// This file is the pure decision (no sim state): interact.cpp gathers the facts. The lockpicking chance formula is the
// exe's (Task_PickLock tick, FUN_140297e50): chance = 0.9 / 2^((level - skill) / 10) while level > skill, else 0.9, one roll
// per attempt interval.

#include "kdata/format.hpp"

#include <cstdint>

namespace ksim {

// CharacterTypeEnum (FACTION `fundamental type`, default 4 civilian).
enum class FactionKind : std::int32_t
{
	None = 0,
	LawEnforcement = 1,
	Military = 2,
	Trader = 3,
	Civilian = 4,
	Diplomat = 5,
	Slave = 6,
	Slaver = 7,
	Bandit = 8,
	Adventurer = 9,
};

// The kinds whose factions bash a door of a non-hostile, non-allied owner (utility 1.0; the rest 0).
bool kindBashesDoors(std::int32_t kind) noexcept;

// The exe's global relation thresholds (FACTIONS.md 3.1): at or below is hostile, at or above is allied.
inline constexpr double kHostileRelation = -30.0;
inline constexpr double kAlliedRelation = 50.0;

inline constexpr double kBashUtility = 1.0;
inline constexpr double kPickUtility = 0.1;
// lock level - Lockpicking must stay below this for PICK_LOCK (MEASURED, DOORS_LIVE.md: 5 x 0.01 minimum chance).
inline constexpr double kPickMaxDifficulty = 41.7;
inline constexpr int kGateCrewCap = 8;
// The top of the chance curve and its halving distance in levels (the exe's constants 0.9 and 0.1 per level).
inline constexpr double kPickBestChance = 0.9;
inline constexpr double kPickLevelsPerHalving = 10.0;
// The time between two attempts (MEASURED, LOCKS_EQUIP_STOCK_LIVE.md c; the lock record's +0x10 / +0x14 / +0x1c): the first
// attempt comes after 5 game-seconds, every failure re-rolls the interval uniform in [5, 20). The elapsed time and the current
// interval live on the lock (state::Door), so they survive an interruption and are shared between pickers.
inline constexpr double kPickIntervalMin = 5.0;
inline constexpr double kPickIntervalMax = 20.0;
inline constexpr double kPickFirstInterval = 5.0;

// The lock timer state (the three numbers of state::Door that belong to the lock).
struct LockTimer
{
	double elapsed = 0.0; // game-seconds since the last attempt (lock +0x18)
	double interval = kPickFirstInterval; // game-seconds until the next one (lock +0x1c)
	std::uint32_t attempts = 0; // rolls so far: the seed of the next dice
};

enum class PickTick : std::uint8_t
{
	Waiting, // no attempt yet
	Failed, // an attempt failed; the interval was re-rolled
	Unlocked, // an attempt succeeded
};

// Lock::tick: elapsed += dt; when it reaches the interval, elapsed = 0 and one roll of `chance` against the dice of
// (lock key, picker, attempts). A failure re-rolls the interval. Deterministic in its inputs.
PickTick tickLock(LockTimer& lock, double dt, double chance, std::uint64_t lockKey, std::uint64_t picker) noexcept;

// Max HP of a door / gate (LOCKS_EQUIP_STOCK_STATIC.md a): the sum of its record's `construction` counts when that list is
// non-empty, else the legacy int `build materials`. The original gives a record with neither (sum 0) HP 1.0, born complete.
double doorMaxHealth(const kdata::Record& door) noexcept;

// `door move speed` (openness per game-second; the ctor default 0.5 when the record has none) and the time an open or close
// takes: 1 / speed game-seconds. A speed of 0 or less would never finish; it is floored at 0.1 (10 s).
double doorMoveSpeed(const kdata::Record& door) noexcept;
double doorMoveSeconds(double speed) noexcept;

// A door's lock level (LOCKS_EQUIP_STOCK_LIVE.md f): clamp(randint(-random, +random) + base + bonus, 1, 100), one uniform
// integer over the 2 random + 1 values, drawn from `key` (rolled once at construction and saved).
std::int32_t rollLockLevel(std::int32_t base, std::int32_t random, std::int32_t bonus, std::uint64_t key) noexcept;

enum class DoorPlan : std::uint8_t
{
	Wait = 0,
	Unlock = 1, // the owner's way: 3.3 s, then open
	Bash = 2,
	Pick = 3,
	Open = 4, // an unlocked shut door: OPEN_DOOR alone
};

struct DoorFacts
{
	bool mine = false; // isObjectMine
	bool hasOwner = false; // the door has an owning faction
	std::int32_t ownerKind = 0; // CharacterTypeEnum of the owner's faction
	bool hostile = false; // the owner is hostile to the actor (a factionless actor counts as hostile)
	bool allied = false; // the owner is allied with the actor
	bool gate = false;
	int mates = 0; // squad-mates already bashing this door
	bool underAttack = false; // a melee attack is running against the actor
	bool weaponInHand = false; // a melee weapon is wielded
	bool weaponCarried = false; // a melee weapon is in the pack (the planner adds an equip step)
	bool locked = true; // isDoorLocked (a shut door that is not locked plans OPEN_DOOR alone)
	bool workingArm = true; // hasOneWorkingArm: the pick and the cuts need one
	std::int32_t lockLevel = 40;
	double lockpicking = 1.0;
};

struct DoorDecision
{
	DoorPlan plan = DoorPlan::Wait;
	double utility = 0.0;
	bool equipFirst = false; // Bash with a carried weapon: equip it before the first hit
	bool unequipFirst = false; // Pick with a wielded weapon: sheathe it first (row 7)
};

// The chance of one lockpicking attempt (0..0.9).
double pickChance(double lockLevel, double skill) noexcept;

// The other two ways through a lock (task ds/shackles-locks). One Lock model serves a door, a cage and a worn shackle; what differs is where the
// Lock lives (kinspect dt24_picklock.md section 8). Task_CutLock / Task_CutShackles (tick 140359f80, dt24_shackles.md section 6) drain the lock's
// hardness (lock +4, "work value") by `2 * lerp(0.05, 0.2, strength / 100)` per game second and open it at 0; Task_BruteForceLock /
// Task_BruteForceShackles (tick 140352c50, section 7) break it at once when strength exceeds 3 x hardness, else refuse. A caged picker's
// skill is lowered by 10 (item28 b2), and iHaveSufficientLockSkill needs the chance above `minimum lockpick chance` (5 x 0.01).
inline constexpr double kCutDrainMin = 0.05;
inline constexpr double kCutDrainMax = 0.2;
inline constexpr double kCutRate = 2.0;
inline constexpr double kBruteFactor = 3.0;
inline constexpr double kCagePickSkillPenalty = 10.0;
inline constexpr double kMinPickChance = 0.05;

// Hardness drained per game second by a cutter of this strength (0..100).
double cutRate(double strength) noexcept;
// Whether a brute-forcer of this strength breaks a lock of this hardness.
bool bruteBreaks(double strength, double hardness) noexcept;
// iHaveSufficientLockSkill: the chance against a lock of `level` clears the minimum (a caged picker pays the penalty).
bool sufficientLockSkill(double level, double skill, bool caged) noexcept;

// The walk-to-the-door tasks' destinations (dt24_movetodoor section 4; 82 / 83 stand at the door, 163 inside, 199 outside). The exe reads the
// two stand points off the DoorStuff (+0x3c8 inside, +0x3bc outside); our door record carries the slab (centre, axis, half depth) and the
// unit normal that points into its building, so the points are derived: the slab's face, kDoorFaceOut (interact.hpp) off it. UNSURE: the
// exe's distance from the slab and the lerp factor f of its stand-point blend; a door with no known building (no inward normal) has no
// inside, so Inside / Outside fall back to the side `from` stands on.
enum class DoorSide : std::uint8_t
{
	Near = 0, // 82 / 83: the face on the walker's side (routeToInteract's rule)
	Inside = 1, // 163
	Outside = 2, // 199
};
struct DoorSlab
{
	double x = 0.0;
	double z = 0.0;
	double axisX = 1.0;
	double axisZ = 0.0;
	double halfDeep = 0.0;
	double inX = 0.0; // the unit normal into the building; 0, 0 = unknown
	double inZ = 0.0;
};
struct DoorPoint
{
	double x = 0.0;
	double z = 0.0;
};
DoorPoint doorStandPoint(const DoorSlab& slab, DoorSide side, double fromX, double fromZ, double faceOut) noexcept;

// The planner's choice at a locked door (door_goap.cpp): the plan against DOOR_IS_OPEN over the registry rows, each scored by the utility above.
// Wait = no plan (the actor waits, or gives the walk up after kDoorWaitCap).
DoorDecision decideDoor(const DoorFacts& facts);

} // namespace ksim
