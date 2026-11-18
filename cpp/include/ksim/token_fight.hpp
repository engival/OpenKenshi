#pragma once

// Off-screen fights between hostile tokens (kenshi-k16.6.46; COMBAT_OPTIONS.md 9, ARCHITECTURE.md "Corpses"; Simulation::stepTokenFights in
// token_fights.cpp), behind setOffscreenFights. OURS: the original resolves nothing off-screen (rwh.38). The pure part, so a test can pin the odds.

#include "ksim/clock.hpp"
#include "ksim/rng.hpp"
#include "ksim/state.hpp"

#include <cstddef>
#include <cstdint>

namespace ksim::tokenfight {

// Two hostile tokens whose positions are this close in one group-layer step meet. OURS / UNSURE (deferred: playtesting): a token walks 55 u/s.
inline constexpr double kEncounterRadius = 120.0;
// A pair fights at most once per this many ticks (one game hour).
inline constexpr std::uint64_t kPairTicks = kTicksPerGameHour;
// The wound a fallen member bleeds with when world.offscreen_medical is on, in units of the race's bleed rate (a severed limb's is 200). OURS: measured on
// a Greenlander with no aid, 500 clots and the member lives, 600 bleeds out in about 4.5 game hours, 800 in 2.7: 600 is the first that kills, so a
// squadmate's first aid decides.
inline constexpr double kFallWound = 600.0;

// One member's score: the mean of its attack, defence, toughness, strength and the skill of its wielded weapon (CombatRules::weaponSkill).
double memberScore(const state::Fighter& fighter);

// P(the first side wins) = SA^2 / (SA^2 + SB^2); 0.5 when both are 0.
double winChance(double strengthA, double strengthB);

struct Outcome
{
	bool aWins = false;
	double chanceA = 0.5; // P(A wins)
	bool winnerFalls = false; // the winner's weakest member falls (probability 1 - P(winner wins))
};

// The fight of squads `idA` < `idB` at `tick`: the first draw of its stream decides the winner, the second the winner's loss.
Outcome resolve(std::uint64_t seed, std::uint64_t idA, std::uint64_t idB, std::uint64_t tick, double strengthA, double strengthB);

// The loser's losses: ceil(living / 2).
inline std::size_t loserLosses(std::size_t living) { return (living + 1) / 2; }

} // namespace ksim::tokenfight
