#pragma once

// Zone files: data/newland/leveldata/**/zone.X.Y.zone
// (docs/formats/WORLD_LAYOUT.md).

#include "kworld/coords.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kdata {
class GameData;
}

namespace kworld {

class Heightmap;

// WORLD_LAYOUT.md "Instances": an empty target is a tombstone, a non-empty
// target with an all-zero x/y/z is an inventory row, anything else is a
// building placement. tools/world_survey.py classifies in this order.
enum class ZoneInstanceKind
{
	Deleted,
	InventoryRow,
	Placement,
};

struct ZoneInstance
{
	std::string instanceID;
	std::string targetID;
	std::int32_t recordType = 0;
	std::string collectionID;

	double x = 0.0;
	double y = 0.0; // terrain-relative for placements
	double z = 0.0;
	double qw = 1.0;
	double qx = 0.0;
	double qy = 0.0;
	double qz = 0.0;
	std::vector<std::string> states;
	// The `owner faction ID` of the instance's local GAMESTATE_BUILDING (the record its `states` names, in the same zone
	// file); "" = none (GATES_CARRY_STATIC.md b: the building then takes its town's faction).
	std::string ownerFaction;
	// The same record's `squad template`, `interior layout name` and `name` (the placement's label, "Bar"); "" = none. The squad template is the
	// resident template the town gave this building (TOWN_LIFE_SLICE 5.17: the Hub's Bar is the placement whose squad template is Bar blackmarket).
	std::string squadTemplate;
	std::string interiorLayout;
	std::string label;
	// The same record's town (`townTYPE` 13, `townC` / `townCS`: the hand of the town whose populate owns this building; TownPlacement::hand).
	// 0 = none.
	std::uint64_t townHand = 0;
	// The same record's construction state (a player building's, kenshi-k16.7.156): `is complete`, `destroyed`, `broken`, `pause`, `power on`
	// (absent = complete, whole, on) and `construction progress` (0..1, read only while not complete).
	bool complete = true;
	bool destroyed = false;
	bool broken = false;
	bool paused = false;
	bool powerOn = true;
	double progress = 1.0;

	ZoneInstanceKind kind = ZoneInstanceKind::Deleted;
};

struct ZoneTypeCount
{
	std::string type;
	std::size_t count = 0;
};

struct ZoneFile
{
	std::string path;
	std::string layer; // "." for the base leveldata directory
	int cellX = 0;
	int cellZ = 0;
	int version = 0;

	std::size_t recordCount = 0;
	std::vector<ZoneTypeCount> typeCounts; // first-appearance order
	std::vector<ZoneInstance> instances;

	std::size_t placementCount = 0;
	std::size_t deletionCount = 0;
	std::size_t inventoryRowCount = 0;

	// v15 trailing block: i32 n + n ascending unique i32.
	// UNSURE: WORLD_LAYOUT.md says the meaning is unknown; the bytes are kept.
	std::vector<std::byte> trailingBytes;
	std::vector<std::int32_t> trailingValues;

	bool hasTrailingBlock() const noexcept { return !trailingBytes.empty(); }
};

// Parses one zone container. `filename` supplies the zone cell and the
// pre-v7 stringID suffix. Validation of the trailing block throws Error;
// container errors propagate as kdata::ParseError.
ZoneFile parseZoneFile(std::span<const std::byte> bytes,
	std::string_view filename, std::string_view layer);

// The sea plane: a placed building's ground is never below it (zoneBuildingY).
inline constexpr double kSeaPlane = 100.0;

// The one placement rule for a zone building's absolute y (COLLISION_FORMAT.md section 4, corrected by the original's
// Building::createPhysical): nodeY (the instance's y) + max(kSeaPlane, ground(x, z)). A door building keeps the terrain
// height (the original's rule is for a non-door placement). Every placement here goes through it: makePlaced, so place,
// placeMerged and everything built on them.
double zoneBuildingY(const Heightmap& terrain, double x, double z, double nodeY, bool doorBuilding = false);

// One placed building. y is absolute: zoneBuildingY of the instance y.
struct PlacedInstance
{
	std::string instanceID;
	std::string buildingID;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double relativeY = 0.0;
	double qw = 1.0;
	double qx = 0.0;
	double qy = 0.0;
	double qz = 0.0;

	// BUILDING float `scale` from the merged records (COLLISION_FORMAT.md
	// section 3.1). hasScale is false for the 14 placements whose target is
	// missing.
	float scale = 1.0f;
	bool hasScale = false;

	ZoneCell cell; // cell named by the zone file
	ZoneCell positionCell; // cell computed from x/z
	std::string layer;
	std::string zonePath;
	std::string ownerFaction; // ZoneInstance::ownerFaction of the surviving instance
	std::string squadTemplate; // ... its squadTemplate, interiorLayout and label
	std::string interiorLayout;
	std::string label;
	std::uint64_t townHand = 0;
};

// Load-order rank of a data layer (the ZoneFile::layer directory name):
// "." (gamedata.base) 0 < "Newwworld" 1 < "Dialogue" 2 < "rebirth" 3.
// Later ranks override earlier ones. Throws Error for any other name.
int zoneLayerRank(const std::string& layer);

// What load-order layering did to the "0-buildinglist" instances. Keys are
// (cell of the zone file, instance id).
struct LayerStats
{
	std::size_t entries = 0; // instances seen, all layers
	std::size_t overrides = 0; // replaced an entry from an earlier layer
	std::size_t deletions = 0; // empty target that removed an earlier entry
	std::size_t orphanDeletions = 0; // empty target with nothing to remove
	std::size_t survivors = 0; // keys left alive, any kind
	std::size_t placements = 0; // survivors classed as placements
};

// One surviving "0-buildinglist" instance; points into Zones::files().
struct ResolvedInstance
{
	const ZoneFile* file = nullptr;
	const ZoneInstance* instance = nullptr;
};

class Zones
{
public:
	// Builds a Zones from parsed files (tests). Layers must be among
	// ".", "Newwworld", "Dialogue", "rebirth".
	static Zones fromFiles(std::vector<ZoneFile> files);

	// Loads every *.zone below dataDir/newland/leveldata, sorted by path.
	static Zones load(std::string_view dataDir);

	const std::vector<ZoneFile>& files() const noexcept { return files_; }
	const std::string& levelDataRoot() const noexcept { return levelDataRoot_; }

	std::size_t placementCount() const noexcept { return placementCount_; }
	std::size_t deletionCount() const noexcept { return deletionCount_; }
	std::size_t inventoryRowCount() const noexcept { return inventoryRowCount_; }

	// Instances of INSTANCE_COLLECTION "0-buildinglist" classified as
	// placements, in file order.
	std::vector<PlacedInstance> place(const Heightmap& terrain,
		const kdata::GameData& gameData) const;

	// Load-order stacking of "0-buildinglist" instances: layers apply in the
	// order gamedata.base (".") < Newwworld < Dialogue < rebirth. A later
	// entry with the same (cell, instance id) replaces the earlier one, in
	// the earlier one's position; an entry with an empty target deletes it.
	// Throws Error for a layer outside that list.
	std::vector<ResolvedInstance> resolveLayers(LayerStats* stats = nullptr) const;

	// Like place(), but over resolveLayers(): only the surviving entries,
	// in first-appearance order of their key.
	std::vector<PlacedInstance> placeMerged(const Heightmap& terrain,
		const kdata::GameData& gameData, LayerStats* stats = nullptr) const;

private:
	std::vector<ZoneFile> files_;
	std::string levelDataRoot_;
	std::size_t placementCount_ = 0;
	std::size_t deletionCount_ = 0;
	std::size_t inventoryRowCount_ = 0;
};

} // namespace kworld
