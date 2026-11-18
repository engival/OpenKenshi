#include "ksim/combat/combat.hpp"

#include "ksim/core/clock.hpp"

#include "ksim/core/rng.hpp"

#include <algorithm>
#include <cmath>

// The rules of the original's combat, one function each. Addresses are the Steam exe
// (docs/reference/combat/FUNCTIONS.md); the section numbers are RESOLUTION.md's.

namespace ksim {

namespace {

// FEATURES.md row 160: the rates a sleeper's medical and hunger clocks run at while it lies in a bed (`Fighter::asleep`).
// All five are community numbers, UNSURE: no measurement gives them. They live here, in one table, and nowhere else.
struct BedRates
{
	double healMult = 8.0; // healing (bandage pool -> health) and stun recovery
	double campBedHealMult = 4.0; // a camp bed instead of a real one (UNSURE: which pieces count; not distinguished yet, so unused)
	double hungerMult = 0.33; // hunger use while asleep, in place of the data's `bed hunger rate`
	double bloodRecoveryMult = 2.0; // the blood recovered per game-second while asleep
	double degenerationMult = 0.0; // untreated damage does not sink while asleep
};
constexpr BedRates kBedRates;

double lerp(double t, double a, double b) noexcept
{
	return a + (b - a) * t;
}

// The exe's `rand()/32768` is replaced by our stream; the draw count per decision is kept.
double draw(std::uint64_t& rng)
{
	RngStream stream(rng);
	const double value = stream.uniform01();
	rng = stream.state();
	return value;
}

std::int32_t effectiveWeaponType(const state::Fighter& f) noexcept
{
	// A defender without a working weapon arm is treated as unarmed (`pickTechnique`).
	return f.rightArmOk ? f.weapon.weaponType : 5;
}

bool inRange(std::int32_t type) noexcept
{
	return type >= 0 && type < 9;
}

const state::FighterPart* findPart(const state::Fighter& f, std::string_view name) noexcept
{
	for (const state::FighterPart& part : f.parts)
	{
		if (part.name == name)
			return &part;
	}
	return nullptr;
}

// `140886030`: the factor a technique's limbs contribute (0.001 missing, x0.25 wounded).
double limbFactor(const state::Fighter& f, std::int32_t limb) noexcept
{
	if (limb == 4) // the weapon arm
		return f.rightArmOk ? 1.0 : 0.001;
	const char* name = limb == 0 ? "Left Arm" : limb == 1 ? "Right Arm" : limb == 2 ? "Left Leg" : "Right Leg";
	const state::FighterPart* part = findPart(f, name);
	if (part == nullptr || part->collapsed)
		return 0.001;
	if (part->health < part->maxHealth * 0.5)
		return 0.25;
	return 1.0;
}

} // namespace

// ---------------------------------------------------------------------------
// CharStats
// ---------------------------------------------------------------------------

double CombatRules::weaponSkill(const state::Fighter& f) noexcept
{
	const state::FighterSkills& s = f.skills;
	switch (effectiveWeaponType(f))
	{
	case 0:
		return s.katana;
	case 1:
		return s.sabres;
	case 2:
		return s.blunt;
	case 3:
		return s.heavy;
	case 4:
		return s.hackers;
	case 8:
		return s.poles;
	default:
		return s.unarmed;
	}
}

namespace {

double armourSum(const state::Fighter& f, double state::FighterArmour::*member) noexcept
{
	double sum = 0.0;
	for (const state::FighterArmour& a : f.armour)
		sum += a.*member;
	return sum;
}

double armourProduct(const state::Fighter& f, double state::FighterArmour::*member) noexcept
{
	double product = 1.0;
	for (const state::FighterArmour& a : f.armour)
		product *= a.*member;
	return product;
}

// The equipment bonus with the weapon's `indoors mod` folded in (indoors_state.md section 2): applied only while the character is
// under a ceiling and the building is not `cavernous` (skipped on a roof, outdoors and in a cave). The attack side zeroes a
// positive bonus first when the mod is negative (the exe's floor in 1408844c0), so the total can go negative; the defence side
// adds it plain. Armour contributes no mod.
double equipmentBonus(const state::Fighter& f, double equipment, bool attack) noexcept
{
	if (!f.indoors || f.cavernous || f.weapon.indoorsMod == 0.0)
		return equipment;
	if (attack && equipment > 0.0 && f.weapon.indoorsMod < 0.0)
		return f.weapon.indoorsMod;
	return equipment + f.weapon.indoorsMod;
}

// ---- getEffectiveStat inputs (EFFECTIVE_STAT_LIVE.md) ----

// part+0x60 = (hp - stun + part+0x4c) / maxhp, not clamped.
double limbRatio(const state::FighterPart& p) noexcept
{
	return p.maxHealth > 0.0 ? CombatRules::partEffective(p) / p.maxHealth : 1.0;
}

// med+0xb8 (the better arm; -1 with no arms), med+0xc0 (the worst torso part), med+0xbc (head).
double bestArm(const state::Fighter& f) noexcept
{
	double best = -1.0;
	for (const state::FighterPart& p : f.parts)
	{
		if (p.type == 2)
			best = std::max(best, limbRatio(p));
	}
	return best;
}

double worstTorso(const state::Fighter& f) noexcept
{
	double worst = 1.0;
	for (const state::FighterPart& p : f.parts)
	{
		if (p.type == 0)
			worst = std::min(worst, limbRatio(p));
	}
	return worst;
}

double headRatio(const state::Fighter& f) noexcept
{
	for (const state::FighterPart& p : f.parts)
	{
		if (p.type == 3)
			return limbRatio(p);
	}
	return 1.0;
}

// FUN_140884e00 for unarmed, 140884ee0 otherwise: the value that drives the clip speeds and the
// attack XP. The skill levels are scaled by getEffectiveStat (martial arts / attack).
double skillValue(const state::Fighter& f, bool hunger) noexcept
{
	if (effectiveWeaponType(f) == 5)
	{
		return lerp(std::clamp(f.encumbrance, 0.0, 1.0), 0.2, 1.0)
			* (f.unarmedInt + f.skills.unarmed * CombatRules::effectiveStat(f, CombatRules::kStatMartialArts, hunger) + armourSum(f, &state::FighterArmour::unarmedBonus));
	}
	return f.attackInt + f.weapon.attackMod + f.skills.attack * CombatRules::effectiveStat(f, CombatRules::kStatAttack, hunger)
		+ armourSum(f, &state::FighterArmour::attackBonus);
}

} // namespace

double CombatRules::effectiveStat(const state::Fighter& f, int stat, bool hunger) noexcept
{
	double hungerFloor = 1.0; // f4
	double limbFloor = 1.0; // f8
	double limb = 1.0; // L
	double floor = 1.0; // f6
	bool collapsedArm = true; // the 0.66 rule applies (not to stealth 0x10 and athletics 0x11 per P(id))
	switch (stat)
	{
	case kStatStrength:
		hungerFloor = 0.0; limbFloor = 0.75; floor = 0.1;
		limb = bestArm(f) * worstTorso(f);
		break;
	case kStatAttack:
	case kStatMartialArts:
		hungerFloor = 0.75; limbFloor = 0.0; floor = 0.7;
		limb = bestArm(f) * worstTorso(f);
		break;
	case kStatDexterity:
		hungerFloor = 0.7; limbFloor = 0.0; floor = 0.4;
		limb = bestArm(f) * worstTorso(f);
		break;
	case kStatDefence:
		hungerFloor = 0.75; limbFloor = 0.0; floor = 0.8;
		limb = bestArm(f) * headRatio(f);
		break;
	case kStatAthletics:
		hungerFloor = 0.7; limbFloor = 1.0; floor = 0.5;
		collapsedArm = false;
		break;
	case kStatThievery: // EFFECTIVE_STAT_LIVE.md: hunger 0.75, the better arm (floor 0.4), floor 0.25
		hungerFloor = 0.75; limbFloor = 0.4; floor = 0.25;
		limb = bestArm(f);
		break;
	case kStatAssassination: // hunger 1 (none), the limb term 1 and x0.01 with both arms down, floor 0.5
		hungerFloor = 1.0; limbFloor = 0.0; floor = 0.5;
		limb = !f.rightArmOk && !f.leftArmOk ? 0.01 : 1.0;
		break;
	default:
		return 1.0;
	}
	const double h = hunger ? std::clamp(f.hunger - 1.0, 0.0, 1.0) : 1.0;
	double x = hungerFloor + (1.0 - hungerFloor) * h;
	x *= limbFloor + (1.0 - limbFloor) * limb;
	double r = std::clamp(floor + (1.0 - floor) * x, 0.0, 1.0);
	if (collapsedArm && (!f.rightArmOk || !f.leftArmOk))
		r *= 0.66; // one dead arm or two: 0.66, not 0.66^2
	return r;
}

// The attack and defence values that feed block and dodge chances, XP and the logged skill_a leave the
// hunger term out (the servant at hunger 1.698 has attack value 17.125 - 1 = 16.125 and a 77.27 % block
// chance, i.e. defence 16.1166 - 1, unscaled; the same servant's damage and clip speed do show the hunger
// penalty). UNSURE whether the limb term applies: all measured cases have healthy arms and head.
double CombatRules::attackValue(const state::Fighter& f) noexcept
{
	const double value = skillValue(f, false);
	if (effectiveWeaponType(f) == 5)
		return value; // bare hands carry no indoors mod
	const double equipment = f.weapon.attackMod + armourSum(f, &state::FighterArmour::attackBonus);
	return value - equipment + equipmentBonus(f, equipment, true);
}

double CombatRules::defenceValue(const state::Fighter& f) noexcept
{
	const double equipment = f.weapon.defenceMod + armourSum(f, &state::FighterArmour::defenceBonus);
	double value = f.defenceInt + f.skills.defence * effectiveStat(f, kStatDefence, false)
		+ equipmentBonus(f, equipment, false);
	if (f.blockMode && !f.down)
		value += 20.0;
	return value;
}

double CombatRules::dodgeSkillEff(const state::Fighter& f) noexcept
{
	const double dodge = f.skills.dodge;
	// FUN_140884970: the worst arm's health ratio * 1.8, in [0, 1].
	double health = 1.0;
	for (const state::FighterPart& part : f.parts)
	{
		if (part.type == 2 && part.maxHealth > 0.0)
			health = std::min(health, std::clamp(partEffective(part) / part.maxHealth * 1.8, 0.0, 1.0));
	}
	double value = dodge - (1.0 - std::clamp(f.encumbrance, 0.4, 1.0)) * dodge
		- (1.0 - std::clamp(health, 0.5, 1.0)) * dodge
		+ (dodge * armourProduct(f, &state::FighterArmour::dodgeMult) - dodge);
	if (f.blockMode && effectiveWeaponType(f) == 5)
		value += 20.0;
	return value;
}

double CombatRules::attackClipSpeed(const state::Fighter& f) const noexcept
{
	const CombatConstants& k = data_->k;
	const double dexTerm = f.skills.dexterity * effectiveStat(f, kStatDexterity);
	const double strEff = f.skills.strength * effectiveStat(f, kStatStrength);
	// [0x184] and [0x180] (`1408838e0`, `1408839a0`).
	const double s184 = k.minStrengthXpMult
		+ std::clamp((f.weapon.weight - strEff) / k.weightStrengthDiff1x, 0.0, k.weightStrengthDiffMax);
	const double h = std::clamp(s184 / k.weightStrengthDiffMax, 0.0, 1.0);
	const double s180 = (0.5 + 0.5 * std::min(1.0, f.encumbrance + 0.5) * (1.0 - h))
		* armourProduct(f, &state::FighterArmour::combatSpeedMult);
	const double x = (dexTerm * 0.6 + skillValue(f, true) * 0.4) * 0.01;
	return std::min(lerp(x, 0.8, 1.2) * s180, 1.2);
}

double CombatRules::reactClipSpeed(const state::Fighter& f) const noexcept
{
	const CombatConstants& k = data_->k;
	const double strEff = f.skills.strength * effectiveStat(f, kStatStrength);
	const double s184 = k.minStrengthXpMult
		+ std::clamp((f.weapon.weight - strEff) / k.weightStrengthDiff1x, 0.0, k.weightStrengthDiffMax);
	const double h = std::clamp(s184 / k.weightStrengthDiffMax, 0.0, 1.0);
	const double s180 = (0.5 + 0.5 * std::min(1.0, f.encumbrance + 0.5) * (1.0 - h))
		* armourProduct(f, &state::FighterArmour::combatSpeedMult);
	// BLOCK_TIMING_LIVE.md (calcClipSpeed 140883a80): effective dexterity x 0.6 + the DEFENCE value x 0.4 (the
	// defensive stance's +20 is in defenceValue), not the attack value the first slice used. Hunger applies here
	// (unlike in the block chance). Checked on cs[4] of the katana duel's joins: Engival (Katana) 1.0505 vs 1.05.
	// An unarmed fighter's cs[4] equals its attack-side x (servant at join: 0.8231 vs 0.7898 with the same x = 0.0785),
	// so unarmed keeps the unarmed value (UNSURE for the stance's +20 there).
	// The indoor defence path (1408866b0) applies the weapon's indoors mod to the equipment bonus too (indoors_state.md section 3).
	const double defence = effectiveWeaponType(f) == 5 ? skillValue(f, true)
		: f.defenceInt + f.skills.defence * effectiveStat(f, kStatDefence, true)
			+ equipmentBonus(f, f.weapon.defenceMod + armourSum(f, &state::FighterArmour::defenceBonus), false)
			+ (f.blockMode && !f.down ? 20.0 : 0.0);
	const double x = (f.skills.dexterity * effectiveStat(f, kStatDexterity) * 0.6 + defence * 0.4) * 0.01;
	return std::min(lerp(x, 0.6, 4.0) * s180, 4.0);
}

double CombatRules::maxBlood(const state::Fighter& f) const noexcept
{
	return (f.maxBloodRace - f.minBlood) * f.skills.strength * 0.01 + f.minBlood;
}

double CombatRules::attackRecoveryTime(int kind) noexcept
{
	return kind == 0 ? 0.6 : kind == 1 ? 0.3 : 0.2;
}

// ---------------------------------------------------------------------------
// Chances
// ---------------------------------------------------------------------------

namespace {

double chanceFrom(const CombatConstants& k, double own, double attacker, double low) noexcept
{
	const double d = own - attacker;
	double c = d <= 0.0 ? k.baseBlockChance + k.blockReductionPerLevel * d
						: k.baseBlockChance + k.blockIncreasePerLevel * d;
	if (c > 90.0)
		c = 90.0 + 0.2 * (c - 90.0); // soft cap (DAT_141689850, DAT_14168a4a8)
	return std::clamp(c, low, 95.0); // DAT_141685340, DAT_141685034
}

} // namespace

double CombatRules::blockChance(const state::Fighter& defender, double attackerSkill) const noexcept
{
	const double own = effectiveWeaponType(defender) == 5 ? attackValue(defender) : defenceValue(defender);
	return chanceFrom(data_->k, own, attackerSkill, 5.0);
}

double CombatRules::dodgeChance(const state::Fighter& defender, double attackerSkill) const noexcept
{
	return chanceFrom(data_->k, dodgeSkillEff(defender), attackerSkill, 0.0);
}

bool CombatRules::attackChanceRoll(double p1, double p2, std::uint64_t& rng) const
{
	const double factor = data_->k.attackChanceFactor;
	double x = 1.0;
	double y = 1.0;
	if (p1 <= p2)
		x = (p2 - p1) * factor + 1.0;
	else
		y = (p1 - p2) * factor + 1.0;
	const double r1 = draw(rng);
	const double r2 = draw(rng);
	return x * r1 < y * r2;
}

// ---------------------------------------------------------------------------
// Technique choice
// ---------------------------------------------------------------------------

AttackPass CombatRules::attackPass(const state::Fighter& f, double d, double half, std::int32_t previous,
	bool targetStumbling, int pass, double& reach) const
{
	AttackPass out;
	// Task BQ: an animal swings with its own technique family: the CHARACTER's `attack type` (Fighter::attackType, a
	// WeaponCategory >= 9) equals COMBAT_TECHNIQUE.animal. It has no weapon-type mask (that array is 9 human slots) and no
	// unarmed passes: it never punches. An animal from a v78 save (attackType 0) keeps the old, human-offering path.
	const bool animal = f.animal && f.attackType != 0;
	if (animal && pass > 0)
		return out;
	const std::int32_t wtype = pass == 0 ? effectiveWeaponType(f) : 5;
	const double skill = pass == 0 ? weaponSkill(f) : f.skills.unarmed;
	const bool prone = f.down;
	const bool previousIsBlock = previous >= 0
		&& data_->techniques[static_cast<std::size_t>(previous)].isBlock;
	for (std::size_t i = 0; i < data_->techniques.size(); ++i)
	{
		const Technique& t = data_->techniques[i];
		if (t.disabled || t.isBlock || t.isDodge)
			continue;
		if (animal ? t.animal != f.attackType : (t.animal != 0 && t.animal != 5))
			continue;
		if (!animal && (!inRange(wtype) || !t.weapon[static_cast<std::size_t>(wtype)]))
			continue;
		if (skill < t.minSkill || skill > t.maxSkill)
			continue;
		if ((1.0 - f.encumbrance) * 100.0 >= t.maxEncumbrance && !(t.isProne == prone))
			continue;
		if ((1.0 - f.encumbrance) * 100.0 > t.maxEncumbrance)
			continue;
		if (t.isProne != prone)
			continue;
		double w = 1.0;
		for (const ImpactPoint& impact : t.impacts)
			w *= limbFactor(f, impact.limb);
		if (w <= 0.01)
			w = 0.0;
		if (pass == 0 ? w < 1.0 : w <= 0.0)
			continue;
		reach = std::max(reach, t.attackDistance + half);
		if (d > half + t.attackDistance)
			continue;
		const double a = d + 3.0;
		const double b = (0.5 * half + t.attackDistance) + 3.0;
		double fit = std::min(a, b) / std::max(a, b);
		fit *= w;
		if (d < t.attackDistanceMinVsStatic)
			fit *= 0.02;
		if (static_cast<std::int32_t>(i) == previous && !previousIsBlock && !targetStumbling)
			fit *= 0.2;
		const double weight = fit * t.chance;
		if (weight > 0.0)
		{
			out.candidates.push_back({static_cast<std::int32_t>(i), weight});
			out.total += weight;
		}
	}
	return out;
}

double CombatRules::weaponChangedReach(const state::Fighter& f) const
{
	double reach = 0.0;
	std::uint64_t unused = 0;
	pickAttack(f, kWeaponChangeDistance, weaponLength(f), -1, false, reach, unused);
	return reach;
}

std::int32_t CombatRules::pickAttack(const state::Fighter& f, double d, double half, std::int32_t previous,
	bool targetStumbling, double& reach, std::uint64_t& rng) const
{
	reach = 0.0; // accumulates over the passes: stats+0x150 = max(stats+0x150, ...) (ENGAGEMENT.md 4.1)
	for (int pass = 0; pass < 3; ++pass)
	{
		const AttackPass result = attackPass(f, d, half, previous, targetStumbling, pass, reach);
		if (result.candidates.empty())
			continue;
		const double r = draw(rng) * result.total;
		double cumulative = 0.0;
		for (const AttackCandidate& candidate : result.candidates)
		{
			cumulative += candidate.weight;
			if (cumulative > r)
				return candidate.technique;
		}
		return result.candidates.back().technique;
	}
	return -1;
}

DefenceChoice CombatRules::pickDefence(const state::Fighter& defender, std::int32_t incomingDir,
	double attackerSkill, bool attackerIsFists, bool behind, bool stumbling, std::uint64_t& rng) const
{
	DefenceChoice choice;
	std::int32_t dirKey = incomingDir; // 1404356c0: the attack comes from behind
	if (behind)
		dirKey = incomingDir == 1 ? 7 : (incomingDir == 2 || incomingDir == 3) ? 9 : incomingDir;
	const std::int32_t wt = effectiveWeaponType(defender);
	double scale = 1.0;
	if (attackerIsFists)
		scale = std::clamp((defender.skills.unarmed - 0.75) / (1.1 - 0.75), 0.5, 1.1);
	choice.blockPercent = blockChance(defender, attackerSkill) * scale;
	choice.blockFail = choice.blockPercent < draw(rng) * 100.0;
	choice.dodgeMode = stumbling;
	if (wt == 5)
	{
		choice.dodgePercent = dodgeChance(defender, attackerSkill);
		if (draw(rng) * 100.0 < choice.dodgePercent)
			choice.dodgeMode = true;
	}
	auto rear = [](std::int32_t dir) { return dir >= 7 && dir <= 9; };
	const bool prone = defender.down;
	std::vector<AttackCandidate> candidates;
	double total = 0.0;
	for (std::size_t i = 0; i < data_->techniques.size(); ++i)
	{
		const Technique& t = data_->techniques[i];
		if (t.disabled || (t.animal != 0 && t.animal != 5) || t.impacts.empty())
			continue;
		if (!inRange(wt) || !t.weapon[static_cast<std::size_t>(wt)] || t.isProne != prone)
			continue;
		bool candidate = false;
		if (t.isDodge)
			candidate = choice.dodgeMode && t.isStumbleDodge == stumbling && t.chance > 0.0;
		else if (t.isBlock && !choice.dodgeMode)
		{
			const std::int32_t dir = t.impacts.back().dir;
			candidate = choice.blockFail ? (dir != dirKey && rear(dir) == rear(dirKey)) : dir == dirKey;
		}
		if (candidate)
		{
			candidates.push_back({static_cast<std::int32_t>(i), t.chance});
			total += t.chance;
		}
	}
	if (candidates.empty())
		return choice;
	const double r = draw(rng) * total;
	double cumulative = 0.0;
	choice.technique = candidates.back().technique;
	for (const AttackCandidate& c : candidates)
	{
		cumulative += c.weight;
		if (cumulative > r)
		{
			choice.technique = c.technique;
			break;
		}
	}
	return choice;
}

// ---------------------------------------------------------------------------
// Damage
// ---------------------------------------------------------------------------

void CombatRules::applyArmour(const state::Fighter& f, std::string_view part, double dmg[6],
	const std::function<bool(std::size_t, double)>& covers) const
{
	// R cut resistance, B blunt resistance, P pierce absorption, S the cut-into-stun weight. The pieces
	// are visited in list order; only S depends on it (RESOLUTION.md 4.2). e1: the High Paladin's
	// Holy Chest Plate (slot 5) before the Chain Shirt (slot 8) reproduces the logged stun to 1e-4,
	// the reverse does not; `makeFighter` therefore lists pieces by ascending slot (UNSURE beyond that pair).
	double R = 0.0;
	double B = 0.0;
	double P = 0.0;
	double S = 0.0;
	for (std::size_t i = 0; i < f.armour.size(); ++i)
	{
		const state::FighterArmour& a = f.armour[i];
		const state::FighterCoverage* cov = nullptr;
		for (const state::FighterCoverage& c : a.coverage)
		{
			if (c.part == part)
				cov = &c;
		}
		if (cov == nullptr || !covers(i, cov->coverage))
			continue;
		const double before = R;
		R += (1.0 - R) * a.cut;
		B += (1.0 - B) * a.blunt;
		P += a.pierce;
		S += a.cut * a.cutIntoStun * (1.0 - before);
	}
	// Weapon `armour penetration` (dmg[5], `+0x1c0`, -0.3..+0.3: negative = the armour resists more). The 0.9 cap
	// sits INSIDE the branch (RESOLUTION.md 4.2, L251): a penetrating weapon is capped after R *= (1-p), and with
	// no penetration there is no cap at all — stacked pieces may reach 0.936 and all 93.6 % is applied.
	if (dmg[5] != 0.0)
	{
		R -= dmg[5] * R;
		B -= dmg[5] * B;
		R = std::min(R, 0.9);
		B = std::min(B, 0.9);
	}
	const double stun0 = dmg[0] * S;
	const double pierceOld = dmg[2];
	dmg[0] -= dmg[0] * R;
	dmg[1] -= dmg[1] * B;
	dmg[2] = std::max(0.0, dmg[2] - P);
	dmg[3] = stun0 + (pierceOld - dmg[2]) * S;
}

double CombatRules::toughnessFactor(const state::Fighter& f) const noexcept
{
	const CombatConstants& k = data_->k;
	const double t = std::clamp(f.skills.toughness * 0.01, 0.0, 1.0);
	return 1.0 - lerp(t, k.resistanceMin, k.resistanceMax);
}

void CombatRules::computeDamage(const state::Fighter& attacker, const state::Fighter& target,
	double dmg[6]) const
{
	const CombatConstants& k = data_->k;
	const state::FighterWeapon& w = attacker.weapon;
	const double strEff = attacker.skills.strength * effectiveStat(attacker, kStatStrength);
	double cut = 0.0;
	double blunt = 0.0;
	double pierce = 0.0;
	if (effectiveWeaponType(attacker) == 5)
	{
		// 3.3: fists scale with toughness and strength.
		const double fval = skillValue(attacker, true);
		const double s = lerp(fval * 0.01, 0.25, 1.0);
		const double raw = std::max(1.0,
			s * (attacker.skills.toughness * k.unarmedDamageMult + strEff + 10.0));
		const double share = lerp(fval * 0.01, 1.0, 0.4);
		blunt = share * raw;
		cut = (1.0 - share) * raw;
	} else {
		// 3.2: weapon damage, 140883ba0 / 140883c60 / 140883d10.
		const double skill = weaponSkill(attacker);
		const double dexEff = attacker.skills.dexterity * effectiveStat(attacker, kStatDexterity);
		cut = lerp((skill + dexEff) * w.cutMult / 200.0, w.cut1, k.cutDamage99);
		blunt = lerp((0.5 * skill + 1.5 * strEff) * w.bluntMult / 200.0, w.blunt1, k.bluntDamage99);
		if (w.pierceMult > 0.0)
			pierce = k.pierceDamageMultiplier * (skill + dexEff) * w.pierceMult + 10.0 * w.pierceMult;
	}
	const RaceCombat* race = data_->race(target.raceSid);
	const double m = race != nullptr && race->isRobot ? w.robotMult : w.humanMult;
	dmg[0] = cut * m;
	dmg[1] = blunt * m;
	dmg[2] = pierce * m;
	dmg[3] = 0.0;
	const double output = armourProduct(attacker, &state::FighterArmour::damageOutputMult);
	if (output > 0.0 && output != 1.0)
	{
		for (int i = 0; i < 4; ++i)
			dmg[i] *= output;
	}
	dmg[4] = w.bleedMult;
	dmg[5] = w.armourPenetration;
}

// ---------------------------------------------------------------------------
// XP
// ---------------------------------------------------------------------------

double CombatRules::xpUnit() const noexcept
{
	return data_->k.expGainMultiplier * 0.25 * 0.1;
}

void CombatRules::addXp(double& skill, double amount) const noexcept
{
	if (settings_.noRunawaySkills && skill >= 101.0)
		return;
	const double q = std::pow((101.0 - skill) / 101.0, 2.0);
	if (amount > 0.0 && amount <= 20.0 && q <= 20.0)
		skill += q * amount;
}

// `CharStats::skillDiffXpMult` (1408c5fa0, MEASURED.md item 15): K = 10 when theirs >= mine, else
// 25 (the two global constants); NaN -> 0.1; clamp [0.1, 6]; then the crowd factor
// n = engaged count (cc+0x200) + overflow counter (cc+0x210 / 0x214 by tick parity) - 1,
// m *= n/10 + 1 when n > 0; clamp [0.01, 10].
// UNSURE: our `overflow` is one folded counter that is never reset; the original's counters
// cycle 1..5 / stick at 5 (FIGHT_G_LIVE.md), so it is capped at 5 here.
// UNSURE: for toughness `mine` should be CS+0x34 x toughness; the callers pass the skill alone.
double CombatRules::skillDiffXpMult(const state::Fighter& me, double mine, double theirs) const noexcept
{
	const CombatConstants& k = data_->k;
	const double divisor = theirs < mine ? k.skillDiffXp0xPenalty : k.skillDiffXp2xBonus;
	double m = (theirs - mine) / divisor + 1.0;
	m = std::isnan(m) ? 0.1 : std::clamp(m, 0.1, 6.0);
	const double n = static_cast<double>(me.engaged.size()) + std::min<std::uint32_t>(me.overflow, 5) - 1.0;
	if (n > 0.0)
		m *= n / 10.0 + 1.0;
	return std::clamp(m, 0.01, 10.0);
}

// ---------------------------------------------------------------------------
// MedicalSystem
// ---------------------------------------------------------------------------

// `startKnockoutTimer` (`1406447d0`), re-measured live (kinspect findings/ko_duration.md):
// T = base + sum_vital 100 g (E - h - s)/E + sum_{vital, h-s<0} 100 f KOmult |h-s|/E + 2 x 100 g (maxBlood - blood)/maxBlood,
// f = lerp(t, knockout mult 1, knockout mult 99), g = 0.25 f, E = max - wear. BOTH part sums run over the VITAL parts only:
// a limb's stun never lengthens the timer (the live KoMults are Head 2, Stomach/Chest 1, limbs 0.5, and the sum skips the
// limbs). T can be negative - a stun past the vital's health with little damage - and the KO then ends at the next medical
// update (live: head stun 108 on Bark, T -11.34, woke at once).
double CombatRules::knockoutTime(const state::Fighter& f) const noexcept
{
	const CombatConstants& k = data_->k;
	const double t = std::clamp(f.skills.toughness * 0.01, 0.0, 1.0);
	const double f6 = lerp(t, k.knockoutMult1, k.knockoutMult99);
	const double f9 = 0.25 * f6;
	double total = k.knockoutTimeBase;
	double vital = 0.0;
	double negative = 0.0;
	for (const state::FighterPart& p : f.parts)
	{
		if (!p.vital)
			continue;
		const double maxP = p.maxHealth - p.wear;
		if (maxP <= 0.0)
			continue;
		vital += (maxP - (p.stun + p.health)) * f9 / maxP;
		const double h = p.health - p.stun;
		if (h < 0.0)
			negative += std::fabs(h) * f6 * p.koMult / maxP;
	}
	const double maxB = maxBlood(f);
	total += vital * 100.0 + negative * 100.0 + (std::fabs(maxB - f.blood) * f9 / maxB) * 100.0 * 2.0;
	return total;
}

double CombatRules::koPoint(const state::Fighter& f) const noexcept
{
	const CombatConstants& k = data_->k;
	const double t = std::clamp(f.skills.toughness * 0.01, 0.0, 1.0);
	return -lerp(t, k.minToughnessKoPoint, k.maxToughnessKoPoint);
}

double CombatRules::koThreshold(const state::Fighter& f) const noexcept
{
	// Which gate the vital-part hit path uses (`KoGate`, RESOLUTION.md 5.2): the old gate at 0, or the KO point.
	// Limb collapse does not use this switch (checkCollapse reads koPoint directly).
	return settings_.koGate == CombatSettings::KoGate::Zero ? 0.0 : koPoint(f);
}

void CombatRules::checkCollapse(state::Fighter& f) const
{
	// The limb collapse law (kinspect 8de1eab findings/medical, VERIFIED-live): an intact limb is down when its
	// effective health (health - stun + splint + heal pool) is below the character's KO point; a severed limb is
	// always down. The DOWN flag is recomputed every tick, so the body stands again as soon as the part rises above
	// the point (live: a leg at -50 walks, at -52 the character goes down).
	//
	// A collapse also FIRES THE FORCED KO WRITER (task BL, kinspect down_ko_trigger.md / fb0248d, live: a leg 66.17 -> 6.42
	// with stun 92.97, h-s -86.5 against a -25.4 point; down and KO both set 11 frames later): the KO timer (med+0xa0) is
	// exactly 3.0 medical timer units, ~2.1 s live at ~1.4 units/s, so it scales with the medical clock and game speed. It
	// does not go through startKnockoutTimer, and when it ends the down flag stays: the body crawls (posture 2, task BF).
	// A sever calls the same writer (arg 0.7529, 3.0 floor).
	const double threshold = koPoint(f);
	bool down = false;
	bool right = true;
	bool left = true;
	for (state::FighterPart& p : f.parts)
	{
		const bool wasCollapsed = p.collapsed;
		p.collapsed = p.collapses && (p.severed() || partEffective(p) + p.pool < threshold);
		if (!p.collapsed)
			continue;
		if (!wasCollapsed)
			f.koTimer = std::max(f.koTimer, kCollapseKoTime); // a new collapse: the brief forced KO
		if (p.collapseBits & 2)
			right = false;
		if (p.collapseBits & 4)
			left = false;
		if (p.collapseBits & (1 | 16 | 32))
			down = true;
	}
	f.rightArmOk = right;
	f.leftArmOk = left;
	f.down = down;
}

bool CombatRules::trySever(state::Fighter& f, state::FighterPart& p, bool viaHit) const
{
	if (settings_.limbLossLevel < (viaHit ? 1 : 2))
		return false;
	if (p.health - p.bonus >= -(p.maxHealth - p.wear))
		return false;
	return severNow(f, p);
}

bool CombatRules::severNow(state::Fighter& f, state::FighterPart& p) const
{
	if (!p.limb() || p.severed() || p.robotic)
		return false;
	for (const state::FighterPart& other : f.parts)
	{
		if (other.name.find("Foreleg") != std::string::npos)
			return false; // animals are never severed (CharacterAnimal overrides the humanoid test)
	}
	// A leg sever of a standing victim starts the forced-KO timer (severPart 14064edc0 -> startForcedKnockoutTimer 140644980, dt40_leg_sever_ko
	// sections 1-2): floor3(((max(0, maxBlood - blood) x 0.5(3 - 2.25t) + 40) x (1 - t) x 4)) x U(0,1), t = toughness x 0.01 (the CharStats+0x34
	// body stat is undecoded and taken as 1, UNSURE). It is computed before the sever's own blood loss; an arm sever sets no timer. The
	// countdown and the save are the koTimer's.
	if (p.name.find("Leg") != std::string::npos && !f.down && !f.unconscious && f.koTimer <= 0.0) // a timer already running (a hit's KO, same tick) is the victim being out
	{
		const double t = std::clamp(f.skills.toughness * 0.01, 0.0, 1.0);
		const double raw = (std::max(0.0, maxBlood(f) - f.blood) * 0.5 * (3.0 - 2.25 * t) + 40.0) * std::clamp(1.0 - t, 0.0, 1.0) * 4.0;
		f.koTimer = std::max(raw, 3.0) * draw(f.rng);
	}
	p.maxHealth = state::FighterPart::kSeveredMax;
	f.blood -= 5.0 + 25.0 * draw(f.rng);
	f.wounds.push_back(data_->k.bleedRate * 200.0);
	return true;
}

bool CombatRules::updateMedical(state::Fighter& f, double gameDt) const
{
	if (f.dead)
		return false;
	const CombatConstants& k = data_->k;
	const double dt = gameDt * settings_.medicalClock; // medical time; hunger below runs on game time
	// A sleeper in a bed heals faster, recovers blood faster and does not degenerate (FEATURES 160, all UNSURE numbers).
	const double healMult = f.asleep ? kBedRates.healMult : 1.0;
	const double degenMult = f.asleep ? kBedRates.degenerationMult : 1.0;
	// updateHealing (`14064f5c0`): stun decay, heat decay.
	for (state::FighterPart& p : f.parts)
	{
		const double maxP = p.maxHealth - p.wear;
		if (p.stun > 0.0)
		{
			const double rate = 0.05 * k.stunRecoveryRate * k.healRateMult * (maxP / 100.0) * f.healFactor * healMult;
			p.stun = std::max(0.0, p.stun - rate * dt);
		}
		if (p.health >= 0.0)
			p.heat = std::max(1.0, p.heat - 0.01 * dt);
		else
			p.heat = 1.0;
	}
	// Bandage pool -> health (MedicalSystem healing step; MEDICAL_HUNGER_LIVE.md d): amt = dt x 0.05 x heal rate mult x H,
	// H = (max - wear) / 100, x250 for a robotic part. Health rises only this way (no natural regeneration), 2.04 HP per
	// game hour on a healthy part. Then degeneration: an untreated part over 20 % damaged loses health, KO'd or not.
	const double degen = lerp(std::clamp(f.skills.toughness * 0.01, 0.0, 1.0), k.degenerationMult1, k.degenerationMult99)
		* k.bodypartDegenerationRate;
	for (state::FighterPart& p : f.parts)
	{
		if (dt > 0.0)
			trySever(f, p, false); // Dismemberment 2: the passive path of updateHealing
		const double maxP = p.maxHealth - p.wear;
		if (maxP <= 0.0)
			continue;
		if (p.pool > 0.0 && dt > 0.0)
		{
			const double amt = std::min(p.pool, dt * 0.05 * k.healRateMult * (maxP / 100.0) * (p.robotic ? 250.0 : 1.0) * healMult);
			p.pool -= amt;
			p.health = std::min(p.health + amt, maxP);
		}
		if (dt > 0.0 && !(p.robotic && !p.vital))
		{
			const double dmgPct = ((maxP - p.health) - p.pool) * 100.0 / maxP;
			double x = std::clamp((dmgPct - 20.0) / 70.0, 0.0, 1.0);
			if (p.health + p.pool > 0.0)
				x *= 0.25;
			// The sink stops at H <= -3M (BQ11 s76: an intact limb -300, a severed one -15; a body already below does not rise to it).
			if (p.health > -3.0 * maxP)
				p.health -= dt * 0.05 * degen * settings_.chanceOfDeath * x * 1.2 * degenMult;
		}
	}
	// Hunger (a): hunger -= game hours / T x M. Below 0.1 a forced KO starts once (timer lerp(toughness, 60, 10) x a
	// random factor, UNSURE: 28.4 and 54.7 on one character, taken as 0.75..1.75); it holds until fed. Below 0: death.
	if (gameDt > 0.0)
		f.hunger -= gameDt / kGameHourSeconds / (k.starvationHours * settings_.hungerTime) * hungerUse(f);
	if (f.hunger < 0.1 && !f.hungerKo)
	{
		f.hungerKo = true;
		f.koTimer = lerp(std::clamp(f.skills.toughness * 0.01, 0.0, 1.0), 60.0, 10.0) * (0.25 + 1.75 * draw(f.rng)); // dt40_leg_sever_ko section 4: U(0.25, 2)
	} else if (f.hunger >= 0.1) {
		f.hungerKo = false;
	}
	// updateBleeding (`140646140`). The limb leak (getBleedLeak `140644b00`, BQ11 s76) is only a SEVERED limb's, from H + R
	// (R = the heal pool), no race / KO / clot test. An intact limb at any negative health never leaks; first aid (pool) cancels it.
	// Retired: the ungated leak of every non-vital part below 0 (RESOLUTION 5.3 as first decoded).
	double leak = 0.0;
	for (const state::FighterPart& p : f.parts)
	{
		if (p.severed() && p.health + p.pool < 0.0)
			leak += (p.health + p.pool) * -0.002;
	}
	double wounds = 0.0;
	for (double& rate : f.wounds)
	{
		rate -= k.bleedClotRate * dt;
		if (rate > 0.0)
			wounds += rate;
	}
	f.wounds.erase(std::remove_if(f.wounds.begin(), f.wounds.end(), [](double r) { return r <= 0.0; }),
		f.wounds.end());
	const double bleed = std::max(0.0, (k.extraBloodLoss * leak + wounds) * settings_.bleedScale * settings_.chanceOfDeath
		* 1.2 * f.bloodFactor);
	f.bleeding = bleed;
	const double maxB = maxBlood(f);
	f.blood -= bleed * dt;
	if (bleed <= 0.0 && f.blood < maxB)
		f.blood = std::min(maxB, f.blood + k.bloodRecoveryRate * dt * f.bloodFactor
			* (f.asleep ? kBedRates.bloodRecoveryMult : 1.0));
	f.blood = std::min(f.blood, maxB);
	// Collapse flags follow the parts (stun decays, so a limb can recover).
	checkCollapse(f);
	// KO state, recomputed every update (FUN_140649320, VERIFIED live, ko_death_model.md): the KO
	// holds while the timer runs, or a vital part is at collapse level 2 (below -koPoint, which sets
	// the latch; between -koPoint and 0 only while the latch is set), or blood <= 0.
	if (f.unconscious)
		f.koTimer = std::max(0.0, f.koTimer - dt);
	// A KO condition that persists past the timer refills it (FUN_140649320, dt40_leg_sever_ko section 4): the hunger KO's window renews
	// at every expiry, lerp(t, 60, 10) x U(0.25, 2). (The bleed-out coma reseeds with knockoutTime above, verified live.)
	if (f.hungerKo && f.hunger < 0.1 && f.koTimer <= 1.0)
		f.koTimer = lerp(std::clamp(f.skills.toughness * 0.01, 0.0, 1.0), 60.0, 10.0) * (0.25 + 1.75 * draw(f.rng));
	const double koPoint = -lerp(std::clamp(f.skills.toughness * 0.01, 0.0, 1.0),
		k.minToughnessKoPoint, k.maxToughnessKoPoint);
	bool vitalDown = false;
	for (const state::FighterPart& p : f.parts)
	{
		if (!p.vital)
			continue;
		const double v = partEffective(p);
		if (v < koPoint)
		{
			vitalDown = true;
			f.koLatch = true;
		} else if (v < 0.0 && f.koLatch) {
			vitalDown = true;
		}
	}
	if (!vitalDown)
		f.koLatch = false;
	if (f.blood <= 0.0 && f.koTimer <= 0.0 && !f.unconscious)
		f.koTimer = knockoutTime(f); // bleed-out coma
	f.unconscious = f.koTimer > 0.0 || vitalDown || f.blood <= 0.0 || f.hunger < 0.1;
	// Death: blood < -maxBlood, or a vital part pushed down to minus its own max.
	bool died = f.blood < -maxB || f.hunger < 0.0;
	for (const state::FighterPart& p : f.parts)
	{
		if (p.vital && p.health - p.stun < -(p.maxHealth - p.wear))
			died = true;
	}
	if (died)
	{
		f.dead = true;
		f.unconscious = true;
	}
	return died;
}

double CombatRules::hungerUse(const state::Fighter& f) const noexcept
{
	if (f.asleep)
		return f.hungerRate * kBedRates.hungerMult; // FEATURES 160, UNSURE: the community number, not the data's `bed hunger rate`
	if (f.dead || f.unconscious)
		return f.hungerRate * data_->k.bedHungerRate; // the bed rate: UNSURE for a body on the ground
	const bool moving = std::fabs(f.forwardSpeed) > 0.5 || std::fabs(f.lateralSpeed) > 0.5 || f.walking()
		|| f.driving();
	if (moving || f.target != 0)
		return f.hungerRate * (1.0 + (1.0 - f.encumbrance) * data_->k.encumbranceHungerRate);
	return f.hungerRate;
}

double CombatRules::bandageRate(double skill, double kitQuality, bool robotKit) const noexcept
{
	const double t = std::clamp(std::min(skill, kitQuality) / 100.0, 0.0, 1.0);
	return lerp(t, 1.0, 15.0) * (robotKit ? data_->k.robotMedicSpeed : data_->k.medicSpeed);
}

double CombatRules::kitDrain(double skill) const noexcept
{
	return lerp(std::clamp((100.0 - skill) / 100.0, 0.0, 1.0), data_->k.medkitDrain99, data_->k.medkitDrain1);
}

double CombatRules::bandage(state::Fighter& patient, double rate, bool robotKit, double dt, bool& more) const
{
	std::vector<state::FighterPart*> order;
	for (state::FighterPart& p : patient.parts)
	{
		if (p.robotic == robotKit)
			order.push_back(&p);
	}
	// head (3), torso (0), arms (2), legs (1); the vector order within a kind.
	const auto rank = [](const state::FighterPart* p) { return p->type == 3 ? 0 : p->type == 0 ? 1 : p->type == 2 ? 2 : 3; };
	std::stable_sort(order.begin(), order.end(), [&](const auto* a, const auto* b) { return rank(a) < rank(b); });
	double budget = rate * dt;
	double added = 0.0;
	more = false;
	for (state::FighterPart* p : order)
	{
		const double room = (p->maxHealth - p->wear) - p->health - p->pool;
		if (room <= 1.0e-9)
			continue;
		const double take = std::min(room, budget);
		p->pool += take;
		added += take;
		budget -= take;
		if (room - take > 1.0e-9)
			more = true;
	}
	return added;
}

void CombatRules::clotWounds(state::Fighter& patient, double skill, double kitQuality, bool robotKit, double dt) const
{
	const double t = std::clamp(std::min(skill, kitQuality) / 100.0, 0.0, 1.0);
	const double extra = lerp(t, 0.0005, 0.03) * 2.0 * (robotKit ? data_->k.robotMedicSpeed : data_->k.medicSpeed) * dt;
	// The newest wound only (the back of the list: combat.cpp appends one per hit), until it is gone; leftover dt does not carry
	// to the next wound (UNSURE), the next tick treats the next newest. FUN_14064d800, MEDICAL_HUNGER_LIVE (c).
	if (patient.wounds.empty())
		return;
	patient.wounds.back() -= extra;
	if (patient.wounds.back() <= 0.0)
		patient.wounds.pop_back();
}

double CombatRules::aidXpPerSecond(const state::Fighter& medic) const noexcept
{
	return 1.0 / lerp(std::clamp(medic.skills.medic * 0.01, 0.0, 1.0), data_->k.xpMedic1, data_->k.xpMedic99);
}

double CombatRules::bleedRate(const state::Fighter& f) const noexcept
{
	return f.bleeding;
}

} // namespace ksim
