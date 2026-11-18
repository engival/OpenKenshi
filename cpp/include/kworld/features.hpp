#pragma once

// Hand-placed world features: data/newland/land/features.dat (MF01,
// docs/formats/TERRAIN_FORMAT.md) and INSTANCE_COLLECTION "5-roadfoliage"
// (docs/formats/WORLD_LAYOUT.md). Placements name a MAP_FEATURES record
// (mesh + material; docs/reference/BUILDING_MATERIALS.md); FOLIAGE_MESH is
// resolved too, with its `leaves mesh` as a second mesh.

#include "kworld/coords.hpp"
#include "kworld/roadoverlay.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kdata {
class GameData;
struct File;
}

namespace kworld {

// One MF01 record. y is absolute (not terrain-relative): features sit at
// their own heights, big formations are offset from the ground by their mesh
// origin. Rotation order is w, x, y, z as in zone instances; a non-finite
// quaternion occurs once in the shipped file and is kept as read
// (rotationFinite() is false).
struct FeaturePlacement
{
	std::string recordID;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	float scaleX = 1.0f; // usually uniform; can be non-uniform or negative
	float scaleY = 1.0f;
	float scaleZ = 1.0f;
	double qw = 1.0;
	double qx = 0.0;
	double qy = 0.0;
	double qz = 0.0;

	ZoneCell cell; // zone slot holding the record
	ZoneCell positionCell; // cell computed from x/z (5 shipped outliers differ)
	std::string layer; // "." = the base file newland/land/features.dat
	int record = -1; // index into Features::records(), -1 when unresolved

	bool rotationFinite() const noexcept;
};

struct FeatureFile
{
	std::size_t zoneCount = 0; // non-empty zones
	std::vector<FeaturePlacement> placements; // zone order, then file order
};

// Parses one MF01 container. Truncation, a wrong magic or zone count, a blob
// outside the file or not consumed exactly, and trailing bytes throw Error.
FeatureFile parseFeatureFile(std::span<const std::byte> bytes,
	std::string_view layer);

// INSTANCE_COLLECTION "5-roadfoliage": points (x, y, z, u, 0, 0, 0) with
// target "-". y is absolute; u is the transform's 4th float (meaning unknown).
// The point names no record, so it has no mesh.
struct RoadFoliagePoint
{
	std::string instanceID;
	std::string targetID;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double u = 0.0;
	std::string layer;
	bool deleted = false; // empty target: tombstone
};

// Instances of every "5-roadfoliage" record in one file, in file order.
std::vector<RoadFoliagePoint> collectRoadFoliage(const kdata::File& file,
	std::string_view layer);

// Overlay by instance id: later layers replace earlier ones, a tombstone
// removes the id. `layers` is in load order (lowest first). Result keeps the
// first-seen order of the surviving ids.
std::vector<RoadFoliagePoint> overlayRoadFoliage(
	std::span<const std::vector<RoadFoliagePoint>> layers);

// Reads dataDir/newland/leveldata.level (layer ".") and every
// leveldata/<layer>/leveldata.level in the order Newwworld, Dialogue,
// rebirth, other names after that (sorted), and overlays them.
std::vector<RoadFoliagePoint> loadRoadFoliage(std::string_view dataDir);

// MapFeatureMode (fcs.def). MAP_FEATURES `texture mode` and FOLIAGE_MESH
// `material type`.
enum class FeatureMode : std::int32_t
{
	UvMapped = 0,
	Triplanar = 1,
	Terrain = 2,
	DualTexture = 3,
	Foliage = 4,
	DualTriplanar = 5,
	Emissive = 6,
};

// One drawable mesh of a record. Paths are record paths (".\data\...",
// backslashes), "" when the field is empty.
struct FeatureMesh
{
	std::string mesh;
	std::string texture;
	std::string normalMap;
	std::string texture2;
	std::string normalMap2;
	float tileX = 1.0f;
	float tileY = 1.0f;
	float specularMult = 0.0f;
	FeatureMode mode = FeatureMode::UvMapped;
	int alphaThreshold = 0; // 0-255; 0 = no alpha test
	bool leaves = false; // FOLIAGE_MESH `leaves mesh`: 2-sided, alpha tested
};

struct FeatureRecord
{
	std::string recordID;
	std::string recordType; // "" when the record does not exist
	bool found = false;
	bool hidden = false; // MAP_FEATURES `hidden`: marker, nothing drawn
	bool localCoordinates = false; // triplanar/terrain mapping in object space
	float distance = 2000.0f; // MAP_FEATURES visibility distance
	std::vector<FeatureMesh> meshes; // empty when unresolved or hidden
};

// Resolves MAP_FEATURES and FOLIAGE_MESH records; any other type (or a
// missing record) yields no meshes.
FeatureRecord resolveFeatureRecord(const kdata::GameData& gameData,
	std::string_view recordID);

class Features
{
public:
	// Loads the merged game data, features.dat and the road foliage.
	static Features load(std::string_view dataDir);
	static Features join(const kdata::GameData& gameData, std::string_view dataDir);

	const std::vector<FeaturePlacement>& placements() const noexcept { return placements_; }
	// One entry per distinct recordID, first-use order.
	const std::vector<FeatureRecord>& records() const noexcept { return records_; }
	const std::vector<RoadFoliagePoint>& roadFoliage() const noexcept { return roadFoliage_; }
	std::size_t zoneCount() const noexcept { return zoneCount_; }
	// The overlay tiles' road alpha (foliage road avoidance); never null.
	const RoadOverlay& roadOverlay() const noexcept { return *roadOverlay_; }

	std::size_t unresolvedPlacements() const noexcept; // no record or no mesh

private:
	std::vector<FeaturePlacement> placements_;
	std::vector<FeatureRecord> records_;
	std::vector<RoadFoliagePoint> roadFoliage_;
	std::size_t zoneCount_ = 0;
	std::shared_ptr<const RoadOverlay> roadOverlay_ = std::make_shared<const RoadOverlay>(std::string());
};

} // namespace kworld
