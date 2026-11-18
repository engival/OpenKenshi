#pragma once

// Commands: the only external write path into simulation state, one queue
// for the whole tick driver (ARCHITECTURE.md "Runtime state": one tick
// driver). A command is plain data: it names instances by state::Id, never
// by flecs id, so a queued command survives a save and load, and a command
// log can be replayed.
//
// Simulation::enqueue stamps the tick a command applies at; it is applied
// at the start of that tick, after the Clock advances and before any system
// or region work, in enqueue order. Invalid commands are dropped then.

#include "ksim/core/state.hpp"

#include <cstdint>
#include <vector>

namespace ksim {

enum class CommandType : std::uint8_t
{
	// subject -> object FactionState instances, value -100..100: sets the
	// state::Relation pair (creates it if absent).
	SetFactionRelation = 0,
	// points: player positions; replaces the full-detail areas.
	SetPlayerAreas = 1,
	// value: stand-in group index; x, z: its new centre (region.hpp).
	SetSquadCentre = 2,
	// subject: the state::Id of a squad; the squad and its members are
	// removed (a kill). A wilderness squad's quota then refills after its
	// respawn delay (wilderness.hpp). object (optional): the state::Id of the
	// killer's FactionState; the squad's faction then lowers its opinion of it
	// by relation reason 5 for every member (factions.hpp, FACTIONS.md 3.1).
	RemoveSquad = 3,
	// subject: the state::Id of a TownState instance; object: the state::Id
	// of the new owner FactionState (0 = none). Faction events flow up: the
	// old owner loses the town, the new one gains it (factions.hpp).
	SetTownOwner = 4,
	// object: a door key (kworld::doorKey); value: knav::DoorState (0 open, 1 closed,
	// 2 locked), or -1 to clear the override. The override wins over the town's posture
	// (doors.hpp). Only the state changes: who may pass stays the town's policy.
	SetDoorState = 5,
	// Player orders for a full-detail character (SIMLINK.md section 7); subject: the fighter's
	// state::Id. Dropped when the fighter is unknown or dead.
	// points: the route the server made on the navmesh (x, z; the first is where the order was
	// given), object: the order's clientSeq, x, z: the destination, value: bit 0 queue (the
	// move waits in state::OrderQueue and is routed to the last point when it starts; otherwise it replaces the running
	// order and empties the queue), bit 1 approach point. With no navmesh or no points the command is dropped: no route, no move.
	FighterMoveTo = 6,
	// object: the enemy's Id; dropped when it is unknown or on the same team.
	FighterAttack = 7,
	// value: 1 hold position.
	FighterStop = 8,
	// value: 0 / 1 block mode.
	FighterBlock = 9,
	// x, z: the camera focus (state::ViewFocus). Not tied to a subject.
	SetView = 10,
	// Direct drive (SIMLINK.md section 12), subject: the fighter; value: 0 stop, 1 forward,
	// 2 backward; x: heading in radians; z: the strafe (-1 left, 0, +1 right); object: the order's clientSeq. Needs a navmesh.
	FighterDrive = 11,
	// Order 3 follow (SQUAD_FOLLOW.md): subject follows the fighter object. Dropped when either is unknown or
	// dead, they are the same, or the chain of leaders would loop back to the subject. Replaces any other order.
	FighterFollow = 12,
	// value: 0 / 1 sneak mode (toggle 1 stealth). Followers copy their leader's every tick.
	FighterSneak = 13,
	// Order 60 interact (SIMLINK.md section 13): subject goes to the use point of the interactable or character `object` and acts
	// (value: the InteractAction; x: the order's clientSeq; points: the route the server made on the navmesh, empty when the
	// subject already stands at the use point; z: 1 when the route ends at an approach point). Dropped when the subject is unknown
	// or dead, or the target is gone. Replaces any other order (the replaced interact and the queue get event 13 result 5)
	// unless z has bit 1 set (bits 2 and up: order 60's trailing flags, z >> 2): then it waits in the subject's state::OrderQueue (points empty, routed when it starts). The
	// action is checked again on arrival (interact.hpp). FighterMoveTo with value bit 0 is queued the same way.
	FighterInteract = 14,
	// Order 61 drop (SIMLINK.md section 13): subject drops `value` carried units of the item record `text` at its feet, as a
	// ground item (an INTERACTABLE; event 6). object: the order's clientSeq. Dropped when the subject is unknown or dead or
	// does not carry that many (the order was checked when it was given).
	FighterDrop = 15,
	// value: 0 walk / 1 run (toggle 2). Not a replacing order: a running walk keeps its route.
	FighterRun = 16,
	// Order 62 equip (SIMLINK.md section 7): subject moves the item record `text` between its pack and its weapon / armour
	// slot (value 1 equip a carried item, 0 unequip a worn or wielded one); object: the order's clientSeq. Instant (UNSURE).
	// Not a replacing order. Dropped when the subject does not have the item there or is down.
	FighterEquip = 17,
	// Order 63 transfer: subject and the container (state::Id `object`) swap `value` units of the item `text` (z: 1 take from
	// the container, 0 put into it); x: the order's clientSeq. Not a replacing order. Dropped when it no longer holds.
	ContainerTransfer = 18,
	// Orders 64-68 (grid inventory, SIMLINK.md section 7), see InventoryOrder: subject the character or container, object the
	// container of a transfer, value the order kind (64..68), heights = {version, index, count, section, x, y, flag}. Not a
	// replacing order. Checked again when it applies (inventory_orders.cpp); a failure drops it.
	InventoryOrder = 19,
	// Orders 70-75 and 85 faction_rename (player squads, SIMLINK.md section 7), see SquadOrder: value the order kind (70..75), subject the character (73,
	// 75) or the squad (71, 72, 74), object the target squad (73), x the index, z the swap flag, text the name (72). Not a
	// replacing order. Checked again when it applies (player_squads.cpp); a failure drops it.
	SquadOrder = 20,
	// A new game's player faction (SIMLINK NEW_GAME 111, MULTIPLAYER.md 4c, newgame.cpp): text = the NEW_GAME_STARTOFF sid, '\n', the
	// faction's name; object = the session's ticket (the outcome observer matches it). Checked again when it applies; a failure drops
	// it and the observers hear the reason.
	NewGame = 21,
	// Orders 76-78 and toggle 10 which 4 (the jobs list, SIMLINK.md JOBS 34, jobs.cpp), see JobOrder: value the order kind, subject the
	// character, object the target (76), x the type (76), the jobId (77, 78) or the enabled flag (10), z the index (78). Not a replacing
	// order. Checked again when it applies; a failure drops it.
	JobOrder = 22,
	// Order 79 prospect (SIMLINK.md, prospect.hpp): subject the character. A replacing order: it stands where it is for kProspectTicks
	// ticks (state::ProspectTask), then PROSPECT 35 goes out. Checked again when it applies (checkProspect); a failure drops it.
	Prospect = 23,
	// Order 91 build_place (SIMLINK.md, BUILDINGS_SLICE.md section 3), see BuildOrder: text the BUILDING record, x, z the origin,
	// heights = {y, qw, qx, qy, qz}, value the floor. Not a replacing order. Checked again when it applies (checkBuildPlace); a
	// failure drops it.
	BuildPlace = 24,
	// Order 94 build_pause (buildings.hpp): subject the building's Id, value 0 / 1 paused. Checked again when it applies.
	BuildPause = 25,
	// Order reply (DIALOGUE.md 3.5): subject the player character, object the speaker whose menu it answers, value the option index,
	// x the menuSeq. Checked again when it applies (checkDialogueReply); a failure drops it.
	DialogueReply = 26,
	// Order end_talk, or the server's forced end (the disconnect grace, an admin; DIALOGUE.md 13.3): subject the player character,
	// object the speaker, value the CONVERSATION_END reason (4 walked away, 5 no client). Checked again when it applies.
	EndConversation = 27,
	// Toggle which 5 (speed mode, SIMLINK.md; the exe's CharMovement cap +0xbc): subject the fighter, value the mode
	// 0 Walk, 1 Jog, 2 Run, 3 Group, object the group key when mode 3 (the lowest state::Id of the order's ids; else 0).
	// Not a replacing order: a move, a chase or a follow keeps running, capped.
	FighterSpeedMode = 28,
	// DA_JOIN_SQUAD_* (DIALOGUE.md 5; the data has no order for it): subject the character, object the player squad (0 = the player
	// faction's first). The character leaves its NPC squad and becomes a player member. Checked again when it applies (checkChangeSquad);
	// a failure drops it.
	ChangeSquad = 29,
	// Order 60 action 14, the stealth KO (SIMLINK.md, stealth_ko.hpp): subject the assassin, object the victim. A replacing order
	// (Simulation::orderStealthKo). Checked again when it applies; a refusal drops it.
	StealthKo = 30,
	// Order 60 actions 15 / 16 / 17 (kenshi-h2v.15.1): subject the actor, object the shackle wearer, value the wire action. A replacing
	// order (Simulation::orderShackleAction). Checked again when it applies; a refusal drops it.
	ShackleAction = 31,
	// value: 0 / 1 the PASSIVE toggle (SIMLINK.md order 10 which 6; k16.6.313). Never replaces the current order.
	FighterPassive = 32,
	// value: 0 / 1 the HOLD POSITION toggle (order 10 which 7). Never replaces the current order.
	FighterHoldPosition = 33,
};

// A building placement (SIMLINK orders 90 build_check and 91 build_place). Rotation is W first.
struct BuildOrder
{
	std::string record;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double qw = 1.0;
	double qx = 0.0;
	double qy = 0.0;
	double qz = 0.0;
	std::uint8_t floor = 0;
	bool scenario = false; // Simulation::placeScenarioBuilding: the unlocked, town, overlap and characters-in-way tests are skipped
};

// A player squad order (SIMLINK orders 70 squad_add, 71 squad_delete, 72 squad_rename, 73 squad_move, 74 squad_reorder, 75 dismiss).
struct SquadOrder
{
	static constexpr std::uint16_t kAppend = 0xFFFF;
	std::uint16_t kind = 0;
	std::uint64_t character = 0; // 73, 75
	std::uint64_t squad = 0; // 71, 72, 74; for 73 the target squad
	std::uint16_t index = 0; // 73: the slot (kAppend = last, 0 = leader); 74: the new list index
	std::uint8_t swap = 0; // 73
	std::string name; // 72, 85
};

// A jobs-list order (SIMLINK orders 76 job_add, 77 job_remove, 78 job_move, and kind 10 for toggle which 4, JOBS on / off) for one character.
struct JobOrder
{
	static constexpr std::uint16_t kLast = 0xFFFF;
	std::uint16_t kind = 0;
	std::uint64_t character = 0;
	std::uint16_t type = 0; // 76: the exe's TaskData number
	std::uint64_t target = 0; // 76
	std::uint32_t jobId = 0; // 77, 78
	std::uint16_t index = 0; // 78: kLast = last
	std::uint8_t enabled = 0; // 10
};

// A grid inventory order (SIMLINK orders 64 move_item, 65 arrange, 66 drop_entry, 67 equip_entry, 68 transfer_entry). `id` is
// the character, or for 64 / 65 a container; `version` the INVENTORY version the client saw (the source's for 68). flag: 67
// equip, 68 take. `item` (not on the wire) addresses the first matching entry by record when `index` is kByRecord: the
// legacy order 62 uses it. version kAnyVersion skips the stale test.
struct InventoryOrder
{
	static constexpr std::uint16_t kByRecord = 0xFFFF;
	static constexpr std::uint32_t kAnyVersion = 0xFFFFFFFFu;
	std::uint16_t kind = 0;
	std::uint64_t id = 0;
	std::uint64_t container = 0;
	std::uint32_t version = 0;
	std::uint16_t index = 0;
	std::uint16_t count = 0;
	std::uint8_t section = 0;
	std::uint8_t x = 0;
	std::uint8_t y = 0;
	std::uint8_t flag = 0;
	std::string item;
	// 68 against a merchant's stock only (not on the wire): the character the session is trading with, filled in by the Session from its
	// open trade; the simulation checks that it belongs to the squad `container` and is up. Carried by the Command's text.
	std::uint64_t trader = 0;
	// 68 sell only (wire: optional trailing u64, SIMLINK.md order 68): an owned container the goods come from instead of the actor's
	// pack (MQ12); `version` / `index` are then that container's. 0 = the actor's own inventory. Carried by the Command's text.
	std::uint64_t source = 0;
	// 68 against a merchant's stock only (not on the wire): the Session's id of the trade window the order was given in (OpenTrade::window; 0 =
	// none, e.g. a test without a Session). The buyback list belongs to one window: an order of another window drops it. Carried by the
	// Command's text, so a replay of the log sees the windows close and reopen.
	std::uint64_t window = 0;
};

struct Command
{
	// Clock tick the command applies at. Anything not after the current
	// tick (0 included) is stamped with the next tick by enqueue.
	std::uint64_t tick = 0;
	CommandType type = CommandType::SetFactionRelation;
	std::uint64_t subject = 0;
	std::uint64_t object = 0;
	std::int32_t value = 0;
	double x = 0.0;
	double z = 0.0;
	std::vector<state::GroundPoint> points;
	// FighterMoveTo and FighterInteract: the surface height of each route point (parallel to `points`), as the server's route
	// (Route::heights) has it. Never recomputed: a recomputation from the previous waypoint dropped routes that climb more than
	// 40 units between two corners (retired/simulation_moveTo_heights.txt). A command whose heights do not match its points is
	// a caller bug and is counted (Simulation::badOrders), never walked.
	std::vector<double> heights;
	// FighterDrop: the item record's stringID.
	std::string text;

	static Command setFactionRelation(state::Id from, state::Id to, std::int32_t value);
	static Command setPlayerAreas(std::vector<state::GroundPoint> centres);
	static Command setSquadCentre(std::int32_t group, double x, double z);
	static Command removeSquad(state::Id squad, state::Id killerFaction = {});
	static Command setTownOwner(state::Id town, state::Id faction);
	static Command setDoorState(std::uint64_t doorKey, std::int32_t state);
	static Command fighterMoveTo(state::Id subject, std::vector<state::GroundPoint> route, std::vector<double> heights,
		std::uint32_t clientSeq, bool queue = false, bool approach = false);
	static Command fighterDrive(state::Id subject, std::int32_t mode, double heading, std::uint32_t clientSeq, int strafe = 0);
	static Command fighterAttack(state::Id subject, state::Id target);
	static Command fighterStop(state::Id subject, bool hold);
	static Command fighterBlock(state::Id subject, bool on);
	static Command fighterFollow(state::Id subject, state::Id leader);
	static Command fighterSneak(state::Id subject, bool on);
	static Command fighterPassive(state::Id subject, bool on);
	static Command fighterHoldPosition(state::Id subject, bool on);
	static Command fighterRun(state::Id subject, bool run);
	// Toggle which 5: `mode` 0 Walk, 1 Jog, 2 Run, 3 Group; `group` the group key for mode 3 (0 otherwise).
	static Command fighterSpeedMode(state::Id subject, std::uint8_t mode, std::uint64_t group);
	static Command fighterInteract(state::Id subject, std::uint64_t target, std::uint8_t action, std::uint32_t clientSeq,
		std::vector<state::GroundPoint> route, std::vector<double> heights, bool approach, bool queue = false, std::uint8_t flags = 0);
	static Command fighterDrop(state::Id subject, std::string item, std::uint32_t count, std::uint32_t clientSeq);
	static Command fighterEquip(state::Id subject, std::string item, bool equip, std::uint32_t clientSeq);
	static Command containerTransfer(state::Id subject, std::uint64_t container, std::string item, std::uint32_t count, bool take,
		std::uint32_t clientSeq);
	static Command inventoryOrder(const InventoryOrder& order);
	static Command squadOrder(const SquadOrder& order);
	static Command jobOrder(const JobOrder& order);
	static Command prospect(state::Id subject);
	static Command stealthKo(state::Id subject, state::Id victim);
	static Command shackleAction(state::Id subject, state::Id wearer, std::uint8_t action);
	static Command buildPlace(const BuildOrder& order);
	static Command buildPause(std::uint64_t building, bool paused);
	static Command dialogueReply(state::Id character, state::Id speaker, std::uint32_t menuSeq, std::uint32_t option);
	static Command endConversation(state::Id character, state::Id speaker, std::uint8_t reason);
	static Command changeSquad(state::Id character, std::uint64_t squad);
	static Command setView(double x, double z);
	// `specs`: the creator's Specs encoded by encodeSpecs (creator.hpp), "" for n = 0. `options`: the new-game advanced
	// options as "cod=1.5;gdm=1;ht=4" (ADVANCED_OPTIONS_STATIC.md; SIMLINK NEW_GAME's optional trailing block), "" for none.
	static Command newGame(const std::string& startoff, const std::string& name, std::uint64_t ticket,
		const std::string& specs = std::string(), const std::string& options = std::string());

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.tick);
		ar(self.type);
		ar(self.subject);
		ar(self.object);
		ar(self.value);
		ar(self.x);
		ar(self.z);
		ar(self.points);
		ar(self.heights);
		ar(self.text);
	}
};

} // namespace ksim
