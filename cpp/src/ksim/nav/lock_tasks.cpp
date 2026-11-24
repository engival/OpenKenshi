// The lock tasks (task ds/shackles-locks): 76 PICK_LOCK / 285 CUT_LOCK / 286 BRUTE_FORCE_LOCK on a cage, 201 / 185 / 186 on worn shackles,
// 219 TAKE_OFF_MY_SHACKLES, 110 RELEASE_PRISONER and 133 GET_OUT_OF_CAGE_LEGIT. One Lock model (kinspect dt24_picklock.md section 8): a
// picking timer shared by every picker, a hardness (lock +4) the cut drains, and an open flag; only where the Lock lives differs.
// Decodes: dt24_shackles.md (185 / 186 / 219), dt24_picklock.md (201, the events and the crime), item28_static.md b2 / b6 (cage locks,
// 110, 133), dt24_207.md (the shared Task_GetOutOfCage action).

#include "world/resident_data.hpp"
#include "ksim/core/simulation.hpp"

#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/law/crime.hpp"
#include "ksim/nav/door_plan.hpp"
#include "ksim/world/interact.hpp"

#include "kdata/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace ksim {

namespace {

constexpr int kEventUnlockSuccess = 0x0d; // EV_UNLOCK_MY_CAGE_OR_SHACKLES (dt24_picklock section 7.3)
constexpr int kEventUnlockAttempt = 0x0e; // EV_UNLOCK_MY_CAGE_ATTEMPT
constexpr int kEventReleased = 0x2e; // posted by Task_ReleasePrisoner (item28 b6)
constexpr std::int32_t kItemFunctionTool = 9; // ITEM `item function` 9 TOOL (GAMEDATA_SCHEMA); the exe's hasTools evaluator is not decoded (UNSURE)

}

// The worn shackles' own lock, from their ARMOUR record: `lock level` (the pick roll) and `hardness` (the cut / brute work value, lock +4)
// (rwh205_shackle_lock_fields.md; the item factory builds the Lock from them). The plain and Hive Prisoner Shackles are 20 / 20.0; the Faulty
// ones 5 / 10.0. The fallbacks are the schema defaults (GAMEDATA_SCHEMA).
const kdata::Record* Simulation::shackleRecord(std::uint64_t wearer) const
{
	const flecs::entity entity = gameData_ != nullptr ? fighterEntity(wearer) : flecs::entity();
	const state::Inventory* held = entity.is_valid() ? entity.get<state::Inventory>() : nullptr;
	if (held != nullptr)
		for (const state::Inventory::Entry& entry : held->items)
			if (entry.slot == state::Inventory::kArmourBase + 9)
				if (const kdata::Record* record = gameData_->find(entry.itemSid); record != nullptr && record->boolOr("is locked", false))
					return record;
	return gameData_ != nullptr ? gameData_->find("SHACKLES") : nullptr;
}

double Simulation::shackleLockLevel(std::uint64_t wearer) const
{
	const kdata::Record* record = shackleRecord(wearer);
	return record != nullptr ? std::clamp(record->intOr("lock level", 20), 0, 100) : 20.0;
}

double Simulation::shackleHardness(std::uint64_t wearer) const
{
	const kdata::Record* record = shackleRecord(wearer);
	return record != nullptr ? static_cast<double>(record->floatOr("hardness", 20.0f)) : 20.0;
}

bool Simulation::shacklesWorn(std::uint64_t wearer) const
{
	const kdata::Record* record = gameData_ != nullptr ? gameData_->find("SHACKLES") : nullptr;
	const flecs::entity entity = fighterEntity(wearer);
	const state::Inventory* held = record != nullptr && entity.is_valid() ? entity.get<state::Inventory>() : nullptr;
	if (held == nullptr)
		return false;
	for (const state::Inventory::Entry& entry : held->items)
		if (entry.itemSid == record->stringID && entry.slot == state::Inventory::kArmourBase + 9)
			return true;
	return false;
}

const Simulation::LockRecord* Simulation::lockFind(bool shackles, std::uint64_t id) const
{
	const state::LockStates* held = world_.ecs().get<state::LockStates>();
	if (held == nullptr)
		return nullptr;
	for (const LockRecord& entry : held->entries)
		if (entry.shackles == shackles && entry.id == id)
			return &entry;
	return nullptr;
}

Simulation::LockRecord* Simulation::lockFind(bool shackles, std::uint64_t id)
{
	return const_cast<LockRecord*>(std::as_const(*this).lockFind(shackles, id));
}

Simulation::LockRecord& Simulation::lockEnsure(bool shackles, std::uint64_t id)
{
	if (LockRecord* found = lockFind(shackles, id))
		return *found;
	if (world_.ecs().get<state::LockStates>() == nullptr)
		world_.ecs().set<state::LockStates>({});
	std::vector<LockRecord>& entries = world_.ecs().get_mut<state::LockStates>()->entries;
	LockRecord& made = entries.emplace_back();
	made.shackles = shackles;
	made.id = id;
	return made;
}

void Simulation::lockErase(bool shackles, std::uint64_t id)
{
	if (lockFind(shackles, id) == nullptr)
		return;
	std::erase_if(world_.ecs().get_mut<state::LockStates>()->entries,
		[&](const LockRecord& entry) { return entry.shackles == shackles && entry.id == id; });
}

bool Simulation::shacklesLocked(std::uint64_t wearer) const
{
	if (!shacklesWorn(wearer))
		return false;
	const LockRecord* lock = lockFind(true, wearer);
	return lock == nullptr || !lock->open;
}

void Simulation::removeShackles(std::uint64_t wearer)
{
	const kdata::Record* record = gameData_ != nullptr ? gameData_->find("SHACKLES") : nullptr;
	flecs::entity entity = fighterEntity(wearer);
	state::Fighter* f = combat_ != nullptr ? combat_->find(wearer) : nullptr;
	const state::Inventory* held = record != nullptr && entity.is_valid() ? entity.get<state::Inventory>() : nullptr;
	if (held == nullptr || f == nullptr)
		return;
	state::Inventory inventory = *held;
	const auto worn = std::find_if(inventory.items.begin(), inventory.items.end(), [&](const state::Inventory::Entry& entry)
		{ return entry.itemSid == record->stringID && entry.slot == state::Inventory::kArmourBase + 9; });
	if (worn == inventory.items.end())
		return;
	inventory.items.erase(worn);
	++inventory.version;
	entity.set<state::Inventory>(std::move(inventory));
	f->armour.erase(std::remove_if(f->armour.begin(), f->armour.end(), [](const state::FighterArmour& p) { return p.slot == 9; }), f->armour.end());
	lockErase(true, wearer);
	if (f->slaveState == 1)
		setSlaveState(wearer, 2); // OURS (UNSURE): the unchained slave is an escaping one, as after 207's leave
	mirrorFighter(wearer);
}

void Simulation::leaveCage(std::uint64_t id, std::uint16_t goal, bool escape)
{
	state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr || f->cageBuilding == 0)
		return;
	{
		std::ostringstream line;
		line << "escape " << goal << " member " << id << " left cage " << f->cageBuilding << ":" << f->cageSlot
			 << (escape && f->slaveState == 1 ? " (slave state 2)" : "");
		logTask(line.str());
	}
	f->cageBuilding = 0;
	f->cageSlot = 0;
	if (escape && f->slaveState == 1)
		setSlaveState(id, 2);
	routineBook_.release(id);
	mirrorFighter(id);
}

bool Simulation::lockProgress(std::uint64_t id, TaskProgressRow& row) const
{
	const state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr)
		return false;
	std::uint16_t task = 0;
	std::uint64_t victim = 0;
	if (const auto order = lockOrders_.find(id); order != lockOrders_.end())
	{
		task = order->second.task;
		victim = order->second.victim;
	} else if (const state::Routine* r = fighterEntity(id).get<state::Routine>(); r != nullptr && f->cageBuilding != 0 && r->step < r->plan.size()) {
		task = r->plan[r->step]; // an escape: the plan's lock step on its own cage
	}
	const bool shackles = task == routines::task::PickLockOnShackles || task == routines::task::CutShackles;
	if (shackles)
	{
		// A worn pair of shackles (order 60 action 15 / 16): the lock is the wearer's, the row's target is the wearer.
		const state::Fighter* wearer = combat_->find(victim);
		const LockRecord* own = wearer != nullptr ? lockFind(true, victim) : nullptr;
		if (own == nullptr || own->open || (victim != id && std::hypot(f->x - wearer->x, f->z - wearer->z) > lockReach(*wearer, true)))
			return false; // still walking
		row.id = id;
		row.target = victim;
		row.flags = kProgressOriginal;
		if (task == routines::task::PickLockOnShackles)
		{
			if (own->interval <= 0.0)
				return false;
			row.kind = 1;
			row.progress = static_cast<float>(std::clamp(own->elapsed / own->interval, 0.0, 1.0));
			row.rate = static_cast<float>(1.0 / own->interval);
			row.value = static_cast<float>(pickChance(shackleLockLevel(victim), f->skills.lockpicking));
			return true;
		}
		const double full = shackleHardness(victim);
		const double left = own->hardness < 0.0 ? full : own->hardness;
		row.kind = 7;
		row.progress = static_cast<float>(std::clamp(1.0 - left / full, 0.0, 1.0));
		row.rate = static_cast<float>(cutRate(f->skills.strength) / full);
		row.value = static_cast<float>(left);
		return true;
	}
	if (task != routines::task::PickLock && task != routines::task::CutLock)
		return false;
	const state::Fighter* target = victim != 0 && victim != id ? combat_->find(victim) : f;
	if (target == nullptr || target->cageBuilding == 0 || !cageDoorLocked(target->cageBuilding))
		return false;
	if (target != f && std::hypot(f->x - target->x, f->z - target->z) > lockReach(*target, true))
		return false; // still walking
	const std::uint64_t building = target->cageBuilding;
	const LockRecord* lock = lockFind(false, building);
	if (lock == nullptr || lock->open)
		return false;
	row.id = id;
	row.target = building;
	row.flags = kProgressOriginal;
	if (task == routines::task::PickLock)
	{
		const double skill = f->skills.lockpicking - (f->cageBuilding != 0 ? kCagePickSkillPenalty : 0.0);
		row.kind = 1;
		row.progress = static_cast<float>(std::clamp(lock->elapsed / lock->interval, 0.0, 1.0));
		row.rate = static_cast<float>(1.0 / lock->interval);
		row.value = static_cast<float>(pickChance(cageLockLevel(building), skill));
		return true;
	}
	// A cut: the hardness drains at cutRate(strength) per second from the lock's start value; kind 7 reads 1 - hardness / start, the
	// value is what is left (the same meaning as the shackles' cut).
	const double start = cageLockHardness(building);
	const double left = lock->hardness < 0.0 ? start : lock->hardness;
	row.kind = 7;
	row.progress = static_cast<float>(std::clamp(1.0 - left / start, 0.0, 1.0));
	row.rate = static_cast<float>(cutRate(f->skills.strength) / start);
	row.value = static_cast<float>(left);
	return true;
}

Simulation::LockStep Simulation::stepLockTask(std::uint64_t id, std::uint16_t task, std::uint64_t victim)
{
	state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr || f->dead || f->unconscious || f->down || f->carriedBy != 0)
		return LockStep::Refused;
	state::Fighter* target = victim != 0 && victim != id ? combat_->find(victim) : f;
	if (target == nullptr)
		return LockStep::Refused;
	const std::uint64_t targetId = target == f ? id : victim;
	const bool others = target != f;
	const double strength = f->skills.strength;
	const auto logLine = [&](const std::string& what)
	{
		std::ostringstream line;
		line << "lock " << task << " member " << id << " " << what;
		logTask(line.str());
	};

	if (task == routines::task::GetOutOfCageLegit)
	{
		// 133 (mode 0): the plain leave; its precondition is the open door (item28 b2).
		if (f->cageBuilding == 0)
			return LockStep::Done;
		if (cageDoorLocked(f->cageBuilding))
			return LockStep::Refused;
		leaveCage(id, task, false);
		return LockStep::Done;
	}

	if (task == routines::task::ReleasePrisoner)
	{
		// 110 Task_ReleasePrisoner (item28 b6): the target must be conscious and in a cage. The releaser's faction clears the bounty the cage
		// faction held, the prisoner is given 133 and leaves without a crime or a slave state change, and event 0x2e is posted.
		if (!others || target->dead || target->unconscious || target->cageBuilding == 0)
			return LockStep::Refused;
		// Both are legal factions (CRIME_STATIC: the bounty is held by the faction's legal system, not the faction itself).
		if (const state::Allegiance* own = fighterEntity(id).get<state::Allegiance>(); own != nullptr && !own->factionSid.empty())
			clearBounty(victim, crime::legalFaction(gameData_.get(), own->factionSid));
		const routines::BuildingSites* cageOf = residentData_ != nullptr ? residentData_->sites.find(target->cageBuilding) : nullptr;
		if (cageOf != nullptr && !cageOf->ownerFaction.empty())
			clearBounty(victim, crime::legalFaction(gameData_.get(), cageOf->ownerFaction));
		// Task_ReleasePrisoner::vf1 (rwh.100 1403570a0): the cage's lock is cleared first, unconditionally (no level, skill or tool), then the prisoner is
		// given 133, whose open-door precondition now holds. The cage reads unlocked by every source cageDoorLocked consults (the record, the scenario's mark).
		lockEnsure(false, target->cageBuilding).open = true;
		scenarioLockedCages_.erase(target->cageBuilding);
		fireDialogue(id, victim, kEventReleased);
		logLine("released prisoner " + std::to_string(victim));
		leaveCage(victim, routines::task::GetOutOfCageLegit, false);
		return LockStep::Done;
	}

	if (task == routines::task::TakeOffMyShackles)
	{
		// 219 Task_TakeOffCuffs (dt24_shackles section 5): instant, no skill; the planner reaches it once isWearingShackles_locked = 0.
		if (shacklesLocked(id))
			return LockStep::Refused;
		if (shacklesWorn(id))
		{
			removeShackles(id);
			logLine("took its shackles off");
		}
		return LockStep::Done;
	}

	// ---- the lock itself: a cage (76 / 285 / 286) or a pair of shackles (201 / 185 / 186) ----
	const bool shackles = task == routines::task::PickLockOnShackles || task == routines::task::CutShackles || task == routines::task::BruteForceShackles;
	LockRecord* lock = nullptr;
	double level = 0.0;
	std::uint64_t building = 0;
	if (shackles)
	{
		if (!shacklesWorn(targetId))
			return LockStep::Done; // no shackles: flagTaskDone / nothing to cut
		lock = &lockEnsure(true, targetId);
		level = shackleLockLevel(targetId);
		if (lock->hardness < 0.0)
			lock->hardness = shackleHardness(targetId);
	} else {
		building = target->cageBuilding;
		if (building == 0)
			return LockStep::Done; // slot 6 yields no lock: flagTaskDone (dt24_picklock section 7.1)
		lock = &lockEnsure(false, building);
		if (lock->hardness < 0.0)
			lock->hardness = cageLockHardness(building);
		level = cageLockLevel(building);
	}
	if (lock->open || (!shackles && !cageDoorLocked(building)))
		return LockStep::Done;
	// The tools the cut needs (hasTools): ours is a carried TOOL item (UNSURE).
	if ((task == routines::task::CutLock || task == routines::task::CutShackles) && !carriesKit(id, kItemFunctionTool))
	{
		logLine("has no tools");
		return LockStep::Refused;
	}
	// The approach: the actor acts from beside the target (the exe's approach host steps toward it every tick).
	if (others && std::hypot(f->x - target->x, f->z - target->z) > lockReach(*target, true))
		return LockStep::Working;
	if (f->walking())
		f->clearRoute();

	// At the lock a chance below the floor never starts the pick (kenshi-k16.6.366): no crime, no roll; the order ends event 13
	// result 8 (too hard) and the log carries the key lock_too_hard.
	if ((task == routines::task::PickLock || task == routines::task::PickLockOnShackles)
		&& !sufficientLockSkill(level, f->skills.lockpicking, !shackles && f->cageBuilding != 0))
	{
		logLine("lock_too_hard");
		return LockStep::TooHard;
	}

	// The crime, every tick (dt24_picklock section 7.2): freeing someone else's slave is crime 7 against the captor, picking a building lock
	// someone else owns crime 2. reportCrime applies setCrime's refusals (a caged picker, its own faction, a faction not real).
	if (others)
	{
		if (shackles || !target->captorSid.empty())
			reportCrime(id, crime::kSlaveFreeing, target->captorSid, victim);
		else if (const BuildingSite* site = world_.buildingSite(building); site != nullptr && !site->ownerFaction.empty())
			{ reportCrime(id, crime::kBurglary, site->ownerFaction, 0); noteCaughtAct(id, 2, building); }
	}

	bool opened = false;
	switch (task)
	{
	case routines::task::PickLock:
	case routines::task::PickLockOnShackles:
	{
		// Task_PickLock (item28 b2 / item19 c): the lock's own timer, one roll per interval at pickChance(level, skill); a caged picker's
		// skill is lowered by 10 (cage locks only: a shackle wearer picks with its full skill, the penalty reads char+0x2f8 == 2 on the picker).
		const bool caged = f->cageBuilding != 0 && !shackles;
		const double skill = f->skills.lockpicking - (caged ? kCagePickSkillPenalty : 0.0);
		LockTimer timer{lock->elapsed, lock->interval, lock->attempts};
		const PickTick tick = tickLock(timer, kTickSeconds, pickChance(level, skill), shackles ? targetId : building, id);
		lock->elapsed = timer.elapsed;
		lock->interval = timer.interval;
		lock->attempts = timer.attempts;
		if (tick != PickTick::Waiting)
		{
			combat_->rules().addXp(f->skills.lockpicking, tick == PickTick::Unlocked ? 1.2 : 0.24);
			// EV 13 / 14 go to the prisoner (the target), never to a self-pick (dt24_picklock section 7.3).
			if (others)
				fireDialogue(victim, id, tick == PickTick::Unlocked ? kEventUnlockSuccess : kEventUnlockAttempt);
			logLine(std::string(shackles ? "shackle " : "cage ") + "pick " + (tick == PickTick::Unlocked ? "succeeded" : "failed"));
		}
		opened = tick == PickTick::Unlocked;
		break;
	}
	case routines::task::CutLock:
	case routines::task::CutShackles:
	{
		// Task_CutLock / Task_CutShackles tick 140359f80: the hardness drains at cutRate(strength) per game second; at 0 the lock is open and
		// the shackles item is removed from the wearer (dt24_shackles section 6).
		lock->hardness -= cutRate(strength) * kTickSeconds;
		if (lock->hardness <= 0.0)
		{
			lock->hardness = 0.0;
			opened = true;
			if (others)
				fireDialogue(victim, id, kEventUnlockSuccess);
			logLine(std::string(shackles ? "shackles" : "lock") + " cut through");
		}
		break;
	}
	default: // 286 and 186
	{
		// Task_BruteForce*: instant, strength against 3 x hardness; the shackles stay on (broken), the strength message otherwise.
		if (!bruteBreaks(strength, lock->hardness))
		{
			logLine("is not strong enough to do that");
			return LockStep::Refused;
		}
		lock->hardness = 0.0;
		opened = true;
		if (others)
			fireDialogue(victim, id, kEventUnlockSuccess);
		logLine(std::string(shackles ? "shackles" : "lock") + " broken by force");
		break;
	}
	}
	if (!opened)
		return LockStep::Working;
	lock->open = true;
	if (shackles && task == routines::task::CutShackles)
	{
		removeShackles(targetId); // the cut shackles are gone; picked or forced ones stay on, open (219 takes them off)
	} else if (shackles) {
		// Picked or forced shackles stay worn: the wearer's INVENTORY (and its ITEM_LOCK) is sent again with the lock open.
		flecs::entity wearer = fighterEntity(targetId);
		if (const state::Inventory* held = wearer.get<state::Inventory>())
		{
			state::Inventory inventory = *held;
			++inventory.version;
			wearer.set<state::Inventory>(std::move(inventory));
		}
	}
	return LockStep::Done;
}

// How close an actor stands to act on a character (a caged prisoner, a shackle wearer): the original's reach rule (interact.hpp characterReach, MQ13).
double Simulation::lockReach(const state::Fighter& target, bool stopped) const
{
	const RaceCombat* race = combat_->data().race(target.raceSid);
	return characterReach(race != nullptr ? race->footprintRadius : 2.6, target.dead || target.unconscious || target.carriedBy != 0, stopped);
}

Simulation::ShackleLockView Simulation::shackleLock(std::uint64_t wearer) const
{
	ShackleLockView view;
	const state::Fighter* f = combat_ != nullptr ? combat_->find(wearer) : nullptr;
	if (f == nullptr || !shacklesWorn(wearer))
		return view;
	const LockRecord* lock = lockFind(true, wearer);
	view.worn = true;
	view.locked = lock == nullptr || !lock->open;
	view.level = shackleLockLevel(wearer);
	view.maxHardness = shackleHardness(wearer);
	view.hardness = lock != nullptr && lock->hardness >= 0.0 ? lock->hardness : view.maxHardness;
	view.chance = pickChance(view.level, f->skills.lockpicking);
	view.canCut = carriesKit(wearer, kItemFunctionTool);
	view.canBreak = bruteBreaks(f->skills.strength, view.hardness);
	return view;
}

std::string Simulation::checkShackleAction(std::uint64_t actor, std::uint64_t wearer, std::uint8_t action) const
{
	const state::Fighter* f = combat_ != nullptr ? combat_->find(actor) : nullptr;
	const bool cage = action >= kPickCageAction;
	if (f == nullptr || action < kPickShacklesAction || action > kBreakCageAction
		|| (cage ? cageOccupant(actor, wearer) == 0 : combat_->find(wearer) == nullptr))
		return "bad_target";
	if (f->dead || f->unconscious || f->down || f->carriedBy != 0 || (cage ? !cageDoorLocked(wearer) : !shacklesLocked(wearer)))
		return "not_allowed";
	return {};
}

std::string Simulation::orderShackleAction(std::uint64_t actor, std::uint64_t wearer, std::uint8_t action)
{
	if (std::string reason = checkShackleAction(actor, wearer, action); !reason.empty())
		return reason;
	// A pick (15 / 18) whose chance is at the floor is unplannable, like a door pick (rwh.178 precondition 167): accepted, then
	// dropped with nothing changed (no walk, no event 13). The gate reads the raw stat; the roll's caged-picker penalty stays in stepLockTask.
	if (action == kPickShacklesAction || action == kPickCageAction)
	{
		const state::Fighter* picker = combat_->find(actor);
		const double level = action == kPickCageAction ? cageLockLevel(wearer) : shackleLockLevel(wearer);
		if (!sufficientLockSkill(level, picker->skills.lockpicking, false))
			return {};
	}
	// A replacing order, as order 60 is: the running interact, the queue and the follow task end.
	cancelOrders(actor);
	state::Fighter* f = combat_->find(actor);
	CombatArena::endFollow(*f);
	f->asleep = false;
	const bool cage = action >= kPickCageAction;
	const std::uint8_t shackleAction = cage ? static_cast<std::uint8_t>(action - kPickCageAction + kPickShacklesAction) : action;
	const std::uint16_t task = cage
		? (shackleAction == kPickShacklesAction ? routines::task::PickLock : shackleAction == kCutShacklesAction ? routines::task::CutLock : routines::task::BruteForceLock)
		: (shackleAction == kPickShacklesAction ? routines::task::PickLockOnShackles : shackleAction == kCutShacklesAction ? routines::task::CutShackles : routines::task::BruteForceShackles);
	orderLockTask(actor, task, cage ? cageOccupant(actor, wearer) : wearer);
	lockOrders_[actor].action = action;
	lockOrders_[actor].cage = cage ? wearer : 0;
	mirrorFighter(actor);
	return {};
}

std::uint64_t Simulation::cageOccupant(std::uint64_t actor, std::uint64_t cage) const
{
	if (combat_ == nullptr || cage == 0)
		return 0;
	if (const state::Fighter* own = combat_->find(actor); own != nullptr && own->cageBuilding == cage)
		return actor;
	std::uint64_t lowest = 0;
	for (const CombatArena::Member& member : combat_->members())
		if (member.fighter.cageBuilding == cage && (lowest == 0 || member.id < lowest))
			lowest = member.id;
	return lowest;
}

bool Simulation::cageLockOpened(std::uint64_t cage) const
{
	const LockRecord* lock = lockFind(false, cage);
	return lock != nullptr && lock->open;
}

bool Simulation::cageView(std::uint64_t cage, CageView& view) const
{
	const BuildingSite* site = world_.buildingSite(cage);
	if (site == nullptr)
		return false;
	const LockRecord* lock = lockFind(false, cage);
	view.cage = cage;
	view.record = site->buildingSid;
	view.locked = cageDoorLocked(cage);
	view.level = cageLockLevel(cage);
	view.maxHardness = cageLockHardness(cage);
	view.hardness = lock != nullptr && lock->hardness >= 0.0 ? lock->hardness : view.maxHardness;
	return true;
}

std::uint32_t Simulation::cageMenuEntries(std::uint64_t actor, std::uint64_t cage, float& chance) const
{
	const state::Fighter* f = combat_ != nullptr ? combat_->find(actor) : nullptr;
	CageView view;
	if (f == nullptr || cageOccupant(actor, cage) == 0 || !cageView(cage, view) || !view.locked)
		return 0;
	if (f->dead || f->unconscious || f->down || f->carriedBy != 0)
		return 0;
	// The same skill stepLockTask rolls with: a caged picker pays the penalty.
	chance = static_cast<float>(pickChance(view.level, f->skills.lockpicking - (f->cageBuilding != 0 ? kCagePickSkillPenalty : 0.0)));
	return kMenuPickCage | (carriesKit(actor, kItemFunctionTool) ? kMenuCutCage : 0) | (bruteBreaks(f->skills.strength, view.hardness) ? kMenuBreakCage : 0);
}

bool Simulation::orderLockTask(std::uint64_t id, std::uint16_t task, std::uint64_t victim)
{
	if (!routines::task::isLockTask(task) || combat_ == nullptr || combat_->find(id) == nullptr)
		return false;
	lockOrders_[id] = LockOrder{task, victim};
	return true;
}

}
