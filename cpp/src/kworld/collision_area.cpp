#include "kworld/collision_area.hpp"

#include "kworld/features.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/towns.hpp"
#include "kworld/zones.hpp"

#include <algorithm>
#include <string>

namespace kworld {

std::vector<WorldShape> AreaGeometry::flatShapes() const
{
	std::vector<const AreaBuilding*> sorted;
	for (const AreaBuilding& building : buildings)
		sorted.push_back(&building);
	std::sort(sorted.begin(), sorted.end(), [](const AreaBuilding* a, const AreaBuilding* b) { return a->id < b->id; });
	std::vector<WorldShape> all;
	for (const AreaBuilding* building : sorted)
		if (PathMode::Ignore != building->nav.pathMode)
			all.insert(all.end(), building->shapes->begin(), building->shapes->end());
	all.insert(all.end(), foliage->begin(), foliage->end());
	return all;
}

struct AreaGatherer::State
{
	std::string root;
	const Heightmap& terrain;
	std::unique_ptr<kdata::GameData> ownedGameData; // null when the caller's game data is shared
	const kdata::GameData& gameData;
	CollisionLibrary library;
	std::vector<PlacedInstance> placed;
	BiomeMap biomes;
	Features features;
	Towns towns;
	InteriorLayouts layouts;

	State(std::string_view dataDir, const Heightmap& heightmap, const kdata::GameData* shared)
		: root(dataDir), terrain(heightmap),
		ownedGameData(shared == nullptr ? std::make_unique<kdata::GameData>(loadMergedGameData(dataDir)) : nullptr),
		gameData(shared != nullptr ? *shared : *ownedGameData),
		library(dataDir, gameData), placed(Zones::load(dataDir).placeMerged(heightmap, gameData)),
		biomes(BiomeMap::load(root + "/newland/land/biomemap.png")),
		features(Features::join(gameData, dataDir)), towns(Towns::join(gameData, dataDir)),
		layouts(InteriorLayouts::load(dataDir))
	{
	}
};

AreaGatherer::AreaGatherer(std::string_view dataDir, const Heightmap& terrain)
	: state_(std::make_unique<State>(dataDir, terrain, nullptr))
{
}

AreaGatherer::AreaGatherer(std::string_view dataDir, const Heightmap& terrain, const kdata::GameData& gameData)
	: state_(std::make_unique<State>(dataDir, terrain, &gameData))
{
}

AreaGatherer::~AreaGatherer() = default;

AreaSources AreaGatherer::sources() const
{
	const State& s = *state_;
	return { s.gameData, s.layouts, s.terrain, s.biomes, s.features, s.towns };
}

AreaGeometry AreaGatherer::gather(ZoneCell min, ZoneCell max)
{
	State& s = *state_;
	AreaGeometry area;
	for (const PlacedInstance& placed : s.placed)
	{
		const ZoneCell cell = placed.positionCell;
		if (cell.x >= min.x - 1 && cell.x <= max.x + 1 && cell.z >= min.z - 1 && cell.z <= max.z + 1)
		{
			std::vector<WorldShape> shapes;
			const auto nav = s.library.addBuilding(placed, shapes, area.coverage);
			// The furniture belongs to the building: its shapes go and come with it (a runtime removal, a destroyed building).
			if (const InteriorLayout* layout = nav ? s.layouts.of(placed) : nullptr)
				s.library.addFurniture(placed, *layout, shapes, area.coverage);
			if (nav)
				area.buildings.push_back({ placementId(placed), placed, *nav,
					std::make_shared<const std::vector<WorldShape>>(std::move(shapes)) });
		}
	}

	std::vector<WorldShape> foliage;
	for (int z = std::max(min.z - 1, 0); z <= std::min(max.z + 1, kZoneCellsPerAxis - 1); ++z)
		for (int x = std::max(min.x - 1, 0); x <= std::min(max.x + 1, kZoneCellsPerAxis - 1); ++x)
			for (const FoliageInstance& tree :
				generateFoliage({ x, z }, s.terrain, s.biomes, s.gameData, s.features, s.towns))
				s.library.addFoliage(tree, foliage, area.coverage);
	area.foliage = std::make_shared<const std::vector<WorldShape>>(std::move(foliage));
	return area;
}

AreaGeometry gatherArea(std::string_view dataDir, const Heightmap& terrain,
	ZoneCell min, ZoneCell max)
{
	return AreaGatherer(dataDir, terrain).gather(min, max);
}

} // namespace kworld
