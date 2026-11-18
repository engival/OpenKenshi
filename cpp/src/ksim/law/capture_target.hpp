#pragma once

// Task BR2 (dt5 FUN_1408cdd00, dt8): the ONE CAPTURE_NEW_SLAVES 167 candidate resolver, shared by the town scorer's CageWarden
// (routine_scorer.cpp, TOWN_LIFE_SLICE 5.19) and a built world squad's stepWorldTasks (world_tasks.cpp). It existed twice, and the two
// copies disagreed on the ally test (`Simulation::alliedTo` in one, `CombatArena::bonded` in the other). The terms are SLAVERY 3's;
// RUNTIME_SYSTEMS G3 names the debt this pays.
//
// Task BU: it reads the hunter's SENSOR LIST now, as the exe does (s59_static.md section 2: 140853960 with include 0x4008 and exclude
// 0x11, then the per-character tests, nearest first), instead of scanning every arena member. A remembered body counts - bit 8 (not
// currently seen) is not excluded - which is what `Fighter::sight` keeps for the memory (perception.cpp).

#include "ksim/combat/combat.hpp"
#include "ksim/law/crime.hpp"
#include "ksim/ai/sensor.hpp"

#include <cmath>
#include <cstdint>

namespace ksim::capture {

// The goal's masks (1408cdd00 line 25): a record with bit 3 (incapacitated) or bit 14 (downed), and neither bit 0 (ally) nor bit 4 (dead).
inline constexpr std::uint32_t kInclude = sensor::kIncapacitated | sensor::kDowned;
inline constexpr std::uint32_t kExclude = sensor::kAlly | sensor::kDead;

// The per-character tests after the masks (s59 section 2, tests 1, 2, 3, 6 and 7): some KO/down state (dt8: a conscious crawler counts),
// not carried, not in an object (a bed or a cage), not already a slave (state 1), and not law / trader / diplomat class. dt5's medical
// test reads 0 on humans, so there is none here; test 8's limb mask is U (not modelled).
inline bool resolvable(const state::Fighter& body)
{
	if (!(body.unconscious || body.down))
		return false;
	if (body.carriedBy != 0 || body.asleep || body.cageBuilding != 0)
		return false;
	if (body.slaveState == 1)
		return false;
	return body.npcClass != crime::kClassLaw && body.npcClass != crime::kClassTrader && body.npcClass != crime::kClassDiplomat;
}

// The nearest resolvable body of `hunter`'s sensor list within `sight` units, with `claimed` reporting a body a squadmate already picked
// (test 5: one catcher per victim); 0 when none. `lookup` resolves a record's target Id (CombatArena::find at the call sites). Distance
// is the body's current position, and the hunter itself is never a candidate.
template <typename Lookup, typename Claimed>
std::uint64_t nearest(std::uint64_t hunter, const state::Fighter& self, const sensor::Context& ctx, double sight, Lookup&& lookup,
	Claimed&& claimed)
{
	std::uint64_t found = 0;
	double best = sight;
	sensor::sensed(self, kInclude, kExclude, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& body, std::uint32_t)
		{
			if (id == hunter || claimed(id) || !resolvable(body))
				return;
			const double d = std::hypot(body.x - self.x, body.z - self.z);
			if (d <= best)
			{
				best = d;
				found = id;
			}
		});
	return found;
}

} // namespace ksim::capture
