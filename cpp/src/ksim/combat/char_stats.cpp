// The character panels' numbers (SIMLINK.md "CHAR_STATS and the inspect focus"; kinspect findings/inspect_focus.md).

#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"

#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/carry.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/law/crime.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/ai/task_names.hpp"
#include "ksim/economy/trader.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <mutex>
#include <set>

namespace ksim {

namespace {

// The Goal row (task AT, ROADMAP 6b; SIMLINK.md "Goal names"): the goal the character is running now, and only with no goal at all the
// squad template's behaviour. The original shows the current GOAL's TaskData name -- never the primitive task (kinspect inspect_goal.md:
// a Bonedog on goal 230 with task 220 read "Eat corpse") -- and "Aimless" when it has none.
//
// AI_TASK enums the sim runs but routines::task does not list (the order, individual and group-layer tasks, ORIGINAL_AI.md 5).
constexpr std::uint16_t kMoveOrdered = 29; // MOVE_CUS_ORDERED (a player's move order; holds under attack, rwh.172)
constexpr std::uint16_t kAttackOrder = 5; // n_ATTACK (a player's attack order)
constexpr std::uint16_t kRaidTown = 18; // RAID_TOWN (the group layer's Raid)
constexpr std::uint16_t kGoHomeBuilding = 19;
constexpr std::uint16_t kWanderer = 24; // WANDERER (the group layer's Roam)
constexpr std::uint16_t kTravelToTargetTown = 53;
constexpr std::uint16_t kGetUp = 65;
constexpr std::uint16_t kFollowSlaveMaster = 171;
constexpr std::uint16_t kGoToDeliverSlaves = 192;
constexpr std::uint16_t kMoveOnFreeWillFast = 67; // the Summoned individual goal
constexpr std::uint16_t kLiftPerson = 68;
constexpr std::uint16_t kCarryWoundedSlaves = 168;
constexpr std::uint16_t kManTheGate = 180;
constexpr std::uint16_t kRunAwayForced = 280;
constexpr std::uint16_t kRunAway = 35;
constexpr std::uint16_t kAttackEnemies16 = 16;

// The AI tail (spectator mode, SIMLINK.md CHAR_STATS rows 16-20): no task, and where the running goal came from.
constexpr std::uint16_t kNoTask = 0xFFFF;
constexpr std::uint8_t kSourceNone = 0;
constexpr std::uint8_t kSourceRoutine = 3; // the per-character layer (the routine slot; a dialogue's individual goal too)
constexpr std::uint8_t kSourceGroup = 4;
constexpr std::uint8_t kSourceCombat = 5;
constexpr std::uint8_t kSourceCarry = 6;
constexpr std::uint8_t kSourceFlee = 7;

// The goal's display name: the exe's string (task_names.cpp), or empty = Aimless. An id the exe has no (decoded) name for is never labelled
// with the data record's own name (that reads "Patrol hometown" where the UI reads "Patrolling town"); it is logged once per id.
std::string taskName(std::uint16_t task)
{
	if (task == 0)
		return {};
	if (const char* name = exeTaskName(task))
		return name;
	static std::mutex mutex;
	static std::set<std::uint16_t> reported;
	const std::lock_guard lock(mutex);
	if (reported.insert(task).second)
		std::cerr << "char_stats: AI_TASK " << task << " has no exe display name; the Goal row reads Aimless\n";
	return {};
}

// The group layer's goal as the AI_TASK that stands for it (0 = nothing more specific than the behaviour).
std::uint16_t groupTask(state::GoalKind goal) noexcept
{
	switch (goal)
	{
	case state::GoalKind::GoHome:
		return kGoHomeBuilding;
	case state::GoalKind::TravelTo:
		return kTravelToTargetTown;
	case state::GoalKind::Patrol:
		return routines::task::PatrolTown;
	case state::GoalKind::Roam:
		return kWanderer;
	case state::GoalKind::Raid:
		return kRaidTown;
	case state::GoalKind::ManGate:
		return kManTheGate;
	case state::GoalKind::None:
	case state::GoalKind::Idle:
		return 0;
	}
	return 0;
}

// The running goal, in the original's order (task AT; the original shows one goal, and a fight shows the goal that started it):
// the fight, fleeing, carrying, the routine slot (sleeping, drinking, working), the individual goal, the group goal. Empty = no goal
// (the caller then falls back to the squad's behaviour). The same walk fills the AI tail (spectator mode, rows 17-19): where the goal
// came from and the id / point it is about, so the enum and the display name can never disagree.
struct LiveGoal
{
	std::string name;
	std::uint16_t task = kNoTask; // the AI_TASK enum behind `name`
	std::uint8_t source = kSourceNone;
	std::uint64_t target = 0; // a character, else a site object
	bool hasPoint = false;
	double x = 0.0;
	double z = 0.0;
};

LiveGoal liveGoal(const state::Fighter& f, const flecs::entity& entity, const CombatArena* combat)
{
	LiveGoal out;
	const auto at = [combat, &out](std::uint64_t id)
	{
		const state::Fighter* other = combat != nullptr && id != 0 ? combat->find(id) : nullptr;
		if (other != nullptr)
		{
			out.hasPoint = true;
			out.x = other->x;
			out.z = other->z;
		}
	};
	// A KO'd or dead character has no goal: the original clears it, so the row reads "Aimless" (inspect_goal.md). Down but conscious
	// keeps its goal.
	if (f.dead)
		return out;
	// A carried character is disabled, so its goal is Aimless, slave or not (live, rwh94 run D; the user 2026-11-14 on kenshi-8x6: "A carried
	// person/creature is disabled, so their goal (and AI goal) is always "Aimless"", kenshi-k16.6.204).
	if (f.carriedBy != 0)
		return out;
	// An enslaved character (kenshi-k16.6.116, rwh.99 section 4; the user 2026-11-13: "the goal shouldn't be 'aimless'"): the Goal row is the committed goal's
	// name, never the 187 Obedient Slave row (a do-nothing order is not a goal). Following the master is the committed goal 171 FOLLOW_SLAVEMASTER. In the camp
	// it is the master-package goal that commits (the routine slot below), else Aimless.
	if (f.slaveState == 1 && f.followTarget != 0)
	{
		out.task = kFollowSlaveMaster;
		out.source = kSourceRoutine;
		out.target = f.followTarget;
		at(out.target);
		out.name = taskName(out.task);
		return out;
	}
	if (f.unconscious)
		return out;
	// A player's move order is the goal for as long as the walk runs, under attack and through a stagger too (COMBAT_MOVE_LIVE.md 5: goal 29 / task
	// 29 from the order to the arrival; the fight label came first and, with the target dropped, the row read Aimless).
	if (f.playerControlled && f.walking() && !f.driving() && f.routeSource == state::Fighter::PlanSource::Order)
	{
		out.task = kMoveOrdered;
		out.source = kSourceRoutine;
		out.hasPoint = true;
		out.x = f.route.back().x;
		out.z = f.route.back().z;
		out.name = taskName(out.task);
		return out;
	}
	// A fight: the wire's combat flag condition (SIMLINK.md section 6). The goal is the one that started it (Fighter::combatGoal).
	if (f.target != 0 || (f.technique >= 0 && !f.techniqueFinished) || f.state == state::CombatState::Attack || f.state == state::CombatState::Defend)
	{
		out.task = f.combatGoal != 0 ? f.combatGoal : (f.playerControlled ? kAttackOrder : kAttackEnemies16);
		out.source = kSourceCombat;
		out.target = f.target;
		at(f.target);
		out.name = taskName(out.task);
		return out;
	}
	if (f.goal.kind == state::IndividualKind::Flee || f.goal.kind == state::IndividualKind::RunAway)
	{
		out.task = f.goal.kind == state::IndividualKind::RunAway ? kRunAway : kRunAwayForced;
		out.source = kSourceFlee;
		out.target = f.goal.target;
		at(f.goal.target);
		out.name = taskName(out.task);
		return out;
	}
	if (f.carrying != 0)
	{
		// A slave chain carries its person on its own goal (CAPTURE_NEW_SLAVES / CARRY_WOUNDED_SLAVES / the intruder take-out); anyone
		// else carries under LIFT_PERSON, the goal the original names "Lifting".
		const state::Routine* routine = entity.get<state::Routine>();
		out.task = routine != nullptr && routine->goal != 0 ? routine->goal : kLiftPerson;
		out.source = kSourceCarry;
		out.target = f.carrying;
		at(f.carrying);
		out.name = taskName(out.task);
		return out;
	}
	if (const state::Routine* routine = entity.get<state::Routine>(); routine != nullptr && routine->goal != 0)
	{
		out.task = routine->goal;
		out.source = kSourceRoutine;
		out.target = routine->chainTarget != 0 ? routine->chainTarget : routine->target.object;
		if (routine->chainTarget != 0)
			at(routine->chainTarget);
		out.name = taskName(out.task);
		return out;
	}
	switch (f.goal.kind)
	{
	case state::IndividualKind::GoHome:
		out.task = kGoHomeBuilding;
		out.source = kSourceRoutine; // the per-character layer; SIMLINK.md row 17 has no separate slot for a dialogue's individual goal
		out.hasPoint = true;
		out.x = f.goal.x;
		out.z = f.goal.z;
		break;
	case state::IndividualKind::Summoned:
		out.task = kMoveOnFreeWillFast;
		out.source = kSourceRoutine;
		out.target = f.goal.target;
		at(f.goal.target);
		break;
	default:
		break;
	}
	if (out.task == kNoTask)
	{
		const flecs::entity squad = entity.target<state::MemberOf>();
		if (const state::GroupAI* group = squad.is_valid() ? squad.get<state::GroupAI>() : nullptr; group != nullptr)
		{
			out.task = groupTask(group->goal);
			out.source = kSourceGroup;
			// Patrol and Roam are the group goals that keep a point in the component; the town ones walk a route.
			if (group->goal == state::GoalKind::Patrol || group->goal == state::GoalKind::Roam)
			{
				out.hasPoint = true;
				out.x = group->goalX;
				out.z = group->goalZ;
			}
		}
	}
	out.name = taskName(out.task);
	return out;
}

// The squad behaviour, the last resort (SIMLINK.md "Goal names"): the original's AI task name for the behaviour the template runs.
const char* goalLabel(Behaviour behaviour)
{
	switch (behaviour)
	{
	case Behaviour::ShopKeeper:
		return "Shopkeeper";
	case Behaviour::BarHangout:
		return "Relaxing";
	case Behaviour::PatrolLocal:
		return "Patrolling";
	case Behaviour::ManTheGate:
	case Behaviour::GuardShift:
	case Behaviour::CageWarden:
		return "Guarding";
	case Behaviour::TradeCircuit:
		return "Trading";
	case Behaviour::Travel:
		return "Travelling";
	case Behaviour::GoHome:
		return "Going home";
	case Behaviour::Raid:
		return "Raiding";
	case Behaviour::AnimalTerritory:
		return "Wandering";
	case Behaviour::ResidentLife:
		return "Living"; // UNSURE: the resident's own town-life goal is not named yet
	case Behaviour::StayHome:
	case Behaviour::Fallback:
		return "";
	}
	return "";
}

} // namespace

Simulation::CharStats Simulation::charStats(std::uint64_t id) const
{
	CharStats stats;
	const state::Fighter* found = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (found == nullptr)
		return stats;
	const state::Fighter& f = *found;
	const CombatConstants& k = combat_->data().k;

	// State: first match wins (inspect_focus.md "State"). 5 / 6 = in a cage, its door locked / unlocked (kenshi-k16.6.36; the lock level follows).
	// 2 = in its bed (Fighter::asleep).
	if (f.dead)
		stats.state = 4;
	else if (f.cageBuilding != 0)
		stats.state = cageDoorLocked(f.cageBuilding) ? 5 : 6;
	else if (f.unconscious)
		stats.state = 3;
	else if (f.asleep)
		stats.state = 2;
	else if (f.blockMode)
		stats.state = 1;
	if (stats.state == 5 || stats.state == 6)
		stats.lockLevel = static_cast<std::uint8_t>(std::clamp(cageLockLevel(f.cageBuilding), 0.0, 255.0));

	const flecs::entity entity = fighterEntity(id);
	// The live goal first (task AT); the squad template's behaviour only when the character has no goal. A KO'd or dead character has
	// none: the original clears the goal, so the row reads "Aimless" (inspect_goal.md; a dead Dust Bandit showed "Patrolling" before,
	// user 2026-11-06). Down but conscious keeps its goal.
	LiveGoal live = liveGoal(f, entity, combat_.get());
	std::uint16_t stepOverride = kNoTask;
	// The capture's aftermath (rwh.89 sections 3-6): the new slave comes round under the first aid on 65 GET_UP, then walks on 171
	// FOLLOW_SLAVEMASTER (carried, it keeps the carrier's goal); the delivering squad's leader runs 192 GO_TO_SOMEWHERE_FOR_DELIVERING_SLAVES
	// with the depot town as its target (the original's nav target is the slave camp's own point), its task 53.
	if (f.slaveState == 1 && !f.dead && !f.unconscious && f.carriedBy == 0 && live.task == kNoTask)
	{
		if (aided(id))
			live.task = kGetUp;
		else if (f.followTarget != 0)
			live.task = kFollowSlaveMaster;
		if (live.task != kNoTask)
		{
			live.source = kSourceRoutine;
			live.target = f.followTarget;
			live.name = taskName(live.task);
		}
	}
	if (const auto board = f.squad != 0 && !f.dead && !f.unconscious ? boards_.find(f.squad) : boards_.end();
		board != boards_.end() && board->second.depot != 0 && !board->second.owned.empty() && live.task == kTravelToTargetTown)
	{
		std::uint64_t leader = 0;
		for (const CombatArena::Member& other : combat_->members())
		{
			if (other.fighter.squad == f.squad && !other.fighter.dead && !other.fighter.unconscious && !other.fighter.playerControlled && (leader == 0 || other.id < leader))
				leader = other.id;
		}
		if (leader == id)
		{
			world_.ecs().each([&](const state::Id& townId, const state::TownState& town)
			{
				if (townId.value != board->second.depot)
					return;
				live.task = kGoToDeliverSlaves;
				live.name = taskName(live.task);
				live.hasPoint = true;
				live.x = town.x;
				live.z = town.z;
				stepOverride = kTravelToTargetTown;
			});
		}
	}
	stats.goal = live.name;
	stats.goalEnum = live.task;
	stats.source = live.source;
	stats.targetId = live.target;
	stats.hasTargetPoint = live.hasPoint;
	stats.targetX = static_cast<float>(live.x);
	stats.targetZ = static_cast<float>(live.z);
	// The G4 plan step the routine is running (row 18): empty for everything that plans per tick (a fight) or has no plan.
	if (const state::Routine* routine = entity.get<state::Routine>(); routine != nullptr && !routine->plan.empty())
	{
		stats.stepCount = static_cast<std::uint8_t>(std::min<std::size_t>(routine->plan.size(), 0xFF));
		stats.stepIndex = static_cast<std::uint8_t>(std::min<std::size_t>(routine->step, stats.stepCount - 1));
		stats.stepEnum = routine->plan[stats.stepIndex];
	}
	if (stepOverride != kNoTask && stats.stepEnum == 0xFFFF)
	{
		stats.stepEnum = stepOverride;
		stats.stepCount = 1;
		stats.stepIndex = 0;
	}
	if (stats.goal.empty() && !f.dead && !f.unconscious && !f.playerControlled && ai_ != nullptr)
	{
		const flecs::entity squad = entity.target<state::MemberOf>();
		const state::SquadInstance* instance = squad.is_valid() ? squad.get<state::SquadInstance>() : nullptr;
		if (instance != nullptr)
			stats.goal = goalLabel(ai_->squadMapping(world_.ecs(), *instance).behaviour);
	}

	// Encumbrance: the pack against carry.hpp's capacity, percent. UNSURE: the exe's percent may be the ratio's complement.
	const double strength = f.skills.strength * CombatRules::effectiveStat(f, CombatRules::kStatStrength);
	const double capacity = carryCapacity(strength, k.carryWeightMult, k.encumbranceBase);
	stats.encumbrance = capacity > 0.0 ? static_cast<float>(100.0 * packWeight(id) / capacity) : 0.0f;
	stats.carryKg = static_cast<float>(packWeight(id)); // without the fixed `carry person weight` (row 5's W minus it)
	stats.carryMaxKg = static_cast<float>(capacity);
	stats.damageResist = static_cast<float>(1.0 - combat_->rules().toughnessFactor(f)); // lerp(min, max, toughness / 100), F8 getDamageResistance

	const double speed = std::hypot(f.velX, f.velZ);
	if (f.sneak)
	{
		stats.extraKind = 1;
		stats.extra = static_cast<float>(f.skills.stealth);
	} else if (speed > 9.0) {
		stats.extraKind = 2;
		stats.extra = static_cast<float>(f.skills.athletics);
	}
	stats.weaponClass = f.weapon.weaponType == 5 ? 0 : 1; // no ranged combat: never 2

	// Skill ids (SIMLINK.md "Skill ids"): fixed, never the struct order. UNSURE: `bonus` is 0, the gear part is not split out yet.
	const double skills[] = {f.skills.strength, f.skills.dexterity, f.skills.toughness, f.skills.athletics, f.skills.dodge, f.skills.attack,
		f.skills.defence, f.skills.katana, f.skills.sabres, f.skills.hackers, f.skills.blunt, f.skills.heavy, f.skills.unarmed, f.skills.poles,
		f.skills.medic, f.skills.lockpicking, f.skills.stealth, f.skills.science, f.skills.labouring};
	for (std::size_t i = 0; i < std::size(skills); ++i)
		stats.skills.push_back({static_cast<std::uint8_t>(i), static_cast<float>(skills[i]), 0.0f});
	// Ids 19-23 are FighterSkills; 24-32 nothing trains or uses yet, so they are the template STATS record's numbers at send time (no
	// save change; STATS keys from docs/reference/combat/data/keys_STATS.tsv: `ff` is the precision shooting skill, `bow smith` the
	// crossbow smith). A character with no STATS record shows the data default 1.
	const double extended[] = {f.skills.engineer, f.skills.crossbows, f.skills.turrets, f.skills.thievery, f.skills.assassination};
	for (std::size_t i = 0; i < std::size(extended); ++i)
		stats.skills.push_back({static_cast<std::uint8_t>(19 + i), static_cast<float>(extended[i]), 0.0f});
	const kdata::Record* statsRecord = nullptr;
	if (const state::CharacterInstance* character = entity.is_valid() ? entity.get<state::CharacterInstance>() : nullptr;
		character != nullptr && gameData_ != nullptr)
	{
		const kdata::Record* record = gameData_->find(character->templateSid);
		const kdata::RefCategory* statsRef = record != nullptr ? record->findRefs("stats") : nullptr;
		if (statsRef != nullptr && !statsRef->entries.empty())
			statsRecord = gameData_->find(statsRef->entries.front().targetID);
	}
	static constexpr const char* kTemplateStats[] = {"perception", "swimming", "robotics", "weapon smith", "armour smith", "bow smith", "farming",
		"cooking", "ff"};
	for (std::size_t i = 0; i < std::size(kTemplateStats); ++i)
		stats.skills.push_back({static_cast<std::uint8_t>(24 + i), statsRecord != nullptr ? statsRecord->floatOr(kTemplateStats[i], 1.0f) : 1.0f, 0.0f});
	// The Next Level bar: the fractional part of the value. UNSURE: the exe's bar formula (deferred: playtesting); the sim trains nothing, so
	// it is static.
	for (const Simulation::CharStats::Skill& skill : stats.skills)
		stats.progress.push_back({skill.id, skill.value - std::floor(skill.value)});

	// Extended rows. UNSURE: no skill training yet, so the XP rates are the static constants (athletics has none: 1).
	stats.athleticsXp = 1.0f;
	stats.strengthXp = static_cast<float>(k.xpRateStrength);
	stats.toughnessXp = static_cast<float>(k.xpRateToughness);
	// The KO point row: -lerp(toughness / 100, min, max) as the medical check uses it (combat_rules.cpp), whatever the KO gate setting.
	const double toughness = std::clamp(f.skills.toughness * 0.01, 0.0, 1.0);
	stats.koPoint = static_cast<float>(-(k.minToughnessKoPoint + (k.maxToughnessKoPoint - k.minToughnessKoPoint) * toughness));
	stats.runSpeed = static_cast<float>(combat_->runSpeed(f));
	stats.runPenalty = static_cast<float>(combat_->runSpeed(f, false)) - stats.runSpeed;
	const RaceCombat* race = combat_->data().race(f.raceSid);
	stats.hungerRate = static_cast<float>(race != nullptr ? race->hungerRate : 1.0); // UNSURE: no bed / encumbrance mults

	// The tag's light (sneak_icons.md "Light level"): daylight > 0.5 -> 1, else 2 x daylight; the sim has no lamps.
	const double day = perception::daylight(combat_->environment().dayFraction);
	stats.light = static_cast<float>(day > 0.5 ? 1.0 : std::clamp(2.0 * day, 0.0, 1.0));

	const double nowHours = gameHoursIn(clockTick());
	if (const state::CrimeRecord* crime = entity.get<state::CrimeRecord>())
	{
		// Row 14 keeps a claimed bounty (the tag row's `claim`), so a row-32 entry rides on each row-14 one.
		for (const state::Bounty& bounty : crime->bounties)
		{
			if (bounty.amount <= 0)
				continue;
			const double expiry = crime::expiryHours(bounty.amount);
			stats.bounties.push_back({bounty.factionSid, bounty.amount});
			stats.bountyTags.push_back({bounty.crimes, static_cast<std::uint8_t>(bounty.claim),
				expiry < 0.0 ? -1.0f : static_cast<float>(std::max(0.0, bounty.stamp + expiry - nowHours))});
		}
		if (crime->sentenceHours > 0.0)
			stats.sentenceHours = static_cast<float>(std::max(0.0, crime->sentenceStart + crime->sentenceHours - trader::gameHours(clockTick())));
		stats.crime = crime->crime;
		stats.crimeTimer = static_cast<float>(crime->timer);
	}

	// The HUD tag row tail (rows 24-32, kenshi-k16.6.215). b1 army commander, b3 diplomat, b5 escaped prisoner and the slave look stay unmodelled
	// (k16.6.211 / .212 / .213 / .214): b4 slaveSeen is meaningful only with slaveLook != 255, so it stays 0 with it.
	const bool leader = [&]
	{
		if (entity.target<state::MemberOf>().is_valid())
			return liveRole(entity) == routines::Role::Leader;
		bool first = false;
		if (f.playerControlled) // a player squad holds its members in a list, leader first
			world().ecs().each([&](const state::PlayerSquad& squad) { first = first || (!squad.members.empty() && squad.members.front() == id); });
		return first;
	}();
	stats.tagFlags = static_cast<std::uint16_t>((leader ? 1 : 0) | (f.captive ? 4 : 0) | (f.obedient ? 64 : 0));
	stats.slaveState = f.slaveState;
	stats.captor = f.captorSid;
	if ((f.slaveState == 2 || f.slaveState == 3) && f.slaveStamp >= 0.0)
		stats.slaveHoursLeft = static_cast<float>(std::max(0.0, f.slaveStamp + (f.slaveState == 2 ? kEscapedSlaveGameHours : kExSlaveGameHours) - nowHours));
	if (const state::TownState* town = townStateAt(f.x, f.z); town != nullptr && town->owner.id != 0)
		if (const state::FactionState* owner = flecs::entity(world_.ecs().c_ptr(), town->owner.id).get<state::FactionState>())
			stats.localFaction = owner->templateSid;
	return stats;
}

} // namespace ksim
