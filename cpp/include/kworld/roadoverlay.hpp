#pragma once

// Alpha channel of the overlay tiles data/newland/land/overlaymaps/
// new_overlay.<tx>.<tz>.png, used by the tree scatter's road-avoidance test
// (navhash.md section 12, FOLIAGE_EXE.md section 4; DECODED + VERIFIED-live).
//
// The game holds one 1024^2 RGBA image per 8x8-zone region, placed so that
// its texel centres sit on the region's 36-unit lattice: origin = region min
// - 18 units, span = 36864 + 36 = 36900, i.e. 36.03515625 units per texel.
// Column = world x, row = world z, row-major, no flip. The road test reads
// the alpha byte of the box perimeter and centre only (roadRejected). Tiles
// load lazily; safe to query from several threads.

#include "kworld/coords.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace kworld {

class RoadOverlay
{
public:
	static constexpr double kRegionSize = 36864.0; // 8 zones of 4608 units
	static constexpr int kTilePixels = 1024;
	static constexpr int kTilesPerAxis = 8;
	// 36900 / 1024: the game's texel pitch (navhash.md section 12c).
	static constexpr double kPixelSize = (kRegionSize + 36.0) / kTilePixels;
	// The image starts half a texel before its region's min corner.
	static constexpr double kOriginMargin = -18.0;

	explicit RoadOverlay(std::string dataDir) : dataDir_(std::move(dataDir)) {}

	// Absolute texel index of a world coordinate along one axis
	// (region * 1024 + texel), clamped to the world image grid.
	long texel(double world) const;

	// Alpha at an absolute texel index; 0 outside the world or where the
	// tile file is absent.
	std::uint8_t alpha(long texelX, long texelZ) const;

	// The PNG's R byte at an absolute texel: the baked grass density (memory byte 2 of the exe's image; the foliage-layer
	// scatter's grass test reads it, FOLIAGE_FORMAT-style notes in FOLIAGE_EXE.md section 4). 0 outside the world or where
	// the tile is absent.
	std::uint8_t grassDensity(long texelX, long texelZ) const;

	// The world coordinate of an absolute texel's centre along one axis (the inverse of texel()).
	static double texelCentre(long texel);

	// The road-avoidance test (1406d82d0): the box of half-width `radius`
	// world units around (x, z) is rejected when any texel on its perimeter
	// (min-z row, max-z row, left/right edges) or at its centre has non-zero
	// alpha. Interior texels other than the centre are never read. The game
	// holds one image per region and clamps the box corners into it, so both
	// corners on an axis are taken in the region that holds the point, never
	// the neighbouring region's image.
	bool roadRejected(double x, double z, double radius) const;

private:
	// Region holding a world coordinate, -1 off the world.
	long region(double world) const;
	// Absolute texel of `world` inside `region`'s image (corners clamped).
	long texelIn(long region, double world) const;
	const std::vector<std::uint8_t>& tile(int tx, int tz) const;

	std::string dataDir_;
	mutable std::array<std::once_flag, kTilesPerAxis * kTilesPerAxis> loaded_;
	mutable std::array<std::vector<std::uint8_t>, kTilesPerAxis * kTilesPerAxis> alpha_;
	mutable std::array<std::vector<std::uint8_t>, kTilesPerAxis * kTilesPerAxis> red_;
};

} // namespace kworld
