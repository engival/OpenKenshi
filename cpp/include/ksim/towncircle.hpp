// The exe's current-town test, one geometry for every consumer (task town-circle, extended by task patrol): the Town of the circles
// that contains the point. `Character vf+0x78` is getTown (1405c89b0 / GOG 1405c8350, reading CharStatus+0x88) - kinspect dt24_34,
// dt24_36 and the ai_structs.h correction (it is getTown, not getBuilding) - and the same call sits behind goal 34's
// `getTown(t) == myTown`, the crime law test's "has no town" (`FUN_140598fa0`, `vf+0x78 == 0`), the dialogue's
// IN_A_NON_PLAYER_TOWN (state block +0x88) and the patrol goal 216.
//
// The radius is the exe's Town radius (vf+0x2a0): buildings::townRadius, the base by TOWN `type` x `town radius mult` (kinspect
// Q2/Q3 in BUILDINGS_SLICE: TOWN 2500, POI 437.5, ...), NOT the record's `size radius` - that is the nest size (GAMEDATA_SCHEMA).
// A point in two overlapping circles belongs to the one whose centre is nearest (the dialogue's townAt rule, kept).
#pragma once

#include "kdata/gamedata.hpp"
#include "ksim/buildings.hpp"
#include "ksim/state.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

namespace ksim::towncircle {

struct Circle
{
	double x = 0.0;
	double z = 0.0;
	double radius = 0.0;
};

// One placed town's circle: the TownState's primary placement centre and the exe's town radius (0 for a marker / NULL town, which
// is not a town a character stands in).
inline Circle of(const kdata::GameData& data, const state::TownState& town) noexcept
{
	const kdata::Record* root = data.find(town.rootSid);
	const kdata::Record* active = data.find(town.activeSid);
	if (active == nullptr)
		active = root;
	const int type = active != nullptr ? static_cast<int>(active->intOr("type", 2)) : 2;
	const double mult = active != nullptr ? active->floatOr("town radius mult", 1.0) : 1.0;
	return {town.x, town.z, buildings::townRadius(type, mult)};
}

// The index of the circle (x, z) stands in, -1 outdoors: the nearest centre among the circles containing the point (the first on a
// tie, so the answer does not depend on the list's order).
inline int at(const std::vector<Circle>& circles, double x, double z) noexcept
{
	int best = -1;
	double nearest = 0.0;
	for (std::size_t i = 0; i < circles.size(); ++i)
	{
		const Circle& c = circles[i];
		const double d2 = (x - c.x) * (x - c.x) + (z - c.z) * (z - c.z);
		if (!(d2 <= c.radius * c.radius))
			continue;
		if (best < 0 || d2 < nearest)
		{
			best = static_cast<int>(i);
			nearest = d2;
		}
	}
	return best;
}

// A player town's size level (FACTIONS.md 4 "Player gates"): `owner`'s (a state::Id value) finished buildings inside the circle, through
// buildings::townLevel. OURS for "parented to the town": a record town owns no PlayerBuilding, so the circle stands in. Walks the
// PlayerBuilding table: call it once per town and cache, never per tick.
inline int levelOf(flecs::world& ecs, const kdata::GameData& data, std::uint64_t owner, const Circle& circle)
{
	std::vector<const state::PlayerBuilding*> members;
	ecs.each([&](const state::PlayerBuilding& building)
	{
		if (building.ownerFaction == owner && std::hypot(building.x - circle.x, building.z - circle.z) <= circle.radius)
			members.push_back(&building);
	});
	return buildings::townLevel(data, members);
}

} // namespace ksim::towncircle
