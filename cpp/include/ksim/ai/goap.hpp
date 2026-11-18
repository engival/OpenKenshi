#pragma once

// G4, the original's GOAP task planner (kinspect dt18_goap_search.md; RUNTIME_SYSTEMS G4). NOT a forward state-space search: the
// search goes BACKWARD from the goal task. A node is one candidate task; its "state" is the set of the task's preconditions (and, for
// the goal task, its goal state) that the LIVE world does not satisfy - an evaluator call, not a model - each mapped to the tasks whose
// effects provide that symbol. A plan is a parent chain of task types, acceptable when every step's needs are met by the world or by a
// step before it.
//
// Ordering (dt18 section 3): the open set is an unsorted vector, and the candidate taken is the max of g+h, where h = the task's own
// utility and g = the weakest-link utility along the chain (the minimum, relaxed as min). Limits (section 5): 4 visits per
// (task, target, position) and a 50-link cycle guard. No plan: the goal scores 0 and the next update re-plans it - never a terminal
// state (section 6).
//
// Pure: no Simulation, no flecs, no save. The caller supplies a World (the live evaluators) and runs the Plan.

#include <cstdint>
#include <string_view>
#include <vector>

namespace ksim::goap {

// Task ids the plans use that routines::task does not name (kinspect GATES_CARRY_STATIC section 2, s58 TQ12; dt24_58.md for 25).
inline constexpr std::uint16_t kRunning = 67; // "Running": the walk to the plan's position
inline constexpr std::uint16_t kPutInCage = 108; // "Put in prison"
inline constexpr std::uint16_t kPutInBed = 99; // PUT_SOMEONE_IN_BED (Task_PutInSomething, goal isCarriedDudeInBed; dt24_plantasks)
inline constexpr std::uint16_t kChainTarget = 166; // "Chain target": the restraining bar and the shackles
inline constexpr std::uint16_t kStripWeapons = 181; // STRIP_TARGETS_WEAPONS
inline constexpr std::uint16_t kMeleeAttack = 4; // MELEE_ATTACK (the combat goals' action step, dt24_16)
// The ranged attack tasks (task ds/ranged S2; GATES_CARRY_STATIC section 2: 262 / 263 / 266 Task_RangedAttack, 264 Task_EquipBestWeapon with the
// crossbow flag, 235 Task_UseTurret's shot). The exe remaps the attack order to them when the hand holds a crossbow (CURSORS_LIVE C4).
inline constexpr std::uint16_t kUseTurret = 146; // USE_TURRET (task ds/turret S3; Task_UseTurret, dt24_stalk_turret section 3: the mount)
inline constexpr std::uint16_t kTurretPlayerJob = 234; // MAN_A_TURRET_PLAYER_JOB (dt34_flee_or_fight: the player job, never planner-selected)
inline constexpr std::uint16_t kShootAtTarget = 235;
inline constexpr std::uint16_t kStealthKnockout = 228; // STEALTH_KNOCKOUT (task ds/stealth-ko S4; Task_StealthKO, dt41_C: a player order / dialogue / sub-goal only)
inline constexpr std::uint16_t kRangedAttack = 262;
inline constexpr std::uint16_t kRangedAttackFocused = 263;
inline constexpr std::uint16_t kEquipCrossbow = 264;
inline constexpr std::uint16_t kRangedAttackFocusedUnprovoked = 266;
inline constexpr std::uint16_t kAttackAttackersOf = 13; // ATTACK_ATTACKERS_OF (the only provider of 143's and 15's goal states)
// The Task_Blank combat nodes and the field-medic rows (dt24_plantasks sections 4-5, dt24_smalltasks section 6, item20_static).
inline constexpr std::uint16_t kChooseEnemyAndAttack = 9;
inline constexpr std::uint16_t kChooseAttackerOfAlly = 10;
inline constexpr std::uint16_t kAttackCharactersAttacker = 11;
inline constexpr std::uint16_t kGetReadyForAction = 59; // GET_READY_FOR_ACTION: stop and become ready (needs a weapon in hand)
inline constexpr std::uint16_t kMakeTargetStandStill = 64;
inline constexpr std::uint16_t kWorshipTarget = 236;
inline constexpr std::uint16_t kSplintOrder = 249;
inline constexpr std::uint16_t kSplintJob = 250;
inline constexpr std::uint16_t kGetNearTo = 40; // GET_NEAR_TO (Task_Move): the walk in front of an attack task (dt24_16)
inline constexpr std::uint16_t kLiftPerson = 68; // LIFT_PERSON (Task_LiftSomething): the generic lift the intruder goal 223 plans
// The other Task_LiftSomething rows (dt24_plantasks section 6, dt26, item25): one executor, the 68 lift. 170 adds the heal-first precondition,
// 213 / 240 are the guards' arrest lifts (240 only inside the home town), 225 the player's order, 246 the kidnapper's order (Task_Kidnapping).
inline constexpr std::uint16_t kLiftObjectHealFirst = 170;
inline constexpr std::uint16_t kLiftPersonSnatching = 213;
inline constexpr std::uint16_t kLiftPersonPlayerOrder = 225;
inline constexpr std::uint16_t kLiftPersonSnatchingInTown = 240;
inline constexpr std::uint16_t kKidnapOrder = 246;
inline constexpr bool isLiftTask(std::uint16_t task) noexcept
{
	return task == kLiftPerson || task == kLiftObjectHealFirst || task == kLiftPersonSnatching || task == kLiftPersonPlayerOrder
		|| task == kLiftPersonSnatchingInTown || task == kKidnapOrder;
}
inline constexpr std::uint16_t kSendDialogue = 161; // SEND_DIALOGUE_SIGNAL_WITHOUT_MOVING (Task_TalktoPlayer): the shoo goal 283's step
inline constexpr std::uint16_t kFirstAidOrder = 25; // FIRST_AID_ORDER -> Task_FirstAid (task CG)
inline constexpr std::uint16_t kHealMyLegs = 269; // HEAL_MY_LEGS -> Task_FirstAid on myself (k16.6.4, dt45_down_treatment.md)
// The machine/storage goals that plan a pull task (dt24_113 / dt24_115 / dt24_131, ai_enum_map's registry rows):
inline constexpr std::uint16_t kWantToFillMachine = 94; // WANT_TO_FILL_MACHINE: goal haveSomeResourcesFrom -> 92
inline constexpr std::uint16_t kFindSomeBuildMats = 115; // FIND_SOME_BUILDING_MATERIALS: goal HAVE_SOME_BUILD_MATS -> 247
inline constexpr std::uint16_t kLootFoodAndStuff = 131; // LOOT_FOOD_AND_STUFF: goal HAVE_SOME_LOOT_FROM -> 284
inline constexpr std::uint16_t kOperateMachinery = 87; // OPERATE_MACHINERY: goal IS_WORKING, plans 92 when the output is full
// The door-task rows (task ds/order-tasks; dt24_plantasks, GATES_CARRY_STATIC a2). The "here" variants (74 / 75 / 79 / 80 / 287) act on the
// door at the actor's current location and carry no AT_LOCATION precondition of their own (UNSURE: the registry rows are not listed).
inline constexpr std::uint16_t kOpenDoor = 72;
inline constexpr std::uint16_t kCloseDoor = 73;
inline constexpr std::uint16_t kOpenDoorHere = 74;
inline constexpr std::uint16_t kCloseDoorHere = 75;
inline constexpr std::uint16_t kPickLock = 76;
inline constexpr std::uint16_t kLockDoor = 77;
inline constexpr std::uint16_t kUnlockDoor = 78;
inline constexpr std::uint16_t kLockDoorHere = 79;
inline constexpr std::uint16_t kUnlockDoorHere = 80;
inline constexpr std::uint16_t kBashDoor = 81;
inline constexpr std::uint16_t kBashGate = 123;
inline constexpr std::uint16_t kUnlockDoorOrder = 140;
inline constexpr std::uint16_t kLockDoorFromInside = 162;
inline constexpr std::uint16_t kLockDoorFromOutside = 198;
inline constexpr std::uint16_t kBashDoorOrder = 226;
inline constexpr std::uint16_t kBashDoorHere = 287;
inline constexpr std::uint16_t kMoveToBuildingDoor = 82; // Task_MoveToDoor (dt24_movetodoor)
inline constexpr std::uint16_t kMoveToCurrentDoor = 83; // Task_MoveToDoor_CurrentLocation
inline constexpr std::uint16_t kMoveToDoorInside = 163; // Task_MoveToDoor_Inside
inline constexpr std::uint16_t kMoveToDoorOutside = 199; // Task_MoveToDoor_Outside
inline constexpr std::uint16_t kEquipWeapon = 6;
inline constexpr std::uint16_t kUseBed = 98;
inline constexpr std::uint16_t kPlayerSleep = 258; // EQUIP_WEAPON (the door plans' local row: pre hasWeaponCarried, effect hasWeaponEquipped)
inline constexpr std::uint16_t kStayAtShopkeeperNode = 20; // STAND_AT_SHOPKEEPER_NODE: goal atNode (dt24_20)
inline constexpr std::uint16_t kRelaxInTownPackage = 214; // RELAX_IN_TOWN_PACKAGE: goal IS_WORKING (dt24_emptymachine_source)
inline constexpr std::uint16_t kAutoLabouringMines = 189; // AUTO_LABOURING_MINES: goal IS_WORKING

// The task-state symbols this slice models (dt18 section 2 / s58: the registry has 169; these are the slaver chain's and, since task
// CG, the medic's). The names are the registry's, where kinspect decoded them.
enum class Sym : std::uint8_t
{
	AtLocation = 0, // 60 LOCATION_IS_ACCESSIBLE, read as "the actor stands at the plan's position" (67's effect: 46 "at target")
	IsTargetDown, // 7 isTargetDown
	TargetIsUnarmed, // the plan target wears no weapon (181's effect; OURS: the registry's name is not decoded)
	HasOneWorkingArm, // "one working arm" (SLAVERY section 4, the chain's precondition)
	IsMovementAllowed, // 27
	IsCarryingTarget, // 48 IS_CARRYING_TARGET
	CarriedDudeIsDead, // the carried body is dead (112's "carried dude not dead", negated)
	CagesAreAllFull, // CAGES_ARE_ALL_FULL (ORIGINAL_AI section 5's sample)
	IsChained, // 134 isChained (the shackles)
	CarriedDudeInCage, // 81 CARRIED_DUDE_NOW_IN_CAGE
	IsAggressionTowards, // 16 isAggressionTowards: something is aggressing against the hand (143's goal: false; 13's effect)
	IsAlliesFighting, // 12 isAlliesFighting (15's goal: false; 13's effect)
	IsEnemy, // symbol 6, the registry's isEnemy (16 / 144's goal: true; the attack tasks' effect)
	ReadyForAction, // isReadyForAction (melee 4's precondition)
	IsWithin50Meters, // isWithin50Meters (melee 4's precondition)
	IsGeneralAggressionLevel, // symbol 29: the goal node 34's effect (dt24_34). No modelled task requires it (175 / 261 do in the exe).
	AtDestinationTown, // 94 atDestinationTown (36 PATROL_TOWN's first precondition, dt24_36)
	HasWeaponEquipped, // 5 / 157 hasWeaponEquipped (36 / 22 need none, 216 a weapon; dt24_36 section 1)
	IsCrouchingOrLying, // 26 (36 / 22)
	IsCarryingSomething, // 47 IS_CARRYING_SOMETHING: 36's precondition; 223's (false) and 224's (true) (task CH)
	IAmInsideTargetBuilding, // 107 I_AM_INSIDE_TARGET_BUILDING: 216's goal state (dt24_36); 283's precondition (task CH)
	IsCarryingTargetHealthy, // IS_CARRYING_TARGET_HEALTHY: the registry's 168 goal / 170's effect (the slaver lift), not 68's symbol
	IsTargetImprisoned, // TARGET_IS_IMPRISONED: 68's precondition (false)
	IntruderIsOutside, // 137 INTRUDER_IS_OUTSIDE: 224's effect
	IntruderIsOutsideGates, // 164 INTRUDER_IS_OUTSIDE_GATES: 274's effect (the town take-out, dt24_273)
	AlarmsInTheVicinity, // 130 ALARMS_IN_THE_VICINITY: 283's precondition (false)
	StrangersInHouse, // 108 "strangers in house": 283's effect (false)
	IsDialogDelivered, // 110 isDialogDelivered: 283's goal state, 161's effect
	NeedsFirstAid, // sym 22 (dt24_58.md): 1 = the patient still needs aid; goal 58's state is 0 and 25 is its only writer
	AtATown, // 86 atATown: SHOPPING 118's precondition (dt24_118)
	ShoppingDone, // 85, the registry's unnamed `?local_2e60`: 118's goal state, 119 BUY_SHIT's effect (dt24_shop)
	// The machine/storage goals (dt24_emptymachine_dismantle; dt24_113 / dt24_115 / dt24_131). The registry's names:
	GotInventoryRoomForResources, // 82: the actor can carry more of the item (92 / 247 / 284's precondition; ours: a probe)
	HaveSomeResourcesFrom, // 47: I hold something out of that machine (92's effect, 94's goal)
	IsMachineOutputFull, // 92's effect (= 0 after a pull); the machine's slot cap is reached otherwise (113's precondition)
	IsMachineInputInvalid, // 92's effect (= 0); a crafting machine whose inputs are wrong (not modelled in ours: false)
	HaveSomeBuildMats, // HAVE_SOME_BUILD_MATS (68): 247's effect, 115's goal
	HaveSomeLootFrom, // HAVE_SOME_LOOT_FROM (68): 284's effect, 131's goal
	IsWorking, // 123 IS_WORKING: 87 OPERATE_MACHINERY's effect; the goal state of 214 and 189 (dt24_emptymachine_source)
	CarriedDudeInDisposal, // 100 CARRIED_DUDE_NOW_IN_DISPOSAL_MACHINE: 137's goal state, 136's effect (the exe's evaluator is "I carry nothing")
	// The occupied-town goals (dt24_243 / dt24_275) and the animal floor sleep (dt24_175). The registry's names:
	GotAFoodItem, // 149 gotAFoodItem: 243 AQUIRE_FOOD_AT_HOMEBASE's goal state, 244 GRAB_ONE_FOOD's effect
	IsWorkingPretend, // 136 IS_WORKING_PRETEND: 275 SIT_AROUND's goal state; 190 / 221's effect
	AtHomeTown, // 95 isAtHomeTown: 175's precondition (false)
	IsFullyRested, // 72 isFullyRested: 175's effect
	NoEnemiesInVicinity, // the 175 row's symbol 6 (s58). The kinspect note reads that precondition set as "not in a fight"
	// and a registered atom is isNoEnemiesInVicinity (the sensor's 0xbc hostile count, PERCEPTION_STATIC section 3); the port
	// reads symbol 6 as that atom (UNSURE: the note also names it isEnemy, which 16 / 144 use for "there is an enemy").
	IAmImprisoned, // I_AM_IMPRISONED: 172 SLAVE_GET_IN_MY_MASTERS_CAGE's goal state, 107 USE_CAGE's effect (dt24_plantasks)
	MachineHasFreeOperatorSlot, // machineHasFreeOperatorSlot: 107's precondition; ours reads the picked cage's free slot
	FinalKidnapVictim, // 153 FINAL_GOT_A_KIDNAP_VICTIM: 132 FIND_AND_KIDNAP's effect and 254's goal test (dt26 section 3)
	IsInBed, // 83 isInBed (char+0x2f8 == 1, s63): 116 / 208 / 257 provide it = 0 (dt24_208)
	CarriedDudeInBed, // isCarriedDudeInBed: 99's effect and 106's goal state (dt24_106)
	IsDoorLocked, // isDoorLocked: 207 / 256 GET_OUT_OF_CAGE's precondition = 0; ours the cage's warden squad locked its cages (OURS, UNSURE)
	IAmBeingMeleeAttacked, // iAmBeingMeleeAttacked: 248's precondition (false); ours: a member targeting me within melee reach
	CanGetUp, // 45 canGetUp: the three get-out-of-bed rows' precondition; the evaluator is not decoded (ours: up, not carried - UNSURE)
	// The lock rows (task ds/shackles-locks; dt24_shackles, dt24_picklock, item28 b2). The registry's names:
	HasTools, // 122 hasTools: 185 / 285's precondition. The evaluator is not decoded (ours: a carried ITEM of function 9 TOOL - UNSURE)
	LockSkillOk, // iHaveSufficientLockSkill: 76's precondition (ours: the chance against the actor's cage clears 0.05, a caged picker at skill - 10)
	ShacklesLocked, // isWearingShackles_locked: 185 / 186 / 201's effect = 0, 219's precondition = 0 (ours: the plan target wears locked shackles)
	ShackleSkillOk, // iHaveSufficientLockSkillForShackles: 201's precondition (ours: the chance against the shackles' lock clears 0.05)
	TargetIsConscious, // TARGET_IS_CONSCIOUS: 110's precondition
	MachineOccupantIsAlive, // MACHINE_OCCUPANT_IS_ALIVE: 134 KILL_CAGE_OCCUPANT's effect = 0, the goal of 135
	TargetIsArrested, // 0x81 TARGET_IS_ARRESTED: 203's goal state (item25); no modelled task writes it, the goal node ends the plan
	BuildingDestroyed, // isBuildingDestroyed: 272 SMASH_BUILDING's goal state (dt24_plantasks section 4)
	TargetStandingStill, // targetStandingStill: 64 MAKE_TARGET_STAND_STILL's effect (the squad order 0x3e on the target, dt24_smalltasks section 3)
	TargetWorshiped, // "target worshiped": 236 WORSHIP_TARGET's effect (dt24_plantasks section 8)
	NeedsSplint, // needsSplint: 1 = the patient's limb still needs rigging; 249 SPLINT_ORDER clears it, 250 SPLINT_JOB's goal state is 0
	// The small-task rows (task ds/small-tasks; dt24_plantasks, dt24_47, dt24_eat, dt24_131). The registry's names:
	AtNode, // atNode: 47 / 37 / 51's goal state (the node resolvers, dt24_47)
	AtHome, // isAtHome: 19 GO_HOMEBUILDING's effect (dt24_47)
	IsSubjectInInventory, // isSubjectInInventory (3): 3 PICKUP's goal, 259 / 277's goal state
	AmHungry, // am hungry (144): 231 EAT_CROPS / 232 FIND_CROPS_TO_EAT's goal state = 0
	FarmHasFood, // the farm has food: 231's precondition (the evaluator is not decoded; no farm plots in the sim)
	LootedStorableItems, // LOOTED_STORABLE_ITEMS (148): 238 LOOT_ANIMALS_JOB's goal state
	CarryingExcessLoot, // CARRYING_EXCESS_LOOT: 184 PUT_LOOT_IN_STORAGE's effect = 0 (dt24_184; Simulation::carryingExcessLoot)
	Impossible, // sym 20: 104 ALWAYS_IMPOSSIBLE_TASK's precondition, which nothing provides (a deliberate failure)
	// The town-routine goals (task ds/town-port; dt24_86 / dt24_155 registry rows): the door goals' states and the lock effect.
	DoorIsOpen, // DOOR_IS_OPEN: 86 OPEN_UP_SHOP_DOORS' goal state
	DoorLockedInside, // DOOR_IS_LOCKED_AND_IM_INSIDE: 155 LOCK_ALL_MY_DOORS' goal state
	DoorLockedOutside, // DOOR_IS_LOCKED_AND_IM_OUTSIDE: 197 LOCK_ALL_MY_DOORS_STAY_OUTSIDE's goal state
	AllDoorsLocked, // allDoorsLocked: 155 / 197's effect true, 86's effect false
	// The door-task rows (task ds/order-tasks; dt24_plantasks, GATES_CARRY_STATIC a2, DOORS_LIVE 15b / 16). The registry's names:
	IsObjectMine, // isObjectMine (0x46): 77 / 78 / 79 / 80 / 162 / 198's precondition (the door's faction is mine or I own its hand)
	DoorIsBroken, // DOOR_IS_BROKEN: 73 CLOSE_DOOR's precondition = 0 (a broken door is open for ever)
	DoorBashable, // 81 / 123 / 287 "not mine, not an ally": the door's owner is hostile to me or its faction kind bashes (OURS, UNSURE: the exe's exact test)
	GateBashOpen, // 123 BASH_GATE's crowd limit (< 8 mates on the gate, nobody melee-attacking me; DOORS_LIVE section 16)
	AtBuildingDoor, // AT_LOCATION_BUILDING_DOOR: 72 / 73 / 77 / 78 / 81's precondition, 82 MOVE_TO_BUILDING_DOOR's effect (a reach test on the door)
	AtCurrentDoor, // AT_LOCATION_CURRENT_LOCATION_BUILDING_DOOR: the here variants' precondition, 83's effect (UNSURE: the symbol name)
	AtDoorInside, // AT_LOCATION_BUILDING_DOOR_INSIDEPOS: 162's precondition, 163's effect
	AtDoorOutside, // AT_LOCATION_BUILDING_DOOR_OUTSIDEPOS: 198's precondition, 199's effect
	HasWeaponCarried, // a melee weapon in the pack (the planner's 6 EQUIP_WEAPON precondition; door plans only, see door_plan.hpp)
	HasTwoWorkingArms, // hasTwoWorkingArm: 262 / 263 / 266's precondition (GATES_CARRY_STATIC section 2)
	OperatesTurret, // OURS (UNSURE): the character is mounted on a turret (Task_UseTurret's start has run, dt41_B); 235's precondition, 146 / 234's effect
	TurretFound, // OURS (UNSURE): the town finder (149 / 222) has a turret for the character. No combat world offers it: 146 / 234 are reached from the town goals
	StealthKoOrdered, // OURS (UNSURE): a stealth KO was ordered on the plan's hand (Fighter::stealthKo). 228's precondition: the row has no finder and no AI_TASK, so no combat world offers it
	HoldsCrossbow, // OURS (UNSURE): the weapon hand is a crossbow with bolts. The exe remaps the attack order to 262 / 263 / 266 by the hand type (CURSORS_LIVE C4), not by a symbol
	Count,
};

std::string_view symName(Sym sym) noexcept;

// One precondition: the symbol must have `value` in the world ("false" = the symbol must not hold, e.g. no weapon, no dead body).
struct Precondition
{
	Sym sym = Sym::Count;
	bool value = true;
};

// One effect: the task makes the symbol `value` once it runs.
struct Effect
{
	Sym sym = Sym::Count;
	bool value = true;
};

// One task type's GOAP row (the registry's preconditions / effects / goal state). A row with `goalState` set is a goal task: the
// planner plans its goal state first, then its preconditions.
struct TaskRow
{
	std::uint16_t task = 0;
	const char* name = "";
	double utility = 1.0; // h (and g's start): the task type's own utility with no target (dt18 section 3)
	Precondition goalState{}; // the goal the task is planned for; sym Count = none
	bool hasGoalState = false;
	bool orderOnly = false; // a player order, never a planner provider (item28 b2: BRUTE_FORCE_LOCK 286; ours: 186 too)
	Precondition pre[8]{};
	std::uint8_t preCount = 0;
	Effect eff[4]{};
	std::uint8_t effCount = 0;
	// A step the actor does not stand still through: the position an earlier step reached no longer holds after it, so a later
	// AT_LOCATION need is decided by the live world again. The lift (168) carries the body from where it fell to the cage.
	bool breaksLocation = false;
};

// The registry: the rows this slice models (the slaver chain, 67 -> 181 -> 166 -> 167; the caging chain 112 -> 67 -> 108, with the
// lift 168 in front of it when the member is not carrying; the combat-joining goals 16 -> 13 -> 4, 15 and 143 over the same 13 and
// 4 - dt24_16 / dt24_143 / dt24_15; the intruder eviction 223 -> 67 -> 68, then 224 - dt24_223). nullptr for a task it does not list
// (then no plan can use it, as a provider or as a goal).
const TaskRow* rowOf(std::uint16_t task) noexcept;
// Whether the task row has the precondition hasWeaponEquipped = 0 (a peaceful goal): the planner puts 7 UNEQUIP_WEAPON in front of it.
bool needsUnarmed(std::uint16_t task) noexcept;
const std::vector<TaskRow>& rows() noexcept;

// The live world: the evaluators (AI vtable[0], dt18 section 4). `x, z` is the plan position (the goal's; a node inherits it).
class World
{
public:
	virtual ~World() = default;
	virtual bool holds(Sym sym, std::uint64_t target, double x, double z) const = 0;
	// The task's utility for this target when it is not the row's static one (the registry's utility function, dt18 section 3; the
	// door bash scores by faction kind and crowd). A negative answer = the row's own `utility`.
	virtual double utility(std::uint16_t task, std::uint64_t target) const { (void)task; (void)target; return -1.0; }
};

// One planned step, in execution order.
struct Step
{
	std::uint16_t task = 0;
	std::uint64_t target = 0; // the plan hand (inherited; 0 = none)
	double g = 1.0;
};

struct Plan
{
	std::vector<Step> steps; // leaf -> goal (the task's own order of execution); empty = no plan
	double score = 0.0; // the minimum utility along the chain; 0 = no plan (dt18 section 6)
	bool found() const noexcept { return !steps.empty() && score > 0.0; }
};

inline constexpr std::uint8_t kVisitCap = 4; // dt18 section 5: identical (task, target, position) nodes per call
inline constexpr std::size_t kCycleGuard = 50; // dt18 section 5: the ancestor walk's length

// planGoal (dt18 section 10): the backward search for `task`, at `target` / (x, z). The returned steps run in order; the last is the
// goal task itself when it materializes. A task the registry does not list, a needed symbol with no provider, or a cycle gives Plan{}
// (score 0). The overload takes a task table (the test's cycle and dead-end registries).
Plan plan(const World& world, std::uint16_t task, std::uint64_t target, double x, double z);
Plan planWith(const World& world, std::uint16_t task, std::uint64_t target, double x, double z,
	const std::vector<TaskRow>& table);
// planWith, but every need tries ALL its providers and keeps the one whose chain has the best weakest link (ties: the first, the
// provider order). dt18's search takes the max of g+h off the open set, so a 1.0 bash beats a 0.1 pick-then-open even though the
// open row is listed first; the depth-first plan() would stop at the first provider that works. Door plans use it.
Plan planBestWith(const World& world, std::uint16_t task, std::uint64_t target, double x, double z,
	const std::vector<TaskRow>& table);

} // namespace ksim::goap
