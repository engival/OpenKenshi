#pragma once

// The in-town routines, pure part (TOWN_LIFE_SLICE.md section 5, phase T3a): the AI_PACKAGE goal lists compiled into role
// lists, the hour windows, the site index with its synthetic sites, and the reservation book. No Simulation, no flecs, no save:
// everything here is derived from the game data (or from a caller's description of a building) and rebuilt at load. The scorer
// loop (stepRoutines) is T3b2 and uses these.
//
// Every tunable marked "deferred: item 57" waits for kinspect's answer on the exact score formula, the re-evaluation cadence,
// the reservation lifetime and tiredness; none shifts the state model, wire, save layout or threading.

#include "kdata/gamedata.hpp"
#include "ksim/ai/goalsel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ksim::routines {

// ---------------------------------------------------------------------------
// Tunables (5.3). Kinspect item 57 (findings/item57_scorer.md, s63_static.md) answered the formula; H unless marked.
// ---------------------------------------------------------------------------

// score = utility * resolverScore * weight * decay^n, in strict tiers: the first tier (URGENT, NON_URGENT, FLUFF) with a plannable
// candidate wins whatever the scores; inside a tier the highest wins, a tie goes to the later-inserted candidate. A score of 0 is
// never inserted (how surplus guards fall through). Nothing plannable: IDLE (ours: the STAY_IN_HOME fallback).
inline constexpr std::uint64_t kScoreIntervalTicks = 20; // OURS: the original has no timer (a full update every ceil(N / 8) frames); a staggered pass at Full LOD
// Promotion (residents.cpp, T3b1): the sim looks at its town squads this often, promotes at most this many squads per look (a world
// loaded at Full detail everywhere spreads the cost over seconds; squads go in Id order), and stands the members of a squad of n > 1
// on a ring of this radius round the squad's position (T3b2 replaces it with the seeded placement).
inline constexpr std::uint64_t kResidentIntervalTicks = 20;
// A newGame world's resident squads promoted within this many ticks of its start walk in from their token instead of being placed (5.34); short,
// so that a save taken later loads to the straight run (the Hub's squads promote within ~40 ticks of NEW_GAME).
inline constexpr std::uint64_t kArriveTicks = 100;
inline constexpr int kResidentSquadsPerPass = 32;
inline constexpr double kResidentRing = 6.0;
// Stragglers (demoteWorldSquad, k16.6.53): a living member farther than kStragglerRadius (ten rings, the follow leash of 60 u) from its squad's leader at demotion becomes a token
// of its own; its token walks back to the parent and merges within kStragglerMerge (half the radius, so a merged member is not a straggler again).
inline constexpr double kStragglerRadius = 10.0 * kResidentRing;
inline constexpr double kStragglerMerge = 0.5 * kStragglerRadius;
inline constexpr double kCrowdDecay = 1.0; // every town task: finder claims do the crowding
inline constexpr double kCrowdDecayMedic = 0.8; // medic, repair, builder, splint, fog worship
inline constexpr double kCrowdDecayBodyguard = 0.9;
inline constexpr double kCrowdDecayCannibal = 0.0; // eat / kill: at most one per squad
inline constexpr double kCurrentBonus = 1.25; // the running goal's multiplier
inline constexpr double kHoldFloor = 0.4; // a held goal's score floor while its hold runs
inline constexpr double kHoldCap = 0.05; // ... and its cap once the hold has expired
inline constexpr std::uint64_t kBlockTicks = 400; // a failed (goal, target) pair is skipped for 20 s
inline constexpr double kFallbackUtility = 0.01; // OURS: STAY_IN_HOME as the last candidate of every member (no terminal state, D6)
inline constexpr double kReservationMaxHours = 12.0; // OURS: the original has no timeout (and may leak on death / unload); listed deviation
inline constexpr double kShoppingNoCounterScore = 0.4; // resolverScore of SHOPPING when no counter is found
// The bar hangout (T3d, TOWN_LIFE_SLICE 5.17). SHOPPING 118 scores utility 1.0 x weight (0.5) = 0.5, RELAX 214 1.0 x 1.25 as the running goal: the plain
// rule, so shopping happens only when RELAX cannot plan (no reachable table), as kinspect TQ24 found live (patrons stayed on 214 for 30 h, 118 never
// appeared). UNSURE: the original gates SHOPPING on the member's shopping list ({item, wanted, bought}, bought counts reset after 07:00); we model no
// list, so SHOPPING is always wanted (the fallback only). Retired: the hourly want of the first T3d (retired/ note in 5.17).
// A designated template that no placement carries takes a free building within this of the town point (xz units), preferring one that has the
// template's `layout interior` (the original's layout compatibility test in assignTemplateToBuilding). UNSURE fallback.
inline constexpr double kBuildingReach = 400.0;
// A placement whose own squad template is a town's resident template belongs to the town within this of the town point (the Hub's Bar is 356 u out).
inline constexpr double kPlacementReach = 800.0;
// A chair or stool of a layout belongs to the nearest table within this (xz units): its seats (a squad member sits at one).
inline constexpr double kSeatReach = 20.0;
// A member within this of a seat of its table counts as seated (SIMLINK flag 15). Layout furniture is cut from the navmesh (2026-11-03), and a
// stool under a table edge lies inside the agent-radius margin round the table: the sitter stands at the walkable point nearest it, 0 to 6.1 u
// off in the Hub bar. UNSURE: the original may walk to a use node and then sit on the seat itself.
inline constexpr double kSeatTolerance = 8.0;
// A member with no seat stands on a ring of this radius round the table (UNSURE: spaced points, the data gave no seat), past the chairs (8-13 u
// out). 1.5 put every point under the table, which is cut from the navmesh, so the walks ended together on its rim.
inline constexpr double kTableRing = 16.0;
inline constexpr std::uint16_t kLooseSlot = 0xfffe; // the slot of a building's loose-chair seat group (the stools no table owns; TOWN_LIFE_SLICE 5.39)
// A ring spot must stand on the table's floor, never the ground outside (a wall cuts a ring sample away from the bar's floor and the nearest
// walkable point is a storey lower): each sample is snapped to the mesh at the table's height, and kept only if the surface is within
// kRingSlack (xz) and kRingFloor (y) of it. kRingNearestReach: when no sample works, the nearest walkable point to the table itself.
inline constexpr double kRingSlack = 4.0;
inline constexpr double kRingReach = 8.0;
inline constexpr double kRingFloor = 30.0;
inline constexpr double kRingNearestReach = 24.0;
inline constexpr int kRingSamples = 24;

// Utilities (code constants in the original).
inline constexpr double kUtilityNode = 0.5; // node 38 / 268, Shopkeeper 20, territorial 144
inline constexpr double kUtilityRandMax = 0.4; // post 157 and STAY_IN_HOME 43: rand(0..1) * 0.4 + 0.01, re-rolled each evaluation
inline constexpr double kUtilityRandBase = 0.01;
inline constexpr double kUtilityPatrol = 0.25; // PATROL_TOWN 36
// The patrol family's walk (task patrol, dt24_36): 36 / 22's point is 6 to 30 u from its anchor (the exe's "then a point within 30 u").
inline constexpr double kPatrolRadius = 30.0;
inline constexpr double kPatrolMin = 6.0;
inline constexpr double kUtilityDefault = 1.0; // doors 155 / 197 / 86, cages 195, RELAX 214, labour 189 / 190, shoo 158, corpse pot 137
inline constexpr double kUtilityUrgent = 2.0; // PUT_LOOT 184; GET_OUT_OF_BED 208 with an alarm
// Bed 153 / 239 (see bedUtility): 0 on an alarm or a visible hostile in the home; 1 already in bed; 0 for 180 min after waking
// (UNSURE: only some squad members); 0.3 under 20 min since the last sleep; else 1.
inline constexpr double kBedUtilityRecent = 0.3;
inline constexpr double kBedRecentMinutes = 20.0;
inline constexpr double kBedWakeCooldownMinutes = 180.0;

// Holds: on switching to a goal with a hold, minHold = now + base + rand * span game hours; a max-duration timer with the same
// numbers ends a held step. Every other town goal has none.
struct Hold
{
	double base = 0.0;
	double span = 0.0;
};
inline constexpr Hold kHoldNode{0.1, 0.5}; // 38 / 268
inline constexpr Hold kHoldPost{0.1, 1.0}; // 157
inline constexpr Hold kHoldStayInHome{0.2, 0.8};
inline constexpr Hold kHoldBed{2.0, 8.0}; // 153 / 239
inline constexpr Hold kHoldShopping{0.25, 1.5};
inline constexpr Hold kHoldLabour{0.5, 2.0}; // 190
// The hold of a goal; {0, 0} when it has none.
Hold holdOf(std::uint16_t task) noexcept;

// The goal-commit lock (dt33; TaskData +0x18 / +0x1c): committing a goal adds base + rand[0,1) * span SECONDS to the selector's
// min-duration timer, and while it runs onTaskDone / clearCurrentGoal refuse to drop the goal. {0, 0} is no lock. Per-task table,
// generic: a task with other values gets its own case in commitOf.
struct Commit
{
	double base = 0.0;
	double span = 0.0;
};
Commit commitOf(std::uint16_t task) noexcept;

// Synthetic beds mean no resident ever lacks a bed (deviation D17). deferred: decide in playtesting.
inline constexpr bool kSyntheticBeds = true;
inline constexpr int kBedRow = 4; // synthetic beds per row (routines.cpp SiteIndex::add)
inline constexpr double kBedSpacing = 20.0; // units between synthetic beds (OURS)
inline constexpr double kRingRadius = 12.0; // synthetic guard nodes round the main door
inline constexpr int kMinSyntheticGuardNodes = 2;
// A window with start == end (0-0) is always on (the original: start == end opens the goal; TQ19 answered). 
inline constexpr bool kEqualWindowAlwaysOn = true;

// ---------------------------------------------------------------------------
// Goals: the AI_TASK `enum` values the scorer knows (ORIGINAL_AI.md section 5, verified on the merged data)
// ---------------------------------------------------------------------------

namespace task
{
inline constexpr std::uint16_t UnequipWeapon = 7; // UNEQUIP_WEAPON: the planner sheathes before a no-weapon goal (GATES_CARRY_STATIC, dt24_275)
inline constexpr std::uint16_t Shopkeeper = 20;
inline constexpr std::uint16_t SelfPreservation = 32; // SELF_PRESERVATION: the race goal whose provider is 35 RUN_AWAY (dt34)
inline constexpr std::uint16_t Patrol = 22; // PATROL (task patrol, dt24_36 section 4): a goal-less walk at the member's own point
inline constexpr std::uint16_t MakeAnnouncement = 103; // MAKE_ANNOUNCEMENT (dt24_smalltasks section 5): Task_MakeAnnouncement, pre isLeaderDown = 0 + atPackageTarget
inline constexpr std::uint16_t MakeAnnouncementFast = 129; // MAKE_ANNOUNCEMENT_FAST: the same class (the plan flag 2 is not read by it)
inline constexpr std::uint16_t DefeatSquad = 101; // DEFEAT_SQUAD (dt24_101): goal isTargetDown on the current target's squad
inline constexpr std::uint16_t DefeatSquadLimitChase = 248; // DEFEAT_SQUAD_LIMIT_CHASE_RANGE: 101 plus iAmBeingMeleeAttacked = 0 and the chase-limit pair
inline constexpr std::uint16_t AttackSlavemasterEnemies = 41; // ATTACK_ENEMIES_OF_MY_SLAVEMASTER (task raid-goals, dt24_41)
inline constexpr std::uint16_t FirstAid = 58; // JOB_MEDIC (task CG): the goal; its plan is 25 FIRST_AID_ORDER -> Task_FirstAid
inline constexpr std::uint16_t PatrolTown = 36;
inline constexpr std::uint16_t RunAroundTown = 216; // RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE (task patrol, dt24_36 section 3)
inline constexpr std::uint16_t ProtectAlliesStayInTown = 288; // PROTECT_ALLIES_STAY_IN_TOWN: 15 inside the home ring (the leash of 144)
inline constexpr std::uint16_t ProtectAllies = 15; // PROTECT_ALLIES (task CF: the package's join goal)
inline constexpr std::uint16_t AttackEnemies = 16; // ATTACK_ENEMIES (task CF)
inline constexpr std::uint16_t AttackEnemiesAndNeutrals = 21; // MQ14 path 2: a squad that attacks the neutral squads it sees
inline constexpr std::uint16_t AttackTroubleMakers = 34; // ATTACK_TROUBLE_MAKERS (dt24_34): a criminal in my town
inline constexpr std::uint16_t TerritorialAggression = 144; // TERRITORIAL_AGGRESSION_BUT_DONT_LEAVE_HOME (dt24_144)
inline constexpr std::uint16_t StaticGuardBuilding = 38; // STAND_AT_GUARD_NODE_HOMEBUILDING
inline constexpr std::uint16_t StaticGuardBuildingIndoors = 268; // ..._INDOORS_ONLY: 38's resolver, the node query's indoors flag (dt24_38 section 4)
inline constexpr std::uint16_t NotBeUnarmed = 42; // DON'T BE UNARMED (dt25 section 2): the rearm loop 8 FIND_WEAPON -> 6 EQUIP
inline constexpr std::uint16_t StayInHome = 43;
inline constexpr std::uint16_t SitOnThrone = 255; // SIT_ON_THRONE (dt24_43 section 2c): 43's resolver on the throne slot of my home building
inline constexpr std::uint16_t RunAwayHometown = 127; // RUN_AWAY_HOMETOWN (dt24_128): Task_GoHomeTownFast, effect isAtHomeTown
inline constexpr std::uint16_t RetreatHometown = 128; // RETREAT_HOMETOWN (dt24_128): Task_GoHomeTown, a walk to the home town
inline constexpr std::uint16_t TravelToTargetTown = 53; // TRAVEL_TO_TARGET_TOWN (dt27 section 5): Task_TravelToTargetTown, pre hasDestinationTown, effect atDestinationTown
inline constexpr std::uint16_t TravelToTargetPackage = 215; // TRAVEL_TO_TARGET_PACKAGE (dt24_215): Task_TravelToMissionTarget_ArmyFormation, pre not crouching / lying, effect atPackageTarget, no finder
inline constexpr std::uint16_t TravelToTargetTownFast = 130; // ..._FAST: the same class and row shape (dt27 section 5)
inline constexpr std::uint16_t GoToSomewhereForDeliveringSlaves = 192; // dt27 section 6.1: the pre list atDestinationTown is the goal; 53 / 130 satisfy it
inline constexpr std::uint16_t GiveAllMySlavesTo = 194; // dt27 section 6.3: 218's twin (same resolver, same behaviour)
inline constexpr std::uint16_t LootFoodAndStuff = 131; // LOOT_FOOD_AND_STUFF (dt24_131): 243's finder, goal HAVE_SOME_LOOT_FROM; plan 284's pull
// The small tasks with an executor of their own (task ds/small-tasks-2; dt24_stalk_turret sections 5-6, item28b_static section 1.3, dt24_plantasks).
// None of them is a package goal in the data: the order or a plan that names them sets the Routine.
inline constexpr std::uint16_t StandUp = 28; // STAND_UP (Task_UnCrouch): goal isCrouching = 0
// The standing goals of the generic path (task port-standing, standing_goals.cpp; dt24_plantasks sections 1 / 2, ai_enum_map rows 19 / 24 / 30 / 62).
inline constexpr std::uint16_t GoHomeBuilding = 19; // GO_HOMEBUILDING: finder 14099f370, goal isAtLocation, effect isAtHome
inline constexpr std::uint16_t Wanderer = 24; // WANDERER: finder 140998040, goal isAtLocation, effect atATown
inline constexpr std::uint16_t HoldPosition = 30; // HOLD_POSITION (Task_Blank): the plan's hand is the place, goal isAtLocation
inline constexpr std::uint16_t StandStill = 62; // STAND_STILL (Task_StandStill 140336ba0): the tick only stops the movement host, no finish flag
inline constexpr std::uint16_t SeekAndTalk = 102; // SEEK_AND_TALK_AND_SEND_SIGNAL (Task_TalktoPlayer): walk to the target, then the dialogue event
inline constexpr std::uint16_t SendDialogue = 159; // SEND_DIALOGUE_SIGNAL (Task_TalktoPlayer): the same class as 102 and 161
inline constexpr std::uint16_t FollowWhileTalking = 164; // FOLLOW_WHILE_TALKING (Task_FollowAndTalk)
inline constexpr std::uint16_t TownStalker = 165; // TOWN_STALKER (Task_TownStalker)
inline constexpr std::uint16_t WorkTheSlaves = 188; // WORK_THE_SLAVES (dt24_131 section 4): a random owned character, the work dialogue
inline constexpr std::uint16_t GatherSlavesFemaleOrBeast = 245; // ..._IF_FEMALE_OR_BEAST (dt24_131 section 7): 173 over the female or other-race occupants
inline constexpr std::uint16_t GatherSlavesFromCagesExport = 217; // ..._IF_ITS_AN_EXPORT_TOWN (dt27 section 6.2): 173 with the standing-occupant filter
inline constexpr std::uint16_t GiveAllMySlavesToImport = 218; // ..._IF_ITS_AN_IMPORT_TOWN (dt27 section 6.3): the slave-storage keeper of my town
inline constexpr std::uint16_t BeatCageOccupant = 196; // BEAT_CAGE_OCCUPANT (dt24_killprisoner section 1): Task_KillPrisoner, goal isTargetDown (dt39 section 4: the cannibals' first step)
inline constexpr std::uint16_t KillRandomCageOccupant = 135; // KILL_A_RANDOM_CAGE_OCCUPANT (dt27 section 3): the cage pick 1408d0d90, then 134 Task_KillPrisoner
inline constexpr std::uint16_t HuntingBloodsmell = 176; // HUNTING_BLOODSMELL (dt27 section 4): Task_HuntingSmell, a walk at the platoon's smell
inline constexpr std::uint16_t EatRandomCageOccupant = 139; // EAT_A_RANDOM_CAGE_OCCUPANT (dt29_fog_kidnap sections 6, 8): the live caged victim, then 220
inline constexpr std::uint16_t EatRandomCageOccupantMeasured = 282; // ..._MEASURED_RATE (dt24_killprisoner section 1): 139's finder and goal; only the data rate differs
inline constexpr std::uint16_t FogmanWorshipVictim = 237; // FOGMAN_WORSHIP_VICTIM (dt24_237): join the worship of a victim already being eaten
inline constexpr std::uint16_t FindAndRescueLeader = 142; // dt24_142 section 2: the platoon leader while it is down (pre isLeaderDown), then the plan's carry
inline constexpr std::uint16_t FindAndRescueIfBeds = 148; // dt24_142 section 3: the nearest downed ally; the bed check is the plan's, not the finder's
inline constexpr std::uint16_t EatRandomDeadBody = 230;
inline constexpr std::uint16_t EatRandomKoBody = 233; // EAT_A_RANDOM_KO_BODY (dt39 section 5): a knocked-out living body, the animals' open-air meal; then 220
inline constexpr std::uint16_t FindAndRescue = 105; // FIND_AND_RESCUE (finder 140009697 -> 1408cd550, 148's resolver); goal IS_CARRYING_TARGET
inline constexpr bool rescues(std::uint16_t task) noexcept { return task == FindAndRescueLeader || task == FindAndRescueIfBeds || task == FindAndRescue; }
// 135 and 196 run the same class (Task_KillPrisoner, 134): the blows on a caged occupant.
inline constexpr bool killsCageOccupant(std::uint16_t task) noexcept { return task == KillRandomCageOccupant || task == BeatCageOccupant; }
inline constexpr bool eatsCageOccupant(std::uint16_t task) noexcept { return task == EatRandomCageOccupant || task == EatRandomCageOccupantMeasured; } // EAT_A_RANDOM_DEAD_BODY (dt27 section 2): the sensed corpse, then 220 Task_EatPrisoner
inline constexpr std::uint16_t Bodyguard = 45; // BODYGUARD (dt25 section 1): the protected hand and the companion 31 follow
inline constexpr std::uint16_t OpenUpShopDoors = 86;
inline constexpr std::uint16_t FindBedAndPutIn = 106; // FIND_BED_AND_PUT_IN (dt24_106, dt29): put the carried person in a bed
inline constexpr std::uint16_t GetOutOfBed = 116; // GET_OUT_OF_BED (dt24_208): Task_GetOutOfBed, the planner step that provides isInBed = 0
inline constexpr std::uint16_t GetOutOfBedEmergency = 208; // ..._IF_ITS_EMERGENCY: the Urgent selector goal over the same class
inline constexpr std::uint16_t GetOutOfBedHealed = 257; // ..._ONCE_HEALED: a third registration of the class (no package row)
inline constexpr std::uint16_t FollowSquadLeader = 141; // FOLLOW_SQUADLEADER (dt24_141): Task_FollowLeader, the platoon leader
inline constexpr std::uint16_t FollowSlaveMaster = 171; // FOLLOW_SLAVEMASTER: the same class, the captor hand answers first
inline constexpr std::uint16_t PutDownCarriedDude = 169; // ..._IF_THEY_CAN_WALK (dt25 section 3): the can-walk gate -> 69 put down
inline constexpr std::uint16_t UseCage = 107; // USE_CAGE (dt24_plantasks): 172's plan step, I_AM_IMPRISONED's writer
inline constexpr std::uint16_t FindCageAndPutIn = 112;
inline constexpr std::uint16_t FindCageAndPutDeadGuyIn = 281; // dt26 section 0, dt24_172: 112's cage finder with a dead body allowed (the same effect 154, then 108)
// The pull tasks (dt24_emptymachine_dismantle): a machine's output, a storage box's contents, and the construction-material
// variant (two per pass). 96 DISMANTLE waits for BUILDINGS_SLICE Phase 4 (FEATURES).
inline constexpr std::uint16_t EmptyMachine = 92; // COLLECT_OUTPUT_RESOURCE
inline constexpr std::uint16_t BuildMats = 247; // COLLECT_OUTPUT_RESOURCE_BUILD_MATS
inline constexpr std::uint16_t LootContainer = 284; // LOOT_CONTAINER
// The machine-work goals (ds/machines; dt24_113, dt24_plantasks section 4): 113 and 152 share the machine picker (1405a70a0) and
// run 92's pull; 93 / 124 (Task_FillMachine) and 88 / 114 / 242 put what the member carries into one of my storage boxes.
inline constexpr std::uint16_t DeliverResources = 88; // DELIVER_RESOURCES (Task_Blank, only pre isAtLocation)
inline constexpr std::uint16_t FillMachine = 93; // FILL_MACHINE (Task_FillMachine)
inline constexpr std::uint16_t EmptyMachineOutputs = 113; // EMPTY_MACHINE_OUTPUTS
inline constexpr std::uint16_t GetRidOfResources = 114; // GET_RID_OF_RESOURCES_IN_MY_INVENTORY
inline constexpr std::uint16_t OperateStorage = 124; // OPERATE_STORAGE (Task_FillMachine)
inline constexpr std::uint16_t OperateAutomaticMachinery = 152; // OPERATE_AUTOMATIC_MACHINERY
inline constexpr std::uint16_t DitchAllResources = 242; // DITCH_ALL_RESOURCES
// The jam goals (ds/machines-2; dt24_113 sections 1-2, dt24_emptymachine_source section 1): a machine is jammed while its work state
// (+0x468, our Producer::prodState) is 0 or 1 - 1 is "output full", the one state ours reaches. 89 / 90 / 91 run the machine picker
// over the jammed machines; the unjam is 92's pull (the only way the output empties). 241 loots a body's storable resources.
inline constexpr std::uint16_t KeepEverythingRunning = 89; // JOB_KEEP_EVERYTHING_RUNNING (Task_Blank, pre: no jammed machines)
inline constexpr std::uint16_t UnjamAllMachines = 90; // UNJAM_ALL_MACHINES (resolver 14059ab90: my town's machines, work state 0 / 1)
inline constexpr std::uint16_t UnjamMachine = 91; // UNJAM_MACHINE (Task_Blank; the machine is 90's action hand)
inline constexpr std::uint16_t LootStorable = 241; // LOOT_RESOURCE_ITEMS_WE_HAVE_STORAGE_FOR (Task_Loot_ResourcesWeHaveStorageFor, dt24_cage_loot_play section 3)
constexpr bool unjams(std::uint16_t task) noexcept
{
	return task == KeepEverythingRunning || task == UnjamAllMachines || task == UnjamMachine;
}
// The goals that put carried resources into a storage box (the push half).
constexpr bool storesResources(std::uint16_t task) noexcept
{
	return task == DeliverResources || task == FillMachine || task == GetRidOfResources || task == OperateStorage || task == DitchAllResources;
}
// The goals that empty a machine's output into the member's inventory (the pull half; 92 is the action).
constexpr bool pullsMachine(std::uint16_t task) noexcept
{
	return task == EmptyMachineOutputs || task == OperateAutomaticMachinery;
}
inline constexpr std::uint16_t Shopping = 118;
inline constexpr std::uint16_t BuyStuff = 119;
inline constexpr std::uint16_t FindAndKidnap = 132; // FIND_AND_KIDNAP (dt26 section 1): the planner-only lift row 254 expands to (no AI_TASK record in the running data)
inline constexpr std::uint16_t FeedCorpseIntoMachine = 136; // "Disposing of corpse": the feed step of 137's plan (dt24_patrol_town_pot s3)
inline constexpr std::uint16_t DeadGuysInThePot = 137;
inline constexpr std::uint16_t FindCorpses = 138; // "Cannibal- Find a corpse": the lift step of 137's plan
inline constexpr std::uint16_t ManTurrets = 149;
inline constexpr std::uint16_t GoHomeToBed = 153;
inline constexpr std::uint16_t LockAllMyDoors = 155;
inline constexpr std::uint16_t StaticGuardTown = 157; // town outside nodes
inline constexpr std::uint16_t ShooStrangers = 158;
inline constexpr std::uint16_t ProtectOwnSquad = 143; // PROTECT_OWN_SQUAD (task CF: a squadmate is fighting)
inline constexpr std::uint16_t CaptureSlaves = 167;
inline constexpr std::uint16_t CarryWoundedSlaves = 168;
inline constexpr std::uint16_t TotalEscape = 202; // TOTAL_ESCAPE (dt24_207): a data-only row, pre isChained = 0 and I_AM_IMPRISONED = 0, no effect
// The lock and captivity tasks (task ds/shackles-locks): player orders and planner steps, never selector goals.
inline constexpr std::uint16_t PickLock = 76;
inline constexpr std::uint16_t SlaveObedience = 187; // _SLAVE_OBEDIENCE: a Task_Blank plan node (goal noneR)
inline constexpr std::uint16_t ArrestTarget = 203; // ARREST_TARGET: target down + unarmed + carried; goal TARGET_IS_ARRESTED (item25)
inline constexpr std::uint16_t SmashBuilding = 272; // SMASH_BUILDING: goal isBuildingDestroyed (Task_BashDoor class)
inline constexpr std::uint16_t CutLock = 285;
inline constexpr std::uint16_t BruteForceLock = 286;
inline constexpr std::uint16_t PickLockOnShackles = 201;
inline constexpr std::uint16_t CutShackles = 185;
inline constexpr std::uint16_t BruteForceShackles = 186;
inline constexpr std::uint16_t TakeOffMyShackles = 219;
inline constexpr std::uint16_t ReleasePrisoner = 110;
inline constexpr bool isLockTask(std::uint16_t task) noexcept
{
	return task == PickLock || task == CutLock || task == BruteForceLock || task == PickLockOnShackles || task == CutShackles
		|| task == BruteForceShackles || task == TakeOffMyShackles || task == ReleasePrisoner || task == 133;
}
// The construction tasks (ds/construction; b50c_static Q6-Q10, Q18, dt24_enterleave): player orders and plan steps, not selector goals
// (nothing shows an NPC package planning them, b50c Q20), so they run from Simulation::orderConstructionTask.
inline constexpr std::uint16_t Build = 2; // BUILD (Task_Build; the player order, order case 2)
inline constexpr std::uint16_t AddMaterials = 71; // ADD_MATERIALS_TO_BUILDING
inline constexpr std::uint16_t Repair = 95; // REPAIR: Task_Build again
inline constexpr std::uint16_t Dismantle = 96; // DISMANTLE (Task_Dismantle)
inline constexpr std::uint16_t UseTrainingDummy = 97; // USE_TRAINING_DUMMY (class body unread)
inline constexpr std::uint16_t EnterBuilding = 156; // ENTER_BUILDING
inline constexpr std::uint16_t LeaveBuilding = 200; // LEAVE_BUILDING
inline constexpr bool isConstructionTask(std::uint16_t task) noexcept
{
	return task == Build || task == AddMaterials || task == Repair || task == Dismantle || task == UseTrainingDummy
		|| task == EnterBuilding || task == LeaveBuilding;
}
inline constexpr std::uint16_t GetOutOfCageLegit = 133; // GET_OUT_OF_CAGE_LEGIT: the same class, mode 0 (a freed prisoner's plain leave; nothing in the data lists it)
inline constexpr std::uint16_t GetOutOfCageEscape = 207; // GET_OUT_OF_CAGE_ESCAPE (dt24_207): Task_GetOutOfCage, mode 0
inline constexpr std::uint16_t GetOutOfCageOpportunistic = 256; // GET_OUT_OF_CAGE_OPPORTUNISTIC: the same class, mode 1
inline constexpr std::uint16_t FollowUrgentEscape = 253; // FOLLOW_URGENT_ESCAPE (dt24_207): Task_Follow, pre isTargetDown = 0
inline constexpr std::uint16_t SlaveGetInCage = 172; // SLAVE_GET_IN_MY_MASTERS_CAGE (dt24_172): the slave walks into its captor's cage
inline constexpr std::uint16_t GatherSlavesFromCages = 173; // task BW (dt15 section 5): take my slave out of its cage
inline constexpr std::uint16_t LootTheDead = 177; // task BO: the winners loot the food off a down body, one member, then the follow
inline constexpr std::uint16_t SlaveWatching = 183;
inline constexpr std::uint16_t PutLootInStorage = 184;
inline constexpr std::uint16_t StripSlaves = 182;
inline constexpr std::uint16_t NewSlaveProcessing = 260; // task BW (dt15 section 2): process a fresh prisoner held in my town's cages
inline constexpr std::uint16_t PretendLabour = 189;
inline constexpr std::uint16_t OperateMachinery = 87; // OPERATE_MACHINERY (dt24_emptymachine_source): the goal's IS_WORKING step
inline constexpr std::uint16_t PretendLabourPretend = 190;
// The occupied-town goals (dt24_243 / dt24_275; the 'hostile occupy town' packages list both) and the animals' floor sleep
// (dt24_175). 243 plans 244 GRAB_ONE_FOOD (code-only: no package names 244); 275 plans 190, the pretend-labour row.
inline constexpr std::uint16_t SleepOnFloor = 175; // SLEEP_ON_FLOOR (Task_SleepOnFloor): no finder, sleep where I stand
inline constexpr std::uint16_t AcquireFood = 243; // AQUIRE_FOOD_AT_HOMEBASE
inline constexpr std::uint16_t GrabOneFood = 244; // GRAB_ONE_FOOD (243's plan step)
inline constexpr std::uint16_t SitAround = 275; // SIT_AROUND
inline constexpr std::uint16_t LockAllMyCages = 195;
inline constexpr std::uint16_t LockDoorsStayOutside = 197;
inline constexpr std::uint16_t RelaxInTownPackage = 214; // "obey the AI package"
inline constexpr std::uint16_t PretendToOperate = 221;
inline constexpr std::uint16_t TurretOnBuilding = 222; // MAN_A_TURRET_ON_BUILDING: the 234 player-job resolver (dt24_turret_finder)
inline constexpr std::uint16_t AssaultFortifications = 270; // ASSAULT_FORTIFICATIONS_PREFER_GATES (task raid-goals, dt24_270)
inline constexpr std::uint16_t LiberatePrisoners = 276; // LIBERATE_ALL_THE_PRISONERS (task raid-goals, dt24_276)
inline constexpr std::uint16_t PickupIntruders = 223;
inline constexpr std::uint16_t TakeIntruderOutside = 224;
inline constexpr std::uint16_t ManTheGate = 180; // MAN_THE_GATE (dt24_180): the gate crew's post goal; no effect, held by its preconditions
inline constexpr std::uint16_t GoHomeToBedSecure = 239;
inline constexpr std::uint16_t FinalKidnapperCageJob = 254; // FINAL_KIDNAPPER_CAGE_JOB (dt26): the kidnapper package goal; lift a victim, then 112/108 cage it
inline constexpr std::uint16_t PickupIntrudersTown = 273; // the town-scoped twin of 223 (dt24_273; task CH group 3)
// The crime goals (task crime-rows; kinspect dt41_E): the alarm investigators, the prisoner finders and the bounty hunt.
inline constexpr std::uint16_t HuntBounties = 204; // goal TARGET_IS_ARRESTED; finder 14099bfc0, admitted by 140853fd0 / 140678960 / isEnemy
inline constexpr std::uint16_t InvestigateAlarms = 209; // Task_InvestigateAlarms (140354260); goal ALARMS_IN_THE_VICINITY = 0
inline constexpr std::uint16_t InvestigateAlarmsAlliesOnly = 210; // the same class: the 210 tick also notifies the sensed allies
inline constexpr std::uint16_t PoliceFreePrisoners = 211; // POLICE_FREE_PRISONERS_WHEN_DONE; finder 1408cf3c0; goal TARGET_IS_IMPRISONED = 0
inline constexpr std::uint16_t LootStolenGoods = 212; // LOOT_STOLEN_GOODS; finder 1408cf6a0; goal isSubjectInInventory
// The animal and food rows (task port-animals; kinspect dt24_eat / dt24_plantasks / dt24_cage_loot_play): code-only goals, named by an order or a
// plan, never a package row. Their finders are worldres::findCrops232 / foodOnGround259 / limb277 / mouthLimbAlly279 / animalLoot238.
inline constexpr std::uint16_t EatCrops = 231; // Task_EatingCrops (1400215ee): goal am hungry = 0
inline constexpr std::uint16_t FindCropsToEat = 232; // finder 1408cb810 (BuildingFinder_EatCrops)
inline constexpr std::uint16_t LootAnimalsJob = 238; // finder 1408ce3a0 + 140998950; goal LOOTED_STORABLE_ITEMS
inline constexpr std::uint16_t EatFoodOnGround = 259; // finder 14099a930
inline constexpr std::uint16_t SleepOnFloorFakeAmbush = 261; // Task_SleepOnFloor (14001231e) again: 175's class with 261's registry row
inline constexpr std::uint16_t AnimalFetchALimb = 277; // finder 1409993a0
inline constexpr std::uint16_t PlayWithLimbInMouth = 278; // Task_DogLimbRunningPlay (1400033a5)
inline constexpr std::uint16_t ChaseAllyDogs = 279; // CHASE_ALLY_DOGS_WITH_MOUTH_LIMBS; finder 14099ad40
inline constexpr std::uint16_t TakeIntruderOutsideTown = 274; // 224's town tick: the drop beyond the town's gates
inline constexpr std::uint16_t ShooStrangersPrivate = 283;
} // namespace task

// How the town scorer treats a taskType.
enum class TaskUse : std::uint8_t
{
	Unmodelled = 0, // not in the table: the row is skipped and counted ("unmodelled goal"), never an error
	Modelled, // the scorer runs it (phase says when it lands)
	Adopted, // an existing system executes it; the scorer only decides when
	Owned, // an always-on goal owned by combat, jobs or crime: the scorer yields to it
	Ignored, // known and deliberately not modelled (turrets)
	Never, // a dead value: no AI_TASK uses it
};

struct TaskInfo
{
	std::uint16_t task = 0;
	const char* name = "";
	TaskUse use = TaskUse::Unmodelled;
	const char* phase = ""; // "T3b" ... "T3e2", "later", "crime", "never"
};

// The goal table (5.5), sorted by task. nullptr for a taskType it does not list.
const TaskInfo* taskInfo(std::uint16_t task) noexcept;
const std::vector<TaskInfo>& taskTable() noexcept;
// Modelled or Adopted: the goal gives a member something to do in T3.
bool runsGoal(std::uint16_t task) noexcept;
// The goals whose moves walk at the race's walk speed (kenshi-h2v.31, TOWN_LIFE_SLICE 5.38): the non-urgent patrols, the search, the bar's relax
// and shopping walks, the stay at home. Every other goal keeps the speed mode (run by default).
inline bool walksGoal(std::uint16_t goal) noexcept
{
	return goal == task::PatrolTown || goal == task::Patrol || goal == task::RunAroundTown || goal == task::RelaxInTownPackage
		|| goal == task::Shopping || goal == task::StayInHome;
}

// ---------------------------------------------------------------------------
// Package compile
// ---------------------------------------------------------------------------

// A member's list: which SQUAD_TEMPLATE list it was spawned from (5.2). The live role is Leader when the member is the squad's
// leader now. Slave members read the slave list.
enum class Role : std::uint8_t
{
	Squad = 0,
	Squad2 = 1,
	Leader = 2,
	Slave = 3,
};
inline constexpr int kRoleCount = 4;

// taskPriority (AI_TASK `classification`).
enum class Tier : std::uint8_t
{
	Fluff = 1,
	NonUrgent = 2,
	Urgent = 3,
};

struct GoalRow
{
	std::string taskSid; // the AI_TASK record
	std::uint16_t task = 0; // its `enum`
	Tier tier = Tier::NonUrgent;
	std::int32_t start = 0; // hour, v0
	std::int32_t end = 24; // hour, v1 (reaches 280 in the data)
	std::int32_t weight = 0; // v2; effectiveWeight: 0 = 1.0, else v2 * 0.01
};

// Hour window [start, end) on a 24 h clock: wraps when start > end; end > 24 is always on; start == end is always on
// (kEqualWindowAlwaysOn). `hour` in [0, 24).
bool windowHolds(std::int32_t start, std::int32_t end, double hour) noexcept;
bool windowHolds(const GoalRow& row, double hour) noexcept;
// The row's weight: 1.0 if val2 == 0, else val2 * 0.01.
double effectiveWeight(const GoalRow& row) noexcept;

// The one G2 offer both selectors build (the town scorer's candidates and the world squads' rows): the task's utility x its resolver's
// score, the row's tier / weight / window, the crowd decay and the goal's hold (holdOf), the resolver's target for the same-goal test and
// the squadmates already on the task. `window` defaults to always on (the scorer applies the data's window in candidatesAt).
goalsel::Offer makeOffer(std::uint16_t task, Tier tier, double weight, double utility, std::uint64_t target, int others = 0, std::int32_t start = 0, std::int32_t end = 24) noexcept;

// The G1 row insert (dt19 section 4): appends `rows` to `into`; a row whose task record the list already holds is skipped.
// The task record stands in for the exe's row "hand" (the data carries no separate one). `inherits from` merges parents first
// into the same lists, so a child's row for a task its parent already listed is dropped rather than substituted - not the
// replace-by-task rule TOWN_LIFE_SLICE 5.5 assumed.
void mergeRows(std::vector<GoalRow>& into, const std::vector<GoalRow>& rows);

struct CompiledPackage
{
	std::string sid;
	std::string name;
	// After the `inherits from` merge and the empty-list fallback. Indexed by Role.
	std::array<std::vector<GoalRow>, kRoleCount> lists;
	// Which list each role actually reads (its own, or the one the fallback copied); equals the role when it is its own.
	std::array<Role, kRoleCount> readsFrom{Role::Squad, Role::Squad2, Role::Leader, Role::Slave};
	bool empty() const noexcept;
	const std::vector<GoalRow>& list(Role role) const noexcept { return lists[static_cast<std::size_t>(role)]; }
};

// One candidate of a scorer pass: a window-valid row, or the fallback.
struct Candidate
{
	const GoalRow* row = nullptr; // nullptr for the fallback
	std::uint16_t task = 0;
	Tier tier = Tier::NonUrgent;
	double weight = 1.0;
	bool fallback = false;
};

// The candidates of one member at `hour`: every window-valid row the scorer runs (runsGoal), in list order, then the
// always-available STAY_IN_HOME fallback (kFallbackUtility; it cannot fail). Never empty, so no member is goal-less
// (5.3, deviation D6). `unmodelled` counts the window-valid rows skipped as not modelled or owned elsewhere.
std::vector<Candidate> candidatesAt(const CompiledPackage& package, Role role, double hour, std::size_t* unmodelled = nullptr);

class PackageTable
{
public:
	// Compiles every AI_PACKAGE (type 72) of the merged data. Pure; nothing is cached outside the table.
	explicit PackageTable(const kdata::GameData& data);

	const CompiledPackage* find(std::string_view packageSid) const noexcept;
	// The RACE record's `AI Goals` rows (dt34): the lists the selector scores next to the package's. Empty for an unknown race.
	const std::vector<GoalRow>& raceRows(const std::string& raceSid) const;
	// What a member of `raceSid` scores: the race rows for tasks the package's role list does not carry, then the package's rows
	// (the race list sits under the package's: a task in both takes the package's row, and a tie goes to the later offer, the
	// package). `package` may be null.
	std::vector<GoalRow> rowsWithRace(const CompiledPackage* package, Role role, const std::string& raceSid) const;
	const std::vector<CompiledPackage>& packages() const noexcept { return packages_; }
	// Rows whose AI_TASK record is absent from the data (skipped).
	std::size_t missingTasks() const noexcept { return missingTasks_; }
	// Cycles met in `inherits from` (the back edge is ignored).
	std::size_t inheritCycles() const noexcept { return cycles_; }

private:
	const kdata::GameData* data_ = nullptr;
	mutable std::unordered_map<std::string, std::vector<GoalRow>> raceRows_;
	std::vector<CompiledPackage> packages_;
	std::unordered_map<std::string, std::size_t> index_;
	std::size_t missingTasks_ = 0;
	std::size_t cycles_ = 0;
};

// The coverage report's "loaded routine" column for a package: "modelled" (a list holds a goal the scorer runs),
// "fallback" (it has goals, none of them modelled: the members live at home), "none" (no goal list at all).
const char* routineStatus(const CompiledPackage& package) noexcept;
// Whether any list of the package holds a row of this task (any window); and whether the role's list holds one whose window is open at `hour`.
bool carriesGoal(const CompiledPackage& package, std::uint16_t task) noexcept;
bool goalHolds(const CompiledPackage& package, Role role, std::uint16_t task, double hour) noexcept;

// ---------------------------------------------------------------------------
// The unloaded phase (5.3 LOD, T3f)
// ---------------------------------------------------------------------------

// What a squad is expected to be doing when nobody is near, from the leader's list and the clock: at its post, at work, relaxing (a
// bar), patrolling, asleep, or at home. 5.7's counters come from Post, Relax and Sleep ("at post", "in a bar", "asleep"); Work is the
// rest of the routine goals and Home the fallback state. Derived from the package and the hour, never saved: one lookup per squad at
// each squad step, and once a game hour while Strategic.
enum class TownPhase : std::uint8_t
{
	Home = 0,
	Post,
	Work,
	Relax,
	Patrol,
	Sleep,
};
inline constexpr int kTownPhaseCount = 6;
const char* townPhaseName(TownPhase phase) noexcept;

// The phase a goal leaves a squad in: Sleep for the bed goals, Post for the two guard-node goals, Patrol for PATROL_TOWN, Relax for
// the bar, Home for STAY_IN_HOME, Work for every other goal the scorer runs. OURS: SHOPPING and the door, cage and slave-errand goals
// all count as Work (the data names no finer split).
TownPhase phaseOfTask(std::uint16_t task) noexcept;

// A goal that only exists while an event does: an intruder, a stranger, a downed body, a corpse, a loot pile. It has no finder to
// assume (its precondition is live state), and the always-on addons carry it in every leader list, so counting it would make it the
// phase at every hour. OURS (5.3 LOD): the phase is the squad's ordinary routine and skips these rows.
bool phaseSkipsTask(std::uint16_t task) noexcept;

// The leader's phase at `hour`: of the leader's window-valid rows the scorer runs (the event goals above aside), the one the scorer's
// strict tiers would pick first (highest tier, then weight; a tie goes to the later row), with all finders assumed to succeed (5.3: no
// catch-up, no walking and no reservation while unloaded). No such row: Home (the STAY_IN_HOME fallback). `unmodelled` counts the
// window-valid rows skipped as not modelled, like candidatesAt.
TownPhase townPhase(const CompiledPackage& package, double hour, std::size_t* unmodelled = nullptr) noexcept;

// ---------------------------------------------------------------------------
// Sites (5.4)
// ---------------------------------------------------------------------------

// Data facts, verified on the merged data (TQ23, docs/formats/TOWN_ROUTINE_DATA.md).
inline constexpr std::int32_t kFunctionBed = 6;
inline constexpr std::int32_t kFunctionCage = 8; // a cage piece of a Slave Shop, Police Station, Prison or Faction HQ layout (T3e2)
inline constexpr std::int32_t kFunctionCounter = 9;
inline constexpr std::int32_t kFunctionTable = 16;
inline constexpr std::int32_t kFunctionChair = 17;
inline constexpr std::int32_t kFunctionThrone = 24; // "throne chair": 1 << 24 is 255's finder mask 0x1000000 (dt24_43 section 2c)
inline constexpr std::int32_t kFunctionTraining = 7; // a Training Dummy's functionality (build21_static section 5: "function 7")
inline constexpr std::int32_t kFunctionGunTurret = 12; // "Gun turret": the building factory's TurretBuilding case (dt24_turret_finder section 2)
// The disposal machine (dt24_patrol_town_pot section 3): 137's pot finder (`building vf+0x2f0` 0xb). "Cooking pot" 2678 and "Corpse
// disposal" 4004 in the merged data; the buildings carrying them are Corpse Furnace 3962 and Cooking Pit 2673.
inline constexpr std::int32_t kFunctionDisposal = 11;
inline constexpr std::int32_t kNodeShopkeeper = 0;
inline constexpr std::int32_t kNodeGeneral = 1;
inline constexpr std::int32_t kNodeGuardpost = 2;
inline constexpr std::int32_t kNodeDefensive = 3; // also Node Turret
inline constexpr std::int32_t kNodeUse = 4;
inline constexpr std::int32_t kNodeLightMount = 5;

enum class SiteKind : std::uint8_t
{
	None = 0,
	HomePoint,
	GuardNode,
	TownNode,
	Bed,
	Table,
	Counter,
	Door,
	Work, // a producer BUILDING a member pretends to operate (T3b2); not in the SiteIndex: the scorer resolves it from World::producerSites()
	Cage = 9, // a cage of a layout (T3e2, function 8); real pieces only, never synthetic. Appended so the saved RoutineSite kind values keep theirs
	Turret = 10, // a turret BUILDING (functionality function 12; task CH turrets); not in the SiteIndex: the scorer resolves it from its own list
	Disposal = 11, // a disposal machine BUILDING (functionality function 11; 137's pot); not in the SiteIndex: the scorer resolves it from World::buildingSites()
	Throne = 12, // a throne chair piece of a layout (functionality function 24 = the exe's mask 0x1000000; 255 SIT_ON_THRONE); real pieces only
	BarSeat = 14, // a chair or stool of a table of a bar (a ResidentData::seatsOfTable entry: object = the table's key, slot = the seat's index); a RELAX 214 member's own claim, one holder per seat; never in the SiteIndex
	Seat = 13, // a chair or stool of a layout that serves no table (kenshi-k16.6.30): where a STAY_IN_HOME 43 resident of the building sits; real pieces only
	ShopkeeperNode = 15, // a `Node Shopkeeper` marker (node type 0) of a layout: where the shopkeeper stands, behind the counter (kenshi-k16.6.106); real pieces only
};
inline constexpr int kSiteKindCount = 16;

// One usable thing. `object` is the owning building's key; `slot` the ordinal within (building, kind), real and synthetic alike.
struct Site
{
	SiteKind kind = SiteKind::None;
	std::uint64_t object = 0;
	std::uint16_t slot = 0;
	auto operator<=>(const Site&) const = default;
};

// A site's id on the wire (SIMLINK INTERACTABLE kind 5 "Bed", pending client approval): kSiteIdFlag + an FNV of the site's
// key (building and slot). Only a SiteKind::Bed site gets one, and a bed's id resolves back to that site and no other.
inline constexpr std::uint64_t kSiteIdFlag = std::uint64_t{1} << 62;
std::uint64_t siteId(const Site& site) noexcept;

// What a BUILDING record offers a member: GuardNode for `node type` 2 (an `is node` record), else the function of its
// `functionality` ref (6 Bed, 9 Counter, 16 Table), else None.
SiteKind classifyPiece(const kdata::GameData& data, const kdata::Record& building);

// A `Node Shopkeeper` marker: an `is node` BUILDING of node type 0. classifyPiece leaves it None (it is no guard node); layouts offer it as ShopkeeperNode.
bool isShopkeeperNode(const kdata::Record& building);

// The `function` ids of a BUILDING record's `functionality` refs (the same id space kFunctionBed ... kFunctionTable are in: 8 is
// the cage). The guard-node resolver 14099edd0 tests the home building's id against {1, 3, 8} before it hunts an intruder
// (dt24_38 section 3; task CH group 3); the data names 1 the production function (mines, farms) and 3 research, so the set is
// taken as decoded (UNSURE names). Empty: the building has no functionality (an ordinary house).
std::vector<std::int32_t> buildingFunctions(const kdata::GameData& data, const kdata::Record& building);

// A turret BUILDING: its `functionality` ref resolves to a BUILDING_FUNCTIONALITY row with int `function` == 12 ("Gun turret"; 13
// BUILDING rows in the merged data). The exe's TurretBuilding class is chosen by that same function id at load (dt24_turret_finder
// section 2), and the building then carries a `gun turret` GUN_DATA ref (all 13 do; the ctor throws without one). Not the layout
// node type 3 (`n-turret node` / `n-defensive position` only mark where one may be mounted) and not the guard-post set {1, 3, 8}.
bool isTurretBuilding(const kdata::GameData& data, const kdata::Record& building);

// A BUILDING_FUNCTIONALITY row the building factory FUN_14057cc70 (case function 8, lines ~850-907; kinspect via the coordinator, H) builds
// a TortureBuilding from instead of a plain cage: function 8 with a `produces` reference, or a non-zero `flags` int. The "skin peeler" row
// (rebirth.mod) has `produces`; "Cage" and "Cage- prisoner post" have neither. Keyed on the data, never on the name.
bool isTortureFunctionality(const kdata::Record& functionality);

// A piece of furniture or a node of an interior layout, building-local.
struct LayoutPiece
{
	std::string target; // the BUILDING record
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double qw = 1.0; // building-local rotation
	double qx = 0.0;
	double qy = 0.0;
	double qz = 0.0;
	std::uint16_t index = 0; // the instance's position in the data's list, every entry counted (an empty-target one is not kept here, so this can exceed its position in `pieces`)
};

struct Layout
{
	std::string sid; // "<building sid>-<layout name>"
	std::string building; // `associated building`
	std::vector<LayoutPiece> pieces;
};

// The interiors.level layouts (WORLD_LAYOUT.md): the four layer files, merged by sid. The files are ordinary kdata files.
class LayoutLibrary
{
public:
	// dataDir/newland/leveldata/{,Newwworld,Dialogue,rebirth}/interiors.level that exist, in layer order.
	static std::vector<std::string> levelFiles(std::string_view dataDir);
	// Throws std::runtime_error on an unreadable file; kdata::ParseError on a bad one.
	static LayoutLibrary load(const std::vector<std::string>& files);
	static LayoutLibrary load(std::string_view dataDir) { return load(levelFiles(dataDir)); }

	const Layout* find(std::string_view layoutSid) const noexcept;
	// The layouts of one building, sid order.
	std::vector<const Layout*> forBuilding(std::string_view buildingSid) const;
	std::size_t size() const noexcept { return layouts_.size(); }

private:
	std::vector<Layout> layouts_;
	std::unordered_map<std::string, std::size_t> index_;
};

// A piece already classified, building-local.
struct Piece
{
	SiteKind kind = SiteKind::None;
	double x = 0.0;
	double z = 0.0;
	double y = 0.0; // layout frame: the floor it stands on (the Hub bar's beds are upstairs)
	// A bed's pose (SIMLINK CHAR_DYNAMIC pose tail, kinspect bed_seat_pose.md 2): the sleeper's facing in the layout frame (its +Z is
	// the use node's -X) and the clip of the bed's functionality `animation`.
	double faceX = 0.0;
	double faceZ = 1.0;
	std::string clip{};
	std::string record{}; // the furniture BUILDING record (kind 5 Bed's `furniture`)
	// A bed's use node height over `y` (layout frame, rotated by the piece): the original lays the body on the node itself, a few
	// units above the floor (kinspect bed_seat_pose.md); the walk still aims at the floor point.
	double nodeY = 0.0;
	// A cage's occupant position: the cage's own centre (the layout piece origin). `x`, `z` are the use node beside it, the walkers' spot
	// (k16.6.370: the prisoner was put at the node and stood outside the bars).
	bool hasIn = false;
	double inX = 0.0;
	double inZ = 0.0;
};

// Classifies a layout's pieces against the BUILDING records (None are dropped).
std::vector<Piece> classifyLayout(const kdata::GameData& data, const Layout& layout);

// The use nodes of a furniture BUILDING: its instances of a node record (`is node`, `node type` kNodeUse), in the piece's own
// frame, scaled by its `scale` (the Bed has two, 11 u either side; kinspect findings/bar_beds.md).
std::vector<std::array<double, 3>> useNodes(const kdata::GameData& data, const kdata::Record& building);
// The same nodes with their rotation (w, x, y, z), building frame.
struct UseNode
{
	std::array<double, 3> at{};
	std::array<double, 4> q{1.0, 0.0, 0.0, 0.0};
};
std::vector<UseNode> useNodePoses(const kdata::GameData& data, const kdata::Record& building);
// v rotated by the unit quaternion q (w, x, y, z).
std::array<double, 3> rotate(const std::array<double, 4>& q, const std::array<double, 3>& v) noexcept;
// The ANIMATION record name of the BUILDING's functionality `animation` ref (bed_seat_pose.md 1, 3), `fallback` when it has none.
std::string functionClip(const kdata::GameData& data, const kdata::Record& building, std::string_view fallback);
inline constexpr std::string_view kClipSleepInBed = "sleepinbed"; // a bed without its own clip (Bed, Old Bed: rebirth.mod adds it)
inline constexpr std::string_view kClipSleepOnFloor = "sleeponfloor"; // a synthetic bed: no furniture, the original's floor sleeping
inline constexpr std::string_view kClipSittingChair = "sitting chair";
inline constexpr std::string_view kClipSittingIdle = "sitting idle"; // the floor-sit of a STAY_IN_HOME resident with no chair (ANIMATION 2623-gamedata.base; male and female skeletons)

// A point of a layout piece's own frame in the layout's frame (building-local), xz: the piece position plus its rotation of `local`.
std::array<double, 2> pieceToLayout(const LayoutPiece& piece, const std::array<double, 3>& local) noexcept;

// The layout's beds as Bed pieces at their sites, layout frame, in layout order. A bed's site is its use node, beside it (the bed itself
// is a hole in the navmesh). A node nearer another bed than its own is blocked (bed A's second node in the Hub bar falls in bed C); of
// the rest, the one nearest the layout origin (beds stand against walls: the wall-side node of bed B is off the floor). The three Hub
// bar beds land within 0.1 u of the nodes kinspect measured (findings/bar_beds.md). One site per bed: one sleeper. A bed without a free
// node keeps its centre. UNSURE: the original takes the free node nearest the sleeper; "nearest the origin" stands in for its blocked test.
std::vector<Piece> bedPieces(const kdata::GameData& data, const Layout& layout);

// What the index needs to know about one placed building. Coordinates are xz on the plane.
struct BuildingSpec
{
	std::uint64_t key = 0; // the placement (DoorSite::building)
	std::string buildingSid;
	std::int32_t designation = 0; // SquadTemplate::buildingDesignation: 3 BAR, 2 BARRACKS, 1 SHOP, ...
	bool publicBuilding = false; // publicDay / publicNight of the owning template
	bool publicBeds = false;
	std::string ownerFaction; // the placement's `owner faction ID` ("" = none): the 153 / 239 ladder's allied test
	double cx = 0.0; // the building centre
	double cz = 0.0;
	double doorX = 0.0; // the main door, outside side
	double doorZ = 0.0;
	double yawCos = 1.0; // layout rotation: world = centre + R * local, R = [[c, s], [-s, c]] on (x, z). UNSURE: handedness, first used at T3b2
	double yawSin = 0.0;
	int guardCount = 0; // the template's guard count: synthetic guard nodes, at least kMinSyntheticGuardNodes
	int memberCount = 1; // the home squad's member count: synthetic beds
	std::vector<Piece> pieces; // real furniture and nodes, building-local; empty = all synthetic
	bool hasHeight = false; // the pieces' y are heights over originY (else real sites have no height either)
	double originY = 0.0; // the base of the pieces' heights; the layout path places with the placement's full rotation (tilt included,
	// so a tilted building's floor y differs piece from piece) and stores world heights, originY 0
};

struct SiteEntry
{
	Site site;
	double x = 0.0;
	double z = 0.0;
	bool synthetic = false;
	bool hasY = false; // a real piece of a layout: y is its floor; otherwise the walk aims at the terrain
	double y = 0.0;
	bool hasNodeY = false; // a real bed's use node height (y above the floor): the pose tail and the kind 5 `use` y
	double nodeY = 0.0;
	double faceX = 0.0; // a real bed's pose facing, world (Piece::faceX)
	double faceZ = 1.0;
	std::string clip{}; // a real bed's clip ("" otherwise)
	std::string record{}; // a real bed's furniture BUILDING record ("" for synthetic sites)
	bool hasIn = false; // a cage: the occupant stands at (inX, inZ), the cage's centre, not at the walk spot (x, z)
	double inX = 0.0;
	double inZ = 0.0;
	double occupantX() const noexcept { return hasIn ? inX : x; }
	double occupantZ() const noexcept { return hasIn ? inZ : z; }
};

struct BuildingSites
{
	std::uint64_t key = 0;
	std::string buildingSid;
	std::int32_t designation = 0;
	bool publicBuilding = false;
	bool publicBeds = false;
	std::string ownerFaction; // BuildingSpec::ownerFaction
	double cx = 0.0;
	double cz = 0.0;
	std::vector<SiteEntry> sites; // kind order, then slot
};

class SiteIndex
{
public:
	// Adds one building: the real sites of its pieces, and a synthetic set for every kind the real pieces do not provide
	// (HomePoint always; Bed only with kSyntheticBeds; TownNode never: the outside nodes of a town are not found in the data,
	// UNSURE). A real site always wins. Re-adding a key replaces it.
	void add(const BuildingSpec& spec);
	// Adds every building of one town's list at once.
	void add(const std::vector<BuildingSpec>& specs);

	const BuildingSites* find(std::uint64_t buildingKey) const noexcept;
	// Every building key, ascending: a scan for a site by its id (siteId) needs them.
	std::vector<std::uint64_t> keys() const;
	// The sites of one kind of one building, slot order.
	std::vector<const SiteEntry*> sitesOf(std::uint64_t buildingKey, SiteKind kind) const;
	const SiteEntry* siteAt(const Site& site) const noexcept;
	std::size_t buildings() const noexcept { return buildings_.size(); }
	// Changes on every add and is unique per index built in the process: a cache of something derived from the sites is stale when it differs.
	std::uint64_t revision() const noexcept { return revision_; }

private:
	static std::uint64_t nextRevision() noexcept;
	std::map<std::uint64_t, BuildingSites> buildings_;
	std::uint64_t revision_ = nextRevision();
};

// ---------------------------------------------------------------------------
// The reservation book (5.4). Not saved: rebuilt at load from every loaded member's Routine::target.
// ---------------------------------------------------------------------------

struct Claim
{
	std::uint64_t owner = 0; // a member Id; a table is claimed by its squad Id
	std::uint64_t since = 0; // tick
};

class Reservations
{
public:
	// Takes `site` for `owner`. An owner holds one site at a time: a successful take releases the owner's previous one. A
	// site already held by the same owner is kept (its `since` is not reset) and succeeds; one held by another fails and
	// changes nothing.
	bool take(const Site& site, std::uint64_t owner, std::uint64_t tick);
	// Releases the owner's site, if any. True when it held one.
	bool release(std::uint64_t owner);
	// Releases `site` only when `owner` holds it.
	bool release(const Site& site, std::uint64_t owner);
	// The claim on a site, nullptr when free.
	const Claim* holder(const Site& site) const noexcept;
	// The owner's site; kind None when it holds none.
	Site held(std::uint64_t owner) const noexcept;
	bool free(const Site& site) const noexcept { return holder(site) == nullptr; }
	// Releases every claim older than maxTicks (kReservationMaxHours); returns how many.
	std::size_t expire(std::uint64_t now, std::uint64_t maxTicks);
	// Releases every claim whose owner `alive(owner)` rejects (a dead or demoted member, a squad out of routine mode).
	template <typename Alive>
	std::size_t releaseDead(Alive&& alive)
	{
		std::vector<std::uint64_t> gone;
		for (const auto& [owner, site] : byOwner_)
			if (!alive(owner))
				gone.push_back(owner);
		for (const std::uint64_t owner : gone)
			release(owner);
		return gone.size();
	}
	// The load-time rebuild: like take, but on a conflict the lower owner Id keeps the site. Returns false when `owner` lost
	// (the caller re-scores it).
	bool adopt(const Site& site, std::uint64_t owner, std::uint64_t since);
	std::size_t size() const noexcept { return bySite_.size(); }
	bool empty() const noexcept { return bySite_.empty(); }
	void clear();

private:
	std::map<Site, Claim> bySite_;
	std::map<std::uint64_t, Site> byOwner_;
};

} // namespace ksim::routines
