#include "ksim/goap.hpp"

#include "ksim/routines.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <tuple>
#include <utility>

namespace ksim::goap {

std::string_view symName(Sym sym) noexcept
{
	switch (sym)
	{
	case Sym::AtLocation: return "AT_LOCATION";
	case Sym::IsTargetDown: return "isTargetDown";
	case Sym::TargetIsUnarmed: return "TARGET_IS_UNARMED";
	case Sym::HasOneWorkingArm: return "hasOneWorkingArm";
	case Sym::IsMovementAllowed: return "isMovementAllowed";
	case Sym::IsCarryingTarget: return "IS_CARRYING_TARGET";
	case Sym::CarriedDudeIsDead: return "isCarriedDudeDead";
	case Sym::CagesAreAllFull: return "CAGES_ARE_ALL_FULL";
	case Sym::IsChained: return "isChained";
	case Sym::CarriedDudeInCage: return "CARRIED_DUDE_NOW_IN_CAGE";
	case Sym::CarriedDudeInBed: return "isCarriedDudeInBed";
	case Sym::IsAggressionTowards: return "isAggressionTowards";
	case Sym::IsAlliesFighting: return "isAlliesFighting";
	case Sym::IsEnemy: return "isEnemy";
	case Sym::ReadyForAction: return "isReadyForAction";
	case Sym::IsWithin50Meters: return "isWithin50Meters";
	case Sym::IsGeneralAggressionLevel: return "isGeneralAggressionLevel";
	case Sym::AtDestinationTown: return "atDestinationTown";
	case Sym::HasWeaponEquipped: return "hasWeaponEquipped";
	case Sym::IsCrouchingOrLying: return "IS_CROUCHING_OR_LYING";
	case Sym::IsCarryingSomething: return "IS_CARRYING_SOMETHING";
	case Sym::IAmBeingMeleeAttacked: return "iAmBeingMeleeAttacked";
	case Sym::IAmInsideTargetBuilding: return "I_AM_INSIDE_TARGET_BUILDING";
	case Sym::IsCarryingTargetHealthy: return "IS_CARRYING_TARGET_HEALTHY";
	case Sym::IsTargetImprisoned: return "TARGET_IS_IMPRISONED";
	case Sym::IntruderIsOutside: return "INTRUDER_IS_OUTSIDE";
	case Sym::IntruderIsOutsideGates: return "INTRUDER_IS_OUTSIDE_GATES";
	case Sym::AlarmsInTheVicinity: return "ALARMS_IN_THE_VICINITY";
	case Sym::StrangersInHouse: return "STRANGERS_IN_HOUSE";
	case Sym::IsDialogDelivered: return "isDialogDelivered";
	case Sym::NeedsFirstAid: return "needsFirstAid";
	case Sym::AtATown: return "atATown";
	case Sym::ShoppingDone: return "?local_2e60";
	case Sym::GotInventoryRoomForResources: return "gotInventoryRoomForResources";
	case Sym::HaveSomeResourcesFrom: return "haveSomeResourcesFrom";
	case Sym::IsMachineOutputFull: return "isMachineOutputFull";
	case Sym::IsMachineInputInvalid: return "isMachineInputInvalid";
	case Sym::HaveSomeBuildMats: return "HAVE_SOME_BUILD_MATS";
	case Sym::HaveSomeLootFrom: return "HAVE_SOME_LOOT_FROM";
	case Sym::IsWorking: return "IS_WORKING";
	case Sym::CarriedDudeInDisposal: return "CARRIED_DUDE_NOW_IN_DISPOSAL_MACHINE";
	case Sym::GotAFoodItem: return "gotAFoodItem";
	case Sym::IsWorkingPretend: return "IS_WORKING_PRETEND";
	case Sym::AtHomeTown: return "isAtHomeTown";
	case Sym::IsFullyRested: return "isFullyRested";
	case Sym::NoEnemiesInVicinity: return "isNoEnemiesInVicinity";
	case Sym::IAmImprisoned: return "I_AM_IMPRISONED";
	case Sym::MachineHasFreeOperatorSlot: return "machineHasFreeOperatorSlot";
	case Sym::FinalKidnapVictim: return "FINAL_GOT_A_KIDNAP_VICTIM";
	case Sym::IsInBed: return "isInBed";
	case Sym::IsDoorLocked: return "isDoorLocked";
	case Sym::CanGetUp: return "canGetUp";
	case Sym::HasTools: return "hasTools";
	case Sym::LockSkillOk: return "iHaveSufficientLockSkill";
	case Sym::ShacklesLocked: return "isWearingShackles_locked";
	case Sym::ShackleSkillOk: return "iHaveSufficientLockSkillForShackles";
	case Sym::TargetIsConscious: return "TARGET_IS_CONSCIOUS";
	case Sym::MachineOccupantIsAlive: return "MACHINE_OCCUPANT_IS_ALIVE";
	case Sym::TargetIsArrested: return "TARGET_IS_ARRESTED";
	case Sym::BuildingDestroyed: return "isBuildingDestroyed";
	case Sym::TargetStandingStill: return "targetStandingStill";
	case Sym::TargetWorshiped: return "targetWorshiped";
	case Sym::NeedsSplint: return "needsSplint";
	case Sym::AtNode: return "atNode";
	case Sym::AtHome: return "isAtHome";
	case Sym::IsSubjectInInventory: return "isSubjectInInventory";
	case Sym::AmHungry: return "am hungry";
	case Sym::FarmHasFood: return "farmHasFood";
	case Sym::LootedStorableItems: return "LOOTED_STORABLE_ITEMS";
	case Sym::CarryingExcessLoot: return "CARRYING_EXCESS_LOOT";
	case Sym::Impossible: return "sym20";
	case Sym::DoorIsOpen: return "DOOR_IS_OPEN";
	case Sym::DoorLockedInside: return "DOOR_IS_LOCKED_AND_IM_INSIDE";
	case Sym::DoorLockedOutside: return "DOOR_IS_LOCKED_AND_IM_OUTSIDE";
	case Sym::AllDoorsLocked: return "allDoorsLocked";
	case Sym::IsObjectMine: return "isObjectMine";
	case Sym::DoorIsBroken: return "DOOR_IS_BROKEN";
	case Sym::DoorBashable: return "doorBashable";
	case Sym::GateBashOpen: return "gateBashOpen";
	case Sym::HasWeaponCarried: return "hasWeaponCarried";
	case Sym::AtBuildingDoor: return "AT_LOCATION_BUILDING_DOOR";
	case Sym::AtCurrentDoor: return "AT_LOCATION_CURRENT_LOCATION_BUILDING_DOOR";
	case Sym::AtDoorInside: return "AT_LOCATION_BUILDING_DOOR_INSIDEPOS";
	case Sym::AtDoorOutside: return "AT_LOCATION_BUILDING_DOOR_OUTSIDEPOS";
	case Sym::HasTwoWorkingArms: return "hasTwoWorkingArm";
	case Sym::HoldsCrossbow: return "holdsCrossbow";
	case Sym::OperatesTurret: return "operatesTurret";
	case Sym::TurretFound: return "turretFound";
	case Sym::StealthKoOrdered: return "stealthKoOrdered";
	case Sym::Count: break;
	}
	return "?";
}

namespace {

// The registry rows this slice models (dt18's registry; the precondition sets are s58 TQ12 / SLAVERY section 4 / GATES_CARRY_STATIC
// section 2; the medic's are dt24_58.md). 167's goal state is isChained only (s58, H): the lift is not part of the capture goal but
// of the caging goal (112 plans 168 in front of itself when the member is not carrying). 58 JOB_MEDIC is Task_Blank like the other
// goal markers (dt24): its row is the goal state, and the work is Task_FirstAid's (25), the only task that clears needsFirstAid.
const std::vector<TaskRow>& table()
{
	static const std::vector<TaskRow> rows = []
	{
		std::vector<TaskRow> out;
		{
			TaskRow row;
			row.task = kRunning;
			row.name = "Running";
			row.pre[0] = {Sym::IsMovementAllowed, true};
			row.preCount = 1;
			row.eff[0] = {Sym::AtLocation, true};
			row.effCount = 1;
			row.breaksLocation = true; // the actor leaves the spot a later step needs
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = kStripWeapons;
			row.name = "STRIP_TARGETS_WEAPONS";
			row.pre[0] = {Sym::IsTargetDown, true};
			row.pre[1] = {Sym::AtLocation, true};
			row.preCount = 2;
			row.eff[0] = {Sym::TargetIsUnarmed, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = kChainTarget;
			row.name = "CHAIN_TARGET";
			row.pre[0] = {Sym::IsTargetDown, true};
			row.pre[1] = {Sym::TargetIsUnarmed, true};
			row.pre[2] = {Sym::HasOneWorkingArm, true};
			row.pre[3] = {Sym::AtLocation, true};
			row.preCount = 4;
			row.eff[0] = {Sym::IsChained, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::CarryWoundedSlaves;
			row.name = "CARRY_WOUNDED_SLAVES";
			row.pre[0] = {Sym::IsChained, true};
			row.pre[1] = {Sym::HasOneWorkingArm, true};
			row.pre[2] = {Sym::AtLocation, true}; // the lift is at the body, so a plan from elsewhere walks to it first
			row.preCount = 3;
			row.eff[0] = {Sym::IsCarryingTargetHealthy, true}; // the registry: 168 is a goal node on IS_CARRYING_TARGET_HEALTHY (170's effect)
			row.effCount = 1;
			row.breaksLocation = true; // the body travels in the member's arms: the next step needs a new position
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::CaptureSlaves;
			row.name = "CAPTURE_NEW_SLAVES";
			row.goalState = {Sym::IsChained, true};
			row.hasGoalState = true; // the cuff ends the capture; putting the person in its arms is the caging goal's (112)
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = kPutInCage;
			row.name = "Put in prison";
			// The registry's 108 precondition is IS_CARRYING_SOMETHING=1, which no registry task provides; ours reads it as "the member
			// carries the body" - the same live fact, and the symbol the lift 168 provides (OURS).
			row.pre[0] = {Sym::IsCarryingTargetHealthy, true};
			row.pre[1] = {Sym::AtLocation, true};
			row.preCount = 2;
			row.eff[0] = {Sym::CarriedDudeInCage, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = kPutInBed;
			row.name = "PUT_SOMEONE_IN_BED";
			row.pre[0] = {Sym::IsCarryingTargetHealthy, true}; // the same OURS reading as 108's IS_CARRYING_SOMETHING
			row.pre[1] = {Sym::AtLocation, true};
			row.preCount = 2;
			row.eff[0] = {Sym::CarriedDudeInBed, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::FindBedAndPutIn;
			row.name = "FIND_BED_AND_PUT_IN";
			row.goalState = {Sym::CarriedDudeInBed, true};
			row.hasGoalState = true;
			row.pre[0] = {Sym::CarriedDudeIsDead, false}; // 112's twin: a corpse never plans (dt24_106 lists no precondition; OURS)
			row.preCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::FindCageAndPutIn;
			row.name = "FIND_CAGE_AND_PUT_IN";
			row.goalState = {Sym::CarriedDudeInCage, true};
			row.hasGoalState = true;
			// s58: the carried dude must not be dead; CAGES_ARE_ALL_FULL is the cage finder's premise (ORIGINAL_AI section 5).
			row.pre[0] = {Sym::CarriedDudeIsDead, false};
			row.pre[1] = {Sym::CagesAreAllFull, false};
			row.preCount = 2;
			out.push_back(row);
		}
		{
			// 281 FIND_CAGE_AND_PUT_DEADGUY_IN (dt26 section 0): 112's row without the carried-dude-not-dead precondition.
			TaskRow row;
			row.task = routines::task::FindCageAndPutDeadGuyIn;
			row.name = "FIND_CAGE_AND_PUT_DEADGUY_IN";
			row.goalState = {Sym::CarriedDudeInCage, true};
			row.hasGoalState = true;
			row.pre[0] = {Sym::CagesAreAllFull, false};
			row.preCount = 1;
			out.push_back(row);
		}
		// The intruder eviction (task CH; dt24_223 sections 1-3). 223 is a Task_Blank goal node: its resolver picks the body, the plan
		// lifts it (68 LIFT_PERSON), and the next goal 224 walks it out of the home. The registry rows:
		//   68 LIFT_PERSON      pre isCarryingArmOK / isMovementAllowed / isAtLocation / isTargetDown   effect IS_CARRYING_TARGET
		//   223 PICKUP_INTRUDERS pre IS_CARRYING_SOMETHING=0                                            goal IS_CARRYING_TARGET
		//   224 TAKE_INTRUDER_OUTSIDE pre IS_CARRYING_SOMETHING=1, LOCATION_IS_ACCESSIBLE              effect INTRUDER_IS_OUTSIDE
		{
			TaskRow row;
			row.task = kLiftPerson;
			row.name = "LIFT_PERSON";
			row.pre[0] = {Sym::HasOneWorkingArm, true}; // isCarryingArmOK
			row.pre[1] = {Sym::IsMovementAllowed, true};
			row.pre[2] = {Sym::AtLocation, true};
			row.pre[3] = {Sym::IsTargetDown, true};
			row.preCount = 4;
			// IS_CROUCHING_OR_LYING=0 is not modelled (OURS): no actor state stands in for it here.
			row.eff[0] = {Sym::IsCarryingTarget, true};
			row.effCount = 1;
			row.breaksLocation = true; // the body travels in the member's arms
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::PickupIntruders;
			row.name = "PICKUP_INTRUDERS";
			row.goalState = {Sym::IsCarryingTarget, true};
			row.hasGoalState = true;
			row.pre[0] = {Sym::IsCarryingSomething, false};
			row.preCount = 1;
			row.utility = 1.0;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::TakeIntruderOutside;
			row.name = "TAKE_INTRUDER_OUTSIDE";
			row.pre[0] = {Sym::IsCarryingSomething, true};
			row.pre[1] = {Sym::AtLocation, true}; // LOCATION_IS_ACCESSIBLE: the outside point the tick computes is reachable
			row.preCount = 2;
			row.eff[0] = {Sym::IntruderIsOutside, true};
			row.effCount = 1;
			row.breaksLocation = true;
			row.utility = 1.0;
			out.push_back(row);
		}
		// The town scope (task CH group 3; dt24_273 section 1): the same carry -> remove pair with 273's goal state and 274's effect
		// INTRUDER_IS_OUTSIDE_GATES (164). 273 is 223's goal node; 274 is 224's tick with the destination beyond the town's gates.
		{
			TaskRow row;
			row.task = routines::task::PickupIntrudersTown;
			row.name = "PICKUP_INTRUDERS_TOWN";
			row.goalState = {Sym::IsCarryingTarget, true};
			row.hasGoalState = true;
			row.pre[0] = {Sym::IsCarryingSomething, false};
			row.preCount = 1;
			row.utility = 1.0;
			out.push_back(row);
		}
		// The crime goals (task crime-rows; kinspect ai_enum_map rows 204 / 209 / 210 / 211 / 212), every one orderOnly like the small rows
		// (a goal a finder adopts, never a provider). 209 and 210 are Task_InvestigateAlarms (registry pre atATown, ALARMS_IN_THE_VICINITY = 0:
		// the investigator works until the town's alarms near it are gone); 211's symbol is TARGET_IS_IMPRISONED = 0, 212's
		// isSubjectInInventory, 204's TARGET_IS_ARRESTED. The other three are Task_Blank rows with no preconditions.
		for (const std::uint16_t crimeGoal : {routines::task::InvestigateAlarms, routines::task::InvestigateAlarmsAlliesOnly})
		{
			TaskRow row;
			row.task = crimeGoal;
			row.name = crimeGoal == routines::task::InvestigateAlarms ? "INVESTIGATE_ALARMS" : "INVESTIGATE_ALARMS_ALLIES_ONLY";
			row.eff[0] = {Sym::AlarmsInTheVicinity, false};
			row.effCount = 1;
			row.pre[0] = {Sym::AtATown, true};
			row.preCount = 1;
			row.utility = 1.0;
			row.orderOnly = true; // never a provider: 283's and USE_BED's alarm precondition is the scorer's recheck, not a plan step
			out.push_back(row);
		}
		{
			struct CrimeGoal
			{
				std::uint16_t task;
				const char* name;
				Sym goal;
				bool value;
			};
			const CrimeGoal goals[] =
			{
				{routines::task::HuntBounties, "HUNT_BOUNTIES", Sym::TargetIsArrested, true},
				{routines::task::PoliceFreePrisoners, "POLICE_FREE_PRISONERS_WHEN_DONE", Sym::IsTargetImprisoned, false},
				{routines::task::LootStolenGoods, "LOOT_STOLEN_GOODS", Sym::IsSubjectInInventory, true},
			};
			for (const CrimeGoal& goal : goals)
			{
				TaskRow row;
				row.task = goal.task;
				row.name = goal.name;
				row.eff[0] = {goal.goal, goal.value};
				row.effCount = 1;
				row.utility = 1.0;
				row.orderOnly = true;
				out.push_back(row);
			}
		}
		{
			TaskRow row;
			row.task = routines::task::TakeIntruderOutsideTown;
			row.name = "TAKE_INTRUDER_OUTSIDE_TOWN";
			row.pre[0] = {Sym::IsCarryingSomething, true};
			row.pre[1] = {Sym::AtLocation, true}; // LOCATION_IS_ACCESSIBLE: the gate point the tick computes is reachable
			// The registry's third precondition, 90 DESTINATION_NOT_BLOCKED_BY_FORTIFICATIONS, is not modelled (OURS: the sim has no
			// fortification flags; the walk-out's stuck handler is the fallback, as for 224).
			row.preCount = 2;
			row.eff[0] = {Sym::IntruderIsOutsideGates, true};
			row.effCount = 1;
			row.breaksLocation = true;
			row.utility = 1.0;
			out.push_back(row);
		}
		// The shoo goal (task CH; dt24_283): 283 is a Task_Blank goal node whose plan delivers the dialogue (161 TalktoPlayer, no
		// move). 283's own effect, "strangers in house" = false, is what the door-lock goals read; the executor is the dialogue
		// producer (dialogue_sim.cpp's shoo, event 9).
		{
			TaskRow row;
			row.task = kSendDialogue;
			row.name = "SEND_DIALOGUE_SIGNAL_WITHOUT_MOVING";
			row.eff[0] = {Sym::IsDialogDelivered, true};
			row.effCount = 1;
			row.utility = 1.0;
			out.push_back(row);
		}
		// 158 and 283 have the same registry row in the exe (dt24_131: the same resolver 1408ced70); the records differ only in name.
		for (const std::uint16_t shoo : {routines::task::ShooStrangers, routines::task::ShooStrangersPrivate})
		{
			TaskRow row;
			row.task = shoo;
			row.name = shoo == routines::task::ShooStrangers ? "SHOO_STRANGERS_OUT_OF_MY_BUILDING"
															 : "SHOO_STRANGERS_OUT_OF_MY_BUILDING_IF_PRIVATE";
			row.goalState = {Sym::IsDialogDelivered, true};
			row.hasGoalState = true;
			row.pre[0] = {Sym::IAmInsideTargetBuilding, true}; // the member is inside the building (the goal's own row)
			row.pre[1] = {Sym::AlarmsInTheVicinity, false};
			row.preCount = 2;
			row.eff[0] = {Sym::StrangersInHouse, false};
			row.effCount = 1;
			row.utility = 1.0;
			out.push_back(row);
		}
		// The town-routine goals (task ds/town-port; dt24_43 / dt24_20 / dt24_86 / dt24_155 registry rows). Every one is a goal node that
		// is its own run (Task_Blank-style): the plan is the preconditions' providers and then the node. 43 STAY_IN_HOME and 255
		// SIT_ON_THRONE share one registry row (the same resolver, two function ids): no weapon in hand, not crouched or lying, isAtHome,
		// goal IS_WORKING_PRETEND. 20 stands at the shopkeeper node (goal atNode). 86 opens the shop doors (goal DOOR_IS_OPEN, effect
		// allDoorsLocked = 0); 155 / 197 lock them from inside / outside (precondition no strangers, effect allDoorsLocked).
		for (const std::uint16_t home : {routines::task::StayInHome, routines::task::SitOnThrone})
		{
			TaskRow row;
			row.task = home;
			row.name = home == routines::task::StayInHome ? "STAY_IN_HOME" : "SIT_ON_THRONE";
			row.pre[0] = {Sym::HasWeaponEquipped, false};
			row.pre[1] = {Sym::IsCrouchingOrLying, false};
			row.pre[2] = {Sym::AtHome, true};
			row.preCount = 3;
			row.goalState = {Sym::IsWorkingPretend, true};
			row.hasGoalState = true;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = kStayAtShopkeeperNode;
			row.name = "STAND_AT_SHOPKEEPER_NODE";
			row.goalState = {Sym::AtNode, true};
			row.hasGoalState = true;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::OpenUpShopDoors;
			row.name = "OPEN_UP_SHOP_DOORS";
			row.goalState = {Sym::DoorIsOpen, true};
			row.hasGoalState = true;
			row.eff[0] = {Sym::AllDoorsLocked, false};
			row.effCount = 1;
			out.push_back(row);
		}
		for (const std::uint16_t lock : {routines::task::LockAllMyDoors, routines::task::LockDoorsStayOutside})
		{
			const bool inside = lock == routines::task::LockAllMyDoors;
			TaskRow row;
			row.task = lock;
			row.name = inside ? "LOCK_ALL_MY_DOORS" : "LOCK_ALL_MY_DOORS_STAY_OUTSIDE";
			row.pre[0] = {Sym::StrangersInHouse, false};
			row.pre[1] = {Sym::IAmInsideTargetBuilding, inside};
			row.preCount = 2;
			row.goalState = {inside ? Sym::DoorLockedInside : Sym::DoorLockedOutside, true};
			row.hasGoalState = true;
			row.eff[0] = {Sym::AllDoorsLocked, true};
			row.effCount = 1;
			out.push_back(row);
		}
		// The combat-joining goals (task CF; kinspect dt24_16 / dt24_143 / dt24_15, registry rows from ai_enum_map.tsv). All three
		// goals are Task_Blank: the goal node ends the plan, and the fight itself is the action steps' - the melee row's, which the
		// arena's swing runs (working model M, UNSURE until Task_Blank's tick is confirmed).
		{
			TaskRow row;
			row.task = kMeleeAttack;
			row.name = "MELEE_ATTACK";
			row.utility = 1.0;
			row.pre[0] = {Sym::IsMovementAllowed, true};
			row.pre[1] = {Sym::ReadyForAction, true};
			row.pre[2] = {Sym::IsWithin50Meters, true};
			// OURS (UNSURE): the exe remaps the attack to 262 / 263 / 266 when the hand holds a crossbow (CURSORS_LIVE C4) instead of planning
			// it; the symbol keeps a crossbow hand off this row, and a melee or a meleed hand on it.
			row.pre[3] = {Sym::HoldsCrossbow, false};
			row.preCount = 4;
			row.eff[0] = {Sym::IsEnemy, true};
			row.eff[1] = {Sym::IsTargetDown, true};
			row.effCount = 2;
			out.push_back(row);
		}
		// The ranged attack rows (task ds/ranged S2; GATES_CARRY_STATIC section 2): 262 / 263 / 266 need the arms, a weapon in hand, no swim (not
		// modelled), no melee on me and a standing body, and make isTargetDown; 264 draws the crossbow (hasWeaponEquipped). The range and the
		// shot are the task's own (CombatArena::rangedFire), so there is no walk row in front: the 40 walk is the melee row's. Like the melee
		// row they also give isEnemy, the combat goals' goal state (working model M, UNSURE). HoldsCrossbow is ours: it keeps the plan of a
		// melee hand on the melee row.
		for (const std::uint16_t task : {kRangedAttack, kRangedAttackFocused, kRangedAttackFocusedUnprovoked})
		{
			TaskRow row;
			row.task = task;
			row.name = task == kRangedAttack ? "RANGED_ATTACK" : task == kRangedAttackFocused ? "RANGED_ATTACK_FOCUSED" : "RANGED_ATTACK_FOCUSED_UNPROVOKED";
			row.utility = 1.0;
			row.pre[0] = {Sym::IAmBeingMeleeAttacked, false};
			row.pre[1] = {Sym::HasTwoWorkingArms, true};
			row.pre[2] = {Sym::IsCrouchingOrLying, false};
			row.pre[3] = {Sym::HasWeaponEquipped, true};
			row.pre[4] = {Sym::HoldsCrossbow, true};
			row.preCount = 5;
			row.eff[0] = {Sym::IsEnemy, true};
			row.eff[1] = {Sym::IsTargetDown, true};
			row.effCount = 2;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = kEquipCrossbow;
			row.name = "EQUIP_CROSSBOW";
			row.pre[0] = {Sym::HasOneWorkingArm, true};
			row.preCount = 1;
			row.eff[0] = {Sym::HasWeaponEquipped, true};
			row.effCount = 1;
			out.push_back(row);
		}
		// 146 USE_TURRET / 235 SHOOT_AT_TARGET are Task_UseTurret (dt24_stalk_turret section 3, dt41_B): the mount, then the shot state machine bound
		// to the turret. 146 mounts (its start: operator slot, `vf+0x460(1)`): OURS effect operatesTurret, offered only where the town finder has a
		// turret (turretFound, never true in a combat world, so no melee goal plans it). 235 shoots: it needs the mount and makes isTargetDown; the
		// shot is CombatArena::rangedFire on the turret's gun, and the mount itself is Simulation's (stepTurretMounts).
		{
			TaskRow row;
			row.task = kUseTurret;
			row.name = "USE_TURRET";
			row.utility = 1.0;
			row.pre[0] = {Sym::HasOneWorkingArm, true};
			row.pre[1] = {Sym::TurretFound, true};
			row.preCount = 2;
			row.eff[0] = {Sym::OperatesTurret, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			// The player job's twin (dt34_flee_or_fight: pre hasOneWorkingArm, isAlliesFighting = 0; its isAggressionTowardsMe = 0 has no symbol
			// here). Never planner-selected in the exe; ours keeps the row for the registry, gated by the same OURS symbol.
			TaskRow row;
			row.task = kTurretPlayerJob;
			row.name = "MAN_A_TURRET_PLAYER_JOB";
			row.utility = 1.0;
			row.pre[0] = {Sym::HasOneWorkingArm, true};
			row.pre[1] = {Sym::IsAlliesFighting, false};
			row.pre[2] = {Sym::TurretFound, true};
			row.preCount = 3;
			row.eff[0] = {Sym::OperatesTurret, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			// 228 (dt41_C): the registry row's preconditions are hasOneWorkingArm, isMovementAllowed and the route / destination / location checks
			// (the walk and the fortification test are the task's own approach here); its effect is not decoded: isTargetDown (OURS, UNSURE: a KO'd
			// victim is down). Gated by the OURS symbol StealthKoOrdered, which no combat world offers, so no melee goal plans a knockout.
			TaskRow row;
			row.task = kStealthKnockout;
			row.name = "STEALTH_KNOCKOUT";
			row.utility = 1.0;
			row.pre[0] = {Sym::HasOneWorkingArm, true};
			row.pre[1] = {Sym::IsMovementAllowed, true};
			row.pre[2] = {Sym::StealthKoOrdered, true};
			row.preCount = 3;
			row.eff[0] = {Sym::IsTargetDown, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = kShootAtTarget;
			row.name = "SHOOT_AT_TARGET";
			row.utility = 1.0;
			row.pre[0] = {Sym::OperatesTurret, true};
			row.preCount = 1;
			row.eff[0] = {Sym::IsTargetDown, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = kGetNearTo;
			row.name = "GET_NEAR_TO";
			row.utility = 1.0;
			row.pre[0] = {Sym::IsMovementAllowed, true};
			row.preCount = 1;
			// OURS: the registry's symbol is isNearTo; the melee's own range need (isWithin50Meters) is what the walk closes here.
			row.eff[0] = {Sym::IsWithin50Meters, true};
			row.effCount = 1;
			row.breaksLocation = true;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = kAttackAttackersOf;
			row.name = "ATTACK_ATTACKERS_OF";
			row.utility = 4.0; // the registry's store +0x28
			row.goalState = {Sym::IsTargetDown, true};
			row.hasGoalState = true;
			row.eff[0] = {Sym::IsAggressionTowards, false};
			row.eff[1] = {Sym::IsAlliesFighting, false};
			row.effCount = 2;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::ProtectOwnSquad;
			row.name = "PROTECT_OWN_SQUAD";
			row.utility = 0.5;
			row.goalState = {Sym::IsAggressionTowards, false};
			row.hasGoalState = true;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::ProtectAllies;
			row.name = "PROTECT_ALLIES";
			row.utility = 0.5;
			row.goalState = {Sym::IsAlliesFighting, false};
			row.hasGoalState = true;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::AttackEnemies;
			row.name = "ATTACK_ENEMIES";
			row.utility = 1.0;
			row.goalState = {Sym::IsEnemy, true};
			row.hasGoalState = true;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::DefeatSquad;
			row.name = "DEFEAT_SQUAD";
			row.utility = 1.0; // the registry's store +0x24 (dt24_101)
			row.goalState = {Sym::IsTargetDown, true};
			row.hasGoalState = true;
			row.pre[0] = {Sym::IsCarryingSomething, false};
			row.preCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::DefeatSquadLimitChase;
			row.name = "DEFEAT_SQUAD_LIMIT_CHASE_RANGE";
			row.utility = 1.0;
			row.goalState = {Sym::IsTargetDown, true};
			row.hasGoalState = true;
			row.pre[0] = {Sym::IAmBeingMeleeAttacked, false};
			row.pre[1] = {Sym::IsCarryingSomething, false};
			row.preCount = 2;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::AttackSlavemasterEnemies;
			row.name = "ATTACK_ENEMIES_OF_MY_SLAVEMASTER";
			row.utility = 1.0; // the registry's store +0x24 (dt24_41)
			row.goalState = {Sym::IsEnemy, true}; // the registry's goal condition isEnemy=1 (dt24_41, the same symbol as 16's)
			row.hasGoalState = true;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::TerritorialAggression;
			row.name = "TERRITORIAL_AGGRESSION_BUT_DONT_LEAVE_HOME";
			row.utility = 1.0; // the registry's store +0x24
			row.goalState = {Sym::IsEnemy, true}; // the registry's goal condition, the same as 16's (dt24_144)
			row.hasGoalState = true;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::AttackEnemiesAndNeutrals;
			row.name = "ATTACK_ENEMIES_AND_NEUTRALS";
			row.utility = 1.0; // the registry's store +0x24
			row.goalState = {Sym::IsTargetDown, true}; // the registry's goal condition (dt24_21)
			row.hasGoalState = true;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::AttackTroubleMakers;
			row.name = "ATTACK_TROUBLE_MAKERS";
			row.utility = 1.0; // OURS: the registry stores the runtime r13d here, no fixed value (dt24_34)
			row.goalState = {Sym::IsTargetDown, true}; // the registry's goal condition (dt24_34)
			row.hasGoalState = true;
			row.eff[0] = {Sym::IsGeneralAggressionLevel, false}; // the only task in the registry with this effect
			row.effCount = 1;
			out.push_back(row);
		}
		// The patrol family (task patrol; kinspect dt24_36 section 1, the registry rows). 36 and 22 are goal-less terminal actions:
		// they have preconditions only, no effect and no resolver, and their destination is the executor's (36 the package's target
		// hand, 22 the member's own point and move-options block); they end only through the precondition / TaskData+0x15 path (model
		// M). 216 is a real goal: it is satisfied by standing inside any building with an interior in the town.
		{
			TaskRow row;
			row.task = routines::task::PatrolTown;
			row.name = "PATROL_TOWN";
			row.utility = routines::kUtilityPatrol;
			row.pre[0] = {Sym::AtDestinationTown, true};
			row.pre[1] = {Sym::IsCarryingSomething, false};
			row.pre[2] = {Sym::IsCrouchingOrLying, false};
			row.pre[3] = {Sym::AtLocation, true}; // 60 LOCATION_IS_ACCESSIBLE
			row.pre[4] = {Sym::HasWeaponEquipped, false};
			row.preCount = 5;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::Patrol;
			row.name = "PATROL";
			row.utility = routines::kUtilityPatrol; // OURS: 22 has no utility of its own in AI_COVERAGE; the patrol family's stands in
			row.pre[0] = {Sym::IsCrouchingOrLying, false};
			row.pre[1] = {Sym::AtLocation, true};
			row.pre[2] = {Sym::HasWeaponEquipped, false};
			row.preCount = 3;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::RunAroundTown;
			row.name = "RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE";
			row.utility = 1.0; // the registry's store +0x24 is not decoded for this row (dt24_36 section 1)
			row.goalState = {Sym::IAmInsideTargetBuilding, true};
			row.hasGoalState = true;
			row.pre[0] = {Sym::HasWeaponEquipped, true};
			row.preCount = 1;
			out.push_back(row);
		}
		// 25 FIRST_AID_ORDER -> Task_FirstAid (dt24): no preconditions of its own (the task arms its own walk to the target); its
		// only state effect is clearing needsFirstAid. Code: the executor's first aid interact in world_tasks.cpp.
		{
			TaskRow row;
			row.task = kFirstAidOrder;
			row.name = "FIRST_AID_ORDER";
			row.eff[0] = {Sym::NeedsFirstAid, false};
			row.effCount = 1;
			out.push_back(row);
		}
		// 269 HEAL_MY_LEGS -> Task_FirstAid (the registry's twin of 25; k16.6.4, MEDICAL_HUNGER_LIVE.md (f)): no preconditions, it clears
		// needsFirstAid. No package names it: the game gives it to a down, conscious character that carries a kit (Simulation::stepSelfAid).
		{
			TaskRow row;
			row.task = kHealMyLegs;
			row.name = "HEAL_MY_LEGS";
			row.eff[0] = {Sym::NeedsFirstAid, false};
			row.effCount = 1;
			out.push_back(row);
		}
		// 58 JOB_MEDIC: Task_Blank (dt24_58.md): the goal state only, so a plan is [25, 58] while the patient still needs aid.
		{
			TaskRow row;
			row.task = routines::task::FirstAid;
			row.name = "JOB_MEDIC";
			row.goalState = {Sym::NeedsFirstAid, false};
			row.hasGoalState = true;
			out.push_back(row);
		}
		// Shopping (task CH group 3; kinspect dt24_118 section 1, dt24_shop section 1). 118 is the Task_Blank goal node: its goal
		// state is the registry's unnamed `?local_2e60` (read as "the day's list is satisfied") and its precondition atATown. 119
		// BUY_SHIT is the Task_Shopping action: the same goal state, isAtLocation in front of it (the counter). The class has no
		// finder - the 118 resolver picks the shop - and the transaction is Simulation::stepShopping's.
		{
			TaskRow row;
			row.task = routines::task::Shopping;
			row.name = "SHOPPING";
			row.utility = 1.0; // the registry's utility for 118 is not decoded (dt24_118 section 1)
			row.goalState = {Sym::ShoppingDone, true};
			row.hasGoalState = true;
			row.pre[0] = {Sym::AtATown, true};
			row.preCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::BuyStuff;
			row.name = "BUY_SHIT";
			row.utility = 1.0;
			row.pre[0] = {Sym::AtLocation, true};
			row.preCount = 1;
			row.eff[0] = {Sym::ShoppingDone, true};
			row.effCount = 1;
			out.push_back(row);
		}
		// The machine / storage pull tasks (dt24_emptymachine_dismantle section 2; the registry rows are ai_enum_map.tsv's
		// reg_pre / reg_eff). All three take from a building in the action hand: 92 a machine's output, 247 the same with the
		// BuildMats quantity (two per pass), 284 a storage box's contents. isMachineInputInvalid is not modelled live (false:
		// the crafting-input test is not in the sim), so 92's row writes it but nothing reads it yet.
		for (const auto& [task, name, effect] : {std::tuple{routines::task::EmptyMachine, "COLLECT_OUTPUT_RESOURCE", Sym::HaveSomeResourcesFrom},
				 std::tuple{routines::task::BuildMats, "COLLECT_OUTPUT_RESOURCE_BUILD_MATS", Sym::HaveSomeBuildMats},
				 std::tuple{routines::task::LootContainer, "LOOT_CONTAINER", Sym::HaveSomeLootFrom}})
		{
			TaskRow row;
			row.task = task;
			row.name = name;
			row.pre[0] = {Sym::GotInventoryRoomForResources, true};
			row.pre[1] = {Sym::AtLocation, true};
			row.preCount = 2;
			row.eff[0] = {effect, true};
			row.effCount = 1;
			if (task == routines::task::EmptyMachine)
			{
				row.eff[1] = {Sym::IsMachineOutputFull, false};
				row.eff[2] = {Sym::IsMachineInputInvalid, false};
				row.effCount = 3;
			}
			out.push_back(row);
		}
		// The Task_Blank goals that plan them (dt24_113 / dt24_115 / dt24_131): the goal state only; the pull task is the
		// plan's action, and the goal entry is the marker the executor pops (as 58's is).
		{
			TaskRow row;
			row.task = kWantToFillMachine;
			row.name = "WANT_TO_FILL_MACHINE";
			row.goalState = {Sym::HaveSomeResourcesFrom, true};
			row.hasGoalState = true;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = kFindSomeBuildMats;
			row.name = "FIND_SOME_BUILDING_MATERIALS";
			row.goalState = {Sym::HaveSomeBuildMats, true};
			row.hasGoalState = true;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = kLootFoodAndStuff;
			row.name = "LOOT_FOOD_AND_STUFF";
			row.goalState = {Sym::HaveSomeLootFrom, true};
			row.hasGoalState = true;
			out.push_back(row);
		}
		// 87 OPERATE_MACHINERY (dt24_operatemachine / dt24_emptymachine_source section 2): its registration's false entries 0x3f /
		// 0x3e are isMachineOutputFull = 0 and isMachineInputInvalid = 0; its effect is IS_WORKING. THAT pair is why the planner
		// inserts 92: a full machine cannot be operated until its output is taken.
		{
			TaskRow row;
			row.task = kOperateMachinery;
			row.name = "OPERATE_MACHINERY";
			row.pre[0] = {Sym::IsMachineOutputFull, false};
			row.pre[1] = {Sym::IsMachineInputInvalid, false};
			row.preCount = 2;
			row.eff[0] = {Sym::IsWorking, true};
			row.effCount = 1;
			out.push_back(row);
		}
		// The goals whose goal state IS_WORKING plans 87, and through it 92 (dt24_emptymachine_source section 3): a working NPC's
		// chain. 214 RELAX_IN_TOWN_PACKAGE is also the town revellers' relax goal (the live TQ24 capture kept the Hub's bar patrons
		// on it): one goal, and the scorer's 214 case works a producer whose chain plans and falls back to the bar table otherwise.
		for (const auto& [task, name] : {std::tuple{kRelaxInTownPackage, "RELAX_IN_TOWN_PACKAGE"},
				 std::tuple{kAutoLabouringMines, "AUTO_LABOURING_MINES"}})
		{
			TaskRow row;
			row.task = task;
			row.name = name;
			row.pre[0] = {Sym::HasWeaponEquipped, false}; // peaceful (GATES_CARRY_STATIC): the planner sheathes in front (task ds/sheathe)
			row.preCount = 1;
			if (task == kRelaxInTownPackage)
				row.pre[row.preCount++] = {Sym::AtATown, true}; // the registry's `atATown`=1 (dt24_214 section 1)
			row.goalState = {Sym::IsWorking, true};
			row.hasGoalState = true;
			out.push_back(row);
		}
		// The corpse pot (dt24_patrol_town_pot section 3; task CH town-residents-3). The registry rows:
		//   137 DEAD_GUYS_GO_IN_THE_POT            goal 100 CARRIED_DUDE_NOW_IN_DISPOSAL_MACHINE = 1, pre 99 isCarriedDudeDead = 1
		//   138 FIND_A_DEAD_GUY ("Cannibal- Find a corpse")  goal 48 IS_CARRYING_TARGET = 1, effect 99 isCarriedDudeDead = 1
		//   136 FEED_CORPSE_INTO_MACHINE ("Disposing of corpse")  pre 99 and isAtLocation, effect 100
		// 137 and 138 are Task_Blank goal markers (popped, never run); 136 is the only task that writes 100 and 138 the only one
		// that writes 99, so the plan is 67 -> 68 (the lift 138's goal state needs) -> 67 -> 136. The exe's condition 100 is
		// literally "I am not carrying anything any more" (14060e5c0): with empty hands the goal state already holds and the plan
		// is the fetch half only; the member re-plans once the body is in its arms and 136 is then the goal's own provider.
		{
			TaskRow row;
			row.task = routines::task::DeadGuysInThePot;
			row.name = "DEAD_GUYS_GO_IN_THE_POT";
			row.utility = routines::kUtilityDefault; // the utility ladder: the flat 1.0 (TOWN_LIFE_SLICE 5.5)
			row.goalState = {Sym::CarriedDudeInDisposal, true};
			row.hasGoalState = true;
			row.pre[0] = {Sym::CarriedDudeIsDead, true};
			row.preCount = 1;
			out.push_back(row);
		}
		// The occupied-town goals (kinspect dt24_243 / dt24_275; the registry rows are ai_enum_map.tsv's reg_pre / reg_eff). Both are
		// Task_Blank goal nodes: 243 plans a food grab (244 GRAB_ONE_FOOD, the only writer of gotAFoodItem), 275 plans the pretend
		// labour 190 (the only row whose effect is IS_WORKING_PRETEND; 221 writes it too but has a target of its own). The data lists
		// 243 and 275 (the 'occupy town' packages); 244 is code-only - no AI_PACKAGE names it.
		// 7 UNEQUIP_WEAPON is the planner's sheathe (GATES_CARRY_STATIC: a peaceful row needs hasWeaponEquipped = 0, so the planner
		// puts the unequip in front of it). 275 SIT_AROUND is the first such row the native selector carries; the sheathe itself is
		// the table's executor (the ritual step in world_tasks.cpp), as 190's pretend is.
		{
			TaskRow row;
			row.task = routines::task::UnequipWeapon;
			row.name = "UNEQUIP_WEAPON";
			row.eff[0] = {Sym::HasWeaponEquipped, false};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::PretendLabourPretend;
			row.name = "AUTO_LABOURING_MINES_PRETEND";
			row.eff[0] = {Sym::IsWorkingPretend, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::GrabOneFood;
			row.name = "GRAB_ONE_FOOD";
			row.pre[0] = {Sym::IsAlliesFighting, false};
			row.pre[1] = {Sym::GotInventoryRoomForResources, true};
			row.pre[2] = {Sym::AtLocation, true};
			row.preCount = 3;
			row.eff[0] = {Sym::GotAFoodItem, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::AcquireFood;
			row.name = "AQUIRE_FOOD_AT_HOMEBASE";
			row.goalState = {Sym::GotAFoodItem, true};
			row.hasGoalState = true;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::FindCorpses;
			row.name = "FIND_A_DEAD_GUY";
			row.utility = 1.0;
			row.goalState = {Sym::IsCarryingTarget, true};
			row.hasGoalState = true;
			row.eff[0] = {Sym::CarriedDudeIsDead, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::FeedCorpseIntoMachine;
			row.name = "FEED_CORPSE_INTO_MACHINE";
			row.utility = 1.0;
			row.pre[0] = {Sym::CarriedDudeIsDead, true};
			row.pre[1] = {Sym::AtLocation, true}; // 1 isAtLocation: at the machine the pot finder picked
			row.preCount = 2;
			row.eff[0] = {Sym::CarriedDudeInDisposal, true};
			row.effCount = 1;
			row.breaksLocation = false; // the body is consumed in the member's hands; the member stands where it fed it
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::SitAround;
			row.name = "SIT_AROUND";
			row.pre[0] = {Sym::IsCrouchingOrLying, false};
			row.pre[1] = {Sym::IsCarryingSomething, false};
			row.pre[2] = {Sym::HasWeaponEquipped, false};
			row.pre[3] = {Sym::AtATown, true};
			row.preCount = 4;
			row.goalState = {Sym::IsWorkingPretend, true};
			row.hasGoalState = true;
			out.push_back(row);
		}
		// 175 SLEEP_ON_FLOOR (dt24_175, Task_SleepOnFloor): no finder and no goal state - the character sleeps where it stands, and the
		// plan is [175] alone while the preconditions hold. Symbol 6 is read as the no-enemies atom (see Sym::NoEnemiesInVicinity).
		{
			TaskRow row;
			row.task = routines::task::SleepOnFloor;
			row.name = "SLEEP_ON_FLOOR";
			row.pre[0] = {Sym::NoEnemiesInVicinity, true};
			row.pre[1] = {Sym::IsGeneralAggressionLevel, false};
			row.pre[2] = {Sym::AtHomeTown, false};
			row.pre[3] = {Sym::HasWeaponEquipped, false};
			row.pre[4] = {Sym::IsCarryingSomething, false};
			row.preCount = 5;
			row.eff[0] = {Sym::IsFullyRested, true};
			row.effCount = 1;
			out.push_back(row);
		}
		// The get-out-of-bed class (task ds/follow-bed; dt24_208): 116 / 208 / 257 are one Task_GetOutOfBed with the same registry rows -
		// canGetUp = 1 -> isInBed = 0, no resolver, no goal state. A goal whose plan starts in bed pulls one in as the isInBed = 0
		// provider (item57_scorer "(c) Reservation lifetime"); 208 is also a selector goal of its own (routine_scorer.cpp).
		for (const std::uint16_t task : {routines::task::GetOutOfBed, routines::task::GetOutOfBedEmergency, routines::task::GetOutOfBedHealed})
		{
			TaskRow row;
			row.task = task;
			row.name = task == routines::task::GetOutOfBed ? "GET_OUT_OF_BED"
				: task == routines::task::GetOutOfBedEmergency ? "GET_OUT_OF_BED_IF_ITS_EMERGENCY" : "GET_OUT_OF_BED_ONCE_HEALED";
			row.pre[0] = {Sym::CanGetUp, true};
			row.preCount = 1;
			row.eff[0] = {Sym::IsInBed, false};
			row.effCount = 1;
			out.push_back(row);
		}
		// USE_BED's registry precondition (s63 lines 263-270; GATES_CARRY_STATIC "peaceful tasks"): no weapon equipped, so the planner puts
		// 7 UNEQUIP_WEAPON in front of the bed (task ds/sheathe). 153 GO_HOME_TO_BED and 239 its secure variant are the goals that take
		// the bed; the other preconditions (no alarm, nothing carried, the slot still the member's) are the scorer's recheck.
		for (const std::uint16_t task : {routines::task::GoHomeToBed, routines::task::GoHomeToBedSecure})
		{
			TaskRow row;
			row.task = task;
			row.name = task == routines::task::GoHomeToBed ? "GO_HOME_TO_BED" : "GO_HOME_TO_BED_SECURE";
			row.pre[0] = {Sym::HasWeaponEquipped, false};
			row.preCount = 1;
			out.push_back(row);
		}
		// USE_BED 98 and the player's sleep order 258: the Sleep step (InteractAction::Sleep, interact.cpp: walk to the bed's use node, lie down) is
		// their executor. Same registry precondition as the goals above (no weapon equipped); the effect is isInBed = 1, which no goal asks for
		// yet, so no plan changes. 258 is a player order. 100 (the put-in of a carried person) stays carry.cpp's. UNSURE: the other
		// registry preconditions (a free bed slot, no alarm) are the scorer's recheck.
		for (const std::uint16_t task : {kUseBed, kPlayerSleep})
		{
			TaskRow row;
			row.task = task;
			row.name = task == kUseBed ? "USE_BED" : "USE_BED_ORDER";
			row.orderOnly = task == kPlayerSleep;
			row.pre[0] = {Sym::HasWeaponEquipped, false};
			row.preCount = 1;
			row.eff[0] = {Sym::IsInBed, true};
			row.effCount = 1;
			out.push_back(row);
		}
		// The slave's own cage entry (task ds/slave-goals; dt24_172 section 1, dt24_plantasks): 172 is a Task_Blank goal row whose
		// only registry datum is the goal state I_AM_IMPRISONED=1; its resolver supplies the cage of the CAPTOR's faction. The plan's
		// writer is 107 USE_CAGE (the player order / plan action: machineHasFreeOperatorSlot=1, isAtLocation=1 -> I_AM_IMPRISONED=1),
		// so the chain from away is [67 Running, 107, 172] and from the cage [107, 172].
		{
			TaskRow row;
			row.task = routines::task::UseCage;
			row.name = "USE_CAGE";
			row.pre[0] = {Sym::MachineHasFreeOperatorSlot, true}; // OURS: the resolver only picks an empty cage (its freeSlot)
			row.pre[1] = {Sym::AtLocation, true}; // 1 isAtLocation: at the cage the 172 resolver picked
			row.preCount = 2;
			row.eff[0] = {Sym::IAmImprisoned, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::SlaveGetInCage;
			row.name = "SLAVE_GET_IN_MY_MASTERS_CAGE";
			row.goalState = {Sym::IAmImprisoned, true};
			row.hasGoalState = true;
			out.push_back(row);
		}
		// The escape family (task ds/escape; dt24_207). 207 GET_OUT_OF_CAGE_ESCAPE and 256 GET_OUT_OF_CAGE_OPPORTUNISTIC are one class
		// (Task_GetOutOfCage): pre isDoorLocked = 0, effect I_AM_IMPRISONED = 0. 202 TOTAL_ESCAPE is a data-only Task_Blank row: pre
		// isChained = 0 and I_AM_IMPRISONED = 0, no effect, no executor (ours never offers it: nothing could run or change).
		for (const auto& [task, name] : {std::tuple{routines::task::GetOutOfCageEscape, "GET_OUT_OF_CAGE_ESCAPE"},
				 std::tuple{routines::task::GetOutOfCageOpportunistic, "GET_OUT_OF_CAGE_OPPORTUNISTIC"},
				 std::tuple{routines::task::GetOutOfCageLegit, "GET_OUT_OF_CAGE_LEGIT"}})
		{
			TaskRow row;
			row.task = task;
			row.name = name;
			row.pre[0] = {Sym::IsDoorLocked, false};
			row.preCount = 1;
			row.eff[0] = {Sym::IAmImprisoned, false};
			row.effCount = 1;
			out.push_back(row);
		}
		// The lock rows (task ds/shackles-locks): the ways to isDoorLocked = 0 (dt24_plantasks; item28 b2: PICK_LOCK needs the skill, CUT_LOCK
		// tools, BRUTE_FORCE_LOCK is a player order) and to isWearingShackles_locked = 0 (dt24_shackles, dt24_picklock), then 219 which takes
		// the opened shackles off (isChained = 0), 110 RELEASE_PRISONER and the cage kill 134 (registry rows in ai_enum_map). 76 is tried
		// before 285 (the stable order of equal utilities, then the lower one last: the exe's order of preference between them is UNSURE).
		{
			struct LockRow
			{
				std::uint16_t task;
				const char* name;
				double utility;
				bool orderOnly;
				Sym skillOrTools; // Count = none
				Sym effect;
			};
			for (const LockRow& spec : {LockRow{76, "PICK_LOCK", 1.0, false, Sym::LockSkillOk, Sym::IsDoorLocked},
					 LockRow{285, "CUT_LOCK", 0.5, false, Sym::HasTools, Sym::IsDoorLocked},
					 LockRow{286, "BRUTE_FORCE_LOCK", 0.1, true, Sym::Count, Sym::IsDoorLocked},
					 LockRow{201, "PICK_LOCK_ON_SHACKLES", 1.0, false, Sym::ShackleSkillOk, Sym::ShacklesLocked},
					 LockRow{185, "CUT_SHACKLES", 0.5, false, Sym::HasTools, Sym::ShacklesLocked},
					 LockRow{186, "BRUTE_FORCE_SHACKLES", 0.1, true, Sym::Count, Sym::ShacklesLocked}})
			{
				TaskRow row;
				row.task = spec.task;
				row.name = spec.name;
				row.utility = spec.utility;
				row.orderOnly = spec.orderOnly;
				row.pre[row.preCount++] = {Sym::HasOneWorkingArm, true};
				if (spec.skillOrTools != Sym::Count)
					row.pre[row.preCount++] = {spec.skillOrTools, true};
				row.pre[row.preCount++] = {Sym::AtLocation, true};
				if (!spec.orderOnly)
					row.pre[row.preCount++] = {Sym::HasWeaponEquipped, false}; // the picks and cuts need empty hands (the brute forces do not)
				row.eff[0] = {spec.effect, false};
				row.effCount = 1;
				out.push_back(row);
			}
		}
		// The door tasks (task ds/order-tasks; dt24_plantasks section 3, GATES_CARRY_STATIC a2, DOORS_LIVE sections 15b / 16). The planner inserts
		// them to satisfy DOOR_IS_OPEN / isDoorLocked on a plan: OPEN_DOOR needs the lock off and the actor at the door, UNLOCK_DOOR only the
		// owner's faction (isObjectMine), the bash only a door that is not mine and not an ally's, with a weapon in hand (BASH_GATE: a
		// fortification gate with fewer than 8 mates on it). A bashed door is broken, which counts as open and needs no unlock in front of it. UNSURE: the
		// utilities are 1.0 stand-ins (the exe's are per faction kind and crowd, see Simulation::doorDecision's utility); the here variants
		// name the current-location door symbol; the effect sets of 81 / 123 / 287 are read from the finding's "goal broken/open".
		{
			struct DoorRow
			{
				std::uint16_t task;
				const char* name;
				bool here;
			};
			const auto atDoor = [](TaskRow& row, bool here)
			{
				row.pre[row.preCount++] = {here ? Sym::AtCurrentDoor : Sym::AtBuildingDoor, true};
			};
			for (const DoorRow& spec : {DoorRow{kOpenDoor, "OPEN_DOOR", false}, DoorRow{kOpenDoorHere, "OPEN_DOOR_HERE", true}})
			{
				TaskRow row;
				row.task = spec.task;
				row.name = spec.name;
				row.pre[row.preCount++] = {Sym::IsDoorLocked, false};
				atDoor(row, spec.here);
				row.eff[row.effCount++] = {Sym::DoorIsOpen, true};
				out.push_back(row);
			}
			for (const DoorRow& spec : {DoorRow{kCloseDoor, "CLOSE_DOOR", false}, DoorRow{kCloseDoorHere, "CLOSE_DOOR_HERE", true}})
			{
				TaskRow row;
				row.task = spec.task;
				row.name = spec.name;
				row.pre[row.preCount++] = {Sym::DoorIsBroken, false};
				atDoor(row, spec.here);
				row.eff[row.effCount++] = {Sym::DoorIsOpen, false};
				out.push_back(row);
			}
			for (const DoorRow& spec : {DoorRow{kUnlockDoor, "UNLOCK_DOOR", false}, DoorRow{kUnlockDoorHere, "UNLOCK_DOOR_HERE", true},
					 DoorRow{kUnlockDoorOrder, "UNLOCK_DOOR_PLAYER_ORDER", false}})
			{
				TaskRow row;
				row.task = spec.task;
				row.name = spec.name;
				row.orderOnly = spec.task == kUnlockDoorOrder;
				row.pre[row.preCount++] = {Sym::IsObjectMine, true};
				atDoor(row, spec.here);
				row.eff[row.effCount++] = {Sym::IsDoorLocked, false};
				out.push_back(row);
			}
			// The lock (dt24_plantasks section 3; DOORS_LIVE 15b): LOCK_DOOR 77 / LOCK_DOOR_HERE 79 need the door shut, mine and whole, and the actor
			// at it; 162 / 198 lock from the inside / outside stand point and also set the matching locked-and-I-am-there symbol (the goal symbol of
			// 155 / 197). UNSURE: the registry's exact preconditions for 162 / 198.
			for (const DoorRow& spec : {DoorRow{kLockDoor, "LOCK_DOOR", false}, DoorRow{kLockDoorHere, "LOCK_DOOR_HERE", true},
					 DoorRow{kLockDoorFromInside, "LOCK_DOOR_FROM_INSIDE", false}, DoorRow{kLockDoorFromOutside, "LOCK_DOOR_FROM_OUTSIDE", false}})
			{
				TaskRow row;
				row.task = spec.task;
				row.name = spec.name;
				row.pre[row.preCount++] = {Sym::DoorIsOpen, false};
				row.pre[row.preCount++] = {Sym::IsObjectMine, true};
				row.pre[row.preCount++] = {Sym::DoorIsBroken, false};
				if (spec.task == kLockDoorFromInside) row.pre[row.preCount++] = {Sym::AtDoorInside, true};
				else if (spec.task == kLockDoorFromOutside) row.pre[row.preCount++] = {Sym::AtDoorOutside, true};
				else atDoor(row, spec.here);
				row.eff[row.effCount++] = {Sym::IsDoorLocked, true};
				if (spec.task == kLockDoorFromInside) row.eff[row.effCount++] = {Sym::DoorLockedInside, true};
				else if (spec.task == kLockDoorFromOutside) row.eff[row.effCount++] = {Sym::DoorLockedOutside, true};
				out.push_back(row);
			}
			// The bash: BASH_DOOR 81 / BASH_DOOR_HERE 287 / BASH_DOOR_PLAYER_ORDER 226 swing a weapon (the planner equips one); BASH_GATE 123
			// is the fortification gate's, any hands, with the 8-mate crowd limit.
			for (const DoorRow& spec : {DoorRow{kBashDoor, "BASH_DOOR", false}, DoorRow{kBashDoorHere, "BASH_DOOR_HERE", true},
					 DoorRow{kBashDoorOrder, "BASH_DOOR_PLAYER_ORDER", false}, DoorRow{kBashGate, "BASH_GATE", false}})
			{
				TaskRow row;
				row.task = spec.task;
				row.name = spec.name;
				row.orderOnly = spec.task == kBashDoorOrder;
				row.pre[row.preCount++] = {Sym::DoorBashable, true};
				if (spec.task == kBashGate)
					row.pre[row.preCount++] = {Sym::GateBashOpen, true};
				else
					row.pre[row.preCount++] = {Sym::HasWeaponEquipped, true};
				atDoor(row, spec.here);
				row.eff[row.effCount++] = {Sym::DoorIsOpen, true}; // a broken door is open; the lock needs no separate step
				out.push_back(row);
			}
			// The walk to the door (dt24_movetodoor sections 1-4): a Task_Move with a door hook. 82 stands at the door (the base destination), 83 at
			// the door of the building I am in, 163 / 199 at its inside / outside stand point. UNSURE: the registry's own preconditions (121, the
			// gate variant, lists LOCATION_IS_ACCESSIBLE and two unnamed symbols; none are read here).
			for (const DoorRow& spec : {DoorRow{kMoveToBuildingDoor, "MOVE_TO_BUILDING_DOOR", false},
					 DoorRow{kMoveToCurrentDoor, "MOVE_TO_CURRENT_LOCATION_BUILDING_DOOR", true},
					 DoorRow{kMoveToDoorInside, "MOVE_TO_BUILDING_DOOR_INSIDEPOS", false},
					 DoorRow{kMoveToDoorOutside, "MOVE_TO_BUILDING_DOOR_OUTSIDEPOS", false}})
			{
				TaskRow row;
				row.task = spec.task;
				row.name = spec.name;
				row.eff[row.effCount++] = {spec.task == kMoveToCurrentDoor ? Sym::AtCurrentDoor
					: spec.task == kMoveToDoorInside ? Sym::AtDoorInside : spec.task == kMoveToDoorOutside ? Sym::AtDoorOutside : Sym::AtBuildingDoor, true};
				out.push_back(row);
			}
		}
		{
			TaskRow row;
			row.task = 219;
			row.name = "TAKE_OFF_MY_SHACKLES";
			row.pre[0] = {Sym::ShacklesLocked, false};
			row.preCount = 1;
			row.eff[0] = {Sym::IsChained, false};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = 110;
			row.name = "RELEASE_PRISONER";
			row.pre[0] = {Sym::TargetIsConscious, true};
			row.pre[1] = {Sym::MachineHasFreeOperatorSlot, false};
			row.pre[2] = {Sym::AtLocation, true};
			row.preCount = 3;
			row.eff[0] = {Sym::IsTargetImprisoned, false};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = 134;
			row.name = "KILL_CAGE_OCCUPANT";
			row.pre[0] = {Sym::HasWeaponEquipped, true};
			row.pre[1] = {Sym::AtLocation, true};
			row.preCount = 2;
			row.eff[0] = {Sym::MachineOccupantIsAlive, false};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::TotalEscape;
			row.name = "TOTAL_ESCAPE";
			row.pre[0] = {Sym::IsChained, false};
			row.pre[1] = {Sym::IAmImprisoned, false};
			row.preCount = 2;
			out.push_back(row);
		}
		// The kidnapper's two rows (task ds/slave-goals; dt26 sections 1-4). 132 FIND_AND_KIDNAP is planner-only in the running game
		// (rebirth re-enums every 132 record to 254): its finder picks the nearest downed non-ally and its effect is 153. 254 is a
		// finder-less terminal marker - its precondition list IS its goal test (153, then 154's "carry hand empty" = 47 false), so once
		// both hold nothing runs. The cage half is the package's own 112 row, which re-plans once the body is in the member's arms.
		{
			TaskRow row;
			row.task = routines::task::FindAndKidnap;
			row.name = "FIND_AND_KIDNAP";
			row.goalState = {Sym::IsCarryingTarget, true}; // 48
			row.hasGoalState = true;
			row.pre[0] = {Sym::IsCarryingSomething, false}; // 47 = 0
			row.preCount = 1;
			row.eff[0] = {Sym::FinalKidnapVictim, true}; // 153
			row.effCount = 1;
			row.breaksLocation = true; // the body travels in the member's arms
			out.push_back(row);
		}
		{
			TaskRow row;
			row.task = routines::task::FinalKidnapperCageJob;
			row.name = "FINAL_KIDNAPPER_CAGE_JOB";
			row.pre[0] = {Sym::FinalKidnapVictim, true}; // 153 FINAL_GOT_A_KIDNAP_VICTIM
			row.pre[1] = {Sym::IsCarryingSomething, false}; // 154 FINAL_FOUND_AND_PUT_CARRIED_IN_A_CAGE: the carry hand is empty
			row.preCount = 2;
			out.push_back(row);
		}
		// The other lifts and the arrest / smash goal rows (task ds/locks-save part 2). Every lift row has 68's preconditions and effect and the
		// 68 executor; they are orders (never planner providers: 223 / 273 / 254 keep planning 68). 170 adds needsFirstAid = 0 (so 25 comes first)
		// and also provides IS_CARRYING_TARGET_HEALTHY; 240 adds atATown (the registry: at the home town, target at a town - UNSURE which town).
		for (const std::uint16_t lift : {goap::kLiftObjectHealFirst, goap::kLiftPersonSnatching, goap::kLiftPersonPlayerOrder,
				 goap::kLiftPersonSnatchingInTown, goap::kKidnapOrder})
		{
			TaskRow row;
			row.task = lift;
			row.name = lift == goap::kLiftObjectHealFirst ? "LIFT_OBJECT_BUT_HEAL_FIRST"
				: lift == goap::kLiftPersonSnatching ? "LIFT_PERSON_SNATCHING_ALLOWED"
				: lift == goap::kLiftPersonPlayerOrder ? "LIFT_PERSON_PLAYER_ORDER"
				: lift == goap::kLiftPersonSnatchingInTown ? "LIFT_PERSON_SNATCHING_ALLOWED_IN_TOWN_ONLY" : "KIDNAP_ORDER";
			row.orderOnly = true;
			row.pre[0] = {Sym::HasOneWorkingArm, true};
			row.pre[1] = {Sym::IsMovementAllowed, true};
			row.pre[2] = {Sym::AtLocation, true};
			row.pre[3] = {Sym::IsTargetDown, true};
			row.preCount = 4;
			if (lift == goap::kLiftObjectHealFirst)
				row.pre[row.preCount++] = {Sym::NeedsFirstAid, false};
			if (lift == goap::kLiftPersonSnatchingInTown)
				row.pre[row.preCount++] = {Sym::AtATown, true};
			row.eff[0] = {Sym::IsCarryingTarget, true};
			row.effCount = 1;
			if (lift == goap::kLiftObjectHealFirst)
				row.eff[row.effCount++] = {Sym::IsCarryingTargetHealthy, true};
			row.breaksLocation = true;
			out.push_back(row);
		}
		{
			// 203 ARREST_TARGET (item25, dt24_plantasks section 6): a Task_Blank node; the registry's preconditions are the target down,
			// unarmed and carried, its goal TARGET_IS_ARRESTED. The arrest itself (the cage, the sentence) is the 112 / 108 chain and the crime
			// system's; nothing here writes the goal symbol.
			TaskRow row;
			row.task = routines::task::ArrestTarget;
			row.name = "ARREST_TARGET";
			row.pre[0] = {Sym::IsTargetDown, true};
			row.pre[1] = {Sym::TargetIsUnarmed, true};
			row.pre[2] = {Sym::IsCarryingTarget, true};
			row.preCount = 3;
			row.eff[0] = {Sym::TargetIsArrested, true}; // the goal symbol (no provider but the node: the plan is the lift chain, then 203)
			row.effCount = 1;
			out.push_back(row);
		}
		{
			// 187 _SLAVE_OBEDIENCE (item28, dt24_plantasks section 6): a Task_Blank plan node, goal noneR, no preconditions or effects.
			TaskRow row;
			row.task = routines::task::SlaveObedience;
			row.name = "_SLAVE_OBEDIENCE";
			out.push_back(row);
		}
		{
			// 272 SMASH_BUILDING (Task_BashDoor class): the goal is isBuildingDestroyed and the registry lists no precondition.
			TaskRow row;
			row.task = routines::task::SmashBuilding;
			row.name = "SMASH_BUILDING";
			row.eff[0] = {Sym::BuildingDestroyed, true};
			row.effCount = 1;
			out.push_back(row);
		}
		// 9 / 10 / 11 are Task_Blank goal nodes with no precondition or goal of their own (dt24_plantasks section 5): the choice is the
		// resolver's (101 / 204 for 9, 13's for 10), the fight is the melee row's.
		for (const std::pair<std::uint16_t, const char*> blank : {std::pair<std::uint16_t, const char*>{kChooseEnemyAndAttack, "CHOOSE_ENEMY_AND_ATTACK"},
			{kChooseAttackerOfAlly, "CHOOSE_ATTACKER_OF_ALLY"}, {kAttackCharactersAttacker, "ATTACK_CHARACTERS_ATTACKER"}})
		{
			TaskRow row;
			row.task = blank.first;
			row.name = blank.second;
			out.push_back(row);
		}
		{
			// 59 GET_READY_FOR_ACTION (Task_Blank, item20_static): the only provider of isReadyForAction; stops the movement, no timer.
			TaskRow row;
			row.task = kGetReadyForAction;
			row.name = "GET_READY_FOR_ACTION";
			row.pre[0] = {Sym::IsCrouchingOrLying, false};
			row.pre[1] = {Sym::HasWeaponEquipped, true};
			row.preCount = 2;
			row.eff[0] = {Sym::ReadyForAction, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			// 64 MAKE_TARGET_STAND_STILL: no precondition; the squad order 0x3e (hold) goes on the target's package.
			TaskRow row;
			row.task = kMakeTargetStandStill;
			row.name = "MAKE_TARGET_STAND_STILL";
			row.eff[0] = {Sym::TargetStandingStill, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			// 236 WORSHIP_TARGET (Task_WorshipTarget): approach to 60 u, bow, a 3-14 s timer; the goal symbol is "target worshiped".
			TaskRow row;
			row.task = kWorshipTarget;
			row.name = "WORSHIP_TARGET";
			row.eff[0] = {Sym::TargetWorshiped, true};
			row.effCount = 1;
			out.push_back(row);
		}
		{
			// 249 SPLINT_ORDER (Task_FirstAidRig): goal needsSplint = 0; 250 SPLINT_JOB (Task_Blank) is its resolver goal, as 58 is 25's.
			TaskRow row;
			row.task = kSplintOrder;
			row.name = "SPLINT_ORDER";
			row.eff[0] = {Sym::NeedsSplint, false};
			row.effCount = 1;
			out.push_back(row);
			TaskRow job;
			job.task = kSplintJob;
			job.name = "SPLINT_JOB";
			job.goalState = {Sym::NeedsSplint, false};
			job.hasGoalState = true;
			out.push_back(job);
		}
		// The small tasks (task ds/small-tasks; the registry rows of dt24_plantasks / dt24_47 / dt24_eat / dt24_131 / dt24_stalk_turret). Every row
		// is orderOnly: a plan node a goal or an order names, never a provider, so the symbols the combat, slaver and rest chains already
		// have providers for (isCarryingTarget, isFullyRested, isDialogDelivered, ...) keep their plans. `goalAtLocation` rows carry the
		// registry's goal isAtLocation, which 67 (Running) provides.
		struct SmallRow
		{
			std::uint16_t task;
			const char* name;
			std::initializer_list<Precondition> pre;
			std::initializer_list<Effect> eff;
			bool goalAtLocation;
		};
		const SmallRow small[] =
		{
			{3, "PICKUP", {}, {{Sym::IsSubjectInInventory, true}}, false},
			{12, "PLAYER_TALK_TO", {}, {}, false},
			{14, "IDLE", {}, {}, false},
			{17, "PROTECTION", {{Sym::IsAggressionTowards, true}, {Sym::NeedsFirstAid, true}}, {}, false},
			{19, "GO_HOMEBUILDING", {}, {{Sym::AtHome, true}}, true},
			{24, "WANDERER", {}, {{Sym::AtATown, true}}, true},
			{26, "LOOT_TARGET", {}, {}, false},
			{28, "STAND_UP", {}, {{Sym::IsCrouchingOrLying, false}}, false},
			{30, "HOLD_POSITION", {}, {}, true},
			{31, "STAY_CLOSE_TO_TARGET", {}, {{Sym::IsWithin50Meters, true}}, false},
			{37, "WANDER_TOWN", {{Sym::IsCrouchingOrLying, false}, {Sym::HasWeaponEquipped, false}}, {{Sym::AtNode, true}}, false},
			{47, "STAND_AT_GENERAL_NODE", {}, {{Sym::AtNode, true}}, false},
			{51, "STAND_AT_NODE", {{Sym::AtLocation, true}}, {{Sym::AtNode, true}}, false},
			{62, "STAND_STILL", {{Sym::IsAggressionTowards, false}}, {}, false},
			{69, "PUT_DOWN_OBJECT", {}, {{Sym::IsCarryingSomething, false}}, false},
			{102, "SEEK_AND_TALK_AND_SEND_SIGNAL", {}, {}, false},
			{104, "ALWAYS_IMPOSSIBLE_TASK", {{Sym::Impossible, true}}, {}, false},
			{105, "FIND_AND_RESCUE", {}, {{Sym::IsCarryingTarget, true}}, false},
			{159, "SEND_DIALOGUE_SIGNAL", {}, {{Sym::IsDialogDelivered, true}}, false},
			{164, "FOLLOW_WHILE_TALKING", {{Sym::IsCrouchingOrLying, false}, {Sym::IAmBeingMeleeAttacked, false}, {Sym::IsWithin50Meters, true}}, {}, false},
			{165, "TOWN_STALKER", {}, {}, false},
			{184, "PUT_LOOT_IN_STORAGE", {}, {{Sym::CarryingExcessLoot, false}}, false},
			{231, "EAT_CROPS", {{Sym::FarmHasFood, true}}, {{Sym::AmHungry, false}}, false},
			{232, "FIND_CROPS_TO_EAT", {}, {{Sym::AmHungry, false}}, false},
			{238, "LOOT_ANIMALS_JOB", {{Sym::GotInventoryRoomForResources, true}}, {{Sym::LootedStorableItems, true}}, false},
			{259, "EAT_FOOD_ON_GROUND", {{Sym::GotAFoodItem, true}}, {{Sym::IsSubjectInInventory, true}}, false},
			{261, "SLEEP_ON_FLOOR_FAKE_AMBUSH", {}, {{Sym::IsFullyRested, true}}, false},
			{277, "ANIMAL_FETCH_A_LIMB", {{Sym::GotAFoodItem, true}}, {{Sym::IsSubjectInInventory, true}}, false},
			{278, "PLAY_BECAUSE_I_HAVE_A_LIMB_IN_MOUTH", {}, {{Sym::GotAFoodItem, false}}, false},
			{279, "CHASE_ALLY_DOGS_WITH_MOUTH_LIMBS", {}, {{Sym::IsWithin50Meters, true}}, false},
		};
		for (const SmallRow& spec : small)
		{
			TaskRow row;
			row.task = spec.task;
			row.name = spec.name;
			row.orderOnly = true;
			for (const Precondition& pre : spec.pre)
				row.pre[row.preCount++] = pre;
			for (const Effect& eff : spec.eff)
				row.eff[row.effCount++] = eff;
			if (spec.goalAtLocation)
			{
				row.goalState = {Sym::AtLocation, true};
				row.hasGoalState = true;
			}
			out.push_back(row);
		}
		return out;
	}();
	return rows;
}

// The search context: the live world, the plan hand, and the chain being built in execution order (leaf first).
struct Search
{
	const World& world;
	const std::vector<TaskRow>& table;
	std::uint64_t target;
	double x;
	double z;
	std::vector<Step> chain;
	std::vector<std::pair<std::uint16_t, std::uint64_t>> ancestors; // the parent chain, for the cycle guard
	std::map<std::pair<std::uint16_t, std::uint64_t>, int> visits;
	bool best = false; // planBestWith: every provider is tried and the best chain kept

	double utilityOf(const TaskRow& row, std::uint64_t hand) const
	{
		const double live = world.utility(row.task, hand);
		return live >= 0.0 ? live : row.utility;
	}

	// The plan so far's own effects decide first (the last step that writes the symbol wins), then the live world (dt18 section 4:
	// the evaluator is a live call, not a model). A step that moves the actor ends the reach of an earlier one's position.
	const TaskRow* lookup(std::uint16_t task) const
	{
		for (const TaskRow& row : table)
			if (row.task == task)
				return &row;
		return nullptr;
	}

	bool satisfied(const Precondition& pre) const
	{
		for (auto it = chain.rbegin(); it != chain.rend(); ++it)
		{
			const TaskRow* row = lookup(it->task);
			if (row == nullptr)
				continue;
			for (std::uint8_t e = 0; e < row->effCount; ++e)
				if (row->eff[e].sym == pre.sym)
					return row->eff[e].value == pre.value;
			if (row->breaksLocation && pre.sym == Sym::AtLocation)
				break; // only the position is lost by moving; every other effect a step left stands
		}
		return world.holds(pre.sym, target, x, z) == pre.value;
	}

	// The providers of a symbol with a value, best first: the open set is unsorted and the node taken is the max of g+h (dt18
	// section 3). Ours orders the candidates the same way (g = the weakest link so far, h = the provider's utility).
	std::vector<const TaskRow*> providers(Sym sym, bool value, std::uint64_t hand) const
	{
		std::vector<const TaskRow*> out;
		for (const TaskRow& row : table)
			for (std::uint8_t e = 0; e < row.effCount && !row.orderOnly; ++e)
				if (row.eff[e].sym == sym && row.eff[e].value == value)
					out.push_back(&row);
		// dt18 section 3 takes the max of g+h off an unsorted open set: the higher-utility provider is tried first (a stable order,
		// so equal utilities keep the registry's). Task CF: 13 (4.0) before 234-style rows that cannot plan.
		std::stable_sort(out.begin(), out.end(), [&](const TaskRow* a, const TaskRow* b) { return utilityOf(*a, hand) > utilityOf(*b, hand); });
		return out;
	}

	bool solve(std::uint16_t task, std::uint64_t hand)
	{
		const TaskRow* row = lookup(task);
		if (row == nullptr)
			return false;
		const std::pair<std::uint16_t, std::uint64_t> key{task, hand};
		if (visits[key] >= kVisitCap)
			return false; // dt18 section 5: 4 visits per (task, target, position)
		// The cycle/ancestor guard: the same (task, target) in the parent chain (at most 50 links; dt18 section 5).
		for (std::size_t i = ancestors.size() > kCycleGuard ? ancestors.size() - kCycleGuard : 0; i < ancestors.size(); ++i)
			if (ancestors[i] == key)
				return false;
		if (chain.size() >= kCycleGuard)
			return false;
		++visits[key];
		ancestors.push_back(key);
		const std::size_t before = chain.size();
		// The needs: the goal state first, then the registry's preconditions in order, taken one at a time - a provider appended
		// below may satisfy the next one (its effects are the chain's from then on, dt18 section 4's live re-evaluation).
		for (;;)
		{
			const Precondition* need = nullptr;
			if (row->hasGoalState && !satisfied(row->goalState))
				need = &row->goalState;
			else
				for (std::uint8_t i = 0; i < row->preCount && need == nullptr; ++i)
					if (!satisfied(row->pre[i]))
						need = &row->pre[i];
			if (need == nullptr)
				break;
			bool solved = false;
			const std::vector<const TaskRow*> candidates = providers(need->sym, need->value, hand);
			if (!best)
			{
				for (const TaskRow* provider : candidates)
				{
					if (solve(provider->task, hand))
					{
						solved = true;
						break;
					}
				}
			} else {
				// Each provider's sub-chain is tried on a copy of the search state; the best weakest link (the first on a tie) is
				// replayed for real.
				const TaskRow* winner = nullptr;
				double winning = 0.0;
				for (const TaskRow* provider : candidates)
				{
					const std::vector<Step> savedChain = chain;
					const auto savedAncestors = ancestors;
					const auto savedVisits = visits;
					if (solve(provider->task, hand))
					{
						double weakest = chain.back().g;
						for (std::size_t i = savedChain.size(); i < chain.size(); ++i)
							weakest = std::min(weakest, chain[i].g);
						if (winner == nullptr || weakest > winning)
						{
							winner = provider;
							winning = weakest;
						}
					}
					chain = savedChain;
					ancestors = savedAncestors;
					visits = savedVisits;
				}
				solved = winner != nullptr && solve(winner->task, hand);
			}
			if (!solved)
			{
				chain.resize(before);
				ancestors.pop_back();
				return false;
			}
		}
		chain.push_back(Step{task, hand, utilityOf(*row, hand)});
		ancestors.pop_back();
		return true;
	}
};

} // namespace

const TaskRow* rowOf(std::uint16_t task) noexcept
{
	for (const TaskRow& row : table())
		if (row.task == task)
			return &row;
	return nullptr;
}

bool needsUnarmed(std::uint16_t task) noexcept
{
	const TaskRow* row = rowOf(task);
	if (row == nullptr)
		return false;
	for (std::uint8_t i = 0; i < row->preCount; ++i)
		if (row->pre[i].sym == Sym::HasWeaponEquipped && !row->pre[i].value)
			return true;
	return false;
}

const std::vector<TaskRow>& rows() noexcept
{
	return table();
}

static Plan planImpl(const World& world, std::uint16_t task, std::uint64_t target, double x, double z,
	const std::vector<TaskRow>& table, bool best)
{
	Search search{world, table, target, x, z, {}, {}, {}, best};
	if (!search.solve(task, target))
		return Plan{};
	// g is the weakest link along the chain (dt18 section 3): the score is that minimum, 0 = no plan (section 6).
	double weakest = search.chain.front().g;
	for (Step& step : search.chain)
	{
		weakest = std::min(weakest, step.g);
		step.g = weakest;
	}
	Plan out;
	out.steps = std::move(search.chain);
	out.score = weakest;
	return out;
}

Plan planWith(const World& world, std::uint16_t task, std::uint64_t target, double x, double z,
	const std::vector<TaskRow>& table)
{
	return planImpl(world, task, target, x, z, table, false);
}

Plan planBestWith(const World& world, std::uint16_t task, std::uint64_t target, double x, double z,
	const std::vector<TaskRow>& table)
{
	return planImpl(world, task, target, x, z, table, true);
}

Plan plan(const World& world, std::uint16_t task, std::uint64_t target, double x, double z)
{
	return planWith(world, task, target, x, z, rows());
}

} // namespace ksim::goap
