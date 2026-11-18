#include "ksim/core/simulation.hpp"

#include "world/resident_data.hpp"
#include "world/world_resolvers.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/nav/door_plan.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/world/towncircle.hpp"
#include "ksim/economy/inventory_grid.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace ksim {

namespace {

// The live evaluators (dt18 section 4: AI vtable[0], a live-world call per precondition). One closure per plan call; the plan's
// position is the goal's (the body's spot for 167, the cage's for 112).
struct FnWorld final : goap::World
{
	std::function<bool(goap::Sym, std::uint64_t, double, double)> eval;
	bool holds(goap::Sym sym, std::uint64_t target, double x, double z) const override { return eval(sym, target, x, z); }
};

// The member's reach on the plan's position: at the body it is the chain's own 4 u (carry.cpp kChainReach); at a cage it is the
// piece's functionality `use range` x2, the same gate the put-in uses (interact.cpp functionalityReach, task BY).
constexpr double kChainReach = 4.0;
constexpr double kAwakeBodyReach = 8.0; // carry.cpp: a conscious body keeps its footprint (kenshi-k16.6.28)

} // namespace

goap::Plan Simulation::planSlaveChain(std::uint64_t actor, std::uint16_t goal, std::uint64_t body,
	const routines::Site* cage) const
{
	const state::Fighter* self = combat_ != nullptr ? combat_->find(actor) : nullptr;
	const state::Fighter* dude = combat_ != nullptr && body != 0 ? combat_->find(body) : nullptr;
	if (self == nullptr)
		return {};
	double x = 0.0;
	double z = 0.0;
	double reach = kChainReach;
	bool cageFree = false;
	std::uint64_t cageObject = 0;
	std::uint16_t cageSlot = 0;
	if (cage != nullptr)
	{
		const routines::SiteEntry* entry = residentData_ != nullptr ? residentData_->sites.siteAt(*cage) : nullptr;
		if (entry == nullptr)
			return {};
		x = entry->x;
		z = entry->z;
		reach = gameData_ != nullptr ? functionalityReach(*gameData_, entry->record,
										cage->kind == routines::SiteKind::Bed ? routines::kFunctionBed : routines::kFunctionCage, true)
									 : kChainReach;
		cageFree = true;
		cageObject = cage->object;
		cageSlot = cage->slot;
	} else if (dude != nullptr) {
		x = dude->x;
		z = dude->z;
		reach = dude->unconscious ? kChainReach : kAwakeBodyReach;
	}
	// An arm that is not severed (FighterPart type 2); a race with no arm part at all can lift.
	bool anyArm = false;
	bool wholeArm = false;
	for (const state::FighterPart& part : self->parts)
	{
		if (part.type != 2)
			continue;
		anyArm = true;
		wholeArm = wholeArm || !part.severed();
	}
	const bool workingArm = !anyArm || wholeArm;
	// IS_CARRYING_TARGET / isChained / the carried dude read the plan body; a caging plan's body is what the member carries.
	const state::Fighter* carried = self->carrying != 0 ? (combat_ != nullptr ? combat_->find(self->carrying) : nullptr) : nullptr;
	FnWorld world;
	world.eval = [&](goap::Sym sym, std::uint64_t, double px, double pz) -> bool
	{
		switch (sym)
		{
		case goap::Sym::AtLocation: return (dude == nullptr && cage == nullptr && self->cageBuilding != 0) || std::hypot(self->x - px, self->z - pz) <= reach; // a prisoner is at its cage's lock
		case goap::Sym::IsTargetDown: return dude != nullptr && !dude->dead && (dude->down || dude->unconscious);
		case goap::Sym::TargetIsUnarmed:
		{
			if (dude == nullptr)
				return false;
			const flecs::entity entity = fighterEntity(body);
			const state::Inventory* held = entity.get<state::Inventory>();
			if (held == nullptr)
				return true;
			for (const state::Inventory::Entry& entry : held->items)
				if (entry.slot == state::Inventory::kWeapon)
					return false;
			return true;
		}
		case goap::Sym::HasOneWorkingArm: return workingArm;
		case goap::Sym::TargetIsArrested: return dude != nullptr && dude->cageBuilding != 0; // 203's goal: the arrest ended in a cage (arrest.cpp)
		case goap::Sym::IsMovementAllowed: return !self->dead && !self->unconscious && !self->down && self->cageBuilding == 0;
		case goap::Sym::IsCarryingTarget: return body != 0 && self->carrying == body;
		case goap::Sym::IsCarryingTargetHealthy: return body != 0 && self->carrying == body; // 168 / 108's symbol (the slaver lift)
		case goap::Sym::CarriedDudeIsDead: return (carried != nullptr && carried->dead) || (dude != nullptr && dude->dead);
		case goap::Sym::CagesAreAllFull: return !cageFree || cageObject == 0;
		case goap::Sym::IAmImprisoned: return self->cageBuilding != 0; // 172: the actor itself is in a cage
		case goap::Sym::IsDoorLocked: return self->cageBuilding != 0 && cageDoorLocked(self->cageBuilding); // 207 / 256 (OURS, UNSURE)
		// The lock rows (task ds/shackles-locks). A caged actor with no plan target stands at its own cage's lock.
		case goap::Sym::HasTools: return carriesKit(actor, 9); // ITEM function 9 TOOL (UNSURE: the exe's evaluator is not decoded)
		case goap::Sym::LockSkillOk:
			return self->cageBuilding != 0 && sufficientLockSkill(cageLockLevel(self->cageBuilding), self->skills.lockpicking, true);
		case goap::Sym::ShackleSkillOk: return sufficientLockSkill(20.0, self->skills.lockpicking, false); // lock_tasks.cpp kShackleLockLevel
		case goap::Sym::ShacklesLocked: return shacklesLocked(body != 0 ? body : actor);
		case goap::Sym::TargetIsConscious: return dude != nullptr && !dude->dead && !dude->unconscious;
		case goap::Sym::MachineHasFreeOperatorSlot: return cageFree; // 107: the picked cage is empty (the resolver's freeSlot)
		case goap::Sym::IsCarryingSomething: return self->carrying != 0; // 47: char+0x348 (dt26 section 3)
		// 153 FINAL_GOT_A_KIDNAP_VICTIM: a body in my arms whose kidnap value is above 0. OURS: the value's ally term (0) is not
		// evaluated here - an animal (0) and nothing in the arms are the cases a plan can reach; the ladder itself is putInCage112.
		case goap::Sym::FinalKidnapVictim: return carried != nullptr && !carried->animal;
		case goap::Sym::IsChained: return dude != nullptr && dude->slaveState == 1;
		case goap::Sym::CarriedDudeInCage:
			if (carried == nullptr && dude == nullptr)
				return false;
			if (cageObject != 0)
			{
				const state::Fighter* in = carried != nullptr ? carried : dude;
				return in->cageBuilding == cageObject && in->cageSlot == cageSlot;
			}
			return (carried != nullptr && carried->cageBuilding != 0) || (dude != nullptr && dude->cageBuilding != 0);
		case goap::Sym::CarriedDudeInBed:
		{
			const state::Fighter* in = carried != nullptr ? carried : dude;
			return in != nullptr && in->asleep;
		}
		// Constant false here, by group (no `default:`: a new Sym must be sorted into the live cases or one of these):
		// the melee-joining symbols - combat.cpp combatHolds answers them. A chain whose target is not down yet has no provider for
		// isTargetDown (4 needs ReadyForAction / HasWeaponEquipped / isWithin50Meters, all false), so it gets no plan and the goal scores 0.
		case goap::Sym::IsAggressionTowards:
		case goap::Sym::IsAlliesFighting:
		case goap::Sym::IsEnemy:
		case goap::Sym::ReadyForAction:
		case goap::Sym::IsWithin50Meters:
		case goap::Sym::IAmBeingMeleeAttacked:
		case goap::Sym::IsGeneralAggressionLevel:
		// the occupied-town and homebase rows - planHomebase answers them (no slave-chain row reads them).
		case goap::Sym::AtDestinationTown:
		case goap::Sym::HasWeaponEquipped:
		case goap::Sym::IsCrouchingOrLying:
		case goap::Sym::AtATown:
		case goap::Sym::AtHomeTown:
		case goap::Sym::IsFullyRested:
		case goap::Sym::NoEnemiesInVicinity:
		case goap::Sym::IsInBed:
		case goap::Sym::IsWorkingPretend:
		case goap::Sym::CanGetUp:
		// the intruder eviction and the shoo - planIntruder22x / 27x and planShoo answer them.
		case goap::Sym::IAmInsideTargetBuilding:
		case goap::Sym::IsTargetImprisoned:
		case goap::Sym::IntruderIsOutside:
		case goap::Sym::IntruderIsOutsideGates:
		case goap::Sym::AlarmsInTheVicinity:
		case goap::Sym::StrangersInHouse:
		case goap::Sym::IsDialogDelivered:
		// the machine, loot and food chains - planWorkChain answers them; the corpse pot's disposal state - planCorpsePot.
		case goap::Sym::GotInventoryRoomForResources:
		case goap::Sym::HaveSomeResourcesFrom:
		case goap::Sym::IsMachineOutputFull:
		case goap::Sym::IsMachineInputInvalid:
		case goap::Sym::HaveSomeBuildMats:
		case goap::Sym::HaveSomeLootFrom:
		case goap::Sym::IsWorking:
		case goap::Sym::GotAFoodItem:
		case goap::Sym::CarriedDudeInDisposal:
		// the medic and shopping chains - planMedicChain / planShopping answer them.
		case goap::Sym::NeedsFirstAid:
		case goap::Sym::ShoppingDone:
		// the small-task rows, which run as orders (their tests stage the evaluator; FarmHasFood has crops.cpp behind it but no planner
		// plans 231) and the deliberate failure row 104.
		case goap::Sym::MachineOccupantIsAlive:
		case goap::Sym::BuildingDestroyed:
		case goap::Sym::TargetStandingStill:
		case goap::Sym::TargetWorshiped:
		case goap::Sym::NeedsSplint:
		case goap::Sym::AtNode:
		case goap::Sym::AtHome:
		case goap::Sym::IsSubjectInInventory:
		case goap::Sym::AmHungry:
		case goap::Sym::FarmHasFood:
		case goap::Sym::LootedStorableItems:
		case goap::Sym::CarryingExcessLoot: // 184 runs as the world selector's goal; no chain plans it
		case goap::Sym::Impossible:
		// the town-routine goals - planTownGoal answers them.
		case goap::Sym::DoorIsOpen:
		case goap::Sym::DoorLockedInside:
		case goap::Sym::DoorLockedOutside:
		case goap::Sym::AllDoorsLocked:
		// the door-task rows: decideDoor's evaluators (door_goap.cpp) answer them; no chain here plans a door.
		case goap::Sym::IsObjectMine:
		case goap::Sym::DoorIsBroken:
		case goap::Sym::DoorBashable:
		case goap::Sym::GateBashOpen:
		case goap::Sym::HasWeaponCarried:
		case goap::Sym::AtBuildingDoor:
		case goap::Sym::AtCurrentDoor:
		case goap::Sym::AtDoorInside:
		case goap::Sym::AtDoorOutside:
		// the ranged rows read the combat arena's world (CombatArena's combatHolds); no town chain plans a shot.
		case goap::Sym::HasTwoWorkingArms:
		case goap::Sym::HoldsCrossbow:
		case goap::Sym::TurretFound:
		case goap::Sym::StealthKoOrdered:
		case goap::Sym::OperatesTurret:
			return false;
		case goap::Sym::Count: break;
		}
		return false;
	};
	return goap::plan(world, goal, body, x, z);
}

goap::Plan Simulation::planIntruder223(std::uint64_t actor, std::uint64_t body) const
{
	const state::Fighter* self = combat_ != nullptr ? combat_->find(actor) : nullptr;
	const state::Fighter* dude = combat_ != nullptr && body != 0 ? combat_->find(body) : nullptr;
	if (self == nullptr || dude == nullptr)
		return {};
	// An arm that is not severed (FighterPart type 2); a race with no arm part at all can lift - the same test as the slave chain's.
	bool anyArm = false;
	bool wholeArm = false;
	for (const state::FighterPart& part : self->parts)
	{
		if (part.type != 2)
			continue;
		anyArm = true;
		wholeArm = wholeArm || !part.severed();
	}
	const bool workingArm = !anyArm || wholeArm;
	FnWorld world;
	world.eval = [&](goap::Sym sym, std::uint64_t, double px, double pz) -> bool
	{
		switch (sym)
		{
		case goap::Sym::AtLocation: return std::hypot(self->x - px, self->z - pz) <= kChainReach;
		case goap::Sym::HasOneWorkingArm: return workingArm;
		case goap::Sym::IsMovementAllowed: return !self->dead && !self->unconscious && !self->down && self->cageBuilding == 0;
		case goap::Sym::IsTargetDown: return !dude->dead && (dude->down || dude->unconscious);
		case goap::Sym::IsTargetImprisoned: return dude->cageBuilding != 0;
		case goap::Sym::IsCarryingTarget: return self->carrying == body;
		case goap::Sym::IsCarryingSomething: return self->carrying != 0;
		case goap::Sym::IntruderIsOutside: return dude->building == 0;
		case goap::Sym::Count: break;
		default: break;
		}
		return false;
	};
	return goap::plan(world, routines::task::PickupIntruders, body, dude->x, dude->z);
}

// Task CH group 3 (dt24_273): 273 PICKUP_INTRUDERS_TOWN is 223's registry row under the town's scope - the same goal state
// IS_CARRYING_TARGET from 68 LIFT_PERSON, so the plan is the same walk -> lift -> goal.
goap::Plan Simulation::planIntruder273(std::uint64_t actor, std::uint64_t body) const
{
	const state::Fighter* self = combat_ != nullptr ? combat_->find(actor) : nullptr;
	const state::Fighter* dude = combat_ != nullptr && body != 0 ? combat_->find(body) : nullptr;
	if (self == nullptr || dude == nullptr)
		return {};
	bool anyArm = false;
	bool wholeArm = false;
	for (const state::FighterPart& part : self->parts)
	{
		if (part.type != 2)
			continue;
		anyArm = true;
		wholeArm = wholeArm || !part.severed();
	}
	const bool workingArm = !anyArm || wholeArm;
	FnWorld world;
	world.eval = [&](goap::Sym sym, std::uint64_t, double px, double pz) -> bool
	{
		switch (sym)
		{
		case goap::Sym::AtLocation: return std::hypot(self->x - px, self->z - pz) <= kChainReach;
		case goap::Sym::HasOneWorkingArm: return workingArm;
		case goap::Sym::IsMovementAllowed: return !self->dead && !self->unconscious && !self->down && self->cageBuilding == 0;
		case goap::Sym::IsTargetDown: return !dude->dead && (dude->down || dude->unconscious);
		case goap::Sym::IsTargetImprisoned: return dude->cageBuilding != 0;
		case goap::Sym::IsCarryingTarget: return self->carrying == body;
		case goap::Sym::IsCarryingSomething: return self->carrying != 0;
		case goap::Sym::IntruderIsOutside: return dude->building == 0;
		case goap::Sym::Count: break;
		default: break;
		}
		return false;
	};
	return goap::plan(world, routines::task::PickupIntrudersTown, body, dude->x, dude->z);
}

goap::Plan Simulation::planIntruder224(std::uint64_t actor, std::uint64_t body, double x, double z) const
{
	const state::Fighter* self = combat_ != nullptr ? combat_->find(actor) : nullptr;
	const state::Fighter* dude = combat_ != nullptr && body != 0 ? combat_->find(body) : nullptr;
	if (self == nullptr || dude == nullptr)
		return {};
	FnWorld world;
	world.eval = [&](goap::Sym sym, std::uint64_t, double px, double pz) -> bool
	{
		switch (sym)
		{
		case goap::Sym::AtLocation: return std::hypot(self->x - px, self->z - pz) <= kChainReach;
		case goap::Sym::IsMovementAllowed: return !self->dead && !self->unconscious && !self->down && self->cageBuilding == 0;
		case goap::Sym::IsCarryingSomething: return self->carrying != 0;
		case goap::Sym::IntruderIsOutside: return dude->building == 0;
		default: break;
		}
		return false;
	};
	return goap::plan(world, routines::task::TakeIntruderOutside, body, x, z);
}

// Task CH group 3 (dt24_273 section 1 / CRIME_STATIC "Planner view"): 274's chain once the body is in the arms - 67 Running to the gate
// point (x, z), then TAKE_INTRUDER_OUTSIDE_TOWN. Its goal state INTRUDER_IS_OUTSIDE_GATES is 164's evaluator: true when the body has no
// town, or is carried by me and I stand outside the town's gates (the exe's `carried && vf+0x118 < 1`; ours is the carrier's position,
// perception.hpp inWalls). The target friendly arm of the evaluator is not modelled: a friendly target never gets this goal.
goap::Plan Simulation::planIntruder274(std::uint64_t actor, std::uint64_t body, double x, double z) const
{
	const state::Fighter* self = combat_ != nullptr ? combat_->find(actor) : nullptr;
	const state::Fighter* dude = combat_ != nullptr && body != 0 ? combat_->find(body) : nullptr;
	if (self == nullptr || dude == nullptr)
		return {};
	const perception::Environment& env = combat_->environment();
	FnWorld world;
	world.eval = [&](goap::Sym sym, std::uint64_t, double px, double pz) -> bool
	{
		switch (sym)
		{
		case goap::Sym::AtLocation: return std::hypot(self->x - px, self->z - pz) <= kChainReach;
		case goap::Sym::IsMovementAllowed: return !self->dead && !self->unconscious && !self->down && self->cageBuilding == 0;
		case goap::Sym::IsCarryingSomething: return self->carrying != 0;
		case goap::Sym::IntruderIsOutsideGates:
			return env.townOf(dude->x, dude->z) < 0 || (self->carrying == body && !env.inWalls(self->x, self->z));
		default: break;
		}
		return false;
	};
	return goap::plan(world, routines::task::TakeIntruderOutsideTown, body, x, z);
}

goap::Plan Simulation::planShoo(std::uint64_t actor, std::uint16_t goal) const
{
	const state::Fighter* self = combat_ != nullptr ? combat_->find(actor) : nullptr;
	if (self == nullptr)
		return {};
	FnWorld world;
	world.eval = [&](goap::Sym sym, std::uint64_t, double, double) -> bool
	{
		switch (sym)
		{
		case goap::Sym::IAmInsideTargetBuilding: return self->building != 0; // the goal's own precondition: I stand in the building
		case goap::Sym::AlarmsInTheVicinity: return pickAlarm(actor, false) != nullptr; // the town alarm manager (alarm.hpp, 14092a810)
		case goap::Sym::IsDialogDelivered: return false; // the line is delivered by the executor (dialogue_sim.cpp's shoo, event 9)
		default: break;
		}
		return false;
	};
	return goap::plan(world, goal, 0, self->x, self->z);
}

goap::Plan Simulation::planTownGoal(std::uint64_t actor, std::uint16_t goal, bool strangers, const LockFacts* doors) const
{
	const state::Fighter* self = combat_ != nullptr ? combat_->find(actor) : nullptr;
	if (self == nullptr)
		return {};
	const bool weapon = weaponInHands(actor);
	const bool crouched = self->crouched || self->down || self->unconscious;
	FnWorld world;
	world.eval = [&](goap::Sym sym, std::uint64_t, double, double) -> bool
	{
		switch (sym)
		{
		case goap::Sym::HasWeaponEquipped: return weapon;
		case goap::Sym::IsCrouchingOrLying: return crouched;
		case goap::Sym::StrangersInHouse: return strangers;
		// OURS (UNSURE, dt24_43 / dt24_155): isAtHome is the node's own walk to the home slot, and the 155 / 197 split is the package's
		// (the data lists the one that applies), so 155 reads "inside" and 197 "outside" as holding; the goal states are the node's own run.
		case goap::Sym::AtHome: return true;
		case goap::Sym::IAmInsideTargetBuilding: return goal == routines::task::LockAllMyDoors;
		// The lock goals' symbols from the door facts (155 / 197): the plan is the walk to the door side, the lock row, the goal node.
		case goap::Sym::DoorLockedInside: return doors == nullptr || goal != routines::task::LockAllMyDoors || doors->locked;
		case goap::Sym::DoorLockedOutside: return doors == nullptr || goal != routines::task::LockDoorsStayOutside || doors->locked;
		case goap::Sym::AtDoorInside: return doors != nullptr && doors->at && goal == routines::task::LockAllMyDoors;
		case goap::Sym::AtDoorOutside: return doors != nullptr && doors->at && goal == routines::task::LockDoorsStayOutside;
		case goap::Sym::IsObjectMine: return doors != nullptr;
		case goap::Sym::IsWorkingPretend:
		case goap::Sym::AtNode: return true;
		case goap::Sym::DoorIsOpen: return doors == nullptr; // the lock rows need the door shut (the 155 / 197 plans pass the facts)
		case goap::Sym::AlarmsInTheVicinity: return pickAlarm(actor, false) != nullptr; // the town alarm manager (alarm.hpp, 14092a810)
		case goap::Sym::IsDialogDelivered: return false; // 283's step: the shoo executor delivers the line
		default: break;
		}
		return false;
	};
	return goap::plan(world, goal, 0, self->x, self->z);
}

// atATown (86, dt24_118's getTown): the actor stands inside a town circle (towncircle.hpp).
bool Simulation::inTownCircle(const state::Fighter& who) const
{
	if (gameData_ == nullptr)
		return false;
	std::vector<towncircle::Circle> circles;
	world_.ecs().each([&](const state::TownState& town) { circles.push_back(towncircle::of(*gameData_, town)); });
	return towncircle::at(circles, who.x, who.z) >= 0;
}

// 87 OPERATE_MACHINERY and the goals whose state is IS_WORKING (dt24_emptymachine_source section 3): a worker's plan at a machine.
// The plan position is the machine's use point and the reach its `use range` (MQ9: an operator arrives within it, a mine's 100).
// The live symbols are the machine's own - a full output slot, the actor already in its operator list - and the room in the
// actor's inventory for one more unit, which is the pull's precondition: a full carrier plans no pull.
// A storage box (a state::Container, 284 / 131) plans the same way without the machine's symbols: the item is its first held type,
// the reach the interact reach (its executor's), and HAVE_SOME_LOOT_FROM holds while it holds anything.
goap::Plan Simulation::planWorkChain(std::uint64_t actor, std::uint16_t goal, std::uint64_t buildingId) const
{
	const state::Fighter* self = combat_ != nullptr ? combat_->find(actor) : nullptr;
	if (self == nullptr)
		return {};
	flecs::entity entity;
	world_.ecs().each([&](flecs::entity e, const state::Id& id)
	{
		if (id.value == buildingId)
			entity = e;
	});
	if (!entity.is_valid())
		return {};
	const state::Producer* machine = entity.get<state::Producer>();
	const state::Container* box = machine == nullptr ? entity.get<state::Container>() : nullptr;
	const state::Inventory* stock = entity.get<state::Inventory>();
	if ((machine == nullptr && box == nullptr) || (machine != nullptr && stock == nullptr))
		return {};
	// The item one unit of the pull takes: a machine's output slot, a box's first held stack (the executor's own walk).
	std::string item = machine != nullptr ? machine->output : std::string();
	if (box != nullptr && stock != nullptr)
	{
		for (const state::Inventory::Entry& entry : stock->items)
			if (entry.slot == state::Inventory::kCarried && entry.count > 0)
			{
				item = entry.itemSid;
				break;
			}
	}
	const bool full = machine != nullptr
		&& (machine->prodState == 1 || (stock != nullptr && machine->cap != 0 && stock->carried(machine->output) >= machine->cap));
	// An automatic producer (maxOperators 0, a drill) takes no operator - interact.cpp's slot rule - and needs none: IS_WORKING
	// holds there without a person (the exe's OPERATE_AUTOMATIC_MACHINERY 152 is its own task).
	const bool working = machine != nullptr
		&& (machine->maxOperators == 0 || std::find(machine->operators.begin(), machine->operators.end(), actor) != machine->operators.end());
	const flecs::entity who = fighterEntity(actor);
	const state::Inventory* packed = who.is_valid() ? who.get<state::Inventory>() : nullptr;
	// haveSomeResourcesFrom / HAVE_SOME_LOOT_FROM: the actor HOLDS what it pulled (92's / 284's effect) - an empty-handed worker
	// plans the pull, one that took its take goes on to the operate step or holds the goal.
	// A pull from a store that is not mine is a raid (131 for a raider, rwh.53 section 4.6): the exe's HAVE_SOME_LOOT_FROM never holds for a
	// plain store, so the pull repeats; ours ends it when the inventory is full (no room: no plan) or the store is empty (no stock: no hand).
	const state::Allegiance* mine = who.is_valid() ? who.get<state::Allegiance>() : nullptr;
	const bool raid = box != nullptr && goal == routines::task::LootFoodAndStuff && (mine == nullptr || box->ownerSid != mine->factionSid);
	const bool held = !raid && packed != nullptr && !item.empty() && packed->carried(item) > 0;
	// gotAFoodItem (149, dt24_243): the actor carries a food item (ITEM function 3) - 243's goal state, and the plan for its
	// goal [67, 244 GRAB_ONE_FOOD, 243] is planned while it does not.
	bool food = false;
	if (packed != nullptr)
		for (const state::Inventory::Entry& entry : packed->items)
		{
			const ItemInfo* info = entry.slot == state::Inventory::kCarried ? combat_->data().item(entry.itemSid) : nullptr;
			if (info != nullptr && info->function == kItemFunctionFood)
			{
				food = true;
				break;
			}
		}
	// isAlliesFighting (12): a squadmate of mine is in a fight (a live target). The exe reads the squad's fighting set; ours is the
	// same live test squadInFight uses in world_tasks.cpp, for the actor's own squad.
	bool alliesFighting = false;
	for (const CombatArena::Member& other : combat_->members())
	{
		if (other.id == actor || other.fighter.squad != self->squad || other.fighter.dead)
			continue;
		const state::Fighter* target = other.fighter.target != 0 ? combat_->find(other.fighter.target) : nullptr;
		if (target != nullptr && !target->dead && !target->unconscious && !target->down && target->carriedBy == 0)
		{
			alliesFighting = true;
			break;
		}
	}
	bool room = false;
	if (gameData_ != nullptr && !item.empty())
	{
		state::Inventory probe = packed != nullptr ? *packed : state::Inventory{};
		const grid::Rules rules(*gameData_);
		state::Inventory::Entry one;
		one.itemSid = item;
		one.count = 1;
		one.slot = state::Inventory::kCarried;
		one.section = state::Inventory::Entry::kUnplaced;
		room = grid::autoplace(probe, rules, one) == 0;
	}
	const double useX = machine != nullptr ? machine->useX : box->useX;
	const double useZ = machine != nullptr ? machine->useZ : box->useZ;
	const double reach = machine != nullptr ? machine->useRange : kInteractReach;
	const bool atTown = inTownCircle(*self);
	FnWorld world;
	world.eval = [&](goap::Sym sym, std::uint64_t, double px, double pz) -> bool
	{
		switch (sym)
		{
		case goap::Sym::AtLocation: return std::hypot(self->x - px, self->z - pz) <= reach;
		// A walker's gate (67 Running's precondition): a member that cannot move has no plan to the building (dt24_243 plans
		// [67, 244, 243] from away; the pull's executor walks the same points).
		case goap::Sym::IsMovementAllowed: return !self->dead && !self->unconscious && !self->down && self->cageBuilding == 0;
		case goap::Sym::IsMachineOutputFull: return full;
		case goap::Sym::IsMachineInputInvalid: return false; // no crafting-input test in the sim (92's row writes it, nothing reads it)
		case goap::Sym::AtATown: return atTown; // 214's precondition (dt24_214)
		case goap::Sym::IsWorking: return working;
		case goap::Sym::GotInventoryRoomForResources: return room;
		case goap::Sym::HaveSomeResourcesFrom: return held;
		case goap::Sym::HaveSomeLootFrom: return held;
		case goap::Sym::GotAFoodItem: return food;
		case goap::Sym::IsAlliesFighting: return alliesFighting;
		case goap::Sym::Count: break;
		default: break;
		}
		return false;
	};
	return goap::plan(world, goal, buildingId, useX, useZ);
}

// Lying in a real bed: asleep, not caged, and either on its own bed routine (the home-bed goals) or put in the bed by a carrier (99
// PUT_SOMEONE_IN_BED leaves a Routine with no goal whose site is the bed). A floor sleeper has no bed site.
bool Simulation::lyingInBed(std::uint64_t id, const state::Fighter& f) const
{
	if (!f.asleep || f.cageBuilding != 0)
		return false;
	const flecs::entity who = fighterEntity(id);
	const state::Routine* routine = who.is_valid() ? who.get<state::Routine>() : nullptr;
	return routine != nullptr && (routine->goal == routines::task::GoHomeToBed || routine->goal == routines::task::GoHomeToBedSecure
		|| (routine->goal == 0 && routine->target.kind == static_cast<std::uint8_t>(routines::SiteKind::Bed)));
}

// The portrait's hurt (the exe's 1404123a0 minus its camera gates, which are the client's; rwh.117): not incapacitated (KO, dead, down,
// carried), engaged (OURS stand-in for the AI+0xb0 float, UNSURE k16.6.159: Fighter::engagedUntil, set while a living target or a carried
// body, held 15 s) and not caged; or a live eat application on it.
bool Simulation::portraitHurt(std::uint64_t id, const state::Fighter& f) const
{
	const auto eaten = eatenUntil_.find(id);
	if (eaten != eatenUntil_.end() && eaten->second > clockTick())
		return true;
	const bool incapacitated = f.unconscious || f.dead || f.down || f.carriedBy != 0;
	return !incapacitated && f.cageBuilding == 0 && clockTick() < f.engagedUntil;
}

// G4, the homebase rows (dt24_243 / dt24_275 / dt24_175): a plan against (x, z) with the homebase symbols live. 275 SIT_AROUND
// plans [190 AUTO_LABOURING_MINES_PRETEND, 275] (190 the only row whose effect is IS_WORKING_PRETEND; 221 writes it too but is a
// target task of its own); 175 SLEEP_ON_FLOOR has no goal state, so a member whose preconditions all hold plans [175] alone - the
// "sleep where I stand" terminal rest. The 175 row's symbol 6 is read as isNoEnemiesInVicinity (see Sym::NoEnemiesInVicinity).
goap::Plan Simulation::planHomebase(std::uint64_t actor, std::uint16_t goal, double x, double z) const
{
	const state::Fighter* self = combat_ != nullptr ? combat_->find(actor) : nullptr;
	if (self == nullptr)
		return {};
	// atATown: the town circle I stand in (dt24_118's getTown, towncircle.hpp); isAtHomeTown: that town is my squad's home town.
	std::vector<towncircle::Circle> circles;
	std::vector<flecs::entity_t> townIds;
	if (gameData_ != nullptr)
		world().ecs().each([&](flecs::entity entity, const state::TownState& town)
		{
			circles.push_back(towncircle::of(*gameData_, town));
			townIds.push_back(entity.id());
		});
	const int at = towncircle::at(circles, self->x, self->z);
	const flecs::entity who = fighterEntity(actor);
	bool homeTown = false;
	if (at >= 0)
	{
		const flecs::entity squad = who.is_valid() ? who.target<state::MemberOf>() : flecs::entity{};
		const state::SquadInstance* instance = squad.is_valid() ? squad.get<state::SquadInstance>() : nullptr;
		homeTown = instance != nullptr && instance->homeTown.id != 0 && instance->homeTown.id == townIds[static_cast<std::size_t>(at)];
	}
	// hasWeaponEquipped / IS_CARRYING_SOMETHING: the inventory's weapon slot and the body in the arms. OURS: an animal's natural
	// weapon (its `_bull type weak`-style record in that slot) is not an equipped weapon - the exe's animal has no weapon hand, and
	// 175 needs hasWeaponEquipped = 0 (UNSURE: the predicate's animal arm was not decoded).
	const bool weapon = weaponInHands(actor);
	const bool carrying = self->carrying != 0;
	const bool inBed = lyingInBed(actor, *self);
	// IS_CROUCHING_OR_LYING: the crouched stance, a down body or an unconscious one (no order sets crouched yet).
	const bool crouched = self->crouched || self->down || self->unconscious;
	// isNoEnemiesInVicinity: I am in no fight - no live target of mine, and no one whose live target is me.
	bool inFight = self->target != 0;
	if (!inFight)
		for (const CombatArena::Member& other : combat_->members())
			if (other.fighter.target == actor && !other.fighter.dead)
			{
				inFight = true;
				break;
			}
	FnWorld world;
	world.eval = [&](goap::Sym sym, std::uint64_t, double, double) -> bool
	{
		switch (sym)
		{
		case goap::Sym::AtATown: return at >= 0;
		case goap::Sym::AtHomeTown: return homeTown;
		case goap::Sym::HasWeaponEquipped: return weapon;
		case goap::Sym::IsCrouchingOrLying: return crouched;
		case goap::Sym::IsCarryingSomething: return carrying;
		case goap::Sym::NoEnemiesInVicinity: return !inFight;
		case goap::Sym::IsGeneralAggressionLevel: return false; // no aggression-level model: 175's precondition holds
		case goap::Sym::IsWorkingPretend: return false; // the effect of 190, the step the plan puts in front of 275
		case goap::Sym::IsFullyRested: return false; // no fatigue model (the effect only)
		case goap::Sym::IsInBed: return inBed; // 83: asleep in a bed (a floor sleeper has none)
		case goap::Sym::IsDoorLocked: return self->cageBuilding != 0 && cageDoorLocked(self->cageBuilding); // 207 / 256 (OURS, UNSURE)
		case goap::Sym::CanGetUp: return !self->dead && !self->unconscious && !self->down && self->carriedBy == 0; // 45: UNSURE, not decoded
		default: break;
		}
		return false;
	};
	return goap::plan(world, goal, 0, x, z);
}

// Task CG (dt24_58.md): the medic's chain. 58 JOB_MEDIC is Task_Blank - its row is needsFirstAid = 0 - and 25 FIRST_AID_ORDER is the
// only task that clears it, so a patient that still needs aid plans [25, 58] and one that does not plans [58] alone (the goal already
// holds; the selector scores it 0). One symbol is live: whether the injury the medic's kits reach is still left on the patient.
goap::Plan Simulation::planMedicChain(std::uint64_t actor, std::uint64_t patient) const
{
	const state::Fighter* self = combat_ != nullptr ? combat_->find(actor) : nullptr;
	const state::Fighter* body = combat_ != nullptr && patient != 0 ? combat_->find(patient) : nullptr;
	if (self == nullptr || body == nullptr)
		return {};
	const worldres::Kits kits = worldres::kitsOf(actor, *self,
		[this](std::uint64_t id, std::int32_t function) { return carriesKit(id, function); });
	const bool needsAid = worldres::needsAid(*body, kits, worldres::bothCategories(*self));
	FnWorld world;
	world.eval = [&](goap::Sym sym, std::uint64_t, double, double) -> bool
	{
		return sym == goap::Sym::NeedsFirstAid && needsAid; // the one symbol the medic rows read
	};
	return goap::plan(world, routines::task::FirstAid, patient, body->x, body->z);
}

// 269 HEAL_MY_LEGS (k16.6.4; dt45_down_treatment.md, MEDICAL_HUNGER_LIVE.md (f)): the game gives it to a down, conscious NPC that carries a
// first-aid kit; the plan is the registry row's one step (Task_FirstAid on itself). Nothing here knows who the character is: a medic
// and an ordinary squadmate run it the same way, and a player character is commanded, never auto-treated (OURS).
goap::Plan Simulation::planSelfAid(std::uint64_t actor) const
{
	const state::Fighter* self = combat_ != nullptr ? combat_->find(actor) : nullptr;
	if (self == nullptr || !worldres::healMyLegs269(*self))
		return {};
	const worldres::Kits kits = worldres::kitsOf(actor, *self,
		[this](std::uint64_t id, std::int32_t function) { return carriesKit(id, function); });
	if (!worldres::needsAid(*self, kits, worldres::bothCategories(*self)))
		return {};
	FnWorld world;
	world.eval = [](goap::Sym sym, std::uint64_t, double, double) -> bool { return sym == goap::Sym::NeedsFirstAid; };
	return goap::plan(world, goap::kHealMyLegs, actor, self->x, self->z);
}

// G4, SHOPPING 118 (task CH group 3; kinspect dt24_118 / dt24_shop): [67 Running to the counter (x, z), 119 BUY_SHIT, 118]. `done` is
// the goal state `?local_2e60` (the member's list has nothing left to buy); atATown is the member's position's town.
goap::Plan Simulation::planShopping(std::uint64_t actor, bool done, double x, double z) const
{
	const state::Fighter* self = combat_ != nullptr ? combat_->find(actor) : nullptr;
	if (self == nullptr)
		return {};
	// atATown: the member stands inside a town's circle (the exe's getTown, towncircle.hpp). Its home town is not consulted - the
	// exe reads the town it stands in (dt24_118 section 2: the town's cell, the town's list).
	std::vector<towncircle::Circle> circles;
	world().ecs().each([&](const state::TownState& town) { circles.push_back(towncircle::of(*gameData_, town)); });
	const bool inTown = towncircle::at(circles, self->x, self->z) >= 0;
	FnWorld world;
	world.eval = [&](goap::Sym sym, std::uint64_t, double px, double pz) -> bool
	{
		switch (sym)
		{
		case goap::Sym::AtATown: return inTown;
		case goap::Sym::AtLocation: return std::hypot(self->x - px, self->z - pz) <= kChainReach;
		case goap::Sym::ShoppingDone: return done;
		default: break;
		}
		return false;
	};
	return goap::plan(world, routines::task::Shopping, 0, x, z);
}

// G4, the corpse pot (task CH town-residents-3; dt24_patrol_town_pot section 3). 137 plans against the live world: empty-handed its
// goal state 100 already holds ("I am not carrying anything", 14060e5c0), so the plan is the fetch half - 67 Running to the body,
// 68 LIFT_PERSON, 138 FIND_A_DEAD_GUY (the marker that effects 99) - and the member re-plans once the body is in its arms, when the
// goal state is false and 136 FEED_CORPSE_INTO_MACHINE is its provider: 67 Running to the pot (x, z), then 136. A corpse is a lift
// target: the plan's isTargetDown reads "down, KO'd or dead" (the exe's 68 takes a dead body here). The marker entries 137 / 138 are
// Task_Blank goal rows; the executor pops them.
goap::Plan Simulation::planCorpsePot(std::uint64_t actor, std::uint64_t body, double x, double z, std::uint16_t goal) const
{
	const state::Fighter* self = combat_ != nullptr ? combat_->find(actor) : nullptr;
	const state::Fighter* dude = combat_ != nullptr && body != 0 ? combat_->find(body) : nullptr;
	if (self == nullptr || dude == nullptr)
		return {};
	bool anyArm = false;
	bool wholeArm = false;
	for (const state::FighterPart& part : self->parts)
	{
		if (part.type != 2)
			continue;
		anyArm = true;
		wholeArm = wholeArm || !part.severed();
	}
	const bool workingArm = !anyArm || wholeArm;
	const state::Fighter* carried = self->carrying != 0 ? combat_->find(self->carrying) : nullptr;
	FnWorld world;
	world.eval = [&](goap::Sym sym, std::uint64_t, double px, double pz) -> bool
	{
		switch (sym)
		{
		case goap::Sym::AtLocation: return std::hypot(self->x - px, self->z - pz) <= kChainReach;
		case goap::Sym::IsTargetDown: return dude->down || dude->unconscious || dude->dead;
		case goap::Sym::HasOneWorkingArm: return workingArm;
		case goap::Sym::IsMovementAllowed: return !self->dead && !self->unconscious && !self->down && self->cageBuilding == 0;
		case goap::Sym::IsCarryingTarget: return self->carrying == body;
		case goap::Sym::IsCarryingSomething: return self->carrying != 0;
		case goap::Sym::CarriedDudeIsDead: return carried != nullptr && carried->dead; // MY arms, never the plan body (1405a14e0)
		case goap::Sym::CarriedDudeInDisposal: return self->carrying == 0; // condition 100's evaluator: "I carry nothing"
		default: break;
		}
		return false;
	};
	return goap::plan(world, goal, body, x, z); // 137, or 138 on its own: [67, 68, 138]
}

} // namespace ksim
