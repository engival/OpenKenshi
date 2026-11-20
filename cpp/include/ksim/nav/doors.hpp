#pragma once

// Town gates in the simulation (docs/reference/DOORS.md, ARCHITECTURE.md "Pathfinding": gates
// are operated, with access control). The nav layer (knav/door.hpp) has the mechanism: a DoorBook
// of per-door state, repainted onto the baked door polygons. This
// is the owner of that book: it decides, per town, how its gates stand, from the town's data and
// what the faction layer is doing, and hands the navigator the result.
//
// Measured model (DOORS.md sections 6 and 8; gate doors are doors): nothing in the sim closes a gate. A gate is open unless a
// command shut it (SetDoorState); a closed gate is opened by whoever walks through it unless it is locked; an `always locks`
// gate is locked whenever it is closed. Raiders refused by a locked gate bash it (DoorAccess::bashing) instead of failing
// their order; a broken gate stays open for ever (override state 3). A gate with a crew (a squad with state::GateCrew, spawn.hpp)
// is reopened by it: the crew's UNLOCK (3.33 s, only if the crew's faction owns the door) then OPEN (until openness 0.8), the
// measured "reopen" of GATES_CARRY_LIVE.md a; the full-detail version is gatecrew.hpp.
// Rules:
//   - Only gate doors are operated: a door whose parent BUILDING is `is wall gate` / `is gateway`
//     belongs to the nearest town centre, no cutoff (gatebind.hpp, as the spawn rule). House
//     doors stay as baked (open to all).
//   - Default Open, for ever: the original never closes a gate (the Alert closing of the first slice is retired,
//     retired/doors_alert_posture.txt). The original has no class, faction or owner test on a door (findings/rwh199_nav_validity_static.md
//     section 2): a gate refuses nobody by who they are, so a private town's gates (`is public` = false) stay open too; the per-class
//     refusal of the first slice is retired.
//   - A command (SetDoorState) overrides one door's state, locked included.
//
// Stance (classOf): how the owner of a town sees a faction: Resident (same faction), Ally (relation >= 50), Neutral, Hostile
// (relation <= -30); a factionless squad is Hostile, a town with no owner treats everyone as Neutral. It does not gate any door.
//
// Saved: the explicit door overrides (state::DoorOverrides). The book itself is derived from those, the world data and
// the live relations, and rebuilt after a load. The crews' reopen timers are not saved (they restart after a load).
//
// Determinism: update() runs in the serial strategic step; everything is ordered by state::Id.

#include "ksim/nav/navigator.hpp"
#include "ksim/core/state.hpp"
#include "ksim/world/world.hpp"

#include "knav/door.hpp"

#include <flecs.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ksim {

class FactionBook;
struct TownSite;

// A faction's stance toward a town's owner.
enum class DoorClass : int
{
	Resident = 0,
	Ally = 1,
	Neutral = 2,
	Hostile = 3,
};

// The immutable inputs, from the world (Simulation::enableAi fills them).
struct DoorData
{
	std::vector<DoorSite> sites;
	// TOWN records with `is public` false.
	std::unordered_set<std::string> privateTowns;

	static std::shared_ptr<const DoorData> fromWorld(const World& world);
};

struct DoorTuning
{
	bool enabled = true;
	// A goal within this of a gate town's centre is "in" that town (the gate->town binding itself has no cutoff, gatebind.hpp). UNSURE.
	double townReach = 1500.0;
	// A squad this close to a gate town's centre is inside the walls. UNSURE.
	double insideReach = 700.0;
	// A crew reopening a shut gate: UNLOCK takes this long (DOORS.md section 6: progress 1 - 0.3 dt) and only a crew of the
	// door's faction can do it; OPEN is done when the openness passes this fraction (DOOR_IS_OPEN, at `door move speed`).
	double crewUnlockSeconds = 3.33;
	double crewOpenFraction = 0.8;
	// The exe's global thresholds (FACTIONS.md 3.1): FactionBook::kAlly / kHostile.
	double allyRelation = 50.0;
	double hostileRelation = -30.0;
};

// The gate a raid has to break to get into a town (DoorAccess::bashing).
struct GateBash
{
	std::uint64_t key = 0; // 0 = none: nothing to break
	double seconds = 0.0; // how long the crew bashes before it gives way
	double x = 0.0; // where the gate stands
	double z = 0.0;
};

class DoorAccess
{
public:
	virtual ~DoorAccess() = default;
	// A raid of `faction` going to (toX, toZ) that the gates refuse bashes the first locked gate of that town
	// (DOORS.md section 6: raiders beat on a locked gate until it breaks). Nothing to break: key 0.
	virtual GateBash bashing(flecs::entity_t faction, double toX, double toZ) const
	{
		(void)faction;
		(void)toX;
		(void)toZ;
		return {};
	}
};

struct DoorStats
{
	std::uint64_t gates = 0; // gate doors with a town
	std::uint64_t gateTowns = 0;
	std::uint64_t booksApplied = 0;
	std::uint64_t doorsRestricted = 0; // doors in the book now
	std::uint64_t raidHours = 0; // sum over strategic steps of started hostile raids seen
	std::uint64_t raidHoursAtGates = 0; // ... whose target town has gates
	std::uint64_t gatesBroken = 0; // gates raiders bashed down (open for ever)
	std::uint64_t gatesCrewed = 0; // gate doors with a crew now
	std::uint64_t crewReopens = 0; // shut gates a crew reopened (overrides it removed)
};

class DoorSystem final : public DoorAccess
{
public:
	DoorSystem(std::shared_ptr<const DoorData> data, std::shared_ptr<const FactionBook> factions,
		DoorTuning tuning);

	// The strategic step (after the faction layer): counts the raids, and, if the book changed, applies it to `navigator`
	// (may be null). towns is the candidate list in Id order.
	void update(flecs::world& ecs, std::uint64_t tick, const std::vector<TownSite>& towns,
		const Navigator* navigator);
	// Before the squad step: lets the crews reopen the gates they man, then applies the book if it is not the navigator's yet
	// (first step, a load, an override command, a reopen).
	void sync(flecs::world& ecs, std::uint64_t tick, const std::vector<TownSite>& towns, const Navigator* navigator);

	GateBash bashing(flecs::entity_t faction, double toX, double toZ) const override;
	// The bash on ONE gate (goal 270's resolved gate, task raid-goals): the same test as bashing's, for the door key the caller holds;
	// key 0 when that gate is not locked now (it is open, or shut and passable - the goal's way through).
	GateBash bashFor(flecs::entity_t faction, std::uint64_t key) const;
	// A bashed-down gate: an override with state 3 (broken: open for ever, no policy, never closed again). Called from the
	// serial squad step.
	void breakGate(flecs::world& ecs, std::uint64_t key);

	const knav::DoorBook& book() const noexcept { return book_; }
	const DoorStats& stats() const noexcept { return stats_; }
	const DoorTuning& tuning() const noexcept { return tuning_; }
	// The town (TownState entity) a gate door belongs to; 0 for a door with no town.
	flecs::entity_t townOf(std::uint64_t doorKey) const;
	// The stance of `faction` for the gates of a town owned by `owner`.
	DoorClass classOf(flecs::entity_t faction, flecs::entity_t owner) const;

private:
	struct GateTown
	{
		flecs::entity_t entity = 0;
		double x = 0.0;
		double z = 0.0;
	};
	struct Gate
	{
		std::uint64_t key = 0;
		std::size_t town = 0; // index in gateTowns_
		bool alwaysLocks = false;
		double moveSpeed = 0.5; // `door move speed`: openness per game-second
		std::string ownerFaction; // the placement's `owner faction ID`, "" = the town's
	};

	void bind(flecs::world& ecs, const std::vector<TownSite>& towns);
	// The crews reopen their shut gates: removes the overrides that have been shut for the unlock + open time.
	void crewsReopen(flecs::world& ecs, std::uint64_t tick);
	knav::DoorBook build(flecs::world& ecs) const;
	void publish(knav::DoorBook&& book, const Navigator* navigator);
	const GateTown* nearest(double x, double z, double reach) const;

	std::shared_ptr<const DoorData> data_;
	std::shared_ptr<const FactionBook> factions_;
	DoorTuning tuning_;
	const flecs::world* ecs_ = nullptr;
	std::size_t boundTowns_ = SIZE_MAX;
	std::vector<GateTown> gateTowns_;
	std::vector<Gate> gates_; // ascending by key
	knav::DoorBook book_;
	std::vector<state::DoorOverride> overrides_; // the ones the book was built with
	bool applied_ = false;
	std::unordered_map<std::uint64_t, std::uint64_t> shutSince_; // gate door key -> tick the crew started on it
	DoorStats stats_;
};

} // namespace ksim
