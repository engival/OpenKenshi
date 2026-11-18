#include "ksim/ai/sensor.hpp"

#include "ksim/law/crime.hpp"
#include "ksim/ai/perception.hpp"

#include <algorithm>

namespace ksim::sensor {

using state::Fighter;

namespace {

// A personal type-4 tag: the squad's personal hostile list (Fighter::hostileSquads, filled by onTargetedBy and kept sorted). dt11's
// 140678960(a, b, 4).
bool personallyHostile(const Fighter& a, std::uint64_t squad) noexcept
{
	return std::binary_search(a.hostileSquads.begin(), a.hostileSquads.end(), squad);
}

} // namespace

bool enemy(const Context& ctx, const Fighter& observer, const Fighter& target)
{
	// dt11 step 1: the same faction object, never an enemy (the factionless share faction 0, as CombatArena::bonded does).
	if (observer.faction == target.faction)
		return false;
	// Step 2: a personal enemy tag; the exe repeats the test the other way for an observer in a player faction.
	if (personallyHostile(observer, target.squad))
		return true;
	if (ctx.playerFaction != 0 && observer.faction == ctx.playerFaction && personallyHostile(target, observer.squad))
		return true;
	// Step 3 (task BV, kinspect dt21, the live trace): an NPC reads its OWN faction's relation toward the target's (its entry, else its
	// default). A player-faction observer does not read its own map: PlayerFactionRelations forwards the lookup to the TARGET faction's
	// relations, keyed by the player faction, so every player <-> NPC pair reads the NPC faction's record toward the player in both
	// orders. Nameless's own -100 entry at the Dust Bandits is never consulted: a Dust Bandit reads its -10 (neutral), a Bloodraider its
	// -100 (hostile), and the player reads the same NPC record back.
	if (!ctx.relation)
		return false;
	const bool playerObservers = ctx.playerFaction != 0 && observer.faction == ctx.playerFaction;
	const std::uint64_t from = playerObservers ? target.faction : observer.faction;
	const std::uint64_t to = playerObservers ? observer.faction : target.faction;
	return ctx.relation(from, to) <= kEnemyRelation;
}

bool ally(const Context& ctx, const Fighter& observer, const Fighter& target)
{
	if (observer.faction == target.faction)
		return true;
	// UNSURE (dt11): isAlly's direction is not pinned; the observer's view is the one CombatArena::bonded stores.
	return ctx.relation ? ctx.relation(observer.faction, target.faction) >= kAllyRelation : false;
}

std::uint32_t flags(const Context& ctx, const Fighter& observer, const Fighter& target, const state::SightRecord& record)
{
	// The exe builds a dead character's record with only the dead bit (s59 bit 4).
	if (target.dead)
		return kDead;

	const bool hostile = enemy(ctx, observer, target);
	const bool allied = !hostile && ally(ctx, observer, target);
	std::uint32_t bits = 0;
	if (hostile)
		bits |= kHostile;
	else
		bits |= allied ? kAlly : kNeutral;

	if (target.unconscious || target.carriedBy != 0)
		bits |= kIncapacitated;
	if (target.slaveState == 1)
		bits |= kSlave;
	else if (target.slaveState == 2)
		bits |= kEscapingSlave;
	if (target.npcClass == crime::kClassLaw || target.npcClass == crime::kClassMilitary)
		bits |= kLawMilitary;
	// Remembered only: the last scan itself did not sense it (the exe's +0x39 sighted flag, s59 bit 8; sinceSensed is reset by a scan
	// that senses the target and is left at its old value otherwise, including after an out-of-scan `notice`).
	if (record.sinceSensed > 0.0)
		bits |= kNotSeen;
	if (target.asleep || target.cageBuilding != 0)
		bits |= kInObject;
	if (observer.faction != 0 && observer.faction == target.faction)
		bits |= kSameFaction;
	if (ctx.playerFaction != 0 && target.faction == ctx.playerFaction)
		bits |= kPlayerFaction;
	// Downed: a ragdoll (a leg or torso collapse) or a KO, not in a fight (s59 bit 14's "not combat-engaged").
	if ((target.down || target.unconscious) && !perception::inCombat(target))
		bits |= kDowned;
	if (target.carriedBy != 0)
		bits |= kCarried;
	return bits;
}

Detected detected(const Context& ctx, const Fighter& observer, const Fighter& target, const state::SightRecord& record)
{
	if (observer.dead || observer.unconscious || observer.down || observer.asleep || observer.cageBuilding != 0 || observer.carriedBy != 0
		|| observer.slaveState == 1)
		return {};
	if (record.state == 0)
		return {};
	if (ally(ctx, observer, target))
		return record.sinceNoticed <= kDetectFresh ? Detected{1, true} : Detected{};
	if (record.state == 1)
		return record.sinceSeen <= kDetectFresh ? Detected{1, false} : Detected{};
	return Detected{2, false};
}

bool matches(std::uint32_t bits, std::uint32_t include, std::uint32_t exclude) noexcept
{
	if (include != 0 && (bits & include) == 0)
		return false;
	return (bits & exclude) == 0;
}

} // namespace ksim::sensor
