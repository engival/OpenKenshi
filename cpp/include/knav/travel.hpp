#pragma once

// Walkable-surface area types and the per-traveller costs over them
// (docs/formats/NAVMESH_CACHE.md "Water"). The bake tags every polygon with one
// Area; a TravelFilter says which areas a traveller may enter and what a unit
// of distance costs there, so a swimmer, a wader and a dry-only walker share one mesh.

#include <array>
#include <cstdint>

namespace knav {

// Detour polygon area id. The surface is judged by its depth under the water plane.
enum class Area : std::uint8_t
{
	Land, // dry, or water shallower than NavConfig::wadeDepth
	Shallow, // wading: wadeDepth <= depth < swimDepth
	Swim, // depth >= swimDepth
	// A doorway or gateway (docs/formats/NAVMESH_CACHE.md "Doors"). The bake emits DoorOpen; the
	// runtime (NavMesh::applyDoors) repaints a door's polygons DoorClosed while it is closed.
	DoorOpen,
	DoorClosed,
	// Dry land within NavConfig::agentRadius of a wall. The original bakes no erosion and keeps a body width off walls at
	// query time (kinspect findings/navbake_static.md); we keep the band in the mesh as its own polygons and charge for it, so
	// routes stay off walls where there is room while the whole surface is there to stand on and to click.
	Edge,
};

inline constexpr int kAreaCount = 6;

// Polygon flag above the area bits: set = the door is locked (a wall for every traveller). The original has no class, faction or
// owner test on a door (findings/rwh199_nav_validity_static.md section 2): only the door's own state refuses.
inline constexpr unsigned kDoorLockedFlag = 1u << 8;

// The polygon flag of an area (one bit each); a filter includes the flags it allows.
constexpr std::uint16_t areaFlag(Area area) noexcept
{
	return static_cast<std::uint16_t>(1u << static_cast<unsigned>(area));
}

// UNSURE: game data has only `swim speed mult` per RACE (Human 1.0, Fishman 5.0, Shek 0.5), no
// absolute swim or wade speed. These are guesses at time cost per unit of distance relative to
// walking: wading is a little slower, swimming about a third of walking speed.
inline constexpr float kShallowCost = 1.5f;
inline constexpr float kSwimCost = 3.0f;
// A door costs the same closed or open to the search: the opening is a timed step of the walker (ksim interact.hpp), not an area
// cost (DOORS.md section 6). Crossing a door adds a flat +5 on the edge that enters a door polygon (PATHFINDING_STATIC.md 2.3 / 6.3,
// rwh.144: the water cost modifier at 1403b57e0 adds 5.0 for face data 4), so a search prefers an open way round. It is in world
// units of distance, as ours are. TravelQueryFilter (query_filter.hpp) adds it; the door polygon itself costs 1.0 per unit.
inline constexpr float kDoorCrossingCost = 5.0f;
inline constexpr float kDoorClosedCost = 1.0f;
inline constexpr float kDoorOpenCost = kDoorClosedCost;
// UNSURE (ours): high enough that a route keeps to the open middle, low enough that a gap narrower than a body width (all
// Edge) still beats a long way round. The original blocks gaps under 9 u outright (minCharacterWidth, BLOCK_EDGES).
inline constexpr float kEdgeCost = 2.0f;

struct TravelFilter
{
	std::array<float, kAreaCount> cost{ 1.0f, kShallowCost, kSwimCost, kDoorOpenCost, kDoorClosedCost, kEdgeCost }; // per Area; >= 0
	// The flat add on the edge that enters a door polygon (kDoorCrossingCost; 0 prices a door as plain walking).
	float doorCrossing = kDoorCrossingCost;
	// Door areas are always allowed here: a door is refused per class by the door state, not by this mask.
	std::uint16_t allowed = areaFlag(Area::Land) | areaFlag(Area::Shallow) | areaFlag(Area::Swim)
		| areaFlag(Area::DoorOpen) | areaFlag(Area::DoorClosed) | areaFlag(Area::Edge);
	// True: locked doors stay in the search (a bash, or a lookup that ignores door state).
	bool throughLocks = false;
	// The traveller's footprint radius (race pathfind footprint radius x size factor, docs/formats/NAVMESH_CACHE.md
	// "Clearance"). 0 = none: the mesh was eroded at bake time. Above 0 the path keeps this far from the mesh's walls.
	float radius = 0.0f;
	// A short route (radius > 0, ends under 300 u apart) places its search nodes on the way and may replace a roundabout
	// corridor (docs/formats/NAVMESH_CACHE.md "Node placement"). Off: Detour's portal middles, as every route did.
	bool straighten = false;

	// The race's water cost scale (PATHFINDING_STATIC.md item 4; rwh144 `RaceData+0x6c` = 1 + water avoidance, read by the cost modifier
	// 1403b57e0 on face data 3 = water): every water polygon costs `scale` per unit, shallow and swimmable alike, as the original has one
	// water face. Human 7.0, Fishman 0.25.
	void setWaterCost(float scale) noexcept
	{
		cost[static_cast<std::size_t>(Area::Shallow)] = scale;
		cost[static_cast<std::size_t>(Area::Swim)] = scale;
	}
	// A swimmer; `swimSpeedMult` is the RACE `swim speed mult` (cost of swimming is divided by it).
	static TravelFilter swimmer(float swimSpeedMult = 1.0f) noexcept
	{
		TravelFilter filter;
		filter.cost[static_cast<std::size_t>(Area::Swim)] = kSwimCost / swimSpeedMult;
		return filter;
	}
	// Cannot swim: never enters a Swim polygon, so islands behind deep water are unreachable.
	static TravelFilter dryOnly() noexcept
	{
		TravelFilter filter;
		filter.allowed = areaFlag(Area::Land) | areaFlag(Area::Shallow) | areaFlag(Area::DoorOpen)
			| areaFlag(Area::DoorClosed) | areaFlag(Area::Edge);
		return filter;
	}

	bool allows(Area area) const noexcept { return (allowed & areaFlag(area)) != 0; }
	// Smallest cost over the allowed areas: scales a straight-line A* heuristic so it stays admissible.
	float minCost() const noexcept
	{
		float least = 1e30f;
		for (int i = 0; i < kAreaCount; ++i)
			if (allows(static_cast<Area>(i)))
				least = cost[static_cast<std::size_t>(i)] < least ? cost[static_cast<std::size_t>(i)] : least;
		return least;
	}

	bool operator==(const TravelFilter&) const = default;
};

} // namespace knav
