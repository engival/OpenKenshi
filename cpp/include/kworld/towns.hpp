#pragma once

// Towns (docs/formats/WORLD_LAYOUT.md "leveldata.level - towns and roads").
//
// TOWN records themselves carry no position: the positional source is a
// GAMESTATE_TOWN_INSTANCE_LIST "0-townlist" instance in a layer's
// leveldata.level.
// Towns::placements() stacks the layers with the Zones::resolveLayers rule
// (zoneLayerRank order; same instance id replaces; empty target deletes).
// Evidence it applies: across the 3 layers no instance id changes target, none
// repeats within a layer, and 79 ids repeat with identical data (one moves).
// LevelDataFile::placements still lists every layer unmerged.

#include "kworld/coords.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kdata {
class GameData;
}

namespace kworld {

struct TownPlacement
{
	std::string instanceID;
	std::string targetID; // TOWN record stringID
	std::string stateID; // local GAMESTATE_TOWN "<id>-S5e"
	std::array<std::byte, 28> transform{};
	double x = 0.0; // world position, per WORLD_LAYOUT.md (not terrain-relative)
	double y = 0.0;
	double z = 0.0;

	int zonesCount = 0; // GAMESTATE_TOWN int `zones_count`
	// The town's hand (GAMESTATE_TOWN ints `handC` / `handCS`, townHand()); a placed building's state names its town with the same pair
	// (`townC` / `townCS`, ZoneInstance::townHand). 0 = none.
	std::uint64_t hand = 0;
	std::vector<std::pair<int, int>> declaredCells; // zone_x_k / zone_y_k

	ZoneCell cell{};
	std::string layer;
};

struct LevelDataFile
{
	std::string path;
	std::string layer;
	int version = 0;
	std::size_t recordCount = 0;
	std::size_t townStateCount = 0; // GAMESTATE_TOWN records
	std::size_t roadCount = 0; // ROAD records
	std::vector<TownPlacement> placements; // every 0-townlist instance
};

// Direct children of dataDir/newland/leveldata, sorted by path.
std::vector<LevelDataFile> loadLevelDataFiles(std::string_view dataDir);

struct TownRecord
{
	std::string stringID;
	std::string name;
	std::vector<std::size_t> placementIndices;
};

class Towns
{
public:
	static Towns load(std::string_view dataDir);
	static Towns join(const kdata::GameData& gameData, std::string_view dataDir);

	const std::vector<TownRecord>& towns() const noexcept { return towns_; }
	const std::vector<TownPlacement>& placements() const noexcept { return placements_; }
	const std::vector<LevelDataFile>& files() const noexcept { return files_; }

	std::size_t townsWithoutPlacement() const noexcept;

private:
	std::vector<LevelDataFile> files_;
	std::vector<TownPlacement> placements_;
	std::vector<TownRecord> towns_;
};

} // namespace kworld
