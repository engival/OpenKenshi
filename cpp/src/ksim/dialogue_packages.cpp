// Package sets, the event index and the static pre-pass (docs/DIALOGUE.md 1.2, 4 lever 3).
//
// The exe (DIALOGUE_SELECTION_STATIC Q5, Character::vf111 14062b210 and the package walk 14067fab0):
//   - a speaker's source list is the FIRST non-empty of its levels, never a merge of levels:
//       animal         : the squad template's `dialog animal`
//       player faction : `dialogue package player`, else the record `5369-gamedata.base`
//       leader         : squad `dialog leader`, else own `dialogue package`, else squad `dialog squad`
//       other member   : squad `dialog squad`, else own `dialogue package`
//   - within the chosen list the result is a union; each package is walked parents first
//     (`inheritsFrom`), then its own `dialogs`; a dialogue already present under an event is not added
//     again; a package whose name contains "Unfinished" is skipped;
//   - the character's own `dialogue` roots are always added under EV_PLAYER_TALK_TO_ME.
// UNSURE (the exe text for it was not found): a human with nothing from the above falls back to its
// faction's `dialog default` (fcs.def: "any characters that don't have a dialogue package will get this").

#include "ksim/dialogue_book.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <set>

namespace ksim {

namespace {

constexpr int kEvPlayerTalkToMe = 1;
constexpr std::string_view kPlayerDefaultPackage = "5369-gamedata.base"; // record name `player character default`

template <typename Func>
void forEachRef(const kdata::Record& record, std::string_view category, Func&& func)
{
	const kdata::RefCategory* refs = record.findRefs(category);
	if (refs == nullptr)
		return;
	for (const kdata::RefEntry& entry : refs->entries)
	{
		if (!entry.opaqueEntry)
			func(entry);
	}
}

std::string setKey(std::span<const std::uint32_t> list, std::span<const std::uint32_t> extraRoots)
{
	std::string key;
	for (const std::uint32_t id : list)
		key.append(reinterpret_cast<const char*>(&id), sizeof id);
	key.push_back('|');
	for (const std::uint32_t id : extraRoots)
		key.append(reinterpret_cast<const char*>(&id), sizeof id);
	return key;
}

bool listHas(std::span<const std::uint32_t> list, std::uint32_t id) noexcept
{
	return std::find(list.begin(), list.end(), id) != list.end();
}

} // namespace

PackageSetId DialogueBook::internSet(std::span<const std::uint32_t> list, std::span<const std::uint32_t> extraRoots) const
{
	const std::string key = setKey(list, extraRoots);
	const std::lock_guard<std::mutex> lock(setMutex_);
	if (auto found = setIds_.find(key); found != setIds_.end())
		return found->second;

	PackageSet set;
	std::unordered_map<std::uint32_t, std::uint32_t> position; // dialogue index -> position in set.roots
	std::set<std::uint32_t> visited;
	auto add = [&](std::uint32_t dialogue, int event)
	{
		auto [it, added] = position.try_emplace(dialogue, static_cast<std::uint32_t>(set.roots.size()));
		if (added)
			set.roots.push_back(dialogue);
		std::vector<std::uint32_t>& slot = set.byEvent[event];
		if (std::find(slot.begin(), slot.end(), it->second) == slot.end())
			slot.push_back(it->second);
	};
	for (const std::uint32_t dialogue : extraRoots)
		add(dialogue, kEvPlayerTalkToMe);
	auto walk = [&](auto&& self, std::uint32_t package) -> void
	{
		auto def = packages_.find(package);
		if (def == packages_.end() || def->second.skipped || !visited.insert(package).second)
			return;
		for (const std::uint32_t parent : def->second.inherits)
			self(self, parent);
		for (const PackageDef::Entry& entry : def->second.entries)
		{
			for (const std::uint8_t event : entry.events)
			{
				if (event != 0)
					add(entry.dialogue, event);
			}
		}
	};
	for (const std::uint32_t package : list)
		walk(walk, package);
	set.sources.assign(visited.begin(), visited.end());

	sets_.push_back(std::move(set));
	const PackageSetId id = static_cast<PackageSetId>(sets_.size() - 1);
	setIds_.emplace(key, id);
	return id;
}

PackageSetId DialogueBook::packageSetOf(std::span<const std::string_view> packages, std::span<const std::string_view> extraRoots) const
{
	std::vector<std::uint32_t> list, extra;
	for (const std::string_view package : packages)
	{
		if (const std::uint32_t id = idOf(package); id != kNoIndex)
			list.push_back(id);
	}
	for (const std::string_view root : extraRoots)
	{
		if (const std::uint32_t index = dialogueIndex(root); index != kNoIndex)
			extra.push_back(index);
	}
	return internSet(list, extra);
}

PackageSetId DialogueBook::packageSetFor(const SpeakerKey& key) const
{
	std::vector<std::uint32_t> list, extra;
	auto collect = [&](const kdata::Record* record, std::string_view category)
	{
		std::vector<std::uint32_t> out;
		if (record == nullptr)
			return out;
		forEachRef(*record, category, [&](const kdata::RefEntry& entry)
		{
			if (const std::uint32_t id = idOf(entry.targetID); id != kNoIndex && packages_.count(id) != 0)
				out.push_back(id);
		});
		return out;
	};
	const kdata::Record* character = key.character.empty() ? nullptr : data_->find(key.character);
	const kdata::Record* squad = key.squadTemplate.empty() ? nullptr : data_->find(key.squadTemplate);
	const kdata::Record* faction = key.faction.empty() ? nullptr : data_->find(key.faction);
	auto firstNonEmpty = [&](auto... lists)
	{
		std::vector<std::uint32_t> result;
		((result.empty() ? (void)(result = lists()) : (void)0), ...);
		return result;
	};
	if (key.animal)
	{
		list = collect(squad, "dialog animal");
	} else if (key.playerFaction) {
		list = collect(character, "dialogue package player");
		if (list.empty())
		{
			if (const std::uint32_t id = idOf(kPlayerDefaultPackage); id != kNoIndex && packages_.count(id) != 0)
				list.push_back(id);
		}
	} else if (key.leader) {
		list = firstNonEmpty([&] { return collect(squad, "dialog leader"); }, [&] { return collect(character, "dialogue package"); },
			[&] { return collect(squad, "dialog squad"); }, [&] { return collect(faction, "dialog default"); });
	} else {
		list = firstNonEmpty([&] { return collect(squad, "dialog squad"); }, [&] { return collect(character, "dialogue package"); },
			[&] { return collect(faction, "dialog default"); });
	}
	if (character != nullptr)
	{
		forEachRef(*character, "dialogue", [&](const kdata::RefEntry& entry)
		{
			if (const std::uint32_t index = dialogueIndex(entry.targetID); index != kNoIndex)
				extra.push_back(index);
		});
	}
	return internSet(list, extra);
}

const PackageSet& DialogueBook::packageSet(PackageSetId id) const
{
	const std::lock_guard<std::mutex> lock(setMutex_);
	return sets_[id]; // a deque: references stay valid as sets are added
}

std::size_t DialogueBook::packageSetCount() const
{
	const std::lock_guard<std::mutex> lock(setMutex_);
	return sets_.size();
}

SpeakerFacts DialogueBook::characterFacts(std::string_view characterSid) const
{
	SpeakerFacts facts;
	const kdata::Record* record = data_->find(characterSid);
	if (record == nullptr)
		return facts;
	facts.character = record->stringID;
	forEachRef(*record, "race", [&](const kdata::RefEntry& entry)
	{
		if (facts.race.empty())
		{
			if (const kdata::Record* race = data_->find(entry.targetID))
				facts.race = race->stringID;
		}
	});
	facts.trader = record->boolOr("is trader", false);
	return facts;
}

std::string DialogueBook::staticExclusion(const Gate& gate, const SpeakerFacts& facts, std::span<const std::uint32_t> packages) const
{
	// Personality: the speaker's own value (HAS_TAG is the speaker's memory of the target, not static).
	if (facts.personality >= 0 && facts.personality < 32)
	{
		const std::uint32_t bit = 1u << facts.personality;
		if (gate.reqPersonality != 0 && (gate.reqPersonality & bit) == 0)
			return "personality not required";
		if ((gate.forbPersonality & bit) != 0)
			return "personality forbidden";
	}
	auto missing = [&](GateList list, std::string_view value)
	{
		const std::span<const std::uint32_t> ids = this->ids(gate.lists[list]);
		if (ids.empty() || value.empty()) // empty value = unknown speaker fact: undecided, left to run time
			return false;
		const std::uint32_t id = idOf(value);
		return id == kNoIndex || !listHas(ids, id);
	};
	if (missing(GlMyFaction, facts.faction))
		return "my faction";
	if (missing(GlMyRace, facts.race))
		return "my race";
	if (missing(GlMySubrace, facts.subrace))
		return "my subrace";
	if (missing(GlIsCharacter, facts.character))
		return "is character";
	if (const std::span<const std::uint32_t> wanted = ids(gate.lists[GlHasPackage]); !wanted.empty())
	{
		// The speaker has the package when it walked it (its list, or anything inherited).
		const bool any = std::any_of(wanted.begin(), wanted.end(), [&](std::uint32_t id) { return listHas(packages, id); });
		if (!any)
			return "has package";
	}
	// Speaker-side booleans fixed at spawn. A pass still runs at run time: DC_IS_FEMALE retargets when it holds.
	for (const CondDef& cond : conds(gate.conds))
	{
		if (cond.onTarget || cond.op != CondOp::Bool)
			continue;
		bool actual;
		switch (cond.kind)
		{
		case dc::IsFemale: actual = facts.female; break;
		case dc::HasRobotLimbs: actual = facts.robotLimbs; break;
		case dc::IsATrader: actual = facts.trader; break;
		case dc::IsLeader: actual = facts.leader; break;
		default: continue;
		}
		if (actual != (cond.value > 0))
			return dialogueCondName(cond.kind);
	}
	return {};
}

StaticPass DialogueBook::staticPass(const PackageSet& set, const SpeakerFacts& facts) const
{
	StaticPass pass(set.roots.size());
	for (std::size_t i = 0; i < set.roots.size(); ++i)
	{
		if (!staticExclusion(dialogues_[set.roots[i]].gate, facts, set.sources).empty())
			pass.clear(i);
	}
	return pass;
}

} // namespace ksim
