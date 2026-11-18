#pragma once

// What counts as alive for a squad (kinspect s71_static.md 5.2, the exe's "intact" test 1407ac7b0 -> 1407a5be0 -> 14079acf0; the
// details from s79_static.md section 2 / bq15_corpse_cleanup.md b, c). A pure read of the squad's members; nothing is saved.
// ARCHITECTURE.md "Squad liveness".

#include "ksim/fighter_state.hpp"

#include <flecs.h>

#include <cstddef>
#include <vector>

namespace kdata
{
class GameData;
struct Record;
}

namespace ksim {

// Fractions of the nominal squad size that must still be effective (floats in the exe, 1416885d8 and 141696c90).
inline constexpr float kIntactFraction = 0.2f;
inline constexpr float kIntactFractionLeaderDown = 0.4f;
// Squads below this nominal size use the leader test instead of a fraction.
inline constexpr std::size_t kIntactSmallSquad = 4;
// A downed member counts as effective while its injury severity is strictly below this (14079acf0: 0.2, 0.8 tied with the host).
inline constexpr double kEffectiveSeverity = 0.2;

// MedicalSystem+0xac (140644e60, s79 2.2): 0 unhurt, about 1 with a vital part at 0 HP.
//   max(0.5 (1 - min part health), 1 - min vital part health), over the natural (non-robotic) parts, health as a fraction of max
//   (above 0.999 it snaps to 1); a bleeding term (1 - blood / maxBlood) max(0, bleeding - 1.5 leak) 10 replaces it when larger and > 0.2.
// A fighter with no natural part (a robot or skeleton race) reports 0. UNSURE: `leak` (ms+0x74, the severed-limb leak) is taken as
// the raw sum of the severed limbs' leak without the data's extra-blood-loss factor.
double injurySeverity(const state::Fighter& fighter) noexcept;

// The exe's isDown (1405c9e10): dead, carried, a ragdoll, or KO'd (posture >= 3, M: the KO postures).
bool isDown(const state::Fighter& fighter) noexcept;

// platoon+0xa4 (1408fe0f0): 1 for a non-empty `leader` list, plus (min + max) / 2 per entry of `squad`, `squad2`, `animals` and
// `animals2` (integer division; min when max is 0 or 100), scaled by the squad size multiplier only when that is < 1, floor 1.
std::size_t nominalSquadSize(const kdata::Record& squadTemplate, double sizeMultiplier = 1.0);

struct SquadVitals
{
	std::size_t members = 0; // live member entities: dead ones stay in the array until their corpse is destroyed
	std::size_t nominal = 0; // the size the thresholds use: the template's nominal size, else `members`
	std::size_t dead = 0;
	std::size_t effective = 0; // not dead, and not down or with severity < 0.2
	bool hasLeader = false;
	bool leaderDown = false; // the leader is not effective
	bool intact = true;
};

// `data` supplies the squad's template for the nominal size; without it (or for a squad with no SquadInstance) the live count is used.
// UNSURE (ARCHITECTURE.md): a squad with no marked leader uses `effective > 0`; the exe's tied-with-host 0.8 threshold is not modelled.
// A squad with no members reports intact = true, as the exe's test does for a platoon without instantiated members; what ends such
// a squad is the faction tick's "no members left" rule (retireWipedSquads), not this test.
SquadVitals squadVitals(const flecs::world& ecs, flecs::entity squad, const kdata::GameData* data = nullptr);
// The same for a world squad (not a player squad) whose MemberOf members the caller already collected: a caller covering every
// squad groups the members in one pass instead of one query per squad (the debug view's DEBUG_SQUADS).
SquadVitals squadVitals(flecs::entity squad, const std::vector<flecs::entity>& members, const kdata::GameData* data = nullptr);

} // namespace ksim
