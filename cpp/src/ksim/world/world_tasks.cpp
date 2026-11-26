// The package goals a built world squad runs outside the town scorer (task BO, TOWN_LIFE_SLICE 5.20). kinspect dt3 (kenshi-inspect
// 28aefb0, 3c670dd) watched a live slaver caravan and a bandit mob after a fight:
//  - a roaming slaver caravan whose package carries CAPTURE_NEW_SLAVES 167 (the caravan's "Slaver Wandering from town to town" and
//    "Slaver deliver to work camp") sends one to five members to a KO'd, free, non-slave character; the nearest strips its weapons
//    (181) and restrains it (166, the cuff), the squad gives it first aid (58 / 25), and the captive is carried (168) or, once awake,
//    follows its captor (171). The chain itself is stepSlaveTrade's (carry.cpp); this file only picks the member and walks it there.
//  - a squad whose package carries LOOT_THE_DEAD 177 (the starving bandits' "Patrol map aggressive ... LOOTING") sends ONE member to
//    a down body for the food it carries (the main inventory only: raw iron and the equipped club stay), then back to the follow.
// The town scorer never sees these members: a world squad has no SquadRoutine, and stepRoutines skips a squad without one. A member on
// a world task keeps a Routine (the follower stops for it) until the task ends; the next second's stepWorldSquads gives the follow
// back. Nothing here is saved beyond that Routine, and no layer reaches a terminal state: a walk that cannot arrive is dropped and
// the pair rests 20 s.

#include "ksim/world/factions.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/squad_caches.hpp"
#include "ksim/law/crime.hpp"

#include "law/capture_target.hpp"
#include "world/resident_data.hpp"
#include "world/world_resolvers.hpp"

#include "ksim/ai/blackboard.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/ai/goalsel.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/nav/doors.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/economy/inventory_grid.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/economy/trader.hpp"
#include "ksim/core/templates.hpp"
#include "ksim/ai/gatebind.hpp"
#include "ksim/world/towncircle.hpp"
#include "ksim/world/world.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <cstddef>
#include <functional>
#include <numbers>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace ksim {

// routine_scorer.cpp (T3b2): indexes one building's layout sites into ResidentData. Called here for the cage finders.
bool ensureBuilding(ResidentData& data, const World& world, flecs::world& ecs, const kdata::GameData& gameData, std::uint64_t key);

namespace {

// The scorer's kReach: the member stands at the body.
constexpr double kHomePointRadius = 200.0; // the town point's spread (dt33: Town vf+0x290's 200.0)
constexpr double kHomeEpochSeconds = 60.0; // UNSURE: the town point's refresh clock
constexpr double kHomeBox = 5000.0; // no home town: +-5000 per axis
constexpr double kBodyReach = 4.0;
// Watchers (183 / 260) walking to one body each take their own spot along the nearest reachable edge, this far apart (a body's R = 8).
constexpr double kWatchSpread = 8.0;
// 128 / 127: the distance to the destination point that counts as arrived (the mover's radius is unread, UNSURE).
// The reach of the gate bash (270, task raid-goals): a locked gate's slab is a wall to the mesh route, so the walk ends on the nearest
// reachable ground a body-width or more out (the live fixture measures ~11 u) - the bash reaches what the route cannot (OURS).
constexpr double kGateReach = 16.0;
// "No radius beyond the sensor list" (5.8): ours is the intruder sight, the same range the town chain gives a downed body.
constexpr double kBodySight = 60.0;
// OURS: a walk with no arrival after this long is given up and the (member, body) pair rests (the town scorer's 12 h release, tightened:
// a body a caravan cannot reach is not worth a whole day).
constexpr double kGiveUpSeconds = 30.0;
// OURS (UNSURE: the exe's rule for an off-mesh loot target; deferred: playtesting): a body that lies off the mesh or in a nav pinch is looted
// from the nearest walkable point to it, within this reach (kenshi-k16.6.35: the live fixture's walk ends 6.3 u out).
constexpr double kOffMeshLootReach = 8.0;
// OURS: a medic (58) that is still on its patient after this long drops it (no goal may lack a terminal state; the exe's first aid took 21-59 s, rwh.9).
constexpr double kAidGiveUpSeconds = 120.0;
constexpr double kStalkSeconds = 60.0; // 165's follow (UNSURE: the exe ends it when both speak, which has no duration here)
constexpr std::uint64_t kBlockTicks = 400; // 20 s

// One unit-count carried as a loose entry: what moves between inventories (as interact.cpp's loot).
state::Inventory::Entry loose(state::Inventory::Entry entry)
{
	entry.slot = state::Inventory::kCarried;
	entry.section = state::Inventory::Entry::kUnplaced;
	return entry;
}

// The members of a squad, in Id order (residents.cpp's own list, which is file-local there).
std::vector<std::pair<std::uint64_t, flecs::entity>> squadMembers(const World& world, flecs::entity squad)
{
	std::vector<std::pair<std::uint64_t, flecs::entity>> found;
	world.eachMember(squad, [&](flecs::entity member, const state::Id& id)
	{
		found.emplace_back(id.value, member);
	});
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	return found;
}

// The leader of a built world squad (residents.cpp's rule): its first member in Id order that is alive, conscious, not the player's and
// on no Routine of its own. It keeps the squad's follow (the selector leaves it out), so a squad never loses its carrot.
std::uint64_t leaderOf(const CombatArena& combat, const std::vector<std::pair<std::uint64_t, flecs::entity>>& members)
{
	for (const auto& [id, member] : members)
	{
		const state::Fighter* f = combat.find(id);
		if (f != nullptr && !f->dead && !f->unconscious && !f->playerControlled && !member.has<state::Routine>())
			return id;
	}
	return 0;
}

constexpr double kTownDoorReach = 12.0; // the town scorer's kDoorReach: within it of a home door, the lock / open step acts

// The G3 goals a world squad runs (task BW): each maps to its direct task - the executor in this file, or carry.cpp's for 112.
bool worldExecutable(std::uint16_t task) noexcept
{
	switch (task)
	{
	case routines::task::CaptureSlaves:
	case routines::task::LootTheDead:
	case routines::task::SlaveGetInCage: // ds/slave-goals: 172 (dt24_172)
	case routines::task::FinalKidnapperCageJob: // ds/slave-goals: 254 (dt26)
	case routines::task::SlaveWatching:
	case routines::task::NewSlaveProcessing:
	case routines::task::CarryWoundedSlaves:
	case routines::task::FindCageAndPutIn:
	case routines::task::FindBedAndPutIn: // ds/bed-audit: 106 (dt24_106), the bed twin of 112
	case routines::task::FindCageAndPutDeadGuyIn: // 281 (dt26 section 0): 112 with a corpse allowed
	case routines::task::GatherSlavesFromCages:
	case routines::task::Patrol: // task patrol: the goal-less walks and the town search (dt24_36)
	case routines::task::PatrolTown:
	case routines::task::RunAroundTown:
	case routines::task::FirstAid: // 58 JOB_MEDIC (task CG)
	case routines::task::EmptyMachine: // 92, 247 and 284 (dt24_emptymachine_dismantle): the pull tasks
	case routines::task::BuildMats:
	case routines::task::LootContainer:
	case routines::task::AcquireFood: // 243 (dt24_243): the occupied town's food, planned through 244
	case routines::task::SitAround: // 275 (dt24_275)
	case routines::task::PickupIntrudersTown: // 273: adopted here, carry.cpp's stepIntruders executes it (an occupying squad's throw-out)
	case routines::task::SleepOnFloor: // 175 (dt24_175): the animals' floor sleep
	case routines::task::AssaultFortifications: // 270 and 276 (task raid-goals; dt24_270 / dt24_276)
	case routines::task::LiberatePrisoners:
	case routines::task::NotBeUnarmed: // 42 (dt25 section 2): the rearm loop 8 FIND_WEAPON -> 6 EQUIP
	case routines::task::EatRandomDeadBody: // 230 (dt27 section 2): the corpse eater, 220 Task_EatPrisoner
	case routines::task::EatRandomKoBody: // 233 (dt39 section 5): the same executor on a knocked-out living body
	case routines::task::FindCorpses: // ds/dead-guy: 138 (dt30), the corpse lift
	case routines::task::GetOutOfCageEscape: // ds/escape: 207 and 256 (dt24_207), the caged slave leaves its cage
	case routines::task::GetOutOfCageOpportunistic:
	case routines::task::FollowUrgentEscape: // 253: 141's follow while the leader is up
	case routines::task::KillRandomCageOccupant: // 135 (dt27 section 3): the cage kill, 134 Task_KillPrisoner
	case routines::task::BeatCageOccupant: // 196 (dt24_killprisoner): the same class, goal isTargetDown
	case routines::task::DeadGuysInThePot: // 137 (dt39 section 4): carry.cpp's stepCorpsePot runs the chain, as the town scorer's
	case routines::task::EatRandomCageOccupant: // 139 (dt29_fog_kidnap section 6): the live cage victim, 220 Task_EatPrisoner
	case routines::task::EatRandomCageOccupantMeasured: // 282 (dt24_killprisoner): 139 again
	case routines::task::FindAndRescueLeader: // 142 and 148 (dt24_142): lift the downed leader / ally, carry it home
	case routines::task::FindAndRescueIfBeds:
	case routines::task::FindAndRescue: // 105 (port-talk): the same finder as 148
	case routines::task::FogmanWorshipVictim: // 237 (dt24_237): the crowd at a victim already being eaten
	case routines::task::HuntingBloodsmell: // 176 (dt27 section 4): the walk at the platoon's smell
	case routines::task::TravelToTargetTown: // 53 and 130 (dt27 section 5): the walk to the package's town
	case routines::task::TravelToTargetTownFast:
	case routines::task::TravelToTargetPackage: // 215 (dt24_215): the army-formation walk to the package's target point
	case routines::task::MakeAnnouncement: // 103 and 129 (dt24_smalltasks section 5): the announcement line at the package target
	case routines::task::MakeAnnouncementFast:
	case routines::task::RetreatHometown: // 128 and 127 (dt24_128): the walk home
	case routines::task::RunAwayHometown:
	case routines::task::GoToSomewhereForDeliveringSlaves: // 192 (dt27 section 6.1): a travel goal, satisfied by 53 / 130
	case routines::task::LootFoodAndStuff: // 131 (dt24_131): 243's food box, 284's pull
	case routines::task::KeepEverythingRunning: // ds/machines-2: 89 / 90 / 91 unjam through 92's pull, 241 loots a body's storable items
	case routines::task::UnjamAllMachines:
	case routines::task::UnjamMachine:
	case routines::task::LootStorable:
	case routines::task::LootAnimalsJob: // 238 (port-animals): 241's executor over the animal-body finder 1408ce3a0
	case routines::task::PutLootInStorage: // ds/loot-storage: 184 (dt24_184)
	case routines::task::DeliverResources: // ds/machines: 88 / 93 / 114 / 124 / 242 put carried resources into my box, 113 / 152 pull 92
	case routines::task::FillMachine:
	case routines::task::GetRidOfResources:
	case routines::task::OperateStorage:
	case routines::task::DitchAllResources:
	case routines::task::EmptyMachineOutputs:
	case routines::task::OperateAutomaticMachinery:
	case routines::task::WorkTheSlaves: // 188 (dt24_131 section 4): the work dialogue at a random owned slave
	case routines::task::GatherSlavesFemaleOrBeast: // 245 (dt24_131 section 7): 217's gather with the female / other-race filter
	case routines::task::GatherSlavesFromCagesExport: // 217 and 218 / 194 (dt27 sections 6.2-6.3): the slave trade's town steps
	case routines::task::GiveAllMySlavesTo:
	case routines::task::GiveAllMySlavesToImport:
	case routines::task::FollowSquadLeader: // 141 and 171 (dt24_141): Task_FollowLeader
	case routines::task::FollowSlaveMaster:
	case routines::task::LockAllMyDoors: // 155 / 197 / 86 (dt24_155): the home's doors, walked to and locked / opened in one step
	case routines::task::LockDoorsStayOutside:
	case routines::task::OpenUpShopDoors:
	case routines::task::Shopkeeper: // 20 / 255: the home building's counter / throne slot, walked to and held
	case routines::task::SitOnThrone:
	case routines::task::WanderTown: // 37 / 47 / 51 (rwh.55): a general node of the home building, reserved, stood on and faced, released
	case routines::task::StandAtGeneralNode:
	case routines::task::StandAtNode:
		return true;
	default:
		return false;
	}
}

// 241's wait at the body (Task_Loot_ResourcesWeHaveStorageFor tick 14034e2a0: 3.0 s of the medic animation).
constexpr double kLootStorableSeconds = 3.0;

// "It already looks like a slave" (TARGET_LOOKS_LIKE_A_SLAVE, dt15 section 0): the worn shackles. OURS: the exe also reads the
// weapon hand and the inventory sections; the shackles alone stand in for them.
bool wearsShackles(const kdata::GameData& data, flecs::entity who)
{
	const kdata::Record* shackles = data.find("SHACKLES");
	const state::Inventory* pack = who.get<state::Inventory>();
	if (shackles == nullptr || pack == nullptr)
		return false;
	for (const state::Inventory::Entry& entry : pack->items)
		if (entry.itemSid == shackles->stringID && entry.slot == state::Inventory::kArmourBase + 9)
			return true;
	return false;
}

// One goal's target: a character, or a cage (its site). `cage` selects which field.
struct Resolved
{
	std::uint64_t fighter = 0;
	routines::Site site;
	bool cage = false;
	std::uint64_t building = 0; // task patrol: the building RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE 216 searches
	// The machine or storage box a pull task empties (92 / 247 / 284); the executor walks to its use point.
	std::uint64_t stock = 0;
	// The gate a raid must break (270, task raid-goals): the gate's door key; the executor looks the site up for its position.
	std::uint64_t gate = 0;
	// 42's FIND_WEAPON source when it is a loose item on the ground (a state::GroundItem entity's Id; a body is `fighter`).
	std::uint64_t item = 0;
	std::uint64_t pot = 0; // 137: the disposal machine's key (DisposalSite::key); `fighter` is the body
	bool point = false; // task patrol: 36 / 22, a goal-less walk (the executor draws its point)
	bool slot = false; // 20 / 255: a counter / throne slot of the home building (pickSite stood; the executor holds the member at the site)
	bool door = false; // 155 / 197 / 86: the home's doors are the target (resolveDoorRow stood; the executor walks to the doors and applies them)
	double px = 0.0; // 176 / 53 / 130: the point the walk is aimed at (logged; the executor re-reads its source every second)
	double pz = 0.0;
	double score = 1.0; // the resolver's own score where it has one (58 is the goal's whole utility; 112 its ladder, dt24_112 section 3); else the flat 1.0
	bool any() const noexcept { return fighter != 0 || cage || building != 0 || stock != 0 || gate != 0 || item != 0 || point || door || slot || pot != 0; }
};

// The patrol / wander goals: a low-tier filler that never ends by itself (216's search picks building after building), so the selector
// re-scores a member on one every kReselectSeconds. Without it the first pass decides for good: a 270 gate locked a tick after
// that pass was never offered (k16.6.651).
bool wanderGoal(std::uint16_t task) noexcept
{
	return task == routines::task::PatrolTown || task == routines::task::Patrol || task == routines::task::RunAroundTown;
}
constexpr double kReselectSeconds = 5.0;

// A goal whose executor in this file ends the Routine itself; the selector's interruption check guards the window for them.
bool selectorGoal(std::uint16_t task) noexcept
{
	return task == routines::task::SlaveWatching || task == routines::task::NewSlaveProcessing
		|| task == routines::task::CarryWoundedSlaves || task == routines::task::GatherSlavesFromCages
		|| task == routines::task::SlaveGetInCage || task == routines::task::FinalKidnapperCageJob
		|| task == routines::task::FindCageAndPutIn || task == routines::task::FindBedAndPutIn || task == routines::task::FindCageAndPutDeadGuyIn
		|| task == routines::task::Patrol
		|| task == routines::task::PatrolTown || task == routines::task::RunAroundTown || task == routines::task::FirstAid
		|| task == routines::task::EmptyMachine || task == routines::task::BuildMats || task == routines::task::LootContainer
		|| task == routines::task::AcquireFood || task == routines::task::SitAround || task == routines::task::SleepOnFloor
		|| task == routines::task::SleepOnFloorFakeAmbush // 261 (port-animals): 175's class, named by an order, not a package row
		|| task == routines::task::AssaultFortifications || task == routines::task::LiberatePrisoners
		|| task == routines::task::NotBeUnarmed || task == routines::task::EatRandomDeadBody || task == routines::task::EatRandomKoBody || task == routines::task::FindCorpses
		|| task == routines::task::GetOutOfCageEscape || task == routines::task::GetOutOfCageOpportunistic
		|| routines::task::killsCageOccupant(task) || task == routines::task::DeadGuysInThePot || task == routines::task::HuntingBloodsmell
		|| routines::task::eatsCageOccupant(task) || routines::task::rescues(task) || task == routines::task::FogmanWorshipVictim
		|| task == routines::task::TravelToTargetTown || task == routines::task::TravelToTargetTownFast
		|| task == routines::task::TravelToTargetPackage || task == routines::task::MakeAnnouncement || task == routines::task::MakeAnnouncementFast
		|| task == routines::task::RetreatHometown || task == routines::task::RunAwayHometown
		|| task == routines::task::GoToSomewhereForDeliveringSlaves || task == routines::task::GatherSlavesFromCagesExport
		|| routines::task::storesResources(task) || routines::task::pullsMachine(task) || routines::task::unjams(task)
		|| task == routines::task::LootStorable || task == routines::task::LootAnimalsJob || task == routines::task::PutLootInStorage
		|| task == routines::task::LootFoodAndStuff || task == routines::task::WorkTheSlaves || task == routines::task::GatherSlavesFemaleOrBeast
		|| task == routines::task::GiveAllMySlavesTo || task == routines::task::GiveAllMySlavesToImport
		|| task == routines::task::LockAllMyDoors || task == routines::task::LockDoorsStayOutside || task == routines::task::OpenUpShopDoors
		|| task == routines::task::Shopkeeper || task == routines::task::SitOnThrone || routines::task::standsAtNode(task)
		|| task == routines::task::StandUp || task == routines::task::SeekAndTalk || task == routines::task::SendDialogue
		|| task == routines::task::FollowWhileTalking || task == routines::task::TownStalker;
}

// A member that may take a world task: up, not the player's, in no fight, carrying nothing, and on no order, routine or other step.
// An order is a move order or an attack order. A move order is routeSeq, the clientSeq of the order that made the route (combat.cpp
// clears it when the route arrives; walkTo, the follow and the AI's own routes leave it 0), and a route the walk step abandons on a
// block keeps its seq, so the member stays under the order (the walker stands; the client's next order is its mover). An attack
// order is orderTarget - a player's command or a dialogue's DA_ATTACK (dialogue_sim.cpp), which readies a fight before pickTarget
// has set a target. dt14 puts the job queue, the oneshot orders and the persistent orders before the package tiers.
// `carrierOk`: a member holding a person is free for the goals that place it (106, dt24_106, and 281 for a corpse; 112 reaches its
// carrier through the capture chain's own Routine instead). Its finished lift (the CarryStep's phase 1 is the carry itself) does not
// keep it busy then; a lift still under way does.
// `graftOk`: the character is a slave the master's package scores (the graft, rwh.99 section 3): a player's slave included, which no other pass scores.
// `reselectOk`: a member on a wander-class goal (wanderGoal) that is due its re-score is free too, so a higher-scoring goal can take it.
// `idleOk`: a member holding only the idle STAY_IN_HOME routine (43) is free too (the squad has a fresh captive to treat: k16.6.413).
bool freeMember(const CombatArena& combat, std::uint64_t id, flecs::entity entity, bool carrierOk = false, bool graftOk = false, bool idleOk = false, bool reselectOk = false)
{
	const state::Fighter* f = combat.find(id);
	const bool ordered = f != nullptr && (f->routeSeq != 0 || f->orderTarget != 0);
	return f != nullptr && !ordered && !f->dead && !f->unconscious && !f->down && (graftOk || !f->playerControlled) && f->target == 0 && (carrierOk || f->carrying == 0)
		&& f->carriedBy == 0 && !f->asleep && f->goal.kind == state::IndividualKind::None && (entity.get<state::Routine>() == nullptr || (idleOk && entity.get<state::Routine>()->goal == routines::task::StayInHome) || reselectOk)
		&& !entity.has<state::InteractTask>() && !entity.has<state::DoorStep>() && !entity.has<state::TakeOut>()
		&& (!entity.has<state::CarryStep>() || (carrierOk && entity.get<state::CarryStep>()->phase == 1));
}

// The graft's gate (rwh.99 section 3): a slave (state 1) with its Obedient Slave row, not sneaking and not locked in a cage (the cage's own).
bool grafts(const state::Fighter& f)
{
	return f.slaveState == 1 && f.obedient && !f.sneak && !f.dead && f.cageBuilding == 0;
}

// Whether the character carries food (an ITEM record of function 3 in its main inventory) or a first aid kit (function 1).
bool carriesFunction(const CombatData& data, const state::Inventory& inventory, std::int32_t function)
{
	for (const state::Inventory::Entry& entry : inventory.items)
	{
		const ItemInfo* info = entry.slot == state::Inventory::kCarried ? data.item(entry.itemSid) : nullptr;
		if (info != nullptr && info->function == function)
			return true;
	}
	return false;
}

// Whether a member is in a fight: it has a target, or a player character targets it.
bool squadInFight(const CombatArena& combat, const std::vector<std::pair<std::uint64_t, flecs::entity>>& members)
{
	// A target that cannot be fought any more (down, unconscious, dead, carried) does not keep the squad in the fight: task BR found
	// a member holding such a target through a hit-reaction stumble 2.6 s past the KO, and a KO'd player holding its old target, which
	// kept the squad "in a fight" for ever and blocked its capture and loot (the client's live run).
	const auto live = [&](const state::Fighter& f)
	{
		if (f.target == 0)
			return false;
		const state::Fighter* target = combat.find(f.target);
		return target != nullptr && !target->dead && !target->unconscious && !target->down && target->carriedBy == 0;
	};
	std::vector<std::uint64_t> ids;
	for (const auto& [id, member] : members)
	{
		const state::Fighter* f = combat.find(id);
		if (f != nullptr && !f->dead && live(*f))
			return true;
		ids.push_back(id);
	}
	for (const CombatArena::Member& other : combat.members())
	{
		if (other.fighter.playerControlled && !other.fighter.dead && live(other.fighter)
			&& std::find(ids.begin(), ids.end(), other.fighter.target) != ids.end())
			return true;
	}
	return false;
}

// The first WEAPON record a sheathe (7 UNEQUIP_WEAPON) or a draw (6 EQUIP_WEAPON) moves: in the hands (`inSlot`) or carried in the
// pack. "" = none. The 275 sitter's sheathe is undone when the sit ends, as the exe draws again before an attack.
std::string weaponSidOf(const kdata::GameData& data, const state::Inventory* pack, bool inSlot)
{
	const std::int32_t weaponType = kworld::itemTypeByName("WEAPON");
	if (pack == nullptr)
		return {};
	for (const state::Inventory::Entry& entry : pack->items)
	{
		const bool hands = entry.slot == state::Inventory::kWeapon;
		if (hands != inSlot || (!hands && entry.slot != state::Inventory::kCarried))
			continue;
		const kdata::Record* record = data.find(entry.itemSid);
		if (record != nullptr && record->type == weaponType)
			return entry.itemSid;
	}
	return {};
}

// ---- task dt25: 45 BODYGUARD, 42 DON'T BE UNARMED, 169 PUT_DOWN_CARRIED_DUDE (kinspect dt25_bodyguard_unarmed_putdown.md) ----

// 45's threat reach: the original's char+0x2b0 "enemy near" flag is set when a hostile comes within sqrt(1600) = 40 u (dt25
// section 1.3; s73). The bodyguard goal is offered only while the charge or the guard has one that close.
constexpr double kBodyguardThreat = 40.0;
// The leader sheathes only with no enemy within this many u (OURS, UNSURE: the original's planner reads its own threat flags; k16.6.288).
constexpr double kLeaderStandDownRadius = 200.0;
// 141 / 171 (dt24_141): the goal's utility is not in the data; the lowest floor makes it the selector's fall-through (UNSURE).
constexpr double kFollowUtility = 0.001;
// 42's finder reach: the original's _DAT_14168dae4 = 10000.0f squared = 100 u (dt25 section 2.4).
constexpr double kWeaponSight = 100.0;

// ---- task dt27: 230 EAT_A_RANDOM_DEAD_BODY, 135 KILL_A_RANDOM_CAGE_OCCUPANT, 176 HUNTING_BLOODSMELL, 130 / 53 TRAVEL_TO_TARGET_TOWN ----

// 230's finder reach: DAT_14169bc38, unread (UNSURE); the sibling body finders' 100 u stands in.
constexpr double kEatSight = 100.0;
// 220 Task_EatPrisoner's approach radius (DAT_141683188 = 20.0, dt27 section 2.2).
constexpr double kEatReach = 20.0;
// 134 Task_KillPrisoner (dt24_killprisoner section 5): one receiveHit per "chop down static" pass, on the crossing of 0.55 of the clip.
// The clip's length is not in the findings (UNSURE): one second stands in.
constexpr double kChopSeconds = 1.0;
constexpr double kChopHitAt = 0.55;
// 139 on a live victim (dt29_fog_kidnap section 8): blood fell 75.8 -> -49.9 over about six game minutes, the body stayed in the world.
// Ours: that rate per second, no removal (UNSURE: the race path vs a player exemption). The victim carries the "being fed on" mark
// (char+0x279, set to 7 by 1404356f0 and counted down by Character::update, dt24_237 section 2); its unit is unread, so ten seconds
// from the last bite stands in.
constexpr double kBeingEatenSeconds = 10.0;
// 148's finder reach (1408cd2d0's sensor radius is unread, UNSURE): the sibling body finders' 100 u stands in.
constexpr double kRescueSight = 100.0;

using worldres::inedibleRace; // RaceData+0x7c, shared with 138 (world_resolvers.hpp)

// 130 / 53's destination (dt27 section 5.2): the package's target town - ours the squad's home town, as 36's (dt24_patrol_town_pot) -
// unless it is a NEST (TownType 0), when the task walks to the nearest other town of a real type instead (140927ad0; the "same holder"
// filter is not read - UNSURE). False when the squad has no such town.
bool travelDestination(flecs::world& ecs, const kdata::GameData& data, const state::SquadInstance& instance, const state::Fighter& self,
	towncircle::Circle& out, bool divertNest = true)
{
	if (instance.homeTown.id == 0 || !ecs.is_alive(instance.homeTown.id))
		return false;
	const state::TownState* home = ecs.entity(instance.homeTown.id).get<state::TownState>();
	if (home == nullptr)
		return false;
	const auto townType = [&](const state::TownState& town)
	{
		const kdata::Record* active = data.find(town.activeSid);
		if (active == nullptr)
			active = data.find(town.rootSid);
		return active != nullptr ? static_cast<int>(active->intOr("type", 2)) : 2;
	};
	towncircle::Circle best = towncircle::of(data, *home);
	if (divertNest && townType(*home) == 0)
	{
		bool found = false;
		double bestD2 = 0.0;
		ecs.each([&](const state::TownState& town)
		{
			if (&town == home || townType(town) == 0)
				return;
			const towncircle::Circle circle = towncircle::of(data, town);
			const double d2 = (circle.x - self.x) * (circle.x - self.x) + (circle.z - self.z) * (circle.z - self.z);
			if (circle.radius > 0.0 && (!found || d2 < bestD2))
			{
				best = circle;
				bestD2 = d2;
				found = true;
			}
		});
		if (!found)
			return false;
	}
	if (best.radius <= 0.0)
		return false;
	out = best;
	return true;
}

// 128 / 127's destination (dt33 section 2; Task_GoHomeTown's vf6). With a home town the TOWN gives one cached point (TownBase
// vf+0x38, cached at town+0xcc..0xd4 and refreshed on a clock), the same for every member. Ours: a point within 200 u of the town
// centre (uniform in the disc), hashed from the town and the clock epoch, so every member of the town reads the same point and it
// moves on the epoch. UNSURE: the clock length (kHomeEpochSeconds is a guess) and the refresh rule (the exe asks Town vf+0x290(out,
// 200.0), else blends from the object at the centre). With no home town the point is the character's own position plus two
// independent uniform draws in -5000..+5000 (one for x, one for z): a square box, not a ring. `outside` is isAtHomeTown (95)
// false: the character is outside the home town's circle, or there is no home town.
struct HomePoint
{
	double x = 0.0;
	double z = 0.0;
	bool outside = false;
};

// True while a just-committed goal is inside its commit lock (commitOf: base + rand[0,1) * span seconds from r.since, the draw hashed
// from the member and the commit tick).
bool commitLocked(const state::Routine& r, std::uint64_t member, std::uint64_t now);

std::uint64_t homeHash(std::uint64_t seed)
{
	std::uint64_t h = seed * 0x9e3779b97f4a7c15ull;
	h ^= h >> 30;
	h *= 0xbf58476d1ce4e5b9ull;
	h ^= h >> 27;
	h *= 0x94d049bb133111ebull;
	h ^= h >> 31;
	return h;
}

bool commitLocked(const state::Routine& r, std::uint64_t member, std::uint64_t now)
{
	const routines::Commit commit = routines::commitOf(r.goal);
	if (commit.base <= 0.0 && commit.span <= 0.0)
		return false;
	const double u = static_cast<double>(homeHash(member * 0x100000001b3ull ^ r.since) & 0xffffff) / 16777216.0;
	return static_cast<double>(ticksSince(now, r.since)) * kTickSeconds < commit.base + u * commit.span;
}

HomePoint homeTownPoint(flecs::world& ecs, const kdata::GameData& data, const state::SquadInstance& instance, const state::Fighter& self,
	std::uint64_t now, std::uint64_t seed)
{
	HomePoint out;
	towncircle::Circle town;
	if (travelDestination(ecs, data, instance, self, town, false))
	{
		const std::uint64_t epoch = now / static_cast<std::uint64_t>(kHomeEpochSeconds / kTickSeconds);
		const std::uint64_t h = homeHash(epoch * 0x2545f4914f6cdd1dull ^ std::hash<double>{}(town.x) * 31 ^ std::hash<double>{}(town.z));
		const double angle = static_cast<double>(h & 0xffffff) / 16777216.0 * 6.283185307179586;
		const double radius = kHomePointRadius * std::sqrt(static_cast<double>((h >> 24) & 0xffffff) / 16777216.0);
		out.x = town.x + std::cos(angle) * radius;
		out.z = town.z + std::sin(angle) * radius;
		out.outside = std::hypot(self.x - town.x, self.z - town.z) > town.radius;
		return out;
	}
	const std::uint64_t hx = homeHash(seed);
	const std::uint64_t hz = homeHash(seed ^ 0xa5a5a5a5a5a5a5a5ull);
	out.x = self.x + (static_cast<double>(hx & 0xffffff) / 16777216.0 * 2.0 - 1.0) * kHomeBox;
	out.z = self.z + (static_cast<double>(hz & 0xffffff) / 16777216.0 * 2.0 - 1.0) * kHomeBox;
	out.outside = true;
	return out;
}

// 42's hasWeapon (dt25 section 2.2): an animal's natural weapon makes it armed for ever - its hand type is its weapon, and the
// FIND_WEAPON search never runs for one (the coordinator's pin; a clawless animal holds the empty hand and is still no
// searcher). A character's own weapon counts in the hands, on the back, or carried in the pack. OURS: the exe's equipment-map
// test (140747370) is not modelled.
bool armed42(const kdata::GameData& data, const CombatArena& combat, const state::Fighter& self, const state::Inventory* pack)
{
	if (self.animal)
		return true;
	if (self.weapon.sid != combat.data().unarmed.sid)
		return true;
	const std::int32_t weaponType = kworld::itemTypeByName("WEAPON");
	const auto isWeapon = [&](const std::string& sid)
	{
		const kdata::Record* record = data.find(sid);
		return record != nullptr && record->type == weaponType;
	};
	if (pack == nullptr)
		return false;
	for (const state::Inventory::Entry& entry : pack->items)
		if ((entry.slot == state::Inventory::kWeapon || entry.slot == state::Inventory::kSheathed || entry.slot == state::Inventory::kCarried) && isWeapon(entry.itemSid))
			return true;
	if (const state::Inventory::Entry* bag = pack->wornBackpack())
		for (const state::Inventory::Entry& entry : bag->contents)
			if (isWeapon(entry.itemSid))
				return true;
	return false;
}

// 42's FIND_WEAPON finder (dt25 section 2.4; the original 14099b1d0): the nearest loose weapon on the ground within 100 u
// (pass 1), else a down, KO'd or dead body holding one within it (pass 3). The original's pass 2 - the first sensed armed
// character - is not modelled: taking a weapon off a living character has no interaction in ours (gap noted in FEATURES).
// OURS also: a body of my own faction is skipped (the exe's package-membership test, 140599590), and only a weapon IN ITS HANDS
// counts - the one our strip can put on the ground; an animal body has a natural weapon that is no item.
struct WeaponSource
{
	std::uint64_t item = 0; // a state::GroundItem entity's Id
	std::uint64_t body = 0; // a down, KO'd or dead fighter's Id
};

WeaponSource findWeapon42(flecs::world& ecs, const CombatArena& combat, const kdata::GameData& data, std::uint64_t selfId,
	const state::Fighter& self)
{
	WeaponSource out;
	double best = kWeaponSight;
	const std::int32_t weaponType = kworld::itemTypeByName("WEAPON");
	ecs.each([&](flecs::entity, const state::Id& id, const state::GroundItem& item)
	{
		const kdata::Record* record = data.find(item.itemSid);
		if (record == nullptr || record->type != weaponType)
			return;
		const double d = std::hypot(item.x - self.x, item.z - self.z);
		if (d > best)
			return;
		best = d;
		out = {id.value, 0};
	});
	if (out.item != 0)
		return out;
	for (const CombatArena::Member& other : combat.members())
	{
		const state::Fighter& body = other.fighter;
		if (other.id == selfId || (!body.dead && !body.down && !body.unconscious) || body.carriedBy != 0 || body.faction == self.faction
			|| body.animal || body.weapon.sid == combat.data().unarmed.sid)
			continue;
		const double d = std::hypot(body.x - self.x, body.z - self.z);
		if (d > best)
			continue;
		best = d;
		out = {0, other.id};
	}
	return out;
}

// 169's can-walk gate (dt25 section 3.2; the original 1405a0710): awake, alive, on its feet, and either a captive (the exe's
// char+0x320 mark; ours slaveState) or no enemy of the carrier. A hostile non-captive stays on the shoulder, and a down or KO
// body is not a walker.
bool canBePutDown(const sensor::Context& ctx, const state::Fighter& carrier, const state::Fighter& carried)
{
	if (carried.dead || carried.unconscious || carried.down)
		return false;
	return carried.slaveState != 0 || !sensor::enemy(ctx, carrier, carried);
}

} // namespace

// ---- task ds/sheathe: the shared plan steps 7 UNEQUIP_WEAPON and 6 EQUIP_WEAPON ----

bool Simulation::weaponInHands(std::uint64_t id) const
{
	const state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	const flecs::entity who = fighterEntity(id);
	const state::Inventory* pack = who.is_valid() ? who.get<state::Inventory>() : nullptr;
	if (f == nullptr || pack == nullptr || f->animal)
		return false; // an animal's natural weapon is not an equipped weapon (planHomebase's reading)
	for (const state::Inventory::Entry& entry : pack->items)
		if (entry.slot == state::Inventory::kWeapon)
			return true;
	return false;
}

std::string Simulation::sheathedWeaponOf(std::uint64_t id) const
{
	const flecs::entity who = fighterEntity(id);
	const state::Inventory* pack = who.is_valid() ? who.get<state::Inventory>() : nullptr;
	if (pack == nullptr)
		return {};
	for (const state::Inventory::Entry& entry : pack->items)
		if (entry.slot == state::Inventory::kSheathed)
			return entry.itemSid;
	return {};
}

bool Simulation::inFight(const state::Fighter& f)
{
	// A technique index alone is no fight: a fighter that lost its target keeps the index of its last swing in a state past the swing (Done 9,
	// Hub 4; the combat step skips a target-less member), and counting it kept a cuffer from ever being scored for the lift (k16.6.256
	// rework). A swing in progress is state Attack / Defend / Stumble, counted below.
	return f.target != 0 || !f.engaged.empty() || f.state == state::CombatState::Attack || f.state == state::CombatState::Defend
		|| f.state == state::CombatState::Stumble;
}

// inFight without the stale clip: a finished technique leaves Fighter::technique set (the pointer stays for the stale block, combat.cpp
// resolveIncoming), so only a playing one is a swing. The sheathe and the squad-quiet test read this; the town scorer's gate keeps inFight (k16.6.288).
bool Simulation::inFightNow(const state::Fighter& f)
{
	return f.target != 0 || (f.technique >= 0 && !f.techniqueFinished) || !f.engaged.empty() || f.state == state::CombatState::Attack || f.state == state::CombatState::Defend
		|| f.state == state::CombatState::Stumble;
}

bool Simulation::sheatheWeapon(std::uint64_t id, bool player)
{
	state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr || (f->playerControlled && !player) || f->animal || gameData_ == nullptr)
		return false;
	const flecs::entity who = fighterEntity(id);
	const std::string weapon = weaponSidOf(*gameData_, who.get<state::Inventory>(), true);
	if (weapon.empty())
		return false;
	// The weapon stays equipped, on the back (rwh.102 4.1): its slot changes, the pack gets nothing. A fight keeps it in the hand; a swing in progress keeps its clip (as order 67 does).
	if (inFightNow(*f))
		return true;
	state::Inventory pack = *who.get<state::Inventory>();
	for (state::Inventory::Entry& entry : pack.items)
		if (entry.slot == state::Inventory::kWeapon)
			entry.slot = state::Inventory::kSheathed;
	++pack.version;
	who.set<state::Inventory>(std::move(pack));
	unequipFromFighter(id, weapon);
	mirrorFighter(id);
	return true;
}

std::string Simulation::drawWeapon(std::uint64_t id)
{
	state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr || f->playerControlled || f->animal || gameData_ == nullptr || weaponInHands(id))
		return {};
	const flecs::entity who = fighterEntity(id);
	const std::int32_t weaponType = kworld::itemTypeByName("WEAPON");
	const state::Inventory* held = who.get<state::Inventory>();
	const auto sheathed = [&](const state::Inventory::Entry& entry)
	{
		const kdata::Record* record = entry.slot == state::Inventory::kSheathed ? gameData_->find(entry.itemSid) : nullptr;
		return record != nullptr && record->type == weaponType;
	};
	if (held != nullptr && std::any_of(held->items.begin(), held->items.end(), sheathed))
	{
		state::Inventory pack = *held;
		const auto entry = std::find_if(pack.items.begin(), pack.items.end(), sheathed);
		const std::string weapon = entry->itemSid;
		if (f->state == state::CombatState::Attack || f->state == state::CombatState::Defend || f->state == state::CombatState::Stumble)
			return weapon;
		entry->slot = state::Inventory::kWeapon;
		f->weapon = combat_->data().buildWeapon(*gameData_, *gameData_->find(weapon), entry->quality);
		++pack.version;
		who.set<state::Inventory>(std::move(pack));
		mirrorFighter(id);
		return weapon;
	}
	const std::string weapon = weaponSidOf(*gameData_, held, false); // a weapon carried in the pack
	if (!weapon.empty())
		equipItem(id, weapon, true);
	return weapon;
}

bool Simulation::runWeaponPrefix(std::uint64_t id, std::uint16_t goal)
{
	// The planner's rule: 7 is the only provider of hasWeaponEquipped = 0, so a goal row that needs it is planned [7, goal] while the
	// weapon is in the hands (planHomebase for the rows it models: 275, 175); the rows without a chain of their own take the same step.
	if (!weaponInHands(id) || !goap::needsUnarmed(goal))
		return false;
	return sheatheWeapon(id);
}

void Simulation::stepRearm()
{
	if (combat_ == nullptr)
		return;
	std::vector<std::uint64_t> drawing;
	for (const CombatArena::Member& m : combat_->members())
		if (m.due && m.fighter.target != 0 && !m.fighter.dead && !m.fighter.playerControlled && !m.fighter.animal)
			drawing.push_back(m.id);
	for (const std::uint64_t id : drawing)
	{
		combat_->find(id)->fought = true;
		drawWeapon(id);
	}
}

void Simulation::stepTorture()
{
	if (combat_ == nullptr || combat_->empty() || gameData_ == nullptr)
		return;
	std::vector<std::uint64_t> occupants;
	std::vector<std::uint64_t> corpses;
	for (const CombatArena::Member& m : combat_->members())
	{
		if (m.fighter.cageBuilding == 0)
			continue;
		auto cached = peelerSites_.find(m.fighter.cageBuilding);
		if (cached == peelerSites_.end())
		{
			bool peeler = false;
			const BuildingSite* site = world_.buildingSite(m.fighter.cageBuilding);
			const kdata::Record* record = site != nullptr ? gameData_->find(site->buildingSid) : nullptr;
			const kdata::RefCategory* functionality = record != nullptr ? record->findRefs("functionality") : nullptr;
			if (functionality != nullptr)
				for (const kdata::RefEntry& entry : functionality->entries)
				{
					const kdata::Record* function = gameData_->find(entry.targetID);
					peeler = peeler || (function != nullptr && routines::isTortureFunctionality(*function));
				}
			cached = peelerSites_.emplace(m.fighter.cageBuilding, peeler).first;
		}
		if (cached->second)
			(m.fighter.dead ? corpses : occupants).push_back(m.id);
	}
	// A torso pass that reaches the vital line kills before the machine's release (rwh42 5 step 6 runs when every part is at its line; the death
	// check is the arena's, a tick earlier): the body is let go all the same, so it does not hold the cage slot.
	for (const std::uint64_t id : corpses)
		leaveCage(id, 0, false);
	for (const std::uint64_t id : occupants)
	{
		const state::Inventory* pack = fighterEntity(id).get<state::Inventory>();
		const bool bandaged = pack != nullptr && carriesFunction(combat_->data(), *pack, kItemFunctionMedkit);
		bool finished = false;
		if (!bandaged && stripArmour(id))
		{
			fireDialogue(id, id, 0x0b);
			continue;
		}
		if (combat_->torture(id, bandaged, kTickSeconds, &finished))
		{
			fireDialogue(id, id, 0x0b); // SCREAMING_TORTURE: the occupant, to itself (item29: speaker the occupant, subject itself)
			mirrorFighter(id);
			if (finished)
				leaveCage(id, 0, false); // rwh42 5 step 6: released, back to its normal AI
		}
	}
}

bool Simulation::stripArmour(std::uint64_t id)
{
	state::Fighter& f = *combat_->find(id);
	flecs::entity entity = fighterEntity(id);
	const state::Inventory* held = entity.get<state::Inventory>();
	if (held == nullptr)
		return false;
	// The worn armour: every equipped piece but the backpack and a slave's shackles (their own unlock removes those).
	const auto worn = std::find_if(held->items.begin(), held->items.end(), [](const state::Inventory::Entry& e)
		{ return e.slot >= state::Inventory::kArmourBase && e.slot != state::Inventory::kBackpackSlot && e.slot != state::Inventory::kSheathed
			&& e.slot != state::Inventory::kArmourBase + 9; });
	if (worn == held->items.end())
	{
		tortureStrip_.erase(f.cageBuilding);
		return false;
	}
	// +0x92 += dt; the piece goes while acc x 0.04 > item+0x228 + item+0x22c: FighterArmour's cut and blunt (their +0x228 / +0x22c, RESOLUTION 4.3). A piece
	// with no FighterArmour row (the inventory and the body out of step) weighs 0.
	const std::int32_t bodySlot = worn->slot - state::Inventory::kArmourBase;
	const auto piece = std::find_if(f.armour.begin(), f.armour.end(), [&](const state::FighterArmour& p) { return p.slot == bodySlot; });
	const double weight = piece != f.armour.end() ? piece->cut + piece->blunt : 0.0;
	double& acc = tortureStrip_[f.cageBuilding];
	acc += kTickSeconds;
	if (acc * 0.04 <= weight)
		return true;
	acc = 0.0;
	const std::uint8_t slot = worn->slot;
	state::Inventory copy = *held;
	copy.items.erase(std::find_if(copy.items.begin(), copy.items.end(), [&](const state::Inventory::Entry& e) { return e.slot == slot; }));
	++copy.version;
	entity.set<state::Inventory>(std::move(copy));
	f.armour.erase(std::remove_if(f.armour.begin(), f.armour.end(), [&](const state::FighterArmour& p) { return p.slot == bodySlot; }), f.armour.end());
	mirrorFighter(id);
	return true;
}

bool Simulation::factionAllied(std::uint64_t from, std::uint64_t to) const
{
	flecs::entity mine;
	flecs::entity theirs;
	world_.each([&](flecs::entity entity, const state::FactionState&, const state::Id& id)
	{
		if (id.value == from)
			mine = entity;
		else if (id.value == to)
			theirs = entity;
	});
	return mine.is_valid() && theirs.is_valid() && factionRelation(*gameData_, mine, theirs) >= FactionBook::kAlly;
}

void Simulation::stepWorldTasks()
{
	if (combat_ == nullptr || combat_->empty() || gameData_ == nullptr)
		return;
	if (residentData_ == nullptr)
		residentData_ = std::make_shared<ResidentData>(*gameData_);
	flecs::world& ecs = ecsMutable();
	const std::uint64_t now = clockTick();
	const auto& members = combat_->members();
	for (auto it = worldTaskBlocks_.begin(); it != worldTaskBlocks_.end();)
	{
		if (it->second > now)
		{
			++it;
			continue;
		}
		if (it->second == 0) // operator[] on a lookup leaves a zero: never a block
		{
			it = worldTaskBlocks_.erase(it);
			continue;
		}
		// addBlockedTarget at expiry (tickCurrentTask 14050f0b0, dt4_unreachable_target.md): the navmesh query on the target's current
		// position; it passes and the entry is erased, it fails and the 20 s timer is re-armed, so an unreachable body stays blocked.
		// UNSURE: ours asks routeFrom from the member (the exe's query is the target position on the navmesh); a target that is not
		// a character (a box, a gone body) is erased without a query.
		const state::Fighter* member = combat_->find(it->first.first);
		const state::Fighter* target = combat_->find(it->first.second);
		if (member != nullptr && target == nullptr && navigation_ != nullptr && !member->dead)
		{
			// A producer the member's Use order gave up on (interact.cpp): the same query through the order's own route.
			const InteractTarget deposit = findInteractTarget(ecsMutable(), combat_.get(), it->first.second, this);
			if (deposit.kind == InteractKind::Producer)
			{
				const Route route = routeToInteract(*navigation_, *member, deposit, static_cast<std::uint8_t>(InteractAction::Use), false, body(*member));
				if (route.found)
					it = worldTaskBlocks_.erase(it);
				else
					(it++)->second = now + kBlockTicks;
				continue;
			}
		}
		bool reachable = member == nullptr || target == nullptr || member->dead || target->dead || navigation_ == nullptr;
		if (!reachable)
			reachable = navigation_->routeFrom({member->x, member->y, member->z}, target->x, target->z, target->y, false, body(*member)).found;
		if (reachable)
			it = worldTaskBlocks_.erase(it);
		else
			(it++)->second = now + kBlockTicks;
	}
	for (auto it = eatenUntil_.begin(); it != eatenUntil_.end();)
		it = it->second <= now || combat_->find(it->first) == nullptr ? eatenUntil_.erase(it) : std::next(it);
	// 230: the corpses whose last bite landed in the previous pass go now (the executor loop below holds the arena by index).
	for (const std::uint64_t eaten : std::exchange(eatenQueue_, {}))
		removeFighter(eaten);
	for (auto it = eatenBites_.begin(); it != eatenBites_.end();)
		it = combat_->find(it->first) == nullptr ? eatenBites_.erase(it) : std::next(it);

	// True when the route ends where the walker already stands (the nearest walkable point to the target is here).
	const auto walkTo = [&](std::uint64_t id, const state::Fighter& target)
	{
		state::Fighter& f = *combat_->find(id);
		f.clearRoute();
		if (navigation_ == nullptr)
			return false;
		const Route route = routeRetry(id, f, target.x, target.z, target.y, true);
		if (!route.found || route.points.empty())
			return false; // no way this second: it stands and tries again next second (never a terminal state)
		if (std::hypot(route.points.back().x - f.x, route.points.back().z - f.z) <= kBodyReach)
			return true; // the nearest reachable point is where it stands: the task's own arrival test takes it from here
		std::vector<state::RoutePoint> points;
		for (std::size_t p = 0; p < route.points.size(); ++p)
			points.push_back({route.points[p].x, p < route.heights.size() ? route.heights[p] : f.y, route.points[p].z});
		setRoute(f, std::move(points), navigation_.get(), body(f));
		++f.routeVersion;
		f.routeSeq = 0;
		f.routeChase = false;
		f.routeApproach = route.approach;
		f.routeSource = state::Fighter::PlanSource::Plan; // a world task's walk is a plan (task fight-goals)
		f.routeTries = 0;
		mirrorFighter(id);
		return false;
	};

	// The same walk at a bare point (task patrol: 36 / 22's goal-less walks and 216's search points have no fighter).
	// False when no walk was set (no mesh, no route, or the route ends at the walker): the callers that must never wait for ever end on it.
	const auto walkToPoint = [&](std::uint64_t id, double toX, double toZ)
	{
		state::Fighter& f = *combat_->find(id);
		f.clearRoute();
		if (navigation_ == nullptr)
			return false;
		const double y = groundHeight(toX, toZ).value_or(f.y);
		const Route route = routeRetry(id, f, toX, toZ, y, true);
		if (!route.found || route.points.empty())
			return false; // no way this second: it stands and tries again next second
		if (std::hypot(route.points.back().x - f.x, route.points.back().z - f.z) <= kBodyReach)
			return false; // arrived as near as the mesh allows
		std::vector<state::RoutePoint> points;
		for (std::size_t p = 0; p < route.points.size(); ++p)
			points.push_back({route.points[p].x, p < route.heights.size() ? route.heights[p] : f.y, route.points[p].z});
		setRoute(f, std::move(points), navigation_.get(), body(f));
		++f.routeVersion;
		f.routeSeq = 0;
		f.routeChase = false;
		f.routeApproach = route.approach;
		f.routeSource = state::Fighter::PlanSource::Plan; // a world task's walk is a plan (task fight-goals)
		f.routeTries = 0;
		mirrorFighter(id);
		return true;
	};

	// ---- task patrol: the town's buildings with an interior and 216's search route (world_resolvers.hpp) ----
	// Cached (kenshi-k16.6.233): rebuilt only when a town's key (its entity, centre and record sids), the door system or the gate sites
	// differ from the last tick's; the check is a cached-query walk with no allocation and no record lookup.
	struct PreludeCache
	{
		struct TownKey
		{
			flecs::entity_t id = 0;
			double x = 0.0;
			double z = 0.0;
			std::string rootSid;
			std::string activeSid;
			bool operator==(const TownKey&) const = default;
		};
		std::vector<TownKey> keys;
		const DoorSystem* doors = nullptr;
		bool doorsOn = false;
		std::size_t doorGates = 0;
		std::size_t doorTowns = 0;
		std::size_t gateSites = 0;
		bool built = false;
		std::vector<towncircle::Circle> townCircles;
		std::vector<flecs::entity_t> townIds; // the TownState entity of each circle, the same order (112's "military of MY town": 140599600's home-town test, dt24_112 section 3)
		std::vector<worldres::Fortification> fortifications;
	};
	if (!wtPrelude_)
		wtPrelude_ = std::make_shared<PreludeCache>();
	PreludeCache& prelude = *std::static_pointer_cast<PreludeCache>(wtPrelude_);
	const DoorSystem* doors = ai_ != nullptr ? ai_->doorSystem() : nullptr;
	if (doors != nullptr && !doors->tuning().enabled)
		doors = nullptr; // switched off (--no-doors): the same as none
	if (!wtTownQuery_)
		wtTownQuery_.emplace(ecs.query<const state::TownState>());
	bool same = prelude.built && prelude.doors == doors && prelude.gateSites == world_.gateSites().size()
		&& (doors == nullptr || (prelude.doorGates == doors->stats().gates && prelude.doorTowns == doors->stats().gateTowns));
	std::size_t townCount = 0;
	if (same)
	{
		wtTownQuery_->each([&](flecs::entity entity, const state::TownState& town)
		{
			if (same && (townCount >= prelude.keys.size() || !(prelude.keys[townCount] == PreludeCache::TownKey{entity.id(), town.x, town.z, town.rootSid, town.activeSid})))
				same = false;
			++townCount;
		});
		same = same && townCount == prelude.keys.size();
	}
	if (!same)
	{
		prelude = PreludeCache{};
		prelude.built = true;
		prelude.doors = doors;
		prelude.gateSites = world_.gateSites().size();
		if (doors != nullptr)
		{
			prelude.doorGates = doors->stats().gates;
			prelude.doorTowns = doors->stats().gateTowns;
		}
		wtTownQuery_->each([&](flecs::entity entity, const state::TownState& town)
		{
			prelude.keys.push_back({entity.id(), town.x, town.z, town.rootSid, town.activeSid});
			prelude.townCircles.push_back(towncircle::of(*gameData_, town));
			prelude.townIds.push_back(entity.id());
		});
		const std::vector<towncircle::Circle>& townCircles = prelude.townCircles;
		const std::vector<flecs::entity_t>& townIds = prelude.townIds;
		std::vector<worldres::Fortification>& fortifications = prelude.fortifications;
		// The gates each town offers an assault (270, task raid-goals): the door system's own gate -> town binding (gatebind.hpp: the
		// nearest town centre, no cutoff) mapped back to the circle index; a gate with no town is left out. With no door system
		// (--no-doors) the same nearest-centre rule binds the gate itself, so 270 is still offered (kenshi-k16.6.66).
		if (!townIds.empty())
		{
			std::map<flecs::entity_t, int> circleOf;
			for (std::size_t t = 0; t < townIds.size(); ++t)
				circleOf.emplace(townIds[t], static_cast<int>(t));
			for (const GateSite& gate : world_.gateSites())
			{
				int circle = -1;
				if (doors != nullptr)
				{
					if (const auto at = circleOf.find(doors->townOf(gate.key)); at != circleOf.end())
						circle = at->second;
				} else {
					const std::size_t near = nearestTownIndex(gate.x, gate.z, townCircles.size(),
						[&](std::size_t i) { return std::pair<double, double>(townCircles[i].x, townCircles[i].z); });
					circle = static_cast<int>(near);
				}
				if (circle >= 0 && circle < static_cast<int>(townIds.size()))
					fortifications.push_back({gate.key, gate.building, gate.x, gate.z, circle});
			}
		}
	}
	const std::vector<towncircle::Circle>& townCircles = prelude.townCircles;
	const std::vector<flecs::entity_t>& townIds = prelude.townIds;
	const std::vector<worldres::Fortification>& fortifications = prelude.fortifications;
	// The cages around a member (dt15 section 0's town finder; OURS: every building within 2,000 u, from the town scorer's site index,
	// where the exe walks the town's 3x3 grid cells). A cage's faction is the nearest town's owner.
	const auto cagesNear = [&](const state::Fighter& f) -> std::vector<worldres::Cage>
	{
		std::vector<worldres::Cage> cages;
		std::uint64_t townFaction = 0;
		double bestTown = 2000.0;
		world_.each([&](const state::TownState& town)
		{
			const double d = std::hypot(town.x - f.x, town.z - f.z);
			if (d > bestTown || town.owner.id == 0 || !ecs.is_alive(town.owner.id))
				return;
			const state::Id* townId = flecs::entity(ecs.c_ptr(), town.owner.id).get<state::Id>();
			if (townId != nullptr)
			{
				bestTown = d;
				townFaction = townId->value;
			}
		});
		// A later placement of a TOWN record (a save's outpost, e.g. SkinHouse's village) is an ExtraTown, not a TownState (kenshi-cym.28).
		world_.each([&](const state::ExtraTown& town)
		{
			const double d = std::hypot(town.x - f.x, town.z - f.z);
			if (!town.placed || d > bestTown || town.owner.id == 0 || !ecs.is_alive(town.owner.id))
				return;
			const state::Id* townId = flecs::entity(ecs.c_ptr(), town.owner.id).get<state::Id>();
			if (townId != nullptr)
			{
				bestTown = d;
				townFaction = townId->value;
			}
		});
		const auto addCages = [&](const BuildingSite& site)
		{
			if (std::hypot(site.cx - f.x, site.cz - f.z) > 2000.0)
				return;
			if (!ensureBuilding(*residentData_, world_, ecs, *gameData_, site.key))
				return;
			const routines::BuildingSites* building = residentData_->sites.find(site.key);
			if (building == nullptr)
				return;
			// The building's faction: its own placement `owner faction ID`, else the nearest town's owner (dt24_172's finder runs over
			// the CAPTOR's faction, and a slave shop inside a UC town is the slavers'). A sid no faction instance carries (a default or
			// mod placeholder) keeps the town's.
			std::uint64_t faction = townFaction;
			if (!site.ownerFaction.empty())
			{
				std::uint64_t owner = 0;
				world_.each([&](flecs::entity entity, const state::FactionState& state)
				{
					if (state.templateSid != site.ownerFaction)
						return;
					const state::Id* factionId = entity.get<state::Id>();
					owner = factionId != nullptr ? factionId->value : static_cast<std::uint64_t>(entity.id());
				});
				if (owner != 0)
					faction = owner;
			}
			for (const routines::SiteEntry* entry : residentData_->sites.sitesOf(site.key, routines::SiteKind::Cage))
			{
				worldres::Cage cage;
				cage.site = entry->site;
				cage.building = site.key;
				cage.x = entry->x;
				cage.z = entry->z;
				cage.hasFloor = entry->hasY;
				cage.floorY = entry->y;
				for (const CombatArena::Member& other : members)
					if (other.fighter.cageBuilding == site.key && other.fighter.cageSlot == entry->site.slot)
					{
						cage.occupant = other.id;
						cage.occupantFaction = other.fighter.faction;
						break;
					}
				cage.town = towncircle::at(townCircles, entry->x, entry->z); // 276's "in my town"
				cage.freeSlot = cage.occupant == 0;
				cage.claimed = routineBook_.holder(cage.site) != nullptr;
				cage.myHome = f.homeBuilding != 0 && f.homeBuilding == site.key;
				cage.faction = faction;
				cage.myFaction = faction != 0 && f.faction != 0 && faction == f.faction;
				cage.ownerAllied = cage.occupant != 0 && faction != 0 && f.faction != 0 && faction != f.faction && factionAllied(faction, f.faction);
				cage.publicBuilding = building->publicBuilding;
				cages.push_back(std::move(cage));
			}
		};
		for (const BuildingSite& site : world_.buildingSites())
			addCages(site);
		for (const BuildingSite& site : world_.furnitureCageSites()) // placed cage furniture (a Peeler Machine), kept out of buildingSites
			addCages(site);
		return cages;
	};

	// 106's bed (dt24_106 + dt29): the buildings within 2,000 u of the member that are its faction's or allied to it, the owner as
	// cagesNear reads it, run through the shared bed resolver (worldres::bedLadder153); the first free bed of the building it names
	// (slot order, no squadmate's claim). OURS: the town scope is the 2,000 u reach, as the cages'; a player faction's scoring path
	// needs the town's player flag, which the world selector has no use for, so it takes the NPC order.
	const auto bedFor106 = [&](const state::Fighter& f, routines::Site& pick) -> bool
	{
		if (gameData_ == nullptr || residentData_ == nullptr)
			return false;
		const sensor::Context relations = sensorContext();
		std::uint64_t townFaction = 0;
		double bestTown = 2000.0;
		world_.each([&](const state::TownState& town)
		{
			const double d = std::hypot(town.x - f.x, town.z - f.z);
			if (d > bestTown || town.owner.id == 0 || !ecs.is_alive(town.owner.id))
				return;
			const state::Id* townId = flecs::entity(ecs.c_ptr(), town.owner.id).get<state::Id>();
			if (townId != nullptr)
			{
				bestTown = d;
				townFaction = townId->value;
			}
		});
		std::vector<worldres::BedBuilding> candidates;
		for (const BuildingSite& site : world_.buildingSites())
		{
			if (std::hypot(site.cx - f.x, site.cz - f.z) > 2000.0 || !ensureBuilding(*residentData_, world_, ecs, *gameData_, site.key))
				continue;
			const routines::BuildingSites* building = residentData_->sites.find(site.key);
			if (building == nullptr)
				continue;
			std::uint64_t faction = townFaction;
			if (!site.ownerFaction.empty())
				world_.each([&](flecs::entity entity, const state::FactionState& state)
				{
					if (state.templateSid != site.ownerFaction)
						return;
					const state::Id* factionId = entity.get<state::Id>();
					faction = factionId != nullptr ? factionId->value : static_cast<std::uint64_t>(entity.id());
				});
			const bool own = faction != 0 && f.faction != 0 && faction == f.faction;
			if (!own && (faction == 0 || relations.relation(faction, f.faction) < 1.0))
				continue; // the finders' accept: my faction or allied
			candidates.push_back({site.key, building->designation, own,
				static_cast<std::int32_t>(residentData_->sites.sitesOf(site.key, routines::SiteKind::Bed).size()), site.cx, site.cz});
		}
		const std::uint64_t homeKey = f.homeBuilding;
		const int homeBeds = homeKey != 0 ? static_cast<int>(residentData_->sites.sitesOf(homeKey, routines::SiteKind::Bed).size()) : 0;
		for (const std::uint64_t key : worldres::bedLadder153(homeKey, homeBeds, f.npcClass, false, candidates, f.x, f.z))
			for (const routines::SiteEntry* bed : residentData_->sites.sitesOf(key, routines::SiteKind::Bed))
				if (routineBook_.holder(bed->site) == nullptr)
				{
					pick = bed->site;
					return true;
				}
		return false;
	};

	std::map<int, std::vector<worldres::TownBuilding>> searchByTown;
	const auto searchBuildings = [&](const state::Fighter& who) -> const std::vector<worldres::TownBuilding>&
	{
		static const std::vector<worldres::TownBuilding> none;
		const int at = towncircle::at(townCircles, who.x, who.z);
		if (at < 0 || townCircles[static_cast<std::size_t>(at)].radius <= 0.0)
			return none;
		std::vector<worldres::TownBuilding>& out = searchByTown[at];
		if (!out.empty())
			return out;
		const towncircle::Circle& circle = townCircles[static_cast<std::size_t>(at)];
		for (const BuildingSite& site : world_.buildingSites())
			if (site.hasInterior && !site.ruined && std::hypot(site.cx - circle.x, site.cz - circle.z) <= circle.radius)
				out.push_back({site.key});
		return out;
	};
	// The door side of a building (the first door moved 4 u toward the centre; the footprint's centre when it has none).
	std::optional<std::map<std::uint64_t, std::array<double, 2>>> doorCache;
	const auto doorOf = [&](const BuildingSite& site) -> std::array<double, 2>
	{
		if (!doorCache)
		{
			doorCache.emplace();
			std::map<std::uint64_t, std::uint64_t> lowest;
			world_.each([&](const state::Door& door)
			{
				if (door.building == 0)
					return;
				const auto at = lowest.find(door.building);
				if (at == lowest.end() || door.key < at->second)
				{
					lowest[door.building] = door.key;
					(*doorCache)[door.building] = {door.x, door.z};
				}
			});
		}
		const auto found = doorCache->find(site.key);
		if (found == doorCache->end())
			return {site.cx, site.cz};
		const double dx = site.cx - found->second[0];
		const double dz = site.cz - found->second[1];
		const double length = std::hypot(dx, dz);
		if (length <= 4.0)
			return found->second;
		return {found->second[0] + dx / length * 4.0, found->second[1] + dz / length * 4.0};
	};
	const auto searchRoute = [&](std::uint64_t building, std::uint64_t diceKey) -> std::vector<worldres::SearchSpot>
	{
		const BuildingSite* site = world_.buildingSite(building);
		if (site == nullptr)
			return {};
		const std::array<double, 2> door = doorOf(*site);
		worldres::SearchBuilding b;
		b.key = building;
		b.cx = site->cx;
		b.cz = site->cz;
		b.axisX = site->axisX;
		b.axisZ = site->axisZ;
		b.halfA = site->halfA;
		b.halfB = site->halfB;
		b.groundY = site->originY;
		b.doorX = door[0];
		b.doorZ = door[1];
		if (!site->floors.empty())
		{
			b.upperFloors = true;
			b.firstFloor = site->floors.front();
		}
		return worldres::searchRoute216(b, seed_, diceKey, [&](double x, double z, worldres::SearchSpot& out)
		{
			if (navigation_ == nullptr)
			{
				out = {x, z, false, 0.0};
				return true;
			}
			const std::optional<knav::Position> snapped = navigation_->walker().snap({x, b.groundY, z}, worldres::kSearchSnapReach);
			if (!snapped || !insideFootprint(*site, {snapped->x, snapped->z}) || buildingAt(snapped->x, snapped->y, snapped->z) == 0)
				return false; // the walking surface must stand inside the building, as the sim's own membership reads it
			out = {snapped->x, snapped->z, true, snapped->y};
			return true;
		});
	};

	// The member's goal rows: the applied package of its squad's Blackboard, for its role (dt19 section 4: the package's own
	// rows and its `inherits from` parents', merged parents-first into the board's role lists - the PackageTable's lists for
	// the applied record). No applied package = no rows: the member keeps the squad's follow (IDLE).
	const auto rowsOf = [&](const blackboard::Board& board, routines::Role role) -> const std::vector<routines::GoalRow>&
	{
		static const std::vector<routines::GoalRow> none;
		const routines::CompiledPackage* package = board.current.empty() || residentData_ == nullptr
			? nullptr : residentData_->packages.find(board.current);
		return package != nullptr ? package->list(role) : none;
	};

	// What a member scores: the applied package's rows for its role with its race's `AI Goals` under them (dt34; the race list is
	// read only while a package is applied, so a package-less member keeps the squad's follow).
	const auto rowsFor = [&](const blackboard::Board& board, routines::Role role, const std::string& raceSid)
	{
		const routines::CompiledPackage* package = board.current.empty() || residentData_ == nullptr
			? nullptr : residentData_->packages.find(board.current);
		if (package == nullptr)
			return std::vector<routines::GoalRow>{};
		return residentData_->packages.rowsWithRace(package, role, raceSid);
	};

	// checkCurrentTask's window test for a selector goal (dt14 section 6): false when the applied package's rows for the
	// member's role have no row of the goal whose window holds now. Missing data keeps the task (an interruption never ends on
	// missing data).
	const auto goalWindowOpen = [&](flecs::entity actor, std::uint16_t goal, const std::string& raceSid) -> bool
	{
		const flecs::entity squad = actor.target<state::MemberOf>();
		const blackboard::Board* board = blackboardOf(squad);
		if (board == nullptr)
			return true;
		const routines::Role listRole = liveRole(actor);
		const std::vector<routines::GoalRow> rows = rowsFor(*board, listRole, raceSid);
		const auto found = std::find_if(rows.begin(), rows.end(), [goal](const routines::GoalRow& row) { return row.task == goal; });
		if (found == rows.end())
			return false; // the goal left the lists: done
		const double hour = std::fmod(trader::gameHours(now), 24.0);
		return goalsel::windowHolds(found->start, found->end, hour);
	};

	KSIM_PROFILE_SUB(WtPrelude);
	// ---- lock orders (task ds/shackles-locks): the player-order tasks 76 / 285 / 286 / 201 / 185 / 186 / 219 / 110 / 133 ----
	for (auto it = lockOrders_.begin(); it != lockOrders_.end();)
	{
		const std::uint64_t actorId = it->first;
		const LockOrder order = it->second;
		state::Fighter* actorFighter = combat_->find(actorId);
		state::Fighter* victimFighter = order.victim != 0 ? combat_->find(order.victim) : nullptr;
		// A cage order (actions 18..20) ends gone (event 13 result 3) when nobody is caged there any more: the occupant it walks to left or died away.
		const bool caged = order.cage == 0 || (victimFighter != nullptr && victimFighter->cageBuilding == order.cage);
		if (actorFighter == nullptr || (order.victim != 0 && victimFighter == nullptr) || !caged)
		{
			if (actorFighter != nullptr && order.action != 0)
				reportInteract(actorId, order.cage != 0 ? order.cage : order.victim, order.action, InteractEvent::kGone, 0);
			it = lockOrders_.erase(it);
			continue;
		}
		if (victimFighter != nullptr && std::hypot(actorFighter->x - victimFighter->x, actorFighter->z - victimFighter->z) > lockReach(*victimFighter, false)
			&& !actorFighter->walking() && inSlice(now, actorId))
			walkTo(actorId, *victimFighter);
		if (actorFighter->orderPause > 0) // stopped for this update by a targeting (stepOrderCombat), the shared lock progress stays
		{
			++it;
			continue;
		}
		if (runWeaponPrefix(actorId, order.task)) // the picks and cuts need empty hands: 7 UNEQUIP_WEAPON first
		{
			++it;
			continue;
		}
		const LockStep step = stepLockTask(actorId, order.task, order.victim);
		if (step != LockStep::Working && order.action != 0)
		{
			const bool incapacitated = actorFighter->dead || actorFighter->unconscious || actorFighter->down || actorFighter->carriedBy != 0;
			reportInteract(actorId, order.cage != 0 ? order.cage : order.victim, order.action,
				step == LockStep::Done ? InteractEvent::kDone : incapacitated ? InteractEvent::kIncapacitated
					: step == LockStep::TooHard ? InteractEvent::kTooHard : InteractEvent::kRefused, 0);
		}
		it = step == LockStep::Working ? std::next(it) : lockOrders_.erase(it);
	}

	// ---- construction orders (task ds/construction): the player-order tasks 2 / 71 / 95 / 96 / 97 / 156 / 200 on a player building ----
	if (!buildOrders_.empty())
		stepConstructionOrders(walkToPoint);
	if (!arrestOrders_.empty())
		stepArrestOrders(walkToPoint);
	if (!smashOrders_.empty())
		stepSmashOrders(walkToPoint);
	stepCrops();
	if (!cropOrders_.empty())
		stepCropOrders(walkToPoint);
	stepAnimalGoals(walkToPoint);
	stepStandingGoals(walkToPoint);
	stepPrisonRelease();

	KSIM_PROFILE_SUB(WtOrders);
	// ---- the tasks in hand ----
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		if (!members[i].due)
			continue; // the arena did not step it this tick (kenshi-k16.6.639)
		const double dt = members[i].dueTicks * kTickSeconds; // the time since its last pass
		flecs::entity actor = ecs.entity(combatEntities_[i]);
		const state::Routine* current = actor.get<state::Routine>();
		if (current == nullptr)
			continue;
		const bool captureGoal = current->goal == routines::task::CaptureSlaves;
		const bool lootGoal = current->goal == routines::task::LootTheDead;
		if (!captureGoal && !lootGoal && !selectorGoal(current->goal))
			continue; // 112 is carry.cpp's own
		const flecs::entity squad = actor.target<state::MemberOf>();
		// A resident's town routine is the town scorer's (the cage warden's own 167 an exception); the pull goals are not - the scorer
		// hands a worker over to this pipeline when its work site's machine is full (dt24_emptymachine_source), and takes it back
		// when the pull ends.
		const bool residentPull = squad.is_valid() && squad.has<state::SquadRoutine>()
			&& (current->goal == routines::task::EmptyMachine || current->goal == routines::task::BuildMats
				|| current->goal == routines::task::LootContainer || current->goal == routines::task::FirstAid);
		if (!squad.is_valid() || (squad.has<state::SquadRoutine>() && !residentPull))
			continue;
		if (current->goal == routines::task::GatePost && squad.has<state::GateCrew>())
			continue; // a gate crew's post is task 51 too, held by stepGateCrewPosts; no node window ends it
		const std::uint64_t id = members[i].id;
		state::Fighter& f = *combat_->find(id);
		state::Routine r = *current;
		// The town routine ends a resident's pull by handing the member back (goal 0, the scorer's own re-score), not by removing the
		// Routine: a resident with none is never scored again.
		const auto endTask = [&]()
		{
			// The goal-commit lock (dt33): inside its window the selector refuses to drop the goal.
			if (commitLocked(r, id, now))
				return;
			routineBook_.release(id); // 20 / 255's slot claim (a no-op for the goals that hold none)
			if (r.goal == routines::task::RetreatHometown || r.goal == routines::task::RunAwayHometown)
				f.moveMode = 2; // 128's walk ends with the goal: back to the mover default
			if (!residentPull)
				actor.remove<state::Routine>();
			else {
				r.goal = 0;
				r.target = {};
				r.chainTarget = 0; // the pull's patient / body is not a 167 chain of the scorer
				r.nextScore = now;
				actor.set<state::Routine>(std::move(r));
			}
			mirrorFighter(id);
		};
		// 28 / 102 / 159 / 164 / 165 are named by an order or a plan, not by a package row: no window closes on them.
		const bool orderTask = r.goal == routines::task::StandUp || r.goal == routines::task::SeekAndTalk || r.goal == routines::task::SendDialogue
			|| r.goal == routines::task::FollowWhileTalking || r.goal == routines::task::TownStalker || r.goal == routines::task::SleepOnFloorFakeAmbush
			|| r.goal == routines::task::LootAnimalsJob;
		if (selectorGoal(r.goal) && !orderTask && !residentPull && inSliceScaled(now, id, f.playerControlled || inFight(f)) && !goalWindowOpen(actor, r.goal, f.raceSid))
		{
			// A floor sleeper whose package window closes (the animals' 22-6 / 23-6 rows) wakes with it; endTask mirrors the wake.
			if (r.goal == routines::task::SleepOnFloor)
				f.asleep = false;
			// A sitter's window can close too (a contract replacing its package): its sheathe is undone with the task.
			if (r.goal == routines::task::SitAround && r.chainElapsed >= 1.0)
				drawWeapon(id);
			endTask();
			continue;
		}
		// ---- task patrol: 36 / 22's walks and 216's search route (dt24_36) ----
		if (r.goal == routines::task::PatrolTown || r.goal == routines::task::Patrol || r.goal == routines::task::RunAroundTown)
		{
			if (f.dead || f.unconscious || f.down || f.carriedBy != 0)
			{
				actor.remove<state::Routine>();
				mirrorFighter(id);
				continue;
			}
			if (r.goal == routines::task::RunAroundTown)
			{
				const std::uint64_t diceKey = f.diceSeed != 0 ? f.diceSeed : id;
				if (r.searchBuilding == 0)
				{
					std::vector<state::RoutineSearch> marks; // the squad's memory: every member's marks
					ecs.query_builder<const state::Routine>().with<state::MemberOf>(squad).build().each(
						[&](const state::Routine& mine) { marks.insert(marks.end(), mine.searched.begin(), mine.searched.end()); });
					r.searchBuilding = worldres::runAroundTown216(searchBuildings(f), marks, seed_, diceKey, now, worldres::kSearchMemoryTicks);
					r.searchPoint = 0;
					r.searchLookUntil = 0;
				}
				const std::vector<worldres::SearchSpot> route = r.searchBuilding != 0 ? searchRoute(r.searchBuilding, diceKey) : std::vector<worldres::SearchSpot>{};
				if (route.empty())
				{
					actor.remove<state::Routine>(); // no building with an interior, or none placed: back to the follow
					mirrorFighter(id);
					continue;
				}
				if (r.searchLookUntil != 0)
				{
					if (now < r.searchLookUntil)
						continue; // looking around at the point
					r.searchLookUntil = 0;
					++r.searchPoint;
				}
				if (r.searchPoint >= route.size())
				{
					// The route is done: remember the building (the squad's shared memory) and pick another.
					r.searched.push_back({r.searchBuilding, now});
					if (r.searched.size() > worldres::kSearchMarks)
						r.searched.erase(r.searched.begin());
					r.searchBuilding = 0;
					r.searchPoint = 0;
					actor.set<state::Routine>(std::move(r));
					continue;
				}
				const worldres::SearchSpot& spot = route[r.searchPoint];
				if (!f.walking() && std::hypot(f.x - spot.x, f.z - spot.z) <= kBodyReach)
				{
					// Arrived: look around for a few seconds (a turn away from the approach), then the next point.
					RngStream look(streamSeed(streamSeed(streamSeed(seed_, kSearchStream), static_cast<std::size_t>(diceKey)),
						static_cast<std::size_t>(r.searchBuilding ^ (0x9E3779B97F4A7C15ull * static_cast<std::uint64_t>(r.searchPoint + 1)))));
					r.searchLookUntil = now + worldres::kSearchLookTicks;
					f.yaw = 2.0 * std::numbers::pi * look.uniform01();
					mirrorFighter(id);
				} else if (!f.walking()) {
					walkToPoint(id, spot.x, spot.z);
				}
				actor.set<state::Routine>(std::move(r));
				continue;
			}
			// 36 / 22: a goal-less walk, re-issued each second while it stands. 36's anchor is the squad's home point (the package
			// target's stand-in), 22's the member's own position.
			if (!f.walking())
			{
				RngStream dice(streamSeed(streamSeed(streamSeed(seed_, kRoutineStream), static_cast<std::size_t>(id)), static_cast<std::size_t>(now + 1)));
				const double angle = dice.uniform01();
				const double radiusRoll = dice.uniform01();
				if (r.goal == routines::task::PatrolTown)
				{
					// 36: the package's town (dt24_patrol_town_pot sections 1-2), a uniform point +/- townRadius/4 of its centre each
					// leg (the order's radius floor 500, so a small town gets +/- 250); no town target, the exe's +/-625 u random walk
					// around the member itself.
					towncircle::Circle circle;
					if (const state::SquadInstance* instance = squad.get<state::SquadInstance>();
						instance != nullptr && instance->homeTown.id != 0 && ecs.is_alive(instance->homeTown.id))
						if (const state::TownState* town = ecs.entity(instance->homeTown.id).get<state::TownState>())
							circle = towncircle::of(*gameData_, *town);
					const worldres::PatrolSpot spot = worldres::patrolSpot36(f.x, f.z, circle.radius > 0.0, circle.x, circle.z, circle.radius,
						angle, radiusRoll);
					walkToPoint(id, spot.x, spot.z);
				} else {
					// 22 PATROL (dt24_patrol_town_pot section 2): the zone's neighbouring sectors, weighted at random; zones and their
					// sectors are not modelled, so ours is a point around the member (UNSURE).
					const double off = routines::kPatrolMin + radiusRoll * (routines::kPatrolRadius - routines::kPatrolMin);
					walkToPoint(id, f.x + off * std::cos(2.0 * std::numbers::pi * angle), f.z + off * std::sin(2.0 * std::numbers::pi * angle));
				}
			}
			continue;
		}
		// STEP SLAVE TRADE walks, strips and restrains a capture; this pass only bounds the walk (a body that cannot be reached).
		if (r.goal == routines::task::CaptureSlaves)
		{
			if (r.chainElapsed < 0.0 && f.carrying == 0 && static_cast<double>(ticksSince(now, r.since)) * kTickSeconds > kGiveUpSeconds)
			{
				worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
				actor.remove<state::Routine>();
				mirrorFighter(id);
				// Task BR2: the walk never reached the body (the client's log).
				std::ostringstream line;
				line << "capture end 167 member " << id << " body " << r.chainTarget << " (the walk was given up after "
					<< kGiveUpSeconds << " s)";
				logTask(line.str());
			}
			continue;
		}
		if (r.goal == routines::task::FirstAid)
		{
			// 58 JOB_MEDIC (dt24_58.md): the selector picked the patient (medic58's score); the plan [25, 58] says whether aid is
			// still owed - 25 FIRST_AID_ORDER is the treatment interact, and the goal entry is popped. Re-planned here each pass: the
			// patient healed, gone or dead leaves nothing to do.
			const state::Fighter* patient = combat_->find(r.chainTarget);
			const goap::Plan chain = patient != nullptr ? planMedicChain(id, r.chainTarget) : goap::Plan{};
			const bool owed = chain.found() && std::any_of(chain.steps.begin(), chain.steps.end(), [](const goap::Step& step) { return step.task == goap::kFirstAidOrder; }); // [25, 58] or [7, 25, 58]; [58] alone = the goal already holds
			// No terminal state is missing: a medic that has not finished in kAidGiveUpSeconds drops the patient for the 20 s block.
			const bool gaveUp = static_cast<double>(ticksSince(now, r.since)) * kTickSeconds > kAidGiveUpSeconds;
			if (patient == nullptr || patient->dead || f.dead || f.unconscious || f.down || !owed || gaveUp)
			{
				if (gaveUp)
					worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
				endTask(); // a resident squad's member is handed back to the town scorer, a world squad's Routine is removed
				continue;
			}
			if (actor.has<state::InteractTask>())
				continue; // the treatment interact runs itself, one tick of treat() at a time
			if (chain.steps.front().task == routines::task::UnequipWeapon && sheatheWeapon(id)) // the plan's 7, before the aid (rwh250)
				continue;
			const InteractTarget aim = findInteractTarget(ecs, combat_.get(), r.chainTarget, this);
			if (aim.kind != InteractKind::None && aim.within(f))
			{
				beginInteract(id, r.chainTarget, static_cast<std::uint8_t>(InteractAction::FirstAid), 0, {}, false);
				continue;
			}
			if (inSlice(now, id))
				walkTo(id, *patient);
			continue;
		}
		if (r.goal == routines::task::EatRandomDeadBody || r.goal == routines::task::EatRandomKoBody || routines::task::eatsCageOccupant(r.goal))
		{
			// 139 EAT_A_RANDOM_CAGE_OCCUPANT (dt29_fog_kidnap sections 6, 8) shares this executor: 220 on a LIVE caged victim. The bite bleeds
			// the victim and marks it "being fed on" (237 reads the mark); the body is not removed. The goal ends when the victim dies
			// or leaves the world - the next pass then picks it as a corpse (230) or another cage.
			const bool live = routines::task::eatsCageOccupant(r.goal) || r.goal == routines::task::EatRandomKoBody; // 233 eats the living KO body like a cage occupant
			// 230 EAT_A_RANDOM_DEAD_BODY -> 220 Task_EatPrisoner (dt27 section 2): the plan walks to the corpse, the task checks the
			// race (an inedible one is flagImpossible - blocked for the 20 s set, the next pass picks another), approaches to 20 u,
			// sheathes (the plan's 7: 220 needs hasWeaponEquipped = 0, M), then bites once per tick (140650370's counter: six bites
			// count, the next one removes the body as "eaten"). The eat clip, the EATING_SOMETHING_SOUNDS event (0x34) and the
			// RUN_AWAY branch of char+0x278 (U) are not modelled; the bite feeds nobody (the exe's tick does not).
			state::Fighter* body = combat_->find(r.chainTarget);
			if (body == nullptr || body->dead != !live || body->carriedBy != 0 || f.dead || f.unconscious || f.down || f.carriedBy != 0
				|| f.carrying != 0)
			{
				endTask();
				continue;
			}
			if (inedibleRace(*body))
			{
				worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
				endTask();
				continue;
			}
			if (std::hypot(f.x - body->x, f.z - body->z) > kEatReach)
			{
				if (inSlice(now, id))
					walkTo(id, *body);
				continue;
			}
			if (f.walking())
			{
				f.clearRoute();
				f.forwardSpeed = 0.0;
				f.lateralSpeed = 0.0;
			}
			if (sheatheWeapon(id)) // the plan's 7 (the shared step); the bite starts next tick
				continue;
			f.yaw = std::atan2(body->x - f.x, body->z - f.z);
			// The eat application (CombatArena::eat, 140650370): the EATER's race picks the limb loop or the blood drain, a live victim
			// also screams and trains toughness, and the counter past the last part removes the body as "eaten".
			const bool first = live && eatenUntil_.find(r.chainTarget) == eatenUntil_.end();
			const CombatArena::EatOutcome eaten = combat_->eat(r.chainTarget, [&] { const RaceCombat* race = combat_->data().race(f.raceSid); return race != nullptr && race->vampiric; }() /* RACE "vampiric" = +0x7d */, dt, eatenBites_[r.chainTarget]);
			if (eaten.result == CombatArena::EatResult::Inedible)
			{
				worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
				endTask();
				continue;
			}
			if (live)
				eatenUntil_[r.chainTarget] = now + static_cast<std::uint64_t>(kBeingEatenSeconds / kTickSeconds);
			if (eaten.screamed)
				fireDialogue(r.chainTarget, id, 0x0b); // SCREAMING_TORTURE every application on a conscious victim
			mirrorFighter(r.chainTarget);
			mirrorFighter(id);
			if (first)
			{
				std::ostringstream line;
				line << "eat " << r.goal << " member " << id << " bites victim " << r.chainTarget;
				logTask(line.str());
			}
			if (eaten.result == CombatArena::EatResult::Eaten)
			{
				std::ostringstream line;
				line << "eat " << r.goal << " member " << id << " ate " << (live ? "victim " : "body ") << r.chainTarget;
				logTask(line.str());
				eatenBites_.erase(r.chainTarget);
				eatenQueue_.push_back(r.chainTarget);
				endTask();
			}
			continue;
		}
		if (r.goal == routines::task::DeadGuysInThePot)
			continue; // carry.cpp's stepCorpsePot runs the chain (the window check above already ended a lapsed goal)
		if (routines::task::killsCageOccupant(r.goal))
		{
			// 135 KILL_A_RANDOM_CAGE_OCCUPANT -> 134 Task_KillPrisoner (dt27 section 3; dt24_killprisoner sections 5-6): walk to the
			// occupant, then one blow per "chop down static" pass at 55% of the clip (CharStats::computeDamage + receiveHit, our
			// CombatArena::strikeFromTask) until the goal MACHINE_OCCUPANT_IS_ALIVE = 0 holds. The class itself never ends; a
			// weapon in hand is the plan's precondition. The town cooldown on death, the victim's SCREAMING_TORTURE (0x0b) and the
			// occupant's MARKED_FOR_DEATH (0x0a) events are not modelled (UNSURE).
			state::Fighter* body = combat_->find(r.chainTarget);
			if (body == nullptr || body->dead || body->cageBuilding == 0 || f.dead || f.unconscious || f.down || f.carriedBy != 0
				|| f.carrying != 0 || f.weapon.sid == combat_->data().unarmed.sid)
			{
				endTask();
				continue;
			}
			// The class has no reach test of its own (dt24_killprisoner section 8): the plan's walk brings the killer to the
			// cage (ours: within the interact reach, 12 u - the separation push keeps two bodies apart, UNSURE), and once the clip
			// runs the blows land - a blow that moves the occupant does not stop them.
			if (r.chainElapsed <= 0.0 && std::hypot(f.x - body->x, f.z - body->z) > kInteractReach)
			{
				if (inSlice(now, id))
					walkTo(id, *body);
				continue;
			}
			if (f.walking())
			{
				f.clearRoute();
				f.forwardSpeed = 0.0;
				f.lateralSpeed = 0.0;
			}
			f.yaw = std::atan2(body->x - f.x, body->z - f.z);
			const double before = std::fmod(r.chainElapsed, kChopSeconds) / kChopSeconds;
			r.chainElapsed += dt;
			const double after = std::fmod(r.chainElapsed, kChopSeconds) / kChopSeconds;
			const std::uint64_t victim = r.chainTarget;
			const std::uint16_t killGoal = r.goal;
			actor.set<state::Routine>(std::move(r));
			if (before < kChopHitAt && after >= kChopHitAt)
			{
				combat_->strikeFromTask(id, victim);
				mirrorFighter(victim);
				if (const state::Fighter* struck = combat_->find(victim); struck != nullptr && struck->dead)
				{
					std::ostringstream line;
					line << "kill " << killGoal << " member " << id << " killed occupant " << victim;
					logTask(line.str());
				}
			}
			mirrorFighter(id);
			continue;
		}
		if (routines::task::rescues(r.goal))
		{
			// 142 FIND_AND_RESCUE_LEADER / 148 FIND_AND_RESCUE_IF_THERES_BEDS (dt24_142): the goal is IS_CARRYING_TARGET = 1, so the plan
			// is the lift (68 LIFT_PERSON, as 254's) and nothing after it. Where the carried casualty is taken is in no finding
			// (UNSURE): ours is 106's bed (the shared bed resolver), else the squad's home town, where it is put down. The routine stays
			// while the body is in the arms (a carrying member is never free), and a casualty that is up again or gone ends it.
			state::Fighter* body = combat_->find(r.chainTarget);
			const std::uint16_t rescueGoal = r.goal;
			const bool lifted = f.carrying == r.chainTarget;
			if (body == nullptr || body->dead || (body->carriedBy != 0 && body->carriedBy != id) || f.dead || f.unconscious || f.down
				|| (!lifted && !body->unconscious && !body->down))
			{
				if (lifted)
					dropCarried(id);
				endTask();
				continue;
			}
			if (!lifted)
			{
				if (actor.has<state::InteractTask>())
					continue; // the lift's timer runs (interact.cpp)
				if (!f.walking() && std::hypot(f.x - body->x, f.z - body->z) <= kBodyReach)
				{
					beginInteract(id, r.chainTarget, static_cast<std::uint8_t>(InteractAction::Carry), 0, {}, true);
					std::ostringstream line;
					line << "rescue " << r.goal << " member " << id << " lifts " << r.chainTarget;
					logTask(line.str());
					mirrorFighter(id);
					continue;
				}
				if (inSlice(now, id))
					walkTo(id, *body);
				continue;
			}
			if (actor.has<state::InteractTask>())
				continue; // the lift is still running (it sets the carry before its timer ends)
			// The lift is done: the carried casualty becomes 106 at the bed the shared resolver names, as 254 becomes 112 (the
			// bed requirement is the plan's, not the resolver's: dt24_142 section 3). No bed in reach falls back to the home town.
			routines::Site bed;
			if (bedFor106(f, bed))
			{
				const std::uint64_t casualty = r.chainTarget;
				r.goal = routines::task::FindBedAndPutIn;
				r.target = {static_cast<std::uint8_t>(bed.kind), bed.object, bed.slot};
				r.plan.clear();
				r.step = 0;
				r.chainElapsed = -1.0;
				const std::uint16_t was = rescueGoal;
				actor.set<state::Routine>(std::move(r));
				std::ostringstream line;
				line << "rescue " << was << " member " << id << " takes " << casualty << " to bed " << bed.object << ":" << bed.slot;
				logTask(line.str());
				mirrorFighter(id);
				continue;
			}
			const state::SquadInstance* instance = squad.get<state::SquadInstance>();
			towncircle::Circle town;
			if (instance == nullptr || !travelDestination(ecs, *gameData_, *instance, f, town, false)
				|| std::hypot(f.x - town.x, f.z - town.z) <= town.radius)
			{
				// Home (or no home to take it to): the casualty is put down here, never a held terminal state.
				const std::uint64_t casualty = r.chainTarget;
				dropCarried(id);
				worldTaskBlocks_[{id, casualty}] = now + kBlockTicks; // delivered: it is not lifted again at once (the 20 s set)
				std::ostringstream line;
				line << "rescue " << r.goal << " member " << id << " put " << casualty << " down";
				logTask(line.str());
				endTask();
				continue;
			}
			if (inSlice(now, id) && !f.walking())
				walkToPoint(id, town.x, town.z);
			continue;
		}
		if (r.goal == routines::task::FogmanWorshipVictim)
		{
			// 237 FOGMAN_WORSHIP_VICTIM -> 236 WorshipTarget (dt24_237; dt29_fog_kidnap section 6): the crowd walks to the cage and stays
			// while the victim carries the mark. 236 itself is undecoded: no clip, and the hold length is the mark's (UNSURE).
			const state::Fighter* victim = combat_->find(r.chainTarget);
			const auto marked = eatenUntil_.find(r.chainTarget);
			if (victim == nullptr || victim->dead || marked == eatenUntil_.end() || marked->second <= now || f.dead || f.unconscious
				|| f.down || f.carriedBy != 0 || f.carrying != 0)
			{
				endTask();
				continue;
			}
			if (std::hypot(f.x - victim->x, f.z - victim->z) > kInteractReach)
			{
				if (inSlice(now, id))
					walkToPoint(id, victim->x, victim->z);
				continue;
			}
			if (f.walking())
			{
				f.clearRoute();
				f.forwardSpeed = 0.0;
				f.lateralSpeed = 0.0;
			}
			f.yaw = std::atan2(victim->x - f.x, victim->z - f.z);
			if (r.chainElapsed <= 0.0)
			{
				r.chainElapsed = kTickSeconds;
				const std::uint64_t victimId = r.chainTarget;
				actor.set<state::Routine>(std::move(r));
				std::ostringstream line;
				line << "worship 237 member " << id << " at victim " << victimId;
				logTask(line.str());
			}
			mirrorFighter(id);
			continue;
		}
		if (r.goal == routines::task::HuntingBloodsmell)
		{
			// 176 HUNTING_BLOODSMELL (dt27 section 4.2): every tick Task_HuntingSmell reads the platoon Blackboard's smell and moves
			// straight at its position (move mode 2, no arrival test). The exe idles when the board holds none and the planner
			// re-plans; ours ends the task then, so the member re-scores (never a held idle).
			const blackboard::Board* board = blackboardOf(squad);
			if (board == nullptr || !board->hasSmell || f.dead || f.unconscious || f.down || f.carriedBy != 0 || f.carrying != 0)
			{
				endTask();
				continue;
			}
			if (inSlice(now, id) && !f.walking())
				walkToPoint(id, board->smellX, board->smellZ);
			continue;
		}
		if (r.goal == routines::task::RetreatHometown || r.goal == routines::task::RunAwayHometown)
		{
			// 128 / 127 Task_GoHomeTown / GoHomeTownFast (dt32): the same body; 127's vf5 is 2 (run), 128's 0 (walk). The destination is
			// stored when the goal is picked (Routine::shoppingX / shoppingZ, reused as the walk point: no new save field), there is no deadline and no package hand to clear, and the task
			// finishes on arrival - at the point, or inside the point's radius of the town (UNSURE: the arrival radius is the mover's).
			const state::SquadInstance* instance = squad.get<state::SquadInstance>();
			if (instance == nullptr || f.dead || f.unconscious || f.down || f.carriedBy != 0 || f.carrying != 0)
			{
				endTask();
				continue;
			}
			// No arrival test (dt33): the walk ends when the mover stops or gives up (no route), or the goal window closes.
			// chainElapsed is the "walk issued" latch (negative until it is).
			if (r.chainElapsed < 0.0)
			{
				f.moveMode = r.goal == routines::task::RunAwayHometown ? 2 : 0;
				const bool walking = walkToPoint(id, r.shoppingX, r.shoppingZ); // the destination stored at the pick
				std::ostringstream line;
				line << "home " << r.goal << " member " << id << " walks to " << r.shoppingX << " " << r.shoppingZ << " (speed mode "
					<< static_cast<int>(f.moveMode) << ")";
				logTask(line.str());
				r.chainElapsed = 0.0;
				actor.set<state::Routine>(r); // the latch survives a commit lock that refuses the end
				if (!walking)
					endTask(); // no route (or already there): the task is impossible, never a wait for ever
				continue;
			}
			if (!f.walking())
				endTask();
			continue;
		}
		if (r.goal == routines::task::TravelToTargetTown || r.goal == routines::task::TravelToTargetTownFast
			|| r.goal == routines::task::GoToSomewhereForDeliveringSlaves)
		{
			// 53 / 130 Task_TravelToTargetTown (dt27 section 5.2): the walk to the package's town; arrived, the task finishes
			// (flagImpossible in the exe, which also clears the package's target hand - ours has a fixed home town, nothing to clear).
			const state::SquadInstance* instance = squad.get<state::SquadInstance>();
			towncircle::Circle town;
			if (instance == nullptr || f.dead || f.unconscious || f.down || f.carriedBy != 0 || f.carrying != 0
				|| !travelDestination(ecs, *gameData_, *instance, f, town)
				|| std::hypot(f.x - town.x, f.z - town.z) <= town.radius)
			{
				endTask();
				continue;
			}
			if (inSlice(now, id) && !f.walking())
				walkToPoint(id, town.x, town.z);
			continue;
		}
		if (r.goal == routines::task::MakeAnnouncement || r.goal == routines::task::MakeAnnouncementFast)
		{
			// 103 / 129 Task_MakeAnnouncement (dt24_smalltasks section 5; the dialogue event is DIALOGUE_SELECTION_STATIC 0x02 ANNOUNCEMENT, speaker
			// the announcer, no subject): the line is said once at the package target. Ours fires the event on the first tick and the
			// task ends; the exe holds the task while the line is speaking and posts the "{1} has arrived at {2}" notification first
			// (neither modelled, UNSURE: the speech has no duration here, the notification has no sink).
			if (!f.dead && !f.unconscious && !f.down && f.carriedBy == 0)
			{
				fireDialogue(id, 0, 0x02);
				announced_.insert(id);
				std::ostringstream line;
				line << "goal " << r.goal << " announcement member " << id;
				logTask(line.str());
			}
			endTask();
			continue;
		}
		if (r.goal == routines::task::TravelToTargetPackage)
		{
			// 215 Task_TravelToMissionTarget_ArmyFormation (dt24_215): the tick walks to the package's target point (ours the squad's home
			// town centre, as 53's, without the nest diversion) and the task is finished on arrival (the exe: my formation slot has become
			// me, or the tick's flag is set). The formation slot (speed mode 1 when far or without a formation object, the in-place stop)
			// is not modelled - our squads have no formation object (UNSURE). An unreachable target ends the goal (the core rule: a
			// member never waits for ever); the selector may offer it again, the walk is then tried afresh.
			const state::SquadInstance* instance = squad.get<state::SquadInstance>();
			towncircle::Circle target;
			if (instance == nullptr || f.dead || f.unconscious || f.down || f.carriedBy != 0 || f.carrying != 0
				|| !travelDestination(ecs, *gameData_, *instance, f, target, false)
				|| std::hypot(f.x - target.x, f.z - target.z) <= target.radius)
			{
				endTask();
				continue;
			}
			if (inSlice(now, id) && !f.walking() && !walkToPoint(id, target.x, target.z))
				endTask();
			continue;
		}
		if (r.goal == routines::task::NotBeUnarmed)
		{
			// 42 DON'T BE UNARMED (dt25 section 2): the rearm loop, 8 FIND_WEAPON -> 6 EQUIP_WEAPON. Step 0 takes the weapon from
			// its source (a loose item on the ground, else a body's hands), step 1 puts it in the member's hands. The goal is met
			// as soon as hasWeapon holds again - a weapon in the hands, the back or the pack - and an animal never searches (dt25
			// section 2.2).
			// Armed-ness is the selector's test, not this loop's: the pick-up itself makes hasWeapon true (the weapon lands in the
			// pack), and a plan once chosen runs to its last step.
			if (f.dead || f.unconscious || f.down || f.carriedBy != 0 || f.carrying != 0)
			{
				actor.remove<state::Routine>();
				mirrorFighter(id);
				continue;
			}
			if (r.step == 0)
			{
				if (!weaponSidOf(*gameData_, actor.get<state::Inventory>(), false).empty())
				{
					r.step = 1; // the weapon is in the pack: 6 EQUIP_WEAPON is next
					actor.set<state::Routine>(std::move(r));
					continue;
				}
				// The chain target is a state::Id, not a flecs entity id: find its entity (a loose item is the only such source).
				flecs::entity source;
				world_.each([&](flecs::entity e, const state::Id& key)
				{
					if (key.value == r.chainTarget)
						source = e;
				});
				if (const state::GroundItem* item = source.get<state::GroundItem>())
				{
					if (actor.has<state::InteractTask>())
						continue; // the pick-up's own walk and reach test run it
					const double d = std::hypot(f.x - item->useX, f.z - item->useZ);
					if (d <= kReachGroundItem && std::fabs(f.y - item->useY) <= kInteractReachY)
					{
						beginInteract(id, r.chainTarget, static_cast<std::uint8_t>(InteractAction::PickUp), 0, {}, true);
						continue;
					}
					if (inSlice(now, id))
						walkToPoint(id, item->useX, item->useZ);
					continue;
				}
				state::Fighter* body = combat_->find(r.chainTarget);
				if (body == nullptr || (!body->dead && !body->down && !body->unconscious) || body->carriedBy != 0)
				{
					actor.remove<state::Routine>(); // gone (another took it, it woke and walked): the next second re-plans
					mirrorFighter(id);
					continue;
				}
				if (!f.walking() && std::hypot(f.x - body->x, f.z - body->z) <= kInteractReach
					&& std::fabs(f.y - body->y) <= kInteractReachY)
				{
					// At the body: its weapon comes off into the ground at its feet (the generic strip - the same one task BP
					// uses), and the loop's pass 1 picks it up. A body whose weapon cannot be taken leaves the goal.
					stripTargetWeapons(r.chainTarget);
					const WeaponSource nearest = findWeapon42(ecs, *combat_, *gameData_, id, f);
					r.chainTarget = nearest.item;
					if (nearest.item != 0)
						actor.set<state::Routine>(std::move(r));
					else
						actor.remove<state::Routine>();
					mirrorFighter(id);
					continue;
				}
				if (inSlice(now, id))
					walkTo(id, *body);
				continue;
			}
			// Step 1: 6 EQUIP_WEAPON - the exe's Task_EquipBestWeapon inventory pass (dt25 section 2.5; ours: order 67 by record).
			const std::string weapon = drawWeapon(id);
			{
				std::ostringstream line;
				line << "rearm 6 equip member " << id << " weapon '" << weapon << "'";
				logTask(line.str());
			}
			actor.remove<state::Routine>();
			mirrorFighter(id);
			continue;
		}
		if (r.goal == routines::task::CarryWoundedSlaves)
		{
			// LIFT_OBJECT_BUT_HEAL_FIRST 170 (dt15 section 3): at the slave, lift it (the carry chain resolves the lift).
			state::Fighter* body = combat_->find(r.chainTarget);
			if (body == nullptr || body->dead || body->carriedBy != 0 || f.dead || f.unconscious || f.down || f.carrying == r.chainTarget)
			{
				actor.remove<state::Routine>();
				mirrorFighter(id);
				continue;
			}
			if (!f.walking() && std::hypot(f.x - body->x, f.z - body->z) <= kBodyReach)
			{
				beginInteract(id, r.chainTarget, static_cast<std::uint8_t>(InteractAction::Carry), 0, {}, true);
				actor.remove<state::Routine>();
				mirrorFighter(id);
				continue;
			}
			if (inSlice(now, id))
				walkTo(id, *body);
			continue;
		}
		if (r.goal == routines::task::SlaveWatching || r.goal == routines::task::NewSlaveProcessing)
		{
			// PROCESS_AND_STRIP_NEW_SLAVE 182 (dt15 sections 1-2, the effect only): the watcher walks to the body, then 3 s of
			// processing that strips its weapons and leaves it a slave (state 1, its captor my faction).
			state::Fighter* body = combat_->find(r.chainTarget);
			if (body == nullptr || body->dead || body->carriedBy != 0 || f.dead || f.unconscious || f.down)
			{
				actor.remove<state::Routine>();
				mirrorFighter(id);
				continue;
			}
			if (r.chainElapsed < 0.0)
			{
				if (!f.walking() && std::hypot(f.x - body->x, f.z - body->z) <= kBodyReach)
				{
					f.clearRoute();
					f.forwardSpeed = 0.0;
					f.lateralSpeed = 0.0;
					f.yaw = std::atan2(body->x - f.x, body->z - f.z);
					r.chainElapsed = 0.0;
					actor.set<state::Routine>(std::move(r));
					mirrorFighter(id);
				} else if (inSlice(now, id)) {
					// Watchers on one body (a caged slave the mesh keeps them from) stand side by side, not on one point (kenshi-k16.6.442):
					// the first by Id walks to the body, the others to a spot along the nearest reachable edge.
					int rank = 0;
					for (std::size_t other = 0; other < members.size(); ++other)
					{
						const state::Routine* mate = members[other].id < id ? ecs.entity(combatEntities_[other]).get<state::Routine>() : nullptr;
						rank += mate != nullptr && mate->goal == r.goal && mate->chainTarget == r.chainTarget;
					}
					const Route route = rank > 0 && navigation_ != nullptr ? routeRetry(id, f, body->x, body->z, body->y, true) : Route{};
					if (route.found && !route.points.empty())
					{
						const double endX = route.points.back().x;
						const double endZ = route.points.back().z;
						const double len = std::hypot(body->x - endX, body->z - endZ);
						const double along = ((rank + 1) / 2) * kWatchSpread * (rank % 2 != 0 ? 1.0 : -1.0);
						f.clearRoute();
						walkToPoint(id, endX + (len > 1.0e-3 ? -(body->z - endZ) / len : 1.0) * along, endZ + (len > 1.0e-3 ? (body->x - endX) / len : 0.0) * along);
					} else {
						walkTo(id, *body);
					}
				}
				continue;
			}
			f.clearRoute();
			f.forwardSpeed = 0.0;
			f.lateralSpeed = 0.0;
			r.chainElapsed += dt;
			if (r.chainElapsed < kProcessSeconds)
			{
				actor.set<state::Routine>(std::move(r));
				continue;
			}
			stripTargetWeapons(r.chainTarget);
			setSlaveState(r.chainTarget, 1, id);
			if (body->captorSid.empty())
			{
				if (const state::Allegiance* mine = actor.get<state::Allegiance>())
					body->captorSid = mine->factionSid;
			}
			noteOwnedSlave(id, r.chainTarget);
			mirrorFighter(r.chainTarget);
			{
				std::ostringstream line;
				line << "process 182 member " << id << " body " << r.chainTarget << " (stripped, slave of '" << body->captorSid << "')";
				logTask(line.str());
			}
			actor.remove<state::Routine>();
			mirrorFighter(id);
			continue;
		}
		if (r.goal == routines::task::StandUp)
		{
			// 28 STAND_UP (Task_UnCrouch, dt24_plantasks section 2): the goal is isCrouching = 0. Ours clears the crouched stance (the only
			// pose flag the sim has besides asleep / down, which are other tasks' to end) and the task is done.
			if (f.crouched)
			{
				f.crouched = false;
				mirrorFighter(id);
				std::ostringstream line;
				line << "stand 28 member " << id;
				logTask(line.str());
			}
			endTask();
			continue;
		}
		if (r.goal == routines::task::SeekAndTalk || r.goal == routines::task::SendDialogue || r.goal == routines::task::FollowWhileTalking
			|| r.goal == routines::task::TownStalker)
		{
			// 102 / 159 Task_TalktoPlayer, 164 Task_FollowAndTalk, 165 Task_TownStalker's out-of-town branch (item28b section 1.3; dt24_stalk_turret
			// sections 5-6): walk to the target the plan's hand names, then send the dialogue event once (102 / 159 / 164: dialog_event, the
			// default 1; 165: line 0x1a). 102 / 159 / 164 end with the line; 165 keeps following until the target is gone or kStalkSeconds
			// run out (the exe ends it when both speak, which has no duration here - UNSURE). 164's stand-off is 17 u indoors (task+0x90).
			// The in-town branch of 165 (a townsperson with loot, a stand-off point) is M in the findings and not ported.
			const state::Fighter* target = combat_->find(r.chainTarget);
			const bool stalker = r.goal == routines::task::TownStalker;
			if (target == nullptr || target->dead || target->carriedBy != 0 || f.dead || f.unconscious || f.down || f.carriedBy != 0
				|| static_cast<double>(ticksSince(now, r.since)) * kTickSeconds > (stalker ? kStalkSeconds : kGiveUpSeconds))
			{
				if (!stalker)
					worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
				endTask();
				continue;
			}
			const double standOff = r.goal == routines::task::FollowWhileTalking && f.building != 0 ? 17.0 : kInteractReach;
			const bool near = std::hypot(f.x - target->x, f.z - target->z) <= standOff;
			if (!near)
			{
				if (inSlice(now, id))
					walkTo(id, *target);
				continue;
			}
			if (f.walking())
			{
				f.clearRoute();
				f.forwardSpeed = 0.0;
				f.lateralSpeed = 0.0;
			}
			if (r.chainElapsed <= 0.0)
			{
				fireDialogue(id, r.chainTarget, stalker ? 0x1a : 1);
				r.chainElapsed = kTickSeconds;
				std::ostringstream line;
				line << "talk " << r.goal << " member " << id << " to " << r.chainTarget;
				logTask(line.str());
				if (stalker)
				{
					actor.set<state::Routine>(std::move(r));
					continue;
				}
			}
			if (!stalker)
				endTask();
			continue;
		}
		if (r.goal == routines::task::WorkTheSlaves)
		{
			// 188 WORK_THE_SLAVES (dt24_131 section 4): walk to the owned character the resolver drew, then the work dialogue (goal
			// isDialogDelivered, plan 161 as 218's). The dialogue's line and its effect on the slave (+0x130 = 0x26) are not decoded:
			// the delivery is the member standing at the slave and saying it (a log line) - UNSURE, no state change. The same slave is
			// not drawn again for 20 s (worldTaskBlocks_), so a squad does not loop on one.
			const state::Fighter* slave = combat_->find(r.chainTarget);
			if (slave == nullptr || slave->dead || slave->carriedBy != 0 || f.dead || f.unconscious || f.down || f.carriedBy != 0)
			{
				endTask();
				continue;
			}
			if (std::hypot(f.x - slave->x, f.z - slave->z) > kInteractReach)
			{
				if (static_cast<double>(ticksSince(now, r.since)) * kTickSeconds > kGiveUpSeconds)
				{
					worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
					endTask();
				} else if (inSlice(now, id)) {
					walkTo(id, *slave);
				}
				continue;
			}
			{
				std::ostringstream line;
				line << "work 188 member " << id << " speaks to " << r.chainTarget;
				logTask(line.str());
			}
			worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
			endTask();
			continue;
		}
		if (r.goal == routines::task::GiveAllMySlavesTo || r.goal == routines::task::GiveAllMySlavesToImport)
		{
			// 194 / 218 GIVE_ALL_MY_SLAVES_TO (dt27 section 6.3): walk to the slave-storage keeper the resolver picked, then the plan's
			// SEND_DIALOGUE_SIGNAL (159 / 161) delivers and iHaveSomeSlaves becomes 0. The dialogue's line is not decoded (the goal
			// state isDialogDelivered), so the delivery is the hand-over itself (OURS, UNSURE): every slave following me now follows
			// the keeper.
			const state::Fighter* keeper = combat_->find(r.chainTarget);
			bool anySlave = false;
			for (const CombatArena::Member& other : members)
				anySlave = anySlave || (other.fighter.followTarget == id && other.fighter.slaveState == 1 && !other.fighter.dead);
			if (keeper == nullptr || keeper->dead || keeper->unconscious || keeper->down || f.dead || f.unconscious || f.down
				|| f.carriedBy != 0 || !anySlave)
			{
				endTask();
				continue;
			}
			if (std::hypot(f.x - keeper->x, f.z - keeper->z) > kInteractReach)
			{
				if (inSlice(now, id))
					walkTo(id, *keeper);
				continue;
			}
			std::size_t given = 0;
			for (std::size_t other = 0; other < members.size(); ++other)
			{
				state::Fighter* slave = combat_->find(members[other].id);
				if (slave == nullptr || slave->followTarget != id || slave->slaveState != 1 || slave->dead)
					continue;
				slave->followTarget = r.chainTarget;
				slave->followAge = 0.0;
				slave->clearRoute();
				mirrorFighter(members[other].id);
				++given;
			}
			{
				std::ostringstream line;
				line << "deliver 218 member " << id << " gave " << given << " slaves to " << r.chainTarget;
				logTask(line.str());
			}
			endTask();
			continue;
		}
		if (r.goal == routines::task::GatherSlavesFromCages || r.goal == routines::task::GatherSlavesFromCagesExport
			|| r.goal == routines::task::GatherSlavesFemaleOrBeast)
		{
			// GET_SLAVE 174 (dt15 section 5): at the cage, take my slave out; it follows its taker. The reach is the interact reach (12 u):
			// the separation push keeps two bodies about 8 u apart, so a 4 u test is met only by a walk's last step (task dt27, shared by 217).
			state::Fighter* body = combat_->find(r.chainTarget);
			if (body == nullptr || body->dead || body->cageBuilding == 0 || f.dead || f.unconscious || f.down)
			{
				actor.remove<state::Routine>();
				mirrorFighter(id);
				continue;
			}
			if (!f.walking() && std::hypot(f.x - body->x, f.z - body->z) <= kInteractReach)
			{
				body->cageBuilding = 0;
				body->cageSlot = 0;
				body->followTarget = id;
				body->followAge = 0.0;
				body->followDirect = true;
				body->clearRoute();
				mirrorFighter(r.chainTarget);
				actor.remove<state::Routine>();
				mirrorFighter(id);
				continue;
			}
			if (inSlice(now, id))
				walkTo(id, *body);
			continue;
		}
		if (r.goal == routines::task::LiberatePrisoners)
		{
			// LIBERATE_ALL_THE_PRISONERS 276 (dt24_276, rwh.100): walk to the cage's captive, then the plan's only action that ends in
			// TARGET_IS_IMPRISONED = 0: 110 RELEASE_PRISONER (stepLockTask: the captive must be conscious, the cage's lock is cleared, the prisoner
			// leaves through 133, event 0x2e, the bounties clear). No lock test is in the chain: a locked cage is freed (rwh.100). Ours also removes
			// the slave state afterwards (removeSlaveStatus: no slave state, no captor, orders cleared); the oracle's 110 does not (UNSURE, kept for
			// the slaves a liberation is for). Crime for the liberation is not modelled (the decode has no crime test either).
			state::Fighter* body = combat_->find(r.chainTarget);
			if (body == nullptr || body->dead || body->unconscious || body->cageBuilding == 0 || f.dead || f.unconscious || f.down)
			{
				endTask();
				continue;
			}
			if (!f.walking() && std::hypot(f.x - body->x, f.z - body->z) <= kBodyReach)
			{
				const std::uint64_t cage = body->cageBuilding;
				if (stepLockTask(id, routines::task::ReleasePrisoner, r.chainTarget) == LockStep::Done)
				{
					removeSlaveStatus(r.chainTarget);
					std::ostringstream line;
					line << "liberate 276 member " << id << " cage " << body->cageBuilding << " occupant " << r.chainTarget << " (from " << cage << ")";
					logTask(line.str());
				}
				endTask();
				continue;
			}
			if (inSlice(now, id))
				walkTo(id, *body);
			continue;
		}
		if (r.goal == routines::task::AssaultFortifications)
		{
			// ASSAULT_FORTIFICATIONS_PREFER_GATES 270 (dt24_270): the gate the shared assault resolver picked (Routine::chainTarget,
			// the door key). Walk to it, then the measured gate bash: a gate that refuses my class is beaten down for DoorSystem's
			// seconds (one basher, unarmed: DOORS.md section 6, the raid model) and broken; a gate nothing refuses is already
			// passable - the goal's effect DESTINATION_NOT_BLOCKED_BY_FORTIFICATIONS holds and the action ends. The wall-smash half
			// is not modelled (UNSURE).
			const GateSite* gate = nullptr;
			for (const GateSite& site : world_.gateSites())
				if (site.key == r.chainTarget)
				{
					gate = &site;
					break;
				}
			DoorSystem* doors = ai_ != nullptr ? ai_->doorSystem() : nullptr;
			if (gate == nullptr || doors == nullptr || f.dead || f.unconscious || f.down || f.carriedBy != 0)
			{
				endTask();
				continue;
			}
			const GateBash bash = doors->bashFor(f.faction, gate->key);
			if (bash.key == 0)
			{
				endTask(); // nothing refuses: the fortification no longer blocks the way (the goal's effect holds)
				continue;
			}
			// The gate's OUTSIDE point: a locked door is a wall to the mesh route, so a walk to the slab's centre ends on the nearest
			// reachable ground (a few units short, out of the bash's reach) and the member would try again for ever. The outside point
			// is where the raid stops (the measured bash at the gate, ai.cpp).
			const double reachX = gate->outX;
			const double reachZ = gate->outZ;
			const double dx = reachX - f.x;
			const double dz = reachZ - f.z;
			if (f.walking())
			{
				continue;
			}
			if (std::hypot(dx, dz) > kGateReach)
			{
				if (inSlice(now, id))
					walkToPoint(id, reachX, reachZ);
				mirrorFighter(id);
				continue;
			}
			// At the gate, bashing: stand, face it, count the bash's seconds on the routine's timer (negative = not started yet).
			f.clearRoute();
			f.forwardSpeed = 0.0;
			f.lateralSpeed = 0.0;
			f.yaw = std::atan2(gate->x - f.x, gate->z - f.z); // face the slab, not the outside point
			if (r.chainElapsed < 0.0)
				r.chainElapsed = 0.0;
			r.chainElapsed += dt;
			if (r.chainElapsed < bash.seconds)
			{
				actor.set<state::Routine>(std::move(r));
				mirrorFighter(id);
				continue;
			}
			doors->breakGate(ecs, bash.key);
			{
				std::ostringstream line;
				line << "assault 270 member " << id << " broke gate " << bash.key << " after " << bash.seconds << " s";
				logTask(line.str());
			}
			endTask();
			continue;
		}
		if (r.goal == routines::task::GetOutOfCageEscape || r.goal == routines::task::GetOutOfCageOpportunistic)
		{
			// 207 / 256 Task_GetOutOfCage (dt24_207 section 2): the shared action 1403554b0 - the occupant slot of the cage is released
			// and a chained slave (slave state 1) becomes ESCAPING (2). Nothing opens a door or fights in the class itself: it is a state
			// change, so the goal ends at once when the door is open. A locked door is the plan's: the steps in front of the goal
			// (7 UNEQUIP_WEAPON, then 76 PICK_LOCK or 285 CUT_LOCK, task ds/shackles-locks) run first, one per tick. A member no longer caged
			// (it was freed or carried) ends too, and is offered again later - an escapee never ends stuck. The stand-up animation and
			// CharStatus+0xe8 are not modelled (UNSURE); +0x80 "the jail is a cage or the player's" is always true here.
			if (f.dead || f.unconscious || f.down || f.carriedBy != 0 || f.cageBuilding == 0)
			{
				endTask();
				continue;
			}
			if (cageDoorLocked(f.cageBuilding))
			{
				const std::uint16_t step = r.step < r.plan.size() ? r.plan[r.step] : 0;
				if (step == routines::task::UnequipWeapon)
				{
					sheatheWeapon(id);
					++r.step;
					actor.set<state::Routine>(std::move(r));
				} else if (!routines::task::isLockTask(step)) {
					endTask(); // the lock closed again with no step to open it: the next pass plans afresh
				} else {
					const LockStep result = stepLockTask(id, step, 0);
					if (result == LockStep::Refused || result == LockStep::TooHard)
						endTask();
					else if (result == LockStep::Done)
					{
						++r.step;
						actor.set<state::Routine>(std::move(r));
					}
				}
				continue;
			}
			// The escape crime (Task_GetOutOfCage start with the escape flag, 1403554b0 L38; CRIME_NOTICE C72): ESCAPE_PRISON against the cage's
			// owner faction (the captor's when the cage records none), written once the occupant is out of the cage (a cage occupant is refused).
			const routines::BuildingSites* cageOf = residentData_ != nullptr ? residentData_->sites.find(f.cageBuilding) : nullptr;
			const std::string cageOwner = cageOf != nullptr && !cageOf->ownerFaction.empty() ? cageOf->ownerFaction : f.captorSid;
			leaveCage(id, r.goal, true);
			reportCrime(id, crime::kEscapingPrison, cageOwner, 0);
			endTask();
			continue;
		}
		if (r.goal == routines::task::SlaveGetInCage)
		{
			// 172 SLAVE_GET_IN_MY_MASTERS_CAGE (dt24_172 section 1): the cage of my captor's faction that the resolver picked, walked
			// to (67 Running) and used (107 USE_CAGE). The slave puts itself in at the cage's own spot, as the put-in sets a carried
			// body (108, carry.cpp): the position, the building and the slot, and no Routine while it is in there.
			const routines::Site site{static_cast<routines::SiteKind>(r.target.kind), r.target.object, r.target.slot};
			const routines::SiteEntry* cage = residentData_ != nullptr ? residentData_->sites.siteAt(site) : nullptr;
			if (cage == nullptr || f.dead || f.unconscious || f.down || f.carriedBy != 0 || f.cageBuilding != 0)
			{
				actor.remove<state::Routine>();
				mirrorFighter(id);
				continue;
			}
			const double reach = gameData_ != nullptr && !cage->record.empty()
				? functionalityReach(*gameData_, cage->record, routines::kFunctionCage, !f.walking()) : kBodyReach;
			if (!f.walking() && std::hypot(f.x - cage->x, f.z - cage->z) > reach)
			{
				if (inSlice(now, id))
					walkToPoint(id, cage->x, cage->z);
				else
					mirrorFighter(id);
				continue;
			}
			if (f.walking())
				continue;
			f.x = cage->occupantX();
			if (cage->hasY)
				f.y = cage->y;
			f.z = cage->occupantZ();
			f.velX = 0.0;
			f.velZ = 0.0;
			f.building = cage->site.object;
			f.cageBuilding = cage->site.object;
			lockErase(false, cage->site.object); // the put-in locks the cage again (item28 b1)
			f.cageSlot = cage->site.slot;
			f.clearRoute();
			f.drive = state::DriveMode::Stop;
			f.driveStrafe = 0;
			f.forwardSpeed = 0.0;
			f.lateralSpeed = 0.0;
			{
				std::ostringstream line;
				line << "cage 172 member " << id << " got in cage " << cage->site.object << ":" << cage->site.slot
					 << " (captor faction " << f.captorFaction << ")";
				logTask(line.str());
			}
			actor.remove<state::Routine>();
			mirrorFighter(id);
			continue;
		}
		if (r.goal == routines::task::FinalKidnapperCageJob)
		{
			// 254 FINAL_KIDNAPPER_CAGE_JOB (dt26 sections 2-4): the plan is [67 Running, 68 LIFT_PERSON, 132 FIND_AND_KIDNAP, 254];
			// 132 has no factory case and 254 is a finder-less marker, so the lift is what this goal runs first. The routine STAYS while
			// the body is in the member's arms: a carrying member is never free (freeMember), so leaving the cage to a re-selection
			// would strand the victim. Once the lift is in, the same routine becomes the caging goal 112 at a cage of the member's town
			// - the exe's second expansion of 254 (154 -> 112 -> 108) - and carry.cpp's chain (67 at the cage, 108) puts the body in.
			state::Fighter* body = combat_->find(r.chainTarget);
			if (body == nullptr || (body->dead && f.carrying != r.chainTarget) || (body->carriedBy != 0 && body->carriedBy != id) || f.dead
				|| f.unconscious || f.down)
			{
				actor.remove<state::Routine>();
				mirrorFighter(id);
				continue;
			}
			if (f.carrying != r.chainTarget)
			{
				if (actor.has<state::InteractTask>())
					continue; // the lift's timer runs (interact.cpp)
				if (!f.walking() && std::hypot(f.x - body->x, f.z - body->z) <= kBodyReach)
				{
					beginInteract(id, r.chainTarget, static_cast<std::uint8_t>(InteractAction::Carry), 0, {}, true);
					{
						std::ostringstream line;
						line << "kidnap 254 member " << id << " lifts victim " << r.chainTarget;
						logTask(line.str());
					}
					mirrorFighter(id);
					continue;
				}
				if (inSlice(now, id))
					walkTo(id, *body);
				continue;
			}
			routines::Site site;
			if (!worldres::findCage112(f, cagesNear(f), site))
			{
				// No cage of my faction or home near: the victim is let go (OURS; the exe's 112 keeps the goal waiting for a cage).
				// Never a terminal state: the routine ends and the member re-scores.
				dropCarried(id);
				{
					std::ostringstream line;
					line << "kidnap 254 member " << id << " no cage for victim " << r.chainTarget << " (let go)";
					logTask(line.str());
				}
				actor.remove<state::Routine>();
				mirrorFighter(id);
				continue;
			}
			{
				std::ostringstream line;
				line << "kidnap 254 member " << id << " carries victim " << r.chainTarget << " to cage " << site.object << ":"
					 << site.slot;
				logTask(line.str());
			}
			r.goal = body->dead ? routines::task::FindCageAndPutDeadGuyIn : routines::task::FindCageAndPutIn; // a man who died in the arms: 281
			r.target = {static_cast<std::uint8_t>(site.kind), site.object, site.slot};
			r.plan.clear();
			r.step = 0;
			r.chainElapsed = -1.0;
			actor.set<state::Routine>(std::move(r));
			mirrorFighter(id);
			continue;
		}
		if (r.goal == routines::task::FindCorpses)
		{
			// 138 FIND_A_DEAD_GUY (dt30): [67 Running, 68 LIFT_PERSON, 138]; the row chains to nothing (+0xc / +0x10 = 0), so what the
			// carrier does next comes from the package's own rows. The lift is 254's: walk to the body, the carry order
			// (beginInteract), done when the body is in the arms. The goal then ends and the selector's next pass places the body: 281
			// when the package has it (a cage of the town), else the carrier puts it down (a world squad runs no 137 pot - the town
			// scorer's - so that hand-off is the resident's, inside 137's own chain; UNSURE for a world squad).
			state::Fighter* body = combat_->find(r.chainTarget);
			if (f.carrying == r.chainTarget)
			{
				{
					std::ostringstream line;
					line << "corpse 138 member " << id << " lifted body " << r.chainTarget;
					logTask(line.str());
				}
				if (body != nullptr && body->cageBuilding != 0)
				{
					body->cageBuilding = 0; // a body taken out of a cage frees its slot (the gather goal 173's rule)
					body->cageSlot = 0;
					mirrorFighter(r.chainTarget);
				}
				endTask();
				continue;
			}
			if (body == nullptr || !body->dead || body->carriedBy != 0 || f.dead || f.unconscious || f.down)
			{
				endTask();
				continue;
			}
			if (actor.has<state::InteractTask>())
				continue; // the lift's timer runs (interact.cpp)
			if (!f.walking() && std::hypot(f.x - body->x, f.z - body->z) <= kBodyReach)
			{
				beginInteract(id, r.chainTarget, static_cast<std::uint8_t>(InteractAction::Carry), 0, {}, true);
				{
					std::ostringstream line;
					line << "corpse 138 member " << id << " lifts body " << r.chainTarget;
					logTask(line.str());
				}
				mirrorFighter(id);
				continue;
			}
			if (static_cast<double>(ticksSince(now, r.since)) * kTickSeconds > kGiveUpSeconds)
			{
				worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
				endTask();
			} else if (inSlice(now, id)) {
				walkTo(id, *body);
			}
			continue;
		}
		if (r.goal == routines::task::LootStorable || r.goal == routines::task::LootAnimalsJob)
		{
			// Task_Loot_ResourcesWeHaveStorageFor (dt24_cage_loot_play section 3): at the body, 3 s of the "medic" loot, then every unit of
			// the FIRST storable resource type in its pack moves to mine (no other type), and the goal ends.
			state::Fighter* body = combat_->find(r.chainTarget);
			if (body == nullptr || body->carriedBy != 0 || f.dead || f.unconscious || f.down)
			{
				endTask();
				continue;
			}
			if (r.chainElapsed < 0.0)
			{
				if (!f.walking() && std::hypot(f.x - body->x, f.z - body->z) <= kBodyReach)
				{
					f.clearRoute();
					f.forwardSpeed = 0.0;
					f.lateralSpeed = 0.0;
					r.chainElapsed = 0.0;
					actor.set<state::Routine>(std::move(r));
					mirrorFighter(id);
				} else if (static_cast<double>(ticksSince(now, r.since)) * kTickSeconds > kGiveUpSeconds) {
					worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
					endTask();
				} else if (inSlice(now, id)) {
					walkTo(id, *body);
				}
				continue;
			}
			f.clearRoute();
			f.forwardSpeed = 0.0;
			f.lateralSpeed = 0.0;
			r.chainElapsed += dt;
			if (r.chainElapsed < kLootStorableSeconds)
			{
				actor.set<state::Routine>(std::move(r));
				continue;
			}
			std::string mySid;
			if (const state::Allegiance* mine = actor.get<state::Allegiance>())
				mySid = mine->factionSid;
			std::set<std::string> storable;
			world_.each([&](flecs::entity entity, const state::Container& container)
			{
				const state::Inventory* stock = entity.get<state::Inventory>();
				if (container.ownerSid != mySid || container.locked || stock == nullptr)
					return;
				for (const state::Inventory::Entry& entry : stock->items)
					if (entry.slot == state::Inventory::kCarried)
						storable.insert(entry.itemSid);
			});
			world_.each([&](const state::Producer& producer)
			{
				if (producer.ownerSid == mySid)
					storable.insert(producer.output);
			});
			flecs::entity bodyEntity = fighterEntity(r.chainTarget);
			const state::Inventory* held = bodyEntity.get<state::Inventory>();
			if (held != nullptr && gameData_ != nullptr)
			{
				const grid::Rules rules(*gameData_);
				state::Inventory source = *held;
				grid::settle(source, rules, r.chainTarget);
				state::Inventory mine;
				if (const state::Inventory* packed = actor.get<state::Inventory>())
					mine = *packed;
				std::vector<state::Inventory::Entry> overflow = grid::settle(mine, rules, id);
				std::string type;
				for (const state::Inventory::Entry& entry : source.items)
				{
					const ItemInfo* info = entry.slot == state::Inventory::kCarried && entry.count > 0 ? combat_->data().item(entry.itemSid) : nullptr;
					if (info != nullptr && info->function == 0 && storable.count(entry.itemSid) != 0)
					{
						type = entry.itemSid;
						break;
					}
				}
				std::uint32_t took = 0;
				for (auto it = source.items.begin(); !type.empty() && it != source.items.end();)
				{
					if (it->slot != state::Inventory::kCarried || it->itemSid != type)
					{
						++it;
						continue;
					}
					const std::uint32_t left = grid::autoplace(mine, rules, loose(*it));
					took += it->count - left;
					if (left > 0)
					{
						it->count = left;
						break; // no room for more
					}
					it = source.items.erase(it);
				}
				if (took > 0)
				{
					++mine.version;
					++source.version;
					storeInventory(actor, id, std::move(mine), std::move(overflow));
					bodyEntity.set<state::Inventory>(std::move(source));
					std::ostringstream line;
					line << "loot 241 member " << id << " from body " << r.chainTarget << ": " << took << " x " << type;
					logTask(line.str());
				}
			}
			r.chainElapsed = -1.0;
			endTask();
			continue;
		}
		if (r.goal == routines::task::PutLootInStorage)
		{
			// 184 (dt24_184): walk to the storage the resolver picked and put the loot in through the container transfer path (marks,
			// locks and item limits hold); the goal then ends. The selector re-issues it while the member still carries loot.
			const Storage box = findStorage(r.chainTarget);
			if (box.entity == 0 || f.dead || f.unconscious || f.down)
			{
				endTask();
				continue;
			}
			if (!f.walking() && std::hypot(f.x - box.x, f.z - box.z) > kInteractReach)
			{
				if (static_cast<double>(ticksSince(now, r.since)) * kTickSeconds > kGiveUpSeconds)
				{
					worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
					endTask();
				} else if (inSlice(now, id)) {
					walkToPoint(id, box.x, box.z);
				} else {
					mirrorFighter(id);
				}
				continue;
			}
			if (f.walking())
				continue;
			if (const std::uint32_t moved = putLoot184(id, r.chainTarget); moved > 0)
			{
				std::ostringstream line;
				line << "store 184 member " << id << " into storage " << r.chainTarget << ": " << moved << " items";
				logTask(line.str());
			} else {
				worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks; // it took nothing (full, refused): not this box for a while
			}
			endTask();
			mirrorFighter(id);
			continue;
		}
		if (routines::task::storesResources(r.goal))
		{
			// The push half (ds/machines): walk to my box and put every resource it accepts from the pack into it, then the goal ends.
			flecs::entity box;
			world_.each([&](flecs::entity entity, const state::Id& boxId, const state::Container&)
			{
				if (boxId.value == r.chainTarget)
					box = entity;
			});
			const state::Container* container = box.is_valid() ? box.get<state::Container>() : nullptr;
			const state::Inventory* held = box.is_valid() ? box.get<state::Inventory>() : nullptr;
			if (container == nullptr || held == nullptr || container->locked || f.dead || f.unconscious || f.down)
			{
				endTask();
				continue;
			}
			if (!f.walking() && std::hypot(f.x - container->useX, f.z - container->useZ) > kInteractReach)
			{
				if (static_cast<double>(ticksSince(now, r.since)) * kTickSeconds > kGiveUpSeconds)
				{
					worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
					endTask();
				} else if (inSlice(now, id)) {
					walkToPoint(id, container->useX, container->useZ);
				} else {
					mirrorFighter(id);
				}
				continue;
			}
			if (f.walking())
				continue;
			const grid::Rules rules(*gameData_);
			state::Inventory stock = *held;
			grid::settle(stock, rules, r.chainTarget);
			state::Inventory mine;
			if (const state::Inventory* packed = actor.get<state::Inventory>())
				mine = *packed;
			std::vector<state::Inventory::Entry> overflow = grid::settle(mine, rules, id);
			int put = 0;
			std::string first;
			for (std::size_t at = 0; at < mine.items.size();)
			{
				state::Inventory::Entry& entry = mine.items[at];
				const ItemInfo* info = entry.slot == state::Inventory::kCarried && entry.count > 0 ? combat_->data().item(entry.itemSid) : nullptr;
				if (info == nullptr || info->function != 0)
				{
					++at;
					continue;
				}
				state::Inventory::Entry one = loose(entry);
				one.count = 1;
				if (grid::autoplace(stock, rules, one) != 0)
				{
					++at; // the box cannot take another of this type
					continue;
				}
				if (first.empty())
					first = entry.itemSid;
				++put;
				if (--entry.count == 0)
					mine.items.erase(mine.items.begin() + static_cast<std::ptrdiff_t>(at));
			}
			if (put > 0)
			{
				++mine.version;
				++stock.version;
				storeInventory(actor, id, std::move(mine), std::move(overflow));
				box.set<state::Inventory>(std::move(stock));
				std::ostringstream line;
				line << "store " << r.goal << " member " << id << " into building " << r.chainTarget << ": " << put << " items (first " << first << ")";
				logTask(line.str());
			}
			endTask();
			continue;
		}
		if (r.goal == routines::task::EmptyMachine || r.goal == routines::task::BuildMats || r.goal == routines::task::LootContainer
			|| r.goal == routines::task::LootFoodAndStuff || routines::task::pullsMachine(r.goal) || routines::task::unjams(r.goal))
		{
			// 131 runs 284's pull on the box 243's finder picked (dt24_131 section 1: the twin; goal HAVE_SOME_LOOT_FROM, plan [284, 131]).
			// 92 / 247 / 284 (dt24_emptymachine_dismantle section 2): the building in the plan's hand (ours: the Routine's
			// chainTarget) is pulled one unit at a time into the member's inventory, up to the vf5 quantity, and the action
			// then ends - the selector re-issues the goal while it still holds. A machine (a state::Producer, 92 / 247) lists
			// its output slot's type; a storage box (a state::Container, 284) every distinct type it holds (section 2.2).
			flecs::entity source;
			world_.each([&](flecs::entity entity, const state::Id& sourceId, const state::Producer&)
			{
				if (sourceId.value == r.chainTarget)
					source = entity;
			});
			if (!source.is_valid())
				world_.each([&](flecs::entity entity, const state::Id& sourceId, const state::Container&)
				{
					if (sourceId.value == r.chainTarget)
						source = entity;
				});
			const state::Producer* machine = source.is_valid() ? source.get<state::Producer>() : nullptr;
			const state::Container* box = source.is_valid() ? source.get<state::Container>() : nullptr;
			const state::Inventory* held = source.is_valid() ? source.get<state::Inventory>() : nullptr;
			if (held == nullptr || (machine == nullptr && box == nullptr) || f.dead || f.unconscious || f.down)
			{
				endTask(); // the building is gone (a box emptied and dropped, a machine removed)
				continue;
			}
			const double useX = machine != nullptr ? machine->useX : box->useX;
			const double useZ = machine != nullptr ? machine->useZ : box->useZ;
			if (!f.walking() && std::hypot(f.x - useX, f.z - useZ) > kInteractReach)
			{
				if (static_cast<double>(ticksSince(now, r.since)) * kTickSeconds > kGiveUpSeconds)
				{
					worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
					endTask();
				} else if (inSlice(now, id)) {
					walkToPoint(id, useX, useZ);
				} else {
					mirrorFighter(id);
				}
				continue;
			}
			if (f.walking())
				continue;
			std::vector<std::string> types;
			if (machine != nullptr)
			{
				if (held->carried(machine->output) > 0)
					types.push_back(machine->output);
			} else {
				for (const state::Inventory::Entry& entry : held->items)
					if (entry.slot == state::Inventory::kCarried && entry.count > 0
						&& std::find(types.begin(), types.end(), entry.itemSid) == types.end())
						types.push_back(entry.itemSid);
			}
			const grid::Rules rules(*gameData_);
			state::Inventory mine;
			if (const state::Inventory* packed = actor.get<state::Inventory>())
				mine = *packed;
			std::vector<state::Inventory::Entry> overflow = grid::settle(mine, rules, id);
			const auto fits = [&](const std::string& item)
			{
				state::Inventory probe = mine;
				state::Inventory::Entry one;
				one.itemSid = item;
				one.count = 1;
				return grid::autoplace(probe, rules, loose(one)) == 0; // the exe's char vf+0x170
			};
			// The vf5 quantity reads the other buildings of my faction near me (the target itself excluded, section 2.1). The
			// exe's `b+0x198 == 6` fallback (the last carryable type wins with n = 1) is not modelled: no state field stands in
			// for it (UNSURE).
			std::string mySid;
			if (const state::Allegiance* mine2 = actor.get<state::Allegiance>())
				mySid = mine2->factionSid;
			const bool raiding = r.goal == routines::task::LootFoodAndStuff && box != nullptr && box->ownerSid != mySid; // a store that is not mine
			const auto othersFor = [&](const std::string& item) -> std::vector<worldres::NearbyStock>
			{
				std::vector<worldres::NearbyStock> others;
				if (mySid.empty())
					return others;
				world_.each([&](flecs::entity entity, const state::Id& otherId, const state::Producer& producer)
				{
					if (otherId.value == r.chainTarget || producer.ownerSid != mySid
						|| std::hypot(producer.x - f.x, producer.z - f.z) > worldres::kStockReach)
						return;
					const state::Inventory* stock = entity.get<state::Inventory>();
					worldres::NearbyStock other;
					other.count = stock != nullptr ? stock->carried(item) : 0;
					if (other.count == 0)
						other.accepts = producer.output == item && (stock == nullptr || stock->carried(item) < producer.cap);
					others.push_back(other);
				});
				world_.each([&](flecs::entity entity, const state::Id& otherId, const state::Container& container)
				{
					if (otherId.value == r.chainTarget || container.ownerSid != mySid || container.locked
						|| std::hypot(container.x - f.x, container.z - f.z) > worldres::kStockReach)
						return;
					const state::Inventory* stock = entity.get<state::Inventory>();
					worldres::NearbyStock other;
					other.count = stock != nullptr ? stock->carried(item) : 0;
					if (other.count == 0)
					{
						state::Inventory probe = stock != nullptr ? *stock : state::Inventory{};
						state::Inventory::Entry one;
						one.itemSid = item;
						one.count = 1;
						other.accepts = grid::autoplace(probe, rules, loose(one)) == 0;
					}
					others.push_back(other);
				});
				return others;
			};
			std::string item;
			int n = 0;
			for (const std::string& type : types)
			{
				if (!fits(type))
					continue; // the inventory cannot accept this type
				const ItemInfo* info = combat_->data().item(type);
				const std::vector<worldres::NearbyStock> others = othersFor(type);
				// A raider's pass takes as many as the food it already carries, at least one (vf5's carried-category sum, rwh.53 section
				// 2.3): the first pass takes 1, the next 1, then 2, 4 ... until the pack refuses the type or the store runs out.
				std::uint32_t carriedFood = 0;
				if (raiding)
					for (const state::Inventory::Entry& entry : mine.items)
					{
						const ItemInfo* carriedInfo = entry.slot == state::Inventory::kCarried ? combat_->data().item(entry.itemSid) : nullptr;
						if (carriedInfo != nullptr && carriedInfo->function == kItemFunctionFood)
							carriedFood += entry.count;
					}
				n = worldres::pullQuantity(carriedFood, info != nullptr && info->function == kItemFunctionFood, others,
					r.goal == routines::task::BuildMats, false);

				if (n > 0)
				{
					item = type;
					break; // the first type with something to take (the exe's own order, section 2)
				}
			}
			if (item.empty())
			{
				endTask(); // nothing to take: the action is impossible (flagImpossible)
				continue;
			}
			state::Inventory stock = *held;
			grid::settle(stock, rules, r.chainTarget);
			int took = 0;
			while (took < n)
			{
				const auto stack = std::find_if(stock.items.begin(), stock.items.end(), [&](const state::Inventory::Entry& entry)
				{
					return entry.slot == state::Inventory::kCarried && entry.itemSid == item && entry.count > 0;
				});
				if (stack == stock.items.end())
					break; // the building ran out
				state::Inventory::Entry one = loose(*stack);
				one.count = 1;
				if (grid::autoplace(mine, rules, one) != 0)
					break; // the character cannot take another
				if (--stack->count == 0)
					stock.items.erase(stack);
				++took;
			}
			if (took > 0)
			{
				++mine.version;
				++stock.version;
				storeInventory(actor, id, std::move(mine), std::move(overflow));
				source.set<state::Inventory>(std::move(stock));
				std::ostringstream line;
				line << "empty " << r.goal << " member " << id << " from building " << r.chainTarget << ": " << took << " x " << item;
				logTask(line.str());
			}
			endTask(); // the action ends; the selector re-issues the goal while it holds
			continue;
		}
		if (r.goal == routines::task::AcquireFood)
		{
			// 243 AQUIRE_FOOD_AT_HOMEBASE (dt24_243): at my food box, 244 GRAB_ONE_FOOD takes ONE food unit into the arms, then the
			// action ends and the selector re-scores (a member that holds food plans [243] alone, which the selector does not offer).
			flecs::entity source;
			world_.each([&](flecs::entity entity, const state::Id& sourceId, const state::Container&)
			{
				if (sourceId.value == r.chainTarget)
					source = entity;
			});
			const state::Container* box = source.is_valid() ? source.get<state::Container>() : nullptr;
			const state::Inventory* held = source.is_valid() ? source.get<state::Inventory>() : nullptr;
			if (box == nullptr || held == nullptr || f.dead || f.unconscious || f.down)
			{
				endTask(); // the box is gone (emptied and dropped) or the member cannot act
				continue;
			}
			if (!f.walking() && std::hypot(f.x - box->useX, f.z - box->useZ) > kInteractReach)
			{
				if (static_cast<double>(ticksSince(now, r.since)) * kTickSeconds > kGiveUpSeconds)
				{
					worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
					endTask();
				} else if (inSlice(now, id)) {
					walkToPoint(id, box->useX, box->useZ);
				} else {
					mirrorFighter(id);
				}
				continue;
			}
			if (f.walking())
				continue;
			// The take: one unit of the first food type the box holds (the exe's grab-one; not vf5's quantity).
			const grid::Rules rules(*gameData_);
			state::Inventory stock = *held;
			grid::settle(stock, rules, r.chainTarget);
			state::Inventory mine;
			if (const state::Inventory* packed = actor.get<state::Inventory>())
				mine = *packed;
			std::vector<state::Inventory::Entry> overflow = grid::settle(mine, rules, id);
			std::string took;
			for (auto it = stock.items.begin(); it != stock.items.end();)
			{
				const ItemInfo* info = it->slot == state::Inventory::kCarried ? combat_->data().item(it->itemSid) : nullptr;
				if (info == nullptr || info->function != kItemFunctionFood)
				{
					++it;
					continue;
				}
				state::Inventory::Entry one = loose(*it);
				one.count = 1;
				if (grid::autoplace(mine, rules, one) != 0)
					break; // the pack cannot take this type (the exe's flagImpossible)
				took = it->itemSid;
				if (--it->count == 0)
					stock.items.erase(it);
				break;
			}
			if (took.empty())
			{
				endTask(); // nothing left to take
				continue;
			}
			++mine.version;
			++stock.version;
			storeInventory(actor, id, std::move(mine), std::move(overflow));
			source.set<state::Inventory>(std::move(stock));
			{
				std::ostringstream line;
				line << "food 243 member " << id << " from box " << r.chainTarget << ": 1 x " << took;
				logTask(line.str());
			}
			endTask();
			continue;
		}
		if (r.goal == routines::task::LockAllMyDoors || r.goal == routines::task::LockDoorsStayOutside || r.goal == routines::task::OpenUpShopDoors)
		{
			// 155 / 197 / 86 (dt24_155): the plan is [the walk to the door side, the lock / open row, the goal node]; ours walks to the
			// door and locks one at a time (orderHomeDoorLock), as the town scorer's door step does; the open (86) stays one act.
			const bool lock = r.goal != routines::task::OpenUpShopDoors;
			const HomeDoors hd = homeDoorsOf(f.homeBuilding, f.x, f.z);
			if (!hd.any || f.dead || f.unconscious || f.down || f.target != 0)
			{
				endTask();
				continue;
			}
			if (lock)
			{
				// the planned steps [163 / 199 walk, 162 / 198 lock] as orders, one door at a time (the walk is the order's own route)
				if (orderHomeDoorLock(id, hd, r.goal == routines::task::LockDoorsStayOutside ? DoorSide::Outside : DoorSide::Inside))
					continue;
				f.houseLocked = true;
				endTask();
				continue;
			}
			if (hd.nearest <= kTownDoorReach)
			{
				applyHomeDoors(hd.doors, lock);
				f.houseLocked = lock;
				f.clearRoute();
				endTask();
				continue;
			}
			if (inSlice(now, id))
			{
				double bestD = 1.0e30;
				std::array<double, 2> at{f.x, f.z};
				for (const flecs::entity door : hd.doors)
				{
					const state::Door* d = door.get<state::Door>();
					const double dist = d != nullptr ? std::hypot(f.x - d->x, f.z - d->z) : 1.0e30;
					if (dist < bestD)
					{
						bestD = dist;
						at = {d->x, d->z};
					}
				}
				walkToPoint(id, at[0], at[1]);
			}
			continue;
		}
		if (r.goal == routines::task::Shopkeeper || r.goal == routines::task::SitOnThrone || routines::task::standsAtNode(r.goal))
		{
			// 20 / 255: walk to the claimed counter / throne slot and stay on it; the package window or a fight ends it (the selector
			// re-scores). No site entry (a stale target) ends the goal - never a terminal state.
			const routines::SiteEntry* entry = residentData_->sites.siteAt({static_cast<routines::SiteKind>(r.target.kind), r.target.object, r.target.slot});
			if (entry == nullptr || f.dead || f.unconscious || f.down || f.target != 0)
			{
				endTask();
				continue;
			}
			if (std::hypot(f.x - entry->x, f.z - entry->z) > kBodyReach)
			{
				if (inSlice(now, id))
					walkToPoint(id, entry->x, entry->z);
				continue;
			}
			f.clearRoute();
			f.forwardSpeed = 0.0;
			f.lateralSpeed = 0.0;
			if (routines::task::standsAtNode(r.goal))
			{
				// Task_StandAtNode start (rwh.55 section 5): a type-1 node is faced along its stored direction (node+0xc0).
				const double yaw = std::atan2(entry->faceX, entry->faceZ);
				if (std::fabs(yaw - f.yaw) > 0.01)
				{
					f.yaw = yaw;
					mirrorFighter(id);
				}
			}
			continue;
		}
		if (r.goal == routines::task::SitAround)
		{
			// 275 SIT_AROUND (dt24_275): walk to the building the resolver picked, then stay for the row's hold (0.2 + rand x 0.8 h,
			// the decode's +0x18 / +0x1c fields). A brawl or a missing building ends it (never a terminal state).
			// 7 UNEQUIP_WEAPON / 6 EQUIP_WEAPON: the plan's sheathe in front of 275 (GATES_CARRY_STATIC - a peaceful row needs
			// hasWeaponEquipped = 0) and its undo; the sit is not a weapon drop. chainElapsed marks the sheathe (1 = the weapon went
			// into the pack here), so a member that was already bare-handed is left as it is. The exe draws again before an attack;
			// ours restores the carried weapon whenever the sit ends (the hold, a fight, or the package losing the row).
			const auto drawBack = [&]()
			{
				if (r.chainElapsed >= 1.0)
					drawWeapon(id);
			};
			const BuildingSite* site = world_.buildingSite(r.chainTarget);
			if (site == nullptr || f.dead || f.unconscious || f.down || f.target != 0)
			{
				drawBack();
				endTask();
				continue;
			}
			if (r.chainElapsed < 1.0 && sheatheWeapon(id)) // the plan's 7 (the shared step)
			{
				r.chainElapsed = 1.0;
				actor.set<state::Routine>(r);
				mirrorFighter(id);
			}
			if (r.holdUntil == 0)
			{
				const std::array<double, 2> door = doorOf(*site);
				if (!f.walking() && std::hypot(f.x - door[0], f.z - door[1]) <= kBodyReach)
				{
					f.clearRoute();
					f.forwardSpeed = 0.0;
					f.lateralSpeed = 0.0;
					f.yaw = std::atan2(site->cx - f.x, site->cz - f.z);
					RngStream dice(streamSeed(streamSeed(streamSeed(seed_, kRoutineStream), static_cast<std::size_t>(id)),
						static_cast<std::size_t>(r.since + 1)));
					const routines::Hold hold = routines::holdOf(r.goal);
					r.holdUntil = now + static_cast<std::uint64_t>((hold.base + dice.uniform01() * hold.span) * static_cast<double>(kTicksPerGameHour));
					actor.set<state::Routine>(std::move(r));
					mirrorFighter(id);
				} else if (inSlice(now, id)) {
					walkToPoint(id, door[0], door[1]);
				}
				continue;
			}
			f.clearRoute();
			f.forwardSpeed = 0.0;
			f.lateralSpeed = 0.0;
			if (now < r.holdUntil)
				continue;
			drawBack(); // the exe draws again: the sit's sheathe is undone
			endTask(); // the sit ran out: the selector re-scores
			continue;
		}
		if (r.goal == routines::task::SleepOnFloor || r.goal == routines::task::SleepOnFloorFakeAmbush)
		{
			// 175 SLEEP_ON_FLOOR (dt24_175, Task_SleepOnFloor); 261 SLEEP_ON_FLOOR_FAKE_AMBUSH (14001231e) is the same class: no finder and no bed - lie down where it stands for a random time,
			// asleep (pose() draws the sleeponfloor clip for a sleeper with no bed), then get up. UNSURE: the exe's duration bounds
			// (DAT_141696c70 / DAT_14169bb40) are unread (the bed hold stands in) and the resting status CharStatus+0xb4 has no
			// field in ours. The dispatch holds the member out of scoring while it sleeps; a fight wakes it here.
			if (f.dead || f.unconscious || f.down)
			{
				f.asleep = false;
				endTask();
				continue;
			}
			if (r.holdUntil == 0)
			{
				f.clearRoute();
				f.forwardSpeed = 0.0;
				f.lateralSpeed = 0.0;
				RngStream dice(streamSeed(streamSeed(streamSeed(seed_, kRoutineStream), static_cast<std::size_t>(id)),
					static_cast<std::size_t>(r.since + 1)));
				f.yaw = 2.0 * std::numbers::pi * dice.uniform01(); // the start's random facing (dt24_175 section 2)
				const routines::Hold hold = routines::holdOf(r.goal);
				r.holdUntil = now + static_cast<std::uint64_t>((hold.base + dice.uniform01() * hold.span) * static_cast<double>(kTicksPerGameHour));
				f.asleep = true;
				actor.set<state::Routine>(std::move(r));
				mirrorFighter(id);
				continue;
			}
			bool underAttack = false;
			for (const CombatArena::Member& other : members)
				if (other.fighter.target == id && !other.fighter.dead)
				{
					underAttack = true;
					break;
				}
			if (f.target != 0 || underAttack || now >= r.holdUntil)
			{
				f.asleep = false;
				endTask();
				continue;
			}
			continue;
		}
		if (!lootGoal)
			continue;
		// LOOT_THE_DEAD: at the body, take the food it carries and hand the member back to the follow.
		state::Fighter* body = combat_->find(r.chainTarget);
		if (body == nullptr || body->carriedBy != 0 || f.dead || f.unconscious || f.down)
		{
			actor.remove<state::Routine>();
			mirrorFighter(id);
			continue;
		}
		if (!f.walking() && std::hypot(f.x - body->x, f.z - body->z) > kBodyReach)
		{
			// A walk that ends where the looter stands, within kOffMeshLootReach of the body, is at the nearest walkable point to a body that
			// lies off the mesh: it loots from there. Otherwise it walks, and after kGiveUpSeconds it gives up with a logged reason.
			bool reached = false;
			if (static_cast<double>(ticksSince(now, r.since)) * kTickSeconds > kGiveUpSeconds)
			{
				worldTaskBlocks_[{id, r.chainTarget}] = now + kBlockTicks;
				actor.remove<state::Routine>();
				std::ostringstream line;
				line << "loot 177 member " << id << " gave up on body " << r.chainTarget << ": no arrival in " << kGiveUpSeconds << " s";
				logTask(line.str());
			} else if (inSlice(now, id)) {
				reached = walkTo(id, *body) && std::hypot(f.x - body->x, f.z - body->z) <= kOffMeshLootReach;
			}
			if (!reached)
			{
				mirrorFighter(id);
				continue;
			}
		}
		if (f.walking())
			continue;
		// The food, from the body's main inventory (the equipped club and the raw iron stay): every carried food entry that fits moves.
		flecs::entity bodyEntity = fighterEntity(r.chainTarget);
		const state::Inventory* held = bodyEntity.get<state::Inventory>();
		if (held != nullptr && gameData_ != nullptr)
		{
			const grid::Rules rules(*gameData_);
			state::Inventory source = *held;
			grid::settle(source, rules, r.chainTarget);
			state::Inventory mine;
			if (const state::Inventory* packed = actor.get<state::Inventory>())
				mine = *packed;
			std::vector<state::Inventory::Entry> overflow = grid::settle(mine, rules, id);
			bool took = false;
			std::map<std::string, std::uint32_t> taken; // per item sid: what moved off the body
			for (auto it = source.items.begin(); it != source.items.end();)
			{
				const ItemInfo* info = it->slot == state::Inventory::kCarried ? combat_->data().item(it->itemSid) : nullptr;
				if (info == nullptr || info->function != kItemFunctionFood)
				{
					++it;
					continue;
				}
				const std::uint32_t left = grid::autoplace(mine, rules, loose(*it));
				if (left == it->count)
				{
					++it; // no room: the food stays on the body
					continue;
				}
				took = true;
				taken[it->itemSid] += it->count - left;
				if (left > 0)
				{
					it->count = left;
					++it;
					continue;
				}
				it = source.items.erase(it);
			}
			if (took)
			{
				++mine.version;
				++source.version;
				storeInventory(actor, id, std::move(mine), std::move(overflow));
				bodyEntity.set<state::Inventory>(std::move(source));
				// Task slaver-crime (peer): the take in the server's log - item, count, the body, and who took it. The client cannot
				// see an NPC loot.
				for (const auto& [itemSid, count] : taken)
				{
					std::ostringstream line;
					line << "loot 177 member " << id << " from body " << r.chainTarget << ": " << count << " x " << itemSid;
					logTask(line.str());
				}
			}
		}
		actor.remove<state::Routine>();
		mirrorFighter(id);
	}

	KSIM_PROFILE_SUB(WtInHand);
	// ---- the fight goals' own pass (task fight-goals) ----
	// The original re-selects goals per AI on its own throttle: a random phase U(1,40)/60 s re-rolled at every full update, while the
	// senses and the threat evaluation run every frame (kinspect patrol_aggro_town.md a8a41b7 section 3; the exact cadence is UNSURE).
	// So the fight goals are scored on that per-character clock, every tick: a member whose plan (the squad's route or follow) is not
	// an order may be preempted by a fight goal its package carries - selectGoal's tier 3 over the plan. Combat no longer decides this
	// for a walker (b38291f's block is gone; patrol_aggro.md c332e51 section 3: the route is dropped at the plan level).
	if (combat_ != nullptr && residentData_ != nullptr && ai_ != nullptr)
	{
		const double hour = std::fmod(trader::gameHours(now), 24.0);
		// One pass over the members (not the squads: a squad's member list costs a query per call, and this runs every tick). No
		// squad-wide fight guard (unlike the 1 Hz pass): the others keep sensing and may join through 143 / 15 / 16 while a squadmate
		// fights (bq18 / BQ18).
		const auto fightGoal = [&](flecs::entity member, const state::Id& memberId)
		{
			const state::Fighter* f = std::as_const(*combat_).find(memberId.value); // a read: the writes below take the mutable find (the mirror copies only what was written)
			if (f == nullptr || f->dead || f->unconscious || f->down || f->playerControlled || f->carrying != 0
				|| f->carriedBy != 0 || f->asleep || f->goal.kind != state::IndividualKind::None || f->fleeing)
				return;
			// Only a plan may be preempted: a route or follow a command did NOT set. Plan is the marked plan (the world carry, a world
			// task's walk); None is an unmarked one a routine wrote (a resident's job walk, a guard's follow to its post); Order is a
			// command's (the persistent-order pass, above the package tiers, dt14). A member with no task is the 1 Hz selector's.
			const bool plan = (f->routeSource != state::Fighter::PlanSource::Order && f->walking())
				|| (f->followTarget != 0 && f->followSource != state::Fighter::PlanSource::Order);
			if (!plan)
				return;
			// A member already holding a target keeps it over a plan issued again after the pick: the squad carry and a world task walk
			// their members every second, and combat's walkOrder moves a walking body instead of fighting (`walking()` returns before the
			// state machine). The exe drops the route at the plan level while a fight goal holds the plan (patrol_aggro.md c332e51), so
			// the replan is dropped here too, for a resident's job walk as well (b38291f dropped it for every walker, from combat); an order
			// is not a plan and stands (dt14). Without this the bandits of ksim_bw_orders seed 3 took the player, then walked their patrol
			// route beside the den while it ran away.
			if (f->target != 0)
			{
				combat_->find(memberId.value)->clearRoute();
				combat_->endFollow(*combat_->find(memberId.value));
				return;
			}
			// A new pick is the package selector's own: not while a task executor holds the member (carry.cpp and the interaction, door,
			// take-out and gate steps own those), and not a world squad's own package task (a capture or loot Routine, below). A promoted
			// resident squad's routine walk is only a routine goal: an URGENT goal whose finder returns a target (16 on a sensed hostile)
			// beats any routine goal at the next selectGoal (s73_static BQ1), and the town scorer offers none of the fight goals, so the
			// walk (a Relaxing patrol that never stands still) would otherwise carry the member past its enemy for ever (k16.6.103).
			if (member.has<state::InteractTask>() || member.has<state::DoorStep>() || member.has<state::CarryStep>() || member.has<state::TakeOut>())
				return;
			if (const state::Routine* routine = member.get<state::Routine>(); routine != nullptr)
			{
				// A goal with a chain in hand (a catch to carry, a corpse, a plan the executor steps: the slavers' capture and cage goals)
				// is not preempted: the chain's own executor ends it.
				const flecs::entity squad = member.target<state::MemberOf>();
				if (!squad.is_valid() || !squad.has<state::SquadRoutine>() || routine->chainTarget != 0 || !routine->plan.empty())
					return;
			}
			const double worldNow = static_cast<double>(now) * kTickSeconds;
			if (f->fightGoalDue > worldNow)
				return;
			// The next re-selection: the exe's random phase U(1,40)/60 s, a pure function of the seed, the fighter's dice key and the
			// tick (task CC's convention: a replay is identical, nothing is saved).
			RngStream phase(streamSeed(streamSeed(streamSeed(seed_, kFightGoalStream), static_cast<std::size_t>(combat_->diceKey(memberId.value))),
				static_cast<std::size_t>(now)));
			combat_->find(memberId.value)->fightGoalDue = worldNow + static_cast<double>(phase.between(1, 40)) / 60.0;
			const CombatArena::FightGoal fight = combat_->fightGoalOf(memberId.value);
			if (fight.target == 0)
				return;
			// The goal must be a row the member's package runs now: its tier, weight and window are the data's (G2); the package is the
			// applied one (the G1 board) - the records refreshJoinGoals reads, so a resident squad (the Bar's crew: goal 34) joins on the
			// goals its combat pick already admits, not only a world squad.
			const flecs::entity squad = member.target<state::MemberOf>();
			if (!squad.is_valid())
				return;
			// The board's applied package, built on first read (appliedPackage): a read that took the registry until a board existed made a
			// loaded run (boards not saved) differ from the straight one (ksim_town_scorer, ksim_world_follow).
			const routines::CompiledPackage* package = appliedPackage(squad);
			if (package == nullptr)
				return;
			const routines::Role listRole = liveRole(member);
			bool inPackage = false;
			for (const routines::GoalRow& row : package->list(listRole))
				inPackage = inPackage || (row.task == fight.task && goalsel::windowHolds(row.start, row.end, hour));
			if (!inPackage)
				return;
			// Commit: the goal preempts the plan and the combat tick fights the target it handed over (whom, not whether).
			combat_->orderFight(memberId.value, fight.target, fight.task);
			combat_->find(memberId.value)->clearRoute();
			combat_->endFollow(*combat_->find(memberId.value));
			logTask("fight goal " + std::to_string(fight.task) + " member " + std::to_string(memberId.value) + " target "
				+ std::to_string(fight.target));
		};
		// The arena's members by index (a few hundred bodies), not every entity holding an Id (squads, towns, factions too).
		for (std::size_t i = 0; i < members.size(); ++i)
			fightGoal(ecs.entity(combatEntities_[i]), state::Id{members[i].id});
	}

	KSIM_PROFILE_SUB(WtFightGoals);
	// From here the second's work is per squad slice (forSquadsInSlice below, k16.6.232); the passes over everyone stay at the second.
	const bool secondTick = now % kSquadIntervalTicks == 0;
	// A player's slave scores only through the graft: once the row is gone, the state leaves 1 or it sneaks, the goal it holds ends too.
	for (std::size_t i = 0; secondTick && i < members.size(); ++i)
	{
		const state::Fighter& f = members[i].fighter;
		flecs::entity slave = ecs.entity(combatEntities_[i]);
		if (!slave.has<state::Routine>())
			graftRoutines_.erase(members[i].id);
		if (!f.playerControlled || grafts(f) || graftRoutines_.erase(members[i].id) == 0 || !slave.has<state::Routine>())
			continue;
		routineBook_.release(members[i].id);
		slave.remove<state::Routine>();
		logTask("graft ends member " + std::to_string(members[i].id));
	}
	knowWorldSquads();
	if (residentData_->world.empty())
		return;

	// Task dt27 (194 / 218): my surroundings' slave-storage buildings - SQUAD_TEMPLATE `building designation` 10 (BD_SLAVE_STORAGE) - with
	// the town they stand in, their keeper (a resident whose home it is, the nearest to the building) and whether their owner is allied to
	// the member (my faction, none for either side, or a bond). The owner is the placement's `owner faction ID`, else the town's owner.
	const auto slaveStoragesNear = [&](const state::Fighter& f) -> std::vector<worldres::SlaveStorage>
	{
		std::vector<worldres::SlaveStorage> out;
		for (const BuildingSite& site : world_.buildingSites())
		{
			if (std::hypot(site.cx - f.x, site.cz - f.z) > 2000.0 || site.squadTemplate.empty())
				continue;
			const kdata::Record* squad = gameData_->find(site.squadTemplate);
			if (squad == nullptr || squad->intOr("building designation", 0) != 10)
				continue;
			worldres::SlaveStorage storage;
			storage.building = site.key;
			storage.x = site.cx;
			storage.z = site.cz;
			storage.town = towncircle::at(townCircles, site.cx, site.cz);
			std::uint64_t owner = 0;
			world_.each([&](const state::Id& id, const state::FactionState& faction)
			{
				if (!site.ownerFaction.empty() && faction.templateSid == site.ownerFaction)
					owner = id.value;
			});
			storage.allied = f.faction == 0 || owner == 0 || combat_->bonded(f.faction, owner);
			double best = 1.0e30;
			for (const CombatArena::Member& other : members)
			{
				if (other.fighter.homeBuilding != site.key || other.fighter.dead || other.fighter.unconscious || other.fighter.down)
					continue;
				const double d2 = (other.fighter.x - site.cx) * (other.fighter.x - site.cx) + (other.fighter.z - site.cz) * (other.fighter.z - site.cz);
				if (d2 < best)
				{
					best = d2;
					storage.keeper = other.id;
				}
			}
			out.push_back(storage);
		}
		return out;
	};

	// Task dt25 (45 BODYGUARD, kinspect dt25 section 1.3 / 1.5): the protected hand. A member whose package carries the goal
	// holds the squad's leader (the package's target hand is not modelled: OURS, the leader stands in for the charge) as
	// AITaskSystem+0x10 while a hostile is within 40 u of the charge or of the member; the combat picker's +10 toward the
	// charge's attackers is live from then on (combat.cpp scanCandidates). The calm moment drops the hand, so the follow wins -
	// the original's goal test - and a goal outside the bodyguard class clears it too. It runs before the free-member test below: the
	// goal is offered precisely while the squad fights (the live check, dt25 section 1.6).
	const auto bodyguardHand = [&](flecs::entity squad, std::uint64_t memberId, flecs::entity member, std::uint64_t followLeader)
	{
		const state::Fighter* self = combat_->find(memberId);
		if (self == nullptr || self->dead || self->unconscious || self->playerControlled || memberId == followLeader)
			return;
		blackboard::Board* board = blackboardOf(squad);
		if (board == nullptr)
			return;
		const routines::Role listRole = liveRole(member);
		const std::vector<routines::GoalRow>& rows = rowsOf(*board, listRole);
		if (std::none_of(rows.begin(), rows.end(), [](const routines::GoalRow& row) { return row.task == routines::task::Bodyguard; }))
			return;
		std::uint64_t hand = 0;
		const state::Fighter* guarded = followLeader != 0 ? combat_->find(followLeader) : nullptr;
		if (guarded != nullptr && !guarded->dead && !guarded->unconscious)
		{
			const sensor::Context ctx = sensorContext();
			const auto near = [&](const state::Fighter& from)
			{
				for (const CombatArena::Member& other : combat_->members())
				{
					if (other.id == memberId || other.id == followLeader || other.fighter.dead || other.fighter.unconscious
						|| other.fighter.carriedBy != 0 || !sensor::enemy(ctx, *self, other.fighter)
						|| !perception::aware(*self, other.id))
						continue;
					if (std::hypot(other.fighter.x - from.x, other.fighter.z - from.z) <= kBodyguardThreat)
						return true;
				}
				return false;
			};
			if (near(*guarded) || near(*self))
				hand = followLeader;
		}
		state::Fighter* me = combat_->find(memberId);
		if (me != nullptr && me->protectTarget != hand)
		{
			me->protectTarget = hand;
			mirrorFighter(memberId);
		}
	};

	// ---- the native goal selector (task BW; kinspect dt14 G2, dt15 / dt16 G3) ----
	// Every free member of a built world squad scores its package's goal rows for its role each second: a resolver per row,
	// goalsel::select (strict tiers, the running goal's 1.25 bonus and hold, crowd decay), and the winner is committed as the
	// member's Routine - its direct task's executor (167 / 177 / 183 / 260 / 168 / 173 in this file, 112 in carry.cpp). Nothing
	// scoring leaves the member on the squad's follow (IDLE, dt14 section 8) and it re-scores next second.
	if (secondTick)
	{
		refreshRaidLeaders();
		applyRaidVictory(now);
	}
	// The occupancy map is shared derived state: built once a second, whole, here (and once after a load or a start) before any slice reads
	// it, never piece by piece by the squads' own slices (k16.6.320). Package_OccupyTown's start / tick hooks (140288fe0, 140290690, rwh.53
	// section 3): the package holds the town the platoon stood in when it started, and the platoon's faction goes into that town's occupancy
	// map. The town's owner is not touched (4.3). It reads each board as its squad's last slice left it.
	if (secondTick || !occupiersBuilt_)
	{
		occupiersBuilt_ = true;
		occupiers_.clear();
		world_.each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance&)
		{
			blackboard::Board* board = residentData_->world.count(id.value) != 0 ? blackboardOf(squad) : nullptr;
			if (board == nullptr)
				return;
			const blackboard::Package* occupying = board->current.empty() ? nullptr : blackboard::find(*board, board->current);
			if (occupying == nullptr || occupying->signalFunc != blackboard::kOccupyTown)
			{
				board->occupyTown = 0;
				return;
			}
			const state::Fighter* lead = combat_->find(leaderOf(*combat_, squadMembers(world_, squad)));
			if (lead != nullptr && board->occupyTown == 0)
				board->occupyTown = townEntityAt(lead->x, lead->z);
			if (lead != nullptr && board->occupyTown != 0)
				occupiers_[board->occupyTown].push_back(lead->faction);
		});
	}
	const auto stepSquad = [&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance)
	{
		if (residentData_->world.count(id.value) == 0)
			return; // a built squad, a wandering trader included (task BP: it is built at Full detail like the others)
		const auto list = squadMembers(world_, squad);
		// ---- the package stack (task BY; kinspect dt19 G1) ----
		// A built squad's platoon holds one Blackboard: the template's `AI packages` (val0 = priority), the faction's `AI
		// fallback` registered at priority 0 after them, the applied package and any overriding contract. The ladder runs each
		// second; the selector below scores the applied package's merged goal rows.
		// The campaign phase hook (FACTION_CAMPAIGN, raid_phases.cpp): a campaign squad pushes its phase package as the 24 h contract; a war
		// class finishes when its campaign no longer has an instance for the squad (the order is over), and the retreat package reads the
		// platoon as not ready (RetreatAndRefresh starts then, 1402871c0; OURS).
		blackboard::Board* board = blackboardOf(squad);
		if (board != nullptr)
		{
			pruneOwned(*board);
			const blackboard::Campaign campaign = raidCampaign(squad, id.value);
			blackboard::Situation situation = situationOf(squad, board, list, now);
			// Only a campaign squad's war contract ends with its instance; one pushed by hand (a dialogue, a test) has no campaign to lose.
			const state::GroupAI* orders = squad.get<state::GroupAI>();
			situation.warInstance = campaign.present || orders == nullptr || orders->order.campaign.empty();
			if (campaign.present && campaign.package.signalFunc == blackboard::kRetreatAndRefresh)
				situation.platoonReady = false;
			blackboard::tick(*board, trader::gameHours(now), campaign, situation);
			situations_[squad.id()] = SituationMemo{situation, now};
			if (ai_ != nullptr && ai_->config().victoryAi)
				watchRaidVictory(squad, id.value, situation, list, now);
			// The slaver shipping class' start hook (140287520, rwh.8): the town finder runs once per activation and the found town is the
			// package's target; a stopped package forgets it. Nothing found writes nothing (the package then has no target to walk to).
			const blackboard::Package* running = board->current.empty() ? nullptr : blackboard::find(*board, board->current);
			if (running == nullptr || !blackboard::slaverShipping(running->signalFunc))
			{
				board->depot = 0;
				board->depotStamp = -1.0;
			} else if (board->depotStamp != board->startedAt) {
				board->depotStamp = board->startedAt;
				const state::Fighter* lead = combat_->find(leaderOf(*combat_, list));
				const std::uint64_t found = lead != nullptr ? findSlaveDepot(squad, *lead, running->signalFunc) : 0;
				if (found != 0)
					board->depot = found;
				std::ostringstream line;
				line << "slaver deliver squad " << id.value << " starts (package " << board->current << ", " << board->owned.size()
					<< " owned): depot town " << found;
				logTask(line.str());
			}
			// 192 / 194 at the depot (the squad's leader is the follow's carrot, so it does not score goals): once the leader stands in the depot
			// town it walks to the keeper of the town's nearest allied slave-storage building (194's resolver) and the talk (159 / 161) is the
			// hand-over itself, `DA_GIVE_TARGET_MY_SLAVES` on the whole ownership set (OURS: a direct re-own, the delivery line is not run).
			if (running != nullptr && blackboard::slaverShipping(running->signalFunc) && board->depot != 0 && !board->owned.empty())
			{
				const std::uint64_t leaderId = leaderOf(*combat_, list);
				const state::Fighter* lead = combat_->find(leaderId);
				std::uint64_t keeperId = 0;
				if (lead != nullptr && !lead->dead && !lead->unconscious && !lead->down && lead->carriedBy == 0 && lead->carrying == 0
					&& lead->target == 0 && !squadInFight(*combat_, list))
				{
					bool inDepot = false;
					world_.each([&](const state::Id& townId, const state::TownState& town)
					{
						if (townId.value != board->depot)
							return;
						const towncircle::Circle circle = towncircle::of(*gameData_, town);
						inDepot = std::hypot(lead->x - circle.x, lead->z - circle.z) <= circle.radius;
					});
					if (inDepot)
						keeperId = worldres::giveSlaves218(*lead, towncircle::at(townCircles, lead->x, lead->z), slaveStoragesNear(*lead));
				}
				const state::Fighter* keeper = keeperId != 0 ? combat_->find(keeperId) : nullptr;
				if (keeper != nullptr && !keeper->dead)
				{
					// The keeper stands inside its building, off the mesh a walker routes on (interiors are not walked, UNSURE): the leader goes
					// as near as the mesh allows (a point walk when the keeper itself has no route) and the talk happens from where it stops.
					bool talks = std::hypot(lead->x - keeper->x, lead->z - keeper->z) <= kInteractReach;
					if (!talks && !lead->walking())
					{
						walkTo(leaderId, *keeper);
						if (!lead->walking())
							walkToPoint(leaderId, keeper->x, keeper->z);
						talks = !lead->walking(); // no nearer point to walk to
					}
					if (talks)
					{
						const std::size_t given = giveOwnedSlaves(*board, keeperId);
						std::ostringstream line;
						line << "deliver 194 member " << leaderId << " gave " << given << " slaves to " << keeperId << " (squad " << id.value << ")";
						logTask(line.str());
					}
				}
			}
		}
		// The graft (rwh.99 section 3): a slave this squad's member holds (state 1, the 187 row, not sneaking; SlaveTrack's captor) scores the
		// package's Slave-role rows as its own candidates, in this pass and through these executors. OURS: the squad's `list` stays the
		// squad's (its leader, fights and crowd counts); the slaves are scored after it. A slave members are on 58 on is not scored yet (rwh.111: its own
		// follow would walk it off from them; the loop below holds it for the medics committed in this pass).
		std::vector<std::pair<std::uint64_t, flecs::entity>> selectList = list;
		std::set<std::uint64_t> grafted;
		for (const auto& [slaveId, track] : slaveTrack_)
		{
			const state::Fighter* slave = combat_->find(slaveId);
			const state::Fighter* hand = combat_->find(track.captor);
			if (slave == nullptr || hand == nullptr || hand->dead || hand->squad != id.value || !grafts(*slave) || aided(slaveId))
				continue;
			selectList.emplace_back(slaveId, fighterEntity(slaveId));
			grafted.insert(slaveId);
		}
		// No squad-level fight guard (rwh.102 1: no sighting is shared, a member with no record keeps its package task - follow 141, patrol 22):
		// each member takes its own goal while its squadmates fight, and one with a target is not free (freeMember). The bodyguard's hand
		// is set for every member in the loop below, before that test, so it is offered while the fight runs.
		std::set<std::uint64_t> squadTargets; // what the squad already runs (the one-catcher test)
		std::set<std::uint64_t> medicked; // the bodies members committed 58 on in this pass
		std::map<std::uint16_t, int> onTask; // squadmates on each goal (the crowd decay)
		for (const auto& [memberId, member] : list)
		{
			const state::Routine* routine = member.get<state::Routine>();
			if (routine == nullptr)
				continue;
			++onTask[routine->goal];
			if (routine->chainTarget != 0)
				squadTargets.insert(routine->chainTarget);
		}
		const double hour = std::fmod(trader::gameHours(now), 24.0);
		const sensor::Context ctx = sensorContext();
		const auto claimed = [&](std::uint64_t target) { return squadTargets.count(target) != 0; };
		const auto lookup = [&](std::uint64_t target) -> const state::Fighter* { return combat_->find(target); };
		const auto looks = [&](std::uint64_t target, const state::Fighter&) { return wearsShackles(*gameData_, fighterEntity(target)); };

		// ---- the selector: one pass per free member ----
		// The squad's follow leader stays out: it is the follow's carrot (OURS; the exe's leader runs its own goals and its squad is
		// moved by the traversal system, which this slice does not have).
		const std::uint64_t followLeader = leaderOf(*combat_, list);
		// 36's package town (dt24_patrol_town_pot section 1): the platoon's town, fixed for the package's life - our squad's home town.
		towncircle::Circle patrolTown;
		bool hasPatrolTown = false;
		if (instance.homeTown.id != 0 && ecs.is_alive(instance.homeTown.id))
			if (const state::TownState* town = ecs.entity(instance.homeTown.id).get<state::TownState>())
			{
				patrolTown = towncircle::of(*gameData_, *town);
				hasPatrolTown = patrolTown.radius > 0.0;
			}
		if (board == nullptr) // no AI layer (a bare rig), or born since the last ensureBlackboards pass
			return;
		std::vector<std::uint64_t> graftPassed; // grafted slaves that went through the selector this pass (a Routine they hold after it is the graft's)
		// Only a member that has had a target (Fighter::fought) stands down: sheathing every armed member would shift every untouched
		// world squad (the pinned hashes, ksim_kscen_radius's bandit) for no bug.
		// Nobody in the squad fights: a member on the peaceful follow (141 / 171, no Routine, so no planner step) and the follow leader (which
		// the selector skips) put the weapon away, as the planner's 7 UNEQUIP_WEAPON does in front of every other peaceful goal (GATES_CARRY_STATIC;
		// whether Task_FollowLeader's row itself needs hasWeaponEquipped = 0 is UNSURE). k16.6.288: a Routine-less caravan kept its swords out after the capture.
		const bool squadQuiet = std::none_of(list.begin(), list.end(), [this](const auto& entry)
		{
			const state::Fighter* other = combat_->find(entry.first);
			return other != nullptr && !other->dead && inFightNow(*other);
		});
		for (const auto& [memberId, member] : selectList)
		{
			const state::Fighter* self = combat_->find(memberId);
			if (self == nullptr)
				continue;
			if (memberId == followLeader)
			{
				// The leader has no pick to say nothing is left to fight, so it also needs no enemy near (the followers' 141 pick says it).
				if (squadQuiet && self->fought && weaponInHands(memberId)
					&& std::none_of(combat_->members().begin(), combat_->members().end(), [&](const CombatArena::Member& other)
					{
						return other.id != memberId && !other.fighter.dead && !other.fighter.unconscious && !other.fighter.down
							&& std::hypot(other.fighter.x - self->x, other.fighter.z - self->z) < kLeaderStandDownRadius
							&& (sensor::enemy(ctx, *self, other.fighter) || sensor::enemy(ctx, other.fighter, *self));
					}))
					sheatheWeapon(memberId);
				continue;
			}
			const bool graft = grafted.count(memberId) != 0;
			if (graft && medicked.count(memberId) != 0)
				continue;
			const routines::Role listRole = graft ? routines::Role::Slave : liveRole(member);
			const std::vector<routines::GoalRow> rows = rowsFor(*board, listRole, self->raceSid);
			const auto hasRow = [&rows](std::uint16_t task)
			{
				return std::any_of(rows.begin(), rows.end(), [task](const routines::GoalRow& row) { return row.task == task; });
			};
			// Task dt25 (169 PUT_DOWN_CARRIED_DUDE_IF_THEY_CAN_WALK, kinspect dt25 section 3): a finder-less goal - a member
			// carrying a person who can walk puts them down (the plan's 69 PUT_DOWN_OBJECT, our generic dropCarried). The node
			// hook is the gate, so a down or KO body rides on, and so does a hostile who is no captive. A lift or a carry step in
			// flight is the carry machinery's own (its phase-1 wake rule drops a waker), not this goal's.
			if (self->carrying != 0 && !member.has<state::CarryStep>() && !self->dead && !self->unconscious && !self->down
				&& hasRow(routines::task::PutDownCarriedDude))
			{
				const state::Fighter* carried = combat_->find(self->carrying);
				if (carried != nullptr && canBePutDown(ctx, *self, *carried))
				{
					const std::uint64_t dropped = self->carrying;
					dropCarried(memberId);
					std::ostringstream line;
					line << "goal 169 put down " << dropped << " member " << memberId;
					logTask(line.str());
					continue;
				}
			}
			bodyguardHand(squad, memberId, member, followLeader);
			// A carrier is offered the goal that places what it holds: 106 for a person, 281 for a corpse (138's lift ends here, so
			// 281 is its hand-off). With no such row in the package the carrier must not sit with the body for ever (no layer may
			// reach a terminal state): it puts it down (a corpse at once, a person after 30 s), and the 20 s block keeps this member from lifting
			// it again at once.
			const state::Fighter* held = self->carrying != 0 ? combat_->find(self->carrying) : nullptr;
			const bool canPlace = held != nullptr
				&& (hasRow(held->dead ? routines::task::FindCageAndPutDeadGuyIn : routines::task::FindBedAndPutIn)
					|| (held->dead && hasRow(routines::task::DeadGuysInThePot)));
			// A corpse is put down at once; a living person after kCarryIdleSeconds (the 169 / KO'd-captive carries ride on for a while).
			// `carryIdleSince_` holds the tick a member's carry began to wait; not saved (a load restarts the wait).
			constexpr double kCarryIdleSeconds = 30.0;
			if (self->carrying == 0 || canPlace)
				carryIdleSince_.erase(memberId);
			const state::Routine* wandering = member.get<state::Routine>();
			const bool reselect = wandering != nullptr && wanderGoal(wandering->goal) && now >= wandering->nextScore && !commitLocked(*wandering, memberId, now);
			if (!freeMember(*combat_, memberId, member, canPlace, graft, !grafted.empty(), reselect))
			{
				if (self->carrying != 0 && !canPlace && arrestOrders_.count(memberId) == 0 && freeMember(*combat_, memberId, member, true))
				{
					const std::uint64_t since = carryIdleSince_.emplace(memberId, now).first->second;
					if (held == nullptr || held->dead || static_cast<double>(ticksSince(now, since)) * kTickSeconds >= kCarryIdleSeconds)
					{
						const std::uint64_t dropped = self->carrying;
						dropCarried(memberId);
						carryIdleSince_.erase(memberId);
						worldTaskBlocks_[{memberId, dropped}] = now + kBlockTicks;
						std::ostringstream line;
						line << "goal carry drop member " << memberId << " body " << dropped << " (no placing row in the package)";
						logTask(line.str());
					}
				}
				continue;
			}
			if (graft)
				graftPassed.push_back(memberId);
			const std::uint16_t heldGoal = reselect ? wandering->goal : 0; // the wander goal this pass re-scores
			if (reselect)
			{
				state::Routine again = *wandering;
				again.nextScore = now + static_cast<std::uint64_t>(std::ceil(kReselectSeconds / kTickSeconds));
				member.set<state::Routine>(std::move(again));
				--onTask[heldGoal]; // not its own crowd
			}
			const auto restoreCrowd = [&]() { if (heldGoal != 0) ++onTask[heldGoal]; };
			std::vector<goalsel::Offer> offers;
			std::vector<Resolved> targets; // parallel to offers
			std::vector<std::vector<std::uint16_t>> plans; // parallel to offers: the GOAP chain of a row that has one (G4)
			std::vector<worldres::Cage> cages;
			bool cagesBuilt = false;
			// The same town floor of 58's distance term (dt24's 0.4): the town circle, else the walled region, else the building.
			const int myTown = combat_->environment().townOf(self->x, self->z);
			const int myWalled = combat_->environment().walledRegion(self->x, self->z);
			const auto sameTown = [this, self, myTown, myWalled](std::uint64_t, const state::Fighter& other)
			{
				if (myTown >= 0 && combat_->environment().townOf(other.x, other.z) == myTown)
					return true;
				if (myWalled >= 0 && combat_->environment().walledRegion(other.x, other.z) == myWalled)
					return true;
				return self->building != 0 && other.building == self->building;
			};
			// 138's body: a dead edible occupant of a built cage of my town, else the nearest unclaimed sensed corpse (also 137's fetch half).
			const auto deadGuy138 = [&]() -> std::uint64_t
			{
				if (!cagesBuilt)
				{
					cages = cagesNear(*self);
					cagesBuilt = true;
				}
				const int myCircle = towncircle::at(townCircles, self->x, self->z);
				std::vector<worldres::DeadGuyCage> caged;
				for (const worldres::Cage& cage : cages)
					if (cage.occupant != 0 && ((myCircle >= 0 && cage.town == myCircle) || cage.myFaction || cage.myHome))
						caged.push_back({cage.occupant, true}); // a world site is always built (UNSURE for a player cage)
				return worldres::findCorpse138(*self, ctx, caged, lookup,
					[&](std::uint64_t target)
					{
						return claimed(target) || worldTaskBlocks_[{memberId, target}] > now;
					}, [](const state::Fighter& body) { return !inedibleRace(body); });
			};
			for (const routines::GoalRow& row : rows)
			{
				if (!worldExecutable(row.task) || !goalsel::windowHolds(row.start, row.end, hour))
					continue;
				if (self->carrying != 0 && row.task != routines::task::FindBedAndPutIn && row.task != routines::task::FindCageAndPutDeadGuyIn
					&& row.task != routines::task::DeadGuysInThePot)
					continue; // a carrier offers only the placing goals (137 only for a corpse, below)
				Resolved res;
				switch (row.task)
				{
				case routines::task::CaptureSlaves:
					// The scorer skips a blocked target (14059e300): a body this member gave up on stays out until the block lapses.
					res.fighter = capture::nearest(memberId, *self, ctx, kBodySight, lookup, [&](std::uint64_t target)
					{
						const auto blocked = worldTaskBlocks_.find({memberId, target});
						return claimed(target) || (blocked != worldTaskBlocks_.end() && blocked->second > now);
					});
					break;
				case routines::task::LootTheDead:
					// OURS (task BO's live run): one member loots; the next body waits for it to finish.
					if (onTask[routines::task::LootTheDead] > 0)
						break;
					res.fighter = worldres::loot177(*self, ctx, lookup, claimed,
						[&](std::uint64_t target, const state::Fighter&)
						{
							const state::Inventory* pack = fighterEntity(target).get<state::Inventory>();
							return pack != nullptr && carriesFunction(combat_->data(), *pack, kItemFunctionFood);
						});
					break;
				case routines::task::LootStorable:
				case routines::task::LootAnimalsJob:
				{
					// 241 (dt24_cage_loot_play section 3; the finder is the 238 family's, dt24_131 section 6): a dead or downed body, not
					// carried, holding a resource my storage has room for (14059acd0 asks the buildings; ours: a type one of my boxes holds
					// or one of my machines makes - UNSURE). One looter at a time, as 177's.
					if (onTask[row.task] > 0)
						break;
					std::string mySid;
					if (const state::Allegiance* mine = member.get<state::Allegiance>())
						mySid = mine->factionSid;
					std::set<std::string> storable;
					if (!mySid.empty())
					{
						world_.each([&](flecs::entity entity, const state::Container& container)
						{
							const state::Inventory* stock = entity.get<state::Inventory>();
							if (container.ownerSid != mySid || container.locked || stock == nullptr)
								return;
							for (const state::Inventory::Entry& entry : stock->items)
								if (entry.slot == state::Inventory::kCarried)
									storable.insert(entry.itemSid);
						});
						world_.each([&](const state::Producer& producer)
						{
							if (producer.ownerSid == mySid)
								storable.insert(producer.output);
						});
					}
					if (storable.empty())
						break;
					const auto holdsStorable = [&](std::uint64_t target, const state::Fighter&)
					{
						const state::Inventory* pack = fighterEntity(target).get<state::Inventory>();
						if (pack == nullptr)
							return false;
						for (const state::Inventory::Entry& entry : pack->items)
						{
							const ItemInfo* info = entry.slot == state::Inventory::kCarried && entry.count > 0 ? combat_->data().item(entry.itemSid) : nullptr;
							if (info != nullptr && info->function == 0 && storable.count(entry.itemSid) != 0)
								return true;
						}
						return false;
					};
					res.fighter = row.task == routines::task::LootAnimalsJob ? worldres::animalLoot238(*self, ctx, lookup, claimed, holdsStorable)
						: worldres::loot177(*self, ctx, lookup, claimed, holdsStorable);
					break;
				}
				case routines::task::NotBeUnarmed:
				{
					// 42 DON'T BE UNARMED (dt25 section 2): the goal is already met while armed - hasWeapon counts the hands,
					// the back and the pack - so a plan exists only when disarmed; an animal's natural weapon is its weapon and
					// an animal never searches at all (dt25 section 2.2; the coordinator's pin). The finder's hand is where the
					// weapon is: a loose one on the ground, else an armed down or dead body.
					if (armed42(*gameData_, *combat_, *self, member.get<state::Inventory>()))
						break;
					const WeaponSource source = findWeapon42(ecs, *combat_, *gameData_, memberId, *self);
					res.item = source.item;
					res.fighter = source.body;
					break;
				}
				case routines::task::FindAndRescueLeader:
				{
					// 142 (dt24_142 section 2): the platoon's leader (ours: the first Leader-role member, else the first member) while it
					// is down - isLeaderDown is the registry's precondition - and not dead, not me, not already carried.
					std::uint64_t leaderId = 0;
					for (const auto& [otherId, other] : list)
					{
						const state::MemberRole* role = other.get<state::MemberRole>();
						if (role != nullptr && role->list == 2)
						{
							leaderId = otherId;
							break;
						}
						leaderId = leaderId != 0 ? leaderId : otherId;
					}
					const state::Fighter* leader = leaderId != 0 && leaderId != memberId ? lookup(leaderId) : nullptr;
					if (leader != nullptr && !leader->dead && (leader->unconscious || leader->down) && leader->carriedBy == 0 && leader->cageBuilding == 0
						&& !claimed(leaderId) && worldTaskBlocks_[{memberId, leaderId}] <= now)
						res.fighter = leaderId;
					break;
				}
				case routines::task::FindAndRescueIfBeds:
				case routines::task::FindAndRescue: // 105's finder 140009697 is 148's 1408cd550 (ai_enum_map)
				{
					// 148 (dt24_142 section 3; finder 1408cd550 -> 1408cd2d0 mask 0x4010): the nearest downed ally. The mask's dead bit (a
					// rescue of a corpse) and the bed requirement (the plan's, not the finder's) are not modelled (UNSURE).
					double best = 0.0;
					for (const CombatArena::Member& other : members)
					{
						const state::Fighter& o = other.fighter;
						if (other.id == memberId || o.dead || !(o.unconscious || o.down) || o.carriedBy != 0 || o.cageBuilding != 0 || claimed(other.id)
							|| (o.faction != self->faction && !combat_->bonded(self->faction, o.faction)) || worldTaskBlocks_[{memberId, other.id}] > now)
							continue;
						const double d = std::hypot(o.x - self->x, o.z - self->z);
						if (d <= kRescueSight && (res.fighter == 0 || d < best))
						{
							best = d;
							res.fighter = other.id;
						}
					}
					break;
				}
				case routines::task::FollowSquadLeader:
				case routines::task::FollowSlaveMaster:
				case routines::task::FollowUrgentEscape:
				{
					// 141 / 171 (dt24_141): one class, the captor hand then the platoon leader. The goal has no resolver utility in the
					// data we hold, so it is offered at the lowest floor: it is the fall-through the squad's follow always was (UNSURE).
					std::vector<std::uint64_t> platoon;
					for (const auto& [otherId, other] : list)
						platoon.push_back(otherId);
					// A grafted slave's hand is its captor (rwh.99), the squad's leader only the platoon fallback of the exe's rule.
					res.fighter = worldres::follow141(*self, memberId, lookup, graft && slaveCaptor(memberId) != 0 ? slaveCaptor(memberId) : followLeader, platoon);
					res.score = kFollowUtility;
					// 253 FOLLOW_URGENT_ESCAPE (dt24_207): the same follow, pre isTargetDown = 0 - the target is not down (a dead or
					// KO'd leader is not followed; ours reads the 141 resolver's target as the hand).
					if (row.task == routines::task::FollowUrgentEscape && res.fighter != 0)
					{
						const state::Fighter* target = lookup(res.fighter);
						if (target == nullptr || target->dead || target->unconscious || target->down)
							res.fighter = 0;
					}
					break;
				}
				case routines::task::PatrolTown:
					// 36 PATROL_TOWN (dt24_patrol_town_pot section 1): the goal walks at the platoon's TOWN, so atDestinationTown -
					// the package town is my current town - asks the member to have a home town and stand in it. With no town target
					// the exe's test degrades to a proximity one with an empty hand, which is false in practice: the goal is refused.
					res.point = hasPatrolTown && towncircle::at(townCircles, self->x, self->z) >= 0
						&& towncircle::at(townCircles, self->x, self->z) == towncircle::at(townCircles, patrolTown.x, patrolTown.z);
					break;
				case routines::task::Patrol:
					// 22 PATROL (dt24_patrol_town_pot section 2): the move order's options block is the platoon's (a Zone pointer, not a
					// route); the zone-sector walk it runs is not modelled (UNSURE). The executor draws a point around the member.
					res.point = true;
					break;
				case routines::task::RunAroundTown:
				{
					// 216 RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE (dt24_36 section 3): the building to search. The registry's
					// precondition is a weapon in hand; then the resolver picks, per character (OURS, the user's ruling).
					const state::Inventory* pack = member.get<state::Inventory>();
					bool armed = false;
					if (pack != nullptr)
						for (const state::Inventory::Entry& entry : pack->items)
							armed = armed || entry.slot == state::Inventory::kWeapon;
					if (!armed)
						break;
					std::vector<state::RoutineSearch> marks; // the squad's memory: every member's marks
					for (const auto& [otherId, other] : list)
						if (const state::Routine* otherRoutine = other.get<state::Routine>(); otherRoutine != nullptr)
							marks.insert(marks.end(), otherRoutine->searched.begin(), otherRoutine->searched.end());
					const std::uint64_t diceKey = self->diceSeed != 0 ? self->diceSeed : memberId;
					res.building = worldres::runAroundTown216(searchBuildings(*self), marks, seed_, diceKey, now, worldres::kSearchMemoryTicks);
					break;
				}
				case routines::task::LockAllMyDoors:
				case routines::task::LockDoorsStayOutside:
				case routines::task::OpenUpShopDoors:
				{
					// The door rows' shared resolver (the town scorer builds the same context); the home is the member's home building.
					if (self->homeBuilding == 0)
						break;
					const state::SquadRoutine* routine = squad.get<state::SquadRoutine>();
					res.door = resolveDoorRow(memberId, row.task, homeDoorsOf(self->homeBuilding, self->x, self->z),
						routine != nullptr && routine->homeLocked != 0, strangerIn(self->homeBuilding));
					break;
				}
				case routines::task::WanderTown:
				case routines::task::StandAtGeneralNode:
				case routines::task::StandAtNode:
				{
					// 37 / 47 / 51 (rwh.55 section 4): the node DB's mode-1 query on the home building (flag 1: nodes the building owns),
					// a free node or the one I hold, then the plan's own test (a 51 plans its walk, 67, in front). UNSURE: 37's district
					// building (1409fc6c0) is not ported, the home building stands in; 51 takes 47's node (its atNode resolver is shared).
					if (self->homeBuilding == 0)
						break;
					const state::Routine* mine = member.get<state::Routine>();
					const state::Routine none;
					RngStream dice(streamSeed(streamSeed(streamSeed(seed_, kRoutineStream), static_cast<std::size_t>(memberId)),
						static_cast<std::size_t>(now + 1)));
					dice.uniform01();
					const std::optional<SiteChoice> pick = pickSite(mine != nullptr ? *mine : none, memberId, row.task, self->homeBuilding,
						routines::SiteKind::GeneralNode, dice.uniform01(), now, self->x, self->z, std::nullopt, true);
					if (pick && planTownGoal(memberId, row.task, false).found())
					{
						res.slot = true;
						res.site = pick->site;
						res.px = pick->x;
						res.pz = pick->z;
					}
					break;
				}
				case routines::task::Shopkeeper:
				case routines::task::SitOnThrone:
				{
					// 20 / 255: the scorer's finder (pickSite) and plan test on the home building's counter / throne slots, 255 real
					// pieces only. Live: worldExecutable lists them.
					if (self->homeBuilding == 0)
						break;
					const state::Routine* mine = member.get<state::Routine>();
					const state::Routine none;
					RngStream dice(streamSeed(streamSeed(streamSeed(seed_, kRoutineStream), static_cast<std::size_t>(memberId)),
						static_cast<std::size_t>(now + 1)));
					dice.uniform01();
					const bool counter = row.task == routines::task::Shopkeeper;
					const std::optional<SiteChoice> pick = pickSite(mine != nullptr ? *mine : none, memberId, row.task, self->homeBuilding,
						counter ? routines::SiteKind::Counter : routines::SiteKind::Throne, dice.uniform01(), now, self->x, self->z,
						std::nullopt, !counter);
					if (pick && planTownGoal(memberId, counter ? goap::kStayAtShopkeeperNode : row.task, false).found())
					{
						res.slot = true;
						res.site = pick->site;
						res.px = pick->x;
						res.pz = pick->z;
					}
					break;
				}
				case routines::task::FirstAid:
				{
					// 58 JOB_MEDIC (dt24_58.md, dt16 section 6): the most injured ally - or my faction's slave - the member's kits
					// reach, by injury x distance, the member itself scored too. The resolver's score is the goal's utility (not the
					// switch's flat 1.0). `healsStrangers` is the exe's data flag we do not carry: pass 2 stays off (OURS).
					const worldres::MedicPick pick = worldres::medic58(memberId, *self, ctx, lookup, claimed,
						[&](std::uint64_t target) { return worldTaskBlocks_[{memberId, target}] > now; },
						[&](std::uint64_t who, std::int32_t function) { return carriesKit(who, function); }, sameTown,
						false, board != nullptr ? std::span<const std::uint64_t>(board->owned) : std::span<const std::uint64_t>());
					res.fighter = pick.target;
					res.score = pick.score;
					break;
				}
				case routines::task::PutLootInStorage:
					// 184 (dt24_184): the storage box the member's excess loot goes to; one resolver for the residents too (lootStorage184).
					res.stock = lootStorage184(memberId);
					break;
				case routines::task::DeliverResources:
				case routines::task::FillMachine:
				case routines::task::GetRidOfResources:
				case routines::task::OperateStorage:
				case routines::task::DitchAllResources:
				{
					// The push half (ds/machines; dt24_plantasks section 4, dt24_113): one of my storage boxes that can take a resource the
					// member carries, the nearest within the machine picker's reach. A resource is an ITEM record with no function (the
					// exe's own test is not decoded: UNSURE); the member must carry one. OURS: the exe's 93 / 124 name a machine's input
					// and 88 / 114 / 242 have no finder, so every one uses my faction's boxes.
					std::string mySid;
					if (const state::Allegiance* mine = member.get<state::Allegiance>())
						mySid = mine->factionSid;
					const state::Inventory* packed = member.get<state::Inventory>();
					std::vector<std::string> carried;
					if (packed != nullptr)
						for (const state::Inventory::Entry& entry : packed->items)
						{
							const ItemInfo* info = entry.slot == state::Inventory::kCarried && entry.count > 0 ? combat_->data().item(entry.itemSid) : nullptr;
							if (info != nullptr && info->function == 0)
								carried.push_back(entry.itemSid);
						}
					if (mySid.empty() || carried.empty())
						break;
					const grid::Rules rules(*gameData_);
					std::vector<worldres::StockBuilding> boxes;
					world_.each([&](flecs::entity entity, const state::Id& buildingId, const state::Container& container)
					{
						const state::Inventory* stock = entity.get<state::Inventory>();
						if (container.ownerSid != mySid || container.locked || stock == nullptr)
							return;
						for (const std::string& item : carried)
						{
							state::Inventory probe = *stock;
							state::Inventory::Entry one;
							one.itemSid = item;
							one.count = 1;
							if (grid::autoplace(probe, rules, loose(one)) == 0)
							{
								boxes.push_back({buildingId.value, container.x, container.z, container.useX, container.useZ, true, {item}});
								return;
							}
						}
					});
					res.stock = worldres::container284(boxes, *self, worldres::kStockReach, nullptr);
					break;
				}
				case routines::task::EmptyMachine:
				case routines::task::BuildMats:
				case routines::task::LootContainer:
				case routines::task::EmptyMachineOutputs: // 113 and 152 share 1405a70a0, the machine picker (dt24_113 section 4)
				case routines::task::KeepEverythingRunning: // 89 / 90 / 91: the same picker over the jammed machines
				case routines::task::UnjamAllMachines:
				case routines::task::UnjamMachine:
				case routines::task::OperateAutomaticMachinery:
				{
					// The machine / storage picker (dt24_113 section 4's nearest-within-reach; dt24_emptymachine_dismantle):
					// 92 / 247 take from one of my machines with stock in its output slot, 284 from one of my storage boxes
					// that holds anything. OURS: the buildings are the materialised producers / containers my faction owns,
					// because the exe's candidate sources (my orders, the package home base) have no data rows here.
					std::string mySid;
					if (const state::Allegiance* mine = member.get<state::Allegiance>())
						mySid = mine->factionSid;
					std::vector<worldres::StockBuilding> buildings;
					if (!mySid.empty())
					{
						const bool containers = row.task == routines::task::LootContainer;
						if (!containers)
							world_.each([&](flecs::entity entity, const state::Id& buildingId, const state::Producer& producer)
							{
								const state::Inventory* stock = entity.get<state::Inventory>();
								if (producer.ownerSid != mySid || stock == nullptr || stock->carried(producer.output) == 0)
									return;
								if (routines::task::unjams(row.task) && producer.prodState != 1 && !(producer.cap != 0 && stock->carried(producer.output) >= producer.cap))
									return; // 89 / 90 / 91 want a jammed machine: work state 1, output full (dt24_113 section 2)
								if (row.task == routines::task::OperateAutomaticMachinery && producer.maxOperators != 0)
									return; // 152 works the automatic machinery: the drills, which take no operator
								buildings.push_back({buildingId.value, producer.x, producer.z, producer.useX, producer.useZ, true,
									{producer.output}});
							});
						else
							world_.each([&](flecs::entity entity, const state::Id& buildingId, const state::Container& container)
							{
								const state::Inventory* stock = entity.get<state::Inventory>();
								if (container.ownerSid != mySid || container.locked || stock == nullptr)
									return;
								worldres::StockBuilding building{buildingId.value, container.x, container.z, container.useX,
									container.useZ, true, {}};
								for (const state::Inventory::Entry& entry : stock->items)
									if (entry.slot == state::Inventory::kCarried && entry.count > 0
										&& std::find(building.types.begin(), building.types.end(), entry.itemSid) == building.types.end())
										building.types.push_back(entry.itemSid);
								if (!building.types.empty())
									buildings.push_back(std::move(building));
							});
					}
					res.stock = row.task == routines::task::LootContainer ? worldres::container284(buildings, *self, worldres::kStockReach, nullptr)
																		  : worldres::machine92(buildings, *self, worldres::kStockReach, nullptr);
					break;
				}
				case routines::task::LootFoodAndStuff: // 131: 243's finder (1408cb650) over the same food boxes
				case routines::task::AcquireFood:
				{
					// 243 AQUIRE_FOOD_AT_HOMEBASE (dt24_243): a box of my faction that holds food (ITEM function 3), the exe's
					// BuildingFinder_LootFoodAndStuff with the food mask 0x2614. OURS: my faction's unlocked containers (no package
					// home-base hand exists) and no reach cap (the exe runs the finder over the whole base, then my town).
					std::string mySid;
					if (const state::Allegiance* mine = member.get<state::Allegiance>())
						mySid = mine->factionSid;
					// 131 for a member of a faction that is not the player's (a raider): the exe's accept rule passes any owner then
					// (1408dac20: the building's faction is mine OR mine is not the player faction), over the town the member stands in
					// (the Blackboard's target town is normally unset, rwh.53 section 2.2). OURS: a locked box still needs its pick.
					int raidTown = -1;
					if (row.task == routines::task::LootFoodAndStuff && !mySid.empty() && mySid != playerFactionSid())
						raidTown = towncircle::at(townCircles, self->x, self->z);
					std::vector<worldres::StockBuilding> boxes;
					if (!mySid.empty())
						world_.each([&](flecs::entity entity, const state::Id& buildingId, const state::Container& container)
						{
							const state::Inventory* stock = entity.get<state::Inventory>();
							if (container.locked || stock == nullptr)
								return;
							if (container.ownerSid != mySid)
							{
								if (raidTown < 0)
									return;
								const towncircle::Circle& circle = townCircles[static_cast<std::size_t>(raidTown)];
								if (std::hypot(container.x - circle.x, container.z - circle.z) > circle.radius)
									return;
							}
							worldres::StockBuilding building{buildingId.value, container.x, container.z, container.useX, container.useZ,
								true, {}};
							for (const state::Inventory::Entry& entry : stock->items)
							{
								if (entry.slot != state::Inventory::kCarried || entry.count == 0
									|| std::find(building.types.begin(), building.types.end(), entry.itemSid) != building.types.end())
									continue;
								const ItemInfo* info = combat_->data().item(entry.itemSid);
								if (info != nullptr && info->function == kItemFunctionFood)
									building.types.push_back(entry.itemSid);
							}
							if (!building.types.empty())
								boxes.push_back(std::move(building));
						});
					res.stock = worldres::food243(boxes, *self, nullptr);
					break;
				}
				case routines::task::PickupIntrudersTown:
				{
					// 273 (dt24_273 section 2): the same town and ownership tests and body scan as the town scorer's; "ours" is also true
					// for an occupying squad (the occupancy map). The body is the target; stepIntruders plans 273 -> 68 and then 274.
					res.fighter = townIntruder273(*self, sensorContext()).value_or(0);
					break;
				}
				case routines::task::SitAround:
				{
					// 275 SIT_AROUND (dt24_275): a building of the town I stand in - the exe's BuildingFinder_SpecialFunction over my
					// town. OURS: the town's interior buildings, the nearest as the seat (see worldres::sitAround275).
					const int at = towncircle::at(townCircles, self->x, self->z);
					if (at < 0)
						break;
					const towncircle::Circle& circle = townCircles[static_cast<std::size_t>(at)];
					std::vector<worldres::SitBuilding> seats;
					for (const BuildingSite& site : world_.buildingSites())
						if (site.hasInterior && !site.ruined && std::hypot(site.cx - circle.x, site.cz - circle.z) <= circle.radius)
							seats.push_back({site.key, site.cx, site.cz});
					res.building = worldres::sitAround275(seats, *self);
					break;
				}
				case routines::task::SleepOnFloor:
					// 175 SLEEP_ON_FLOOR (dt24_175): no finder - the character sleeps where it stands. The plan's live
					// preconditions (no enemies, not at home, unarmed, empty-handed) decide whether it may.
					res.point = true;
					break;
				case routines::task::EatRandomDeadBody:
					// 230 EAT_A_RANDOM_DEAD_BODY (dt27 section 2.1): the nearest sensed corpse. The exe's package veto (1402690e0) is
					// ours a squadmate's body (UNSURE) and a body this member gave up on (the 20 s set).
					res.fighter = worldres::eatDeadBody230(*self, ctx, lookup,
						[&](std::uint64_t target, const state::Fighter& body)
						{
							return (body.squad != 0 && body.squad == self->squad) || worldTaskBlocks_[{memberId, target}] > now;
						}, kEatSight);
					break;
				case routines::task::EatRandomKoBody:
					// 233 EAT_A_RANDOM_KO_BODY (finder 140039c0c, dt39 section 5): the nearest sensed knocked-out living body, same vetoes as 230.
					res.fighter = worldres::eatKoBody233(*self, ctx, lookup,
						[&](std::uint64_t target, const state::Fighter& body)
						{
							return (body.squad != 0 && body.squad == self->squad) || worldTaskBlocks_[{memberId, target}] > now;
						}, kEatSight);
					break;
				case routines::task::HuntingBloodsmell:
					// 176 HUNTING_BLOODSMELL (dt27 section 4.1): no finder; the registry's preconditions are not crouching and
					// LOCATION_IS_ACCESSIBLE (read as true: UNSURE). Ours offers it only while the platoon's smell exists, where the
					// exe would hold an idle task.
					res.point = board->hasSmell && !self->sneak;
					res.px = board->smellX;
					res.pz = board->smellZ;
					break;
				case routines::task::TravelToTargetTown:
				case routines::task::TravelToTargetTownFast:
				case routines::task::GoToSomewhereForDeliveringSlaves: // 192: the pre list atDestinationTown is its goal, 53 / 130 reach it
				{
					// 53 / 130 (dt27 section 5): pre hasDestinationTown - the squad has a package town - and the goal atDestinationTown,
					// so it is offered while the member stands outside that town.
					towncircle::Circle town;
					res.point = !self->sneak
						&& travelDestination(ecs, *gameData_, instance, *self, town)
						&& std::hypot(self->x - town.x, self->z - town.z) > town.radius;
					res.px = town.x;
					res.pz = town.z;
					break;
				}
				case routines::task::MakeAnnouncement:
				case routines::task::MakeAnnouncementFast:
				{
					// 103 / 129 (dt24_smalltasks section 5, registry rows): pre isLeaderDown = 0 and atPackageTarget = 1, effect
					// isMessageDelivered. Ours: the squad's leader (the first Leader-role member, else the first member; as 142's)
					// is up, the member stands inside the package target's circle (as 215's, no nest diversion), and it has not made
					// its announcement since it arrived (announced_ clears when it leaves the circle).
					std::uint64_t leaderId = 0;
					for (const auto& [otherId, other] : list)
					{
						const state::MemberRole* role = other.get<state::MemberRole>();
						if (role != nullptr && role->list == 2)
						{
							leaderId = otherId;
							break;
						}
						leaderId = leaderId != 0 ? leaderId : otherId;
					}
					const state::Fighter* leader = leaderId != 0 ? lookup(leaderId) : nullptr;
					towncircle::Circle target;
					const bool atTarget = travelDestination(ecs, *gameData_, instance, *self, target, false)
						&& std::hypot(self->x - target.x, self->z - target.z) <= target.radius;
					if (!atTarget)
						announced_.erase(memberId);
					res.point = atTarget && leader != nullptr && !leader->dead && !leader->down && !leader->unconscious
						&& announced_.count(memberId) == 0;
					res.px = self->x;
					res.pz = self->z;
					break;
				}
				case routines::task::TravelToTargetPackage:
				{
					// 215 (dt24_215): pre not crouching / lying, effect atPackageTarget, no finder; offered while the member stands outside
					// the package's target point (ours the home town's circle, no nest diversion).
					towncircle::Circle target;
					res.point = !self->sneak
						&& travelDestination(ecs, *gameData_, instance, *self, target, false)
						&& std::hypot(self->x - target.x, self->z - target.z) > target.radius;
					res.px = target.x;
					res.pz = target.z;
					break;
				}
				case routines::task::RetreatHometown: // 128 / 127 (dt24_128, dt32): offered while isAtHomeTown (95) is false
				case routines::task::RunAwayHometown:
				{
					// Outside the home town's circle, or with no home town at all (the exe's node is null, 95 false): the walk then
					// wanders to a point near the member (dt32 section 2).
					const HomePoint point = homeTownPoint(ecs, *gameData_, instance, *self, now, homeHash(now) * 31 + memberId);
					res.point = !self->sneak && point.outside;
					res.px = point.x;
					res.pz = point.z;
					break;
				}
				default:
				{
					if (!cagesBuilt)
					{
						cages = cagesNear(*self);
						cagesBuilt = true;
					}
					if (row.task == routines::task::SlaveWatching)
						res.fighter = worldres::slaveWatching183(*self, ctx, lookup, claimed, looks, cages);
					else if (row.task == routines::task::NewSlaveProcessing)
						res.fighter = worldres::newSlave260(cages, lookup);
					else if (row.task == routines::task::CarryWoundedSlaves)
					{
						if (self->carrying == 0)
							res.fighter = worldres::carryWounded168(*self, ctx, lookup, [&](std::uint64_t slave) { return claimed(slave) || aided(slave); });
					}
					else if (row.task == routines::task::GatherSlavesFromCages)
						res.fighter = worldres::gather173(*self, cages);
					else if (row.task == routines::task::GatherSlavesFemaleOrBeast)
						res.fighter = worldres::gatherFemaleOrBeast245(*self, towncircle::at(townCircles, self->x, self->z), cages, lookup);
					else if (row.task == routines::task::WorkTheSlaves)
					{
						// 188 (dt24_131 section 4): a random one of my owned, standing, non-animal characters (the exe's global rand();
						// ours a hash of the member and the pass). "Owned" is ours a slave following me or held by my faction.
						std::string mySid;
						if (const state::Allegiance* mine = member.get<state::Allegiance>())
							mySid = mine->factionSid;
						std::vector<std::uint64_t> owned;
						for (const CombatArena::Member& other : members)
						{
							const state::Fighter& slave = other.fighter;
							if (other.id == memberId || slave.dead || slave.carriedBy != 0 || slave.animal || slave.slaveState != 1
								|| worldTaskBlocks_[{memberId, other.id}] > now)
								continue;
							if (slave.followTarget == memberId || (!mySid.empty() && slave.captorSid == mySid))
								owned.push_back(other.id);
						}
						if (!owned.empty())
							res.fighter = owned[owned.size() == 1 ? 0 : homeHash(now / kSquadIntervalTicks * 0x9e3779b1ull + memberId) % owned.size()];
					}
					else if (row.task == routines::task::GatherSlavesFromCagesExport)
						res.fighter = worldres::gatherExport217(*self, towncircle::at(townCircles, self->x, self->z), cages, lookup);
					else if (row.task == routines::task::GiveAllMySlavesTo || row.task == routines::task::GiveAllMySlavesToImport)
					{
						// 194 / 218 (dt27 section 6.3): the goal's effect is iHaveSomeSlaves = 0, so it is offered while I have slaves - the
						// slaves following me - and its target is the keeper of my town's nearest allied slave-storage building.
						bool haveSlaves = false;
						for (const CombatArena::Member& other : members)
							haveSlaves = haveSlaves || (other.fighter.followTarget == memberId && other.fighter.slaveState == 1 && !other.fighter.dead);
						if (haveSlaves)
							res.fighter = worldres::giveSlaves218(*self, towncircle::at(townCircles, self->x, self->z), slaveStoragesNear(*self));
					}
					else if (row.task == routines::task::LiberatePrisoners)
					{
						// 276 LIBERATE_ALL_THE_PRISONERS (dt24_276): an occupied cage of MY town whose captive is neither mine nor covered
						// by my package. `my town` is the town circle I stand in (ours for the exe's Town object, UNSURE); the protection
						// veto is the same squad (my squad's captive) or a bond (OURS for 1402690e0, UNSURE). The finder's gates (rwh.100): a cage
						// whose owner is allied to me (relation >= 50, Cage::ownerAllied) and a prisoner my squad holds a hate tag on (0x2b0).
						const int myCircle = towncircle::at(townCircles, self->x, self->z);
						res.fighter = worldres::liberate276(*self, myCircle, cages, lookup,
							[&](std::uint64_t, const state::Fighter& occupant)
							{
								return (occupant.squad != 0 && occupant.squad == self->squad)
									|| combat_->bonded(self->faction, occupant.faction);
							},
							[&](std::uint64_t prisoner)
							{
								// The platoon's long-term tag table (platoon+0x98) is the squad's: any member's memory of the prisoner counts.
								std::uint32_t tags = 0;
								if (dialogue_ != nullptr)
									for (const CombatArena::Member& mate : members)
										if (mate.fighter.squad == self->squad && dialogue_->memory(mate.id, prisoner, tags) && (tags & kLongTermHateTags) != 0)
											return true;
								return false;
							});
					}
					else if (routines::task::eatsCageOccupant(row.task))
					{
						// 139 (dt29_fog_kidnap section 6; finder 1408d0d90, the sibling of 135's): the package cage hand, else the ranked cage
						// pick, here with a live occupant (no weapon gate: 220 sheathes). Inedible races are refused as in 135.
						const worldres::KillCagePick pick = worldres::killCage135(*self, cages, board->stickyCageBuilding,
							board->stickyCageSlot, lookup, claimed, [](const state::Fighter& occupant) { return !inedibleRace(occupant); });
						board->stickyCageBuilding = pick.occupant != 0 ? pick.building : 0;
						board->stickyCageSlot = pick.occupant != 0 ? pick.slot : 0;
						res.fighter = pick.occupant;
					}
					else if (row.task == routines::task::FindCorpses)
					{
						// 138 FIND_A_DEAD_GUY (dt30 + its correction): the lift is the plan's 68. "My area" is ours the town circle I stand
						// in, or a cage of my faction or home (UNSURE: the exe's ownerships query).
						res.fighter = deadGuy138();
					}
					else if (row.task == routines::task::DeadGuysInThePot)
					{
						// 137 DEAD_GUYS_GO_IN_THE_POT (dt39 section 4; dt24_patrol_town_pot section 3), the world squad's: the machine is
						// 1408d1090's argmax over the disposal machines (function 11) - ours those of my faction, or with no owner, within
						// 4608 u (the finder's own range term; the town filter of the resident scorer is not applied, UNSURE) - and the
						// body is 138's pick, or the corpse already in the arms. carry.cpp's stepCorpsePot runs lift, walk and feed (136).
						const state::Fighter* carried = self->carrying != 0 ? combat_->find(self->carrying) : nullptr;
						if (self->carrying == 0 || (carried != nullptr && carried->dead))
						{
							const state::Allegiance* allegiance = member.get<state::Allegiance>();
							const std::string mySid = allegiance != nullptr ? allegiance->factionSid : std::string();
							std::vector<worldres::Pot> pots;
							for (const DisposalSite& site : world_.disposalSites())
							{
								if (std::hypot(site.x - self->x, site.z - self->z) > 4608.0 || (!site.ownerFaction.empty() && site.ownerFaction != mySid))
									continue;
								worldres::Pot pot;
								pot.building = site.key;
								pot.x = site.x;
								pot.z = site.z;
								pot.myFaction = !mySid.empty() && site.ownerFaction == mySid;
								pot.myHome = self->homeBuilding != 0 && self->homeBuilding == site.key;
								pots.push_back(pot);
							}
							std::uint64_t potKey = 0;
							const std::uint64_t corpse = self->carrying != 0 ? self->carrying : deadGuy138();
							if (corpse != 0 && worldres::findPot137(*self, self->npcClass == crime::kClassLaw, pots, potKey))
							{
								res.fighter = corpse;
								res.pot = potKey;
							}
						}
					}
					else if (row.task == routines::task::GetOutOfCageEscape || row.task == routines::task::GetOutOfCageOpportunistic)
					{
						// 207 / 256 (dt24_207): a member in a cage whose door is not locked (pre isDoorLocked = 0). The goal has no finder;
						// the plan's effect is I_AM_IMPRISONED = 0 and the executor is the shared release action.
						res.point = self->cageBuilding != 0; // a locked door is the plan's (76 / 285 in front), or no plan: planSlaveChain refuses it
					}
					else if (row.task == routines::task::FogmanWorshipVictim)
					{
						// 237 (dt24_237 section 2): the nearest cage of my faction's town whose occupant carries the "being fed on" mark. The
						// package's own target hand is not modelled (ours has none: UNSURE). No marked victim, no goal.
						double best = 0.0;
						for (const worldres::Cage& cage : cages)
						{
							const auto marked = cage.occupant != 0 ? eatenUntil_.find(cage.occupant) : eatenUntil_.end();
							const state::Fighter* occupant = marked != eatenUntil_.end() && marked->second > now ? lookup(cage.occupant) : nullptr;
							if (occupant == nullptr || occupant->dead || (!cage.myFaction && !cage.myHome))
								continue;
							const double d = std::hypot(cage.x - self->x, cage.z - self->z);
							if (res.fighter == 0 || d < best)
							{
								best = d;
								res.fighter = cage.occupant;
							}
						}
					}
					else if (routines::task::killsCageOccupant(row.task))
					{
						// 135 KILL_A_RANDOM_CAGE_OCCUPANT (dt27 section 3; 196 BEAT_CAGE_OCCUPANT runs the same class and takes the same pick, UNSURE: its
						// finder is plan-driven in the exe, dt24_killprisoner section 1): 134's precondition is a weapon in hand, and an unarmed
						// member would plan 42 first (UNSURE: ours does not offer the goal until the 42 row has armed it).
						if (self->weapon.sid != combat_->data().unarmed.sid)
						{
							const worldres::KillCagePick pick = worldres::killCage135(*self, cages, board->stickyCageBuilding,
								board->stickyCageSlot, lookup, claimed, [](const state::Fighter& occupant) { return !inedibleRace(occupant); });
							board->stickyCageBuilding = pick.occupant != 0 ? pick.building : 0;
							board->stickyCageSlot = pick.occupant != 0 ? pick.slot : 0;
							res.fighter = pick.occupant;
						}
					}
					else if (row.task == routines::task::AssaultFortifications)
					{
						// 270 ASSAULT_FORTIFICATIONS_PREFER_GATES (dt24_270): my town's gates first (the shared assault resolver's gate
						// preference: goal 122's 1405a28e0, dt24_84 section 4), then the gate I already hold; the wall-smash half is
						// not modelled (UNSURE, no world building damage). The held gate is the Routine's target of a running 270 - the
						// selector only runs for a free member, so it is the last pick's residue: none (the row re-scores).
						// Only a gate that still refuses my class is a fortification in the way: the goal's effect DESTINATION_NOT_BLOCKED_BY_
						// FORTIFICATIONS already holds for an open one, and the executor ends the task on it at once, so offering it re-picked
						// 270 every second and starved the loot row below it (OURS, kenshi-h2v.21.6; the exe's shared resolver is undecoded).
						const DoorSystem* doors = ai_ != nullptr ? ai_->doorSystem() : nullptr;
						if (doors != nullptr && !doors->tuning().enabled)
							doors = nullptr; // --no-doors: no lock to read
						std::vector<worldres::Fortification> shut;
						for (const worldres::Fortification& gate : fortifications)
							if (doors == nullptr || doors->bashFor(self->faction, gate.door).key != 0)
								shut.push_back(gate); // no door system: every gate is a fortification, as before 5d437988
						const int myCircle = towncircle::at(townCircles, self->x, self->z);
						res.gate = worldres::assaultFortifications270(*self, myCircle, shut, 0);
					}
					else if (row.task == routines::task::FinalKidnapperCageJob)
					{
						// 254 FINAL_KIDNAPPER_CAGE_JOB (dt26 section 0): the hook is a hard stop - 0 while I carry or two platoon
						// members carry (one victim per platoon); otherwise the 132 finder's nearest downed non-ally.
						int carriers = 0;
						for (const auto& [otherId, other] : list)
							if (const state::Fighter* otherFighter = combat_->find(otherId);
								otherFighter != nullptr && otherFighter->carrying != 0)
								++carriers;
						if (self->carrying == 0 && carriers < 2)
							res.fighter = worldres::kidnap132(*self, ctx, lookup, claimed);
					}
					else if (row.task == routines::task::SlaveGetInCage)
					{
						// 172 SLAVE_GET_IN_MY_MASTERS_CAGE (dt24_172 section 1): a free cage of the captor's faction; the entry itself is
						// the plan's 107 USE_CAGE, and the executor puts the slave in at the cage's spot.
						routines::Site site;
						if (worldres::slaveCage172(*self, cages, site))
						{
							res.site = site;
							res.cage = true;
						}
					}
					else if (row.task == routines::task::FindBedAndPutIn)
					{
						// 106 FIND_BED_AND_PUT_IN (dt24_106 + dt29): the carried man into a bed the shared bed resolver names. Like 112 it
						// plans only while a live man is carried; there is no utility ladder in the exe's row, so the flat 1.0 stands.
						routines::Site site;
						const state::Fighter* body = combat_->find(self->carrying);
						if (body != nullptr && !body->dead && bedFor106(*self, site))
						{
							res.site = site;
							res.cage = true;
						}
					}
					else if (row.task == routines::task::FindCageAndPutIn || row.task == routines::task::FindCageAndPutDeadGuyIn)
					{
						// Only while carrying a man (the goal's isCarriedDudeDead precondition, dt15 section 4: a corpse never plans).
						// The cage is the resolver's pick; whether the payload is worth a cage is the utility's (dt24_112 section 3).
						routines::Site site;
						const state::Fighter* body = combat_->find(self->carrying);
						if (body != nullptr && (row.task != routines::task::FindCageAndPutDeadGuyIn || body->dead)
							&& worldres::findCage112(*self, cages, site))
						{
							const int townIndex = towncircle::at(townCircles, self->x, self->z);
							// 140599600: law, or military of the town I stand in (my home town is this one).
							bool lawMilitary = self->npcClass == crime::kClassLaw;
							if (!lawMilitary && self->npcClass == crime::kClassMilitary && townIndex >= 0 && instance.homeTown.id != 0
								&& townIds[static_cast<std::size_t>(townIndex)] == instance.homeTown.id)
								lawMilitary = true;
							const double utility = worldres::putInCage112(*body, townIndex >= 0, lawMilitary,
								ctx.relation(body->faction, self->faction) >= 1.0, alliedTo(memberId, self->carrying));
							if (utility > 0.0)
							{
								res.site = site;
								res.cage = true;
								res.score = utility;
							}
						}
					}
					break;
				}
				}
				if (!res.any())
					continue;
				// G4 (task CB, kinspect dt18): a row whose goal has GOAP preconditions plans its chain against the live world. No plan =
				// the goal scores 0 this pass (the next pass re-plans it): never a terminal state, and no hand-written fallback.
				std::vector<std::uint16_t> plan;
				if (row.task == routines::task::CaptureSlaves)
				{
					const goap::Plan chain = planSlaveChain(memberId, row.task, res.fighter, nullptr);
					if (!chain.found())
						continue;
					for (const goap::Step& step : chain.steps)
						plan.push_back(step.task);
				} else if (row.task == routines::task::FindCageAndPutIn || row.task == routines::task::FindBedAndPutIn
					|| row.task == routines::task::FindCageAndPutDeadGuyIn) {
					const goap::Plan chain = planSlaveChain(memberId, row.task, self->carrying, &res.site);
					if (!chain.found())
						continue;
					for (const goap::Step& step : chain.steps)
						plan.push_back(step.task);
				} else if (row.task == routines::task::GetOutOfCageEscape || row.task == routines::task::GetOutOfCageOpportunistic) {
					const goap::Plan chain = planSlaveChain(memberId, row.task, 0, nullptr);
					if (!chain.found())
						continue;
					for (const goap::Step& step : chain.steps)
						plan.push_back(step.task);
				} else if (row.task == routines::task::SlaveGetInCage) {
					// 172's chain over the slave itself: [67 Running, 107 USE_CAGE, 172] (107 is I_AM_IMPRISONED's only writer).
					const goap::Plan chain = planSlaveChain(memberId, row.task, 0, &res.site);
					if (!chain.found())
						continue;
					for (const goap::Step& step : chain.steps)
						plan.push_back(step.task);
				} else if (row.task == routines::task::FinalKidnapperCageJob) {
					// 254's chain over the victim: [67 Running, 68 LIFT_PERSON, 132 FIND_AND_KIDNAP, 254] (dt26 section 2). The goal
					// is a finder-less terminal marker - its two FINAL atoms are the test; the lift is the action this pass runs, and
					// the package's own 112 row (carry.cpp) takes the body to a cage once it is in the member's arms.
					const goap::Plan chain = planSlaveChain(memberId, row.task, res.fighter, nullptr);
					if (!chain.found())
						continue;
					for (const goap::Step& step : chain.steps)
						plan.push_back(step.task);
				} else if (row.task == routines::task::DeadGuysInThePot) {
					// 137's chain: [67, 68, 67, 136] from empty hands, [67, 136] with the body already carried (the town scorer's plan).
					const DisposalSite* machine = world_.disposalSite(res.pot);
					const goap::Plan chain = planCorpsePot(memberId, res.fighter, machine != nullptr ? machine->x : 0.0, machine != nullptr ? machine->z : 0.0);
					if (machine == nullptr || !chain.found())
						continue;
					for (const goap::Step& step : chain.steps)
						plan.push_back(step.task);
				} else if (row.task == routines::task::FindCorpses) {
					// 138's plan [67 Running, 68 LIFT_PERSON, 138] (dt30: the row chains to nothing, +0xc / +0x10 = 0).
					const goap::Plan chain = planCorpsePot(memberId, res.fighter, 0.0, 0.0, row.task);
					if (!chain.found())
						continue;
					for (const goap::Step& step : chain.steps)
						plan.push_back(step.task);
				} else if (row.task == routines::task::FirstAid) {
					// The 58 chain [25, 58]: 25 does the work, the goal entry is the Task_Blank marker (popped, never run). A plan
					// with nothing left after it means the patient needs no aid - the goal already holds, so it scores 0.
					const goap::Plan chain = planMedicChain(memberId, res.fighter);
					for (const goap::Step& step : chain.steps)
						if (const goap::TaskRow* taskRow = goap::rowOf(step.task); taskRow == nullptr || !taskRow->hasGoalState)
							plan.push_back(step.task);
					if (!chain.found() || plan.empty())
						continue;
				} else if (row.task == routines::task::NotBeUnarmed) {
					// 8 FIND_WEAPON -> 6 EQUIP_WEAPON (dt25 sections 2.4-2.5): the exe's rearm loop - 6 is not reached from 42
					// by effect matching, the pair is the game's find-then-equip. The executor below runs the two steps.
					plan = {8, 6};
				} else if (routines::task::unjams(row.task)) {
					// 89 -> 90 -> 91 -> 92 (dt24_113 sections 1-2; dt24_emptymachine_source section 3): the jam atoms are written by 91, a
					// Task_Blank marker, and the only action that clears a full output is 92's pull.
					plan = row.task == routines::task::KeepEverythingRunning ? std::vector<std::uint16_t>{90, 91, routines::task::EmptyMachine}
						: row.task == routines::task::UnjamAllMachines ? std::vector<std::uint16_t>{91, routines::task::EmptyMachine}
						: std::vector<std::uint16_t>{routines::task::EmptyMachine};
				} else if (row.task == routines::task::LootStorable || row.task == routines::task::LootAnimalsJob) {
					plan = {row.task};
				} else if (row.task == routines::task::PutLootInStorage) {
					plan = {row.task}; // the walk and the put-in are the goal's own action (the put-in task's identity is undecoded: UNSURE)
				} else if (routines::task::pullsMachine(row.task)) {
					plan = {routines::task::EmptyMachine}; // 113 / 152 -> 92, the provider of haveSomeResourcesFrom (dt24_113 section 4: one picker)
				} else if (routines::task::storesResources(row.task)) {
					plan = {row.task}; // the push is the goal's own action (Task_FillMachine for 93 / 124; the others UNSURE)
				} else if (row.task == routines::task::WorkTheSlaves) {
					plan = {161}; // -> 161 SEND_DIALOGUE_SIGNAL_WITHOUT_MOVING, the provider of isDialogDelivered (dt24_131 section 1)
				} else if (row.task == routines::task::GatherSlavesFromCagesExport || row.task == routines::task::GatherSlavesFemaleOrBeast) {
					plan = {174}; // 217 and 245 -> 174 GET_SLAVE, the only provider of TARGET_IS_MY_SLAVE (dt27 section 6.2) // 217 -> 174 GET_SLAVE, the only provider of TARGET_IS_MY_SLAVE (dt27 section 6.2)
				} else if (row.task == routines::task::GiveAllMySlavesTo || row.task == routines::task::GiveAllMySlavesToImport) {
					plan = {161}; // -> 161 SEND_DIALOGUE_SIGNAL_WITHOUT_MOVING, the provider of isDialogDelivered (dt27 section 6.3)
				} else if (row.task == routines::task::EatRandomDeadBody || row.task == routines::task::EatRandomKoBody) {
					plan = {220}; // 230 and 233 -> 220 EAT_TARGET_ALIVE (the only provider of TARGET_IS_EATEN), after the walk (dt27 section 2.4)
				} else if (routines::task::rescues(row.task)) {
					plan = {68}; // -> 68 LIFT_PERSON, the provider of IS_CARRYING_TARGET (dt24_142 section 1)
				} else if (routines::task::eatsCageOccupant(row.task)) {
					plan = {220}; // 139 -> 220 EAT_TARGET_ALIVE (dt29_fog_kidnap section 6)
				} else if (row.task == routines::task::FogmanWorshipVictim) {
					plan = {236}; // 237 -> 236 WorshipTarget (dt24_237; task 236 is undecoded)
				} else if (row.task == routines::task::BeatCageOccupant) {
					plan = {196}; // 196 is Task_KillPrisoner itself, goal isTargetDown (dt24_killprisoner section 1)
				} else if (row.task == routines::task::KillRandomCageOccupant) {
					plan = {134}; // 135 -> 134 KILL_CAGE_OCCUPANT (the only provider of MACHINE_OCCUPANT_IS_ALIVE = 0, dt27 section 3)
				} else if (row.task == routines::task::LootFoodAndStuff) {
					// 131's goal HAVE_SOME_LOOT_FROM: [67, 284, 131] while the member holds nothing from the box (284 is the only
					// provider); the goal alone once it does, so it scores 0 as 243's does.
					const goap::Plan chain = planWorkChain(memberId, row.task, res.stock);
					if (!chain.found() || chain.steps.size() < 2)
						continue;
					for (const goap::Step& step : chain.steps)
						plan.push_back(step.task);
				} else if (row.task == routines::task::AcquireFood) {
					// 243 (dt24_243): [67 Running, 244 GRAB_ONE_FOOD, 243] while the member holds no food; [243] alone once it does -
					// the goal already holds, so it scores 0 (as 58's [58] does).
					const goap::Plan chain = planWorkChain(memberId, row.task, res.stock);
					if (!chain.found() || chain.steps.size() < 2)
						continue;
					for (const goap::Step& step : chain.steps)
						plan.push_back(step.task);
				} else if (row.task == routines::task::PickupIntrudersTown) {
					const goap::Plan chain = planIntruder273(memberId, res.fighter);
					if (!chain.found())
						continue;
					for (const goap::Step& step : chain.steps)
						plan.push_back(step.task);
				} else if (row.task == routines::task::SitAround || row.task == routines::task::SleepOnFloor) {
					// 275 (dt24_275) plans [190 AUTO_LABOURING_MINES_PRETEND, 275]; 175 (dt24_175) is terminal: [175] while its
					// preconditions hold, no plan when one does not (never a terminal state - the member simply does not offer it).
					const BuildingSite* site = row.task == routines::task::SitAround ? world_.buildingSite(res.building) : nullptr;
					const goap::Plan chain = planHomebase(memberId, row.task, site != nullptr ? site->cx : self->x,
						site != nullptr ? site->cz : self->z);
					if (!chain.found())
						continue;
					for (const goap::Step& step : chain.steps)
						plan.push_back(step.task);
				}
				// utility: the flat 1.0, except where the resolver scores (58: dt16 section 6; 112: res.score, dt24_112)
				offers.push_back(routines::makeOffer(row.task, row.tier, routines::effectiveWeight(row), res.score,
					res.cage ? routines::siteId(res.site) : (res.stock != 0 ? res.stock : (res.gate != 0 ? res.gate : res.fighter)),
					onTask[row.task], row.start, row.end));
				targets.push_back(res);
				plans.push_back(std::move(plan));
			}
			if (offers.empty())
			{
				restoreCrowd();
				continue;
			}
			std::vector<goalsel::Verdict> verdicts;
			const bool traced = tracing(memberId);
			const goalsel::Choice choice = goalsel::select(offers, {}, hour, traced ? &verdicts : nullptr);
			if (traced)
			{
				trace::Pick pick = trace::makePick(now, memberId, trace::Source::WorldSelector, {}, offers, verdicts);
				pick.result = choice.found ? trace::Result::Chosen : trace::Result::DefaultIdle;
				pick.task = choice.task;
				pick.target = choice.target;
				pick.score = choice.score;
				pick.tier = choice.tier;
				decisionTrace_.record(pick);
			}
			if (!choice.found)
			{
				restoreCrowd();
				continue; // the default task (IDLE): the member keeps the follow and re-scores next second
			}
			const auto picked = std::find_if(offers.begin(), offers.end(), [&choice](const goalsel::Offer& offer)
			{
				return offer.task == choice.task && offer.target == choice.target;
			});
			if (picked == offers.end())
			{
				restoreCrowd();
				continue;
			}
			const auto at = static_cast<std::size_t>(picked - offers.begin());
			const Resolved& res = targets[at];
			if (reselect && (choice.task == heldGoal || choice.task == routines::task::FollowSquadLeader || choice.task == routines::task::FollowSlaveMaster
				|| choice.task == routines::task::FollowUrgentEscape))
			{
				restoreCrowd(); // the wander goal still wins (or only a follow does): it keeps running, re-scored in kReselectSeconds
				continue;
			}
			state::Fighter& f = *combat_->find(memberId);
			if (choice.task == routines::task::FollowSquadLeader || choice.task == routines::task::FollowSlaveMaster
				|| choice.task == routines::task::FollowUrgentEscape)
			{
				// 141 / 171: the follow IS the member's state (Task_FollowLeader has no arrival and ends only by the give-up monitor,
				// which clears followTarget), so no Routine is kept: it re-scores every second, as the IDLE follow always did. The
				// command is refused without a navmesh, as stepWorldSquads' is.
				if (squadQuiet && self->fought)
					sheatheWeapon(memberId);
				if (f.followTarget != res.fighter && navigation_ != nullptr)
				{
					apply(Command::fighterFollow(state::Id{memberId}, state::Id{res.fighter}));
					if (state::Fighter* follower = combat_->find(memberId))
						follower->followSource = state::Fighter::PlanSource::Plan;
					std::ostringstream line;
					line << "goal " << choice.task << " follow member " << memberId << " target " << res.fighter;
					logTask(line.str());
				}
				continue;
			}
			CombatArena::endFollow(f);
			state::Routine routine;
			routine.goal = choice.task;
			routine.since = now;
			routine.nextScore = now;
			if (choice.task == routines::task::FirstAid)
				routine.nextScore = now + static_cast<std::uint64_t>(std::ceil(kAidGiveUpSeconds / kTickSeconds)) + 1; // the town scorer leaves it to the executor
			if (res.cage)
			{
				routine.target = {static_cast<std::uint8_t>(res.site.kind), res.site.object, res.site.slot};
				// The caging chain runs on the person the member carries (the executor's body), as the town scorer's does.
				if (choice.task == routines::task::FindCageAndPutIn || choice.task == routines::task::FindBedAndPutIn
					|| choice.task == routines::task::FindCageAndPutDeadGuyIn)
					routine.chainTarget = self->carrying;
			} else if (res.slot) {
				// 20 / 255: the counter / throne slot the resolver picked, claimed as the town scorer's finder expects.
				routine.target = {static_cast<std::uint8_t>(res.site.kind), res.site.object, res.site.slot};
				routineBook_.take(res.site, memberId, now);
			} else if (res.door) {
				// 155 / 197 / 86: the executor reads the home's doors itself.
			} else if (choice.task == routines::task::SitAround && res.building != 0) {
				// 275's seat: the building the resolver picked; the executor walks its door and holds (dt24_275).
				routine.chainTarget = res.building;
			} else if (res.building != 0) {
				// 216's search: the building the resolver picked; the executor walks its route (task patrol).
				routine.searchBuilding = res.building;
				routine.searchPoint = 0;
				routine.searchLookUntil = 0;
			} else if (res.pot != 0) {
				// 137: the machine (the Disposal claim key stepCorpsePot reads) and the body to carry to it.
				routine.target = {static_cast<std::uint8_t>(routines::SiteKind::Disposal), res.pot, 0};
				routine.chainTarget = res.fighter;
				routine.chainElapsed = -1.0;
			} else if (res.stock != 0) {
				// A pull task: the machine or box the executor empties (92 / 247 / 284).
				routine.chainTarget = res.stock;
			} else if (res.gate != 0) {
				// 270: the gate's door key; the executor walks to it and bashes it (task raid-goals).
				routine.chainTarget = res.gate;
			} else if (res.item != 0) {
				// 42's FIND_WEAPON source: a loose weapon on the ground (the executor walks to it and picks it up).
				routine.chainTarget = res.item;
			} else if (!res.point) {
				routine.chainTarget = res.fighter;
			}
			if (choice.task == routines::task::RetreatHometown || choice.task == routines::task::RunAwayHometown)
			{
				routine.shoppingX = res.px; // 128 / 127's destination (homeTownPoint), held for the goal's life
				routine.shoppingZ = res.pz;
			}
			if (choice.task == routines::task::SlaveWatching || choice.task == routines::task::NewSlaveProcessing || choice.task == routines::task::LootStorable
				|| choice.task == routines::task::LootAnimalsJob)
				routine.chainElapsed = -1.0; // the process task's timer: negative while walking, then counting
			routine.plan = plans[at]; // the GOAP chain (G4); step 0 is its first
			routine.step = 0;
			member.set<state::Routine>(std::move(routine));
			// Live claims: the next member of this pass sees this one's target and task (one catcher per victim, one looter).
			if (!res.cage && res.fighter != 0)
				squadTargets.insert(res.fighter);
			if (choice.task == routines::task::FirstAid)
				medicked.insert(res.fighter);
			++onTask[choice.task];
			{
				const routines::TaskInfo* info = routines::taskInfo(choice.task);
				std::ostringstream line;
				line << "goal " << (info != nullptr ? info->name : "?") << " member " << memberId;
				if (res.cage)
					line << (choice.task == routines::task::FindBedAndPutIn ? " bed " : " cage ") << res.site.object << ":" << res.site.slot;
				else if (res.pot != 0)
					line << " pot " << res.pot << " body " << res.fighter;
				else if (res.stock != 0)
					line << " building " << res.stock;
				else if (res.building != 0)
					line << " building " << res.building;
				else if (res.gate != 0)
					line << " gate " << res.gate;
				else if (res.point && (res.px != 0.0 || res.pz != 0.0))
					line << " at " << res.px << " " << res.pz;
				else
					line << " target " << res.fighter;
				line << " score " << choice.score << " tier " << choice.tier;
				logTask(line.str());
			}
			if (!res.cage && !res.point && res.building == 0 && res.stock == 0 && res.gate == 0 && res.item == 0 && res.pot == 0 && res.fighter != 0
				&& choice.task != routines::task::PickupIntrudersTown)
				walkTo(memberId, *combat_->find(res.fighter)); // a pull task, 270 and 42's pick-up walk in their executors
		}
		for (const std::uint64_t slaveId : graftPassed)
			if (fighterEntity(slaveId).has<state::Routine>())
				graftRoutines_.insert(slaveId);
	};
	forSquadsInSlice(now, stepSquad);
}

blackboard::Board* Simulation::blackboardOf(flecs::entity squad)
{
	const state::Id* id = squad.is_valid() ? squad.get<state::Id>() : nullptr;
	const auto found = id != nullptr ? boards_.find(id->value) : boards_.end();
	return found != boards_.end() ? &found->second : nullptr;
}

// Every squad gets its Board here, in Id order, at fixed points of the tick (and once after a load), never when a reader first asks;
// the map is pruned of squads that are gone. The Boards are not entity components (see boards_), so none of this moves an entity.
void Simulation::ensureBlackboards()
{
	if (gameData_ == nullptr)
		return;
	// The walk below costs every squad, twice a tick: skip it while the squad set is as it was (see squadsDirty_). Ticks are absolute, so
	// the resync lands on the same ticks in a loaded run.
	constexpr std::uint64_t kBoardResyncTicks = 600;
	const std::size_t squadCount = static_cast<std::size_t>(ecsMutable().count<state::SquadInstance>());
	if (!squadsDirty_ && squadCount == boardsSquadCount_ && clockTick() % kBoardResyncTicks != 0)
		return;
	squadsDirty_ = false;
	boardsSquadCount_ = squadCount;
	std::map<std::uint64_t, flecs::entity> squads;
	ecsMutable().query_builder<const state::SquadInstance, const state::Id>().build().each(
		[&](flecs::entity squad, const state::SquadInstance&, const state::Id& id) { squads.emplace(id.value, squad); });
	for (auto it = boards_.begin(); it != boards_.end();)
		it = squads.count(it->first) != 0 ? std::next(it) : boards_.erase(it);
	for (const auto& [id, squad] : squads)
		if (boards_.count(id) == 0)
			createBlackboard(squad);
}

void Simulation::syncBoardStates() const
{
	state::BoardStates states;
	states.entries.reserve(boards_.size());
	for (const auto& [id, board] : boards_)
		states.entries.push_back(blackboard::capture(board, id));
	flecs::world& ecs = const_cast<flecs::world&>(world_.ecs());
	const auto townId = [&](flecs::entity_t town) { const state::Id* id = town != 0 ? ecs.entity(town).get<state::Id>() : nullptr; return id != nullptr ? id->value : std::uint64_t(0); };
	for (state::BoardStates::Entry& entry : states.entries)
		entry.occupyTown = townId(boards_.at(entry.squad).occupyTown);
	states.occupiersBuilt = occupiersBuilt_;
	for (const auto& [town, factions] : occupiers_)
		states.occupiers.push_back({townId(town), factions});
	std::sort(states.occupiers.begin(), states.occupiers.end(), [](const auto& a, const auto& b) { return a.town < b.town; });
	ecs.set<state::BoardStates>(std::move(states));
	if (!arrestOrders_.empty())
	{
		state::ArrestStates arrests;
		for (const auto& [guard, order] : arrestOrders_)
			arrests.entries.push_back({guard, order.victim, order.since, order.lift, order.started, order.snatched, order.gaveUp});
		ecs.set<state::ArrestStates>(std::move(arrests));
	}
	const bool streaming = navigation_ != nullptr && navigation_->streaming() && !navigation_->resident(); // a resident mesh saves no zone list: every load is the whole world
	const bool treesBuilt = los_ != nullptr && los_->loadedRegions() > 0; // line of sight enabled but with no tree built is the same as off: a load need not enable it
	const bool inFlight = streaming || treesBuilt || travelKnown_ || (residentData_ != nullptr && !residentData_->queued.empty());
	if (limbPlay_.empty() && alarms_.empty() && alarmDwell_.empty() && alarms_.last() == 0 && farOrders_.empty() && !inFlight)
		return;
	state::RunStates run;
	// The region promotion's work in flight (v126): what a pass queued, the zones and trees the spread has done so far, the heading.
	if (residentData_ != nullptr)
	{
		for (const auto& [squad, world] : residentData_->queued)
			if (squad.is_alive() && squad.get<state::Id>() != nullptr)
				run.promotions.push_back({squad.get<state::Id>()->value, world});
	}
	run.streaming = streaming;
	if (streaming)
	{
		for (const kworld::ZoneCell zone : navigation_->coveredZones())
		{
			run.navZones.push_back(static_cast<std::uint32_t>(kworld::zoneIndexOf(zone)));
			if (furnishedZones_.count(kworld::zoneIndexOf(zone)) != 0)
				run.navFurnished.push_back(static_cast<std::uint32_t>(kworld::zoneIndexOf(zone)));
		}
	}
	run.hasLos = treesBuilt;
	if (treesBuilt)
	{
		for (const std::int32_t region : los_->loadedRegionList())
			run.losRegions.push_back(static_cast<std::uint32_t>(region));
	}
	run.travelKnown = travelKnown_;
	run.travelX = travelX_;
	run.travelZ = travelZ_;
	run.headX = headX_;
	run.headZ = headZ_;
	run.lastAlarm = alarms_.last();
	for (const auto& [member, play] : limbPlay_)
		run.plays.push_back({member, play.x, play.z, play.picks});
	alarms_.each([&](int town, const alarm::Alarm& a)
	{
		run.alarms.push_back({static_cast<std::uint32_t>(town), a.id, a.kind, a.owner, a.raiser, a.building, a.x, a.z, a.life});
	});
	for (const auto& [member, left] : alarmDwell_)
		run.dwells.push_back({member, left});
	for (const auto& [member, o] : farOrders_)
	{
		state::RunStates::FarOrder& e = run.farOrders.emplace_back();
		e.member = member;
		e.seq = o.seq;
		e.goalX = o.goalX;
		e.goalZ = o.goalZ;
		e.goalY = o.goalY.value_or(0.0);
		e.hasGoalY = o.goalY.has_value();
		for (const kworld::RoadNetwork::Marker& m : o.markers)
			e.markers.push_back({m.x, m.y, m.z, m.width});
		e.next = static_cast<std::uint32_t>(o.next);
		e.cur = static_cast<std::uint32_t>(o.cur);
		e.onMarker = o.onMarker;
		e.done = o.done;
		e.blocked = o.blocked;
		e.blockedUntil = o.blockedUntil;
		e.legs = o.legs;
		e.clamps = o.clamps;
	}
	ecs.set<state::RunStates>(std::move(run));
}

void Simulation::dropBoardStates() const
{
	flecs::world& ecs = const_cast<flecs::world&>(world_.ecs());
	ecs.remove<state::BoardStates>();
	ecs.remove<state::ArrestStates>();
	ecs.remove<state::RunStates>();
}

void Simulation::restoreBoards()
{
	boards_.clear();
	squadsDirty_ = true;
	*squadCaches_ = SquadCaches{};
	arrestOrders_.clear();
	if (const state::ArrestStates* arrests = world_.ecs().get<state::ArrestStates>())
		for (const state::ArrestStates::Entry& e : arrests->entries)
			arrestOrders_[e.guard] = {e.lift, e.victim, e.since, e.started, e.snatched, e.gaveUp};
	limbPlay_.clear();
	alarms_.clear();
	alarmDwell_.clear();
	farOrders_.clear();
	if (const state::RunStates* run = world_.ecs().get<state::RunStates>())
	{
		// run->cooldowns: an older save's 277 cooldown, which the exe does not have (rwh.203): dropped
		for (const state::RunStates::Play& e : run->plays)
			limbPlay_[e.member] = {e.x, e.z, e.picks};
		for (const state::RunStates::Alarm& e : run->alarms)
			alarms_.restore(static_cast<int>(e.town), {e.id, e.kind, e.owner, e.raiser, e.building, e.x, e.z, e.life});
		alarms_.resume(run->lastAlarm);
		for (const state::RunStates::Dwell& e : run->dwells)
			alarmDwell_[e.member] = e.left;
		for (const state::RunStates::FarOrder& e : run->farOrders)
		{
			FarOrder& o = farOrders_[e.member];
			o.seq = e.seq;
			o.goalX = e.goalX;
			o.goalZ = e.goalZ;
			if (e.hasGoalY)
				o.goalY = e.goalY;
			for (const state::RunStates::FarMarker& m : e.markers)
				o.markers.push_back({m.x, m.y, m.z, m.width});
			o.next = e.next;
			o.cur = e.cur;
			o.onMarker = e.onMarker;
			o.done = e.done;
			o.blocked = e.blocked;
			o.blockedUntil = e.blockedUntil;
			o.legs = e.legs;
			o.clamps = e.clamps;
			o.restored = true;
		}
	}
	const state::BoardStates* held = world_.ecs().get<state::BoardStates>();
	const state::BoardStates saved = held != nullptr ? *held : state::BoardStates{}; // the singletons go now: they are not part of a running world
	dropBoardStates();
	std::map<std::uint64_t, flecs::entity> towns;
	ecsMutable().query_builder<const state::TownState, const state::Id>().build().each(
		[&](flecs::entity town, const state::TownState&, const state::Id& id) { towns.emplace(id.value, town); });
	const auto townEntity = [&](std::uint64_t id) { const auto at = towns.find(id); return at != towns.end() ? at->second.id() : flecs::entity_t(0); };
	occupiers_.clear();
	occupiersBuilt_ = saved.occupiersBuilt; // a file before v117 has none saved: false rebuilds it from the boards at the first tick
	for (const state::BoardStates::Occupancy& occupancy : saved.occupiers)
		if (const flecs::entity_t town = townEntity(occupancy.town); town != 0)
			occupiers_[town] = occupancy.factions;
	if (held == nullptr)
		return;
	if (gameData_ == nullptr)
		return;
	std::map<std::uint64_t, flecs::entity> squads;
	ecsMutable().query_builder<const state::SquadInstance, const state::Id>().build().each(
		[&](flecs::entity squad, const state::SquadInstance&, const state::Id& id) { squads.emplace(id.value, squad); });
	for (const state::BoardStates::Entry& entry : saved.entries)
	{
		const auto squad = squads.find(entry.squad);
		if (squad == squads.end())
			continue;
		createBlackboard(squad->second);
		if (const auto board = boards_.find(entry.squad); board != boards_.end())
		{
			blackboard::restore(board->second, entry, *gameData_);
			board->second.occupyTown = townEntity(entry.occupyTown);
		}
	}
}

void Simulation::createBlackboard(flecs::entity squad)
{
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	if (instance == nullptr)
		return;
	// 140271620: the template's `AI packages`, val0 = priority, in data order.
	blackboard::Board board;
	if (const kdata::Record* record = gameData_->find(instance->templateSid))
		if (const kdata::RefCategory* links = record->findRefs("AI packages"))
			for (const kdata::RefEntry& entry : links->entries)
			{
				blackboard::Package package;
				if (blackboard::readPackage(*gameData_, entry.targetID, entry.v0, package))
					blackboard::registerPackage(board, std::move(package));
			}
	// 140271fd0: the faction's `AI fallback`, right after the template, at priority 0.
	if (instance->faction.id != 0)
	{
		const state::FactionState* faction = ecsMutable().entity(instance->faction.id).get<state::FactionState>();
		const kdata::Record* record = faction != nullptr ? gameData_->find(faction->templateSid) : nullptr;
		const kdata::RefCategory* fallback = record != nullptr ? record->findRefs("AI fallback") : nullptr;
		blackboard::Package package;
		if (fallback != nullptr && !fallback->entries.empty()
			&& blackboard::readPackage(*gameData_, fallback->entries.front().targetID, 0, package))
			blackboard::registerPackage(board, std::move(package));
	}
	blackboard::resolveLeads(board);
	boards_.emplace(squad.get<state::Id>()->value, std::move(board));
}

// The list a member's goals come from (dt19 section 2: the role is squad_mem_type with the squad leader forced to role 2 = Leader AI
// Goals; the empty-list fallbacks are the package's, CompiledPackage::readsFrom). The leader is the squad's first member in Id order
// carrying the Leader list (a template `leader` entry), else its first member. UNSURE: the exe's leader pick after a leader dies or
// leaves is untraced; ours stays on the Id so a member's list does not flip mid-run. The squad is scanned once per squad per tick (leaderMemo_).
routines::Role Simulation::liveRole(flecs::entity member) const
{
	const state::MemberRole* role = member.get<state::MemberRole>();
	const routines::Role own = role != nullptr ? static_cast<routines::Role>(std::min<std::uint8_t>(role->list, 3)) : routines::Role::Squad;
	const flecs::entity squad = member.target<state::MemberOf>();
	const state::Id* self = member.get<state::Id>();
	if (!squad.is_valid() || self == nullptr)
		return own;
	if (leaderMemoTick_ != clockTick())
	{
		leaderMemo_.clear();
		leaderMemoTick_ = clockTick();
	}
	auto memo = leaderMemo_.find(squad.id());
	if (memo == leaderMemo_.end())
	{
		std::uint64_t firstLeader = ~std::uint64_t(0);
		std::uint64_t first = ~std::uint64_t(0);
		world_.eachMember(squad, [&](flecs::entity other, const state::Id& id)
		{
			first = std::min(first, id.value);
			const state::MemberRole* otherRole = other.get<state::MemberRole>();
			if (otherRole != nullptr && otherRole->list == state::kRoleLeader)
				firstLeader = std::min(firstLeader, id.value);
		});
		memo = leaderMemo_.emplace(squad.id(), firstLeader != ~std::uint64_t(0) ? firstLeader : first).first;
	}
	const std::uint64_t leader = memo->second;
	return self->value == leader ? routines::Role::Leader : own;
}

flecs::entity_t Simulation::townEntityAt(double x, double z) const
{
	if (gameData_ == nullptr)
		return 0;
	const flecs::world& ecs = world_.ecs();
	const std::size_t count = static_cast<std::size_t>(ecs.count<state::TownState>());
	if (count != townCirclesFor_)
	{
		townCircles_.clear();
		ecs.each([&](flecs::entity town, const state::TownState& state) { townCircles_.emplace_back(towncircle::of(*gameData_, state), town.id()); });
		townCirclesFor_ = count;
	}
	for (const auto& [circle, id] : townCircles_)
		if (circle.radius > 0.0 && std::hypot(x - circle.x, z - circle.z) <= circle.radius)
			return id;
	return 0;
}

const state::TownState* Simulation::townStateAt(double x, double z) const
{
	const flecs::entity_t id = townEntityAt(x, z);
	return id != 0 ? flecs::entity(world_.ecs().c_ptr(), id).get<state::TownState>() : nullptr;
}

blackboard::Situation Simulation::situationOf(flecs::entity squad, const blackboard::Board* board, const std::vector<std::pair<std::uint64_t, flecs::entity>>& members, std::uint64_t nowTick) const
{
	blackboard::Situation out;
	// The slaver shipping classes (0xb / 0x1c) read the platoon's ownership set, not a mission (kinspect rwh.8, correcting dt41 s8).
	out.owned = board != nullptr ? static_cast<int>(board->owned.size()) : 0;
	int engaged = 0;
	const state::Fighter* lead = nullptr;
	const std::vector<const state::Fighter*> living = [&]
	{
		std::vector<const state::Fighter*> list;
		for (const auto& [memberId, member] : members)
			if (const state::Fighter* f = combat_->find(memberId); f != nullptr && !f->dead)
				list.push_back(f);
		return list;
	}();
	for (const state::Fighter* f : living)
	{
		if (lead == nullptr)
			lead = f;
		++out.members;
		// Package_PassingByTownAssault vf13 / vf18 (14028eca0 / 14028ea90, rwh.228 s2): a member counts with vf+0x118 > 0, the zone id (inside the
		// town). M: our zone is the town circle (townEntityAt); a combat target plays no part.
		if (townEntityAt(f->x, f->z) != 0)
			++engaged;
	}
	out.engagedMembers = engaged;
	out.leaderAlive = lead != nullptr;
	if (const auto memo = situations_.find(squad.id()); memo != situations_.end() && nowTick > memo->second.tick)
		out.stepHours = gameHoursIn(ticksSince(nowTick, memo->second.tick));
	if (lead == nullptr)
		return out;
	const state::TownState* town = townStateAt(lead->x, lead->z);
	const bool owned = town != nullptr && town->owner.id != 0;
	// 140290450: a town the platoon stands in whose faction is not allied to it and an enemy of it (relation <= -30, or the platoon attacks neutrals): a caravan in a neutral town never assaults it. Not read: the per-town cooldown and the 10-object query
	// (the cooldown's length +0xb0 is not in the data), so the assault restarts as soon as it finishes. UNSURE.
	// Fighter::faction is the faction's Id value, TownState::owner and the squad's faction the entities. The relation is read from the data
	// (the arena's sets only cover the factions present as fighters).
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	const state::Id* ownerKey = owned ? flecs::entity(world_.ecs().c_ptr(), town->owner.id).get<state::Id>() : nullptr;
	bool hostile = false;
	if (ownerKey != nullptr)
	{
		const flecs::entity mine = instance != nullptr && instance->faction.id != 0 ? flecs::entity(world_.ecs().c_ptr(), instance->faction.id) : flecs::entity();
		const flecs::entity theirs(world_.ecs().c_ptr(), town->owner.id);
		// 140290450 asks only `!allied` of a player-owned town (rwh.53 section 2.4; kenshi-k16.6.66 re-read it): a squad that carries the
		// assault package assaults the player's town it stands in whatever the relation reads; another owner's town still needs an enemy.
		const bool playerOwned = theirs.has<state::PlayerFaction>();
		hostile = blackboard::assaultsTown(combat_->bonded(lead->faction, ownerKey->value), lead->attacksNeutrals, playerOwned,
			!playerOwned && mine.is_valid() && factionsHostile(*gameData_, mine, theirs));
	}
	out.assaultTown = hostile;
	// The assault's target set (rwh.228 s2, +0xd8): hostiles the members sense that stand in a town, are not dead, KO'd, down or bedded.
	// The exe never prunes the set (a gone one fails the field test); ours reads the members' records now, which is the same for the living.
	const blackboard::Package* assaulting = board != nullptr && !board->current.empty() ? blackboard::find(*board, board->current) : nullptr;
	if (assaulting != nullptr && assaulting->signalFunc == blackboard::kPassingByTownAssault)
	{
		for (const state::Fighter* f : living)
		{
			for (const state::SightRecord& record : f->sight)
			{
				const state::Fighter* t = combat_->find(record.target);
				if (t != nullptr && !t->dead && !t->unconscious && !t->down && !t->asleep && combat_->hostileFactions(f->faction, t->faction) && townEntityAt(t->x, t->z) != 0)
					out.targetsGone = false;
			}
		}
	}
	// 140289180: a living, standing member whose current town is the occupied one (the town the package started in, else the leader's now).
	const blackboard::Package* running = board != nullptr && !board->current.empty() ? blackboard::find(*board, board->current) : nullptr;
	if (running != nullptr && running->signalFunc == blackboard::kOccupyTown)
	{
		const flecs::entity_t held = board->occupyTown != 0 ? board->occupyTown : townEntityAt(lead->x, lead->z);
		for (const auto& [memberId, member] : members)
		{
			const state::Fighter* f = combat_->find(memberId);
			if (f != nullptr && !f->dead && !f->down && !f->unconscious && held != 0 && townEntityAt(f->x, f->z) == held)
			{
				out.occupying = true;
				break;
			}
		}
	}
	// 1405944e0: the trader is at its target town while its trip says it is staying; any other platoon is at a town when it stands in one.
	const state::TraderTrip* trip = squad.get<state::TraderTrip>();
	// A trader is at its target town from an arrival after its package started: the stay of an earlier trip is not a new arrival (the original
	// picks a new target when the package starts, so the state machine waits for the next one).
	out.atTown = trip != nullptr ? trip->staying && board != nullptr && trip->arrivedAt >= board->startedAt : town != nullptr;
	out.townOwned = owned && instance != nullptr && instance->faction.id == 0; // the platoon's own record has no owner (1402869a0, UNSURE)
	return out;
}

void Simulation::noteOwnedSlave(std::uint64_t captor, std::uint64_t slave)
{
	const state::Fighter* by = combat_ != nullptr ? combat_->find(captor) : nullptr;
	if (by == nullptr || by->squad == 0)
		return;
	const auto board = boards_.find(by->squad);
	if (board == boards_.end())
		return;
	std::vector<std::uint64_t>& owned = board->second.owned;
	const auto at = std::lower_bound(owned.begin(), owned.end(), slave);
	if (at == owned.end() || *at != slave)
		owned.insert(at, slave);
}

void Simulation::pruneOwned(blackboard::Board& board)
{
	// The set keeps a downed or dropped slave (nothing recovers one, rwh.9); a slave that died, was freed or left the world is no longer an
	// owned character. UNSURE: the exe's set is not cleared by a death (rwh.8 "U"), ours is, so a caravan never stalls on a corpse.
	board.owned.erase(std::remove_if(board.owned.begin(), board.owned.end(), [&](std::uint64_t id)
	{
		const state::Fighter* f = combat_->find(id);
		return f == nullptr || f->dead || f->slaveState != 1;
	}), board.owned.end());
}

std::uint64_t Simulation::findSlaveDepot(flecs::entity squad, const state::Fighter& lead, std::int32_t signalFunc) const
{
	flecs::world& ecs = const_cast<flecs::world&>(world_.ecs());
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	const flecs::entity mine = instance != nullptr && instance->faction.id != 0 ? flecs::entity(ecs.c_ptr(), instance->faction.id) : flecs::entity();
	const auto typeOf = [&](const state::TownState& town)
	{
		const kdata::Record* active = gameData_->find(town.activeSid);
		if (active == nullptr)
			active = gameData_->find(town.rootSid);
		return active != nullptr ? static_cast<int>(active->intOr("type", 2)) : 2;
	};
	// 140927f10 pass 2: owner any, hostile owners skipped. The squared xz distance picks the nearest (town +0x48 / +0x50).
	const auto hostile = [&](const state::TownState& town)
	{
		return mine.is_valid() && town.owner.id != 0 && ecs.is_alive(town.owner.id)
			&& factionsHostile(*gameData_, mine, flecs::entity(ecs.c_ptr(), town.owner.id));
	};
	const auto d2 = [&](const state::TownState& town) { return (town.x - lead.x) * (town.x - lead.x) + (town.z - lead.z) * (town.z - lead.z); };
	std::uint64_t camp = 0;
	double campD2 = 0.0;
	world_.each([&](const state::Id& id, const state::TownState& town)
	{
		if (typeOf(town) != 5 || hostile(town))
			return;
		const double d = d2(town);
		if (camp == 0 || d < campD2)
		{
			camp = id.value;
			campD2 = d;
		}
	});
	if (signalFunc != blackboard::kSlaverAnywhere)
		return camp;
	// The Anywhere class: a TOWN_TOWN with a slave-storage building (designation 10) nearer than sqrt(0.7) x the camp's distance, or at any
	// distance when there is no camp. The designation mask is read off the placed buildings inside the town's circle.
	const double bound = camp != 0 ? campD2 * 0.7 : 1.0e300;
	std::uint64_t storageTown = 0;
	double storageD2 = 0.0;
	world_.each([&](const state::Id& id, const state::TownState& town)
	{
		if (typeOf(town) != 2 || hostile(town))
			return;
		const double d = d2(town);
		if (d >= bound || (storageTown != 0 && d >= storageD2))
			return;
		const towncircle::Circle circle = towncircle::of(*gameData_, town);
		for (const BuildingSite& site : world_.buildingSites())
		{
			if (site.squadTemplate.empty() || std::hypot(site.cx - circle.x, site.cz - circle.z) > circle.radius)
				continue;
			const kdata::Record* record = gameData_->find(site.squadTemplate);
			if (record != nullptr && record->intOr("building designation", 0) == 10)
			{
				storageTown = id.value;
				storageD2 = d;
				return;
			}
		}
	});
	return storageTown != 0 ? storageTown : camp;
}

std::size_t Simulation::giveOwnedSlaves(blackboard::Board& board, std::uint64_t receiver)
{
	// DA_GIVE_TARGET_MY_SLAVES (24, handler 140680560) on the platoon's set: a carried slave is put down first, then each is re-owned by the
	// receiver's side and the set is cleared (which finishes the package). The slave's own faction is left as is (the model keeps the
	// captor beside it: captorSid), and it follows the receiver as every slave follows its owner (OURS).
	const state::Fighter* to = combat_->find(receiver);
	std::string side;
	if (to != nullptr)
		if (const state::Allegiance* allegiance = fighterEntity(receiver).get<state::Allegiance>())
			side = allegiance->factionSid;
	std::size_t given = 0;
	if (side.empty())
		return given;
	for (const std::uint64_t id : board.owned)
	{
		state::Fighter* slave = combat_->find(id);
		if (slave == nullptr || slave->dead)
			continue;
		if (slave->carriedBy != 0)
			dropCarried(slave->carriedBy);
		slave->captorSid = side;
		slave->followTarget = receiver;
		slave->followAge = 0.0;
		slave->clearRoute();
		mirrorFighter(id);
		++given;
	}
	board.owned.clear();
	return given;
}

// The longest the depot walk waits for the captive to be lifted: 0.55 game hours = 60 s.
constexpr double kLiftWaitHours = 0.55;

void Simulation::attachSlaveDepots()
{
	std::map<std::uint64_t, std::uint64_t> depots;
	for (const auto& [id, board] : boards_)
	{
		if (board.depot == 0 || board.owned.empty() || board.current.empty())
			continue;
		const blackboard::Package* running = blackboard::find(board, board.current);
		if (running == nullptr || !blackboard::slaverShipping(running->signalFunc))
			continue;
		// The walk leaves once every owned slave that cannot walk is lifted or follows a captor, or after kLiftWaitHours: the first aid and the
		// lift come first (a squad that walked off at once left its carrier chasing a captive nobody had lifted, k16.6.21 step 2; the exe's
		// first aid takes 21-59 s, rwh.9, and the wait is bounded so no layer stands for ever).
		const bool lifted = trader::gameHours(clockTick()) - board.startedAt >= kLiftWaitHours
			|| std::all_of(board.owned.begin(), board.owned.end(), [&](std::uint64_t slave)
		{
			const state::Fighter* f = combat_->find(slave);
			return f == nullptr || f->dead || (!aided(slave) && ((!f->down && !f->unconscious) || f->carriedBy != 0 || f->followTarget != 0));
		});
		if (lifted)
			depots.emplace(id, board.depot);
	}
	if (ai_ != nullptr)
		ai_->setSlaveDepots(std::move(depots));
}

void Simulation::logDeliveryFailures()
{
	if (ai_ == nullptr)
		return;
	for (const AiSystem::DeliveryFailure& failure : ai_->takeDeliveryFailures())
	{
		std::ostringstream line;
		line << "slaver deliver squad " << failure.squad;
		if (failure.edge == 2)
			line << ": at the mesh edge toward depot town " << failure.depot << ", holding";
		else if (failure.edge == 1)
			line << ": no route to depot town " << failure.depot << " (" << state::aiReasonName(failure.reason)
				<< "); walking to the mesh point nearest it, the full route is retried in " << UtilityGroupLayer::kCooldownHours << " game hours";
		else
			line << ": no route to depot town " << failure.depot << " (" << state::aiReasonName(failure.reason)
				<< "); the squad falls back to its ordinary life, the walk is retried in " << UtilityGroupLayer::kCooldownHours << " game hours";
		logTask(line.str());
	}
}

const routines::CompiledPackage* Simulation::appliedPackage(flecs::entity squad)
{
	blackboard::Board* board = residentData_ != nullptr ? blackboardOf(squad) : nullptr;
	if (board == nullptr)
		return nullptr;
	blackboard::Situation situation; // the selector's last feed, read again without moving the classes' own state
	if (const auto memo = situations_.find(squad.id()); memo != situations_.end())
		situation = memo->second.situation;
	situation.advance = false;
	blackboard::tick(*board, trader::gameHours(clockTick()), blackboard::Campaign{}, situation);
	return board->current.empty() ? nullptr : residentData_->packages.find(board->current);
}

bool Simulation::setSquadSmell(flecs::entity squad, double x, double z)
{
	blackboard::Board* board = blackboardOf(squad);
	if (board == nullptr)
		return false;
	board->hasSmell = true;
	board->smellX = x;
	board->smellZ = z;
	return true;
}

void Simulation::clearSquadSmell(flecs::entity squad)
{
	if (blackboard::Board* board = blackboardOf(squad))
		board->hasSmell = false;
}

void Simulation::stripTargetWeapons(std::uint64_t victim)
{
	state::Fighter* f = combat_ != nullptr ? combat_->find(victim) : nullptr;
	if (f == nullptr || gameData_ == nullptr)
		return;
	flecs::entity entity = fighterEntity(victim);
	const state::Inventory* held = entity.get<state::Inventory>();
	if (held == nullptr)
		return;
	state::Inventory inventory = *held;
	std::vector<state::Inventory::Entry> dropped;
	for (auto it = inventory.items.begin(); it != inventory.items.end();)
	{
		if (it->slot != state::Inventory::kWeapon)
		{
			++it;
			continue;
		}
		dropped.push_back(*it);
		it = inventory.items.erase(it);
	}
	if (dropped.empty())
		return;
	++inventory.version;
	entity.set<state::Inventory>(std::move(inventory));
	f->weapon = combat_->data().unarmed;
	mirrorFighter(victim);
	dropPieces(victim, std::move(dropped));
}

void Simulation::shackleTarget(std::uint64_t victim)
{
	state::Fighter* f = combat_ != nullptr ? combat_->find(victim) : nullptr;
	if (f == nullptr || gameData_ == nullptr)
		return;
	// Prisoner Shackles (ARMOUR, slot 9 = the boots slot; OURS: a lock level of 20 and `is locked` are carried by the record, not modelled).
	const kdata::Record* record = gameData_->find("SHACKLES");
	if (record == nullptr || record->type != kworld::itemTypeByName("ARMOUR"))
		return;
	flecs::entity entity = fighterEntity(victim);
	const state::Inventory* held = entity.get<state::Inventory>();
	if (held == nullptr)
		return;
	state::Inventory inventory = *held;
	for (const state::Inventory::Entry& entry : inventory.items)
	{
		if (entry.itemSid == record->stringID && entry.slot == state::Inventory::kArmourBase + 9)
			return; // already shackled
	}
	state::Inventory::Entry shackles;
	shackles.itemSid = record->stringID;
	shackles.count = 1;
	shackles.quality = CombatData::rollQuality(*gameData_, *record, 0.75, 0.0); // the stock roll (simlink_scenario's)
	// Whatever wore that slot goes back to the pack (wearOnto), and the Fighter's armour numbers follow.
	if (!wearOnto(inventory, std::move(shackles), *record, victim, true).empty())
		return;
	++inventory.version;
	entity.set<state::Inventory>(std::move(inventory));
	mirrorFighter(victim);
}

} // namespace ksim
