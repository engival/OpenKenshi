#pragma once

// G3, the consumer resolvers a world squad's selector runs (kinspect dt15_resolvers.md, dt16_resolvers.md; RUNTIME_SYSTEMS G3).
// Each mirrors one goal's target finder: it returns the target (a character Id, or a cage's site) or 0, exactly the exe's
// "resolver returns 1.0 or 0, and copies the target's hand". The sensor-backed ones read the member's sensor list (sensor.hpp) with
// the goal's masks; the cage-backed ones take a list of cages the caller has collected for the member's town (Cage).
//
// Pure over Fighters and values: the caller supplies `lookup` (record target -> Fighter*, CombatArena::find), `claimed` (a squadmate
// is already on the target: one catcher per victim) and, for 183, `looks` ("it already looks like a slave", the exe's
// TARGET_LOOKS_LIKE_A_SLAVE; ours reads the worn shackles, so the caller supplies it).
//
// Kept faithful, quirks included: 260 takes no squad claim (two processors may pick the same prisoner, dt15), 183's sensor path has
// no distance test at all, 177 picks the FIRST record in list order, not the nearest. Our record list iterates in Id order where the
// exe iterates a hash map (UNSURE, s59), so "first" is Id order here.

#include "../law/capture_target.hpp" // file-relative: tests include this header by path, without src/ksim on theirs
#include "ksim/law/crime.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/ai/sensor.hpp"
#include "ksim/world/towncircle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ksim::worldres {

// A cage the cage resolvers see, already reduced to what they read (the caller resolves the occupant, the building's faction and
// the reservation state). `floorY`/`hasFloor` are the piece's floor for 183's same-floor test.
struct Cage
{
	routines::Site site;
	std::uint64_t occupant = 0;
	std::uint64_t building = 0;
	double x = 0.0;
	double z = 0.0;
	double floorY = 0.0;
	bool hasFloor = false;
	bool freeSlot = true; // the cage can take a body (its slot count)
	bool blocked = false; // the member's 20 s blocked set
	bool claimed = false; // a squadmate is registered on it
	bool myFaction = false; // the building's faction is the member's
	std::uint64_t faction = 0; // the building's faction entity id (its own `owner faction ID`, else the town's owner's; 172's scope)
	bool myHome = false; // the building is the member's home building
	bool publicBuilding = false;
	// 276's finder gate (rwh.100, BuildingFinder_CagesToLiberate::vf0 140007e3c): the cage owner's relation to the liberator's faction is >= 50 (the
	// caller's bonded(owner, mine); false for a cage of my own faction or of nobody: UNSURE what the exe's own-faction relation reads).
	bool ownerAllied = false;
	// 276 LIBERATE_ALL_THE_PRISONERS (task raid-goals): the cage's town circle index (-1 outdoors; the town the finder runs over,
	// dt24_276) and the occupant's faction (0 when the cage is empty), which is all its resolver reads of the captive.
	int town = -1;
	std::uint64_t occupantFaction = 0;
};

// 177 LOOT_THE_DEAD (dt16 section 2): pass 1 a corpse (the record's dead bit), pass 2 an incapacitated or downed body; the first in
// list order, not carried, within 100 u, no squadmate on it, with something the member's direct task takes (`wanted`: task BO moves
// the food only, so a body with no food is not a target of ours). The zone / building-access / platoon gates are not modelled (M in the
// spec). The executor takes the food, not the gear (task BO).
template <typename Lookup, typename Claimed, typename Wanted>
std::uint64_t loot177(const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup, Claimed&& claimed, Wanted&& wanted)
{
	const auto pass = [&](std::uint32_t include, std::uint32_t exclude) -> std::uint64_t
	{
		std::uint64_t found = 0;
		sensor::sensed(self, include, exclude, ctx, lookup,
			[&](std::uint64_t id, const state::Fighter& body, std::uint32_t)
			{
				if (found != 0 || body.carriedBy != 0 || claimed(id) || !wanted(id, body))
					return;
				const double dx = body.x - self.x;
				const double dz = body.z - self.z;
				if (dx * dx + dz * dz <= 100.0 * 100.0)
					found = id;
			});
		return found;
	};
	const std::uint64_t corpse = pass(sensor::kDead, 0);
	if (corpse != 0)
		return corpse;
	return pass(sensor::kIncapacitated | sensor::kDowned, sensor::kAlly | sensor::kDead);
}

// 14099c110 PICKUP_INTRUDERS 223 (dt24_223 section 2; s59 section 3): the first record of the member's sensor list (include 0x400e =
// neutral | hostile | incapacitated | downed, exclude 0x201 = ally | in an object) that is an incapacitated body inside MY HOME
// building: not carried, not in a bed or a cage, not a conscious crawler. The exe takes the first hit in its record-map order; ours is
// Id order (UNSURE, the same caveat as 177). The exe's resolver has no squad claim, so neither has this one - two residents may pick the
// same body, and the caller's own taken set settles it (the take-out executor's).
template <typename Lookup>
std::uint64_t pickupIntruders223(const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup)
{
	if (self.homeBuilding == 0)
		return 0; // I own no building: nothing to shoo from (the resolver's own ownerships test)
	std::uint64_t found = 0;
	sensor::sensed(self, sensor::kNeutral | sensor::kHostile | sensor::kIncapacitated | sensor::kDowned,
		sensor::kAlly | sensor::kInObject, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& body, std::uint32_t)
		{
			if (found != 0 || body.carriedBy != 0 || body.building != self.homeBuilding)
				return; // the interior must be my home building's, and no one may be carrying it
			// vf+0x30: incapacitated (KO or down); +0x2f8 == 0: not in use / not in a bed.
			if (!(body.unconscious || body.down) || body.asleep || body.cageBuilding != 0)
				return;
			found = id;
		});
	return found;
}

// 14099edd0 STAND_AT_GUARD_NODE_HOMEBUILDING 38, its intruder hunt (dt24_38 section 3; task CH group 3): a guard whose home building is
// a guard post goes first for the people who are inside SOMEBODY ELSE'S building. The sensor mask is include 2 (neutral) with exclude
// 0x2a9 = ally | incapacitated | slave | law or military | in an object (bed or cage); a record is kept when it does NOT sneak
// (1405c9750) and stands inside some building (its interior is set; the exe's "interior type != 0xb") that is not my own home. The
// exe takes the whole list (the shared node finder then picks one at random, the caller's roll); ours is Id order (OURS, the
// sensor-list caveat of 223). A dead record is not in the mask's excludes and is kept here too.
template <typename Lookup>
std::vector<std::uint64_t> guardNodeIntruders38(const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup)
{
	std::vector<std::uint64_t> found;
	if (self.homeBuilding == 0)
		return found;
	sensor::sensed(self, sensor::kNeutral,
		sensor::kAlly | sensor::kIncapacitated | sensor::kSlave | sensor::kLawMilitary | sensor::kInObject, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& body, std::uint32_t)
		{
			if (body.sneak)
				return; // a sneaking character is not the intruder this goal hunts
			if (body.building == 0 || body.building == self.homeBuilding)
				return; // inside some building, and not my own
			found.push_back(id);
		});
	return found;
}

// 14099c250 PICKUP_INTRUDERS_TOWN 273 (dt24_273 section 2; task CH group 3): the town-scoped twin of 14099c110. The member's current
// town must exist and be ours (the caller checks the ownership, `Simulation::townOursAt`; the exe's town `function id` != 0 is not
// modelled - OURS: a TownState with a circle is a town). The sensor list is 223's mask (include 0x400e, exclude 0x201); a body is
// accepted when it is not carried, incapacitated, not in a bed, stands in MY town (anywhere in it, not only my home building; the
// exe's `its town == my town`) and inside the town's gates (vf+0x118 > 0 - ours is the walled region, perception.hpp inWalls). The
// exe's own version of this finder, FUN_14099bfc0, is decoded in GATES_CARRY_STATIC 3.3 with a crime-knowledge test instead; that
// discrepancy is not resolved (UNSURE), and this port follows dt24_273. First hit in Id order, like 223.
template <typename Lookup>
std::uint64_t pickupIntrudersTown273(const state::Fighter& self, int myTown, const sensor::Context& ctx,
	const perception::Environment& env, Lookup&& lookup)
{
	if (myTown < 0)
		return 0; // no current town
	std::uint64_t found = 0;
	sensor::sensed(self, sensor::kNeutral | sensor::kHostile | sensor::kIncapacitated | sensor::kDowned,
		sensor::kAlly | sensor::kInObject, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& body, std::uint32_t)
		{
			if (found != 0 || body.carriedBy != 0 || body.cageBuilding != 0)
				return; // no one may be carrying it, and a caged body is not the lift's (167 / 204)
			// vf+0x30: incapacitated (KO or down); +0x2f8 != 2: not in a bed (UNSURE: GATES_CARRY_STATIC reads +0x2f8 == 2 as a cage
			// occupant; either way it is in use, and the mask's kInObject already excludes both).
			if (!(body.unconscious || body.down) || body.asleep)
				return;
			if (env.townOf(body.x, body.z) != myTown)
				return; // the body's town must be mine
			if (!env.inWalls(body.x, body.z))
				return; // vf+0x118 > 0: inside the town's gates
			found = id;
		});
	return found;
}

// 1408ced70 SHOO_STRANGERS_OUT_OF_MY_BUILDING_IF_PRIVATE 283 (dt24_283 section 2; task CH): the exe's "is there a stranger in my
// building" test. A stranger is a NEUTRAL bit record (include 2: hostiles are other goals' business), conscious, inside MY HOME
// building and not of my faction (the neutral bit already excludes an ally; a same-faction body is skipped too). The goal keeps its
// current hand (`current`) while that character is still inside my building; else the scan takes the first hit in list order.
template <typename Lookup>
std::uint64_t shooStrangers283(const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup, std::uint64_t current)
{
	if (self.homeBuilding == 0)
		return 0; // I own no building: nothing to shoo from
	if (current != 0)
	{
		const state::Fighter* held = lookup(current);
		if (held != nullptr && !held->dead && held->building == self.homeBuilding)
			return current; // the hand is kept while the stranger is still in my building (the exe's first test)
	}
	std::uint64_t found = 0;
	// The exe's excludes 0x298 resolve to "in an object / a law or military class / incapacitated" (UNSURE beyond those names, dt24_283).
	sensor::sensed(self, sensor::kNeutral, sensor::kInObject | sensor::kIncapacitated | sensor::kLawMilitary, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& body, std::uint32_t)
		{
			if (found != 0 || body.dead || body.building != self.homeBuilding)
				return;
			// vf+0x30 == 0: conscious. A bed or a cage is the exclude mask's business; the body must stand in the room.
			if (body.unconscious || body.down || body.asleep || body.cageBuilding != 0)
				return;
			if (body.faction == self.faction)
				return; // 14032e2a0: never my own faction's
			found = id;
		});
	return found;
}

// The door-list resolvers of the town goals (task ds/town-port; dt24_86 / dt24_155). The exe walks my building's door list: 86's
// resolver 1408cb360 takes the first door that is not open (vf+0x308 == 0 and door+0x380 != 1); 155 / 197's shared 1408cb1c0 the first
// door whose lock is not effective (lock+0x20 == 0, a broken lock, or level < 1) or that is open. doors.md (live-checked): door+0x380 is
// the door STATE (0 closed, 1 open, 2 opening, 3 closing) and the lock is a separate object (+0x370), so dt24_86's "barred / locked by
// policy" reading of +0x380 is a naming error and a LOCKED door (closed) is 86's to open - the plan unlocks it first. `states` is the home's
// doors in list order, a state::Door::state each (0 open, 1 closed, 2 locked; gates and broken doors are left out by the caller).
// Both return the door's index, -1 for none. UNSURE: 86's shop-counter tail is undecoded.
inline int firstDoorToOpen86(const std::vector<std::uint8_t>& states)
{
	for (std::size_t i = 0; i < states.size(); ++i)
		if (states[i] == 1 || states[i] == 2)
			return static_cast<int>(i);
	return -1;
}
inline int firstDoorToLock155(const std::vector<std::uint8_t>& states)
{
	for (std::size_t i = 0; i < states.size(); ++i)
		if (states[i] != 2)
			return static_cast<int>(i);
	return -1;
}

// 1408ca3d0 "still to be processed" (dt15 section 0): no captor or no owner faction = yes; else the owner must be friendly, the body
// not carried or dead, no squadmate on it; escaping / ex-slave state = yes; a slave that already looks like one = no.
template <typename Claimed, typename LooksLike>
bool stillToProcess(const sensor::Context& ctx, const state::Fighter& self, std::uint64_t id, const state::Fighter& body,
	Claimed&& claimed, LooksLike&& looks)
{
	if (body.carriedBy != 0 || body.dead)
		return false;
	if (body.captorFaction == 0)
		return true;
	if (body.captorFaction != self.faction
		&& (!ctx.relation || ctx.relation(self.faction, body.captorFaction) < sensor::kAllyRelation))
		return false;
	if (claimed(id))
		return false;
	if (body.slaveState == 2 || body.slaveState == 3)
		return true;
	return !looks(id, body);
}

// 183 SLAVE_WATCHING (dt15 section 1): a visible slave (state 1 or 2) that still needs processing, first in list order - no distance
// test on this path; else an occupied cage in the building the watcher stands in, on its floor, within 180 u.
template <typename Lookup, typename Claimed, typename LooksLike>
std::uint64_t slaveWatching183(const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup, Claimed&& claimed,
	LooksLike&& looks, std::span<const Cage> cages)
{
	if (self.asleep || self.cageBuilding != 0)
		return 0; // the watcher itself is in an object
	std::uint64_t found = 0;
	sensor::sensed(self, sensor::kSlave | sensor::kEscapingSlave, sensor::kDead | sensor::kNotSeen, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& body, std::uint32_t)
		{
			if (found == 0 && body.slaveState != 0 && stillToProcess(ctx, self, id, body, claimed, looks))
				found = id;
		});
	if (found != 0)
		return found;
	for (const Cage& cage : cages)
	{
		if (cage.occupant == 0 || cage.building == 0 || cage.building != self.building || cage.claimed)
			continue;
		if (cage.hasFloor && std::fabs(cage.floorY - self.y) > 30.0)
			continue; // same floor (kFloorReach)
		const double dx = cage.x - self.x;
		const double dz = cage.z - self.z;
		if (dx * dx + dz * dz > 180.0 * 180.0)
			continue;
		const state::Fighter* occupant = lookup(cage.occupant);
		if (occupant != nullptr && stillToProcess(ctx, self, cage.occupant, *occupant, claimed, looks))
			return cage.occupant;
	}
	return 0;
}

// 260 NEW_SLAVE_PROCESSING (dt15 section 2): the first cage whose occupant is not incapacitated and not yet a slave (state 0).
// Deliberately no squad claim and no distance test (dt15: two processors can pick the same prisoner).
template <typename Lookup>
std::uint64_t newSlave260(std::span<const Cage> cages, Lookup&& lookup)
{
	for (const Cage& cage : cages)
	{
		const state::Fighter* occupant = cage.occupant != 0 ? lookup(cage.occupant) : nullptr;
		if (occupant == nullptr || occupant->unconscious || occupant->carriedBy != 0 || occupant->slaveState != 0)
			continue;
		if (cage.blocked)
			continue;
		return cage.occupant;
	}
	return 0;
}

// 168 CARRY_WOUNDED_SLAVES (dt15 section 3): my faction's slave, in some down state (a conscious crawler too: a squad that shot its
// captive down leaves it on the ground otherwise, kenshi-k16.6.28; the exe's first-down-stage exclusion is UNSURE), not carried, not in an object, not claimed; nearest.
template <typename Lookup, typename Claimed>
std::uint64_t carryWounded168(const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup, Claimed&& claimed)
{
	std::uint64_t found = 0;
	double best = 1.0e30;
	sensor::sensed(self, sensor::kSlave, sensor::kDead | sensor::kInObject, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& body, std::uint32_t)
		{
			if (!(body.unconscious || body.down))
				return;
			if (body.captorFaction != self.faction || body.slaveState == 0 || body.carriedBy != 0 || body.asleep
				|| body.cageBuilding != 0 || claimed(id))
				return;
			const double dx = body.x - self.x;
			const double dz = body.z - self.z;
			const double d2 = dx * dx + dz * dz;
			if (d2 < best)
			{
				best = d2;
				found = id;
			}
		});
	return found;
}

// 132 FIND_AND_KIDNAP (dt26 section 4; 1408cd2d0 with include 0x4008, exclude 0x811, flag 1): the nearest downed or incapacitated
// character that is not an ally, not dead, not a robot, not an animal, not carried, not in an object and not already the squad's.
// 0 = none (the goal then scores nothing and the next pass re-tries).
template <typename Lookup, typename Claimed>
std::uint64_t kidnap132(const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup, Claimed&& claimed)
{
	std::uint64_t found = 0;
	double best = 0.0;
	sensor::sensed(self, sensor::kIncapacitated | sensor::kDowned, sensor::kAlly | sensor::kDead | sensor::kRobot, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& body, std::uint32_t)
		{
			if (body.animal || body.carriedBy != 0 || body.cageBuilding != 0 || claimed(id))
				return;
			const double dx = body.x - self.x;
			const double dz = body.z - self.z;
			const double d2 = dx * dx + dz * dz;
			if (found == 0 || d2 < best)
			{
				best = d2;
				found = id;
			}
		});
	return found;
}

// 112 FIND_CAGE_AND_PUT_IN (dt15 section 4): the best free cage, score clamp(1 - d2 / 4608^2, 1e-5, 1) x100 for my faction, x100
// for my home building, x1e-5 when a law / military member takes a non-public building's cage. A foreign cage without the permission
// is skipped (OURS: the exe's 140621230). False when no cage admits.
inline double cageScore(const state::Fighter& self, const Cage& cage) noexcept
{
	const double dx = cage.x - self.x;
	const double dz = cage.z - self.z;
	const double d2 = dx * dx + dz * dz;
	double f = std::clamp(1.0 - d2 / 21233664.0, 1.0e-05, 1.0);
	if (cage.myFaction)
		f *= 100.0;
	if (cage.myHome)
		f *= 100.0;
	if ((self.npcClass == crime::kClassLaw || self.npcClass == crime::kClassMilitary) && !cage.publicBuilding)
		f *= 1.0e-05;
	return f;
}

inline bool findCage112(const state::Fighter& self, std::span<const Cage> cages, routines::Site& site, double* outScore = nullptr)
{
	double best = -1.0;
	const Cage* found = nullptr;
	for (const Cage& cage : cages)
	{
		if (cage.blocked || cage.claimed || !cage.freeSlot)
			continue;
		if (!cage.myFaction && !cage.myHome)
			continue;
		const double f = cageScore(self, cage);
		if (f > best)
		{
			best = f;
			found = &cage;
		}
	}
	if (found == nullptr)
		return false;
	site = found->site;
	if (outScore != nullptr)
		*outScore = best;
	return true;
}

// A disposal machine 137's finder can pick (dt24_patrol_town_pot section 3 = 1408d0770(ai, usable=1, function 0xb); dt24_172
// section 1408d0770 for the body): already reduced to the terms the picker reads. The caller drops the pots that are neither my
// faction's nor mine (the exe's `else if !140621230(...) continue`) and the ones already one of my task targets.
struct Pot
{
	std::uint64_t building = 0;
	double x = 0.0;
	double z = 0.0;
	bool usable = true; // (b vf+0x4f0)(b, empty hand) == 1; nothing occupies a pot in our model, so always true (OURS/UNSURE)
	bool myFaction = false; // b faction == my faction (x100)
	bool myHome = false; // the building is my home building (x100)
	bool publicBuilding = true; // false -> x1e-5 for a law / military member
};

// 137 DEAD_GUYS_GO_IN_THE_POT's finder (1408d1090): an ARGMAX over the pots of clamp(1 - d2 / 4608^2, 1e-5, 1) x100 (my faction)
// x100 (my home building) x1e-5 (a law / military member on a non-public interior). No RNG. The first of an equal pair (the caller's
// order) wins, the registry's `score > best`. False when no pot admits.
inline bool findPot137(const state::Fighter& self, bool lawMilitary, std::span<const Pot> pots, std::uint64_t& out, double* outScore = nullptr)
{
	double best = -1.0;
	const Pot* found = nullptr;
	for (const Pot& pot : pots)
	{
		if (!pot.usable)
			continue;
		const double dx = pot.x - self.x;
		const double dz = pot.z - self.z;
		double f = std::clamp(1.0 - (dx * dx + dz * dz) / 21233664.0, 1.0e-05, 1.0);
		if (pot.myFaction)
			f *= 100.0;
		if (pot.myHome)
			f *= 100.0;
		if (lawMilitary && !pot.publicBuilding)
			f *= 1.0e-05;
		if (f > best)
		{
			best = f;
			found = &pot;
		}
	}
	if (found == nullptr)
		return false;
	out = found->building;
	if (outScore != nullptr)
		*outScore = best;
	return true;
}

// RaceData+0x7c (dt27 section 2.2: the cage executors' "inedible race" test, 135's occupant acceptance and 138's): read as the robot
// flag (the s59 flag table's bit 11, UNSURE). A robot race's parts are all robotic.
inline bool inedibleRace(const state::Fighter& f)
{
	if (f.parts.empty())
		return false;
	std::size_t robotic = 0;
	for (const state::FighterPart& part : f.parts)
		robotic += part.robotic ? 1 : 0;
	return robotic * 2 > f.parts.size();
}

// One function-8 cage of the corpse search: its occupant (0 = empty) and whether it is built (the construction block's byte 0,
// `vf+0x228`, dt30 correction).
struct DeadGuyCage
{
	std::uint64_t occupant = 0;
	bool built = true;
};

// 138 FIND_A_DEAD_GUY ("Cannibal- Find a corpse"; 1408cfb00, dt30 + its correction): first the BUILT function-8 cages of my area
// (`cages`: the caller's list, in its query order) and their occupant - dead, not carried, an edible race (RaceData+0x7c == 0) and no
// package target of a squad-mate - the first match wins; else the nearest unclaimed dead body in the member's senses (1408ce170, section
// 230 of dt24_172: dead, not carried, no distance cap - the cap is FLT_MAX). The fallback has no race gate (dt30 section 2 step 5 lists
// dead / uncarried / package only; UNSURE). A cage pass tie is the caller's order (the exe's hash order is UNSURE).
template <typename Lookup, typename Claimed, typename Edible>
std::uint64_t findCorpse138(const state::Fighter& self, const sensor::Context& ctx, std::span<const DeadGuyCage> cages, Lookup&& lookup,
	Claimed&& claimed, Edible&& edible)
{
	for (const DeadGuyCage& cage : cages)
	{
		if (!cage.built || cage.occupant == 0)
			continue;
		const state::Fighter* body = lookup(cage.occupant);
		if (body != nullptr && body->dead && body->carriedBy == 0 && edible(*body) && !claimed(cage.occupant))
			return cage.occupant;
	}
	std::uint64_t found = 0;
	double best = 0.0;
	sensor::sensed(self, sensor::kDead, 0, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& body, std::uint32_t)
		{
			if (body.carriedBy != 0 || body.cageBuilding != 0 || claimed(id))
				return; // a caged corpse comes only through the cage list above
			const double dx = body.x - self.x;
			const double dz = body.z - self.z;
			const double d2 = dx * dx + dz * dz;
			if (found == 0 || d2 < best)
			{
				best = d2;
				found = id;
			}
		});
	return found;
}

// 112's utility (kinspect dt24_112 section 3, FUN_1405a0f10): how much putting the carried `body` in a cage is worth, which is NOT the
// cage choice above - a port that only reproduces findCage112 cages allies, whom the exe scores 0. The ladder, in the exe's order:
// no town -> 0.1 (DAT_14167fad0); an animal -> 0 (the character virtual at vtable+0x248, a 0 stub for every non-animal class); a law /
// military member fencing a slave it has no positive relation with -> 1.4 (DAT_14169bb48); an ally -> 0; a corpse -> 0.5; anything else
// -> 2.0. Not modelled: the package-targets-my-town 0 (we carry no package target hand) and the unresolvable-hand 1.0 (our carried is
// always an entity). The 0.5 is unreachable behind the goal's isCarriedDudeDead = 0 precondition, as in the exe.
inline double putInCage112(const state::Fighter& body, bool hasTown, bool lawOrMilitary, bool positiveRelation, bool allied) noexcept
{
	if (!hasTown)
		return 0.1;
	if (body.animal)
		return 0.0;
	if (lawOrMilitary && !positiveRelation && body.slaveState == 1)
		return 1.4;
	if (allied)
		return 0.0;
	if (body.dead)
		return 0.5;
	return 2.0;
}

// -------------------------------------------------------------------------------------------------------------------------------
// The machine and storage pull goals (dt24_emptymachine_dismantle; dt24_113 / dt24_115 / dt24_131). 92 / 247 / 284 have no finder
// of their own (0 AI_TASK records, no resolver): the exe's goal node picks the building and the plan puts it in the action hand.
// Ours keeps one reduced building view and one picker for both halves.

// A building the pull tasks may take from: a machine (state::Producer: its output slot's type and stock) or a storage box
// (state::Container: every distinct type it holds). Those are the "collectable item types" the task's tick asks the building for
// (m vf+0x548, dt24_emptymachine_dismantle section 2.2). `mine` is the picker's ownership test; ours is the member's faction
// owning the building (OURS: the exe's 1408cbea0 walks "my town's buildings" and 243 the package home base).
struct StockBuilding
{
	std::uint64_t id = 0;
	double x = 0.0;
	double z = 0.0;
	double useX = 0.0;
	double useZ = 0.0;
	bool mine = false;
	std::vector<std::string> types; // the collectable types (never empty for a building that offers nothing: see the callers)
};

// The reach of the machine picker (dt24_113 section 4: AI+0x2bc distance_mode 40 / 200 / 6516.7 u). OURS: mode 1's 200 u, the
// mode the machine jobs use (UNSURE; the exe's per-character distance_mode is not stored in ours).
inline constexpr double kStockReach = 200.0;

// The nearest of my buildings within `reach` that lists something; `type` gets its first collectable type. 0 = none.
template <typename Buildings>
std::uint64_t nearestStock(const Buildings& buildings, const state::Fighter& self, double reach, std::string* type)
{
	std::uint64_t found = 0;
	double best = reach * reach;
	for (const StockBuilding& building : buildings)
	{
		if (!building.mine || building.types.empty())
			continue;
		const double dx = building.x - self.x;
		const double dz = building.z - self.z;
		const double d2 = dx * dx + dz * dz;
		if (d2 <= best)
		{
			best = d2;
			found = building.id;
			if (type != nullptr)
				*type = building.types.front();
		}
	}
	return found;
}

// 92 / 247 (the machine half: 113 / 94's picker, dt24_113 section 4): the nearest of my machines whose output slot holds something.
inline std::uint64_t machine92(std::span<const StockBuilding> machines, const state::Fighter& self, double reach, std::string* type)
{
	return nearestStock(machines, self, reach, type);
}

// 284 (the storage half: 131 / 243's picker, dt24_131 section 1): the nearest of my storage boxes that holds something.
inline std::uint64_t container284(std::span<const StockBuilding> boxes, const state::Fighter& self, double reach, std::string* type)
{
	return nearestStock(boxes, self, reach, type);
}

// 184 PUT_LOOT_IN_STORAGE (dt24_184, resolver 14059b040): the nearest storage within the reach cap. The caller has already chosen the
// candidate set (the home's storages, or the town's nearest-allies storages) and kept only the boxes that accept a carried item
// (`types` holds that item); it has also returned nothing for a character that carries no excess loot or stands in no town.
inline std::uint64_t storage184(std::span<const StockBuilding> storages, const state::Fighter& self, double reach)
{
	return nearestStock(storages, self, reach, nullptr);
}

// 243 AQUIRE_FOOD_AT_HOMEBASE (dt24_243): the same finder as 131 with the food mask 0x2614, run over the package's home base first
// and my town as the fallback, from the character's position, first result - any food-holding object, no ranking. OURS: the caller
// passes only the boxes that hold a food item (it has the item data) and there is no reach cap - the exe searches the whole
// base/town, and our containers only materialise near a player anyway; the package home-base hand has no data row in ours.
inline constexpr double kBaseReach = 1.0e9;
inline std::uint64_t food243(std::span<const StockBuilding> boxes, const state::Fighter& self, std::string* type)
{
	return nearestStock(boxes, self, kBaseReach, type);
}

// One other building the quantity reads: how many of the item its slot holds, and whether its inventory can take one.
struct NearbyStock
{
	std::uint32_t count = 0; // units of the item (an unlimited slot: at least 1)
	bool accepts = false; // the building's own inventory can accept the item type
};

// vf5 (dt24_emptymachine_dismantle section 2.1): how many units of `item` this pass takes. The carried count of the categories
// the exe sums is 0 in ours (we do not store item category 3 / 0xf, UNSURE); an empty-handed character taking a function-3 item
// (food) still takes one; the other buildings' stock of the item adds on, and a building that cannot stock it but can take it
// returns the flat 5. BuildMats overrides the whole result to 2 (the class's vf5 1403411c0) and the task's +0x78 mode to the
// tick's 10 (types[0], no vf5).
inline int pullQuantity(std::uint32_t carried, bool functionThree, std::span<const NearbyStock> others, bool buildMats, bool mode) noexcept
{
	if (mode)
		return 10; // the task byte argument: take types[0], ten per pass
	if (buildMats)
		return 2; // Task_EmptyMachine_BuildMats vf5: `return 2`
	int n = static_cast<int>(carried);
	if (n == 0 && functionThree)
		n = 1; // the empty-handed baseline for an ITEM of function 3
	for (const NearbyStock& other : others)
	{
		if (other.count != 0)
		{
			n += static_cast<int>(other.count);
			continue;
		}
		if (other.accepts)
			return 5; // "the colony handles this item": a batch of five
	}
	return n;
}

// -------------------------------------------------------------------------------------------------------------------------------
// The combat-joining goals (task CF; kinspect dt24_143 / dt24_15 / dt24_16). Each returns the goal's hand - a squadmate or an
// ally that is fighting (143 / 15), or the enemy itself (16) - or 0. The exe's resolver utilities (flat 1.2 / 1.0) are the goal
// rows' in goap.cpp; these only pick the hand.

// 14099a190, the shared combat-class score (dt24_143): 0 when the character cannot fight (dead, KO, down), 0.01 when it is in a
// fight. The graded value under the 0.01 ceiling (combat state x 0.25 - 0.2, CharStats+0xb0 x 0.1) needs CombatClass, which we do
// not model - ours is the fight / no-fight split, all the flat-1.2 goals read (UNSURE).
inline double combatScore14099a190(const state::Fighter& other) noexcept
{
	if (other.dead || other.unconscious || other.down)
		return 0.0;
	return other.target != 0 ? 0.01 : 0.0;
}

// 14099d510 PROTECT_OWN_SQUAD 143: a member of my platoon (same squad) that is seen or heard and in a fight, the best score. The
// exe's pass 2 (my ownerships, x0.1) is not modelled: we have no ownership list (OURS/UNSURE).
template <typename Lookup>
std::uint64_t protectOwnSquad143(const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup)
{
	if (self.squad == 0)
		return 0; // the visit below never picks anyone then: skip the walk of the sight records
	std::uint64_t best = 0;
	double bestScore = 0.0;
	sensor::sensed(self, sensor::kSameFaction, sensor::kDead | sensor::kIncapacitated | sensor::kInObject, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& other, std::uint32_t)
		{
			// The exe's gate is the record's seen flag (+0x38): aware, not necessarily sighted by the last scan.
			if (self.squad == 0 || other.squad != self.squad || !perception::aware(self, id))
				return;
			const double score = combatScore14099a190(other);
			if (score > bestScore)
			{
				bestScore = score;
				best = id;
			}
		});
	return best;
}

// 14099ca10 PROTECT_ALLIES 15: an ally (same faction or relation >= 50) that is seen or heard and in a fight; the best score, with
// the exe's player-faction distance factor clamp((1000 - d^2) / 990, 0.1, 1). The foreign-target squaring is not modelled (UNSURE).
template <typename Lookup>
std::uint64_t protectAllies15(const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup)
{
	std::uint64_t best = 0;
	double bestScore = 0.0;
	sensor::sensed(self, sensor::kAlly, sensor::kIncapacitated | sensor::kInObject, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& other, std::uint32_t)
		{
			if (!perception::aware(self, id))
				return; // the record's seen flag (exe +0x38), not the last scan's sighting
			double score = combatScore14099a190(other);
			if (score <= 0.0)
				return;
			if (ctx.playerFaction != 0 && other.faction == ctx.playerFaction)
			{
				const double dx = other.x - self.x;
				const double dz = other.z - self.z;
				score *= std::clamp((1000.0 - (dx * dx + dz * dz)) / 990.0, 0.1, 1.0);
			}
			if (score > bestScore)
			{
				bestScore = score;
				best = id;
			}
		});
	return best;
}

// 14059f3a0 ATTACK_ENEMIES 16: every hostile record seen or heard is a CANDIDATE; the shared picker 14059e300 scores the candidates and
// chooses the one (dt24_16; ai_target_hand.md Q3: the resolver copies no hand - it buffers, and the picker picks). So this is the
// resolver's admission of one candidate `id`, not a pick: the score and the one-per-tick choice stay in our picker stand-in
// (CombatArena::pickTarget). The resolver's combat gate (AI+0xbc) is not modelled.
template <typename Lookup>
bool attackEnemies16(const state::Fighter& self, const sensor::Context& ctx, std::uint64_t id, Lookup&& lookup)
{
	const state::SightRecord* record = perception::find(self, id);
	const state::Fighter* other = lookup(id);
	if (record == nullptr || other == nullptr || other->dead || !perception::aware(self, id))
		return false; // the record's seen flag (exe +0x38), not the last scan's sighting
	return sensor::matches(sensor::flags(ctx, self, *other, *record), sensor::kHostile,
		sensor::kIncapacitated | sensor::kInObject);
}

// 14059f720 ATTACK_ENEMIES_AND_NEUTRALS 21 (dt24_21; batch 2): 16 with the sensor mask widened to hostile + neutral, no seen/heard gate
// and no reach filter. Gates modelled: not a unique squad (the platoon vtable test), not in a bed or cage, the friendly test 1406b23c0
// (same faction or an ally) and the admission 1408569a0 - a relation enemy or a personal tag is admitted; a plain neutral only through
// the place tests we do not model, so it is NOT admitted here (UNSURE). The picker's own enemy()/bonded terms still cover the neutrals
// our world makes hostile (the team partition). The downed rule and the relevance test are not modelled.
template <typename Lookup>
bool attackEnemiesAndNeutrals21(const state::Fighter& self, const sensor::Context& ctx, std::uint64_t id, Lookup&& lookup)
{
	const state::SightRecord* record = perception::find(self, id);
	const state::Fighter* other = lookup(id);
	if (record == nullptr || other == nullptr || other->dead || other->uniqueSquad || other->asleep || other->cageBuilding != 0)
		return false;
	if (other->faction == self.faction || sensor::ally(ctx, self, *other))
		return false;
	const std::uint32_t bits = sensor::flags(ctx, self, *other, *record);
	return (bits & (sensor::kHostile | sensor::kNeutral)) != 0 && (bits & sensor::kIncapacitated) == 0;
}

// 140599f30's range (dt24_34): |me - t|^2 below 42467328 (6517 u).
inline constexpr double kTroubleRelevanceD2 = 42467328.0;

// 14099baa0 ATTACK_TROUBLE_MAKERS 34 (dt24_34; batch 2): a troublemaker in MY town. No candidate when I stand in no town - the exe
// returns 0 when its town is 0 (the town test replaces 16's AI+0xbc gate). Two sensor lists (4 hostiles, 2 neutrals with the class
// rules), then the accept path's town test, the criminal test (its goal aggression: we read it running an attack task, or my squad's
// short-term AGGRESSOR tag on its squad) and the admittance rules of 14099b820 (never a slaver; the place branches are not modelled,
// UNSURE). A remembered record counts: this resolver has no seen/heard test (s81). The exe's Town object is not modelled: the caller
// supplies `hasTown` and `sameTown(other)` (the town circle / walled region / building equality, and the live-observed join area -
// OURS, BAR_FIGHT_LOOP.md BQ14; task BZ's terms). `tagged` is my squad's AGGRESSOR test against a squad key.
template <typename Lookup, typename SameTown, typename Tagged>
bool troubleMaker34(const state::Fighter& self, const sensor::Context& ctx, std::uint64_t id, Lookup&& lookup, std::uint64_t myId,
	bool hasTown, SameTown&& sameTown, Tagged&& tagged)
{
	if (!hasTown)
		return false;
	const state::SightRecord* record = perception::find(self, id);
	const state::Fighter* other = lookup(id);
	if (record == nullptr || other == nullptr || other->dead || other->npcClass == crime::kClassSlaver)
		return false;
	const std::uint32_t bits = sensor::flags(ctx, self, *other, *record);
	if ((bits & (sensor::kHostile | sensor::kNeutral)) == 0 || (bits & sensor::kIncapacitated) != 0)
		return false;
	// List B's class rules: a diplomat, a trader or a law enforcer is never a troublemaker; a military one only when it is neither mine
	// nor an ally's (the exe's myFaction+0xf0 faction set is not modelled: `sensor::ally` stands in, OURS).
	if ((bits & sensor::kNeutral) != 0)
	{
		if (other->npcClass == crime::kClassDiplomat || other->npcClass == crime::kClassTrader || other->npcClass == crime::kClassLaw)
			return false;
		if (other->npcClass == crime::kClassMilitary && (other->faction == self.faction || sensor::ally(ctx, self, *other)))
			return false;
	}
	// The accept path: its town is mine (the exe re-checks getTown(t) == myTown after the lists).
	if (!sameTown(*other))
		return false;
	// Criminal (the goal aggression > 0, our stand-in = it runs an attack task, or the squad tag 2) and relevant (140599f30: its town
	// is my home place, it is within 6517 u, or it is already fighting me).
	// A defender (32 SELF_PRESERVATION, 143 PROTECT_OWN_SQUAD, 15 PROTECT_ALLIES) standing inside MY home building is no troublemaker (k16.6.85):
	// a bar patron answering the player's attack in the bar draws no 34 from the crew (rwh.82), while a defender elsewhere (the Dust Bandits
	// on the street) and an attacker that came in (Starving Bandits on 16 / 21) are picked as before (bar_fight_loop step 2, bq14). OURS / UNSURE: this
	// stands in for the exe's place branch of 14099b820 (the place the target stands in, dt24_34.md), not decoded: kenshi-k16.6.94.
	const bool defending = self.homeBuilding != 0 && other->building == self.homeBuilding
		&& (other->combatGoal == routines::task::SelfPreservation || other->combatGoal == routines::task::ProtectOwnSquad
			|| other->combatGoal == routines::task::ProtectAllies);
	const bool criminal = (other->target != 0 && !defending) || tagged(other->squad);
	const double dx = other->x - self.x;
	const double dz = other->z - self.z;
	const bool relevant = dx * dx + dz * dz < kTroubleRelevanceD2 || other->target == myId;
	return criminal && relevant;
}

// 14059fd70 TERRITORIAL_AGGRESSION_BUT_DONT_LEAVE_HOME 144 (dt24_144; batch 2): hostiles only, a remembered record counts (no
// seen/heard or relevance test - the home ring replaces it). An ally's slave is spared. Inside my home town's circle, radius x1.3,
// the candidate is kept; outside it only under the leash exception, which the exe reads through Character vf+0x1d8 and we model as "it
// is already fighting me" (OURS/UNSURE). With no home the exe tail-calls its no-home picker (14059fa70), a plain hostile attack: every
// admitted hostile is kept. `homeX/homeZ/homeRadius` are the town circle the member stands in; `haveHome` false = no circle.
inline constexpr double kHomeRingScale = 1.3; // dt24_144: DAT_14169bb40

template <typename Lookup>
bool territorial144(const state::Fighter& self, const sensor::Context& ctx, std::uint64_t id, Lookup&& lookup, std::uint64_t myId,
	bool haveHome, double homeX, double homeZ, double homeRadius)
{
	const state::SightRecord* record = perception::find(self, id);
	const state::Fighter* other = lookup(id);
	if (record == nullptr || other == nullptr || other->dead)
		return false;
	const std::uint32_t bits = sensor::flags(ctx, self, *other, *record);
	if ((bits & sensor::kHostile) == 0 || (bits & (sensor::kIncapacitated | sensor::kInObject)) != 0)
		return false;
	// An ally's slave is not my enemy (1406b2630 on the owner faction; our captorFaction stands in for the owner).
	if (other->slaveState == 1 && other->captorFaction != 0
		&& (other->captorFaction == self.faction || (ctx.relation && ctx.relation(self.faction, other->captorFaction) >= sensor::kAllyRelation)))
		return false;
	if (!haveHome)
		return true; // the no-home fallback: a plain hostile attack
	const double dx = other->x - homeX;
	const double dz = other->z - homeZ;
	const double ring = homeRadius * kHomeRingScale;
	if (dx * dx + dz * dz <= ring * ring)
		return true;
	return other->target == myId; // the leash exception (OURS)
}

// 14059ee50 ATTACK_ENEMIES_OF_MY_SLAVEMASTER 41 (dt24_41; task raid-goals): a slave (state 1) takes the enemies of its slavemaster. Like
// 16 this is the resolver's admission of one candidate `id`, not a pick: the shared picker chooses. Gates modelled: the slave state (the
// exe's 1405c8aa0 != 1) and a captor (the exe's captor hand; ours is Fighter::captorFaction, the faction the cuff wrote - UNSURE, the
// exe's master is a character); the platoon test (dt24_16's: an active platoon, not mine; ours is the squad id, and a candidate with
// none is not admitted - UNSURE); the excludes 0x4208 (down, in an object, incapacitated); not a fellow slave of the same captor (the
// exe compares the two captor serials, ours the captor factions - UNSURE). The relevance / admissibility pair (140598630 / 140599f30) is
// not decoded; dt24_41's port note says "enemy of my slavemaster", so the candidate must be an enemy of the MASTER's faction (a copy of
// me with that faction, my personal tags cleared), not only of mine. The include mask 0x1000 (SF_JOB_TEST) has no equivalent here: the
// sensor list is not filtered by it (OURS).
template <typename Lookup>
bool masterEnemies41(const state::Fighter& self, const sensor::Context& ctx, std::uint64_t id, Lookup&& lookup, std::uint64_t myId)
{
	if (self.slaveState != 1 || self.captorFaction == 0)
		return false;
	const state::SightRecord* record = perception::find(self, id);
	const state::Fighter* other = lookup(id);
	if (record == nullptr || other == nullptr || other->dead || id == myId)
		return false;
	if (other->squad == 0 || other->squad == self.squad)
		return false; // the platoon test: an active platoon other than mine
	const std::uint32_t bits = sensor::flags(ctx, self, *other, *record);
	if ((bits & (sensor::kDowned | sensor::kInObject | sensor::kIncapacitated)) != 0)
		return false;
	if (other->slaveState == 1 && other->captorFaction == self.captorFaction)
		return false; // a fellow slave of the same master
	state::Fighter master = self;
	master.faction = self.captorFaction;
	master.hostileSquads.clear();
	master.aggressorSquads.clear();
	return sensor::enemy(ctx, master, *other);
}

// 140793eb0 DEFEAT_SQUAD 101 / 248 (dt24_101 section 2): the squad is the platoon of the current target object; the resolver buffers every
// member I sense, that is relevant (140599f30, not decoded), not in a bed and standing (down state 0), and the shared picker chooses.
// Ours: the target object is my current target's squad (the exe holds an object hand; UNSURE), and a candidate is any other
// fighter of that squad that is not my own squad's, sensed (a sight record), not in a bed/cage, not downed. No hostility test:
// the exe has none here.
template <typename Lookup>
bool defeatSquad101(const state::Fighter& self, const sensor::Context& ctx, std::uint64_t id, Lookup&& lookup, std::uint64_t myId)
{
	if (self.target == 0 || id == myId)
		return false;
	const state::Fighter* held = lookup(self.target);
	const state::Fighter* other = lookup(id);
	const state::SightRecord* record = perception::find(self, id);
	if (held == nullptr || other == nullptr || record == nullptr || other->dead)
		return false;
	if (held->squad == 0 || other->squad != held->squad || other->squad == self.squad)
		return false;
	return (sensor::flags(ctx, self, *other, *record) & (sensor::kDowned | sensor::kInObject)) == 0;
}

// 172 SLAVE_GET_IN_MY_MASTERS_CAGE (dt24_172 section 1): a FREE cage of the CAPTOR's faction. The exe keeps the package hand when it
// resolves, else runs the cage finder over the captor's faction and takes its first result. OURS: the captor is our faction-level
// captorSid (Fighter::captorFaction), not the exe's character/building hand, and the pick is the nearest such cage (the finder's own
// order is not decoded); a body in the cage (no free slot) is not a target. False when the captor has no free cage here.
inline bool slaveCage172(const state::Fighter& self, std::span<const Cage> cages, routines::Site& site)
{
	if (self.captorFaction == 0 || self.cageBuilding != 0)
		return false;
	double best = 1.0e30;
	const Cage* found = nullptr;
	for (const Cage& cage : cages)
	{
		if (cage.faction != self.captorFaction || !cage.freeSlot || cage.claimed || cage.blocked)
			continue;
		const double dx = cage.x - self.x;
		const double dz = cage.z - self.z;
		const double d2 = dx * dx + dz * dz;
		if (found == nullptr || d2 < best)
		{
			best = d2;
			found = &cage;
		}
	}
	if (found == nullptr)
		return false;
	site = found->site;
	return true;
}

// 141 FOLLOW_SQUADLEADER / 171 FOLLOW_SLAVEMASTER (dt24_141 section 2, vf6 14034bac0): one class, so one resolver. Who I follow is the
// character in my captor hand if there is one, else the leader of my platoon (140792b90: the leader, or the first live member when it is
// absent or down). OURS: a slave's captor hand is the character it was taken out of the cage by - its followTarget while slaveState is 1
// (the exe's hand is the captor's serial). Gates: the registry's IS_CROUCHING_OR_LYING = 0 (26); the two accessibility atoms (59 / 60) and
// the fortification one (90) are not modelled, so they hold (UNSURE). `members` is the platoon's member Ids in Id order.
template <typename Lookup>
std::uint64_t follow141(const state::Fighter& self, std::uint64_t selfId, Lookup&& lookup, std::uint64_t leaderId,
	std::span<const std::uint64_t> members)
{
	if (self.dead || self.unconscious || self.down || self.crouched || self.asleep)
		return 0;
	const auto live = [&](std::uint64_t id)
	{
		const state::Fighter* other = id != 0 && id != selfId ? lookup(id) : nullptr;
		return other != nullptr && !other->dead && !other->unconscious && !other->down;
	};
	if (self.slaveState == 1 && live(self.followTarget))
		return self.followTarget;
	if (live(leaderId))
		return leaderId;
	for (const std::uint64_t id : members)
		if (live(id))
			return id;
	return 0;
}

// 173 GATHER_SLAVES_FROM_CAGES (dt15 section 5): the nearest occupied cage (my town, my faction; the caller's list), its occupant.
inline std::uint64_t gather173(const state::Fighter& self, std::span<const Cage> cages)
{
	std::uint64_t found = 0;
	double best = 1.0e30;
	for (const Cage& cage : cages)
	{
		if (cage.occupant == 0 || cage.claimed || cage.blocked)
			continue;
		const double dx = cage.x - self.x;
		const double dz = cage.z - self.z;
		const double d2 = dx * dx + dz * dz;
		if (d2 < best)
		{
			best = d2;
			found = cage.occupant;
		}
	}
	return found;
}

// 217 GATHER_SLAVES_FROM_CAGES_IF_ITS_AN_EXPORT_TOWN (dt27 section 6.2; resolver 1408cc600): 173's cage gather over MY town's cages with
// the occupant loop's filter - the occupant must resolve and stand (1405c9e10 "not standing": dead, downed, carried or a state >= 3 drops
// the cage). The "export town" of the name has no code or data behind it (sections 6.4 / 6.5): the town's contents are the qualifier.
// Not modelled (UNSURE): the holder-slot gate (`*(c vf+0x58 + 0x250) == 0`), the +0x5b8 byte of vf+0x360 and the package veto (1402690e0).
// The nearest qualifying cage's occupant; the exe takes the finder's first result.
template <typename Lookup>
std::uint64_t gatherExport217(const state::Fighter& self, int myTown, std::span<const Cage> cages, Lookup&& lookup)
{
	if (myTown < 0)
		return 0; // no town, no finder to run over
	std::uint64_t found = 0;
	double best = 1.0e30;
	for (const Cage& cage : cages)
	{
		if (cage.occupant == 0 || cage.claimed || cage.blocked || cage.town != myTown)
			continue;
		const state::Fighter* occupant = lookup(cage.occupant);
		if (occupant == nullptr || occupant->dead || occupant->unconscious || occupant->down || occupant->carriedBy != 0)
			continue;
		const double dx = cage.x - self.x;
		const double dz = cage.z - self.z;
		const double d2 = dx * dx + dz * dz;
		if (d2 < best)
		{
			best = d2;
			found = cage.occupant;
		}
	}
	return found;
}

// 245 GATHER_SLAVES_FROM_CAGES_IF_FEMALE_OR_BEAST (dt24_131 section 7; resolver 1408d1410): the cages of MY town, an occupant accepted when it
// is female ("sex female") or its RaceData field +0x120 differs from mine - read as a different race (UNSURE: the field is undecoded, the
// name says beast). The same standing-occupant filter as 217; the exe never consults the package, ours keeps the claimed / blocked cage
// out so two members do not take one. Female is Fighter::look.female (set for creator characters only: a spawned one has no stored sex,
// UNSURE - so for them the race test is the live half). The nearest qualifying cage's occupant (the exe takes the finder's first result).
template <typename Lookup>
std::uint64_t gatherFemaleOrBeast245(const state::Fighter& self, int myTown, std::span<const Cage> cages, Lookup&& lookup)
{
	if (myTown < 0)
		return 0;
	std::uint64_t found = 0;
	double best = 1.0e30;
	for (const Cage& cage : cages)
	{
		if (cage.occupant == 0 || cage.claimed || cage.blocked || cage.town != myTown)
			continue;
		const state::Fighter* occupant = lookup(cage.occupant);
		if (occupant == nullptr || occupant->dead || occupant->unconscious || occupant->down || occupant->carriedBy != 0)
			continue;
		if (occupant->look.female == 0 && occupant->raceSid == self.raceSid)
			continue;
		const double dx = cage.x - self.x;
		const double dz = cage.z - self.z;
		const double d2 = dx * dx + dz * dz;
		if (d2 < best)
		{
			best = d2;
			found = cage.occupant;
		}
	}
	return found;
}

// A building of designation 10 (BD_SLAVE_STORAGE, SQUAD_TEMPLATE `building designation`), reduced to what 218's finder reads.
struct SlaveStorage
{
	std::uint64_t building = 0;
	std::uint64_t keeper = 0; // the building's occupant character (a resident whose home it is); 0 = none
	double x = 0.0;
	double z = 0.0;
	int town = -1; // the town circle the building stands in
	bool allied = false; // the owner is my faction (or I have none), or bonded to it
};

// 194 / 218 GIVE_ALL_MY_SLAVES_TO(_IF_ITS_AN_IMPORT_TOWN) (dt27 section 6.3; resolver 1408ca950): the nearest allied slave-storage building
// of MY town (BuildingFinderNearestAllies_Designated, designation 10), and its occupant character as the target. A building with no
// occupant to resolve gives no target (the exe returns 0). The "import town" of the name is again only the town's contents.
inline std::uint64_t giveSlaves218(const state::Fighter& self, int myTown, std::span<const SlaveStorage> storages)
{
	if (myTown < 0)
		return 0;
	std::uint64_t found = 0;
	double best = 1.0e30;
	for (const SlaveStorage& storage : storages)
	{
		if (storage.town != myTown || !storage.allied || storage.keeper == 0)
			continue;
		const double dx = storage.x - self.x;
		const double dz = storage.z - self.z;
		const double d2 = dx * dx + dz * dz;
		if (d2 < best)
		{
			best = d2;
			found = storage.keeper;
		}
	}
	return found;
}

// 1408cf140 LIBERATE_ALL_THE_PRISONERS 276 (dt24_276; task raid-goals): an occupied cage in MY TOWN whose captive is neither mine nor
// protected by my AI package. The finder is BuildingFinder_CagesToLiberate (mask 0x100 - the decode's only "is this a cage I can open"
// test, so no lock test here); it runs over my town, so a cage of another town is not a candidate (the caller supplies `cage.town`).
// The exe takes the first hit of the finder; ours is the caller's list order (UNSURE, the same caveat as 177/223). `protects(occupant)`
// is the exe's 1402690e0 "my package protects this prisoner" veto (the port note: freeing every foreign prisoner breaks the slave
// caravans); ours is the same squad or a bond, the caller's test (UNSURE).
// The finder's accept gates of rwh.100 (vf0): the cage owner's relation to my faction below 50 (`Cage::ownerAllied`), and `hated(occupantId)` - the
// long-term SubjectiveTags mask 0x2b0 (STOLE_FROM_ME, MY_CAPTOR, DEFEATED_MY_SQUAD_ONCE, KILLED_MY_FRIEND, the dialogue memory's kHateTags) of my squad's
// table about the prisoner is clear. Not modelled, UNSURE: the AI blocked-cage map (no cage-keyed set exists here; `Cage::blocked` is never filled) and
// "the liberator does not treat the prisoner as an enemy" (vf+0x3e8, by use only).
template <typename Lookup, typename Protects, typename Hated>
std::uint64_t liberate276(const state::Fighter& self, int myTown, std::span<const Cage> cages, Lookup&& lookup, Protects&& protects, Hated&& hated)
{
	if (myTown < 0)
		return 0; // no town, no finder to run over
	for (const Cage& cage : cages)
	{
		if (cage.occupant == 0 || cage.town != myTown || cage.ownerAllied)
			continue;
		const state::Fighter* occupant = lookup(cage.occupant);
		if (occupant == nullptr || occupant->dead)
			continue;
		if (hated(cage.occupant))
			continue; // a long-term hate tag (mask 0x2b0) on it in my squad's table
		if (occupant->faction != 0 && occupant->faction == self.faction)
			continue; // mine (the exe's c vf+0x58 != myFaction test)
		if (protects(cage.occupant, *occupant))
			continue; // my package protects it
		return cage.occupant;
	}
	return 0;
}

// 135 KILL_A_RANDOM_CAGE_OCCUPANT (dt27 section 3; resolver 1408d0d90, dt24_killprisoner section 7): the package's sticky cage while it
// still holds an occupant (returned without a race test), else the ranked cage pick (1408d0770: the 112 scoring over the cages my
// faction or home owns, skipping the ones a squadmate already works) and its occupant, accepted unless the race is inedible-or-robot
// (RaceData+0x7c, read as the robot flag - UNSURE). The exe's pick ranks cages whether or not they hold anyone and hands an empty one
// to the plan, whose goal then holds already; ours ranks the occupied cages only (UNSURE). `occupant` is 0 when nothing is offered;
// `building`/`slot` are the cage the caller stores as the new sticky hand (0 = forget it).
struct KillCagePick
{
	std::uint64_t occupant = 0;
	std::uint64_t building = 0;
	std::uint16_t slot = 0;
};

template <typename Lookup, typename Claimed, typename Edible>
KillCagePick killCage135(const state::Fighter& self, std::span<const Cage> cages, std::uint64_t stickyBuilding, std::uint16_t stickySlot,
	Lookup&& lookup, Claimed&& claimed, Edible&& edible)
{
	const auto live = [&](const Cage& cage) -> const state::Fighter*
	{
		const state::Fighter* occupant = cage.occupant != 0 ? lookup(cage.occupant) : nullptr;
		return occupant != nullptr && !occupant->dead ? occupant : nullptr;
	};
	if (stickyBuilding != 0)
		for (const Cage& cage : cages)
			if (cage.building == stickyBuilding && cage.site.slot == stickySlot && live(cage) != nullptr)
				return {cage.occupant, cage.building, cage.site.slot};
	double best = -1.0;
	const Cage* found = nullptr;
	for (const Cage& cage : cages)
	{
		if (cage.blocked || cage.claimed || live(cage) == nullptr || claimed(cage.occupant))
			continue;
		if (!cage.myFaction && !cage.myHome)
			continue; // the exe's ownership test (140621230), ours the faction / home reading of 112
		const double f = cageScore(self, cage);
		if (f > best)
		{
			best = f;
			found = &cage;
		}
	}
	if (found == nullptr || !edible(*live(*found)))
		return {};
	return {found->occupant, found->building, found->site.slot};
}

// 230 EAT_A_RANDOM_DEAD_BODY (dt27 section 2.1; resolver 1408ce170): the nearest sensed corpse (SF_DEAD) that nobody carries and my
// package does not protect, within `sight`. The race test is not the finder's - it is the executor's, once (dt27 section 2.2).
// `sight` is DAT_14169bc38, unread (UNSURE): the caller passes the 100 u of the sibling body finders.
template <typename Lookup, typename Protects>
std::uint64_t eatDeadBody230(const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup, Protects&& protects, double sight)
{
	std::uint64_t found = 0;
	double best = sight * sight;
	sensor::sensed(self, sensor::kDead, 0, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& body, std::uint32_t)
		{
			if (!body.dead || body.carriedBy != 0 || protects(id, body))
				return;
			const double dx = body.x - self.x;
			const double dz = body.z - self.z;
			const double d2 = dx * dx + dz * dz;
			if (d2 <= best)
			{
				best = d2;
				found = id;
			}
		});
	return found;
}

// 233 EAT_A_RANDOM_KO_BODY (finder 140039c0c, dt39 section 5): 230's search over the living knocked-out instead of the dead.
template <typename Lookup, typename Protects>
std::uint64_t eatKoBody233(const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup, Protects&& protects, double sight)
{
	std::uint64_t found = 0;
	double best = sight * sight;
	sensor::sensed(self, sensor::kIncapacitated, 0, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& body, std::uint32_t)
		{
			if (body.dead || !body.unconscious || body.carriedBy != 0 || protects(id, body))
				return;
			const double dx = body.x - self.x;
			const double dz = body.z - self.z;
			const double d2 = dx * dx + dz * dz;
			if (d2 <= best)
			{
				best = d2;
				found = id;
			}
		});
	return found;
}

// 1405a3f60 ASSAULT_FORTIFICATIONS_PREFER_GATES 270 (dt24_270; task raid-goals). The exe's order is (1) the shared assault resolver
// 1405a28e0 - goal 122's OPEN_FORTIFICATION_GATE (dt24_84 section 4): the fortification query at my position with my town's territory
// id hands back a gate's door ("prefer gates"), (2) my current building target while it still stands and is not reserved, (3) the
// nearest wall (BuildingFinder_WallsToSmash, mask 0x100000). Step 3 is NOT modelled: world building damage does not exist (the sim's
// damageBuilding is the player buildings' slice), so with no gate the goal scores 0 (UNSURE; FEATURES "Raids"). Step 1's query is
// ours as "the nearest gate of my town" - the query itself is a zone tree (UNSURE). `current` is the door key the member holds now.
struct Fortification
{
	std::uint64_t door = 0; // the gate's door key
	std::uint64_t building = 0; // the gate placement
	double x = 0.0;
	double z = 0.0;
	int town = -1; // the town circle the gate belongs to (-1 none)
};

inline std::uint64_t assaultFortifications270(const state::Fighter& self, int myTown, std::span<const Fortification> gates,
	std::uint64_t current)
{
	if (myTown < 0)
		return 0; // the exe's "my Town == 0" gate
	std::uint64_t best = 0;
	double bestD2 = 0.0;
	for (const Fortification& gate : gates)
	{
		if (gate.town != myTown)
			continue;
		const double dx = gate.x - self.x;
		const double dz = gate.z - self.z;
		const double d2 = dx * dx + dz * dz;
		if (best == 0 || d2 < bestD2)
		{
			best = gate.door;
			bestD2 = d2;
		}
	}
	if (best != 0)
		return best; // the shared assault resolver's pick, first: the gate
	for (const Fortification& gate : gates)
		if (gate.door == current && gate.town == myTown)
			return gate.door; // the current building target, while it still stands
	return 0; // the wall-smash finder is not modelled (UNSURE)
}

// 36 PATROL_TOWN's walk (dt24_patrol_town_pot sections 1-2, kinspect 2026-11-08): the package's target is the platoon's TOWN, fixed
// for the package's life; each leg is a uniform random point in a square of half-side radius/2 around its centre, where the move
// order's radius is max(500, townRadius * 0.5) - so +/- townRadius/4 of the town centre for a town above the floor. With no town
// target the order is issued at the character's own position with radius 1250 (+/- 625 u). A new spot is asked for on the first idle
// tick after arriving or failing, not on a timer: the callers re-draw whenever the member stands still.
inline constexpr double kPatrolNoTownRadius = 1250.0; // DAT_1416f190c
inline constexpr double kPatrolOrderFloor = 500.0; // DAT_14168dcf4

struct PatrolSpot
{
	double x = 0.0;
	double z = 0.0;
};

inline PatrolSpot patrolSpot36(double selfX, double selfZ, bool hasTown, double townX, double townZ, double townRadius, double rollX,
	double rollZ) noexcept
{
	const double radius = hasTown ? std::max(kPatrolOrderFloor, townRadius * 0.5) : kPatrolNoTownRadius;
	const double cx = hasTown ? townX : selfX;
	const double cz = hasTown ? townZ : selfZ;
	return {cx + (rollX - 0.5) * radius, cz + (rollZ - 0.5) * radius};
}

// A building in the member's home town or the town it stands in, reduced to what the 153 / 239 / 106 bed resolver reads (dt29 sections
// 0-4). The caller passes only allied or own buildings (the finders' base accept); `beds` is the count of its function-6 slots.
struct BedBuilding
{
	std::uint64_t key = 0;
	std::int32_t designation = 0; // SQUAD_TEMPLATE `building designation`: 2 armoury / barracks, 3 bar
	bool own = false; // the building's faction is the member's (the score's 0.5 against the allied -0.01)
	std::int32_t beds = 0;
	double x = 0.0; // the building's centre: the finders' squared-distance argmin
	double z = 0.0;
};

inline constexpr int kBedSkipDesignation2Class = 4; // dt29 section 2 "npc_class != 4" (it calls it trader; the enum's 4 is the civilian, UNSURE)

// 1408cfe00 BED, the shared resolver of 153 GO_HOME_AND_GO_TO_BED / 239 / 106 FIND_BED_AND_PUT_IN (dt29 sections 0, 2, 3): the first
// source with a function-6 bed wins, and only that building is offered - a full one ends the pick, it does not fall to the next rung.
//   player faction: every candidate building (my town + my home), scored; the best first (section 4), and only when none has a
//                   bed the NPC order below;
//   NPC: the home building, else the nearest (squared distance, the first wins a tie) allied designation-2 building (not for
//        npc_class 4), else the nearest allied designation-3 building (not for npc_class 1 / 2 / 5 / 6), else the nearest allied
//        building with a bed. A rung whose building has no bed hands over to the next.
// Returns the building keys in pick order (at most one for the NPC order). UNSURE: the distance point (the exe's finder position is
// the character's, the building's is its centre here), the allied direction (the owner's relation to the member), and section 4's
// three multipliers (+0x28 > 1.0 read as "more than one bed", the interior hand always set for a building, vf+0x118 unread = x1).
inline std::vector<std::uint64_t> bedLadder153(std::uint64_t homeKey, int homeBeds, int npcClass, bool playerFaction,
	std::span<const BedBuilding> buildings, double selfX, double selfZ)
{
	if (playerFaction)
	{
		std::vector<std::pair<double, std::uint64_t>> scored;
		const auto score = [](const BedBuilding& building)
		{
			double value = building.own ? 0.5 : -0.01;
			if (building.beds > 1)
				value *= 3.0;
			return value * 2.0; // the interior hand of a building is never empty
		};
		for (const BedBuilding& building : buildings)
			if (building.beds > 0)
				scored.push_back({score(building), building.key});
		if (homeKey != 0 && homeBeds > 0 && std::none_of(scored.begin(), scored.end(), [&](const auto& s) { return s.second == homeKey; }))
			scored.push_back({0.5 * (homeBeds > 1 ? 3.0 : 1.0) * 2.0, homeKey});
		std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		std::vector<std::uint64_t> order;
		for (const auto& [value, key] : scored)
			order.push_back(key);
		if (!order.empty())
			return order;
	}
	if (homeKey != 0 && homeBeds > 0)
		return {homeKey};
	const auto rung = [&](auto&& accept) -> std::uint64_t
	{
		const BedBuilding* best = nullptr;
		double bestD = 0.0;
		for (const BedBuilding& building : buildings)
		{
			if (!accept(building))
				continue;
			const double d = (building.x - selfX) * (building.x - selfX) + (building.z - selfZ) * (building.z - selfZ);
			if (best == nullptr || d < bestD)
			{
				best = &building;
				bestD = d;
			}
		}
		return best != nullptr && best->beds > 0 ? best->key : 0;
	};
	std::uint64_t pick = 0;
	if (npcClass != kBedSkipDesignation2Class)
		pick = rung([](const BedBuilding& building) { return building.designation == 2; });
	if (pick == 0 && npcClass != 1 && npcClass != 2 && npcClass != 5 && npcClass != 6)
		pick = rung([](const BedBuilding& building) { return building.designation == 3; });
	if (pick == 0)
		pick = rung([](const BedBuilding& building) { return building.beds > 0; });
	return pick != 0 ? std::vector<std::uint64_t>{pick} : std::vector<std::uint64_t>{};
}

// A building with an interior in the member's current town, reduced to what 216's picker reads (task patrol).
struct TownBuilding
{
	std::uint64_t key = 0;
};

// 275 SIT_AROUND (dt24_275): a "special function" building of my faction in my town, the current seat kept when one is set (the
// out hand's type 0xb -> 1.0). OURS: the candidate list is the town's interior buildings (the exe's BuildingFinder_SpecialFunction
// mask is not decoded), the package veto 1402690e0 and the day/night mask switch 140856bc0 are not modelled (UNSURE), and the pick
// is the nearest standing building rather than a seat; 0 = no building in reach of my town.
struct SitBuilding
{
	std::uint64_t key = 0;
	double x = 0.0;
	double z = 0.0;
};

inline std::uint64_t sitAround275(std::span<const SitBuilding> buildings, const state::Fighter& self) noexcept
{
	std::uint64_t found = 0;
	double best = 0.0;
	for (const SitBuilding& building : buildings)
	{
		const double dx = building.x - self.x;
		const double dz = building.z - self.z;
		const double d2 = dx * dx + dz * dz;
		if (found == 0 || d2 < best)
		{
			best = d2;
			found = building.key;
		}
	}
	return found;
}

// 216 RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE (dt24_36 section 3; the exe's resolver 1408cb510). The exe hands every member the FIRST
// building with an interior in the town, so a party searches building after building in single file. The user's ruling (2026-11-07):
// ours is per character, a deliberate difference (FEATURES). The member takes a seeded random one of `buildings` that no squadmate
// searched in the last `memoryTicks`, else the one searched longest ago (ties: list order). 0 = no building at all.
// `searched` is the union of the squad's members' Routine::searched marks; `seed` is the world seed and `diceKey` the member's stable
// identity (Fighter::diceSeed, else its Id), so the pick is a pure function of the saved state.
inline std::uint64_t runAroundTown216(std::span<const TownBuilding> buildings, std::span<const state::RoutineSearch> searched,
	std::uint64_t seed, std::uint64_t diceKey, std::uint64_t tick, std::uint64_t memoryTicks) noexcept
{
	if (buildings.empty())
		return 0;
	// The squad's newest mark per building.
	const auto newest = [&](std::uint64_t key, std::uint64_t& at) -> bool
	{
		bool found = false;
		for (const state::RoutineSearch& mark : searched)
			if (mark.building == key && (!found || mark.tick > at))
			{
				at = mark.tick;
				found = true;
			}
		return found;
	};
	std::vector<std::uint64_t> fresh;
	std::uint64_t oldestKey = buildings.front().key;
	std::uint64_t oldestTick = 0;
	bool oldestSet = false;
	for (const TownBuilding& building : buildings)
	{
		std::uint64_t at = 0;
		const bool marked = newest(building.key, at);
		if (!marked || (tick > at && tick - at >= memoryTicks))
			fresh.push_back(building.key);
		if (marked && (!oldestSet || at < oldestTick))
		{
			oldestKey = building.key;
			oldestTick = at;
			oldestSet = true;
		}
	}
	if (fresh.empty())
		return oldestKey;
	// The stable dice: a pure function of the world seed, the member's dice key and the tick (nothing is saved but the pick).
	RngStream dice(streamSeed(streamSeed(streamSeed(seed, kSearchStream), static_cast<std::size_t>(diceKey)), static_cast<std::size_t>(tick)));
	return fresh[dice.next() % fresh.size()];
}

// One building of 216's search route, reduced to what the route reads (task patrol): its footprint on the plane, the ground height
// and the door the points keep off, and its lowest upper floor (none = every height is the ground floor).
struct SearchBuilding
{
	std::uint64_t key = 0;
	double cx = 0.0;
	double cz = 0.0;
	double axisX = 1.0;
	double axisZ = 0.0;
	double halfA = 0.0;
	double halfB = 0.0;
	double groundY = 0.0;
	double doorX = 0.0;
	double doorZ = 0.0;
	bool upperFloors = false;
	double firstFloor = 0.0;
	// The user's ruling: the ground floor only by default (hiding upstairs or on a roof is a stealth tactic, so searching them is
	// gameplay). A mod / Lua hook sets this to search every floor, walkable roofs included.
	bool allFloors = false;
};

// A point of the route: its xz and, when the caller's mesh knows one, the walking surface's height.
struct SearchSpot
{
	double x = 0.0;
	double z = 0.0;
	bool hasY = false;
	double y = 0.0;
};

// The route's constants (OURS, the user's ruling 2026-11-07): 2-4 points per building, each this far off the door and this far from
// the point before, at least this far off the walls.
inline constexpr int kSearchPointsMin = 2;
inline constexpr int kSearchPointsMax = 4;
inline constexpr double kSearchDoorClear = 8.0;
inline constexpr double kSearchGap = 6.0;
inline constexpr double kSearchEdge = 1.5;
inline constexpr double kSearchSnapReach = 40.0;
// The look-around at each point (3 s) and the squad's memory of the buildings it searched ("a few minutes", 30 game minutes).
inline constexpr std::uint64_t kSearchLookTicks = 60;
inline constexpr std::uint64_t kSearchMemoryTicks = kTicksPerGameHour / 2;
inline constexpr std::size_t kSearchMarks = 8; // the marks one member keeps; the squad reads the union over its members

// The route's length, 2 to 4, a pure function of (seed, the member's dice key, the building).
inline int searchCount216(std::uint64_t seed, std::uint64_t diceKey, std::uint64_t building) noexcept
{
	RngStream length(streamSeed(streamSeed(streamSeed(seed, kSearchStream), static_cast<std::size_t>(diceKey)), static_cast<std::size_t>(building)));
	return kSearchPointsMin + static_cast<int>(length.next() % static_cast<std::uint64_t>(kSearchPointsMax - kSearchPointsMin + 1));
}

// The route through one building (task patrol): 2-4 points on its GROUND floor (the user's ruling: searching the upper floors is a
// stealth tactic, so they take an explicit parameter elsewhere), each off the door and off the point before. `walkable(x, z, spot)`
// puts a sample on the walking mesh (false = no walkable floor there); a point no sample passes falls back to the door side, which
// always stands. Every draw is a pure function of (seed, the member's dice key, building, index).
template <typename Walkable>
std::vector<SearchSpot> searchRoute216(const SearchBuilding& building, std::uint64_t seed, std::uint64_t diceKey, Walkable&& walkable)
{
	std::vector<SearchSpot> route;
	const int count = searchCount216(seed, diceKey, building.key);
	for (int step = 0; step < count; ++step)
	{
		RngStream dice(streamSeed(streamSeed(streamSeed(seed, kSearchStream), static_cast<std::size_t>(diceKey)),
			static_cast<std::size_t>(building.key ^ (0x9E3779B97F4A7C15ull * static_cast<std::uint64_t>(step + 1)))));
		bool found = false;
		SearchSpot spot;
		for (int attempt = 0; attempt < 16 && !found; ++attempt)
		{
			const double u = (2.0 * dice.uniform01() - 1.0) * std::max(0.0, building.halfA - kSearchEdge);
			const double v = (2.0 * dice.uniform01() - 1.0) * std::max(0.0, building.halfB - kSearchEdge);
			const double x = building.cx + u * building.axisX - v * building.axisZ;
			const double z = building.cz + u * building.axisZ + v * building.axisX;
			if (std::hypot(x - building.doorX, z - building.doorZ) < kSearchDoorClear)
				continue;
			if (!route.empty() && std::hypot(x - route.back().x, z - route.back().z) < kSearchGap)
				continue;
			if (!walkable(x, z, spot))
				continue;
			if (!building.allFloors && building.upperFloors && spot.hasY && spot.y >= building.firstFloor - 1.0)
				continue; // the ground floor only (allFloors: the mod hook)
			found = true;
		}
		if (!found)
			spot = SearchSpot{building.doorX, building.doorZ, false, 0.0};
		route.push_back(spot);
	}
	return route;
}

// ---- 58 JOB_MEDIC (task CG; kinspect dt24_58.md, dt16_resolvers.md section 6) ----

// The kit a first aid goal can use (dt24 step 0): item function 1 treats the flesh injury category, function 12 (the exe's flag 0xc)
// the robotic one. `kit(id, function)` answers for any character: a kit carried, or one already in use (Fighter::kitCharges).
struct Kits
{
	bool flesh = false;
	bool robot = false;
	bool any() const noexcept { return flesh || robot; }
};

// The medic's kit flags: its own equipment first, then (dt24 step 0) the carried body's item when the medic has none of its own.
template <typename Kit>
Kits kitsOf(std::uint64_t selfId, const state::Fighter& self, Kit&& kit)
{
	Kits kits;
	kits.flesh = kit(selfId, kItemFunctionMedkit);
	kits.robot = kit(selfId, kItemFunctionRobotKit);
	if ((!kits.flesh || !kits.robot) && self.carriedBy != 0)
	{
		kits.flesh = kits.flesh || kit(self.carriedBy, kItemFunctionMedkit);
		kits.robot = kits.robot || kit(self.carriedBy, kItemFunctionRobotKit);
	}
	return kits;
}

// A player-owned medic counts only the categories its kit flags allow; every other medic counts both (dt24 step 3). OURS: the
// exe's "or my squad's / carried" exemption for a player's character is not modelled (a player-owned medic of a world squad is rare).
inline bool bothCategories(const state::Fighter& self) noexcept
{
	return !self.playerControlled;
}

// A part healed to the bit leaves a rounding residue in (max - wear) - health - pool (~1e-11, task CG: nine medics kept a healed
// patient a target forever and nobody could lift it); under this the part counts as whole, the same 1e-9 room test bandage uses.
inline constexpr double kInjuryEpsilon = 1.0e-9;

// The exe sums the medical system's per-category injury (140643ca0, not decoded: dt16 section 7). Ours sums the damage the bandage
// pool has not covered yet on the parts of that category, marked robotic or not. OURS: the wound list is not part of the sum (a cut
// that wounds the body always leaves part damage too).
inline double injuryOf(const state::Fighter& body, bool robotic) noexcept
{
	double sum = 0.0;
	for (const state::FighterPart& part : body.parts)
	{
		if (part.robotic != robotic)
			continue;
		const double room = (part.maxHealth - part.wear) - part.health - part.pool;
		if (room > kInjuryEpsilon)
			sum += room;
	}
	return sum;
}

// The injury the kits can treat, in the categories the medic counts.
inline double treatableInjury(const state::Fighter& body, const Kits& kits, bool both) noexcept
{
	double sum = 0.0;
	if (both || kits.flesh)
		sum += injuryOf(body, false);
	if (both || kits.robot)
		sum += injuryOf(body, true);
	return sum;
}

// Whether `body` still needs first aid at all: the goal's needsFirstAid side (dt24: the goal state is needsFirstAid = 0, so the live
// world holds it while an injury the kits reach is left).
inline bool needsAid(const state::Fighter& body, const Kits& kits, bool both) noexcept
{
	if ((both || kits.flesh) && injuryOf(body, false) > 0.0)
		return true;
	return (both || kits.robot) && injuryOf(body, true) > 0.0;
}

// 269 HEAL_MY_LEGS applies (k16.6.4; dt45_down_treatment.md): a living, down, conscious NPC that nobody carries. The collapse's own
// forced KO (~0.4 s live, `unconscious` too) is over first: the two flags are never merged. A player character is commanded.
inline bool healMyLegs269(const state::Fighter& self) noexcept
{
	return !self.dead && self.down && !self.unconscious && self.carriedBy == 0 && !self.playerControlled;
}

// dt24's distance term (140998b90): the exe re-measures from its mover's published point when d^2 > 1600 and gives 0 when it is
// still beyond (40 u); ours has no separate mover point, so the character's own position is the one distance. Inside the cap the
// curve over d^2 is clamped to <= 1 with a 0.4 floor in the same town. The curve's shape is not decoded (dt16 section 7: monotone
// falloff): OURS 1 - d^2 / 1600.
inline double healDistance(double dSquared, bool sameTown) noexcept
{
	if (dSquared > 1600.0)
		return 0.0;
	const double curve = std::min(1.0, 1.0 - dSquared / 1600.0);
	return std::max(curve, sameTown ? 0.4 : 0.0);
}

// The relation factor 1406b2460 for a foreign target (dt24 step 3): 1.0 for the same faction, else the relation mapped to a factor
// with the 0.05 floor (`DAT_141696c74`), or 0.01 (`DAT_141682ad8`) for a player-faction medic. OURS: the exe's factor curve is not
// decoded; ours maps the relation band linearly.
inline double relationFactor(const sensor::Context& ctx, const state::Fighter& self, const state::Fighter& target) noexcept
{
	if (self.faction == target.faction)
		return 1.0;
	const double relation = ctx.relation ? ctx.relation(self.faction, target.faction) : 0.0;
	double factor = std::min(1.0, (relation + 100.0) / 200.0);
	if (factor < 0.05)
		factor = self.playerControlled ? 0.01 : 0.05;
	return factor;
}

// One goap target and the score the selector uses as the goal's utility (dt16: 58 returns the score itself, not the flat 1.0).
struct MedicPick
{
	std::uint64_t target = 0;
	double score = 0.0;
};

// 58 JOB_MEDIC (dt24; dt16 section 6): the most injured ally the kits can treat, by injury x distance, the medic itself scored too
// (dt24 step 4). Pass 1 is the ally mask 1 / 0x10 (allies, not dead); a character of my faction's slaves counts as an ally there (its
// own faction stays its master's enemy, so the exe's ally test must read the slave bond too - OURS). Per candidate: not dead, not in
// a fight (140436610), not carried by my enemy, the injury category matches a kit, not the member's recently blocked target, and (a
// non-player medic) the record is not a mere memory (dt16 gate 6's seen/heard half). Pass 2 "heals strangers" (mask 0x420a / 0x14)
// needs the character's data flag (*(char+0x240)+0xf8), which our data does not carry: the caller passes `healsStrangers`.
// OURS/UNSURE: a medic with no kit at all is not scored (the exe would pick it and fail at the action), the medical-state and
// permission gates ((t+0x279 & 7), 140644170) are not modelled, and the claim penalty is applied but the claim itself is the
// selector's (it registers a chosen target).
template <typename Lookup, typename Claimed, typename Blocked, typename Kit, typename SameTown>
MedicPick medic58(std::uint64_t selfId, const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup, Claimed&& claimed,
	Blocked&& blocked, Kit&& kit, SameTown&& sameTown, bool healsStrangers)
{
	const Kits kits = kitsOf(selfId, self, kit);
	if (!kits.any())
		return {};
	const bool both = bothCategories(self);
	const auto injury = [&](const state::Fighter& body) { return treatableInjury(body, kits, both); };

	MedicPick best;
	// dt24 step 4: the self case is scored too (its distance term is the curve at 0 = 1).
	const double selfScore = injury(self);
	if (selfScore > 0.0)
		best = {selfId, selfScore};

	const auto pass = [&](std::uint32_t include, std::uint32_t exclude, bool strangers)
	{
		for (const state::SightRecord& record : self.sight)
		{
			const state::Fighter* body = lookup(record.target);
			if (body == nullptr)
				continue;
			const std::uint32_t bits = sensor::flags(ctx, self, *body, record);
			// My faction's slave: an ally for this goal (see above), whatever its own faction's relation to mine.
			const bool mySlave = !strangers && body->captorFaction != 0 && body->captorFaction == self.faction
				&& body->slaveState != 0;
			if (!mySlave && !sensor::matches(bits, include, exclude))
				continue;
			if (body->dead || perception::inCombat(*body) || blocked(record.target))
				continue;
			if (strangers && body->faction != 0)
				continue; // dt24 pass 2: a stranger has no faction
			if (!self.playerControlled && !mySlave && (bits & sensor::kNotSeen) != 0)
				continue; // dt16 gate 6: the record must be seen/heard (our stand-in for "inside my building"); my own slave is known without a sighting (OURS)
			if (body->carriedBy != 0)
			{
				const state::Fighter* carrier = lookup(body->carriedBy);
				if (carrier != nullptr && sensor::enemy(ctx, self, *carrier))
					continue; // dt16 gate 4
			}
			const double need = injury(*body);
			if (need <= 0.0)
				continue;
			const double dx = body->x - self.x;
			const double dz = body->z - self.z;
			double score = need * healDistance(dx * dx + dz * dz, sameTown(record.target, *body));
			if (score <= 0.0)
				continue;
			if (body->faction != self.faction)
				score *= relationFactor(ctx, self, *body);
			if (claimed(record.target))
				score *= 0.05; // dt16 step 4: another squadmate is registered on it
			if (score > best.score)
				best = {record.target, score};
		}
	};
	pass(sensor::kAlly, sensor::kDead, false);
	if (healsStrangers)
		pass(sensor::kNeutral | sensor::kIncapacitated | sensor::kInObject | sensor::kDowned, sensor::kHostile | sensor::kDead, true);
	return best;
}

// -------------------------------------------------------------------------------------------------------------------------------
// 1409997f0 SHOPPING 118 (task CH group 3; kinspect dt24_118 section 2): the town's shops, of which one is picked.
// The exe first takes a shop that stocks an item on the town's shopping list (1409fc070); when none does, the fallback is the shop's
// counter (1409fc1b0: "any counter for this shop"). The goal's hand is that shop's function-9 counter: the resolver scores 1.0 there
// and kShoppingNoCounterScore (0.4) when the target is a shop object with no counter.
//
// dt24_118: "the shop is a random pick among shops ... that sell an item still wanted today"; the exe's 1409fc070 returns a shop
// and the note says random, so the pick among the stocking shops is uniform over `roll` (ours is seeded; the caller's day-keyed stream).
// No stocking shop: the first shop of the town's list is the fallback (OURS: 1409fc1b0's own order is its container's).
struct Shop
{
	std::uint64_t building = 0; // BuildingSite::key
	bool stocksWanted = false; // a stock item's ITEM `item function` is on the member's unmet list
	bool counter = true; // the building has a function-9 counter piece
};
inline std::size_t shopping118(std::span<const Shop> shops, double roll, double* outScore = nullptr)
{
	std::size_t chosen = shops.size();
	std::size_t stocking = 0;
	for (const Shop& shop : shops)
		stocking += shop.stocksWanted ? 1 : 0;
	if (stocking != 0)
	{
		std::size_t pick = std::min<std::size_t>(static_cast<std::size_t>(roll * static_cast<double>(stocking)), stocking - 1);
		for (std::size_t i = 0; i < shops.size(); ++i)
		{
			if (!shops[i].stocksWanted)
				continue;
			if (pick == 0)
			{
				chosen = i;
				break;
			}
			--pick;
		}
	} else if (!shops.empty()) {
		chosen = 0; // the counter fallback: any shop of the town
	}
	if (outScore != nullptr)
		*outScore = chosen < shops.size() && shops[chosen].counter ? 1.0 : routines::kShoppingNoCounterScore;
	return chosen;
}

// 149 MAN_A_TURRET's distance bands (dt24_turret_finder section 1, resolver 1408ccc70): the finder's turret at `d2` squared units from
// the member. Engaged (the combat-class flag, perception::inCombat) 0.8 within 600 u and 0.05 beyond; not engaged 1.0 within 1500 u
// and 0.3 beyond. The exe's constants are the squared distances 360000 and 2250000.
inline double turretBand149(double d2, bool engaged) noexcept
{
	if (engaged)
		return d2 < 360000.0 ? 0.8 : 0.05;
	return d2 < 2250000.0 ? 1.0 : 0.3;
}

// ---- the animal and food rows (task port-animals; kinspect dt24_eat, dt24_131, dt24_cage_loot_play) ----

// A loose item as the ground finders read it, reduced by the caller (the GroundItem's Id, where it lies, the ITEM's `item function`).
struct GroundThing
{
	std::uint64_t id = 0;
	double x = 0.0;
	double z = 0.0;
	std::int32_t function = 0;
};

inline constexpr double kGroundFoodReach = 100.0; // 14099a930 / 1409993a0: the 100-unit ground query

// A crop plot as 232's finder reads it, already reduced to edible plots with food left (the caller's test); `town` is the plot's town.
struct CropPick
{
	std::uint64_t id = 0;
	double x = 0.0;
	double z = 0.0;
	std::uint64_t town = 0;
};

// 232 FIND_CROPS_TO_EAT (1408cb810, BuildingFinder_EatCrops): the plots of my town (any town when I have none), the first result; a
// town whose owner my faction is friendly with (or is) yields nothing but the plot I already hold (`held`, 0 = none; dt24_eat keeps a
// current function-0xd building). OURS: "first" is the nearest within `range`. 0 = none.
inline std::uint64_t findCrops232(std::span<const CropPick> plots, double x, double z, std::uint64_t town, bool friendly, std::uint64_t held,
	double range)
{
	if (friendly)
		return held;
	std::uint64_t found = 0;
	double best = range * range;
	for (const CropPick& plot : plots)
	{
		if (town != 0 && plot.town != town)
			continue;
		const double d = (plot.x - x) * (plot.x - x) + (plot.z - z) * (plot.z - z);
		if (d < best)
		{
			best = d;
			found = plot.id;
		}
	}
	return found;
}

// 259 EAT_FOOD_ON_GROUND (14099a930): ground food within 100 u first (14042ee50's race / food test is the caller's `foods`: the items an
// eater of this race takes), then a dead body within 100 u whose main inventory holds food. `onBody` says which. The interior / zone /
// permission gates (140997d80) are not modelled (UNSURE). The nearest of each pass.
template <typename Lookup, typename BodyHasFood>
std::uint64_t foodOnGround259(const state::Fighter& self, std::span<const GroundThing> foods, const sensor::Context& ctx, Lookup&& lookup,
	BodyHasFood&& bodyHasFood, bool& onBody)
{
	onBody = false;
	std::uint64_t found = 0;
	double best = kGroundFoodReach * kGroundFoodReach;
	for (const GroundThing& thing : foods)
	{
		const double d = (thing.x - self.x) * (thing.x - self.x) + (thing.z - self.z) * (thing.z - self.z);
		if (thing.function == kItemFunctionFood && d <= best)
		{
			best = d;
			found = thing.id;
		}
	}
	if (found != 0)
		return found;
	onBody = true;
	sensor::sensed(self, sensor::kDead, 0, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& body, std::uint32_t)
		{
			const double d = (body.x - self.x) * (body.x - self.x) + (body.z - self.z) * (body.z - self.z);
			if (body.carriedBy == 0 && d <= best && bodyHasFood(id))
			{
				best = d;
				found = id;
			}
		});
	if (found == 0)
		onBody = false;
	return found;
}

// 277 ANIMAL_FETCH_A_LIMB (1409993a0): the nearest loose limb (`item function` 0x11) within the 100-unit query, the 200-unit (40000.0
// squared) cap on top. The hunger gate (vf+0x398 against 0.9) and the cooldown (AI+0x318) are the caller's `allowed`: this runs only
// when the animal is not full. 0 = none.
inline std::uint64_t limb277(const state::Fighter& self, std::span<const GroundThing> grounds, bool allowed)
{
	std::uint64_t found = 0;
	double best = kGroundFoodReach * kGroundFoodReach;
	for (const GroundThing& thing : grounds)
	{
		const double d = (thing.x - self.x) * (thing.x - self.x) + (thing.z - self.z) * (thing.z - self.z);
		if (allowed && thing.function == kItemFunctionLimb && d <= best && d <= 40000.0)
		{
			best = d;
			found = thing.id;
		}
	}
	return found;
}

// 279 CHASE_ALLY_DOGS_WITH_MOUTH_LIMBS (14099ad40): the first sensed ally animal (sensor include 1, exclude 0x18) whose data block holds a
// "mouth limb" (1405c82d0): ours, an animal carrying a limb item (`hasLimb`). Id order, like the other sensor finders. 0 = none.
template <typename Lookup, typename HasLimb>
std::uint64_t mouthLimbAlly279(const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup, HasLimb&& hasLimb)
{
	std::uint64_t found = 0;
	sensor::sensed(self, sensor::kAlly, sensor::kIncapacitated | sensor::kDead, ctx, lookup,
		[&](std::uint64_t id, const state::Fighter& other, std::uint32_t)
		{
			if (found == 0 && other.animal && hasLimb(id))
				found = id;
		});
	return found;
}

// 238 LOOT_ANIMALS_JOB (1408ce3a0 + 140998950): a sensed ANIMAL body (include 0x18 = incapacitated | dead, exclude 1 = ally) that is not
// carried and not already a squadmate's target, holding something my storage takes (`wanted`: 14059acd0's test, the caller's). The
// exe's "enemy or player-faction" gate is the sensor's own mask here. The first in list order within 100 u (OURS: the exe's reach is unread).
template <typename Lookup, typename Claimed, typename Wanted>
std::uint64_t animalLoot238(const state::Fighter& self, const sensor::Context& ctx, Lookup&& lookup, Claimed&& claimed, Wanted&& wanted)
{
	return loot177(self, ctx, lookup, claimed, [&](std::uint64_t id, const state::Fighter& body) { return body.animal && wanted(id, body); });
}

// 24 WANDERER (140998040): the town nearest to the town-holder point (140927f10, type filter 10), out = the town's hand. Ours: the nearest circle
// (towncircle.hpp, radius > 0: not a marker) to the member; the exe's filter 10 is unread (UNSURE). Returns the circle's index, -1 = none.
inline int wanderer24(const std::vector<towncircle::Circle>& circles, double x, double z)
{
	int best = -1;
	double nearest = 0.0;
	for (std::size_t i = 0; i < circles.size(); ++i)
	{
		const double d2 = (x - circles[i].x) * (x - circles[i].x) + (z - circles[i].z) * (z - circles[i].z);
		if (circles[i].radius > 0.0 && (best < 0 || d2 < nearest))
		{
			best = static_cast<int>(i);
			nearest = d2;
		}
	}
	return best;
}

} // namespace ksim::worldres
