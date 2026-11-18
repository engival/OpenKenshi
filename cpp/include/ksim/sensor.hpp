#pragma once

// G5, task BU: the original's per-character SENSOR LIST and its record flags (docs/reference/RUNTIME_SYSTEMS.md G5). The exe keeps,
// per AI, a map of records keyed by the seen character (SensoryData at AI+0x28); each record carries a bit field at record+0x30 built by
// 1408580d0 (kinspect s59_static.md, section 1) and a few memory flags. A goal resolver does not scan the world: it asks the iterator
// 140853960 for the records that pass an include mask (ANY bit set) and an exclude mask (NO bit set), then applies per-goal tests and
// takes the nearest (s59, e.g. CAPTURE_NEW_SLAVES 167 with 0x4008 / 0x11). Our record list is perception's state::Fighter::sight
// (perception.hpp), sorted by target Id; the exe iterates its hash map, so the ORDER is ours (UNSURE; the resolvers pick by distance).
//
// The faction bits come from dt11 (FUN_14079c950, isEnemy): same faction = not enemy; a personal type-4 tag = enemy; otherwise MY
// faction's relation toward the TARGET'S (my entry for it, else my default relation) <= -30 = enemy. Task BV fixed the direction from
// dt10's live read (a Dust Bandit reads its own -10, neutral; a Bloodraider its -100, hostile); dt21 confirms it live. A character has
// no faction entity here, so the caller supplies the relation function (Simulation::sensorContext). isAlly (FUN_1406b2630) is "same faction, or
// relation >= 50"; its direction is not pinned (dt11), and ours reads the OBSERVER's relation to the target, the direction
// CombatArena::bonded stores for the 167 resolver (BR2), so the capture decision is unchanged.
//
// OURS / UNSURE: bits 11 (robot) and 12 (job target) are M in kinspect (not decoded) and are never set; the neutral bit's "enemy but
// ignorable" cases (the package's attackable test, in a bed, my own captive) are not modelled; bit 8 ("not currently seen") reads the
// last scan rather than the exe's per-write flag; the dead record carries only bit 4, as the exe builds it (s59 bit 4).

#include "ksim/fighter_state.hpp"

#include <cstdint>
#include <functional>

namespace ksim::sensor {

// The record flags (record+0x30), kinspect s59_static.md section 1.
inline constexpr std::uint32_t kAlly = 1u << 0; // !enemy && isAlly
inline constexpr std::uint32_t kNeutral = 1u << 1; // !enemy && !ally (plus the exe's "enemy but ignorable" cases, not modelled)
inline constexpr std::uint32_t kHostile = 1u << 2; // enemy && the package's attackable test (not modelled: enemy alone)
inline constexpr std::uint32_t kIncapacitated = 1u << 3; // KO'd or carried
inline constexpr std::uint32_t kDead = 1u << 4;
inline constexpr std::uint32_t kSlave = 1u << 5; // slave state 1
inline constexpr std::uint32_t kEscapingSlave = 1u << 6; // slave state 2
inline constexpr std::uint32_t kLawMilitary = 1u << 7; // NPC class 1 or 2
inline constexpr std::uint32_t kNotSeen = 1u << 8; // remembered only: the last scan did not sense it
inline constexpr std::uint32_t kInObject = 1u << 9; // in a bed or a cage
inline constexpr std::uint32_t kSameFaction = 1u << 10;
inline constexpr std::uint32_t kRobot = 1u << 11; // never set (M)
inline constexpr std::uint32_t kJobTarget = 1u << 12; // never set (M)
inline constexpr std::uint32_t kPlayerFaction = 1u << 13;
inline constexpr std::uint32_t kDowned = 1u << 14; // down (crawling) or KO'd, and not fighting
inline constexpr std::uint32_t kCarried = 1u << 15;

// The relation thresholds: the exe's DAT_1416ccd2c and FactionBook::kAlly.
inline constexpr double kEnemyRelation = -30.0;
inline constexpr double kAllyRelation = 50.0;

// What a faction thinks of another (from's relation to to: factions.cpp factionRelation). Empty = no relations: everyone neutral.
using Relation = std::function<double(std::uint64_t fromFaction, std::uint64_t toFaction)>;

// The world inputs the flags read. Built by Simulation::sensorContext; a test stages its own.
struct Context
{
	Relation relation;
	std::uint64_t playerFaction = 0; // the player faction's state::Id (0 = none)
};

// dt11's isEnemy, one way: the observer's faction against the target's. Same faction (including both factionless) = not an enemy; a
// personal type-4 tag = an enemy (the observer's own list, and for a player-faction observer also the target's, dt11 step 2); else the
// target faction's relation toward the observer's (UNSURE: the only reading of dt11's FUN_1406b25d0(faction->relations, other)).
bool enemy(const Context& ctx, const state::Fighter& observer, const state::Fighter& target);
// Not an enemy and the observer's faction counts the target as an ally (same faction, or relation >= 50).
bool ally(const Context& ctx, const state::Fighter& observer, const state::Fighter& target);

// The 16-bit flag word of `observer`'s record of `target` (1408580d0).
std::uint32_t flags(const Context& ctx, const state::Fighter& observer, const state::Fighter& target, const state::SightRecord& record);

// One entry of a sneaking player character's detection map (the stealth arrows, SIMLINK DETECTED_BY; red_arrow_rules.md): `state` 0 =
// none, 2 = the observer sees it and the meter is <= 1 (yellow), 1 = noticed (red); `friendly` = a friendly observer told about it.
struct Detected
{
	std::uint8_t state = 0;
	bool friendly = false;
};
// The record's age limit (DAT_14168bf24): a record not refreshed within 4 s of game time is stale.
inline constexpr double kDetectFresh = 4.0;
// `observer`'s entry for the sneaking `target` from its `record`. The observer must be awake, uncaged, not down or KO'd, not carried and
// not a slave (the exe erases the record on its next update otherwise). A friendly observer holds one only through a force-notice
// (perception::notice), as state 1 with friendly set; sight never writes one for it. Wire filter only: it changes no decision.
Detected detected(const Context& ctx, const state::Fighter& observer, const state::Fighter& target, const state::SightRecord& record);

// The masks' test (140853960): the include mask must share a bit (0 = no include filter, OURS: no exe call seen with 0), the exclude
// mask must not.
bool matches(std::uint32_t bits, std::uint32_t include, std::uint32_t exclude) noexcept;

// The sensor-list iterator: every record of `observer` that passes the masks, resolved through `lookup` (target Id -> character; null
// skips the record). `visit(id, target, bits)`.
template <typename Lookup, typename Visit>
void sensed(const state::Fighter& observer, std::uint32_t include, std::uint32_t exclude, const Context& ctx, Lookup&& lookup,
	Visit&& visit)
{
	for (const state::SightRecord& record : observer.sight)
	{
		const state::Fighter* target = lookup(record.target);
		if (target == nullptr)
			continue;
		const std::uint32_t bits = flags(ctx, observer, *target, record);
		if (matches(bits, include, exclude))
			visit(record.target, *target, bits);
	}
}

} // namespace ksim::sensor
