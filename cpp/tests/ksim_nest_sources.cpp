// One nest-source rule (ksim/world/nest_sources.hpp) for placement, the default-world spawner and the wilderness spawner, judged on the
// game's facts: a unique NPC dead in the world's WorldEventState (not at its default) turns a nest TOWN off as a source. The shared
// function sees it, and so does a WildernessSpawner built on that world (its slots' nest quota drops); a scratch world could not.
#include "ksim/core/state.hpp"
#include "ksim/world/nest_sources.hpp"
#include "ksim/world/wilderness.hpp"
#include "ksim/world/world_state.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <iostream>
#include <set>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

std::int64_t nestQuotas(const WildernessSpawner& spawner, const flecs::world& ecs)
{
	std::int64_t total = 0;
	for (const WildernessSlotCount& slot : spawner.counts(ecs))
		total += slot.nestQuota;
	return total;
}

} // namespace

int main()
{
	try
	{
		const auto data = std::make_shared<kdata::GameData>(kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA)));
		const std::int32_t groupType = kworld::itemTypeByName("BIOME_GROUP");
		const std::int32_t townType = kworld::itemTypeByName("TOWN");

		flecs::world fresh; // a new game: every unique NPC alive
		worldstate::Facts defaults;
		worldstate::collectStatuses(fresh, defaults);

		// A live nest source whose `world state` names an NPC: marking that NPC dead must turn it off.
		std::string flipSid, flipNest;
		for (const kdata::Record* group : data->byType(groupType))
		{
			forEachLiveNest(*data, defaults, townType, *group, [&](const kdata::Record& nest, const kdata::RefEntry&)
			{
				if (!flipSid.empty())
					return;
				const kdata::RefCategory* states = nest.findRefs("world state");
				for (const kdata::RefEntry& stateRef : states != nullptr ? states->entries : std::vector<kdata::RefEntry>{})
				{
					const kdata::Record* state = data->find(stateRef.targetID);
					for (const char* category : {"NPC is", "NPC is NOT"})
					{
						const kdata::RefCategory* npcs = state != nullptr ? state->findRefs(category) : nullptr;
						for (const kdata::RefEntry& npc : npcs != nullptr ? npcs->entries : std::vector<kdata::RefEntry>{})
						{
							worldstate::Facts trial = defaults;
							trial.npcStatus[npc.targetID] = worldstate::kDead;
							if (flipSid.empty() && !worldstate::listHolds(*data, trial, nest))
							{
								flipSid = npc.targetID;
								flipNest = nest.stringID;
							}
						}
					}
				}
			});
		}
		test::require(!flipSid.empty(), "no live nest source with a world state an NPC death turns off");
		std::cout << "nest " << flipNest << " goes off when " << flipSid << " is dead\n";

		flecs::world changed;
		state::WorldEventState events;
		events.dead.push_back(flipSid);
		changed.set<state::WorldEventState>(events);
		worldstate::Facts moved;
		worldstate::collectStatuses(changed, moved);

		// the shared function
		const auto sources = [&](const worldstate::Facts& facts)
		{
			std::set<std::string> found;
			for (const kdata::Record* group : data->byType(groupType))
				forEachLiveNest(*data, facts, townType, *group, [&](const kdata::Record& nest, const kdata::RefEntry&) { found.insert(nest.stringID); });
			return found;
		};
		test::require(sources(defaults).count(flipNest) != 0, "the nest is a source at the defaults");
		test::require(sources(moved).count(flipNest) == 0, "the nest is no source once its NPC is dead");

		// the wilderness spawner reads the world it is built on
		const WildernessSpawner before(*data, KENSHI_DATA, fresh);
		const WildernessSpawner after(*data, KENSHI_DATA, changed);
		test::require(nestQuotas(after, changed) <= nestQuotas(before, fresh), "a dead NPC cannot add nest quota");
		std::cout << "nest quota " << nestQuotas(before, fresh) << " -> " << nestQuotas(after, changed) << "\n";
		std::cout << "ksim_nest_sources: ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_nest_sources: FAILED: " << error.what() << "\n";
		return 1;
	}
}
