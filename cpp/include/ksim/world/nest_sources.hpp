#pragma once

// The live nest sources of a BIOME_GROUP / BIOMES record: the one rule shared by placement (nav/placement.cpp), the default-world
// spawner (world/spawn.cpp) and the wilderness spawner (world/wilderness.cpp). A `nests` entry is a source when its target is a TOWN,
// its weight (v0) is positive and the TOWN's own `world state` list holds against the game's facts ("Fog Deathyard Invasion" holds
// only once the west hive queen is not okay). The next nest rule goes here, once.

#include "ksim/world/world_state.hpp"
#include "kdata/gamedata.hpp"

#include <cstdint>
#include <string_view>

namespace ksim
{

// The squad lists of a nest TOWN, in world_model.py's build_spawn_sources order.
inline constexpr std::string_view kNestSquadLists[] = {"residents", "default resident", "bar squads", "roaming squads"};

// Calls fn(const Record& nest, const RefEntry& entry) for each live nest source of `owner`'s `nests` list.
template <typename Func>
void forEachLiveNest(const kdata::GameData& data, const worldstate::Facts& facts, std::int32_t townType,
	const kdata::Record& owner, Func&& fn)
{
	const kdata::RefCategory* refs = owner.findRefs("nests");
	if (refs == nullptr)
		return;
	for (const kdata::RefEntry& entry : refs->entries)
	{
		if (entry.opaqueEntry || entry.v0 <= 0)
			continue;
		const kdata::Record* nest = data.find(entry.targetID);
		if (nest != nullptr && nest->type == townType && worldstate::listHolds(data, facts, *nest))
			fn(*nest, entry);
	}
}

} // namespace ksim
