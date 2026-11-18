#pragma once

// World coordinate helpers. The constants and formulas copy the coordinate
// table in docs/ARCHITECTURE.md and the zone mapping in
// docs/formats/WORLD_LAYOUT.md ("Zone files - zone.X.Y.zone").

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace kworld {

inline constexpr double kWorldMin = -147456.0;
inline constexpr double kWorldExtent = 294912.0;
inline constexpr double kZoneSize = 4608.0;
inline constexpr int kZoneCellsPerAxis = 64;
inline constexpr int kZoneCount = kZoneCellsPerAxis * kZoneCellsPerAxis;

// Zone.X.Y: x is the x cell, z is the z cell (the doc calls it Y).
struct ZoneCell
{
	int x = 0;
	int z = 0;

	friend bool operator==(const ZoneCell&, const ZoneCell&) = default;
};

// Total for any input: NaN, inf and huge values land on an out-of-world cell
// instead of an undefined float-to-int cast.
inline int zoneAxisOf(double v)
{
	const double cell = std::floor((v - kWorldMin) / kZoneSize);
	if (!(cell > -1e6 && cell < 1e6))
		return cell > 0 ? 1000000 : -1000000;
	return static_cast<int>(cell);
}

inline ZoneCell zoneCellOf(double x, double z)
{
	return { zoneAxisOf(x), zoneAxisOf(z) };
}

// ARCHITECTURE.md: zone index = X + 64*Y.
inline int zoneIndexOf(const ZoneCell& cell)
{
	return cell.x + kZoneCellsPerAxis * cell.z;
}

inline ZoneCell zoneCellOfIndex(int index)
{
	return { index % kZoneCellsPerAxis, index / kZoneCellsPerAxis };
}

inline double zoneMinX(int cellX)
{
	return kWorldMin + kZoneSize * cellX;
}

inline double zoneMinZ(int cellZ)
{
	return kWorldMin + kZoneSize * cellZ;
}

// A saved hand (the exe's object handle: TYPE, I, C, CS, S ints of a GAMESTATE record) reduced to the pair that names a town: C and the serial
// CS. A GAMESTATE_TOWN stores its own as `handC` / `handCS`; a GAMESTATE_BUILDING names its town as `townC` / `townCS` with `townTYPE` 13.
inline constexpr std::int32_t kTownHandType = 13;

inline constexpr std::uint64_t townHand(std::int32_t c, std::int32_t cs) noexcept
{
	return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c)) << 32) | static_cast<std::uint32_t>(cs);
}

} // namespace kworld
