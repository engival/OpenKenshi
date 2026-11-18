#pragma once

// Lifting, carrying and putting down a character, the encumbrance a carried person causes, and NPC residents taking KO'd
// intruders out of their home (GATES_CARRY_LIVE.md c, GATES_CARRY_STATIC.md sections 3 to 5; SIMLINK.md section 13).
//
// The carrier (state::CarryStep on its entity, Fighter::carrying) and the carried (Fighter::carriedBy) are two fighters; the
// carried one has no movement of its own: Simulation::stepCarry copies the carrier's position onto it after every combat step.
// Lifting is interact action 8 on a KO'd or dead character (walk up, then LIFT); putting down is interact action 13 on the
// carried character. Both are animation-driven in the original; the measured times are the constants below.
//
// Encumbrance (CharStats+0x190) so far exists only for a carrier: capacity = `carry weight mult` x strength (effective) x vf10 +
// `encumbrance base`, W = the pack's weight + `carry person weight` (a fixed 30, not the body's), ratio = clamp01((capacity -
// 0.05 W) / (0.95 W)); the run speed is (base - 11) x ratio + 11 (knav::Gait::encumbrance). The ratio of an ordinary pack (a
// character that carries nobody) stays 1: weighing everyone's pack would change every fight (dodge, damage use it too) and
// needs its own calibration against the replays. OPEN.
//
// The NPC take-out (goals 223 -> 224): a resident of a building (a fighter inside it whose state::Allegiance is the faction that
// owns the building's doors) that finds a KO'd non-member inside lifts it, walks it out through the door (the walker's door steps
// open or unlock it; nobody closes it) and puts it down at door outside + 1.1 x (outside - inside), once outside and idle or after
// 5 game-minutes. The town version (273 -> 274), jailing and any crime or trespass event are OPEN.

#include "ksim/core/state.hpp"

#include <algorithm>
#include <cstdint>

namespace ksim {

// The lift clip "shoulder lift left" (2.4667 s, rwh.114 section 4); the body attaches at 40 % of it (0.987 s) and the task finishes at 95 % (2.343 s).
// (The 1.17 s / 0.49 s of GATES_CARRY_LIVE.md c were the original's measured wall time at another clip speed; the percentages stay, the clip is this.)
inline constexpr double kLiftClipSeconds = 2.4667;
inline constexpr double kLiftSeconds = kLiftClipSeconds * 0.95;
inline constexpr double kLiftAttachSeconds = kLiftClipSeconds * 0.4;
// PUT_DOWN: the lift clip played at rate -1.0; the carried is released at 20 % of the reversed clip (1.973 s), which is where it ends (rwh.114).
inline constexpr double kPutDownSeconds = kLiftClipSeconds * 0.8;
// A put-down non-ally starts a forced KO timer, never shorter than this (MedicalSystem::startForcedKnockoutTimer).
inline constexpr double kForcedKoMinSeconds = 3.0;
// CharStats::vf10, the multiplier inside the carry capacity. UNSURE: not decoded (GATES_CARRY_STATIC.md section 6.8); 1.
inline constexpr double kCapacityVf10 = 1.0;
// The share of the weight that costs nothing (the exe's 0.05).
inline constexpr double kEncumbranceFree = 0.05;
// An NPC carrier that was asked to walk goes at move mode 1, capped at 55 units/s.
inline constexpr double kCarryWalkCap = 55.0;
// The door's inside and outside points lie this far from its centre (xz): the drop point 'outside + 1.1 x (outside - inside)'
// is then 3.2 x this beyond the door, ~45 units, which is what item 15 measured live. UNSURE: the exe's two points are not
// read, 14 is kDoorStopReach.
inline constexpr double kDoorSideDistance = 14.0;
inline constexpr double kDropBeyondDoor = 3.2 * kDoorSideDistance;
// When the standard drop point is still inside the building's footprint, the drop point moves out in steps of this size, up to
// this far past the standard one (live Bar: 60-90 units from the door; ours is UNSURE, the search is ours).
inline constexpr double kDropSearchStep = 8.0;
inline constexpr double kDropSearchExtra = 96.0;
// 274's destination (dt24_273 section 1 / CRIME_STATIC "Town::vf38"): the first gate of the carrier's town, then this far further
// out along the town centre's ray through it (Town::vf82's 200.0, DAT_1416abee4).
inline constexpr double kTownDropOut = 200.0;
// The take-out drops its person after this long outside (5 game-minutes), whatever the mover is doing.
inline constexpr double kTakeOutOutsideCap = 300.0;
// No-terminal-state cap on a whole take-out (ours): the carrier gives up and puts the person down where it stands.
inline constexpr double kTakeOutCap = 900.0;

// The carry capacity of a character of this effective strength.
inline double carryCapacity(double strengthEffective, double weightMult, double base, double vf10 = kCapacityVf10) noexcept
{
	return weightMult * strengthEffective * vf10 + base;
}

// CharStats+0x190 for a pack weighing `weight`: the inverse lerp of the capacity between 0.05 W (ratio 0) and W (ratio 1).
inline double encumbranceRatio(double capacity, double weight) noexcept
{
	if (weight <= 0.0)
		return 1.0;
	return std::clamp((capacity - kEncumbranceFree * weight) / (weight - kEncumbranceFree * weight), 0.0, 1.0);
}

// The forced KO time (MedicalSystem::startForcedKnockoutTimer 140644980): t0 = 2 knockoutTimeBase, mult =
// lerp(knockout mult 1, knockout mult 99, toughness) x 0.25, t = ((maxBlood - blood) x mult x 2 + t0) x param x 4, at least 3 s.
inline double forcedKoSecondsParam(double knockoutTimeBase, double knockoutMult1, double knockoutMult99, double bloodMissing,
	double victimToughness, double param) noexcept
{
	const double toughness = std::clamp(victimToughness * 0.01, 0.0, 1.0);
	const double mult = (knockoutMult1 + (knockoutMult99 - knockoutMult1) * toughness) * 0.25;
	return std::max((bloodMissing * mult * 2.0 + knockoutTimeBase * 2.0) * param * 4.0, kForcedKoMinSeconds);
}

// The forced KO time of a put-down person: param = lerp(0.1, 0.4, carrier stat / 100).
// UNSURE: which stat of the carrier (strength here) and whose toughness (the carried's).
inline double forcedKoSeconds(double knockoutTimeBase, double knockoutMult1, double knockoutMult99, double bloodMissing,
	double carriedToughness, double carrierStat) noexcept
{
	return forcedKoSecondsParam(knockoutTimeBase, knockoutMult1, knockoutMult99, bloodMissing, carriedToughness,
		0.1 + 0.3 * std::clamp(carrierStat * 0.01, 0.0, 1.0));
}

} // namespace ksim
