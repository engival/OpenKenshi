#include "ksim/world_state.hpp"

#include "ksim/state.hpp"
#include "kworld/gamedata.hpp"

namespace ksim::worldstate {

namespace {

const kdata::RefCategory* refs(const kdata::Record& record, std::string_view category)
{
	const kdata::RefCategory* found = record.findRefs(category);
	return found != nullptr && !found->entries.empty() ? found : nullptr;
}

std::int32_t statusOf(const Facts& facts, const std::string& character)
{
	// A unique character with no instance counts as alive (the initial snapshot: every unique NPC is alive).
	const auto found = facts.npcStatus.find(character);
	return found != facts.npcStatus.end() ? found->second : kAlive;
}

bool npcTest(const Facts& facts, const kdata::RefCategory* entries, bool equal)
{
	for (std::size_t i = 0; entries != nullptr && i < entries->entries.size(); ++i)
	{
		const kdata::RefEntry& entry = entries->entries[i];
		// v0 1 alive / 0 dead / 2 imprisoned (GAMEDATA_SCHEMA "WORLD_EVENT_STATE"). A status never seen is alive.
		const std::int32_t status = statusOf(facts, entry.targetID);
		const bool holds = status == entry.v0;
		if (holds != equal)
			return false;
	}
	return true;
}

bool factionTest(const Facts& facts, const kdata::RefCategory* entries, bool ally)
{
	for (std::size_t i = 0; entries != nullptr && i < entries->entries.size(); ++i)
	{
		const kdata::RefEntry& entry = entries->entries[i];
		const double relation = facts.playerRelation != nullptr ? facts.playerRelation(entry.targetID) : 0.0;
		const bool holds = ally ? relation >= kAllyRelation : relation <= kEnemyRelation;
		if (holds != (entry.v0 != 0))
			return false;
	}
	return true;
}

} // namespace

bool stateHolds(const kdata::GameData& data, const Facts& facts, const kdata::Record& state)
{
	(void)data; // the record carries its own entries; the data is for a caller that resolves them (town okay, later)
	if (!npcTest(facts, refs(state, "NPC is"), true) || !npcTest(facts, refs(state, "NPC is NOT"), false))
		return false;
	// `town okay` has no entries in the shipped data (WORLD_MODEL.md a); a state with one is not expressible yet (UNSURE: it fails).
	if (refs(state, "town okay") != nullptr)
		return false;
	return factionTest(facts, refs(state, "player ally"), true) && factionTest(facts, refs(state, "player enemy"), false);
}

bool stateHolds(const kdata::GameData& data, const Facts& facts, std::string_view sid)
{
	const kdata::Record* state = data.find(sid);
	return state != nullptr && stateHolds(data, facts, *state);
}

bool listHolds(const kdata::GameData& data, const Facts& facts, const kdata::Record& record, std::string_view category)
{
	const kdata::RefCategory* entries = refs(record, category);
	for (std::size_t i = 0; entries != nullptr && i < entries->entries.size(); ++i)
	{
		const kdata::RefEntry& entry = entries->entries[i];
		if (stateHolds(data, facts, entry.targetID) != (entry.v0 != 0))
			return false;
	}
	return true;
}

bool involved(const kdata::GameData& data, const Facts& facts, const kdata::Record& state)
{
	(void)data; // as stateHolds: the state's own entries are all it reads
	for (const std::string_view category : {"NPC is", "NPC is NOT"})
	{
		const kdata::RefCategory* entries = refs(state, category);
		for (std::size_t i = 0; entries != nullptr && i < entries->entries.size(); ++i)
			if (facts.playerTouched.count(entries->entries[i].targetID) != 0)
				return true;
	}
	return false;
}

void collectStatuses(const flecs::world& ecs, Facts& facts, const state::WorldEventState* events)
{
	if (events == nullptr)
		events = ecs.get<state::WorldEventState>(); // the world's own set, when there is one
	if (events != nullptr)
	{
		facts.playerTouched.insert(events->playerTouched.begin(), events->playerTouched.end());
		// The dead set first: it is what remembers a unique character whose body (its instance) is gone.
		for (const std::string& sid : events->dead)
			facts.npcStatus[sid] = kDead;
	}
	ecs.each([&](flecs::entity entity, const state::CharacterInstance& character)
	{
		if (character.templateSid.empty())
			return;
		std::int32_t status = kAlive;
		if (const state::Fighter* f = entity.get<state::Fighter>())
		{
			// A held slave or a caged body is imprisoned (UNSURE: an escaping slave is not counted).
			if (f->dead)
				status = kDead;
			else if (f->cageBuilding != 0 || f->slaveState == 1)
				status = kImprisoned;
		}
		auto [at, inserted] = facts.npcStatus.emplace(character.templateSid, status);
		if (!inserted && status == kDead)
			at->second = kDead; // several instances of one record: dead if any is
	});
}

std::size_t countStates(const kdata::GameData& data)
{
	return data.byType(kworld::itemTypeByName("WORLD_EVENT_STATE")).size();
}

std::size_t countTrue(const kdata::GameData& data, const Facts& facts)
{
	std::size_t count = 0;
	for (const kdata::Record* state : data.byType(kworld::itemTypeByName("WORLD_EVENT_STATE")))
		count += stateHolds(data, facts, *state) ? 1 : 0;
	return count;
}

} // namespace ksim::worldstate
