#pragma once

// Seed points for region pruning (NavMesh::prune): the original keeps only the parts of its navmesh that a seed reaches,
// which is why it has no roofs and no sealed buildings. Its sources, from the kinspect session's findings/navseed_static.md
// and navbake_static.md ("Seed points"): per zone either the manual seeds of navtiles/seeds.def or a 3 x 3 grid of ground
// probes, each building's door outside point, and for an interior the building's origin. Road markers (the original's global
// list) are not read: roads lie on probed ground. A modded building needs nothing of its own: its door and origin seed it.

#include "knav/navmesh.hpp"
#include "kworld/collision_area.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/zones.hpp"

#include <string>
#include <vector>

namespace knav {

// The original's minDistanceToSeedPoints is 4 u. The height reach is ours (UNSURE): a probe or origin sits on the physics
// surface, our polygon's detail surface can be a few units off it.
inline constexpr Point kSeedReach{ 4.0f, 8.0f, 4.0f };

// navtiles/seeds.def: x, y, z little-endian floats, world coordinates. Empty when the file is absent.
std::vector<Point> readSeedsDef(const std::string& navtilesDir);

// The ground seeds of the zone cells min..max. A zone with points in `seedsDef` uses those (a y of 0 or below means "drop to
// the ground": the terrain height here; UNSURE, the original casts down with mask 0x600 and skips a positive-y point that
// hits). Any other zone gets the 3 x 3 grid: 1 u in from its edges, the midpoints and the centre, at the terrain height or
// the sea level, whichever is higher; a probe with a building over it is rejected (the original's down-ray hits a shape that
// is not ground; ours tests the building shapes' boxes).
std::vector<Point> zoneSeeds(const kworld::Heightmap& terrain, const kworld::AreaGeometry& area, const std::vector<Point>& seedsDef,
	kworld::ZoneCell min, kworld::ZoneCell max, float seaLevel);

// Each walkable building's origin: its interior's seed when it has no seed object of its own.
std::vector<Point> buildingSeeds(const kworld::AreaGeometry& area);

// A building's door seed: its first door (the original takes the building's first child when that child is a door; UNSURE
// that our first door is it) and the building origin, which says which side is outside.
struct DoorSeed
{
	DoorKey door = 0;
	double originX = 0.0;
	double originZ = 0.0;
};
std::vector<DoorSeed> doorSeedsOf(const kworld::AreaGeometry& area);

// Each door's outside point: 15 u from the doorway's centre along its normal (findings/item21b_static.md), on the side away
// from the building's origin (UNSURE: the original takes the panel's normal sign), skipped below `seaLevel`.
std::vector<Point> doorSeeds(const NavMesh& mesh, const std::vector<DoorSeed>& doors, float seaLevel);

} // namespace knav
