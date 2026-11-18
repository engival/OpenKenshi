#pragma once

// Procedural foliage (docs/reference/FOLIAGE_EXE.md, "Implementation").
// Reproduces the exe's per-zone scatter of FOLIAGE_MESH records: each zone
// runs the FOLIAGE_LAYERs of the biomes it contains, every layer re-seeds a
// private MT19937 from (zone origin, layer record id), and every mesh gets
// (int)val0 placement attempts. generateFoliage() is call-local: no shared
// mutable state, so any number of threads may call it at once.

#include "kworld/coords.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kdata {
class GameData;
}

namespace kworld {

class Features;
class Heightmap;
class Towns;

// MT19937 with the exe's non-standard tempering masks (FOLIAGE_EXE.md
// section 2). One object per (zone, layer); never shared.
class FoliageRng
{
public:
	explicit FoliageRng(std::uint32_t seed);

	// Next tempered 32-bit output.
	std::uint32_t next();
	// a + y * 2^-32 * (b - a), evaluated as the exe does (float inputs,
	// double arithmetic, float result): half-open [a, b).
	float uniform(float a, float b);
	// Integer in 0..max: draws masked by the smeared max until <= max.
	std::uint32_t uniformInt(std::uint32_t max);

private:
	void twist();

	std::array<std::uint32_t, 624> state_{};
	std::size_t index_ = 0;
};

// Seed of one layer in one zone: the layer's leading record id minus the
// zone-origin hash term. (zoneMinX, zoneMinZ) are the truncated world
// coordinates of the zone's min corner.
std::uint32_t foliageLayerSeed(int zoneMinX, int zoneMinZ, int layerRecordID);

// Leading integer of a record string id ("1452-vurt.mod" -> 1452), 0 when it
// has none (Ogre::StringConverter::parseInt).
int leadingRecordID(std::string_view stringID);

// data/newland/land/biomemap.png (TERRAIN_FORMAT.md): 1024^2, 288 units per
// pixel, column = x, row = z, pixel 0xRRGGBB == BIOMES `index`.
class BiomeMap
{
public:
	static BiomeMap load(const std::string& pngPath);
	// 0xRRGGBB per pixel, row-major; size must be width * height.
	static BiomeMap fromPixels(int width, int height, std::vector<std::uint32_t> rgb);

	int width() const noexcept { return width_; }
	int height() const noexcept { return height_; }
	std::uint32_t pixel(int col, int row) const;
	// Biome index under a world point; coordinates clamp to the map edge.
	std::uint32_t biomeAt(double x, double z) const;
	double unitsPerPixel() const noexcept { return kWorldExtent / width_; }

private:
	int width_ = 0;
	int height_ = 0;
	std::vector<std::uint32_t> rgb_;
};

struct FoliageInstance
{
	std::string recordID; // FOLIAGE_MESH stringID
	double x = 0.0;
	double y = 0.0; // absolute world height
	double z = 0.0;
	// Orientation, w first: yaw about +Y, then aligned to the terrain normal
	// for `slope align` meshes, or a random rotation for meshes that are not
	// `keep upright` (see ZoneScatter::orientation). x, z, yawDegrees and
	// scale are the loader's quantised values (u16 page offsets, yaw byte
	// * 360/255, scale byte), as the game renders them.
	double qw = 1.0;
	double qx = 0.0;
	double qy = 0.0;
	double qz = 0.0;
	float yawDegrees = 0.0f;
	float scale = 1.0f; // uniform
	bool isChild = false; // spawned from a parent mesh's `meshes` list
	// This instance enters the navtile vegetation fold (kworld/navhash.hpp): the FOLIAGE_MESH has a `navmesh
	// cutter` or a collision file. Grass layers never spawn here, so every instance here is a tree-loader one.
	bool navFold = false;
	// The FOLIAGE_MESH is `limit to grass areas`: the layer spawner draws the position through the Forest's
	// grass mask (FOLIAGE_EXE.md section 4, NAVHASH.md). Set on the instance for the tools' diagnostics.
	bool grassLimited = false;
};

struct FoliageParams
{
	// The exe clamps `floating` meshes to the water node's height.
	// UNVERIFIED: the constant is a runtime scene node; CLIENT.md puts sea
	// level near 100.
	float waterHeight = 100.0f;
	// rebirth.mod writes `no-foliage range` 0.0 over 30 towns' 2000.0, The
	// Hub included; the exe reads that 0 literally, so towns then clear
	// nothing. True restores the 2000.0 default for a 0 range.
	bool zeroTownRangeIsDefault = false;
	// Instance x and z are the decoded u16 page offsets the game renders with (the live check above), which differ
	// from the scatter point by up to a page's 65535th (0.035 units). True keeps the raw scatter position instead: the
	// navtile hash folds the raw position (kworld/navhash.hpp).
	bool unquantisedPositions = false;
};

// All instances of one zone. Order is deterministic: biomes in first-seen
// order over the zone's biome-map window, layers in the biome's `foliage`
// order, meshes in the layer's `meshes` order, then draw order. Eraser discs
// (Features::roadFoliage, `dx^2 + dz^2 < r^2`) and the town no-foliage range
// suppress an instance without changing the random stream. Returns {} for a
// cell outside the world.
// The exe also drops parents whose `road avoidance` square touches a non-zero
// alpha pixel of the overlay tiles (Features::roadOverlay; the alpha is baked
// into the PNGs).
// `limit to grass areas` (16 of 748 meshes): the layer scatter re-rolls a candidate (up to 20 times) until the overlay's R
// (the PNG's baked grass density) + G (the grass spots the zone's earlier placements wrote) exceeds 149
// (FOLIAGE_EXE.md section 4, 1406d3740:143-163). Rejected rolls consume stream draws.
std::vector<FoliageInstance> generateFoliage(ZoneCell cell,
	const Heightmap& terrain, const BiomeMap& biomes,
	const kdata::GameData& gameData, const Features& features,
	const Towns& towns, const FoliageParams& params = {});

// Whether the zone runs any tree FOLIAGE_LAYER (a non-grass layer of one of its biomes), whatever the scatter survives:
// the zone hash folds the forest word whenever the zone has one, even a 0 (NAVHASH.md "What reproduces"; the game's
// Forest node exists per tree layer, kinspect findings/navhash.md section 11).
bool zoneHasTreeLayer(ZoneCell cell, const BiomeMap& biomes, const kdata::GameData& gameData);

} // namespace kworld
