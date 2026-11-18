// Character creation for a new game: the creator's choices from the merged data and the check of a NEW_GAME Spec list
// (include/ksim/creator.hpp, docs/formats/SIMLINK.md NEW_GAME). The creator itself is the client's (client/src/creator.cpp,
// characters.cpp); this reads the same data the same way so that the server accepts exactly what that creator can produce.

#include "ksim/creator.hpp"

#include "ksim/player_squads.hpp"

#include "kworld/gamedata.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <set>
#include <sstream>

namespace ksim {

namespace {

// The editor's sliders for one sex: every <Range> of the <Config gender="Male|Female">. `mid` defaults to round(min + (max - min) / 2)
// (140077090, charedit.md 2). Same reading as the client's readRanges.
std::vector<EditorSlider> readRanges(const std::string& path, bool female)
{
	std::vector<EditorSlider> out;
	std::ifstream in(path);
	if (!in)
		return out;
	std::stringstream ss;
	ss << in.rdbuf();
	const std::string xml = ss.str();
	std::size_t at = xml.find(female ? "gender=\"Female\"" : "gender=\"Male\"");
	const std::size_t end = at == std::string::npos ? at : xml.find("</Config>", at);
	while (at != std::string::npos)
	{
		at = xml.find("<Range ", at + 1);
		if (at == std::string::npos || at > end)
			break;
		const std::string tag = xml.substr(at, xml.find('>', at) - at);
		const auto attr = [&](const char* key) -> std::string
		{
			const std::string k = std::string(" ") + key + "=\"";
			const std::size_t v = tag.find(k);
			return v == std::string::npos ? std::string() : tag.substr(v + k.size(), tag.find('"', v + k.size()) - v - k.size());
		};
		EditorSlider slider;
		slider.name = attr("name");
		slider.min = static_cast<float>(std::atof(attr("min").c_str()));
		slider.max = static_cast<float>(std::atof(attr("max").c_str()));
		if (const std::size_t cat = xml.rfind("<Category name=\"", at); cat != std::string::npos && cat > xml.rfind("<Config ", at))
			slider.category = xml.substr(cat + 16, xml.find('"', cat + 16) - cat - 16);
		const std::string mid = attr("mid");
		slider.mid = mid.empty() ? std::round(slider.min + (slider.max - slider.min) * 0.5f) : static_cast<float>(std::atof(mid.c_str()));
		out.push_back(std::move(slider));
	}
	return out;
}

// `editor limits` is a game-relative Windows path (".\data\editor\editor_data_human.xml"): the file by name under <data>/editor.
std::string editorFile(std::string_view dataDir, std::string path)
{
	std::replace(path.begin(), path.end(), '\\', '/');
	const std::size_t slash = path.rfind('/');
	if (slash != std::string::npos)
		path.erase(0, slash + 1);
	return std::string(dataDir) + "/editor/" + path;
}

bool anyFile(const kdata::Record& record, std::initializer_list<const char*> keys)
{
	for (const char* key : keys)
	{
		if (!record.fileOr(key).empty())
			return true;
	}
	return false;
}

bool inList(const std::vector<std::string>& list, const std::string& sid)
{
	return std::find(list.begin(), list.end(), sid) != list.end();
}

// ---- encoding ----

void putString(std::string& out, std::string_view text)
{
	out += std::to_string(text.size());
	out += ':';
	out += text;
}

void putNumber(std::string& out, std::int64_t value)
{
	out += std::to_string(value);
	out += ';';
}

class Reader
{
public:
	explicit Reader(std::string_view text) : text_(text) {}
	bool atEnd() const noexcept { return at_ == text_.size(); }
	bool number(std::int64_t& value)
	{
		const std::size_t end = text_.find(';', at_);
		if (end == std::string_view::npos || end == at_)
			return false;
		char* tail = nullptr;
		const std::string digits(text_.substr(at_, end - at_));
		value = std::strtoll(digits.c_str(), &tail, 10);
		if (tail != digits.c_str() + digits.size())
			return false;
		at_ = end + 1;
		return true;
	}
	bool string(std::string& value)
	{
		const std::size_t colon = text_.find(':', at_);
		if (colon == std::string_view::npos || colon == at_)
			return false;
		char* tail = nullptr;
		const std::string digits(text_.substr(at_, colon - at_));
		const unsigned long long length = std::strtoull(digits.c_str(), &tail, 10);
		if (tail != digits.c_str() + digits.size() || colon + 1 + length > text_.size())
			return false;
		value.assign(text_.substr(colon + 1, static_cast<std::size_t>(length)));
		at_ = colon + 1 + static_cast<std::size_t>(length);
		return true;
	}

private:
	std::string_view text_;
	std::size_t at_ = 0;
};

} // namespace

CreatorCatalog CreatorCatalog::build(const kdata::GameData& gameData, std::string_view dataDir)
{
	CreatorCatalog catalog;
	const std::int32_t raceType = kworld::itemTypeByName("RACE");
	for (const kdata::Record* race : gameData.byType(raceType))
	{
		if (!race->boolOr("REMOVED", false))
			catalog.raceRecords_.push_back(race->stringID);
	}
	const auto refSids = [&](const kdata::Record& owner, const std::string& category)
	{
		std::vector<std::string> sids;
		if (const kdata::RefCategory* refs = owner.findRefs(category))
		{
			for (const kdata::RefEntry& entry : refs->entries)
			{
				if (!entry.opaqueEntry && gameData.find(entry.targetID) != nullptr)
					sids.push_back(entry.targetID);
			}
		}
		return sids;
	};
	const auto build = [&](const kdata::Record& race) -> std::optional<CreatorRace>
	{
		// Characters::races(true): a body mesh, `playable`, and an editor limits file (charsel23 section 3: the race must also be in a group).
		if (!race.boolOr("playable", false) || !anyFile(race, {"male mesh", "female mesh"}) || race.fileOr("editor limits").empty())
			return std::nullopt;
		CreatorRace out;
		out.sid = race.stringID;
		out.name = race.name;
		out.heads[0] = refSids(race, "heads male");
		out.heads[1] = refSids(race, "heads female");
		for (const std::string& sid : refSids(race, "hairs"))
		{
			const kdata::Record& hair = *gameData.find(sid);
			// The race's empty records (Bald, No beard) are the picker's none; the rest are choices (Characters::choices).
			if (!anyFile(hair, {"mesh", "mesh female", "head texture", "head texture female"}))
				continue;
			(hair.intOr("attach slot", 2) == 13 ? out.beards : out.hairs).push_back(sid);
		}
		out.hairColours = refSids(race, "hair colors");
		const std::string editor = editorFile(dataDir, race.fileOr("editor limits"));
		out.sliders[0] = readRanges(editor, false);
		out.sliders[1] = readRanges(editor, true);
		return out;
	};
	for (const kdata::Record* group : gameData.byType(kworld::itemTypeByName("RACE_GROUP")))
	{
		for (const std::string& sid : refSids(*group, "races"))
		{
			const kdata::Record& race = *gameData.find(sid);
			if (race.type != raceType || catalog.find(sid) != nullptr)
				continue;
			if (std::optional<CreatorRace> found = build(race))
				catalog.races_.push_back(std::move(*found));
		}
	}
	// The Idle stance slider: the idle ANIMATION records the editor lists (AppearanceManager 14007d480; the same filter as the client's
	// Characters::idleStances: not disabled, weather type < 1, not on the overlay layer, `idle`, has weapon L = R = 0, not crouched).
	for (const kdata::Record* r : gameData.byType(kworld::itemTypeByName("ANIMATION")))
	{
		std::string layer = r->stringOr("layer", "all");
		std::transform(layer.begin(), layer.end(), layer.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (!r->boolOr("disabled", false) && r->intOr("weather type", 0) <= 0 && r->boolOr("idle", false) && layer != "overlay"
			&& r->intOr("has weapon L", 2) == 0 && r->intOr("has weapon R", 2) == 0 && !r->boolOr("crouched", false))
			++catalog.stanceCount_;
	}
	return catalog;
}

CreatorCatalog CreatorCatalog::from(std::vector<CreatorRace> races, std::vector<std::string> raceRecords, int stanceCount)
{
	CreatorCatalog catalog;
	catalog.races_ = std::move(races);
	catalog.raceRecords_ = std::move(raceRecords);
	catalog.stanceCount_ = stanceCount;
	return catalog;
}

const CreatorRace* CreatorCatalog::find(std::string_view sid) const noexcept
{
	for (const CreatorRace& race : races_)
	{
		if (race.sid == sid)
			return &race;
	}
	return nullptr;
}

std::vector<std::string> CreatorCatalog::sids() const
{
	std::set<std::string> all;
	for (const CreatorRace& race : races_)
	{
		all.insert(race.sid);
		for (const std::vector<std::string>* list : {&race.heads[0], &race.heads[1], &race.hairs, &race.beards, &race.hairColours})
			all.insert(list->begin(), list->end());
	}
	return {all.begin(), all.end()};
}

std::vector<std::string> CreatorCatalog::allowedRaces(const std::vector<std::string>& forceRace) const
{
	std::vector<std::string> allowed;
	for (const CreatorRace& race : races_)
	{
		if (forceRace.empty() || inList(forceRace, race.sid))
			allowed.push_back(race.sid);
	}
	if (allowed.empty())
	{
		for (const CreatorRace& race : races_)
			allowed.push_back(race.sid);
	}
	return allowed;
}

std::string CreatorCatalog::check(const std::vector<std::string>& forceRace, const std::vector<CharacterSpec>& specs,
	const std::vector<SquadSlot>& slots) const
{
	const std::vector<std::string> allowed = allowedRaces(forceRace);
	for (std::size_t index = 0; index < specs.size(); ++index)
	{
		const CharacterSpec& spec = specs[index];
		if (index < slots.size() && !slots[index].editable)
		{
			const SquadSlot& slot = slots[index];
			if (spec.race != slot.race || spec.female != slot.female || !spec.head.empty() || !spec.hair.empty() || !spec.beard.empty()
				|| !spec.hairColour.empty() || spec.skin != 0.0f || !spec.sliders.empty() || spec.stance != 0)
				return "spec_mismatch";
			if (const std::string reason = checkCharacterName(spec.name); !reason.empty())
				return reason;
			continue;
		}
		if (!inList(raceRecords_, spec.race))
			return "unknown_race";
		const CreatorRace* race = find(spec.race);
		if (race == nullptr)
			return "race_not_playable";
		if (!inList(allowed, spec.race))
			return "race_forced";
		if (spec.female > 1 || !race->sex(spec.female != 0))
			return "sex_not_available";
		const std::vector<std::string>& heads = race->heads[spec.female];
		if (heads.empty() ? !spec.head.empty() : !inList(heads, spec.head))
			return "bad_head";
		if (!spec.hair.empty() && !inList(race->hairs, spec.hair))
			return "bad_hair";
		// The creator offers no beard picker for a female character.
		if (!spec.beard.empty() && (spec.female != 0 || !inList(race->beards, spec.beard)))
			return "bad_beard";
		if (race->hairColours.empty() ? !spec.hairColour.empty() : !inList(race->hairColours, spec.hairColour))
			return "bad_hair_colour";
		// A value outside its range is rejected, not clamped: the creator clamps its own sliders, so a valid client never sends one
		// (the original's editor clamps typed values; MULTIPLAYER 4b says reject). NaN and infinity are frame faults, kicked earlier.
		const std::vector<EditorSlider>& sliders = race->sliders[spec.female];
		std::set<std::string> seen;
		for (const auto& [name, value] : spec.sliders)
		{
			const auto found = std::find_if(sliders.begin(), sliders.end(), [&](const EditorSlider& s) { return s.name == name; });
			if (found == sliders.end() || !seen.insert(name).second)
				return "bad_slider";
			if (value < std::min(found->min, found->max) || value > std::max(found->min, found->max))
				return "slider_range";
		}
		if (!(spec.skin >= 0.0f && spec.skin <= 1.0f))
			return "slider_range";
		if (spec.stance < 1 || spec.stance > stanceCount_)
			return "slider_range";
		if (const std::string reason = checkCharacterName(spec.name); !reason.empty())
			return reason;
	}
	return "";
}

std::string checkCharacterName(const std::string& name)
{
	bool visible = false;
	for (const char c : name)
	{
		const auto byte = static_cast<unsigned char>(c);
		if (byte < 0x20 || byte == 0x7F)
			return "bad_character_name";
		visible = visible || (byte != ' ' && byte != 0xC2 && byte != 0xA0);
	}
	if (!visible)
		return "empty_name";
	return validSquadName(name) ? "" : "bad_character_name";
}

std::string encodeSpecs(const std::vector<CharacterSpec>& specs)
{
	std::string out;
	putNumber(out, static_cast<std::int64_t>(specs.size()));
	for (const CharacterSpec& spec : specs)
	{
		putString(out, spec.race);
		putNumber(out, spec.female);
		putString(out, spec.head);
		putString(out, spec.hair);
		putString(out, spec.beard);
		putString(out, spec.hairColour);
		putNumber(out, std::bit_cast<std::uint32_t>(spec.skin));
		putNumber(out, static_cast<std::int64_t>(spec.sliders.size()));
		for (const auto& [name, value] : spec.sliders)
		{
			putString(out, name);
			putNumber(out, std::bit_cast<std::uint32_t>(value));
		}
		putString(out, spec.name);
		putNumber(out, spec.stance);
	}
	return out;
}

bool decodeSpecs(std::string_view text, std::vector<CharacterSpec>& specs)
{
	specs.clear();
	Reader r(text);
	std::int64_t count = 0;
	if (!r.number(count) || count < 0 || count > 1000)
		return false;
	for (std::int64_t i = 0; i < count; ++i)
	{
		CharacterSpec spec;
		std::int64_t female = 0, skin = 0, sliders = 0, stance = 0;
		if (!r.string(spec.race) || !r.number(female) || !r.string(spec.head) || !r.string(spec.hair) || !r.string(spec.beard)
			|| !r.string(spec.hairColour) || !r.number(skin) || !r.number(sliders) || sliders < 0 || sliders > 1000)
			return false;
		spec.female = static_cast<std::uint8_t>(female);
		spec.skin = std::bit_cast<float>(static_cast<std::uint32_t>(skin));
		for (std::int64_t k = 0; k < sliders; ++k)
		{
			std::string name;
			std::int64_t bits = 0;
			if (!r.string(name) || !r.number(bits))
				return false;
			spec.sliders.emplace_back(std::move(name), std::bit_cast<float>(static_cast<std::uint32_t>(bits)));
		}
		if (!r.string(spec.name) || !r.number(stance))
			return false;
		spec.stance = static_cast<std::int32_t>(stance);
		specs.push_back(std::move(spec));
	}
	return r.atEnd();
}

} // namespace ksim
