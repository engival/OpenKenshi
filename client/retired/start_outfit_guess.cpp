// Retired 2026-11-12 (k16.2.107): the creator's CLOTHES outfit GUESSED from the game-data records. It took the first piece per armour slot of a
// start squad member's CHARACTER `clothing` refs and its first `weapons` entry with quantity >= 1, but the sim draws the member's outfit from a
// weighted random pick at spawn, so the two could differ. The sim now sends the DRAWN outfit in STARTS (type 33): each member's `armour` and
// `weapon` record strids (ksim k16.5.4, master c808d14a), and the creator reads those (main.cpp, the NewGamePanel BEGIN handler). No fallback to
// the guess stays in main.cpp.
// Kept for reference only; not built.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"	// itemTypeByName

namespace retired {

// A start squad member's outfit from the game data (k16.17.6.1): the ARMOUR names the sim spawns it in, and the carried WEAPON
struct StartOutfit { std::vector<std::string> wear, weapons; };

// The start-off's squad members in the order STARTS lists them (the sim's startMembers with no rng: `leader`, then `squad` / `squad2` at v0, a bare
// CHARACTER at max(1, v0), then `choosefrom list` at `num random chars`, its first pick): the CHARACTER stringIDs.
void start_members(const kdata::GameData& gd, const kdata::Record& start, std::vector<std::string>& out)
{
	const int character = kworld::itemTypeByName("CHARACTER"), human = kworld::itemTypeByName("HUMAN_CHARACTER"), templateType = kworld::itemTypeByName("SQUAD_TEMPLATE");
	const auto add = [&](const std::string& sid)
	{
		const kdata::Record* r = gd.find(sid);
		if (r && (r->type == character || r->type == human)) { out.push_back(sid); }
	};
	const kdata::RefCategory* squads = start.findRefs("squad");
	if (!squads) { return; }
	for (const kdata::RefEntry& ref : squads->entries)
	{
		const kdata::Record* squad = gd.find(ref.targetID);
		if (!squad) { continue; }
		if (squad->type != templateType)
		{
			for (int i = 0; i < std::max(1, ref.v0); i++) { add(ref.targetID); }	// a bare CHARACTER: v0 of them, 0 counts as 1
			continue;
		}
		if (const kdata::RefCategory* leaders = squad->findRefs("leader")) { for (const kdata::RefEntry& e : leaders->entries) { for (int i = 0; i < e.v0; i++) { add(e.targetID); } } }
		for (const char* list : { "squad", "squad2" })
		{
			if (const kdata::RefCategory* c = squad->findRefs(list)) { for (const kdata::RefEntry& e : c->entries) { for (int i = 0; i < e.v0; i++) { add(e.targetID); } } }
		}
		if (const kdata::RefCategory* picks = squad->findRefs("choosefrom list"))
		{
			const int draws = squad->intOr("num random chars", 0);
			const kdata::Record* first = nullptr;
			for (const kdata::RefEntry& e : picks->entries) { const kdata::Record* t = e.v0 > 0 ? gd.find(e.targetID) : nullptr; if (t && (t->type == character || t->type == human)) { first = t; break; } }
			if (first) { for (int i = 0; i < draws; i++) { add(first->stringID); } }
		}
	}
}

StartOutfit start_outfit(const kdata::GameData& gd, const std::string& characterSid)
{
	StartOutfit out;
	const kdata::Record* character = gd.find(characterSid);
	if (!character) { return out; }
	const int armourType = kworld::itemTypeByName("ARMOUR"), weaponType = kworld::itemTypeByName("WEAPON");
	std::map<int, std::string> perSlot;	// slot -> the first worn piece; pieces come out by ascending slot, as the sim lists them
	if (const kdata::RefCategory* clothing = character->findRefs("clothing"))
	{
		for (const kdata::RefEntry& e : clothing->entries)
		{
			const kdata::Record* armour = e.opaqueEntry ? nullptr : gd.find(e.targetID);
			if (armour && armour->type == armourType && e.v0 >= 1) { perSlot.try_emplace(armour->intOr("slot", 0), armour->name); }
		}
	}
	for (const auto& [slot, name] : perSlot) { (void)slot; out.wear.push_back(name); }
	if (const kdata::RefCategory* weapons = character->findRefs("weapons"))
	{
		for (const kdata::RefEntry& e : weapons->entries)
		{
			const kdata::Record* weapon = e.opaqueEntry ? nullptr : gd.find(e.targetID);
			if (weapon && weapon->type == weaponType && e.v0 >= 1) { out.weapons.push_back(weapon->name); break; }
		}
	}
	return out;
}

} // namespace retired
