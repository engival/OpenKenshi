#pragma once

// The stealth knockout (task ds/stealth-ko S4, docs/reference/combat/STEALTH_KO.md; dt41_C: Task_StealthKO 14034cd80, registry row 228).
// The pure parts: the constants and the chance of the roll. The task itself runs in the combat arena (CombatArena::orderStealthKo,
// Fighter::stealthKo); its crime, dialogue event and bookkeeping go through the Simulation (stepStealthKo).

#include "ksim/combat/combat.hpp"
#include "ksim/combat/fighter_state.hpp"
#include "ksim/ai/perception.hpp"

#include <algorithm>

namespace ksim::stealthko {

// EV 0x38 EV_ASSASSINATION_FAILED, fired at the victim when the roll fails (H).
inline constexpr int kEvAssassinationFailed = 0x38;
// The approach ends this close to the victim (DAT_1416bf47c, H).
inline constexpr double kApproachDistance = 14.0;
// The "knockout" clip: the roll is made at 0.5 of its progress (H), the task ends when it is over. UNSURE: the clip's length is not read
// (nor the exe's hold after it, _DAT_1416c0ee0): 1 s here, the length of a melee swing clip.
inline constexpr double kClipSeconds = 1.0;
inline constexpr double kResolveProgress = 0.5;
// The XP of the roll, before the scale below: success 100, failure 300 (H); 0.07 of it against a caged victim (H).
inline constexpr double kXpSuccess = 100.0;
inline constexpr double kXpFailure = 300.0;
inline constexpr double kXpCaged = 0.07;
// A raw XP number of the exe becomes this many addXp units (the toughness XP of a sever uses the same, CombatArena::kEatXpScale; UNSURE).
inline constexpr double kXpScale = 0.015;
// The chance runs 0..100 over attack - defence from -kChanceBound to +kChanceBound. UNSURE: the exe's two bounds are 15 and 30 and the
// way they combine in 1408852f0 is not read; this is the narrow one, an even match 50 %.
inline constexpr double kChanceBound = 15.0;

// The abort test: the victim's sensor already knows the assassin (140855830). The approach aborts on it, and SIMLINK SEEN_BY lists
// the observers it holds for, so the client's red fist never promises a KO the sim then aborts.
inline bool victimKnows(const state::Fighter& victim, std::uint64_t assassin) noexcept
{
	return perception::knownNow(victim, assassin);
}

// The attack value: Assassination x its effective stat (the exe's CharStats+0xb8 x getEffectiveStat 0x16).
inline double attackValue(const state::Fighter& assassin) noexcept
{
	return assassin.skills.assassination * CombatRules::effectiveStat(assassin, CombatRules::kStatAssassination, true);
}

// The victim's defence: the best of its Assassination, Thievery / 2, stealth fraction x 100 / 2, Defence / 2 and Toughness (H: the pieces;
// the exe takes their maximum).
inline double defenceValue(const state::Fighter& victim) noexcept
{
	const double stealth = perception::stealthFraction(victim.skills.stealth, victim.crouched) * 100.0;
	return std::max({attackValue(victim),
		victim.skills.thievery * CombatRules::effectiveStat(victim, CombatRules::kStatThievery, true) * 0.5,
		stealth * 0.5,
		victim.skills.defence * CombatRules::effectiveStat(victim, CombatRules::kStatDefence, true) * 0.5,
		victim.skills.toughness});
}

// 1408852f0(assassin, victim, 1): 0..100, 0 when the victim already knows the assassin. (The exe halves it for a victim with a sighted
// record: `aware` but not knownNow here.)
inline double chance(const state::Fighter& assassin, const state::Fighter& victim, std::uint64_t assassinId) noexcept
{
	if (victimKnows(victim, assassinId))
		return 0.0;
	const double heard = perception::aware(victim, assassinId) ? 0.5 : 1.0; // heard only (DAT_14172381c): half
	return heard * 100.0 * std::clamp((attackValue(assassin) - defenceValue(victim) + kChanceBound) / (2.0 * kChanceBound), 0.0, 1.0);
}

} // namespace ksim::stealthko
