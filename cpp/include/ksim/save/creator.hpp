#pragma once

// Character creation for a new game (docs/MULTIPLAYER.md 4a, docs/formats/SIMLINK.md NEW_GAME): what the client's creator can
// produce from the merged data (races, sexes, heads, hairs, beards, hair colours, editor sliders, idle stances), and the check of a
// NEW_GAME frame's Spec list against it. Rules: ~/git/kenshi/kenshi-inspect/findings/charedit.md, charedit8b_static.md, charsel23_static.md.

#include "kdata/gamedata.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ksim {

// One Spec of frame 111, with the strids resolved to record sids ("" = strid 0 = none). A strid the string table does not know as a
// sid is kBadSid, which no record has.
struct CharacterSpec
{
	std::string race; // RACE sid
	std::uint8_t female = 0;
	std::string head; // HEAD sid
	std::string hair; // ATTACHMENT sid, "" = none
	std::string beard; // ATTACHMENT sid, "" = none
	std::string hairColour; // COLOR_DATA sid
	float skin = 0.0f; // the "Skin Tone" colour range, 0..1 (the creator shows 0..100)
	std::vector<std::pair<std::string, float>> sliders; // editor Range names and raw values
	std::string name; // the character's name
	std::int32_t stance = 0; // the "Idle stance" slider, 1..stanceCount
};
inline constexpr std::string_view kBadSid = "\x01unknown-strid";

// One editor slider (data/editor/editor_data_*.xml <Range>) of a race and sex.
struct EditorSlider
{
	std::string name;
	std::string category; // "Face", "Body" (the XML's <Category>)
	float min = 0.0f;
	float max = 0.0f;
	float mid = 0.0f; // the default: round(min + (max - min) / 2) unless the XML gives one
};

// What the creator offers for one race (index 0 = male, 1 = female).
struct CreatorRace
{
	std::string sid;
	std::string name;
	std::vector<std::string> heads[2]; // HEAD sids, in the RACE's `heads male` / `heads female` order (CHAR_APPEARANCE headIndex)
	std::vector<std::string> hairs; // ATTACHMENT sids of `hairs` on attach slot 2 with a mesh or head texture (the picker's none is not listed)
	std::vector<std::string> beards; // the same on attach slot 13
	std::vector<std::string> hairColours; // COLOR_DATA sids of `hair colors`
	std::vector<EditorSlider> sliders[2]; // empty = the race has no such sex in the creator
	bool sex(bool female) const { return !sliders[female ? 1 : 0].empty(); }
};

// What a start-off's squad slot is (STARTS 33 Member): a Spec for a non-editable slot must echo it (spec_mismatch otherwise).
struct SquadSlot
{
	std::string race;
	std::uint8_t female = 0;
	bool editable = false;
};

class CreatorCatalog
{
public:
	static CreatorCatalog build(const kdata::GameData& gameData, std::string_view dataDir);
	// A catalog from given races (tests: the vanilla data has no one-sex creator race). `raceRecords` are the RACE sids that exist.
	static CreatorCatalog from(std::vector<CreatorRace> races, std::vector<std::string> raceRecords, int stanceCount);
	// The creator's races in its order: each RACE_GROUP's races that are `playable`, have a body mesh and an editor limits file.
	const std::vector<CreatorRace>& races() const noexcept { return races_; }
	const CreatorRace* find(std::string_view sid) const noexcept;
	// The Idle stance slider's length (the ANIMATION idle records the editor lists; charedit.md 1).
	int stanceCount() const noexcept { return stanceCount_; }
	// Every sid the creator can name (races, heads, hairs, beards, hair colours): the session puts them in the string table so a
	// client can send their strids.
	std::vector<std::string> sids() const;
	// The races a start-off allows: the creator's races, narrowed to its `force race` list; a list that leaves none allows every
	// creator race (the exe logs "Game start has no valid races" and rebuilds with all playable ones, charsel23 section 3).
	std::vector<std::string> allowedRaces(const std::vector<std::string>& forceRace) const;
	// "" when every Spec is something the creator can produce for this start-off, else the reason (SIMLINK.md NEW_GAME). The first
	// Spec with a fault decides; within one Spec the order is race, sex, head, hair, beard, hair colour, sliders, skin, stance, name.
	// `slots` (same size as specs) says which members are editable; a non-editable one takes a Spec that echoes its race and female with an
	// empty look ("spec_mismatch" otherwise; its name may change). An empty `slots` treats every member as editable.
	std::string check(const std::vector<std::string>& forceRace, const std::vector<CharacterSpec>& specs, const std::vector<SquadSlot>& slots = {}) const;

private:
	std::vector<CreatorRace> races_;
	std::vector<std::string> raceRecords_; // every RACE sid, to tell an unknown race from an unplayable one
	int stanceCount_ = 0;
};

// The reason a character name is refused, "" if it is fine: "empty_name" (nothing or only blanks) or "bad_character_name"
// (invalid UTF-8, a control character, more than 32 code points).
std::string checkCharacterName(const std::string& name);

// The Specs inside a logged Command's text, and back (length-prefixed, floats as their bits: the replay is exact).
std::string encodeSpecs(const std::vector<CharacterSpec>& specs);
bool decodeSpecs(std::string_view text, std::vector<CharacterSpec>& specs);

} // namespace ksim
