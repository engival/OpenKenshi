#pragma once

// PROVISIONAL placement of wilderness squads (no home town, so spawnDefaultWorld
// gives them no position). Not decided: see ARCHITECTURE.md "Wilderness
// placement" for what the data says and the open design question.
//
// What the data gives (GAMEDATA_SCHEMA.md BIOME_GROUP, TERRAIN_FORMAT.md):
//   - areasmap.tga (256^2, 1,152 units per pixel) colour == BIOME_GROUP `index`;
//     biomemap.png (1024^2) colour == BIOMES `index` (legacy lists, 1 squad
//     is only in those);
//   - BIOME_GROUP `homeless spawns` (squad, relative chance) with `homeless
//     spawn amount` "approximately squads per zone", and `nests` (nest TOWN,
//     weight) with `num nests` "target number of nests in the biome". A nest
//     TOWN lists its own squads.
// The original therefore generates about 4,400 homeless squads by density
// (sum over groups of zones x amount) from 175 distinct templates; the spawner
// makes one persistent squad per template. For each such squad this picks:
//   1. an area, weighted by the squad's expected instances there
//      (zones x amount x chance / sum of chances; nests: num nests x weight /
//      sum of weights), over BIOME_GROUP then BIOMES lists;
//   2. else (faction roaming squads with no biome row) the FACTION `biomes`
//      territory, else the areas where the faction's other squads spawn,
//      else anywhere;
//   3. a point of that area, uniform over its pixels, on dry land above the
//      water plane and not on a cliff (heightmap only), 48 tries; an area
//      that is all water keeps its last sample (sea creatures).
// The point is the squad's den: its position and the centre of its patrol or
// territory (SquadInstance::den). The AI snaps it to the navmesh at its first
// step (ai.cpp). Randomness: kPlacementStream of the world seed, squads in
// Id order, so the result is a pure function of (data, seed).

#include "ksim/rng.hpp"
#include "ksim/state.hpp"

#include <flecs.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace kdata {
class GameData;
}

namespace ksim {

struct PlacementReport
{
	std::size_t squads = 0; // wilderness squads given a position
	std::size_t byBiomeGroup = 0; // area from BIOME_GROUP lists
	std::size_t byFactionTerritory = 0; // FACTION `biomes`
	std::size_t byFactionSpawns = 0; // where the faction's other squads spawn
	std::size_t anywhere = 0;
	std::size_t onWater = 0; // no dry point found in the area
	// Expected homeless squads by density, for the persistent-vs-dynamic note.
	double densitySquads = 0.0;
};

// The areas map and the heightmap, for the wilderness spawner (wilderness.hpp):
// where an area (BIOME_GROUP `index` colour) is and a dry point inside it. The
// same rules as the placer's step 3 above.
class WildGround
{
public:
	explicit WildGround(std::string_view dataDir);
	~WildGround();
	WildGround(const WildGround&) = delete;
	WildGround& operator=(const WildGround&) = delete;

	// Zones (4,608 units square) the area covers; 0 when it is not on the map.
	double zones(std::uint32_t colour) const;
	// A uniform dry non-cliff point of the area (48 tries); an all-water area
	// keeps its last sample. The area must exist (zones(colour) > 0).
	state::GroundPoint pick(std::uint32_t colour, RngStream& rng) const;
	// The area (BIOME_GROUP `index` colour) a world point lies in.
	std::uint32_t colourAt(double x, double z) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// The areas map as a point lookup: which BIOME_GROUP (`index` colour) a world
// point lies in. Used by the faction layer for `biomes` / `no-go zones`.
class AreaLookup
{
public:
	explicit AreaLookup(std::string_view dataDir);
	~AreaLookup();
	AreaLookup(const AreaLookup&) = delete;
	AreaLookup& operator=(const AreaLookup&) = delete;

	std::uint32_t colourAt(double x, double z) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// Gives every SquadInstance with no home town and no Position a Position and a
// den. dataDir is the game's data directory (areasmap, biomemap, heightmap).
PlacementReport placeWilderness(flecs::world& ecs, const kdata::GameData& gameData,
	std::string_view dataDir, std::uint64_t seed);

} // namespace ksim
