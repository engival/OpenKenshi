#include "ksim/token_fight.hpp"

#include "ksim/combat.hpp"
#include "ksim/rng.hpp"

#include <algorithm>

namespace ksim::tokenfight {

double memberScore(const state::Fighter& fighter)
{
	const state::FighterSkills& s = fighter.skills;
	return (s.attack + s.defence + s.toughness + s.strength + CombatRules::weaponSkill(fighter)) / 5.0;
}

double winChance(double strengthA, double strengthB)
{
	const double a = strengthA * strengthA;
	const double b = strengthB * strengthB;
	return a + b > 0.0 ? a / (a + b) : 0.5;
}

Outcome resolve(std::uint64_t seed, std::uint64_t idA, std::uint64_t idB, std::uint64_t tick, double strengthA, double strengthB)
{
	RngStream stream(streamSeed(streamSeed(streamSeed(streamSeed(seed, kTokenFightStream), std::min(idA, idB)), std::max(idA, idB)), static_cast<std::size_t>(tick)));
	Outcome out;
	out.chanceA = winChance(strengthA, strengthB);
	out.aWins = stream.uniform01() < out.chanceA;
	const double chanceWinner = out.aWins ? out.chanceA : 1.0 - out.chanceA;
	out.winnerFalls = stream.uniform01() < 1.0 - chanceWinner;
	return out;
}

} // namespace ksim::tokenfight
