#pragma once

// Instance components: the simulation's live, saved state (ARCHITECTURE.md
// "Runtime state (decided)"). Templates (templates.hpp, ksim::tmpl) are
// rebuilt from game data on load; everything here is what a save holds.
//
// Every instance entity carries an Id, the stable key that saves, the state
// hash and snapshots use instead of flecs entity ids. A reference to another
// instance inside a component is an EntityRef; relationships are flecs pairs.
//
// Characters are always entities of their own (MemberOf their squad). In
// this slice only squads have a Position; nothing may assume characters never
// get one (ARCHITECTURE.md "Open: what has a position at Strategic level").
//
// Each saved type lists its fields once, in fields(); the save writer/reader
// and the state hash all go through it.

#include "ksim/fighter_state.hpp"

#include <flecs.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ksim::state {

// Reference to another instance entity; 0 = none. Saved as a save-local
// index, hashed as the target's Id.
struct EntityRef
{
	flecs::entity_t id = 0;
};

// Stable instance key, unique per world, never reused. Assigned by the
// spawner (and later by whatever creates instances) from the Ids singleton.
struct Id
{
	std::uint64_t value = 0;
};

// Singleton: next Id to hand out.
struct Ids
{
	std::uint64_t next = 1;
};

// Singleton: absolute tick count since game day 0, 00:00 (clock.hpp derives
// the calendar). The sim owns it; the client is sent it.
struct Clock
{
	std::uint64_t tick = 0;
};

// Where a squad instance came from (docs/reference/WORLD_MODEL.md sections
// d and f; tools/world_model.py build_spawn_sources).
enum class SpawnSource : std::uint8_t
{
	TownResident = 0, // TOWN `residents`
	TownBar = 1, // TOWN `bar squads`
	TownDefaultResident = 2, // TOWN `default resident`
	TownRoaming = 3, // TOWN `roaming squads`
	BiomeHomeless = 4, // BIOMES / BIOME_GROUP `homeless spawns`
	BiomeNest = 5, // resident lists of a biome `nests` TOWN
	FactionRoaming = 6, // FACTION `roaming squads`
	Campaign = 7, // raiders commissioned by a faction's campaign (factions.hpp); removed when it ends
	Dismissed = 8, // a character dismissed from a player faction (player_squads.cpp), in a squad of its own
};

inline constexpr int kSpawnSourceCount = 9;

std::string_view spawnSourceName(SpawnSource source) noexcept;

struct FactionState
{
	static constexpr std::string_view kName = "FactionState";
	std::string templateSid;
	// Prosperity lost below the FACTION's `max prosperity` (0 = at the cap). A dead squad costs kSquadDeathProsperity (10), the faction layer
	// gives back kProsperityRegen (0.1) per update (TOWN_LIFE_SLICE 4.5, kinspect item 53 / 54). Stored as the loss so a new faction starts full.
	double prosperityLost = 0.0;
	// The faction's money (save v70; the exe's Ownerships cash at ownerships+0x88, DIALOGUE_ACTIONS_STATIC 2). Today only the dialogue
	// economy writes it: DA_TAKE_MONEY credits the speaker's faction (DIALOGUE.md D5, decision 10). A plain counter, not the economy overlay's.
	std::int64_t cash = 0;
	// The original's per-faction standing (save v105, kenshi-cym.6; GAMESTATE_FACTION `rank`, `global trust`, `trust#` / `trustNeg#`): imported from an
	// original save and kept as it was; no system of ours reads or changes it yet (meaning UNSURE, SAVE_IMPORT.md Stage 3b). A trust row is by the other
	// faction's FACTION sid, sorted.
	struct Trust
	{
		static constexpr std::string_view kName = "FactionTrust";
		std::string other;
		double trust = 0.0;
		double trustNeg = 0.0;

		template <typename Archive, typename Self>
		static void fields(Archive& ar, Self& self)
		{
			ar(self.other);
			ar(self.trust);
			ar(self.trustNeg);
		}
	};
	std::int32_t rank = 0;
	double globalTrust = 0.0;
	std::vector<Trust> trust;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.templateSid);
		ar(self.prosperityLost);
		// Save v70: the faction cash. An older save loads every faction at 0.
		if (ar.version >= 70)
			ar(self.cash);
		// Save v105: the imported standing. An older save loads every faction at rank 0 with no trust rows.
		if (ar.version >= 105)
		{
			ar(self.rank);
			ar(self.globalTrust);
			ar(self.trust);
		}
	}
};

// A town's flags and counters in the original's GAMESTATE_TOWN (save v105, kenshi-cym.6): `public`, `plevel`, `building material`, the discovery flags and
// the population counters. Imported from an original save and kept as it was: no system of ours reads them yet (SAVE_IMPORT.md Stage 3b). A town of
// ours that was not imported holds the defaults (`known` false: nothing was read), and `isPublic` then says nothing.
struct TownInfo
{
	static constexpr std::string_view kName = "TownInfo";
	bool known = false; // read from an original save
	bool isPublic = true;
	std::int32_t level = 0; // plevel
	std::string material; // `building material`, a BUILDING_MATERIAL sid
	bool started = false;
	bool explored = false;
	bool discovered = false;
	bool recentlyDiscovered = false;
	bool ded = false;
	double pop = 0.0; // pop, popd, pop2, popd2: the counters, meaning UNSURE
	double popd = 0.0;
	double pop2 = 0.0;
	double popd2 = 0.0;
	std::vector<std::string> factionsHere; // `factions here#`, FACTION sids

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.known);
		ar(self.isPublic);
		ar(self.level);
		ar(self.material);
		ar(self.started);
		ar(self.explored);
		ar(self.discovered);
		ar(self.recentlyDiscovered);
		ar(self.ded);
		ar(self.pop);
		ar(self.popd);
		ar(self.pop2);
		ar(self.popd2);
		ar(self.factionsHere);
	}
};

struct TownState
{
	static constexpr std::string_view kName = "TownState";
	std::string rootSid; // the default TOWN record (not an override target)
	std::string activeSid; // the variant in force (`override town` chain)
	EntityRef owner; // FactionState; none for marker towns
	// The primary 0-townlist placement (world_model.py): where squads travel
	// from and to. Not terrain-snapped.
	double x = 0.0;
	double z = 0.0;
	// The original's flags and counters (save v105): see TownInfo.
	TownInfo info;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.rootSid);
		ar(self.activeSid);
		ar(self.owner);
		ar(self.x);
		ar(self.z);
		if (ar.version >= 105)
			ar(self.info);
	}
};

// A town record of an original save that has no TownState of ours (save v105, kenshi-cym.6): the later placements of a TOWN record (we keep one state per
// TOWN record, at its first placement) and the player's own town. Imported so the save's owner, flags and replacement are not lost; nothing reads it
// yet (SAVE_IMPORT.md Stage 3b). `placed` false: no townlist placement names `instance` (the player's town: `player_town_template`), x / z are 0.
struct ExtraTown
{
	static constexpr std::string_view kName = "ExtraTown";
	std::string instance; // the townlist placement id (`98-rebirth-INGAME`) or the template name
	std::string rootSid; // the TOWN record the placement belongs to, "" when `placed` is false
	std::string activeSid; // the replacement variant in force, "" = rootSid
	std::string name; // the record's `name`, for a town named by the player
	EntityRef owner; // FactionState
	bool placed = false;
	double x = 0.0;
	double z = 0.0;
	TownInfo info;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.instance);
		ar(self.rootSid);
		ar(self.activeSid);
		ar(self.name);
		ar(self.owner);
		ar(self.placed);
		ar(self.x);
		ar(self.z);
		ar(self.info);
	}
};

struct SquadInstance
{
	static constexpr std::string_view kName = "SquadInstance";
	std::string templateSid; // SQUAD_TEMPLATE
	EntityRef faction; // FactionState; none for factionless templates
	EntityRef homeTown; // TownState; none for wilderness spawns
	SpawnSource source = SpawnSource::TownResident;
	// Wilderness squads: where placement put them (placement.hpp), the centre
	// of their patrol or territory. PROVISIONAL (see placement.hpp).
	bool hasDen = false;
	double denX = 0.0;
	double denZ = 0.0;
	// Density-spawned wilderness squads (wilderness.hpp): the spawner slot
	// (BIOME_GROUP) whose quota the squad fills, -1 for every other squad,
	// and for nest squads the nest instance (a Nests counter value), 0 else.
	std::int32_t slot = -1;
	std::uint32_t nest = 0;
	// Town residents: the building the town populate gave the squad (BuildingSite::key, or the gate's GateSite::building), its home, and where
	// it spawned there (the building's centre, a gate's outside point): the squad layer's home point. 0 = none.
	std::uint64_t homeBuilding = 0;
	double homeX = 0.0;
	double homeZ = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.templateSid);
		ar(self.faction);
		ar(self.homeTown);
		ar(self.source);
		ar(self.hasDen);
		ar(self.denX);
		ar(self.denZ);
		ar(self.slot);
		ar(self.nest);
		ar(self.homeBuilding);
		ar(self.homeX);
		ar(self.homeZ);
	}
};

struct CharacterInstance
{
	static constexpr std::string_view kName = "CharacterInstance";
	std::string templateSid; // CHARACTER or ANIMAL_CHARACTER

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.templateSid);
	}
};

// Ground-plane position; y comes from the heightmap when needed. Every
// instance with a Position is a region-engine entity (region.hpp): the
// engine's arrays are canonical and this is their one-way mirror, written at
// the tick boundary.
struct Position
{
	static constexpr std::string_view kName = "Position";
	double x = 0.0;
	double z = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.x);
		ar(self.z);
	}
};

// The rest of a region-engine entity's hot state, mirrored like Position.
// The move order is kinematic state proper; group, target, hits and steps
// belong to the engine's stand-in workload (region.hpp) and go when real AI
// replaces it. rng is the entity's own stream (streamSeed by Id).
struct Mover
{
	static constexpr std::string_view kName = "Mover";
	std::int32_t group = -1; // stand-in squad index; -1 = none
	bool hasOrder = false;
	double orderX = 0.0;
	double orderZ = 0.0;
	EntityRef target; // locked entity (another Mover); none = 0
	std::uint32_t hits = 0;
	std::uint32_t steps = 0;
	std::uint64_t rng = 0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.group);
		ar(self.hasOrder);
		ar(self.orderX);
		ar(self.orderZ);
		ar(self.target);
		ar(self.hits);
		ar(self.steps);
		ar(self.rng);
	}
};

struct GroundPoint
{
	double x = 0.0;
	double z = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.x);
		ar(self.z);
	}
};

// One detail-level system's RNG stream and run counters (simulation.hpp).
struct SubsystemState
{
	std::uint64_t rng = 0;
	std::uint64_t runs = 0;
	std::uint64_t checksum = 0;
	std::uint64_t lastRunTick = 0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.rng);
		ar(self.runs);
		ar(self.checksum);
		ar(self.lastRunTick);
	}
};

// Singleton: the tick driver's own state, mirrored by Simulation at the
// tick boundary. seed derives every stream that has not started yet (new
// region entities, new stand-in groups).
struct Scheduler
{
	static constexpr std::string_view kName = "Scheduler";
	std::uint64_t seed = 0;
	std::vector<SubsystemState> subsystems;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.seed);
		ar(self.subsystems);
	}
};

// A stand-in squad's group state (region.hpp), indexed by Mover::group.
struct StandInGroup
{
	double x = 0.0;
	double z = 0.0;
	std::uint64_t rng = 0;
	std::uint64_t steps = 0;

	bool operator==(const StandInGroup&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.x);
		ar(self.z);
		ar(self.rng);
		ar(self.steps);
	}
};

// Singleton: mirror of the region engine's stand-in group states.
struct StandInGroups
{
	static constexpr std::string_view kName = "StandInGroups";
	std::vector<StandInGroup> groups;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.groups);
	}
};

// Singleton: player positions; each one makes a full-detail area
// (region.hpp). Canonical here, set only by the SetPlayerAreas command.
struct PlayerAreas
{
	static constexpr std::string_view kName = "PlayerAreas";
	std::vector<GroundPoint> centres;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.centres);
	}
};

// What a squad is doing (ai.hpp). Every kind can end, fail or be replaced;
// the group layer always picks a successor (ARCHITECTURE.md "AI").
enum class GoalKind : std::uint8_t
{
	None = 0, // not yet assigned; never persists past a group-layer step
	Idle = 1, // ordinary life where it stands, until `until`
	GoHome = 2, // travel to the home town
	TravelTo = 3, // travel to `target`
	Patrol = 4, // walk to patrol point `leg` of the loop round the anchor (goalX, goalZ)
	Roam = 5, // walk to a point of the territory round the anchor (goalX, goalZ)
	Raid = 6, // a faction's order (GroupAI::order): outbound, stay at the target town, return
	ManGate = 7, // a gate crew: walk to the gate's outside point (state::GateCrew) and hold it for ever (MAN_THE_GATE 180)
};

inline constexpr int kGoalKindCount = 8;

// Why a squad runs the generic fallback instead of a mapped behaviour
// (ai.hpp), or why a goal failed. One enum: both are "the reason logged".
enum class AiReason : std::uint8_t
{
	None = 0,
	Unmapped = 1, // no registry entry for its package
	Planned = 2, // mapped to a behaviour not implemented yet
	NoPosition = 3, // wilderness squad: no position yet
	NoNavigation = 4, // no navigator configured
	OutsideWindow = 5, // position outside the navigation window
	NoRoute = 6, // the coarse graph does not join the ends
	OffMesh = 7, // an endpoint has no walkable polygon nearby
	Stuck = 8, // no progress along the route for kStuckHours
	NoTarget = 9, // no candidate town
	Expired = 10, // a raid order passed its deadline
	DoorRefused = 11, // a route exists with every door open, but a gate refuses this squad
};

inline constexpr int kAiReasonCount = 12;

std::string_view aiReasonName(AiReason reason) noexcept;

// A failed or abandoned goal that may not be chosen again until `until`.
struct GoalCooldown
{
	GoalKind kind = GoalKind::None;
	EntityRef target; // TownState; none for Idle
	std::uint64_t until = 0; // Clock tick

	bool operator==(const GoalCooldown&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.kind);
		ar(self.target);
		ar(self.until);
	}
};

// Where a faction's order to a squad stands (ai.hpp group layer, factions.hpp
// faction layer). The faction writes Issued; the group layer moves it on and
// the faction reads Done / Failed back as an event.
enum class OrderStatus : std::uint8_t
{
	None = 0,
	Issued = 1, // written by the faction; the squad has not started
	Outbound = 2, // walking to the target
	Staying = 3, // at the target until `until`
	Returning = 4, // walking back to the origin
	Done = 5, // back at the origin
	Failed = 6, // `failure` says why; the squad stops
	Retreating = 7, // after the on-site time, holding at the target for `retreat` ticks
};

// A raid order: intent from the faction layer, executed by the group layer
// as GoalKind::Raid. Never terminal: Done and Failed hand the squad back to
// ordinary life (and to the faction, which removes campaign squads).
struct RaidOrder
{
	OrderStatus status = OrderStatus::None;
	AiReason failure = AiReason::None;
	EntityRef target; // TownState to raid
	double originX = 0.0; // where it returns to
	double originZ = 0.0;
	std::uint64_t stay = 0; // ticks on site at the target
	std::uint64_t retreat = 0; // ticks of Retreating hold after the stay, then the way home
	std::uint64_t deadline = 0; // Clock tick after which the order expires
	std::uint64_t issued = 0;
	std::string campaign; // FACTION_CAMPAIGN stringID

	bool operator==(const RaidOrder&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.status);
		ar(self.failure);
		ar(self.target);
		ar(self.originX);
		ar(self.originZ);
		ar(self.stay);
		ar(self.retreat);
		ar(self.deadline);
		ar(self.issued);
		ar(self.campaign);
	}
};

// The group layer's state for one squad: goal, route and bookkeeping that
// feeds goal selection. Canonical in flecs (slow-changing state); only the
// squad's position is hot state and lives in the region engine.
struct GroupAI
{
	static constexpr std::string_view kName = "GroupAI";
	GoalKind goal = GoalKind::None;
	EntityRef target; // TownState for GoHome / TravelTo
	std::uint64_t since = 0; // tick the goal was chosen
	std::uint64_t until = 0; // Idle: the tick it ends
	AiReason fallback = AiReason::None; // why the generic fallback runs
	std::vector<GroundPoint> route; // coarse path, on walkable ground
	double length = 0.0; // route length
	double along = 0.0; // distance travelled along the route
	std::uint64_t lastStep = 0; // tick of the last advance
	std::uint64_t progressTick = 0; // last tick `along` moved
	std::uint64_t homeTick = 0; // last tick the squad was at its home town
	std::vector<GoalCooldown> cooldowns; // stable-sorted by (kind, until): ties keep their saved order
	// Patrol / Roam: the point being walked to, and the patrol point index
	// (the next one is leg + 1 after an arrival).
	double goalX = 0.0;
	double goalZ = 0.0;
	std::uint32_t leg = 0;
	RaidOrder order; // the faction's order, if any
	// A raid whose gates refused it bashes the gate on the way (doors.hpp): the gate's door key (0 = none), how far along the
	// route it stands, how long the bashing takes, and the tick it ends (0 = not started).
	std::uint64_t bashGate = 0;
	double bashAt = 0.0;
	double bashSeconds = 0.0;
	std::uint64_t bashUntil = 0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.goal);
		ar(self.target);
		ar(self.since);
		ar(self.until);
		ar(self.fallback);
		ar(self.route);
		ar(self.length);
		ar(self.along);
		ar(self.lastStep);
		ar(self.progressTick);
		ar(self.homeTick);
		ar(self.cooldowns);
		ar(self.goalX);
		ar(self.goalZ);
		ar(self.leg);
		ar(self.order);
		ar(self.bashGate);
		ar(self.bashAt);
		ar(self.bashSeconds);
		ar(self.bashUntil);
	}
};

// Faction layer: what a faction wants (factions.hpp). Utility-scored every
// game-hour; groups read it as intent. Expand and Purge are named by the
// design (ARCHITECTURE.md "AI") and not implemented yet.
enum class Intent : std::uint8_t
{
	Consolidate = 0, // the baseline: nothing pressing
	Defend = 1, // towns lost or squads hurt: stay close, no trips
	Raid = 2, // run a campaign against a hostile town
	Mourn = 3, // after losses: no new campaigns for a while
};

inline constexpr int kIntentCount = 4;

std::string_view intentName(Intent intent) noexcept;

// One campaign in progress: its commissioned squads (state::Id values, so a
// squad that is removed or killed cannot leave a dangling reference).
struct Raid
{
	std::string campaign; // FACTION_CAMPAIGN stringID
	EntityRef target; // TownState
	EntityRef origin; // TownState the squads left from
	std::uint64_t planned = 0; // Clock tick
	std::uint64_t arrived = 0; // first tick a squad was seen at the target; 0 = not yet
	bool started = false; // a squad has left
	std::uint32_t squadsAtStart = 0; // squads raised
	std::uint32_t squadsLost = 0; // ... removed from outside (killed); alive fraction = 1 - lost / start
	std::vector<std::uint64_t> squads;

	bool operator==(const Raid&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.campaign);
		ar(self.target);
		ar(self.origin);
		ar(self.planned);
		ar(self.arrived);
		ar(self.started);
		ar(self.squadsAtStart);
		ar(self.squadsLost);
		ar(self.squads);
	}
};

// A campaign or a target town that may not be chosen again until `until`.
struct Cooldown
{
	std::string campaign; // used by campaign cooldowns
	EntityRef town; // used by target blacklists
	std::uint64_t until = 0;

	bool operator==(const Cooldown&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.campaign);
		ar(self.town);
		ar(self.until);
	}
};

// A follow-up campaign (`victory trigger` / `loss trigger`) waiting for its time.
struct PendingTrigger
{
	std::string campaign;
	std::uint64_t due = 0;

	bool operator==(const PendingTrigger&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.campaign);
		ar(self.due);
	}
};

// A field a behaviour script declared on its faction (script.hpp, LUA_API.md "State"):
// typed, named, saved with the faction. The list is sorted by name; the script's
// declaration decides which exist and their type, so a field the script no longer
// declares is dropped at the next call and a new one starts at its default.
enum class ScriptFieldType : std::uint8_t
{
	Number = 0, // a double, in `number`
	Integer = 1, // an int64, in `integer`
	Boolean = 2, // 0 / 1, in `integer`
};

struct ScriptField
{
	std::string name;
	ScriptFieldType type = ScriptFieldType::Number;
	std::int64_t integer = 0;
	double number = 0.0;

	bool operator==(const ScriptField&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.name);
		ar(self.type);
		ar(self.integer);
		ar(self.number);
	}
};

// The faction layer's state for one FactionState entity. Slow state, canonical
// in flecs. Temperament variables are 0..1.
struct FactionAI
{
	static constexpr std::string_view kName = "FactionAI";
	Intent intent = Intent::Consolidate;
	std::uint64_t since = 0; // tick the intent was chosen
	std::uint64_t lastStep = 0;
	std::uint64_t nextRoll = 0; // Clock tick of the next campaign roll (every 6 game-hours); 0 = unset
	double aggression = 0.0; // from the data: share of hostile relations
	double alarm = 0.0; // towns lost: rises on events, decays
	double grief = 0.0; // squads lost: rises on events, decays
	double confidence = 0.0; // towns gained: rises on events, decays
	std::vector<Raid> raids;
	std::vector<Cooldown> campaignCooldowns; // sorted by (campaign, until)
	std::vector<Cooldown> targetCooldowns; // sorted by (town, until)
	std::vector<PendingTrigger> triggers;
	// Counters over the whole game (saved: soak and tests read them).
	std::uint32_t planned = 0; // raids commissioned (squads spawned)
	std::uint32_t started = 0; // raids whose squads left
	std::uint32_t arrived = 0; // raids that reached the target
	std::uint32_t completed = 0; // arrived and came home
	std::uint32_t failed = 0; // never arrived (every squad failed or was lost)
	std::uint32_t rejected = 0; // chosen but could not be commissioned (no route, no forces)
	std::uint32_t squadsLost = 0;
	std::uint32_t events = 0; // events consumed
	// The faction-intent script's declared fields (empty without one, or when it
	// declares none).
	std::vector<ScriptField> script;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.intent);
		ar(self.since);
		ar(self.lastStep);
		ar(self.nextRoll);
		ar(self.aggression);
		ar(self.alarm);
		ar(self.grief);
		ar(self.confidence);
		ar(self.raids);
		ar(self.campaignCooldowns);
		ar(self.targetCooldowns);
		ar(self.triggers);
		ar(self.planned);
		ar(self.started);
		ar(self.arrived);
		ar(self.completed);
		ar(self.failed);
		ar(self.rejected);
		ar(self.squadsLost);
		ar(self.events);
		ar(self.script);
	}
};

// Information flowing up to the faction layer (ARCHITECTURE.md "AI").
enum class EventKind : std::uint8_t
{
	SquadLost = 0, // faction lost a squad
	TownLost = 1, // faction lost a town (owner changed away from it)
	TownGained = 2, // faction gained a town
	RaidOutcome = 3, // one of the faction's campaigns ended (`town` = its target, `code` = RaidResult)
	RelationCrossed = 4, // the faction's opinion of `other` crossed -30 or 50 (`code` = RelationCrossing)
};

// FactionEvent::code of a RaidOutcome.
enum class RaidResult : std::int32_t
{
	Victory = 0, // the force arrived (and stayed, and came home)
	Loss = 1, // never arrived: every squad failed or was lost
	Defeated = 2, // the alive fraction fell under 0.2
};

// FactionEvent::code of a RelationCrossed: the exe's thresholds (FactionBook::kHostile
// -30, kAlly 50). Hostile = at or below -30, Ally = at or above 50.
enum class RelationCrossing : std::int32_t
{
	BecameHostile = 1,
	NoLongerHostile = 2,
	BecameAlly = 3,
	NoLongerAlly = 4,
};

struct FactionEvent
{
	EventKind kind = EventKind::SquadLost;
	EntityRef faction; // whose event
	EntityRef town; // TownLost / TownGained / RaidOutcome (the target)
	std::uint64_t tick = 0;
	EntityRef other; // RelationCrossed: the faction the opinion is about
	std::int32_t code = 0; // RaidOutcome / RelationCrossed detail

	bool operator==(const FactionEvent&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.kind);
		ar(self.faction);
		ar(self.town);
		ar(self.tick);
		ar(self.other);
		ar(self.code);
	}
};

// Singleton: events waiting for the faction layer's next step.
struct FactionEvents
{
	static constexpr std::string_view kName = "FactionEvents";
	std::vector<FactionEvent> pending;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.pending);
	}
};

// Relationship (character, MemberOf, squad). Exclusive: one squad at a time.
struct MemberOf
{
	static constexpr std::string_view kName = "MemberOf";

	template <typename Archive, typename Self>
	static void fields(Archive&, Self&)
	{
	}
};

// Relationship (faction, Relation, faction): the live relation value,
// -100..100 (a float, as in the exe: reasons move it by fractions, see
// FactionRelationReason in factions.hpp). Spawned from FACTION `relations`; an
// unlisted pair means the source template's `default relation`.
struct Relation
{
	static constexpr std::string_view kName = "Relation";
	double value = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.value);
	}
};

// One spawner slot's pending respawns (wilderness.hpp): the Clock ticks at
// which a missing squad / nest is replaced, ascending.
struct WildSlotState
{
	std::vector<std::uint64_t> homelessDue;
	std::vector<std::uint64_t> nestDue;

	bool operator==(const WildSlotState&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.homelessDue);
		ar(self.nestDue);
	}
};

// Singleton: the wilderness spawner's state (wilderness.hpp). The quotas are
// derived from game data and these two multipliers; what is saved is the
// timers, the stream and the nest counter.
struct WildernessState
{
	static constexpr std::string_view kName = "WildernessState";
	double population = 1.0; // multiplies homeless squads per slot
	double nests = 1.0; // multiplies nests per slot (the exe's new-game option)
	std::uint64_t rng = 0;
	std::uint32_t nextNest = 1;
	std::vector<WildSlotState> slots;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.population);
		ar(self.nests);
		ar(self.rng);
		ar(self.nextNest);
		ar(self.slots);
	}
};

// A gate crew: the squad is the garrison of one town gate (SQUAD_TEMPLATE `building` names the gate's building type; the town's
// populate step assigned it, spawn.hpp). Only its leader runs MAN_THE_GATE (goal 180): it stands at the gate's outside point
// (door centre + 15 n) and does nothing else; the members static-guard beside it (GATES_CARRY_LIVE.md a). The squad's home is the
// gate, not the town. `post` is that outside point, saved so the AI needs no door geometry; (faceX, faceZ) is the unit direction
// outside - inside. On the squad entity.
struct GateCrew
{
	static constexpr std::string_view kName = "GateCrew";
	std::uint64_t gate = 0; // the gate's first door key (World GateSite::key)
	double postX = 0.0;
	double postZ = 0.0;
	double faceX = 0.0;
	double faceZ = 0.0;
	// The home building's gate (the exe's Ownerships+0x38): the gate the crew was housed in at spawn. `gate` is the binding
	// in force (the task's +0x10); Simulation::stepGateBinding rebinds it when it is gone (gatebind.hpp). 0 = none.
	std::uint64_t home = 0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.gate);
		ar(self.postX);
		ar(self.postZ);
		ar(self.faceX);
		ar(self.faceZ);
		ar(self.home);
	}
};

// MAN_THE_GATE for one full-detail character (the squad leader of a gate crew, gatecrew.cpp): the gate's first door key (the
// state::Door's `key`) and the outside point it stands on. `phase`, for observers: 0 standing at / walking to the post, 1 working
// the shut gate (UNLOCK, then OPEN), 2 walking back. On the leader's entity.
struct GateDuty
{
	static constexpr std::string_view kName = "GateDuty";
	std::uint64_t gate = 0;
	double postX = 0.0;
	double postY = 0.0;
	double postZ = 0.0;
	std::uint8_t phase = 0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.gate);
		ar(self.postX);
		ar(self.postY);
		ar(self.postZ);
		ar(self.phase);
	}
};

// A character's open conversation (DIALOGUE.md 3.1, slice 1 step 5: monologues only; the exe's `dlg` per character). On the speaker that
// opened it; an interjecting squad mate speaks a line (`voice`) without one of its own. Records by sid (the book's indices are derived).
// The line holds until `until` (the speech timer, DIALOGUE.md 3.2), then the next child is picked or the conversation ends. A busy speaker
// drops new events (DIALOGUE_SELECTION_LIVE: no queue).
struct Conversation
{
	static constexpr std::string_view kName = "Conversation";
	std::string dialogue; // DIALOGUE sid of the root
	std::string line; // DIALOGUE_LINE sid now spoken
	std::uint64_t voice = 0; // state::Id of who speaks the line (the speaker, or an interjector)
	std::uint64_t target = 0; // state::Id, 0 = none
	std::uint64_t started = 0; // tick
	std::uint64_t until = 0; // tick the line's speech timer runs out
	std::uint8_t event = 0; // EventTriggerEnum that opened it
	std::uint64_t follower = 0; // state::Id DA_FOLLOW_WHILE_TALKING set following the target (the voice of that line); the follow ends with the conversation
	// The open reply menu (DIALOGUE.md 13): the DIALOGUE_LINE sids offered to the target, in menu order, "" = the "[...]" continue entry;
	// empty = no menu. While it is open the conversation waits for Command::DialogueReply / EndConversation. menuTick: the tick it opened
	// (its texts are rolled at it; menuSeq = the low 32 bits).
	std::vector<std::string> replies;
	std::uint64_t menuTick = 0;
	// The DIALOGUE sid a spoken line's `interrupt` set (DIALOGUE.md 3.6): the player's talk while it runs opens that root instead; "" = none.
	std::string interrupt;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.dialogue);
		ar(self.line);
		ar(self.voice);
		ar(self.target);
		ar(self.started);
		ar(self.until);
		ar(self.event);
		ar(self.follower);
		ar(self.replies);
		ar(self.menuTick);
		ar(self.interrupt);
	}
};

struct DialogueLock
{
	std::string sid; // DIALOGUE sid
	bool locked = false;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.sid);
		ar(self.locked);
	}
};

// A character's dialogue lock map (the exe's dlg+0x30, per character, DIALOGUE.md 3.7): what DA_LOCK_THIS_DIALOG and a line's `locks` /
// `unlocks` / `unlock but keep me` wrote, ascending by sid. A root with an entry takes its value; one without keeps the record's `locked`.
struct DialogueMemory
{
	static constexpr std::string_view kName = "DialogueMemory";
	std::vector<DialogueLock> locks;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.locks);
	}
};

struct ShownStamp
{
	std::string sid; // DIALOGUE / DIALOGUE_LINE sid
	std::uint64_t tick = 0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.sid);
		ar(self.tick);
	}
};

// Singleton: when each dialogue record was last shown, for the repetition limit (DIALOGUE_SELECTION_LIVE Q4: global per record, every speaker
// and target share it; showing a line stamps its root too). Ascending by sid. Absent until the first line is spoken.
struct DialogueShown
{
	static constexpr std::string_view kName = "DialogueShown";
	std::vector<ShownStamp> stamps;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.stamps);
	}
};

// Singleton: the town gates that no longer exist (Simulation::removeGate), ascending by door key. Absent while there are none. A
// gate crew bound to one of them rebinds (gatebind.hpp, GATE_CREWS_LIVE.md section 5).
struct GatesGone
{
	static constexpr std::string_view kName = "GatesGone";
	std::vector<std::uint64_t> keys;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.keys);
	}
};

// Singleton: where Simulation::stepBuildings last looked at each fighter, ascending by Id. It tests a fighter's building only once it
// has moved since that look, so the look is state: kept only in memory, a load started every fighter fresh and missed the test a
// fighter moved after the last look of the saved tick was due (ksim_newgame load hash, 2026-11-04).
struct BuildingLook
{
	std::uint64_t id = 0;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.id);
		ar(self.x);
		ar(self.y);
		ar(self.z);
	}
};

struct BuildingLooks
{
	static constexpr std::string_view kName = "BuildingLooks";
	std::vector<BuildingLook> looks;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.looks);
	}
};

// An explicit state for one door, set by a command (a script, the player). Nothing else closes a gate in the sim (a gate crew
// that stands at it reopens it, doors.hpp). `state` is knav::DoorState (0 Open, 1 Closed, 2 Locked).
struct DoorOverride
{
	std::uint64_t key = 0;
	std::uint8_t state = 0;

	bool operator==(const DoorOverride&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.key);
		ar(self.state);
	}
};

// Singleton: the door overrides, ascending by key. Absent while there are none.
struct DoorOverrides
{
	static constexpr std::string_view kName = "DoorOverrides";
	std::vector<DoorOverride> entries;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.entries);
	}
};

// An interactable door (SIMLINK.md section 13): an instance entity beside the characters, in their id space. `key` is the
// kworld::doorKey of the placement (the navmesh bake's door; also what the gate system names). The position is the centre of
// the door's navmesh polygons and also the use point (UNSURE: the original approaches from a side). `state` is
// 0 open, 1 closed, 2 locked, 3 broken (open for ever: cannot be closed, its lock is gone; DOORS.md section 6). `version` bumps
// with every change the client must hear of.
struct Door
{
	static constexpr std::string_view kName = "Door";
	std::uint64_t key = 0;
	std::string recordSid; // the door's BUILDING record
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	std::uint8_t state = 1;
	double health = 16.0; // the building hp (DoorStuff +0x164)
	double maxHealth = 16.0; // the record's Σ `construction` counts, else `build materials` (door_plan.hpp doorMaxHealth)
	std::uint32_t version = 1;
	bool alwaysLocks = false; // the door record's `always locks`: a close locks it again
	double moveSpeed = 0.5; // the door record's `door move speed` (openness per game-second; open / close take 1 / this)
	std::string ownerSid; // the building's FACTION record (isObjectMine: a character of this faction may unlock it); may be empty
	bool gate = false; // a door of a `is wall gate` / `is gateway` building: bashed by BASH_GATE (shared by up to 8, DOORS.md section 6)
	std::int32_t lockLevel = 40; // the lock's level (DOORS_LIVE.md): the owner's `doors lock level` +- `lock level random` + the door's `lock level bonus`
	// The lock's picking timer (lock +0x18 / +0x1c, door_plan.hpp LockTimer): on the lock, so an interrupted pick resumes and
	// every picker shares it.
	double lockElapsed = 0.0;
	double lockInterval = 5.0;
	std::uint32_t lockAttempts = 0;
	// The slab's xz footprint about `x`, `z` (a gate is wide; unit long axis and the half extents along and across it): a
	// character acts from within kInteractReach of its edge, not of its centre.
	double axisX = 1.0;
	double axisZ = 0.0;
	double halfLong = 0.0;
	double halfDeep = 0.0;
	// The building the door belongs to (World DoorSite::building; 0 = none known) and the unit xz normal of the slab that points
	// into it (towards the parent placement's centre): crossing the slab's plane there sets or clears Fighter::building
	// (Simulation::stepBuildings). UNSURE: a parent whose origin is not inside the walls gets the wrong side.
	std::uint64_t building = 0;
	double inX = 0.0;
	double inZ = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.key);
		ar(self.recordSid);
		ar(self.x);
		ar(self.y);
		ar(self.z);
		ar(self.state);
		ar(self.health);
		ar(self.maxHealth);
		ar(self.version);
		ar(self.alwaysLocks);
		ar(self.moveSpeed);
		ar(self.ownerSid);
		ar(self.gate);
		ar(self.lockLevel);
		ar(self.lockElapsed);
		ar(self.lockInterval);
		ar(self.lockAttempts);
		ar(self.axisX);
		ar(self.axisZ);
		ar(self.halfLong);
		ar(self.halfDeep);
		ar(self.building);
		ar(self.inX);
		ar(self.inZ);
	}
};

// A character lifting, carrying or putting down another (carry.hpp, GATES_CARRY_LIVE.md c). On the carrier's entity; the carried
// has Fighter::carriedBy set from the attach on. phase: 0 lifting (elapsed counts up to the lift time; the body attaches at 40 %),
// 1 carrying, 2 putting down (releases at the put-down time).
struct CarryStep
{
	static constexpr std::string_view kName = "CarryStep";
	std::uint64_t target = 0; // the carried character's state::Id
	std::uint8_t phase = 0;
	double elapsed = 0.0;
	bool attached = false; // phase 0: the body has been attached (it is Fighter::carriedBy from here on)

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.target);
		ar(self.phase);
		ar(self.elapsed);
		ar(self.attached);
	}
};

// An NPC resident taking a KO'd intruder out of its home (PICKUP_INTRUDERS 223 -> TAKE_INTRUDER_OUTSIDE 224, carry.cpp; dt24_223).
// On the resident's entity. `plan` is the goal's G4 chain: 67 Running (the walk to the body, installed when the take-out starts),
// 68 LIFT_PERSON (the lift), the 223 goal node, then - planned once the body is in the arms - 67 Running (the way out) and 224
// TAKE_INTRUDER_OUTSIDE (the moveTo outside; the drop is its arrival hook). `step` is the entry being run. `dropX/Y/Z` is 224's
// outside point. `outside` counts the seconds since the carrier was last seen outside the building while still walking (-1 =
// inside); the drop comes when it is outside and idle, or after 5 game-minutes outside.
struct TakeOut
{
	static constexpr std::string_view kName = "TakeOut";
	std::uint64_t target = 0;
	std::uint64_t building = 0;
	std::vector<std::uint16_t> plan;
	std::uint16_t step = 0;
	bool liftOrdered = false; // the 68 carry order went out (an order that ends without a lift ends the take-out)
	bool dropSet = false;
	double dropX = 0.0;
	double dropY = 0.0;
	double dropZ = 0.0;
	double outside = -1.0;
	double elapsed = 0.0; // seconds since the task began (the no-terminal-state cap)

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.target);
		ar(self.building);
		// Save v84 (task CH): the plan and the step it runs replaced the old phase byte (0 = the lift, 1 = the way out). An older
		// save loads with no plan; the executor rebuilds it from the live hands on its first tick (carry.cpp stepIntruders), the
		// same rule Routine::plan follows.
		if (ar.version >= 84)
		{
			ar(self.plan);
			ar(self.step);
			ar(self.liftOrdered);
			ar(self.dropSet);
		} else {
			// The old phase byte (0 = the lift, 1 = the way out): consumed, not kept. The executor rebuilds the plan from the
			// live hands (stepIntruders); fields() sees a const self on the writing side, so it cannot fill the plan here.
			std::uint8_t phase = 0;
			ar(phase);
			(void)phase;
		}
		ar(self.dropX);
		ar(self.dropY);
		ar(self.dropZ);
		ar(self.outside);
		ar(self.elapsed);
	}
};

// The faction a character belongs to (a FACTION record), for door ownership (isObjectMine, DOORS.md section 6). On the
// character's entity; absent = no faction (animals, the unaffiliated).
struct Allegiance
{
	static constexpr std::string_view kName = "Allegiance";
	std::string factionSid;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.factionSid);
	}
};

// A player faction's squad list (docs/reference/FACTION_SQUADS_LIVE.md, SIMLINK PLAYER_SQUADS). On the FactionState entity of a
// faction a player owns. `name` is the faction's display name (the squad default names are `<name>_<N>`), `squadCounter` the running N
// of the next added squad (saved, never reused; starts at 1, UNSURE), `squads` the PlayerSquad entities' state::Id in list order
// (the HUD tab order).
inline constexpr std::string_view kImportedStartoff = "<save import>";

struct PlayerFaction
{
	static constexpr std::string_view kName = "PlayerFaction";
	std::string name;
	std::uint32_t squadCounter = 1;
	std::vector<std::uint64_t> squads;
	// The faction's money (the start-off's `money`; the original keeps it on the player faction's Ownerships, STARTS_CAGES_STATIC a2.6)
	// and the NEW_GAME_STARTOFF sid it began with ("" = a scenario's faction; kImportedStartoff = a faction of an imported save, kenshi-cym.21).
	std::int64_t money = 0;
	std::string startoff;
	// The completed RESEARCH records (stringIDs, in completion order; SIMLINK UNLOCKS 36). The unlocked buildings derive from them.
	std::vector<std::string> research;
	// The TOWN records the faction has discovered / visited (stringIDs, in discovery order; SIMLINK DISCOVERED 37). Both only grow;
	// visited implies discovered.
	std::vector<std::string> discovered;
	std::vector<std::string> visited;
	// The FACTION records the player knows (stringIDs, in the order they became known; SIMLINK FACTION_RELATIONS 38, kinspect 49: the
	// exe's PlayerFactionRelations "known" set). Only grows.
	std::vector<std::string> known;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.name);
		ar(self.squadCounter);
		ar(self.squads);
		ar(self.money);
		ar(self.startoff);
		ar(self.research);
		ar(self.discovered);
		ar(self.visited);
		ar(self.known);
	}
};

// One squad of a player faction, on an instance entity of its own (not a SquadInstance: the squad AI never sees it). `members` are
// the characters' state::Id (fighters), leader first; may be empty.
struct PlayerSquad
{
	static constexpr std::string_view kName = "PlayerSquad";
	std::string name;
	std::vector<std::uint64_t> members;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.name);
		ar(self.members);
	}
};

// One bounty a faction holds on a character (crime.hpp; the exe's Bounty node, CRIME_STATIC.md section 0). `stamp` is the world
// time in game hours of the last commit (the exe's save key "bountyexp" is a timestamp, not an expiry).
struct Bounty
{
	static constexpr std::string_view kName = "Bounty";
	std::string factionSid; // the holder: the legal-system faction of whoever committed it
	std::int32_t amount = 0;
	std::uint32_t crimes = 0; // bit n = CrimeEnum n was committed
	bool claim = false; // the reward was collected
	double stamp = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.factionSid);
		ar(self.amount);
		ar(self.crimes);
		ar(self.claim);
		ar(self.stamp);
	}
};

// A character's crime record (the exe's CrimeRecord at char+0xF0, crime.hpp): ONE pending crime with its timer, the victim, the
// bounties held per faction and the prison sentence. On the character's entity, present only while it holds anything. These are
// per character: several player factions keep separate bounties.
struct CrimeRecord
{
	static constexpr std::string_view kName = "CrimeRecord";
	std::uint8_t crime = 0; // CrimeEnum, 0 = none
	std::string victimFaction; // FACTION sid
	std::uint64_t victimChar = 0; // state::Id of the victim character, 0 = none (a container theft)
	double timer = 0.0; // seconds
	bool committed = false; // the pending crime already went into a bounty (+0xA4)
	double sentenceStart = 0.0; // game hours; with sentenceHours 0 = none
	double sentenceHours = 0.0;
	std::vector<Bounty> bounties; // sorted by factionSid

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.crime);
		ar(self.victimFaction);
		ar(self.victimChar);
		ar(self.timer);
		ar(self.committed);
		ar(self.sentenceStart);
		ar(self.sentenceHours);
		ar(self.bounties);
	}
};

// A character busy at a door (interact.hpp): the walker stopped in front of a closed or locked door on its route, or an
// interact order (open, close, unlock, bash) reached its door. The phases run one after the other; the walk's route is kept
// here and given back when the step ends. On the character's entity.
struct DoorStep
{
	static constexpr std::string_view kName = "DoorStep";
	std::uint64_t door = 0; // the door's state::Id
	std::uint8_t phase = 0; // 0 unlock, 1 open, 2 bash, 3 close, 4 wait (a locked door the character may not open), 5 pick the lock, 6 lock a shut door
	std::uint32_t attempts = 0; // phase 5: lockpicking attempts so far (the seed of the next roll)
	double remaining = 0.0; // seconds left in the phase (bash: until the next hit)
	bool thenOpen = false; // unlock continues with open
	double waited = 0.0; // seconds spent waiting (phase 4)
	std::vector<RoutePoint> route; // the walk to resume (empty for an interact order that stands at its door)
	std::uint32_t routeNext = 0;
	std::uint32_t routeSeq = 0;
	bool routeApproach = false;
	bool toggled = false; // phases 1 and 3: the door has crossed openness 0.6 (its state changed); the step runs on to 1.0

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.door);
		ar(self.phase);
		ar(self.attempts);
		ar(self.remaining);
		ar(self.thenOpen);
		ar(self.waited);
		ar(self.route);
		ar(self.routeNext);
		ar(self.routeSeq);
		ar(self.routeApproach);
		ar(self.toggled);
	}
};

// One carried stack or equipped piece of a state::Inventory (SIMLINK.md section 6, INVENTORY_GRID_STATIC.md). A carried entry is one stack: it
// holds up to the item's stack cap, takes one footprint and sits at (section, x, y) of a grid. `section` 0 is the owner's `main` (or a container's own
// grid); inside `contents` it is the container item's own `backpack_content` grid. kUnplaced = nobody has placed it yet (grid::settle does); kEquipped = worn
// or wielded (the INVENTORY `slot` says where, no x / y). `material` is the instance's MATERIAL_SPECS_CLOTHING (armour, containers, items; a
// weapon's is `quality.model`), `colour` its COLOR_DATA (always empty today: OPEN, UNSURE, INVENTORY_GRID_STATIC (f)). `contents` is the
// backpack_content of a CONTAINER item instance; it moves with the item (INVENTORY_GRID_STATIC (d)).
// Item provenance (ARCHITECTURE.md "Item provenance"): `units` of a stack that `planter` put into another character's inventory or
// equipment at `tick`. Silent: nothing in the base rules reads it.
struct Planting
{
	std::uint32_t units = 0;
	std::uint64_t planter = 0;
	std::uint64_t tick = 0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.units);
		ar(self.planter);
		ar(self.tick);
	}
};

struct InventoryEntry
{
	static constexpr std::uint8_t kUnplaced = 254;
	static constexpr std::uint8_t kEquipped = 255;
	std::string itemSid;
	std::uint32_t count = 0;
	std::uint8_t slot = 0; // Inventory::kCarried, kWeapon, kArmourBase + n
	ItemQuality quality; // a weapon's or armour piece's generated quality; unset for plain items
	std::uint8_t section = kUnplaced;
	std::uint8_t x = 0;
	std::uint8_t y = 0;
	std::string material;
	std::string colour;
	std::vector<InventoryEntry> contents;
	// The exe's `Item+0x12a` flag (TOWN_LIFE_SLICE 3.1): a trader's special item. A merchant's wipe keeps it and a clone copies it. It is part of
	// the merge key (a unique stack never merges with a plain one, so the wipe cannot eat a unique unit).
	bool unique = false;
	// Provenance (save v89). planted: a short list of {units, planter, time}; not in the merge key, a merge concatenates and a split
	// takes units first in, first out. stolen: the victim FACTION ("" = clean); IS in the merge key (a stolen stack never joins a clean one).
	std::vector<Planting> planted;
	std::string stolen;

	// Moves `units` of `from`'s planted list (first in, first out) onto this entry's list. Call beside every count transfer between stacks.
	void takePlantedFrom(InventoryEntry& from, std::uint32_t units)
	{
		for (auto it = from.planted.begin(); it != from.planted.end() && units > 0;)
		{
			const std::uint32_t part = std::min(it->units, units);
			planted.push_back({part, it->planter, it->tick});
			units -= part;
			it->units -= part;
			it = it->units == 0 ? from.planted.erase(it) : it + 1;
		}
	}

	// The merge key of INVENTORY_GRID_STATIC (c): same record, manufacturer and material (colour is not part of it), and the unique flag.
	const std::string& materialSid() const noexcept { return quality.model.empty() ? material : quality.model; }
	bool sameKind(const InventoryEntry& other) const noexcept
	{
		return itemSid == other.itemSid && quality.maker == other.quality.maker && materialSid() == other.materialSid() && unique == other.unique && stolen == other.stolen;
	}

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.itemSid);
		ar(self.count);
		ar(self.slot);
		ar(self.quality);
		ar(self.section);
		ar(self.x);
		ar(self.y);
		ar(self.material);
		ar(self.colour);
		ar(self.contents);
		ar(self.unique);
		if (ar.version >= 89)
		{
			ar(self.planted);
			ar(self.stolen);
		}
	}
};

// An item lying on the ground (SIMLINK.md section 13). `use` is where a character stands to pick it up (the walkable point
// nearest it). A stack: `count` of the one item record.
// A severed limb's tint (rwh42 section 2, task limb-tint, save v104): what `severPart` -> 1400ced10 fits to the item from the victim, kept for the
// item's life. Colours are 0..1 as the exe stores them (SeveredLimbItem +0x1f4 skin = 1 - the victim's Skin Tone, +0x204 blood = the race's blood
// colour); the scale is the exe's +0x1e8 vec3 (the Forearm / Calf bone size). OURS/UNSURE: our skin tone is one 0..1 number (FighterLook::skin), so
// the skin colour is grey, and we have no bone sizes, so the scale is 1 on every axis. `set` is false for every other item.
struct LimbTint
{
	bool set = false;
	double skinR = 1.0;
	double skinG = 1.0;
	double skinB = 1.0;
	double bloodR = 0.0;
	double bloodG = 0.0;
	double bloodB = 0.0;
	double scaleX = 1.0;
	double scaleY = 1.0;
	double scaleZ = 1.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.set);
		ar(self.skinR);
		ar(self.skinG);
		ar(self.skinB);
		ar(self.bloodR);
		ar(self.bloodG);
		ar(self.bloodB);
		ar(self.scaleX);
		ar(self.scaleY);
		ar(self.scaleZ);
	}
};

struct GroundItem
{
	static constexpr std::string_view kName = "GroundItem";
	std::string itemSid; // ITEM / WEAPON / ARMOUR ... record
	std::uint32_t count = 1;
	ItemQuality quality; // the stack's quality (a dropped weapon keeps its generated one)
	std::string material; // the instance's MATERIAL_SPECS_CLOTHING ("" = none yet: picked when it is picked up)
	std::string colour;
	std::vector<InventoryEntry> contents; // a dropped container keeps its backpack_content
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double useX = 0.0;
	double useY = 0.0;
	double useZ = 0.0;
	std::uint32_t version = 1;
	std::string ownerSid; // the FACTION that owns it ("" = nobody's: anything dropped); taking an owned one is theft (event 14)
	std::string stolen; // provenance (save v89): the victim faction of a stolen stack that was dropped; the pick-up keeps it (no smuggling by dropping)
	LimbTint tint; // a severed limb's skin, blood and scale (save v104)

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.itemSid);
		ar(self.count);
		ar(self.quality);
		ar(self.material);
		ar(self.colour);
		ar(self.contents);
		ar(self.x);
		ar(self.y);
		ar(self.z);
		ar(self.useX);
		ar(self.useY);
		ar(self.useZ);
		ar(self.version);
		ar(self.ownerSid);
		if (ar.version >= 89)
			ar(self.stolen);
		if (ar.version >= 104)
			ar(self.tint);
	}
};

// A storage building with an inventory (BUILDING `has inventory`: crates, barrels, shelves, safes; SIMLINK.md section 13). An
// instance entity in the character id space. Its contents are the state::Inventory component on the same entity (all entries
// carried, slot 0): INVENTORY records name a container by its id. `ownerSid` is the FACTION record that owns it (the placement's
// own owner faction, else the nearest town's, as for doors, UNSURE); taking from a container whose owner is not the actor's
// faction is theft (event 14). A box is a placed storage BUILDING or a storage piece of a building's interior layout
// (routines.cpp classifyPiece); `key` names its site, so a zone that is dropped and furnished again finds the same box.
// `locked` is the record's `has lock` (DOORS.md section 6's lock on a container): loot and transfers are refused until it is
// picked, and the lock's timer is the container's, shared by every picker (as state::Door's).
struct Container
{
	static constexpr std::string_view kName = "Container";
	std::uint64_t key = 0; // the site's key (World ContainerSite::key, or the layout piece's); 0 = an unnamed scenario box
	std::string recordSid; // the BUILDING record
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double useX = 0.0; // where to stand: the walkable point nearest it
	double useY = 0.0;
	double useZ = 0.0;
	std::string ownerSid; // may be empty (nobody's)
	bool locked = false; // `has lock`: a pick (interact action 3) is the only way in
	std::int32_t lockLevel = 40; // the lock's level: the owner faction's `doors lock level` +- `lock level random` + the record's `lock level bonus`
	// The lock's picking timer (lock_plan's LockTimer), on the lock: an interrupted pick resumes and every picker shares it.
	double lockElapsed = 0.0;
	double lockInterval = 5.0;
	std::uint32_t lockAttempts = 0;
	std::uint32_t version = 1; // bumps when the owner or the lock changes (the contents have their own, Inventory::version)
	// What the wire names (INTERACTABLE kind 3 tail, save v108): the owning building's doorKey and the piece's index in its interior layout's
	// data list (every entry counted); a placed box's own placement key and 0xFFFF; a built storage's BUILDING doorKey and 0xFFFF; a scenario box 0 and 0xFFFF.
	static constexpr std::uint16_t kNoPiece = 0xFFFF;
	std::uint64_t building = 0;
	std::uint16_t piece = kNoPiece;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		// Save v71: the site key and the lock. A v70 save loads its boxes unlocked with key 0 (the furnish dedupes them by spot).
		if (ar.version >= 71)
			ar(self.key);
		ar(self.recordSid);
		ar(self.x);
		ar(self.y);
		ar(self.z);
		ar(self.useX);
		ar(self.useY);
		ar(self.useZ);
		ar(self.ownerSid);
		if (ar.version >= 71)
		{
			ar(self.locked);
			ar(self.lockLevel);
			ar(self.lockElapsed);
			ar(self.lockInterval);
			ar(self.lockAttempts);
		}
		ar(self.version);
		if (ar.version >= 108)
		{
			ar(self.building);
			ar(self.piece);
		}
	}
};

// A placement item node: an ITEM_PLACEMENT_GROUP instance of a building's interior layout (a cup on a bar table, a book on a
// shelf; WORLD_LAYOUT.md "Placement item nodes"). It holds at most one GroundItem, owned like the building; once that is taken the
// node refills randint(min, max) game hours later with a new weighted pick (kinspect itemgroup_static.md: the original spawns when
// the interior is first shown; ours when the zone's navmesh is furnished). `key` names the node (the building's key and the layout instance), so a dropped and refurnished zone finds it.
struct ItemNode
{
	static constexpr std::string_view kName = "ItemNode";
	std::uint64_t key = 0;
	std::string groupSid; // the ITEM_PLACEMENT_GROUP
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double useX = 0.0;
	double useY = 0.0;
	double useZ = 0.0;
	std::string ownerSid;
	std::uint64_t item = 0; // the Id of its GroundItem, 0 = empty
	std::uint64_t refillTick = 0; // when empty: the tick it refills (0 = not yet timed)
	std::uint32_t picks = 0; // items it has put out, the dice of the next pick

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.key);
		ar(self.groupSid);
		ar(self.x);
		ar(self.y);
		ar(self.z);
		ar(self.useX);
		ar(self.useY);
		ar(self.useZ);
		ar(self.ownerSid);
		ar(self.item);
		ar(self.refillTick);
		ar(self.picks);
	}
};

// A producer: a placed BUILDING whose functionality `produces` and `consumes` nothing (wild deposits, mines, later drills and wells;
// producers.hpp, docs/reference/MINE_SELL_SLICE.md). An instance entity with a deterministic Id (producers.hpp kProducerIdBase + the
// World::producerSites() index), so a producer that is dropped and made again keeps its Id. Its output is the state::Inventory on the
// same entity (one slot of the produced item's footprint, Inventory::stackLimit = the `produces` v0 cap). Saved only while it holds stock
// or progress or an operator (Producer::pristine); every other one is made again from the data when a player comes near.
// `progress` is 0..1 towards the next unit, shared by the operators. `operators` are the characters working it, in arrival order (each
// also holds an InteractTask with action Use on this Id). `prodState`: 1 full, 2 working, 3 idle (CRAFTING_SLICE 4.5; 0 unused).
struct Producer
{
	static constexpr std::string_view kName = "Producer";
	std::string recordSid; // the BUILDING record
	std::string ownerSid; // the placement's owner faction, empty = wild
	std::string output; // the produced ITEM (the functionality's `produces`)
	std::uint32_t cap = 0; // `produces` v0: the most the slot holds
	std::uint8_t maxOperators = 0; // the merged `max operators` (producers.hpp); 0 = automatic
	double useRange = 20.0; // the functionality's `use range` (Ore mine: 100): an operator counts as arrived within it of the origin (xz)
	bool progressBar = false; // the functionality's `has progress bar when used`: TASK_PROGRESS kind 9 is sent only when set (MQ9)
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double useX = 0.0;
	double useY = 0.0;
	double useZ = 0.0;
	// The site's collision bounds (ProducerSite::boxMinX ..); all 0 = none. Inside them x 1.2 also counts as arrived (MQ9).
	double boxMinX = 0.0;
	double boxMaxX = 0.0;
	double boxMinZ = 0.0;
	double boxMaxZ = 0.0;
	double richness = 1.0;
	double progress = 0.0;
	std::uint8_t prodState = 3;
	std::vector<std::uint64_t> operators;
	std::uint32_t version = 1; // bumps on a state or operator-count change (the stock has Inventory::version)
	// The tick each operator started operating (CHAR_DYNAMIC actionStart). Not saved: an operator found without one after a load starts
	// at the first tick stepProducers sees it.
	std::map<std::uint64_t, std::uint64_t> since;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.recordSid);
		ar(self.ownerSid);
		ar(self.output);
		ar(self.cap);
		ar(self.maxOperators);
		ar(self.progressBar);
		ar(self.useRange);
		ar(self.x);
		ar(self.y);
		ar(self.z);
		ar(self.useX);
		ar(self.useY);
		ar(self.useZ);
		ar(self.boxMinX);
		ar(self.boxMaxX);
		ar(self.boxMinZ);
		ar(self.boxMaxZ);
		ar(self.richness);
		ar(self.progress);
		ar(self.prodState);
		ar(self.operators);
		ar(self.version);
	}
};

// A merchant squad's refresh state (TOWN_LIFE_SLICE 3.1, ksim/trader.hpp), on the squad entity next to its state::Inventory (the shop's
// stock, section 0 of the store). `nextRefresh` is the exe's `Platoon+0x1f8` (save key `inventory refresh time`), absolute game hours;
// `money` the squad wallet (`Platoon+0x1d0`); `refreshes` the draws made so far (it keys the dice, so a replay cannot shift one).
struct TraderStock
{
	static constexpr std::string_view kName = "TraderStock";
	double nextRefresh = 0.0;
	std::uint32_t refreshes = 0;
	std::int32_t money = 0;
	bool seeded = false; // the forced spawn-time refresh has run (the unique special items exist)
	std::uint64_t destroyed = 0; // units destroyed because they did not fit, over the squad's life (D2)

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.nextRefresh);
		ar(self.refreshes);
		ar(self.money);
		ar(self.seeded);
		ar(self.destroyed);
	}
};

// A wandering trader squad's circuit (TOWN_LIFE_SLICE 4.6, the `trade_circuit` behaviour): on the squad entity next to its GroupAI. `town` is
// the town it is in while `staying`, else the town it is travelling to (0 before the first pick); `lastTown` the one it left (logging only; the
// A-B-A pattern is normal). `arrivedAt` (game hours) is stamped only on the no-town -> town transition (kinspect item 54 TQ10); `stayHours`
// is the U(5, 8) rolled once per arrival; `arrivals` counts the stamps and keys the dice.
struct TraderTrip
{
	static constexpr std::string_view kName = "TraderTrip";
	EntityRef town;
	EntityRef lastTown;
	bool staying = false;
	double arrivedAt = 0.0;
	double stayHours = 0.0;
	std::uint32_t arrivals = 0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.town);
		ar(self.lastTown);
		ar(self.staying);
		ar(self.arrivedAt);
		ar(self.stayHours);
		ar(self.arrivals);
	}
};

// The real-economy overlay (TOWN_LIFE_SLICE 7, save v69). All of it exists only while `economy.mode = real`: the mode is the
// singleton EconomySettings, and with it absent nothing else here is created (the default, `original`).
//
// EconomySettings: the world flag, one per world (7.1). Original (0) is V1: the T1-T3 shops and traders, no economy system.
inline constexpr std::uint8_t kEconomyOriginal = 0;
inline constexpr std::uint8_t kEconomyReal = 1;

struct EconomySettings
{
	static constexpr std::string_view kName = "EconomySettings";
	std::uint8_t mode = kEconomyOriginal;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.mode);
	}
};

// The world's event set (WORLD_EVENT_STATE, WORLD_MODEL.md a; save v72, task AN). One per world, made at the first death or
// imprisonment so a quiet world carries none. `dead` are the unique CHARACTER records that have died and whose corpse is gone:
// an instance's Fighter says dead while the body is there, but a decayed corpse would read "never seen = alive" again.
// `playerTouched` are the unique characters the player killed or imprisoned: the `player involvement` input (tracked, not part of
// a state's truth; WORLD_MODEL.md "The player-involvement ambiguity"). Both only grow, in the order it happened.
struct WorldEventState
{
	static constexpr std::string_view kName = "WorldEventState";
	std::vector<std::string> dead;
	std::vector<std::string> playerTouched;
	// The unique NPCs an original save lists as already spawned (save v105, kenshi-cym.6; CAMERA `usedUniques#`): the CHARACTER sid, the original's
	// state value (1 = 66 of 75 in SkinHouse, 2 = 9; meaning UNSURE), the player flag and the character's entity Id when its handle resolved (0 = none).
	// Kept as it was: nothing reads it yet (SAVE_IMPORT.md Stage 3b).
	struct Used
	{
		static constexpr std::string_view kName = "UsedUnique";
		std::string sid;
		std::int32_t state = 0;
		bool player = false;
		std::uint64_t member = 0;

		template <typename Archive, typename Self>
		static void fields(Archive& ar, Self& self)
		{
			ar(self.sid);
			ar(self.state);
			ar(self.player);
			ar(self.member);
		}
	};
	std::vector<Used> used;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		if (ar.version >= 72)
		{
			ar(self.dead);
			ar(self.playerTouched);
		}
		if (ar.version >= 105)
			ar(self.used);
	}
};

// The world's new-game advanced options (ADVANCED_OPTIONS_STATIC.md "Ours"; save v77, task BT). One per world, made by a
// NEW_GAME that carries options or by the debug order that sets one; a world without the singleton keeps the server's
// CombatSettings values, so a debug server can pin a knob without writing it into every save. The keys are the exe's:
// cod (chance of death), gdm (global damage), ht (hunger time). The ranges are the UI's sliders' (ADVANCED_OPTIONS_STATIC).
struct WorldOptions
{
	static constexpr std::string_view kName = "WorldOptions";
	// Every default is 1.0 except hunger time, which was 4.0 on the measured save (MEDICAL_HUNGER_LIVE.md "Globals").
	static constexpr double kChanceOfDeathDefault = 1.0;
	static constexpr double kChanceOfDeathMin = 0.5;
	static constexpr double kChanceOfDeathMax = 4.0;
	static constexpr double kDamageScaleDefault = 1.0;
	static constexpr double kDamageScaleMin = 0.5;
	static constexpr double kDamageScaleMax = 4.0;
	static constexpr double kHungerTimeDefault = 4.0;
	static constexpr double kHungerTimeMin = 0.25;
	static constexpr double kHungerTimeMax = 8.0;

	double chanceOfDeath = kChanceOfDeathDefault; // cod: x the bleed rate and the wound degeneration
	double damageScale = kDamageScaleDefault; // gdm: x every damage component (CombatSettings::damageScale)
	double hungerTime = kHungerTimeDefault; // ht: x `starvation time` (CombatSettings::hungerTime)

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.chanceOfDeath);
		ar(self.damageScale);
		ar(self.hungerTime);
	}
};

// TownStock: one aggregate amount per item per town (7.2), on the town entity. It never reads the shops' physical inventories.
// `target` is the town's desired level (the fill ratio's denominator and the price rule's target, 7.4); `amount` is integrated
// hourly by the economy tick. Sorted by itemSid.
struct StockLine
{
	std::string itemSid;
	double amount = 0.0;
	double target = 0.0;

	bool operator==(const StockLine&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.itemSid);
		ar(self.amount);
		ar(self.target);
	}
};

struct TownStock
{
	static constexpr std::string_view kName = "TownStock";
	std::vector<StockLine> lines;
	double prosperity = 1.0; // 0..1, derived each tick from the fill ratios (7.4)
	double lastTick = 0.0; // game hours at the last integration

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.lines);
		ar(self.prosperity);
		ar(self.lastTick);
	}
};

// A production camp (farm, mine, stone camp) or an NPC converter supplying a town (7.3). T4a defines it and the tick runs it;
// T4b attaches the baseline camps. `consumes` are the chain's inputs (7.3: a converter consumes items, e.g. wheat to flour); a
// producer with no inputs runs while its town has room for the output. `buffer` is output waiting for a caravan (7.4, T4c).
// NOTE: state::Producer already exists (a game machine's operation state); this one is the economy's supply node.
struct ConsumeLine
{
	std::string itemSid;
	double perHour = 0.0;

	bool operator==(const ConsumeLine&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.itemSid);
		ar(self.perHour);
	}
};

struct EconomyProducer
{
	static constexpr std::string_view kName = "EconomyProducer";
	EntityRef town; // the town it supplies
	std::string itemSid; // the output item
	double ratePerHour = 0.0;
	double buffer = 0.0;
	std::vector<ConsumeLine> consumes;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.town);
		ar(self.itemSid);
		ar(self.ratePerHour);
		ar(self.buffer);
		ar(self.consumes);
	}
};

// A farm's crop (task ds/arrest-2, save v89): one per placed BUILDING with `farm data` (the exe's function-0xd buildings, which 232's
// finder and Task_EatingCrops name; UNSURE that the function id is exactly "has farm data"). `growth` is game hours since planting, up to
// FARM_DATA `growth time`; `food` is the units ripe and uneaten (plants x `output per plant` once grown), which 231 eats down. `town` is
// the nearest town centre when the plot was made (OURS, like the camps). Derived from the world's placements at the first use, saved.
struct CropPlot
{
	static constexpr std::string_view kName = "CropPlot";
	std::string buildingSid;
	std::string ownerFaction; // the placement's `owner faction ID`, "" = none
	EntityRef town;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double growth = 0.0;
	double food = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.buildingSid);
		ar(self.ownerFaction);
		ar(self.town);
		ar(self.x);
		ar(self.y);
		ar(self.z);
		ar(self.growth);
		ar(self.food);
	}
};

// A character the player talked to stands still until `until` (save v93): Simulation::talkTo sets it, the town scorer yields to it,
// and the scorer's loop removes it once it has run out. OURS: the original holds for as long as the dialogue / trade window is open
// (UNSURE); the sim does not see the window (it is the Session's), so the hold is a TTL (kTalkHoldSeconds, dialogue_sim.cpp). It always
// expires, so no layer reaches a terminal state. Same pattern as the container holds per client session (ROADMAP 7): share one mechanism then.
struct TalkHold
{
	static constexpr std::string_view kName = "TalkHold";
	std::uint64_t until = 0; // tick

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.until);
	}
};

// What a member keeps while its squad is a token (save v99, kenshi-k16.6.51): the persistent subset of its Fighter, as the difference from what
// Simulation::buildMember would make from the template. demoteWorldSquad writes it only for a member that differs (an untouched member stores
// nothing and is rebuilt exactly as before); buildMember applies it to the fresh Fighter and removes it. The original keeps the member's
// state across unload the same way (rwh.37: the characters are serialised into the platoon record). Transient state (route, target, task,
// combat timers) is never stored. `mask` says which blocks follow, so a wounded member does not also carry its gear.
struct MemberDelta
{
	static constexpr std::string_view kName = "MemberDelta";
	static constexpr std::uint8_t kBody = 1; // parts, blood, wounds, KO and arm state, dead
	static constexpr std::uint8_t kSlave = 2; // slaveState and the captor
	static constexpr std::uint8_t kWeapon = 4;
	static constexpr std::uint8_t kArmour = 8;
	static constexpr std::uint8_t kSkills = 16; // trained stats
	static constexpr std::uint8_t kKit = 32; // the first-aid kit in use (save v100): its sid and charges left
	static constexpr std::uint8_t kLook = 64; // the name and the sex (save v103): what an imported save's member is called and is, which the template does not say

	std::uint8_t mask = 0;
	std::vector<FighterPart> parts;
	double blood = 0.0;
	std::vector<double> wounds;
	double koTimer = 0.0;
	bool unconscious = false;
	bool dead = false;
	bool rightArmOk = true;
	bool leftArmOk = true;
	bool down = false;
	std::uint8_t slaveState = 0;
	std::string captorSid;
	double slaveStamp = -1.0; // save v107
	FighterWeapon weapon;
	std::vector<FighterArmour> armour;
	FighterSkills skills;
	std::string kitSid;
	double kitCharges = 0.0;
	std::string name;
	std::uint8_t female = 0;

	// The blocks in which `now` differs from `built`, none = untouched.
	static std::optional<MemberDelta> between(const Fighter& now, const Fighter& built)
	{
		MemberDelta delta;
		if (now.parts != built.parts || now.blood != built.blood || now.wounds != built.wounds
			|| now.koTimer != built.koTimer || now.unconscious != built.unconscious || now.dead != built.dead
			|| now.rightArmOk != built.rightArmOk || now.leftArmOk != built.leftArmOk || now.down != built.down)
		{
			delta.mask |= kBody;
			delta.parts = now.parts;
			delta.blood = now.blood;
			delta.wounds = now.wounds;
			delta.koTimer = now.koTimer;
			delta.unconscious = now.unconscious;
			delta.dead = now.dead;
			delta.rightArmOk = now.rightArmOk;
			delta.leftArmOk = now.leftArmOk;
			delta.down = now.down;
		}
		if (now.slaveState != built.slaveState || now.captorSid != built.captorSid || now.slaveStamp != built.slaveStamp)
		{
			delta.mask |= kSlave;
			delta.slaveState = now.slaveState;
			delta.captorSid = now.captorSid;
			delta.slaveStamp = now.slaveStamp;
		}
		if (now.weapon != built.weapon)
		{
			delta.mask |= kWeapon;
			delta.weapon = now.weapon;
		}
		if (now.armour != built.armour)
		{
			delta.mask |= kArmour;
			delta.armour = now.armour;
		}
		if (now.skills != built.skills)
		{
			delta.mask |= kSkills;
			delta.skills = now.skills;
		}
		if (now.kitSid != built.kitSid || now.kitCharges != built.kitCharges)
		{
			delta.mask |= kKit;
			delta.kitSid = now.kitSid;
			delta.kitCharges = now.kitCharges;
		}
		if (now.name != built.name || now.look.female != built.look.female)
		{
			delta.mask |= kLook;
			delta.name = now.name;
			delta.female = now.look.female;
		}
		if (delta.mask == 0)
			return std::nullopt;
		return delta;
	}

	void applyTo(Fighter& f) const
	{
		if ((mask & kBody) != 0)
		{
			f.parts = parts;
			f.blood = blood;
			f.wounds = wounds;
			f.koTimer = koTimer;
			f.unconscious = unconscious;
			f.dead = dead;
			f.rightArmOk = rightArmOk;
			f.leftArmOk = leftArmOk;
			f.down = down;
		}
		if ((mask & kSlave) != 0)
		{
			f.slaveState = slaveState;
			f.captorSid = captorSid;
			f.slaveStamp = slaveStamp;
		}
		if ((mask & kWeapon) != 0)
			f.weapon = weapon;
		if ((mask & kArmour) != 0)
			f.armour = armour;
		if ((mask & kSkills) != 0)
			f.skills = skills;
		if ((mask & kKit) != 0)
		{
			f.kitSid = kitSid;
			f.kitCharges = kitCharges;
		}
		if ((mask & kLook) != 0)
		{
			f.name = name;
			f.look.female = female;
		}
	}

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.mask);
		if ((self.mask & kBody) != 0)
		{
			ar(self.parts);
			ar(self.blood);
			ar(self.wounds);
			ar(self.koTimer);
			ar(self.unconscious);
			ar(self.dead);
			ar(self.rightArmOk);
			ar(self.leftArmOk);
			ar(self.down);
		}
		if ((self.mask & kSlave) != 0)
		{
			ar(self.slaveState);
			ar(self.captorSid);
			if (ar.version >= 107)
				ar(self.slaveStamp);
		}
		if ((self.mask & kWeapon) != 0)
			ar(self.weapon);
		if ((self.mask & kArmour) != 0)
			ar(self.armour);
		if ((self.mask & kSkills) != 0)
			ar(self.skills);
		if ((self.mask & kKit) != 0)
		{
			ar(self.kitSid);
			ar(self.kitCharges);
		}
		if ((self.mask & kLook) != 0)
		{
			ar(self.name);
			ar(self.female);
		}
	}
};

// A goods caravan's order (7.4, T4c): on the caravan squad. `phase`: 0 load, 1 go, 2 unload. `postedAt` (save v73): the game hour the
// match created it; a carrier that has not delivered after T4d's limit has the order released (economy.hpp).
inline constexpr std::uint8_t kCaravanLoad = 0;
inline constexpr std::uint8_t kCaravanGo = 1;
inline constexpr std::uint8_t kCaravanUnload = 2;

struct CaravanOrder
{
	static constexpr std::string_view kName = "CaravanOrder";
	EntityRef from;
	EntityRef to;
	std::string itemSid;
	double amount = 0.0;
	std::uint8_t phase = kCaravanLoad;
	double postedAt = 0.0; // game hours

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.from);
		ar(self.to);
		ar(self.itemSid);
		ar(self.amount);
		ar(self.phase);
		// Save v73: the order's age clock. A v72 save carries orders without one; they load at 0, so the first economy tick treats them
		// as stale and returns the goods to the source (RUNTIME_SAVE.md).
		if (ar.version >= 73)
			ar(self.postedAt);
	}
};

// The town's supply/demand board (7.4, T4c): `kind` 0 an offer (a producer has surplus), 1 a need (the town is short); `expires`
// is a game hour, so a dead or distracted worker never holds an entry. Sorted by (kind, itemSid, expires) for determinism.
inline constexpr std::uint8_t kBoardOffer = 0;
inline constexpr std::uint8_t kBoardNeed = 1;

struct BoardEntry
{
	std::uint8_t kind = kBoardOffer;
	EntityRef from; // the producer or town offering / needing
	EntityRef to; // the town an offer is for, or the town a need is posted for (0 = any)
	std::string itemSid;
	double amount = 0.0;
	double postedAt = 0.0;
	double expires = 0.0;

	bool operator==(const BoardEntry&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.kind);
		ar(self.from);
		ar(self.to);
		ar(self.itemSid);
		ar(self.amount);
		ar(self.postedAt);
		ar(self.expires);
	}
};

struct SupplyBoard
{
	static constexpr std::string_view kName = "SupplyBoard";
	std::vector<BoardEntry> entries;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.entries);
	}
};

// The in-town routines' saved state (TOWN_LIFE_SLICE 5.2, save v48). Nothing derived is saved: the compiled goal tables, the site
// index and the reservation book are rebuilt at load (the book from every loaded member's Routine::target and SquadRoutine::barTable).
//
// MemberRole: on a member character, written at spawn (SquadBuilder::addMember knows the template list it came from): 0 Squad,
// 1 Squad2, 2 Leader, 3 Slave. The live role is Leader when the member leads its squad now; else `list`. A character without the
// component (a dismissed one, a new-game member) reads as 0.
inline constexpr std::uint8_t kRoleSquad = 0;
inline constexpr std::uint8_t kRoleSquad2 = 1;
inline constexpr std::uint8_t kRoleLeader = 2;
inline constexpr std::uint8_t kRoleSlave = 3;

struct MemberRole
{
	static constexpr std::string_view kName = "MemberRole";
	std::uint8_t list = 0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.list);
	}
};

// A member's build slot (save v101, kenshi-k16.6.55): the k its template build is keyed on (Simulation::templateFighter, memberDiceKey), fixed when
// the member is spawned, so a squadmate leaving the squad (a corpse decays, a capture) does not move the others' dice. A member without one (an older
// save, a character made outside the spawner) takes its index in the squad's Id-sorted list once, at its first build, and keeps it.
struct MemberSlot
{
	static constexpr std::string_view kName = "MemberSlot";
	std::uint32_t slot = 0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.slot);
	}
};

// A straggler token (save v102, kenshi-k16.6.53): a squad of one member that demoteWorldSquad split off its parent squad because it stood farther than
// kStragglerRadius from the squad's leader (knocked out, fled, picked off). `parent` is the squad it came from; the token's SquadInstance is a copy of
// the parent's, so the member's template dice stay the parent's. Removed when the token merges back (Simulation::mergeStraggler) or the parent is gone.
struct StragglerOf
{
	static constexpr std::string_view kName = "StragglerOf";
	EntityRef parent;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.parent);
	}
};

// The guards' arrest orders (Simulation::arrestOrders_, save v109, kenshi-k16.6.200): a singleton like BoardStates, written just before a save is
// encoded and read back (then removed) at a load; absent with no order running. One entry per guard in Id order. `since` is a clock tick.
struct ArrestStates
{
	static constexpr std::string_view kName = "ArrestStates";
	struct Entry
	{
		std::uint64_t guard = 0;
		std::uint64_t victim = 0;
		std::uint64_t since = 0;
		std::uint16_t lift = 240;
		bool started = false;
		bool snatched = false;
		bool gaveUp = false;

		template <typename Archive, typename Self>
		static void fields(Archive& ar, Self& self)
		{
			ar(self.guard);
			ar(self.victim);
			ar(self.since);
			ar(self.lift);
			ar(self.started);
			ar(self.snatched);
			ar(self.gaveUp);
		}
	};
	std::vector<Entry> entries;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.entries);
	}
};

// The animals' and investigators' run state (Simulation::limbCooldown_, limbPlay_, alarms_, alarmDwell_, save v110, kenshi-k16.6.202): a singleton like
// ArrestStates, written just before a save is encoded and read back (then removed) at a load; absent with all four empty. Members and buildings are
// Id values; `finder` is a clock tick; an alarm's `town` is the environment's town index and `life` its seconds left.
struct RunStates
{
	static constexpr std::string_view kName = "RunStates";
	struct Cooldown
	{
		std::uint64_t member = 0;
		std::uint64_t finder = 0;

		template <typename Archive, typename Self>
		static void fields(Archive& ar, Self& self)
		{
			ar(self.member);
			ar(self.finder);
		}
	};
	struct Play
	{
		std::uint64_t member = 0;
		double x = 0.0;
		double z = 0.0;
		std::uint32_t picks = 0;

		template <typename Archive, typename Self>
		static void fields(Archive& ar, Self& self)
		{
			ar(self.member);
			ar(self.x);
			ar(self.z);
			ar(self.picks);
		}
	};
	struct Alarm
	{
		std::uint32_t town = 0;
		std::uint64_t id = 0;
		std::uint8_t kind = 0;
		std::uint64_t owner = 0;
		std::uint64_t raiser = 0;
		std::uint64_t building = 0;
		double x = 0.0;
		double z = 0.0;
		double life = 0.0;

		template <typename Archive, typename Self>
		static void fields(Archive& ar, Self& self)
		{
			ar(self.town);
			ar(self.id);
			ar(self.kind);
			ar(self.owner);
			ar(self.raiser);
			ar(self.building);
			ar(self.x);
			ar(self.z);
			ar(self.life);
		}
	};
	struct Dwell
	{
		std::uint64_t member = 0;
		double left = 0.0;

		template <typename Archive, typename Self>
		static void fields(Archive& ar, Self& self)
		{
			ar(self.member);
			ar(self.left);
		}
	};
	std::vector<Cooldown> cooldowns;
	std::vector<Play> plays;
	std::vector<Alarm> alarms;
	std::uint64_t lastAlarm = 0; // the board's id counter: a new alarm after the load keeps counting
	std::vector<Dwell> dwells;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.cooldowns);
		ar(self.plays);
		ar(self.alarms);
		ar(self.lastAlarm);
		ar(self.dwells);
	}
};

// CorpseDecay (save v54; kinspect bq15_corpse_cleanup.md a, s79_static.md 1.2): on a dead character, written by the first
// corpse evaluation after death (Simulation::stepCorpses; ARCHITECTURE.md "Corpses"). `stamp` is Character+0xd0, in game hours
// (the exe's saved "decay" key): the corpse is destroyed when now - stamp > 12 h. 0 is "not yet stamped", as in the exe.
// Save v100 (kenshi-k16.6.52): `deathTick` and `cause` for a body that died while its squad was a token (Simulation::leaveCorpse); 0 / 0 for
// every other (the stamp is then the first evaluation, as before).
struct CorpseDecay
{
	static constexpr std::string_view kName = "CorpseDecay";
	static constexpr std::uint8_t kCauseOther = 0;
	static constexpr std::uint8_t kCauseBledOut = 1; // the off-screen medical step took the last blood
	static constexpr std::uint8_t kCauseTokenFight = 2; // k16.6.46: a stats fight between tokens
	double stamp = 0.0;
	std::uint64_t deathTick = 0;
	std::uint8_t cause = kCauseOther;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.stamp);
		if (ar.version >= 100)
		{
			ar(self.deathTick);
			ar(self.cause);
		}
	}
};

// One usable thing of a building (routines::Site, with the kind as its u8): kind 0 = none.
struct RoutineSite
{
	std::uint8_t kind = 0;
	std::uint64_t object = 0; // the building's key
	std::uint16_t slot = 0; // ordinal within (building, kind)

	bool operator==(const RoutineSite&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.kind);
		ar(self.object);
		ar(self.slot);
	}
};

// A (goal, target) pair that failed: skipped until the tick.
struct RoutineBlocked
{
	std::uint16_t goal = 0;
	RoutineSite target;
	std::uint64_t until = 0;

	bool operator==(const RoutineBlocked&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.goal);
		ar(self.target);
		ar(self.until);
	}
};

// A building a member's RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE 216 has searched, and the tick it started (task patrol). The squad's
// memory of who searched what is the union over its members' `Routine::searched` lists: there is no separate squad state.
struct RoutineSearch
{
	std::uint64_t building = 0;
	std::uint64_t tick = 0;

	bool operator==(const RoutineSearch&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.building);
		ar(self.tick);
	}
};

// One record of a member's shopping list (task CH group 3, dt24_shop / dt24_118): the wanted ITEM `item function` (the shop
// transaction filters stock by it, so two ITEM records of one function share a record), how many the day's list wants, and how
// many the member has bought. The exe's record is 12 bytes at AI+0x2b0, taken at +8 (dt24_shop section 5); the field order within
// it is UNSURE there, ours is named.
struct ShoppingWant
{
	std::uint16_t function = 0;
	std::uint16_t want = 0;
	std::uint16_t taken = 0;

	bool operator==(const ShoppingWant&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.function);
		ar(self.want);
		ar(self.taken);
	}
};

// A member at Full detail whose squad is in routine mode (added at promotion, removed at demotion or death). `goal` is the running
// AI_TASK enum (0 = none yet), `since` the tick it was adopted, `holdUntil` the minimum hold's end, `target` the reserved site,
// `lastSleep` the game hours of the last sleep event (the one tiredness field: NPCs have no tiredness stat, kinspect item 57),
// `blocked` sorted, `nextScore` the tick of the next evaluation (the stagger).
// v68 (T3e2): `chainTarget` is the character the member's slave chain picked (167, CAPTURE_NEW_SLAVES) and still holds until it is put
// in a cage, 0 = none; `chainElapsed` is the "Restraining" timer in seconds, negative until the member has reached the target.
// v79 (G4, task CB): `plan` is the member's planned task chain and `step` the step it runs now (goap.hpp; empty = none yet).
struct Routine
{
	static constexpr std::string_view kName = "Routine";
	std::uint16_t goal = 0;
	std::uint64_t since = 0;
	std::uint64_t holdUntil = 0;
	RoutineSite target;
	double lastSleep = 0.0;
	std::vector<RoutineBlocked> blocked;
	std::uint64_t nextScore = 0;
	std::uint64_t chainTarget = 0;
	double chainElapsed = -1.0;
	// v79 (G4, task CB): the member's planned task chain (goap.hpp `plan`, execution order) and the step it runs now. Empty = no
	// plan (the executor plans once; a goal without a plan scores 0).
	std::vector<std::uint16_t> plan;
	std::uint16_t step = 0;
	// v83 (task patrol): the 216 town search. `searchBuilding` is the building the member searches (0 = none picked yet),
	// `searchPoint` the index of its current search point, `searchLookUntil` the tick its look-around there ends (0 = walking to
	// the point), `searched` the buildings it has searched recently (oldest first; the resolver reads the squad's union).
	std::uint64_t searchBuilding = 0;
	std::uint8_t searchPoint = 0;
	std::uint64_t searchLookUntil = 0;
	std::vector<RoutineSearch> searched;
	// v86 (task CH group 3): the shopping list (the member's copy of the template's `shopping` refs, dt24_shop). `shoppingDay`
	// is the game day the list was built (-1 = never); it is rebuilt once the hour passes 08:00 on a later day (the daily
	// reset). `shoppingTimer` is the exe's +0x98 interaction timer (seconds, started at 1.0), `shoppingLatch` its +0x9c
	// "transaction ran" byte: a failed attempt after a success ends the goal. `shoppingShop` is the shop building 118 picked and
	// `shoppingX`/`shoppingZ` its counter (where the 119 transaction runs; the scorer walks there).
	std::vector<ShoppingWant> shopping;
	std::int32_t shoppingDay = -1;
	double shoppingTimer = 0.0;
	bool shoppingLatch = false;
	std::uint64_t shoppingShop = 0;
	double shoppingX = 0.0;
	double shoppingZ = 0.0; // 128 / 127 also keep their walk's destination point in shoppingX / shoppingZ (no shopping goal runs then)

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.goal);
		ar(self.since);
		ar(self.holdUntil);
		ar(self.target);
		ar(self.lastSleep);
		ar(self.blocked);
		ar(self.nextScore);
		// Save v68: the slave chain. A v67 save loads with no chain.
		if (ar.version >= 68)
		{
			ar(self.chainTarget);
			ar(self.chainElapsed);
		}
		// Save v82: the GOAP plan. An older save loads with no plan (the executor plans the running goal once).
		if (ar.version >= 82)
		{
			ar(self.plan);
			ar(self.step);
		}
		// Save v83: the 216 town search. An older save loads with no search (the next pass picks a building).
		if (ar.version >= 83)
		{
			ar(self.searchBuilding);
			ar(self.searchPoint);
			ar(self.searchLookUntil);
			ar(self.searched);
		}
		// Save v86: the shopping list and its timer. An older save loads with no list (built at the next evaluation) and a
		// zero timer (the first transaction fires at once, as the exe's start resets it to 1.0).
		if (ar.version >= 86)
		{
			ar(self.shopping);
			ar(self.shoppingDay);
			ar(self.shoppingTimer);
			ar(self.shoppingLatch);
			ar(self.shoppingShop);
			ar(self.shoppingX);
			ar(self.shoppingZ);
		}
	}
};

// On the squad entity of a squad whose members run a town routine (present while they are promoted). `homeLocked` is the original's
// pkg+0xe8 (one member locking covers the squad); `barTable` the squad's claimed table (Site.object), 0 = none. `cagesLocked` (v68,
// T3e2) is 1 once a warden locked the cages of its home (195); nothing unlocks one yet.
struct SquadRoutine
{
	static constexpr std::string_view kName = "SquadRoutine";
	std::uint8_t homeLocked = 0;
	std::uint64_t barTable = 0;
	std::uint8_t cagesLocked = 0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.homeLocked);
		ar(self.barTable);
		if (ar.version >= 68)
			ar(self.cagesLocked);
	}
};

// What a character owns (SIMLINK.md section 6 INVENTORY): item records with counts. `slot` says where an entry is: Carried
// (in the pack; stacks merge per record and quality), Weapon (the wielded weapon) or ArmourBase + n (worn in the ARMOUR `slot` n,
// AttachSlot). Equipped entries are single items and never merge. The combat numbers of the equipped pieces live in the
// state::Fighter (weapon, armour), rebuilt from the entry's ItemQuality on every equip; a fighter spawned by Simulation::spawnFighter starts with an Inventory holding them, and
// looting empties a body's. Replaces the pick-up STUB. `version` counts changes (1 at spawn); the INVENTORY record follows it.
struct Inventory
{
	static constexpr std::string_view kName = "Inventory";
	static constexpr std::uint8_t kCarried = 0;
	static constexpr std::uint8_t kWeapon = 1;
	static constexpr std::uint8_t kArmourBase = 2;
	using Entry = InventoryEntry;
	static constexpr std::uint8_t kBackpackSlot = kArmourBase + 12; // AttachSlot 12 BACKPACK
	// The weapon on the back, sheathed (plan task 7; rwh.102 4.1): still equipped, in no hand, needs no grid room. Task 6 moves it to kWeapon.
	static constexpr std::uint8_t kSheathed = kBackpackSlot + 1;
	std::vector<Entry> items;
	std::uint32_t version = 1;
	// The size of section 0 (`main`): 8 x 10 for a human (INVENTORY_GRID_STATIC (a)); a container's is its BUILDING `storage size`.
	std::uint8_t gridW = 8;
	std::uint8_t gridH = 10;
	// A producer's one output slot (state::Producer): 0 = every stack takes its item's own cap; else this is the cap of every stack in `main` (the
	// slot holds one stack of up to `produces` v0 units, however low the item's `stackable` is).
	std::uint32_t stackLimit = 0;

	// The worn backpack (the entry in the BACKPACK slot), nullptr when none.
	Entry* wornBackpack() noexcept
	{
		for (Entry& entry : items)
		{
			if (entry.slot == kBackpackSlot)
				return &entry;
		}
		return nullptr;
	}
	const Entry* wornBackpack() const noexcept { return const_cast<Inventory*>(this)->wornBackpack(); }

	// Adds `count` of an item as a new, unplaced entry (or onto an unplaced one of the same quality): the builder for spawn code and tests.
	// It knows no grid: grid::settle (inventory_grid.hpp) places, caps and merges. Bumps the version.
	void add(const std::string& itemSid, std::uint32_t count, std::uint8_t slot = kCarried, const ItemQuality& quality = {})
	{
		++version;
		if (slot == kCarried)
		{
			for (Entry& entry : items)
			{
				if (entry.slot == kCarried && entry.section == Entry::kUnplaced && entry.itemSid == itemSid && entry.quality == quality)
				{
					entry.count += count;
					return;
				}
			}
		}
		items.push_back({itemSid, count, slot, quality});
	}
	// Carried count of an item record, over all its stacks and qualities (the worn backpack's contents included).
	std::uint32_t carried(const std::string& itemSid) const
	{
		std::uint32_t total = 0;
		const auto sum = [&](const std::vector<Entry>& list)
		{
			for (const Entry& entry : list)
			{
				if (entry.slot == kCarried && entry.itemSid == itemSid)
					total += entry.count;
			}
		};
		sum(items);
		if (const Entry* bag = wornBackpack())
			sum(bag->contents);
		return total;
	}
	// Takes `count` of a carried item record out of the pack, from its first entries (the main grid in entry order, then the worn
	// backpack's; UNSURE which stack the original hands over when the order names only the record). The pieces taken, one per
	// stack, unplaced; empty (and no change) when fewer are carried or count is 0.
	std::vector<Entry> takeCarried(const std::string& itemSid, std::uint32_t count)
	{
		std::vector<Entry> taken;
		if (count == 0 || carried(itemSid) < count)
			return taken;
		const auto take = [&](std::vector<Entry>& list)
		{
			for (auto entry = list.begin(); entry != list.end() && count > 0;)
			{
				if (entry->slot != kCarried || entry->itemSid != itemSid)
				{
					++entry;
					continue;
				}
				const std::uint32_t part = std::min(entry->count, count);
				Entry piece = *entry;
				piece.count = part;
				piece.section = Entry::kUnplaced;
				if (part < entry->count)
					piece.contents.clear();
				piece.planted.clear();
				piece.takePlantedFrom(*entry, part);
				taken.push_back(std::move(piece));
				count -= part;
				entry->count -= part;
				entry = entry->count == 0 ? list.erase(entry) : entry + 1;
			}
		};
		take(items);
		if (Entry* bag = wornBackpack())
			take(bag->contents);
		++version;
		return taken;
	}
	// Removes up to `count` of a carried item; false (and no change) when fewer are carried.
	bool removeCarried(const std::string& itemSid, std::uint32_t count)
	{
		return !takeCarried(itemSid, count).empty();
	}

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.items);
		ar(self.version);
		ar(self.gridW);
		ar(self.gridH);
		ar(self.stackLimit);
	}
};

// A character's running `interact` order (SIMLINK.md section 13): it walks to the target's use point and acts on arrival.
// On the character's entity; any other order from the player removes it. `action` is simlink's InteractAction.
struct InteractTask
{
	static constexpr std::string_view kName = "InteractTask";
	std::uint64_t target = 0;
	std::uint8_t action = 0;
	std::uint32_t seq = 0; // the order's clientSeq
	std::uint8_t flags = 0; // order 60's trailing flags (v45): action 5 loot, bit 0 = open (nothing is taken)

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.target);
		ar(self.action);
		ar(self.seq);
		ar(self.flags);
	}
};

// A character's running prospecting order (SIMLINK.md order 79, prospect.hpp): it stands where it is for kProspectTicks ticks (1.0
// sim-second, measured), then PROSPECT 35 goes out. On the character's entity; any replacing order, a KO or death removes it.
struct ProspectTask
{
	static constexpr std::string_view kName = "ProspectTask";
	std::uint32_t ticks = 0; // ticks run so far

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.ticks);
	}
};

// One order waiting in a character's queue (order 1 move_to flags bit0, order 60 interact queue 1; SIMLINK.md section 13). It
// holds the intent, not a route: the server routes it from where the character stands when it starts. A move keeps the snapped
// goal the first validation found (x, y, z); an interact keeps its target and action.
struct QueuedOrder
{
	std::uint8_t kind = 0; // 1 move, 2 interact
	std::uint32_t seq = 0; // the order's clientSeq
	std::uint64_t target = 0; // interact: the target's id
	std::uint8_t action = 0; // interact: the InteractAction
	double x = 0.0; // move: the goal
	double y = 0.0;
	double z = 0.0;
	std::uint8_t flags = 0; // interact: order 60's trailing flags (v45)

	bool operator==(const QueuedOrder&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.kind);
		ar(self.seq);
		ar(self.target);
		ar(self.action);
		ar(self.x);
		ar(self.y);
		ar(self.z);
		ar(self.flags);
	}
};

// A character's queued orders, oldest first. On the character's entity; present only while non-empty. Any order that is not
// queued clears it (every queued interact then gets event 13 result 5, cancelled).
struct OrderQueue
{
	static constexpr std::string_view kName = "OrderQueue";
	std::vector<QueuedOrder> orders;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.orders);
	}
};

// One row of a character's jobs list (jobs.hpp, ARCHITECTURE "V1: the original's per-character jobs"). `type` is the exe's TaskData
// number (jobs17 Appendix A); `target` 0 = none; `jobId` is per character and never reused.
struct JobRow
{
	std::uint32_t jobId = 0;
	std::uint16_t type = 0;
	std::uint64_t target = 0;

	bool operator==(const JobRow&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.jobId);
		ar(self.type);
		ar(self.target);
	}
};

// A character's persistent jobs (SIMLINK JOBS 34, orders 76-78). On the character's entity; created by the first job order or
// JOBS toggle, kept after (the counter must not restart). `rows` order is priority. `running` is the jobId whose goal the sim
// started (0 = none), `runType` the goal's kind (58 / 57 first aid, 125 construction, 16 attack, 31 follow) and `runTarget` its target, so a player's
// order that replaced the goal is told from a goal still running.
struct Jobs
{
	static constexpr std::string_view kName = "Jobs";
	bool enabled = true;
	std::uint32_t nextJobId = 1;
	std::vector<JobRow> rows;
	std::uint32_t running = 0;
	std::uint16_t runType = 0;
	std::uint64_t runTarget = 0;

	bool operator==(const Jobs&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.enabled);
		ar(self.nextJobId);
		ar(self.rows);
		ar(self.running);
		ar(self.runType);
		ar(self.runTarget);
	}
};

// One material row of a building under construction (buildings.hpp): the item record, how many units the BUILDING record's
// `construction` list wants, and how many a builder has delivered. The list keeps the record's order.
struct BuildMaterial
{
	std::string itemSid;
	std::uint32_t need = 0;
	std::uint32_t have = 0;

	bool operator==(const BuildMaterial&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.itemSid);
		ar(self.need);
		ar(self.have);
	}
};

// A player-placed building (BUILDINGS_SLICE.md section 2, SIMLINK BUILDING 47): an instance entity in the character id space. The
// BUILDING record is the template; everything live is here. While `built` is false `hp` is the construction progress
// (progress = hp / maxHp) and the building has no function. `status` is derived (buildings.hpp statusOf) but saved, so the state hash
// does not depend on step order. `version` bumps with every change the client must hear of (not with hp alone: the client
// extrapolates progress from the rate). Phase 1-2 fields only; the power, battery and production fields come with their phases.
struct PlayerBuilding
{
	static constexpr std::string_view kName = "PlayerBuilding";
	std::string recordSid;
	std::uint64_t ownerFaction = 0; // the player faction's state::Id
	std::uint64_t town = 0; // PlayerTown Id, 0 = none
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double qw = 1.0; // rotation, W first like tmpl::Rotation
	double qx = 0.0;
	double qy = 0.0;
	double qz = 0.0;
	std::uint8_t floor = 0;
	bool built = false;
	bool paused = false; // construction paused (order 94); only while !built
	bool broken = false;
	bool dismantling = false;
	bool switchedOn = true;
	bool runOffBattery = false;
	bool repairWanted = false;
	// Save v88 (ds/construction; b50c_static section 0): `ruin` is the destroyed state (CS +0x1a1: the first delivery or build tick
	// revives it at 0.4 max HP), `dismantled` the "destroyed for good" byte (CS +0x162: HP ran out, the refund was dropped), `furniture`
	// and `mounted` what Task_Dismantle refuses over (the interior's furniture, the mounted buildings; UNSURE: nothing in the sim
	// furnishes a player building yet, so they are set by the scenario or a later phase).
	bool ruin = false;
	bool dismantled = false;
	std::uint16_t furniture = 0;
	std::uint16_t mounted = 0;
	double hp = 0.0;
	double maxHp = 1.0;
	std::vector<BuildMaterial> mats; // frozen at creation, in the record's `construction` order
	std::uint16_t workers = 0; // builders on site at the last step (Phase 3 recounts it from the Build tasks; until then a scripted count)
	std::uint8_t status = 0; // buildings.hpp BuildStatus
	std::uint32_t version = 1;
	std::uint64_t placedTick = 0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.recordSid);
		ar(self.ownerFaction);
		ar(self.town);
		ar(self.x);
		ar(self.y);
		ar(self.z);
		ar(self.qw);
		ar(self.qx);
		ar(self.qy);
		ar(self.qz);
		ar(self.floor);
		ar(self.built);
		ar(self.paused);
		ar(self.broken);
		ar(self.dismantling);
		ar(self.switchedOn);
		ar(self.runOffBattery);
		ar(self.repairWanted);
		if (ar.version >= 88)
		{
			ar(self.ruin);
			ar(self.dismantled);
			ar(self.furniture);
			ar(self.mounted);
		}
		ar(self.hp);
		ar(self.maxHp);
		ar(self.mats);
		ar(self.workers);
		ar(self.status);
		ar(self.version);
		ar(self.placedTick);
	}
};

// A player outpost (BUILDINGS_SLICE.md section 2, D9): created by the first building with `creates player town` out of reach of the
// faction's others. Not a TownState (that one is record-derived and the squad AI iterates it). `radius` is the "too close" reach
// against other players' placements (UNSURE, Q3). `members` are the building Ids, ascending.
struct PlayerTown
{
	static constexpr std::string_view kName = "PlayerTown";
	std::string name;
	std::uint64_t owner = 0;
	double x = 0.0;
	double z = 0.0;
	double radius = 0.0;
	std::vector<std::uint64_t> members;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.name);
		ar(self.owner);
		ar(self.x);
		ar(self.z);
		ar(self.radius);
		ar(self.members);
	}
};

// Singleton: the client camera's focus (command SetView; SIMLINK.md section 5). Absent until the
// first SetView. The full-detail view area is the focus zone plus its 8 neighbours.
struct ViewFocus
{
	static constexpr std::string_view kName = "ViewFocus";
	double x = 0.0;
	double z = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.x);
		ar(self.z);
	}
};

// One weather region (weather.hpp): the state of one BIOME_GROUP's seasons and current weather. Every region ticks whether or
// not anyone is in the group (WEATHER_CYCLE_LIVE.md). Times are game minutes since day 0 00:00. `started` is 0 until the first
// step picks a season end and a weather (a new game or a load whose ids no longer exist).
struct WeatherRegion
{
	std::string group; // BIOME_GROUP stringID
	std::uint32_t season = 0; // index into the group's seasons, sorted by their v0
	std::uint64_t seasonEndDay = 0;
	std::string weather; // WEATHER stringID of the current instance
	std::string previous; // the weather before it, empty after a fresh start
	std::uint64_t startMinute = 0;
	std::uint64_t endMinute = 0;
	double strength = 0.0; // 0..1, rolled once per instance
	double effectStrength = 0.0; // "effect strength" rolled once per instance (UNSURE what it scales)
	std::uint64_t rng = 0; // this group's stream
	std::uint8_t started = 0;
	// A debug pin holds this weather past its season end and lifetime (WeatherSystem::pin; SIMLINK "DEBUG_SET_WEATHER").
	// Transient: not serialized, so a save stores the pinned weather as the current one and the hold ends with the restart.
	std::uint8_t held = 0;

	bool operator==(const WeatherRegion&) const = default;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.group);
		ar(self.season);
		ar(self.seasonEndDay);
		ar(self.weather);
		ar(self.previous);
		ar(self.startMinute);
		ar(self.endMinute);
		ar(self.strength);
		ar(self.effectStrength);
		ar(self.rng);
		ar(self.started);
	}
};

// Singleton: every weather region, in BIOME_GROUP record order. Absent in a world without game data.
struct WeatherRegions
{
	static constexpr std::string_view kName = "WeatherRegions";
	std::vector<WeatherRegion> regions;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.regions);
	}
};

template <typename... T>
struct TypeList
{
};

// The state of every lock a character picked, cut or forced (save v87; the one Lock model of lock_tasks.cpp): a cage building's, keyed by
// the building object id, and a worn pair of shackles', keyed by the wearer's member id. A lock nobody touched has no entry. `open` is the
// unlocked or broken state, elapsed / interval / attempts the picking timer every picker shares, hardness the work value a cut drains
// (< 0 = not read from the data yet).
struct LockStates
{
	static constexpr std::string_view kName = "LockStates";
	struct Entry
	{
		bool shackles = false; // false = a cage building's lock, true = a wearer's shackles
		std::uint64_t id = 0;
		double elapsed = 0.0;
		double interval = 5.0;
		std::uint32_t attempts = 0;
		double hardness = -1.0;
		bool open = false;

		template <typename Archive, typename Self>
		static void fields(Archive& ar, Self& self)
		{
			ar(self.shackles);
			ar(self.id);
			ar(self.elapsed);
			ar(self.interval);
			ar(self.attempts);
			ar(self.hardness);
			ar(self.open);
		}
	};
	std::vector<Entry> entries;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		if (ar.version >= 87)
			ar(self.entries);
	}
};

// The G1 package Board's runtime state (save v92; ksim/blackboard.hpp, kinspect dt19 section 0: the exe saves "replacementAI" and
// "runningAwayMode"). One entry per squad that holds a Board, keyed by the squad's Id. The priority table is derived from the data and
// rebuilt on load; what the data cannot rebuild is saved: the `change AI` replacement package (its sid, "" = the table is the data's),
// the overriding contract (sid, signal func, expiry in game hours), the applied package and its priority, the reapply flag, running
// away, the sticky cage hand and the blood smell. A singleton, not a component, so a squad gains no archetype (TOWN_LIFE_SLICE.md).
struct BoardStates
{
	static constexpr std::string_view kName = "BoardStates";
	struct Entry
	{
		std::uint64_t squad = 0;
		std::string replacement;
		std::string contract;
		std::int32_t contractSignal = 0;
		double contractExpiry = 0.0;
		std::string current;
		std::int32_t currentPriority = 0;
		bool reapply = false;
		bool runningAway = false;
		std::uint64_t stickyCageBuilding = 0;
		std::uint16_t stickyCageSlot = 0;
		bool hasSmell = false;
		double smellX = 0.0;
		double smellZ = 0.0;
		double startedAt = 0.0; // v94: the F classes' per-package state (dt41 F): when the applied package started,
		std::int32_t traderState = 0; // the wandering trader's 0 / 1 / 2,
		double occupyHeld = 0.0; // and how long OccupyTown's condition has held
		std::vector<std::uint64_t> owned; // v95: the ownership set (fighter ids) and the slaver packages' depot (a town Id), kinspect rwh.8
		std::uint64_t depot = 0;

		template <typename Archive, typename Self>
		static void fields(Archive& ar, Self& self)
		{
			ar(self.squad);
			ar(self.replacement);
			ar(self.contract);
			ar(self.contractSignal);
			ar(self.contractExpiry);
			ar(self.current);
			ar(self.currentPriority);
			ar(self.reapply);
			ar(self.runningAway);
			ar(self.stickyCageBuilding);
			ar(self.stickyCageSlot);
			ar(self.hasSmell);
			ar(self.smellX);
			ar(self.smellZ);
			if (ar.version >= 94)
			{
				ar(self.startedAt);
				ar(self.traderState);
				ar(self.occupyHeld);
			}
			if (ar.version >= 95)
			{
				ar(self.owned);
				ar(self.depot);
			}
		}
	};
	std::vector<Entry> entries; // by squad Id, ascending

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		if (ar.version >= 92)
			ar(self.entries);
	}
};

// What a save holds besides the Clock and Ids singletons. Adding a saved
// component, relationship or singleton is one entry here (plus a save
// format version bump, docs/formats/RUNTIME_SAVE.md).
using SavedComponents = TypeList<FactionState, TownState, SquadInstance,
	CharacterInstance, Position, Mover, GroupAI, FactionAI, Fighter, Door, GroundItem, Inventory,
	InteractTask, OrderQueue, Allegiance, DoorStep, Container, CarryStep, TakeOut, GateCrew, GateDuty, CrimeRecord,
	PlayerFaction, PlayerSquad, Jobs, ProspectTask, PlayerBuilding, PlayerTown, TraderStock, TraderTrip, MemberRole, Routine, SquadRoutine, Producer, CorpseDecay, Conversation, DialogueMemory, ItemNode,
	TownStock, EconomyProducer, CaravanOrder, SupplyBoard, CropPlot, TalkHold, MemberDelta, MemberSlot, StragglerOf, ExtraTown>;
using SavedRelations = TypeList<MemberOf, Relation>;
using SavedSingletons = TypeList<Scheduler, StandInGroups, PlayerAreas, WildernessState,
	FactionEvents, DoorOverrides, ViewFocus, WeatherRegions, GatesGone, DialogueShown, BuildingLooks, EconomySettings,
	WorldEventState, WorldOptions, LockStates, BoardStates, ArrestStates, RunStates>;

// Registers the state components and relationship traits. Idempotent.
void registerComponents(flecs::world& ecs);

// Creates an entity with the next Id.
flecs::entity createInstance(flecs::world& ecs);

} // namespace ksim::state
