#include "kworld/towns.hpp"

#include "kworld/error.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/zones.hpp"
#include "io.hpp"
#include "layer_stack.hpp"
#include "le.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>

namespace kworld {

namespace {

bool findIntLast(const kdata::Record& record, std::string_view key, std::int32_t& out)
{
	for (auto entry = record.ints.rbegin(); entry != record.ints.rend(); ++entry)
		if (entry->key == key)
		{
			out = entry->value;
			return true;
		}
	return false;
}

LevelDataFile loadOne(const std::filesystem::path& path, std::string_view layer)
{
	LevelDataFile result;
	result.path = path.string();
	result.layer = std::string(layer);

	auto bytes = detail::readFile(result.path);
	auto file = kdata::parse(bytes, path.filename().string());
	result.version = file.version;
	result.recordCount = file.records.size();

	const std::int32_t townType = itemTypeByName("GAMESTATE_TOWN");
	const std::int32_t roadType = itemTypeByName("ROAD");

	// WORLD_LAYOUT.md's survey joins each 0-townlist instance to the
	// GAMESTATE_TOWN record named by its first state (last duplicate wins).
	std::unordered_map<std::string, const kdata::Record*> states;
	for (const auto& record : file.records)
	{
		if (record.type == townType)
		{
			++result.townStateCount;
			states[record.stringID] = &record;
		}
		if (record.type == roadType)
			++result.roadCount;
	}

	for (const auto& record : file.records)
	{
		if (record.stringID != "0-townlist")
			continue;
		for (const auto& instance : record.instances)
		{
			TownPlacement placement;
			placement.instanceID = instance.idIsString
				? instance.id : std::to_string(instance.idInt);
			placement.targetID = instance.targetStringID;
			placement.transform = instance.transform;
			placement.x = detail::readF32(instance.transform.data());
			placement.y = detail::readF32(instance.transform.data() + 4);
			placement.z = detail::readF32(instance.transform.data() + 8);
			placement.cell = zoneCellOf(placement.x, placement.z);
			placement.stateID = instance.states.empty() ? "" : instance.states.front();
			placement.layer = result.layer;

			const auto state = states.find(placement.stateID);
			if (state != states.end())
			{
				findIntLast(*state->second, "zones_count", placement.zonesCount);
				std::int32_t handC = 0;
				std::int32_t handCS = 0;
				if (findIntLast(*state->second, "handC", handC) && findIntLast(*state->second, "handCS", handCS))
					placement.hand = townHand(handC, handCS);
				for (int k = 0; k < placement.zonesCount; ++k)
				{
					std::int32_t x = 0;
					std::int32_t z = 0;
					// UNSURE: what a lone zone_x_k/zone_y_k means is not
					// specified; only complete pairs are recorded.
					if (findIntLast(*state->second, "zone_x_" + std::to_string(k), x)
						&& findIntLast(*state->second, "zone_y_" + std::to_string(k), z))
						placement.declaredCells.emplace_back(x, z);
				}
			}
			result.placements.push_back(std::move(placement));
		}
	}
	return result;
}

} // namespace

std::vector<LevelDataFile> loadLevelDataFiles(std::string_view dataDir)
{
	namespace fs = std::filesystem;

	const fs::path root = fs::absolute(fs::path(dataDir)) / "newland" / "leveldata";
	std::error_code error;
	if (!fs::is_directory(root, error))
		throw Error("towns: no leveldata directory at " + root.string());

	std::vector<fs::path> files;
	for (fs::directory_iterator it(root, error), end; it != end; it.increment(error))
	{
		if (error)
			throw Error("towns: cannot walk " + root.string() + ": " + error.message());
		if (!it->is_directory(error))
			continue;
		const fs::path candidate = it->path() / "leveldata.level";
		if (fs::is_regular_file(candidate, error))
			files.push_back(candidate);
	}
	if (error)
		throw Error("towns: cannot walk " + root.string() + ": " + error.message());

	// Same order as the Python survey's sorted(glob(root/*/leveldata.level)).
	std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
		return a.string() < b.string();
	});

	std::vector<LevelDataFile> result;
	result.reserve(files.size());
	for (const auto& path : files)
		result.push_back(loadOne(path, path.parent_path().filename().string()));
	return result;
}

Towns Towns::join(const kdata::GameData& gameData, std::string_view dataDir)
{
	Towns towns;
	towns.files_ = loadLevelDataFiles(dataDir);

	// Same stacking rule as Zones::resolveLayers: layers in load order, a
	// repeated instance id replaces the earlier entry, an empty target deletes.
	std::vector<const LevelDataFile*> ordered;
	for (const auto& file : towns.files_)
		ordered.push_back(&file);
	std::stable_sort(ordered.begin(), ordered.end(),
		[](const LevelDataFile* a, const LevelDataFile* b) {
			return zoneLayerRank(a->layer) < zoneLayerRank(b->layer);
		});
	detail::LayerStack<TownPlacement> stack;
	for (const LevelDataFile* file : ordered)
		for (const auto& placement : file->placements)
		{
			if (placement.targetID.empty())
				stack.remove(placement.instanceID);
			else
				stack.put(placement.instanceID, placement);
		}
	towns.placements_ = std::move(stack).survivors();

	const std::int32_t townType = itemTypeByName("TOWN");
	if (townType < 0)
		throw Error("towns: TOWN missing from the item type table");

	std::unordered_map<std::string, std::size_t> index;
	for (const kdata::Record* record : gameData.byType(townType))
	{
		index.emplace(record->stringID, towns.towns_.size());
		towns.towns_.push_back({ record->stringID, record->name, {} });
	}
	for (std::size_t i = 0; i < towns.placements_.size(); ++i)
	{
		const auto found = index.find(towns.placements_[i].targetID);
		if (found != index.end())
			towns.towns_[found->second].placementIndices.push_back(i);
	}
	return towns;
}

Towns Towns::load(std::string_view dataDir)
{
	return join(loadMergedGameData(dataDir), dataDir);
}

std::size_t Towns::townsWithoutPlacement() const noexcept
{
	std::size_t count = 0;
	for (const auto& town : towns_)
		if (town.placementIndices.empty())
			++count;
	return count;
}

} // namespace kworld
