// The package's join goals (kinspect BQ13 / BQ14; docs/reference/BAR_FIGHT_LOOP.md "bar_crowd: join rules"): there is no sight rule for
// neutrals. A character picks a fight only through the attack goals its own AI package role list carries:
//   16  ATTACK_ENEMIES                  a hostile sensed fighter (resolver 14059f3a0)
//   21  ATTACK_ENEMIES_AND_NEUTRALS     the same with the wide mask and its admission (resolver 14059f720)
//   15  PROTECT_ALLIES                  a fighting ally's attacker (resolver 14099ca10)
//   34  ATTACK_TROUBLE_MAKERS           a criminal in my town (resolver 14099baa0)
//   143 PROTECT_OWN_SQUAD               a fighting squadmate's attacker (resolver 14099d510)
//   144 TERRITORIAL_AGGRESSION_BUT_DONT_LEAVE_HOME  a hostile inside my home ring (resolver 14059fd70)
//   101 / 248 DEFEAT_SQUAD (+ LIMIT_CHASE_RANGE)  a sensed, standing member of my current target's squad (resolver 140793eb0, dt24_101)
//   41  ATTACK_ENEMIES_OF_MY_SLAVEMASTER            a slave (state 1) takes its slavemaster's enemies (resolver 14059ee50, task raid-goals)
// and, whatever the package says, whoever targets the character (32 SELF_PRESERVATION). This header is that bit mapping only: the
// resolvers, their GOAP rows and the live plans are tasks CF's / raid-goals' (world_resolvers.hpp, goap.cpp, combat.cpp).
#pragma once

#include <cstdint>

namespace ksim::joins {

inline constexpr std::uint8_t kAttackEnemies = 1; // goal 16
inline constexpr std::uint8_t kProtectAllies = 2; // goal 15
inline constexpr std::uint8_t kTroubleMakers = 4; // goal 34
inline constexpr std::uint8_t kProtectSquad = 8; // goal 143
inline constexpr std::uint8_t kAttackNeutrals = 16; // goal 21
inline constexpr std::uint8_t kTerritorial = 32; // goal 144
inline constexpr std::uint8_t kMasterEnemies = 64; // goal 41
inline constexpr std::uint16_t kDefeatSquad = 128; // goal 101 DEFEAT_SQUAD (dt24_101)
inline constexpr std::uint16_t kDefeatSquadChase = 256; // goal 248 DEFEAT_SQUAD_LIMIT_CHASE_RANGE (dt24_101)

// The bit a package task number sets (0 for any other task).
std::uint16_t bitOfTask(std::uint16_t task) noexcept;

} // namespace ksim::joins
