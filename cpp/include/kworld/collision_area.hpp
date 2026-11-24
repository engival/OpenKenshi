#pragma once

// Collision geometry of a rectangle of zones, gathered from the game data: the shapes the
// navmesh bakes from (knav) and the client's click ray tests against. Moved from knav/area.

#include "kworld/collision.hpp"
#include "kworld/coords.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kdata {
class GameData;
}

namespace kworld {

class BiomeMap;
class Features;
class Heightmap;
class InteriorLayouts;
class Towns;

// The game data an AreaGatherer loads once, for the consumers beside it (the navtile hash recipe, navhash_source.hpp).
struct AreaSources
{
	const kdata::GameData& gameData;
	const InteriorLayouts& layouts;
	const Heightmap& terrain;
	const BiomeMap& biomes;
	const Features& features;
	const Towns& towns;
};

struct AreaBuilding
{
	std::string id; // "cellX,cellZ/instanceID"
	PlacedInstance placed;
	BuildingNav nav;
	std::shared_ptr<const std::vector<WorldShape>> shapes; // empty for IGNORE
};

struct AreaGeometry
{
	std::vector<AreaBuilding> buildings; // placement order
	std::shared_ptr<const std::vector<WorldShape>> foliage;
	CollisionCoverage coverage;

	// Every shape, buildings in id order then foliage: the order NavWorld rebuilds in.
	std::vector<WorldShape> flatShapes() const;
};

// A digest of everything a bake reads from the gathered geometry: per building its id, pose (the rotation included), nav fields
// and every shape's vertices, indices, use and frame, then the foliage shapes. A mod that swaps a collision mesh, rotates a
// building or changes a layout's furniture moves it (the nav cache's zone key folds it, kenshi-k16.7.67).
std::uint64_t areaDigest(const AreaGeometry& area);

// Loads the game data once and gathers any number of zone rectangles from it
// (a world bake gathers one per region). Not thread-safe.
class AreaGatherer
{
public:
	AreaGatherer(std::string_view dataDir, const Heightmap& terrain);
	// As above, but reading the caller's game data (the sim's, which it loaded anyway) instead of loading another copy. The
	// caller's GameData must outlive the gatherer.
	AreaGatherer(std::string_view dataDir, const Heightmap& terrain, const kdata::GameData& gameData);
	~AreaGatherer();

	// Every building placed in, and foliage generated for, the zone cells
	// min..max inclusive. Buildings and foliage one cell outside are added
	// too, so shapes overhanging the edge are seen and a tile on the edge
	// bakes the same as it would inside a bigger rectangle.
	AreaGeometry gather(ZoneCell min, ZoneCell max);

	// A placement the zone files do not carry (a save's own building, a player construction): later gathers of its zone see it.
	void addPlacement(const PlacedInstance& placed);

	// What the gatherer loaded; the references live as long as it does (terrain is the caller's).
	AreaSources sources() const;

private:
	struct State;
	std::unique_ptr<State> state_;
};

// One-shot gather for a single rectangle.
AreaGeometry gatherArea(std::string_view dataDir, const Heightmap& terrain,
	ZoneCell min, ZoneCell max);

} // namespace kworld
