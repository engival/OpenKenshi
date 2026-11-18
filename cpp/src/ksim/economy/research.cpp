// Research state from the FCS data (kenshi-h2v.17.7, CRAFTING_SLICE 2.3, 3, 9.2).

#include "ksim/economy/research.hpp"

#include "kworld/gamedata.hpp"

#include <algorithm>

namespace ksim::research {

namespace {

constexpr std::array<std::string_view, kKindCount> kFields = {
	"enable item", "enable weapon type", "enable weapon model", "enable armour", "enable crossbow", "enable backpack", "enable robotics", "enable buildings"};

std::vector<std::string> refsOf(const kdata::Record& record, std::string_view category)
{
	std::vector<std::string> out;
	if (const kdata::RefCategory* refs = record.findRefs(category))
		for (const kdata::RefEntry& entry : refs->entries)
			if (!entry.opaqueEntry && !entry.targetID.empty())
				out.push_back(entry.targetID);
	return out;
}

} // namespace

std::string_view kindField(Kind kind) noexcept
{
	return kFields[static_cast<std::size_t>(kind)];
}

bool Snapshot::isUnlocked(Kind kind, const std::string& sid) const
{
	return unlockedSet_[static_cast<std::size_t>(kind)].count(sid) != 0;
}

const Row* Snapshot::row(const std::string& sid) const
{
	const auto found = rowOf_.find(sid);
	return found == rowOf_.end() ? nullptr : &rows[found->second];
}

Book::Book(const kdata::GameData& data)
	: data_(data)
{
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("RESEARCH")))
	{
		if (record->boolOr("REMOVED", false))
			continue;
		Entry entry;
		entry.sid = record->stringID;
		entry.level = record->intOr("level", 0);
		entry.blueprintOnly = record->boolOr("blueprint only", false);
		entry.levelUpgrade = record->boolOr("is level upgrade", false);
		entry.requirements = refsOf(*record, "requirements");
		for (std::size_t kind = 0; kind < kKindCount; ++kind)
			entry.enables[kind] = refsOf(*record, kFields[kind]);
		index_.emplace(entry.sid, entries_.size());
		entries_.push_back(std::move(entry));
	}
}

Snapshot Book::evaluate(const std::vector<std::string>& completed) const
{
	Snapshot out;
	out.completed = completed;
	std::unordered_set<std::string> finished(completed.begin(), completed.end());
	const std::int32_t armourType = kworld::itemTypeByName("ARMOUR");
	const std::int32_t crossbowType = kworld::itemTypeByName("CROSSBOW");
	const auto enable = [&](Kind kind, const std::string& sid)
	{
		const auto at = static_cast<std::size_t>(kind);
		if (out.unlockedSet_[at].insert(sid).second)
			out.unlocked[at].push_back(sid);
	};
	// Everything the finished records enable, in finish order; the techLevel with it.
	const auto unlock = [&]
	{
		out.techLevel = 1;
		for (auto& list : out.unlocked)
			list.clear();
		for (auto& set : out.unlockedSet_)
			set.clear();
		for (const std::string& sid : out.completed)
		{
			const auto found = index_.find(sid);
			if (found == index_.end())
			{
				// The exe's synthetic `blueprint only` research of an armour or crossbow (7.7): `<sid>.TECH.1` unlocks that record.
				constexpr std::string_view suffix = ".TECH.1";
				if (sid.size() > suffix.size() && sid.compare(sid.size() - suffix.size(), suffix.size(), suffix) == 0)
				{
					const std::string base = sid.substr(0, sid.size() - suffix.size());
					if (const kdata::Record* record = data_.find(base))
					{
						if (record->type == armourType)
							enable(Kind::Armour, base);
						else if (record->type == crossbowType)
							enable(Kind::Crossbow, base);
					}
				}
				continue;
			}
			const Entry& entry = entries_[found->second];
			if (entry.levelUpgrade)
				out.techLevel = std::max(out.techLevel, entry.level + 1);
			for (std::size_t kind = 0; kind < kKindCount; ++kind)
				for (const std::string& ref : entry.enables[kind])
					enable(static_cast<Kind>(kind), ref);
		}
	};
	// UNSURE (deferred: playtesting): a record that enables nothing at all (a level upgrade, an `improve buildings` one) is not finished silently:
	// the rule reads "everything it enables is unlocked", which would be vacuous for it, and finishing a level upgrade unasked is not the exe's way.
	const auto enablesNothingNew = [&](const Entry& entry)
	{
		bool any = false;
		for (std::size_t kind = 0; kind < kKindCount; ++kind)
			for (const std::string& ref : entry.enables[kind])
			{
				any = true;
				if (out.unlockedSet_[kind].count(ref) == 0)
					return false;
			}
		return any;
	};
	// The state of an unfinished record: Locked with a reason, or Listable (`silent`: it enables nothing that is not unlocked already).
	const auto classify = [&](const Entry& entry, bool& silent)
	{
		silent = false;
		if (entry.blueprintOnly)
			return Row{entry.sid, State::Locked, Reason::BlueprintOnly, entry.level};
		if (entry.level > out.techLevel)
			return Row{entry.sid, State::Locked, Reason::TechLevel, entry.level};
		for (const std::string& requirement : entry.requirements)
		{
			if (finished.count(requirement) != 0)
				continue;
			const auto found = index_.find(requirement);
			if (found == index_.end())
				return Row{entry.sid, State::Locked, Reason::Requirement, entry.level};
			const Entry& needed = entries_[found->second];
			if (needed.blueprintOnly)
				return Row{entry.sid, State::Locked, Reason::BlueprintOnly, entry.level};
			for (const std::string& deeper : needed.requirements)
				if (finished.count(deeper) == 0)
					return Row{entry.sid, State::Locked, Reason::Requirement, entry.level};
		}
		silent = enablesNothingNew(entry);
		return Row{entry.sid, silent ? State::Completed : State::Listable, Reason::None, entry.level};
	};
	// A record whose every enable is unlocked already is finished without being researched (9.2); that can unlock a level, or make another record
	// silent, so it repeats until nothing changes.
	for (bool changed = true; changed;)
	{
		changed = false;
		unlock();
		for (const Entry& entry : entries_)
		{
			bool silent = false;
			if (finished.count(entry.sid) == 0)
				classify(entry, silent);
			if (silent)
			{
				finished.insert(entry.sid);
				out.completed.push_back(entry.sid);
				changed = true;
			}
		}
	}
	for (const Entry& entry : entries_)
	{
		bool silent = false;
		out.rowOf_.emplace(entry.sid, out.rows.size());
		out.rows.push_back(finished.count(entry.sid) != 0 ? Row{entry.sid, State::Completed, Reason::None, entry.level} : classify(entry, silent));
	}
	return out;
}

} // namespace ksim::research
