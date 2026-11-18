#pragma once

// What a crafting bench can make (CRAFTING_SLICE 3; kinspect craft27 2.2, 3.2), a pure function of the bench's BUILDING_FUNCTIONALITY record and
// the faction's research snapshot.

#include "ksim/research.hpp"

#include "kdata/gamedata.hpp"

#include <string>
#include <vector>

namespace ksim::recipes {

struct Craftable
{
	research::Kind kind = research::Kind::Item; // WeaponType for a weapon + grade pair
	std::string sid; // the item / armour / crossbow / backpack / limb, or the weapon
	std::string grade; // MATERIAL_SPECS_WEAPON of a weapon pair, else empty
};

// The bench's list, in the exe's order: weapon pairs, items, armour, crossbows, backpacks, robotics. Items, armour, crossbows, backpacks and
// robotics: each ref of the bench's `<kind> crafts` list that the snapshot has unlocked, in the bench's own order. Weapons: for each unlocked
// weapon type (in unlock order), the grades that are unlocked (`enable weapon model`) and in the bench's `weapon crafts`, but only the
// top-level grade (highest level first) and every grade whose `craft list fixed` is set. An empty list is a state, not an error.
std::vector<Craftable> craftable(const kdata::GameData& data, const kdata::Record& bench, const research::Snapshot& research);

} // namespace ksim::recipes
