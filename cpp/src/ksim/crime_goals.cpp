// The crime goals of the generic path (task crime-rows; kinspect dt41_E): the town alarm manager's world side (the raise, the nearest-alarm
// selection 14092a810, the expiry 14092bfe0), the 209 / 210 investigator's dwell, 212 LOOT_STOLEN_GOODS (finder 1408cf6a0 and the
// confiscation), 211's finder (1408cf3c0) and 204 HUNT_BOUNTIES' finder. The scorer (routine_scorer.cpp) adopts a goal when its finder
// names something; stepCrimeGoals executes it. Docs: CRIME_STATIC.md "Memory/tags and alarm", AI_COVERAGE.md rows 204 / 209 - 212.

#include "ksim/simulation.hpp"

#include "ksim/clock.hpp"
#include "ksim/combat.hpp"
#include "ksim/inventory_grid.hpp"
#include "ksim/perception.hpp"
#include "ksim/sensor.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ksim {

namespace {

// OURS (UNSURE): how close the looter stands to the cage occupant to take its things (the exe's reach is the Task_Loot arm, not decoded).
constexpr double kConfiscateReach = 20.0;

// The prisoners of the cages in the town the searcher stands in (the exe's BuildingFinder_Cages, mask 0x100, "my town": a cage occupant
// whose position is in the same town circle). `visit(member)`.
template <typename Visit>
void townPrisoners(const CombatArena& arena, const state::Fighter& me, Visit&& visit)
{
	const perception::Environment& env = arena.environment();
	const int town = env.townOf(me.x, me.z);
	if (town < 0)
		return;
	for (const CombatArena::Member& member : arena.members())
		if (member.fighter.cageBuilding != 0 && !member.fighter.dead && env.townOf(member.fighter.x, member.fighter.z) == town)
			visit(member);
}

// 14079e550 for an inventory entry (dt41_E section 5), the exact = 0 form: the holder holds something whose owner is another faction.
// The stack's `stolen` is its victim FACTION ("" = clean, save v89). Not stolen when the victim is the holder's own faction or a player
// faction (the exe's `faction+0x1d0`); the shop-faction test (`faction+0x34 == 8`) is not modelled (UNSURE: ours has no shop faction flag).
bool stolenGood(const state::Inventory::Entry& entry, const std::string& holderSide, const std::vector<std::string>& playerSides)
{
	return entry.count > 0 && !entry.stolen.empty() && entry.stolen != holderSide
		&& std::find(playerSides.begin(), playerSides.end(), entry.stolen) == playerSides.end();
}

} // namespace

const alarm::Alarm* Simulation::raiseAlarm(std::uint8_t kind, double x, double z, std::uint64_t raiser, std::uint64_t criminal)
{
	if (combat_ == nullptr)
		return nullptr;
	const int town = combat_->environment().townOf(x, z);
	if (town < 0)
		return nullptr; // the writer runs on the town's manager: no town, no alarm
	const state::Fighter* who = combat_->find(raiser);
	const std::uint64_t before = alarms_.last();
	const alarm::Alarm& held = alarms_.raise(town, kind, x, z, who != nullptr ? who->building : 0, who != nullptr ? who->faction : 0, raiser);
	if (crimeObserver_ && alarms_.last() != before) // a new alarm on the board; a re-raise of a live one is silent
	{
		CrimeEvent raised;
		raised.kind = CrimeEvent::Kind::AlarmRaised;
		raised.who = raiser;
		raised.other = criminal;
		raised.cause = kind;
		if (const state::Allegiance* side = fighterEntity(raiser).get<state::Allegiance>())
			raised.faction = side->factionSid;
		raised.x = x;
		raised.z = z;
		crimeObserver_(raised);
	}
	return &held;
}

const alarm::Alarm* Simulation::pickAlarm(std::uint64_t id, bool alliesOnly) const
{
	const state::Fighter* me = combat_ != nullptr && !alarms_.empty() ? combat_->find(id) : nullptr;
	if (me == nullptr)
		return nullptr;
	const int town = combat_->environment().townOf(me->x, me->z);
	if (town < 0)
		return nullptr; // registry precondition atATown = 1
	const sensor::Context ctx = sensorContext();
	// A record's building: a player building that is broken down is unusable; any other placed building counts as usable (UNSURE: the
	// exe's usable test, building+0xc2, is not decoded).
	const auto buildingState = [&](std::uint64_t building)
	{
		alarm::BuildingState state = alarm::BuildingState::Usable;
		world_.ecs().each([&](const state::Id& key, const state::PlayerBuilding& site)
		{
			if (key.value == building && (site.ruin || site.dismantled || !site.built))
				state = alarm::BuildingState::Unusable;
		});
		return state;
	};
	const auto accept = [&](const alarm::Alarm& held)
	{
		if (!alliesOnly)
			return true;
		const state::Fighter* raiser = combat_->find(held.raiser);
		return raiser != nullptr && (raiser->faction == me->faction || sensor::ally(ctx, *me, *raiser));
	};
	return alarms_.nearest(town, 0, alarm::kSearchRadius, me->x, me->z, 0, me->building, buildingState, accept);
}

// 1408cf3c0: a prisoner of my town's cages that has served, is no slave and no enemy of mine, and that no one is already freeing.
std::uint64_t Simulation::prisonerToFree211(std::uint64_t id) const
{
	const state::Fighter* me = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (me == nullptr)
		return 0;
	const sensor::Context ctx = sensorContext();
	std::uint64_t found = 0;
	townPrisoners(*combat_, *me, [&](const CombatArena::Member& member)
	{
		const state::Fighter& p = member.fighter;
		if (found != 0 || member.id == id || p.slaveState != 0 || p.unconscious || p.down || !prisonerServed(member.id) || sensor::enemy(ctx, *me, p))
			return;
		for (const auto& [actor, order] : lockOrders_) // the squad package's claim (140269190 / 1402690e0): someone is already on it
			if (order.task == routines::task::ReleasePrisoner && order.victim == member.id)
				return;
		found = member.id;
	});
	return found;
}

// 1408cf6a0: the occupant of a cage of my town that holds a stolen item (equipment, then the pack).
std::uint64_t Simulation::lootedPrisoner212(std::uint64_t id) const
{
	const state::Fighter* me = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (me == nullptr)
		return 0;
	std::vector<std::string> playerSides;
	for (const CombatArena::Member& member : combat_->members())
		if (member.fighter.playerControlled)
			if (const state::Allegiance* side = fighterEntity(member.id).get<state::Allegiance>())
				playerSides.push_back(side->factionSid);
	std::uint64_t found = 0;
	townPrisoners(*combat_, *me, [&](const CombatArena::Member& member)
	{
		const state::Inventory* held = member.id != id ? fighterEntity(member.id).get<state::Inventory>() : nullptr;
		const state::Allegiance* side = fighterEntity(member.id).get<state::Allegiance>();
		if (found != 0 || held == nullptr || member.fighter.dead)
			return;
		const std::string holder = side != nullptr ? side->factionSid : std::string{};
		for (const state::Inventory::Entry& entry : held->items)
			if (stolenGood(entry, holder, playerSides))
			{
				found = member.id; // vf+0x170 "may I take it" is not decoded: every stolen item may be taken (UNSURE)
				return;
			}
	});
	return found;
}

// 14099bfc0 as 204 uses it: a sensed non-ally, non-slave, not in a bed or cage, that passes one of the three admission tests - the group
// test 140853fd0 (UNSURE, M on what it reads: ours is "it holds a bounty with MY legal faction"), the platoon test 140678960(, 7) (bit 7 of
// the relation record's flags; its meaning is not decoded, so never true here) or isEnemy.
std::uint64_t Simulation::bountyTarget204(std::uint64_t id) const
{
	const state::Fighter* me = combat_ != nullptr ? combat_->find(id) : nullptr;
	const state::Allegiance* mine = me != nullptr ? fighterEntity(id).get<state::Allegiance>() : nullptr;
	if (me == nullptr || mine == nullptr)
		return 0;
	const std::string legal = crime::legalFaction(gameData_.get(), mine->factionSid);
	const sensor::Context ctx = sensorContext();
	std::uint64_t found = 0;
	double nearest = 0.0;
	sensor::sensed(*me, sensor::kHostile | sensor::kNeutral | sensor::kIncapacitated | sensor::kDowned,
		sensor::kAlly | sensor::kSlave | sensor::kInObject | sensor::kPlayerFaction, ctx,
		[&](std::uint64_t target) { return combat_->find(target); },
		[&](std::uint64_t target, const state::Fighter& body, std::uint32_t)
		{
			if (body.dead || body.carriedBy != 0 || body.cageBuilding != 0)
				return;
			const state::CrimeRecord* record = crimeRecord(target);
			const bool wanted = record != nullptr
				&& std::any_of(record->bounties.begin(), record->bounties.end(), [&](const state::Bounty& b) { return b.factionSid == legal && b.amount > 0; });
			if (!wanted && !sensor::enemy(ctx, *me, body))
				return;
			const double d = std::hypot(body.x - me->x, body.z - me->z);
			if (found == 0 || d < nearest)
			{
				found = target;
				nearest = d;
			}
		});
	return found;
}

// The 210 tick's ally notify: the responder force-notices each ally of its own within the arrival radius.
void Simulation::noticeAlliesNear(std::uint64_t id)
{
	state::Fighter* me = combat_->find(id);
	const sensor::Context ctx = sensorContext();
	for (const CombatArena::Member& member : combat_->members())
	{
		if (member.id != id && !member.fighter.dead && std::hypot(me->x - member.fighter.x, me->z - member.fighter.z) < alarm::kCloseEnough
			&& sensor::ally(ctx, *me, member.fighter))
			perception::notice(*me, member.id);
	}
}

void Simulation::stepCrimeGoals()
{
	if (combat_ == nullptr)
		return;
	alarms_.tick(kTickSeconds); // 14092bfe0: the town update's elapsed time
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		flecs::entity actor = ecsMutable().entity(combatEntities_[i]);
		const state::Routine* current = actor.get<state::Routine>();
		if (current == nullptr)
			continue;
		const std::uint16_t goal = current->goal;
		if (goal != routines::task::InvestigateAlarms && goal != routines::task::InvestigateAlarmsAlliesOnly
			&& goal != routines::task::LootStolenGoods && goal != routines::task::PoliceFreePrisoners && goal != routines::task::HuntBounties)
			continue;
		const std::uint64_t id = members[i].id;
		state::Routine r = *current;
		const state::Fighter& f = *combat_->find(id);
		// The goal ends: the scorer takes the member back at once (a resident's own re-score), nothing is held.
		const auto end = [&]()
		{
			alarmDwell_.erase(id);
			r.goal = 0;
			r.target = {};
			r.chainTarget = 0;
			r.nextScore = 0;
			actor.set<state::Routine>(std::move(r));
		};
		if (f.dead || f.unconscious || f.down)
		{
			end();
			continue;
		}
		if (goal == routines::task::InvestigateAlarms || goal == routines::task::InvestigateAlarmsAlliesOnly)
		{
			// Task_InvestigateAlarms (140354260): walk to a point 30 u off the alarm (the scorer's walk); within 100 u of it start the
			// 10 s wait (this+0x7c, counted down by the frame delta), then delete the alarm. The 210 tick's notify (1405cb0a0, per sensed
			// ally) force-notices the ally (perception::notice, the red / green arrow's record, red_arrow_rules.md section 5); its mode-4
			// temporary enemy is skipped for an ally (1406744a0). OURS (UNSURE, deferred: playtesting): "sensed" is read as every ally of
			// mine within the 100 u arrival radius, not only one already in my sight list: sight never lists a friendly sneaker.
			const alarm::Alarm* held = alarms_.find(r.chainTarget);
			if (held == nullptr)
			{
				end();
				continue;
			}
			if (std::hypot(f.x - held->x, f.z - held->z) >= alarm::kCloseEnough)
			{
				alarmDwell_.erase(id);
				continue;
			}
			if (goal == routines::task::InvestigateAlarmsAlliesOnly)
				noticeAlliesNear(id);
			double& left = alarmDwell_.try_emplace(id, alarm::kDwellSeconds).first->second;
			left -= kTickSeconds;
			if (left > 0.0)
				continue;
			std::ostringstream line;
			line << "investigate " << goal << " member " << id << " cleared alarm " << held->id << " kind " << static_cast<int>(held->kind);
			logTask(line.str());
			alarms_.erase(r.chainTarget); // 14092c300
			end();
		} else if (goal == routines::task::LootStolenGoods) {
			// The confiscation: the stolen items go from the occupant into my own inventory (dt41_E section 5), equipment first.
			state::Fighter* occupant = combat_->find(r.chainTarget);
			if (occupant == nullptr || occupant->dead || occupant->cageBuilding == 0)
			{
				end();
				continue;
			}
			if (std::hypot(f.x - occupant->x, f.z - occupant->z) > kConfiscateReach)
				continue; // the scorer's walk is still on
			confiscate212(id, r.chainTarget);
			end();
		} else if (goal == routines::task::PoliceFreePrisoners) {
			if (lockOrders_.count(id) == 0)
			{
				const state::Fighter* prisoner = combat_->find(r.chainTarget);
				if (prisoner == nullptr || prisoner->cageBuilding == 0)
				{
					end();
					continue;
				}
				orderLockTask(id, routines::task::ReleasePrisoner, r.chainTarget);
			}
		} else {
			// HUNT_BOUNTIES: the arrest chain (arrest.cpp) takes the target once it is down; the goal ends with the order.
			const state::Fighter* victim = combat_->find(r.chainTarget);
			if (victim == nullptr || victim->dead || victim->cageBuilding != 0)
			{
				end();
				continue;
			}
			if (arrestOrders_.count(id) == 0 && !orderArrest(id, r.chainTarget, goap::kLiftPersonSnatchingInTown))
				end();
		}
	}
}

std::uint32_t Simulation::confiscate212(std::uint64_t looter, std::uint64_t occupant)
{
	flecs::entity from = fighterEntity(occupant);
	flecs::entity to = fighterEntity(looter);
	const state::Inventory* held = from.get<state::Inventory>();
	if (held == nullptr || gameData_ == nullptr)
		return 0;
	std::vector<std::string> playerSides;
	for (const CombatArena::Member& member : combat_->members())
		if (member.fighter.playerControlled)
			if (const state::Allegiance* side = fighterEntity(member.id).get<state::Allegiance>())
				playerSides.push_back(side->factionSid);
	const state::Allegiance* side = from.get<state::Allegiance>();
	const std::string holder = side != nullptr ? side->factionSid : std::string{};
	const grid::Rules rules(*gameData_);
	state::Inventory source = *held;
	grid::settle(source, rules, occupant);
	state::Inventory mine;
	if (const state::Inventory* packed = to.get<state::Inventory>())
		mine = *packed;
	std::vector<state::Inventory::Entry> overflow = grid::settle(mine, rules, looter);
	std::uint32_t took = 0;
	// Equipment first (pass 1), then the pack (pass 2).
	for (const bool equipment : {true, false})
	{
		for (auto it = source.items.begin(); it != source.items.end();)
		{
			if ((it->slot != state::Inventory::kCarried) != equipment || !stolenGood(*it, holder, playerSides))
			{
				++it;
				continue;
			}
			state::Inventory::Entry piece = *it;
			piece.slot = state::Inventory::kCarried;
			piece.section = state::Inventory::Entry::kUnplaced;
			const std::uint32_t left = grid::autoplace(mine, rules, piece);
			if (equipment)
				unequipFromFighter(occupant, it->itemSid);
			took += it->count - left;
			if (left > 0)
			{
				it->count = left;
				it->slot = state::Inventory::kCarried;
				++it;
			} else {
				it = source.items.erase(it);
			}
		}
	}
	if (took == 0)
		return 0;
	++mine.version;
	++source.version;
	storeInventory(to, looter, std::move(mine), std::move(overflow));
	from.set<state::Inventory>(std::move(source));
	std::ostringstream line;
	line << "confiscate 212 member " << looter << " from prisoner " << occupant << ": " << took << " items";
	logTask(line.str());
	return took;
}

} // namespace ksim
