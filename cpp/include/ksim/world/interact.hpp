#pragma once

// Interactables and the generic `interact` order (docs/formats/SIMLINK.md section 13).
//
// Doors and ground items are instance entities (state::Door, state::GroundItem) with a state::Id from the same counter as
// the characters, so one u64 names any of them in orders and events. The `interact` order sends a character to the target's
// use point (the server routes it with the navmesh, like move_to) and acts on arrival: Simulation::stepInteractions, after
// the combat step, finishes every state::InteractTask whose walk has ended.
//
// The rules (checkInteract) answer the order and are asked again on arrival (a door may have changed meanwhile). The
// default action of a click is the client's job (docs/reference/CURSORS_LIVE.md); the server only validates.
//
// Doors (the measured model, DOORS.md section 6): state::Door is the owner of a door's state in a navmesh scenario;
// Simulation::syncDoorPaint paints it on the mesh. A closed door costs the search nothing extra (the planner adds a step);
// a locked one is a wall to a player's order. A character that walks up to a closed door stops, opens it (a timed step,
// Simulation::stepDoorSteps) and walks on. A locked door is locked for everyone: a character of the building's faction
// (isObjectMine) that plans a walk through it unlocks it (kUnlockSeconds) and then opens it; a non-owner that is hostile to the
// owner bashes it (kBashInterval per hit, hp from the building) until it breaks (state 3, open for ever); any other waits.
// A player's move order never plans an unlock; the player unlocks with the interact action Unlock.
// Loot takes everything from a downed body into the looter's state::Inventory (Simulation::lootBody); a dropped item becomes
// a GroundItem (Simulation::dropItem).
//
// Carrying (carry.hpp): lift (action 8) and put down (action 13), the NPC take-out of KO'd intruders out of their building.
//
// Talk (action 6) opens a trade with a merchant (trade.hpp, docs/reference/MINE_SELL_SLICE.md M4); there is no dialogue yet. Not done /
// OPEN: sleep is a reserved action number the server refuses as `unsupported`; repair of a broken
// door; the gate system of doors.hpp keeps its own book for the whole-world navigators.

#include "ksim/nav/navigator.hpp"
#include "ksim/core/state.hpp"

#include <flecs.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace kdata {
class GameData;
struct Record;
}

namespace ksim {

class CombatArena;
class MeshNavigator;
class Simulation;

// The `action` byte of the interact order (SIMLINK.md section 13). 0 is invalid.
enum class InteractAction : std::uint8_t
{
	Open = 1,
	Close = 2,
	Lockpick = 3, // a locked door: the lock's timer ticks (first attempt 5 s, re-rolls 5-20 s) until one succeeds (door_plan.hpp)
	Use = 4, // work a producer (producers.hpp): walk to its use point, then stay and operate until it is full or the order is replaced
	Loot = 5,
	Talk = 6, // a merchant character: opens a trade (trade.hpp); any other conscious character: EV_PLAYER_TALK_TO_ME (DIALOGUE.md 13)
	PickUp = 7,
	Carry = 8, // lift a KO'd or dead character (carry.hpp): walk up, LIFT 1.17 s, then it rides along
	Sleep = 9, // a real bed (Simulation::bedOf by its site id): walk to its use node, then lie down (Fighter::asleep, the pose tail)
	FirstAid = 10,
	Unlock = 11, // a door of the actor's own faction (isObjectMine), kUnlockSeconds
	Bash = 12, // a shut door: hits until it breaks
	PutDown = 13, // target: the character the actor carries; PUT_DOWN 1.00 s, then it lies there (carry.hpp)
};
inline constexpr std::uint8_t kInteractActionMax = 13;
// Order 60 action 14 (client-approved 2026-11-10): the stealth KO, Simulation::orderStealthKo; not an InteractAction.
inline constexpr std::uint8_t kStealthKoAction = 14;
// Order 60 actions 15 / 16 / 17 (kenshi-h2v.15.1, client-approved 2026-11-11): not InteractActions either. The target is the character wearing the
// locked shackles: 15 picks them (task 201), 16 cuts them (185), 17 breaks them (186). Queue 1 and any flag are bad_value.
inline constexpr std::uint8_t kPickShacklesAction = 15;
inline constexpr std::uint8_t kCutShacklesAction = 16;
inline constexpr std::uint8_t kBreakShacklesAction = 17;
// Order 60 actions 18 / 19 / 20 (kenshi-k16.5.7, client-approved 2026-11-13, kenshi-k16.5.6): the same three on a cage's lock (tasks 76 / 285 / 286).
// The target is the cage's building key (CAGES), not a character; Simulation::orderShackleAction runs them.
inline constexpr std::uint8_t kPickCageAction = 18;
inline constexpr std::uint8_t kCutCageAction = 19;
inline constexpr std::uint8_t kBreakCageAction = 20;
// MENU_ENTRIES bits for a cage target (order 99): 0-15 are character gates (bit 0 talk), 16-31 are non-character targets.
inline constexpr std::uint32_t kMenuPickCage = 1u << 16;
inline constexpr std::uint32_t kMenuCutCage = 1u << 17;
inline constexpr std::uint32_t kMenuBreakCage = 1u << 18;
// Order 60 action 21 (kenshi-k16.6.816, client-approved 2026-11-25 on kenshi-8vju): lock a shut door the actor's faction owns. Not an InteractAction: the
// wire turns it into Close + kInteractFlagLock (Simulation::orderLockDoor's order), and event 13 names it 21 again. Queue 1 and any flag are bad_value.
inline constexpr std::uint8_t kLockDoorAction = 21;
// MENU_ENTRIES bits for a door target (order 99): 19 Lock (action 21), 20 Unlock (action 11); each is the order's own gate.
inline constexpr std::uint32_t kMenuLockDoor = 1u << 19;
inline constexpr std::uint32_t kMenuUnlockDoor = 1u << 20;
// Order 60's trailing flags (SIMLINK.md section 13). Action 5 loot: bit 0 = open (the walk ends without taking anything; the
// body's or container's INVENTORY + ITEM_GRID are then kept current for the looter's session). Other bits: bad_value.
inline constexpr std::uint8_t kInteractFlagOpen = 1;
inline constexpr std::uint8_t kInteractFlagsMask = kInteractFlagOpen;
// Internal (never on the wire, outside kInteractFlagsMask): a Close order that locks the shut door instead (LOCK_DOOR 77 / 162 / 198;
// Simulation::orderLockDoor). On the wire it is order 60 action 21 (kLockDoorAction).
inline constexpr std::uint8_t kInteractFlagLock = 2;
// Internal: a Carry that is the 240 arrest lift, whose precondition is isTargetDown (not unconscious).
inline constexpr std::uint8_t kInteractFlagArrestLift = 4;
// Symbol 7 isTargetDown (rwh.124 4.1) on a fighter: KO'd, dead, carried, caged, ragdolled (down) or crouched (posture >= 2).
bool isTargetDown(const state::Fighter& f);

// How near the use point a character must stand to act (xz units; 3D height within kInteractReachY). UNSURE: a guess.
inline constexpr double kInteractReach = 12.0;
inline constexpr double kInteractReachY = 30.0;
// The vertical half of the reach rule (rwh127 s2: isAtOrCanReachTarget): |dy| < 10, or < 35 on the same floor (ours: the walkable ground counts as one
// floor). A body a storey up or down is out of reach: the actor walks (or gives up), it never strips or lifts it through the floor.
inline constexpr double kBodyReachY = 35.0;
// A producer's: its arrival is horizontal only (MQ9); this keeps a mine from being worked through a floor or from a cliff top.
// UNSURE: the value is ours (the iron deposit's walkable ring lies 10-36 above its use point).
inline constexpr double kProducerReachY = 80.0;
// The producer approach point (kinspect item59 MQ9): this far from the origin toward the actor, snapped to the navmesh within the second.
inline constexpr double kProducerApproach = 65.0;
inline constexpr double kProducerApproachSnap = 10.0; // then kProducerApproachSnapFar (the original's 10 / 50 search; kenshi-rwh.299)
inline constexpr double kProducerApproachSnapFar = 50.0;
// A shut door's face on the actor's side (routeToInteract): this far out from the slab, snapped to the navmesh within the second. Ours.
inline constexpr double kDoorFaceOut = 2.0;
inline constexpr double kDoorFaceSnap = 6.0;
// The shared reach rule (MQ13, kinspect item 60: AI::isAtOrCanReachTarget). Talk, Trade / Loot, ground pickup and Operate all measure xz
// centre to centre. The threshold is tight while the actor moves and larger once it has stopped. A character target: R + 7 x 3 (a human
// 23.6; 44.6 stopped); a KO'd one: R + 7 x 2 (16.6; 30.6); R = the race's `pathfind footprint radius`. A ground item: 21 (42). A building:
// its `use range` (x 2 stopped). A point goal (a move order): 12, which kInteractReach keeps for a door. Heights stay kInteractReachY
// (the exe's is |dy| < 10, or < 35 on the same floor; 15 / 45 / 130 by building kind: not modelled).
inline constexpr double kReachStep = 7.0;
inline constexpr double kReachGroundItem = 21.0;
inline constexpr double kReachStorage = 25.0; // a General storage's `use range`; UNSURE for the other containers (the record is not read)
// A container order ends at the box's use point, not within a reach of its centre (kenshi-rwh.95: the original walks the navmesh route to the use
// point, 0.3 u of the door's, and never opens across a counter). Ours is an arrival tolerance round that point: the use point of a box stands in
// its own furniture, the route ends at the mesh point nearest it, and the walker's clearance radius keeps that end off the wall: the Hub bar's
// boxes end 4.9-6.3 u off (ksim_counter_path), not the 0.3 u of the live door run. Narrow enough that the customer side of a counter (>= 13 u from
// the stock) is not arrived.
inline constexpr double kArriveContainer = 8.0;
// A character target's reach; `down` = KO'd or dead (the exe: carried or KO'd), `stopped` = the actor stands still.
inline constexpr double characterReach(double footprintRadius, bool down, bool stopped) noexcept
{
	return footprintRadius + kReachStep * (down ? 2.0 : 3.0) * (stopped ? 2.0 : 1.0);
}
// A layout piece's pre-task reach (dt22, the s69 reach gate): the `use range` of the piece's fitting functionality record
// (`function`, routines.hpp kFunction*), 20 when the record or the field is missing, x1 while the actor moves and x2 once it
// stands still - the same rule the targets above carry as reach / reachStopped. A cage 25 (50 stopped), a storage 25, a bed 15.
double functionalityReach(const kdata::GameData&, std::string_view pieceSid, std::int32_t function, bool stopped) noexcept;
// A character walking a route stops at a closed or locked door within this many xz units of its centre (when its route passes
// within kDoorPassReach of the centre). UNSURE: the original's approach points are not read.
inline constexpr double kDoorStopReach = 14.0; // plus two ticks of the walker's speed: it must stop before the mat (the paint blocks a locked one)
inline constexpr double kDoorPassReach = 8.0;
// MEASURED (DOORS_LIVE.md): the door toggles when its openness crosses 0.6. Openness grows by `door move speed` per second
// (MEASURED, item 19): an open or close takes 1 / speed s in all; the state flips at this fraction of it.
inline constexpr double kDoorToggleOpenness = 0.6;
// MEASURED: UNLOCK_DOOR takes about 3.3 s (18 unlock ticks, ~420 frames), no skill.
inline constexpr double kUnlockSeconds = 3.3;
// MEASURED: one hit per basher every 2.6-2.9 s; a hit takes clamp(2 blunt + 0.5 cut, 20, 60) x 0.015 hp.
inline constexpr double kBashInterval = 2.7;
// A character that may not pass waits this long for the door to change, then gives up its walk. UNSURE (ours).
inline constexpr double kDoorWaitCap = 60.0;
// The hp a hit takes off a door from the basher's weapon (blunt, cut).
double bashDamage(double blunt, double cut) noexcept;

// What an interact target is. Wire kinds of INTERACTABLE records are Door = 1, GroundItem = 2, Container = 3 (this enum's
// Container is 4: Character takes 3 here and is never an INTERACTABLE record).
enum class InteractKind : std::uint8_t
{
	None = 0,
	Door = 1,
	GroundItem = 2,
	Character = 3, // a fighter (loot, first aid); never an INTERACTABLE record
	Container = 4, // a storage building (state::Container); loot takes all
	Producer = 5, // a mine, deposit or drill (state::Producer, producers.hpp); use works it
	Machine = 8, // a placed converter or turret (a kind 12 wire row, World::machineSites); no entity: id names the site. Use operates / mans it (kenshi-k16.6.788)
	Bed = 6, // a real layout bed (Simulation::bedOf): just after a site id (routines::siteId); action 9 sleeps in it
	Cage = 7, // a cage building's key (Simulation::cageOf): a carrier's order on it, any action, is the put-in (exe 1407f9e20: N -> 0x6c)
};

// How close a sleeper must stand to a bed's use point (UNSURE: the exe's own reach is not measured).
inline constexpr double kReachBed = 4.0;
// How close a carrier must stand to a cage's walk spot to put its body in (OURS, as the planner's put-in reach: functionalityReach).
inline constexpr double kReachCage = 6.0;

struct InteractTarget;

// The route of an interact order to its target (every order 60 and queued order goes through this). A Talk to a character picks the
// stand-up GOAL first (MeshNavigator::routeToStand: a point within the stopped reach with a clear line, the shortest path of them
// wins; the target's own position only when none is found), so a barman behind a counter is talked to across it instead of
// the walk circling round behind (docs/FEATURES.md "Talk / trade approach stops on the near side of a counter"). Every other
// action routes to the target's use point as before, but a door whose centre the walk cannot reach (shut or locked: its polygons are a
// wall) routes to its face on the actor's side. `planned` and `body` as MeshNavigator::routeFrom.
Route routeToInteract(const MeshNavigator& nav, const state::Fighter& who, const InteractTarget& target, std::uint8_t action,
	bool planned, Body body);

struct InteractTarget
{
	InteractKind kind = InteractKind::None;
	flecs::entity_t entity = 0; // the door or item entity; 0 for a character
	std::uint64_t id = 0;
	// Where to stand: the use point (a character's is where it lies now).
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	// A wide target (a door slab): the footprint about x, z, a unit long axis and the half extents along and across it. The
	// reach is measured from its edge. A shut door's polygons are a wall to the walk, which ends at their edge, not at the
	// centre (the kenshi-0f bug: a lockpick order on the Defensive Gate ended 12.4 units short of its centre, event 13 result 2).
	double axisX = 1.0;
	double axisZ = 0.0;
	double halfLong = 0.0;
	double halfDeep = 0.0;
	// How near a character must stand to act: kInteractReach, or a producer's `use range` (100 on a mine; operators count as arrived within it).
	double reach = kInteractReach;
	// The same once the actor has stopped (the exe's hysteresis; reachFor picks).
	double reachStopped = kInteractReach;
	// How far above or below the target the actor may stand (a producer's arrival is horizontal only, kinspect item59 MQ9).
	double reachY = kInteractReachY;
	// A producer's collision bounds (xz), all 0 = none: standing inside them x 1.2 about their centre also counts as arrived (MQ9:
	// the iron deposit's use range 50 to the origin lies inside its own rock).
	double boxMinX = 0.0;
	double boxMaxX = 0.0;
	double boxMinZ = 0.0;
	double boxMaxZ = 0.0;

	// The reach for `who`: the tight value while it walks a route, the doubled one when it stands.
	double reachFor(const state::Fighter& who) const noexcept { return who.routeNext < who.route.size() ? reach : reachStopped; }

	// The xz distance from (px, pz) to the target: to its footprint's edge (0 inside), a point's own distance for the rest.
	double distanceTo(double px, double pz) const noexcept
	{
		const double dx = px - x;
		const double dz = pz - z;
		const double along = std::max(std::fabs(dx * axisX + dz * axisZ) - halfLong, 0.0);
		const double across = std::max(std::fabs(-dx * axisZ + dz * axisX) - halfDeep, 0.0);
		return std::hypot(along, across);
	}

	// Near enough at a point: within `reach` and reachY, or inside the bounds x 1.2. A route's end is where the walker
	// stops, so it is asked with reachStopped (routeToInteract).
	bool nearEnough(double px, double py, double pz, double reach) const noexcept
	{
		if (std::fabs(py - y) > reachY)
			return false;
		if (distanceTo(px, pz) <= reach)
			return true;
		if (boxMaxX <= boxMinX || boxMaxZ <= boxMinZ)
			return false;
		const double halfX = 0.6 * (boxMaxX - boxMinX);
		const double halfZ = 0.6 * (boxMaxZ - boxMinZ);
		return std::fabs(px - 0.5 * (boxMinX + boxMaxX)) <= halfX && std::fabs(pz - 0.5 * (boxMinZ + boxMaxZ)) <= halfZ;
	}
	// Near enough for `who` to act.
	bool within(const state::Fighter& who) const noexcept
	{
		return nearEnough(who.x, who.y, who.z, reachFor(who));
	}
	// Arrived for an order: within() for everything but a container, whose order ends at its use point (kArriveContainer), not across a counter from
	// within the reach (kenshi-rwh.95). The reach stays the one MQ13 measures: a transfer window already open is checked against it.
	bool arrived(const state::Fighter& who) const noexcept
	{
		return kind == InteractKind::Container ? nearEnough(who.x, who.y, who.z, kArriveContainer) : within(who);
	}
};

// The door, ground item or fighter with this id; kind None if there is none.
// `sim`: resolves a bed's site id (routines::siteId); null in the callers that never see one.
InteractTarget findInteractTarget(const flecs::world& ecs, const CombatArena* arena, std::uint64_t id, const class Simulation* sim = nullptr);

// isObjectMine: the door's building faction is the actor's (state::Allegiance). The faction-owns-it branch of the original
// (bought player buildings) is not modelled.
bool isObjectMine(const flecs::world& ecs, std::uint64_t actor, const state::Door& door);

// Whether taking from a container is theft: it has an owner faction and the actor is not a member (state::Allegiance).
bool isForeignContainer(const flecs::world& ecs, std::uint64_t actor, const state::Container& container);
// The same rule for any storage owner faction: `ownerSid` is set and the actor's state::Allegiance is another faction (a container's
// owner, or the faction of a KO'd or dead body).
bool isForeignOwner(const flecs::world& ecs, std::uint64_t actor, const std::string& ownerSid);
// The faction of the character `actor` (state::Allegiance), "" without one.
std::string factionOf(const flecs::world& ecs, std::uint64_t actor);
// Item provenance, where the stolen mark clears (dt43 D section 4, ARCHITECTURE "Item provenance"): a stack that lands in a pack or box held for
// its victim faction `holderSid` (a container's owner, a body's faction, the taking character's own), or in shop stock (`shopStock`: a
// fence launders), is clean. Its nested contents go with it.
void clearReturnedMark(state::Inventory::Entry& piece, const std::string& holderSid, bool shopStock);

// Empty when `actor` may start (or finish) `action` on `target`; else the reason for event 10: "bad_target" (no such target
// or the action does not fit its kind), "not_allowed" (the target's state refuses: a locked, open or shut door, a standing
// character), "unsupported" (a reserved action). `flags`: order 60's trailing flags; loot with the open bit answers "bad_target" for
// a character that is up and conscious (take-all keeps "not_allowed"). Talk: "bad_target" (not a character, yourself), "not_allowed" (the
// target is down, asleep, or a merchant whose faction is hostile to the player faction: needs `data`).
std::string checkInteract(const flecs::world& ecs, const CombatArena& arena, std::uint64_t actor, const InteractTarget& target, std::uint8_t action,
	std::uint8_t flags = 0, const kdata::GameData* data = nullptr, const class Simulation* sim = nullptr);

// A lockpick order whose chance is at or below the planner's floor: the original's goal (iHaveSufficientLockSkill) cannot be planned, so the
// order starts no walk and no pick, says nothing, and leaves the character as it was (kenshi-rwh.178). The sim drops such an order.
bool pickUnplannable(const flecs::world& ecs, const state::Fighter& actor, const InteractTarget& target, std::uint8_t action);

// Makes a state::Door entity for every placed door that has polygons in the navmesh (Hub scenarios call it once after the
// characters spawn). Position and use point: the centre of the door's polygons. Initial state: Closed, or Locked for a door
// whose record says `always locks` (UNSURE: the data has no other initial state for a placed door; SQUAD_TEMPLATE
// `initial door state` is about homes). A door that already has an entity (same key) is skipped, so it may be called again when a
// streaming mesh brings zones in (Simulation::syncNavCoverage). Returns how many were made.
std::size_t spawnMeshDoors(Simulation& sim, const MeshNavigator& navigation);

// Makes a state::Container (with its state::Inventory) for every placed `has inventory` building whose position lies within
// kContainerSnap of the navmesh, with the owner of the nearest town within 1,500 units as its faction (UNSURE, as for doors). The
// data holds no per-container contents; each container is filled with kContainerDraws items drawn, weighted by the entry's v0,
// from the VENDOR_LIST `contents` (nullptr = empty containers). UNSURE: the draw count and the list (the Hub's own item spawn
// lists are not read). Returns how many.
std::size_t spawnMeshContainers(Simulation& sim, const MeshNavigator& navigation, const kdata::Record* contents);
// One container at (x, y, z) on the mesh (snapped within five times kContainerSnap), contents drawn from `contents` with the dice
// of `key`; the scenarios' stand-in for the Hub's interior furniture, which the original generates at runtime (a save holds it as
// `serialised interior`) and no static data places. Invalid entity when the point is off the mesh.
flecs::entity spawnContainerAt(Simulation& sim, const MeshNavigator& navigation, const kdata::Record* contents,
	const std::string& recordSid, double x, double y, double z, std::uint64_t key);
// Makes a state::ItemNode for every ITEM_PLACEMENT_GROUP instance of a placed building's interior layout (WORLD_LAYOUT.md
// "Placement item nodes") whose point lies within kContainerSnap of the navmesh, each with its first item out. The owner is the
// building's (its doors' instance owner faction), else nobody (kinspect itemgroup_static.md c). A node that already has an entity is
// skipped (a refurnished zone, a load). Returns how many were made.
std::size_t spawnMeshItemNodes(Simulation& sim, const MeshNavigator& navigation);
inline constexpr double kContainerSnap = 20.0; // a container farther than this (3D) from the navmesh has no use point
inline constexpr int kContainerDraws = 3;
// A transfer needs the character within the container's reach (kReachStorage, a producer's `use range`, a body's character reach).
// The old fixed 16 is retired: kTransferReach stays as the floor of a container's reach in tests.
inline constexpr double kTransferReach = kReachStorage;

// What the simulation reports about an interaction (Simulation::setInteractObserver); SIMLINK events 7 and 13.
struct InteractEvent
{
	enum class Kind : std::uint8_t
	{
		PickedUp, // who took `item` (record sid) at x, y, z
		Dropped, // who put `item` on the ground at x, y, z
		Done, // who finished (result 0) or failed `action` on `target`
		LockRoll, // who rolled a lockpick attempt on the door `target`; result 1 = unlocked (SIMLINK event 20)
		Theft, // who took from `target` (a container) owned by the faction `item` (SIMLINK event 14); crime itself is out of scope
	};
	// Done results.
	static constexpr std::uint8_t kDone = 0;
	static constexpr std::uint8_t kRefused = 1; // the target's state no longer allows it
	static constexpr std::uint8_t kUnreachable = 2; // the walk ended short of the use point
	static constexpr std::uint8_t kGone = 3; // the target no longer exists
	static constexpr std::uint8_t kIncapacitated = 4; // the actor was knocked out or died
	static constexpr std::uint8_t kCancelled = 5; // replaced or cleared by an order that was not queued (running or queued)
	static constexpr std::uint8_t kFull = 7; // a producer reached its cap: every operator stops
	static constexpr std::uint8_t kTooHard = 8; // a lock pick reached the lock and the skill is below the floor: no pick ran (k16.6.377)
	static constexpr std::uint8_t kPartial = 6; // a pick-up or loot took some, the rest did not fit (nothing fitting is kRefused)

	Kind kind = Kind::Done;
	std::uint64_t who = 0;
	std::uint64_t target = 0;
	std::uint8_t action = 0;
	std::uint8_t result = 0;
	std::uint8_t flags = 0; // Done: the order's flags (loot open: bit 0)
	std::string item;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

} // namespace ksim
