#pragma once

// Corpse decay (kinspect BQ15: bq15_corpse_cleanup.md a, s79_static.md 1; ARCHITECTURE.md "Corpses"; Simulation::stepCorpses in
// corpses.cpp). Constants of the exe's dead-character branch (Character::vf29, 1405ccd90) and b0's mapping onto our all-loaded world.

#include <cstdint>

namespace ksim {

// A corpse is destroyed when now - stamp > this many game hours (1416b3bfc, strict), reason "corpse decayed".
inline constexpr double kCorpseDecayHours = 12.0;
// A carried corpse's stamp is clamped to >= now - this (141693f2c): its age freezes at 6 h and it never decays while carried.
inline constexpr double kCorpseCarriedAgeCapHours = 6.0;
// Ticks between corpse evaluations (about 1.1 game minutes). The exe evaluates one character per frame, round-robin; the decay is
// measured in game hours, so any fine cadence gives the same outcome.
inline constexpr std::uint64_t kCorpseIntervalTicks = 40;
// The off-screen medical step's clock (Simulation::stepStoredMedical, stored_medical.cpp): one game minute (kTicksPerGameMinute, 36.4), each step
// running that many in-detail ticks of medical time on the stored members.
inline constexpr std::uint64_t kStoredMedicalTicks = 36;
// The straggler step's clock (Simulation::stepStragglers, residents.cpp): a straggler token walks to its parent this often.
inline constexpr std::uint64_t kStragglerTicks = 36;
// The original's zone load range (s71_static.md 5.1, ZoneMgr 140a0f590 / 140a0f640): the 3x3 zones around the camera zone, plus
// 340 units around a player-faction platoon (14174a100). The corpse is "unloaded" when no player is within it (or in a detail area).
inline constexpr int kZoneLoadRadiusCells = 1;
inline constexpr double kPlayerLoadRadius = 340.0;

} // namespace ksim
