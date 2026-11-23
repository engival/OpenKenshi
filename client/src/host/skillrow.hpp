#pragma once

// The skill id -> name table (stats window + inspect focus) and the inspect focus's skill-in-use row. Header-only so tests/skillrow can check it.

#include <cmath>
#include <cstdint>
#include <string>

#include "net/simlink.hpp"
#include "ui/hud.hpp"

namespace host
{

// skill id -> name and stats-panel group, the one table for the stats panel and the inspect focus's skill-in-use row
inline const struct SkillName { uint8_t skill; const char* name, *group; int column; } kSkills[] = {
	{ 7, "Katanas", "Weapons", 0 }, { 8, "Sabres", "Weapons", 0 }, { 9, "Hackers", "Weapons", 0 },
	{ 11, "Heavy weapons", "Weapons", 0 }, { 10, "Blunt", "Weapons", 0 }, { 13, "Polearms", "Weapons", 0 },
	{ simlink::CharStats::kAttack, "Melee attack", "Combat", 1 }, { simlink::CharStats::kDefence, "Melee defence", "Combat", 1 },
	{ simlink::CharStats::kDodge, "Dodge", "Combat", 1 }, { simlink::CharStats::kMartialArts, "Martial arts", "Combat", 1 },
	{ 21, "Turrets", "Ranged", 1 }, { 20, "Crossbows", "Ranged", 1 }, { 32, "Precision shooting", "Ranged", 1 },
	{ simlink::CharStats::kStealth, "Stealth", "Thievery", 2 }, { 15, "Lockpicking", "Thievery", 2 },	// 15: lockpicking
	{ 22, "Thievery", "Thievery", 2 }, { 23, "Assassination", "Thievery", 2 },
	{ simlink::CharStats::kAthletics, "Athletics", "Athletics", 2 }, { 25, "Swimming", "Athletics", 2 },
	{ 14, "Field medic", "Sciences", 3 }, { 19, "Engineer", "Sciences", 3 }, { 26, "Robotics", "Sciences", 3 }, { 17, "Science", "Sciences", 3 },
	{ 27, "Weapon smith", "Trades", 3 }, { 28, "Armour smith", "Trades", 3 }, { 29, "Crossbow smith", "Trades", 3 },
	{ 18, "Labouring", "Trades", 3 }, { 30, "Farming", "Trades", 3 }, { 31, "Cooking", "Trades", 3 } };

inline const char* skill_name(uint8_t id)
{
	for (const auto& k : kSkills) { if (k.skill == id) { return k.name; } }
	return nullptr;
}

// The inspect focus's skill-in-use row (CHAR_STATS row 6, SIMLINK): kind 1 Stealth, 2 Athletics, 3 job skill, 4 ranged accuracy error,
// 5 combat-ready melee skill, 6 swimming speed; 0 none. An unknown kind or an unnamed skill prints raw ("skill N"), never dropped.
inline void skill_in_use_row(const simlink::CharStats& cs, ui::HudInfo& info)
{
	if (cs.extraKind == 0) { return; }
	const std::string v = std::to_string((int)std::lround(cs.extra));
	const char* nm = skill_name(cs.extraSkill);
	switch (cs.extraKind)
	{
		case 1: case 2: case 3: case 5:
			info.state.push_back({ nm ? std::string(nm) + ":" : "skill " + std::to_string(cs.extraSkill) + ":", v });
			return;
		case 4: info.state.push_back({ "Accuracy Error:", v + " degrees" }); return;
		case 6: info.state.push_back({ "Swimming:", v + " mph" }); return;
		default: info.state.push_back({ "skill " + std::to_string(cs.extraKind) + ":", v }); return;
	}
}


}	// namespace host
