// World creation: no squad starts in a biome its spawn source does not list. A wilderness squad's sources are the BIOME_GROUP
// `homeless spawns` and the squad lists of the group's `nests`; a faction roaming squad's is its faction's `biomes`. Surveys every
// placed squad of a new game (seed 1) and prints the counts per spawn source and the mismatches (KSIM_SPAWN_BIOME_VERBOSE=1 lists
// them all).
#include "ksim/core/simulation.hpp"
#include "ksim/nav/placement.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/world/spawn.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

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

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = 1;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		const kdata::GameData& data = *sim.gameData();
		const AreaLookup areas(KENSHI_DATA);
		const std::int32_t groupType = kworld::itemTypeByName("BIOME_GROUP");
		const std::int32_t townType = kworld::itemTypeByName("TOWN");

		flecs::world scratch;
		RngStream unused(1);
		const SquadBuilder judge(scratch, data, unused);

		// squad template -> colours of the BIOME_GROUPs that list it
		std::map<std::string, std::set<std::uint32_t>> allowed;
		for (const kdata::Record* group : data.byType(groupType))
		{
			const std::uint32_t colour = static_cast<std::uint32_t>(group->intOr("index", 0));
			forEachRef(*group, "homeless spawns", [&](const kdata::RefEntry& entry) { allowed[entry.targetID].insert(colour); });
			forEachRef(*group, "nests", [&](const kdata::RefEntry& nestEntry)
			{
				const kdata::Record* nest = data.find(nestEntry.targetID);
				if (nest == nullptr || nest->type != townType || !judge.worldStateHolds(*nest))
					return; // a nest whose own `world state` fails is no source
				for (const char* list : {"residents", "default resident", "bar squads", "roaming squads"})
					forEachRef(*nest, list, [&](const kdata::RefEntry& entry) { allowed[entry.targetID].insert(colour); });
			});
		}

		const bool verbose = std::getenv("KSIM_SPAWN_BIOME_VERBOSE") != nullptr;
		std::map<int, std::size_t> total, listed, bad, unlisted;
		std::size_t shown = 0;
		sim.world().ecs().each([&](const state::SquadInstance& squad, const state::Position& position)
		{
			if (squad.homeTown.id != 0)
				return;
			const int source = static_cast<int>(squad.source);
			++total[source];
			const auto found = allowed.find(squad.templateSid);
			if (found == allowed.end())
			{
				++unlisted[source]; // faction roaming: no biome list, placed by the faction's territory
				return;
			}
			++listed[source];
			const std::uint32_t colour = areas.colourAt(position.x, position.z);
			if (found->second.count(colour) != 0)
				return;
			++bad[source];
			if (verbose || shown++ < 12)
			{
				const kdata::Record* record = data.find(squad.templateSid);
				std::cout << "  outside: " << squad.templateSid << " " << (record != nullptr ? record->name : "?") << " source " << source
					<< " slot " << squad.slot << " at " << position.x << "," << position.z << " colour 0x" << std::hex << colour
					<< std::dec << "\n";
			}
		});
		std::size_t mismatches = 0;
		for (const auto& [source, count] : total)
		{
			std::cout << "source " << source << ": " << count << " squads, " << listed[source] << " with a biome list, " << bad[source]
				<< " outside it, " << unlisted[source] << " with none\n";
			mismatches += bad[source];
		}
		test::require(mismatches == 0, std::to_string(mismatches) + " squads start in a biome their spawn source does not list");
		std::cout << "ksim_spawn_biome: ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_spawn_biome: FAILED: " << error.what() << "\n";
		return 1;
	}
}
