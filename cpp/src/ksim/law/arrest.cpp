// The arrest chain and the smash order (task ds/arrest): 203 ARREST_TARGET as one generic order the crime system, the dialogue actions
// and a player share (lift 240 / 213 -> 112 cage -> TARGET_IS_ARRESTED), the sentence the put-in starts, 211 POLICE_FREE_PRISONERS_WHEN_DONE
// and 272 SMASH_BUILDING. Decodes: docs/reference/CRIME_STATIC.md ("What guards do (arrest chain)", the put-in and release sections),
// item25_static (203), dt26_kidnapper_cage, item28_static b6 (put-in, 211, 110), dt40_tasks_289_290 (the bash).

#include "world/resident_data.hpp"
#include "ksim/core/simulation.hpp"

#include "ksim/world/buildings.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/law/crime.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/ai/sensor.hpp"
#include "ksim/world/towncircle.hpp"

#include "kdata/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace ksim {

namespace {

// OURS: how long a lift order other than 240 waits for its target to be liftable before it is dropped. The 240 arrest lift has no timer
// (kenshi-rwh.124 section 5: considerJobQueue reads no age; the order ends by its task, utility <= 0, clearOrders or the world).
constexpr double kArrestWaitSeconds = 120.0;
// OURS (UNSURE): how long a smash order tries to reach the building.
constexpr double kSmashReach = 40.0;
constexpr double kSmashGiveUpSeconds = 60.0;
constexpr std::uint64_t kReleaseIntervalTicks = 20;

flecs::entity playerBuildingEntity(const flecs::world& ecs, std::uint64_t id)
{
	flecs::entity found{};
	ecs.each([&](flecs::entity entity, const state::Id& i, const state::PlayerBuilding&)
	{
		if (i.value == id)
			found = entity;
	});
	return found;
}

}

bool Simulation::orderArrest(std::uint64_t guard, std::uint64_t victim, std::uint16_t lift)
{
	if (combat_ == nullptr || guard == victim || combat_->find(guard) == nullptr || combat_->find(victim) == nullptr)
		return false;
	ArrestOrder order;
	order.lift = lift;
	order.victim = victim;
	order.since = clockTick();
	arrestOrders_[guard] = order;
	// The order takes the member over: a resident's routine goal (a patrol walk, a post) is dropped here, and the scorer yields to the
	// order until it ends (routine_scorer.cpp), so the guard is not walked away from its target.
	const flecs::entity actor = fighterEntity(guard);
	if (const state::Routine* routine = actor.get<state::Routine>())
	{
		state::Routine r = *routine;
		r.goal = 0;
		r.target = {};
		r.chainElapsed = -1.0;
		r.plan.clear();
		r.step = 0;
		actor.set<state::Routine>(std::move(r));
		combat_->find(guard)->clearRoute();
		mirrorFighter(guard);
	}
	return true;
}

bool Simulation::orderSmash(std::uint64_t id, std::uint64_t building)
{
	if (combat_ == nullptr || combat_->find(id) == nullptr)
		return false;
	const flecs::entity site = playerBuildingEntity(world_.ecs(), building);
	if (!site.is_valid() || site.get<state::PlayerBuilding>()->dismantled)
		return false; // UNSURE: only a player building has the hit points we model; the world's own buildings are refused
	SmashOrder order;
	order.building = building;
	order.since = clockTick();
	smashOrders_[id] = order;
	return true;
}

bool Simulation::atHomeTown(std::uint64_t id) const
{
	const state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr || gameData_ == nullptr)
		return false;
	std::vector<towncircle::Circle> circles;
	std::vector<flecs::entity_t> towns;
	world().ecs().each([&](flecs::entity entity, const state::TownState& town)
	{
		circles.push_back(towncircle::of(*gameData_, town));
		towns.push_back(entity.id());
	});
	const int at = towncircle::at(circles, f->x, f->z);
	const flecs::entity squad = fighterEntity(id).target<state::MemberOf>();
	const state::SquadInstance* instance = squad.is_valid() ? squad.get<state::SquadInstance>() : nullptr;
	return at >= 0 && instance != nullptr && instance->homeTown.id != 0 && instance->homeTown.id == towns[static_cast<std::size_t>(at)];
}

bool Simulation::prisonerServed(std::uint64_t id) const
{
	const state::CrimeRecord* record = crimeRecord(id);
	if (record == nullptr || record->sentenceStart + record->sentenceHours == 0.0)
		return true; // FUN_1405c9f00: a never-sentenced prisoner counts as served at once
	return gameHoursIn(clockTick()) >= record->sentenceStart + record->sentenceHours;
}

void Simulation::imprison(std::uint64_t carrierId, std::uint64_t bodyId, const routines::SiteEntry& cage)
{
	state::Fighter* carrier = combat_->find(carrierId);
	if (carrier == nullptr)
		return;
	const double carrierY = carrier->y;
	const double carrierYaw = carrier->yaw;
	dropCarried(carrierId);
	if (state::Fighter* put = combat_->find(bodyId))
	{
		put->x = cage.occupantX();
		put->y = carrierY;
		put->z = cage.occupantZ();
		put->yaw = carrierYaw;
		put->velX = 0.0;
		put->velZ = 0.0;
		put->building = cage.site.object;
		put->cageBuilding = cage.site.object;
		lockErase(false, cage.site.object); // a prisoner put in auto-locks the cage (item28 b1)
		put->cageSlot = cage.site.slot;
		put->clearRoute();
		put->forwardSpeed = 0.0;
		put->lateralSpeed = 0.0;
		// The prisoner's squad leaves its captors' personal hostile lists (the carrier's squad): the arrest is over, and a law NPC that still lists
		// the prisoner as hostile never frees it (stepPrisonRelease, sensor::enemy). UNSURE (ours): the original's list handling is not decoded.
		const std::uint64_t prisonerSquad = put->squad;
		const std::uint64_t captorSquad = carrier->squad;
		std::vector<std::uint64_t> captors;
		for (const CombatArena::Member& m : combat_->members())
			if (m.id == carrierId || m.fighter.squad == captorSquad)
				captors.push_back(m.id);
		for (const std::uint64_t id : captors)
			if (state::Fighter* c = combat_->find(id))
			{
				auto& list = c->hostileSquads;
				list.erase(std::remove(list.begin(), list.end(), prisonerSquad), list.end());
			}
		mirrorFighter(bodyId);
		if (const state::Allegiance* mine = fighterEntity(carrierId).get<state::Allegiance>())
			noteNpcImprisoned(bodyId, mine->factionSid);
	}
	// A caged resident leaves routine mode: it keeps no Routine (and no claim) while it is in the cage.
	fighterEntity(bodyId).remove<state::Routine>();
	routineBook_.release(bodyId);
	routineBook_.release(carrierId);

	// The sentence (CRIME_STATIC "sentence at put-in"): the bounty the CAGE OWNER's legal faction holds on the prisoner, if any.
	// Another faction's bounty does not count.
	// UNSURE: a cage with no owner faction of its own belongs to the captor's.
	const routines::BuildingSites* building = residentData_ != nullptr ? residentData_->sites.find(cage.site.object) : nullptr;
	std::string cageOwner = building != nullptr ? building->ownerFaction : std::string();
	if (cageOwner.empty())
		if (const state::Allegiance* mine = fighterEntity(carrierId).get<state::Allegiance>())
			cageOwner = mine->factionSid;
	const std::string legal = crime::legalFaction(gameData_.get(), cageOwner);
	const state::CrimeRecord* held = legal.empty() ? nullptr : crimeRecord(bodyId);
	const std::int32_t amount = held != nullptr ? crime::bountyOf(*held, legal) : 0;
	if (amount > 0)
	{
		state::CrimeRecord copy = *held;
		crime::startSentence(copy, amount, gameHoursIn(clockTick()));
		fighterEntity(bodyId).set<state::CrimeRecord>(std::move(copy));
	}
	std::ostringstream line;
	line << "imprison member " << carrierId << " body " << bodyId << " cage " << cage.site.object << ":" << cage.site.slot
		<< " bounty " << amount << " (legal " << legal << ")";
	logTask(line.str());
}

void Simulation::stepArrestOrders(const std::function<bool(std::uint64_t, double, double)>& walk)
{
	for (auto it = arrestOrders_.begin(); it != arrestOrders_.end();)
	{
		const std::uint64_t id = it->first;
		ArrestOrder& order = it->second;
		state::Fighter* f = combat_->find(id);
		state::Fighter* victim = combat_->find(order.victim);
		const double waited = static_cast<double>(ticksSince(clockTick(), order.since)) * kTickSeconds;
		const bool guardDown = f == nullptr || f->dead || f->unconscious || f->down;
		if (!guardDown && victim != nullptr && order.lift == goap::kKidnapOrder)
		{
			// Task_Kidnapping's start (vf1 14034ffa0) fails for a target already down or dead, and the tick after a failed start (vf2 1403552d0)
			// runs once more: both force-notice the arrester from the target (rwh.76; aware 1). OURS (UNSURE): the start roll 140885600 for a
			// conscious target is not decoded, so a conscious target is not failed here; it waits to be downed as every arrest does.
			if (order.gaveUp)
			{
				perception::notice(*victim, id);
				it = arrestOrders_.erase(it);
				continue;
			}
			if (!order.started)
			{
				order.started = true;
				if (victim->unconscious || victim->dead)
				{
					perception::notice(*victim, id);
					order.gaveUp = true;
					++it;
					continue;
				}
			}
		}
		if (guardDown || victim == nullptr || victim->dead || victim->cageBuilding != 0)
		{
			if (f != nullptr && f->carrying == order.victim && !victim->cageBuilding)
				dropCarried(id);
			it = arrestOrders_.erase(it);
			continue;
		}
		flecs::entity actor = fighterEntity(id);
		bool lifted = false; // the snatch's lift starts in the same tick, ahead of the dropped carrier's own retry
		if (f->carrying != order.victim)
		{
			if (victim->carriedBy != 0 && victim->carriedBy != id)
			{
				if (order.lift == goap::kLiftPersonSnatching && !order.snatched)
				{
					// 213 snatches the person from whoever carries it, once: the carrier's own order lifts the dropped body again (its retry is on the
					// squad interval) and a second snatch every time it wins the lift would trade the body for ever (kenshi-k16.6.143). The body free after the drop,
					// the guard's own lift starts below in this tick. The snatch loser (carried again, order.snatched) ends at once: end 2 of
					// rwh.124 section 5 (the finder no longer resolves a carried body, utility <= 0); it used to wait kArrestWaitSeconds (k16.6.187).
					dropCarried(victim->carriedBy);
					order.snatched = true;
					lifted = true;
				} else if (order.lift == goap::kLiftPersonSnatching || order.lift == goap::kLiftPersonSnatchingInTown
					|| waited > kArrestWaitSeconds)
				{
					// 240 refuses a carried target (Task_LiftSomething::vf1 14034f990, rwh.104 2.4): the arrest started on a body the slaver already holds
					// ends at once (the slaver's chain goes on); only DA 30's 213 takes it off the carrier. The other lifts keep the wait (OURS).
					it = arrestOrders_.erase(it);
					continue;
				}
			}
			// 240's lift needs isTargetDown (rwh.124 4.1: KO'd, dead, in a bed / cage, ragdolled or crouched), refused at assignment while the
			// victim stands: the order stays queued and is retried, no attack step (rwh.125: the guard's own package re-downs it). The other lifts keep
			// the KO'd / dead test.
			const bool liftable = order.lift == goap::kLiftPersonSnatchingInTown ? isTargetDown(*victim) : victim->unconscious || victim->dead;
			if (!liftable || victim->carriedBy != 0)
			{
				if (order.lift != goap::kLiftPersonSnatchingInTown && waited > kArrestWaitSeconds)
				{
					it = arrestOrders_.erase(it);
					continue;
				}
			} else if (!actor.has<state::InteractTask>() && !actor.has<state::CarryStep>() && (lifted || clockTick() % kSquadIntervalTicks == 0)) {
				f->clearRoute();
				beginInteract(id, order.victim, static_cast<std::uint8_t>(InteractAction::Carry), 0, {}, true,
					order.lift == goap::kLiftPersonSnatchingInTown ? kInteractFlagArrestLift : 0);
			}
			++it;
			continue;
		}
		if (const state::CarryStep* step = actor.get<state::CarryStep>(); step != nullptr && step->phase != 1)
		{
			++it; // still lifting (or putting down)
			continue;
		}
		// Carrying: the nearest free cage the guard's faction may use.
		const routines::SiteEntry* cage = nullptr;
		if (residentData_ != nullptr)
		{
			const state::Allegiance* mine = actor.get<state::Allegiance>();
			double best = 0.0;
			for (const std::uint64_t key : residentData_->sites.keys())
			{
				const routines::BuildingSites* building = residentData_->sites.find(key);
				if (building != nullptr && !building->ownerFaction.empty()
					&& (mine == nullptr || (building->ownerFaction != mine->factionSid
						&& crime::legalFaction(gameData_.get(), building->ownerFaction) != crime::legalFaction(gameData_.get(), mine->factionSid))))
					continue; // a cage of the guard's faction, or one whose owner answers to the same legal faction
				for (const routines::SiteEntry* entry : residentData_->sites.sitesOf(key, routines::SiteKind::Cage))
				{
					bool taken = routineBook_.holder(entry->site) != nullptr;
					for (const CombatArena::Member& m : combat_->members())
						taken = taken || (m.fighter.cageBuilding == entry->site.object && m.fighter.cageSlot == entry->site.slot);
					const double d = std::hypot(entry->x - f->x, entry->z - f->z);
					if (!taken && (cage == nullptr || d < best))
					{
						cage = entry;
						best = d;
					}
				}
			}
		}
		if (clockTick() % 200 == 0)
		{
			std::ostringstream line;
			line << "arrest member " << id << " body " << order.victim << " cage " << (cage != nullptr ? cage->site.object : 0) << " at "
				<< (cage != nullptr ? std::hypot(f->x - cage->x, f->z - cage->z) : -1.0) << " u walking " << f->walking();
			logTask(line.str());
		}
		if (cage == nullptr)
		{
			if (waited > kArrestWaitSeconds)
			{
				dropCarried(id);
				it = arrestOrders_.erase(it);
			} else {
				++it; // no free cage this second: it waits (never a terminal state)
			}
			continue;
		}
		const double reach = gameData_ != nullptr ? functionalityReach(*gameData_, cage->record, routines::kFunctionCage, !f->walking()) : 4.0;
		if (std::hypot(f->x - cage->x, f->z - cage->z) > reach)
		{
			if (!f->walking() && clockTick() % kSquadIntervalTicks == 0)
				walk(id, cage->x, cage->z);
			++it;
			continue;
		}
		imprison(id, order.victim, *cage);
		it = arrestOrders_.erase(it);
	}
}

void Simulation::stepSmashOrders(const std::function<bool(std::uint64_t, double, double)>& walk)
{
	for (auto it = smashOrders_.begin(); it != smashOrders_.end();)
	{
		const std::uint64_t id = it->first;
		SmashOrder& order = it->second;
		state::Fighter* f = combat_->find(id);
		const flecs::entity site = playerBuildingEntity(world_.ecs(), order.building);
		if (f == nullptr || f->dead || f->unconscious || f->down || f->carriedBy != 0 || !site.is_valid()
			|| site.get<state::PlayerBuilding>()->dismantled)
		{
			it = smashOrders_.erase(it);
			continue;
		}
		state::PlayerBuilding* b = site.get_mut<state::PlayerBuilding>();
		if (std::hypot(f->x - b->x, f->z - b->z) > kSmashReach)
		{
			if (!f->walking())
			{
				if (static_cast<double>(ticksSince(clockTick(), order.since)) * kTickSeconds > kSmashGiveUpSeconds)
				{
					it = smashOrders_.erase(it);
					continue;
				}
				if (clockTick() % kSquadIntervalTicks == 0)
					walk(id, b->x, b->z);
			}
			++it;
			continue;
		}
		f->clearRoute();
		f->forwardSpeed = 0.0;
		f->lateralSpeed = 0.0;
		order.elapsed += kTickSeconds;
		if (order.elapsed < kBashInterval)
		{
			++it;
			continue;
		}
		order.elapsed = 0.0;
		// Task_BashDoor's blow: clamp(2 blunt + 0.5 cut, 20, 60) through the building's remove-HP (vf+0x1f0), at the construction rate.
		const double blow = bashDamage(f->weapon.blunt1, f->weapon.cut1) / 0.015;
		const kdata::Record* record = gameData_ != nullptr ? gameData_->find(b->recordSid) : nullptr;
		const double mult = record != nullptr ? record->floatOr("build speed mult", 1.0f) : 1.0;
		if (buildings::removeHp(*b, blow, mult / buildings::kRateDivisor))
		{
			// UNSURE: a smashed building falls to a ruin the builders can revive, not the refund of a dismantle.
			b->dismantled = false;
			b->ruin = true;
			b->hp = 0.0;
			++b->version;
			it = smashOrders_.erase(it);
			continue;
		}
		++b->version;
		++it;
	}
}

void Simulation::stepPrisonRelease()
{
	// 211 POLICE_FREE_PRISONERS_WHEN_DONE (only in the "addon- law enforcement" package): a law-enforcement NPC frees a served, conscious,
	// non-slave prisoner of its town's cages that it does not treat as an enemy, by task 110 (lock_tasks.cpp: erases the bounty, event
	// 0x2e). The finder is prisonerToFree211 (1408cf3c0, crime_goals.cpp). A member with a Routine runs this as the goal (the scorer
	// adopts it, stepCrimeGoals orders the release); this pass is for a law NPC with none (a scenario's hand-built guard).
	if (clockTick() % kReleaseIntervalTicks != 0 || combat_ == nullptr)
		return;
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const state::Fighter& npc = members[i].fighter;
		if (npc.npcClass != crime::kClassLaw || npc.playerControlled || npc.dead || npc.unconscious || npc.down || npc.carriedBy != 0
			|| npc.cageBuilding != 0 || lockOrders_.count(members[i].id) != 0 || arrestOrders_.count(members[i].id) != 0
			|| ecsMutable().entity(combatEntities_[i]).has<state::Routine>())
			continue;
		if (const std::uint64_t prisoner = prisonerToFree211(members[i].id); prisoner != 0)
			orderLockTask(members[i].id, routines::task::ReleasePrisoner, prisoner);
	}
}

} // namespace ksim
