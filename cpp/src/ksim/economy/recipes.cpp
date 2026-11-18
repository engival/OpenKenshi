// What a crafting bench can make (CRAFTING_SLICE 3; kinspect craft27 2.2, 3.2).

#include "ksim/economy/recipes.hpp"

#include "kworld/gamedata.hpp"

#include <algorithm>

namespace ksim::recipes {

namespace {

using research::Kind;

std::vector<const kdata::RefEntry*> refsOf(const kdata::Record& record, std::string_view category)
{
	std::vector<const kdata::RefEntry*> out;
	if (const kdata::RefCategory* refs = record.findRefs(category))
		for (const kdata::RefEntry& entry : refs->entries)
			if (!entry.opaqueEntry && !entry.targetID.empty())
				out.push_back(&entry);
	return out;
}

// A grade's level: v0 of its entry in a WEAPON_MANUFACTURER's `weapon models` (the first manufacturer that lists it), 0 when none does.
int gradeLevel(const kdata::GameData& data, const std::string& grade)
{
	for (const kdata::Record* maker : data.byType(kworld::itemTypeByName("WEAPON_MANUFACTURER")))
	{
		if (maker->boolOr("REMOVED", false))
			continue;
		for (const kdata::RefEntry* entry : refsOf(*maker, "weapon models"))
			if (entry->targetID == grade)
				return entry->v0;
	}
	return 0;
}

} // namespace

std::vector<Craftable> craftable(const kdata::GameData& data, const kdata::Record& bench, const research::Snapshot& research)
{
	std::vector<Craftable> out;
	// Weapons: per unlocked type, the bench's unlocked grades, the top-level one and every `craft list fixed` one.
	struct Grade
	{
		std::string sid;
		int level = 0;
		bool fixed = false;
	};
	std::vector<Grade> grades;
	for (const kdata::RefEntry* entry : refsOf(bench, "weapon crafts"))
	{
		if (!research.isUnlocked(Kind::WeaponModel, entry->targetID))
			continue;
		const kdata::Record* record = data.find(entry->targetID);
		grades.push_back({entry->targetID, gradeLevel(data, entry->targetID), record != nullptr && record->boolOr("craft list fixed", false)});
	}
	std::stable_sort(grades.begin(), grades.end(), [](const Grade& a, const Grade& b) { return a.level > b.level; });
	for (const std::string& weapon : research.unlocked[static_cast<std::size_t>(Kind::WeaponType)])
		for (std::size_t i = 0; i < grades.size(); ++i)
			if (i == 0 || grades[i].fixed)
				out.push_back({Kind::WeaponType, weapon, grades[i].sid});
	// The other kinds: the bench's own list order, filtered by the unlocked set.
	constexpr std::pair<Kind, std::string_view> kLists[] = {
		{Kind::Item, "item crafts"}, {Kind::Armour, "armour crafts"}, {Kind::Crossbow, "crossbow crafts"}, {Kind::Backpack, "backpack crafts"}, {Kind::Robotics, "robotics crafts"}};
	for (const auto& [kind, field] : kLists)
		for (const kdata::RefEntry* entry : refsOf(bench, field))
			if (research.isUnlocked(kind, entry->targetID))
				out.push_back({kind, entry->targetID, {}});
	return out;
}

} // namespace ksim::recipes
