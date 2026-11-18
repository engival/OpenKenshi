// The traveller a route is planned for: the footprint radius (docs/formats/NAVMESH_CACHE.md "Clearance") and the race's water cost scale.
#pragma once

#include <optional>

namespace ksim
{

struct Body
{
	// Clearance from the mesh's walls (CombatData::pathRadius); 0 = none.
	double radius = 0.0;
	// RACE water cost scale = 1 + `water avoidance` (human 7.0, Fishman 0.25; PATHFINDING_STATIC.md item 4), applied to every water
	// polygon. Empty = no race (a travel estimate, a test): the filter's own default water costs.
	std::optional<float> water;

	Body(double r = 0.0, std::optional<float> w = std::nullopt) noexcept : radius(r), water(w) {}
};

} // namespace ksim
