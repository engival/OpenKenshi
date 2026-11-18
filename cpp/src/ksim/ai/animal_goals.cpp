// The animal and food goals of the generic path (task port-animals; kinspect dt24_eat, dt24_plantasks, dt24_131, dt24_cage_loot_play): 259
// EAT_FOOD_ON_GROUND, 277 ANIMAL_FETCH_A_LIMB, 278 PLAY_BECAUSE_I_HAVE_A_LIMB_IN_MOUTH, 279 CHASE_ALLY_DOGS_WITH_MOUTH_LIMBS and 238
// LOOT_ANIMALS_JOB's order side. None is a package goal in the data ("code-only: player order / dialogue / sub-goal"), so
// orderAnimalGoal runs the finder (world_resolvers.hpp, the exe's own) and puts the goal in the member's Routine; stepAnimalGoals executes it.
// 231 / 232 are crops.cpp's, 261 is 175's floor sleep and 238's executor is 241's (world_tasks.cpp). Docs: AI_COVERAGE.md rows 231 - 279.

#include "ksim/core/simulation.hpp"

#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/economy/inventory_grid.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/ai/sensor.hpp"

#include "world/world_resolvers.hpp"

#include <cmath>
#include <numbers>
#include <set>
#include <sstream>

namespace ksim {

namespace {

constexpr double kGiveUpSeconds = 30.0; // world_tasks.cpp's: a walk that never arrives drops the goal
constexpr double kHungerGate = 0.9 * 3.0; // 1409993a0: vf+0x398 against 0.9, as a share of the fed hunger (3.0). UNSURE: the stat's direction
constexpr double kLimbCooldownSeconds = 30.0; // AI+0x318's cooldown after a fetch. OURS: the exe's span is unread
constexpr double kChaseNear = 50.0; // 279's goal isNearTo = isWithin50Meters (combat.cpp's own reading)
// Task_DogLimbRunningPlay (dt24_cage_loot_play): the start point lies 80 - 180 u beyond the group's average position; the tick re-picks a
// destination 1000 - 1400 u away on arrival or within 200 u of it.
constexpr double kPlayStartMin = 80.0;
constexpr double kPlayStartMax = 180.0;
constexpr double kPlayRepickMin = 1000.0;
constexpr double kPlayRepickMax = 1400.0;
constexpr double kPlayArrived = 200.0;

// The ground item of a state::Id, an invalid entity when there is none.
flecs::entity groundOf(flecs::world& ecs, std::uint64_t id)
{
	flecs::entity found;
	ecs.each([&](flecs::entity entity, const state::Id& key, const state::GroundItem&)
	{
		if (key.value == id)
			found = entity;
	});
	return found;
}

} // namespace

bool Simulation::carriesLimb(std::uint64_t id) const
{
	const state::Inventory* pack = fighterEntity(id).get<state::Inventory>();
	if (pack == nullptr || combat_ == nullptr)
		return false;
	for (const state::Inventory::Entry& entry : pack->items)
	{
		const ItemInfo* info = entry.slot == state::Inventory::kCarried && entry.count > 0 ? combat_->data().item(entry.itemSid) : nullptr;
		if (info != nullptr && info->function == kItemFunctionLimb)
			return true;
	}
	return false;
}

void Simulation::endGoalRoutine(std::uint64_t id, std::uint16_t goal)
{
	flecs::entity actor = fighterEntity(id);
	const state::Routine* current = actor.get<state::Routine>();
	if (current == nullptr || current->goal != goal)
		return;
	limbPlay_.erase(id);
	standingRun_.erase(id);
	const flecs::entity squad = actor.target<state::MemberOf>();
	if (squad.is_valid() && squad.has<state::SquadRoutine>())
	{
		state::Routine r = *current; // a resident with no Routine is never scored again: hand it back
		r.goal = 0;
		r.target = {};
		r.chainTarget = 0;
		r.nextScore = clockTick();
		actor.set<state::Routine>(std::move(r));
	} else {
		actor.remove<state::Routine>();
	}
	mirrorFighter(id);
}

bool Simulation::orderAnimalGoal(std::uint64_t id, std::uint16_t goal)
{
	const state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr || gameData_ == nullptr || f->dead || f->unconscious || f->down || f->carriedBy != 0)
		return false;
	if (goal == routines::task::EatCrops)
		return orderEatCrops(id);
	flecs::world& ecs = ecsMutable();
	const sensor::Context ctx = sensorContext();
	const auto lookup = [this](std::uint64_t target) { return combat_->find(target); };
	const auto holdsFunction = [this](std::uint64_t holder, std::int32_t function)
	{
		const state::Inventory* pack = fighterEntity(holder).get<state::Inventory>();
		if (pack == nullptr)
			return false;
		for (const state::Inventory::Entry& entry : pack->items)
		{
			const ItemInfo* info = entry.slot == state::Inventory::kCarried && entry.count > 0 ? combat_->data().item(entry.itemSid) : nullptr;
			if (info != nullptr && info->function == function)
				return true;
		}
		return false;
	};
	const auto grounds = [&]()
	{
		std::vector<worldres::GroundThing> things;
		ecs.each([&](const state::Id& key, const state::GroundItem& item)
		{
			const ItemInfo* info = combat_->data().item(item.itemSid);
			things.push_back({key.value, item.useX, item.useZ, info != nullptr ? info->function : 0});
		});
		return things;
	};
	std::uint64_t target = 0;
	switch (goal)
	{
	case routines::task::EatFoodOnGround:
	{
		bool onBody = false;
		const std::vector<worldres::GroundThing> things = grounds();
		target = worldres::foodOnGround259(*f, things, ctx, lookup, [&](std::uint64_t body) { return holdsFunction(body, kItemFunctionFood); }, onBody);
		break;
	}
	case routines::task::AnimalFetchALimb:
	{
		const auto cooling = limbCooldown_.find(id);
		const bool allowed = f->hunger < kHungerGate && (cooling == limbCooldown_.end() || cooling->second <= clockTick());
		target = worldres::limb277(*f, grounds(), allowed);
		break;
	}
	case routines::task::PlayWithLimbInMouth:
		target = carriesLimb(id) ? id : 0; // no finder (desc+0x160 null): the plan's hand is the animal itself
		break;
	case routines::task::ChaseAllyDogs:
		target = worldres::mouthLimbAlly279(*f, ctx, lookup, [&](std::uint64_t ally) { return carriesLimb(ally); });
		break;
	case routines::task::LootAnimalsJob:
	{
		// 140998950's storage test (14059acd0): a type one of my boxes holds or one of my machines makes (241's reading, UNSURE).
		std::string mySid;
		if (const state::Allegiance* mine = fighterEntity(id).get<state::Allegiance>())
			mySid = mine->factionSid;
		std::set<std::string> storable;
		if (!mySid.empty())
		{
			ecs.each([&](flecs::entity entity, const state::Container& container)
			{
				const state::Inventory* stock = entity.get<state::Inventory>();
				if (container.ownerSid != mySid || container.locked || stock == nullptr)
					return;
				for (const state::Inventory::Entry& entry : stock->items)
					if (entry.slot == state::Inventory::kCarried)
						storable.insert(entry.itemSid);
			});
			ecs.each([&](const state::Producer& producer)
			{
				if (producer.ownerSid == mySid)
					storable.insert(producer.output);
			});
		}
		target = worldres::animalLoot238(*f, ctx, lookup, [](std::uint64_t) { return false; },
			[&](std::uint64_t body, const state::Fighter&)
			{
				const state::Inventory* pack = fighterEntity(body).get<state::Inventory>();
				if (pack == nullptr)
					return false;
				for (const state::Inventory::Entry& entry : pack->items)
				{
					const ItemInfo* info = entry.slot == state::Inventory::kCarried && entry.count > 0 ? combat_->data().item(entry.itemSid) : nullptr;
					if (info != nullptr && info->function == 0 && storable.count(entry.itemSid) != 0)
						return true;
				}
				return false;
			});
		break;
	}
	default:
		return false;
	}
	if (target == 0)
		return false;
	state::Routine routine;
	routine.goal = goal;
	routine.since = clockTick();
	routine.chainTarget = target;
	routine.chainElapsed = goal == routines::task::LootAnimalsJob ? -1.0 : 0.0; // 241's timer: negative while walking
	routine.plan = {goal};
	routine.nextScore = clockTick() + 100000; // the scorer does not re-score the member while the goal runs; endGoalRoutine hands it back
	fighterEntity(id).set<state::Routine>(std::move(routine));
	mirrorFighter(id);
	return true;
}

void Simulation::stepAnimalGoals(const std::function<bool(std::uint64_t, double, double)>& walk)
{
	if (combat_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const std::uint64_t now = clockTick();
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		flecs::entity actor = ecs.entity(combatEntities_[i]);
		const state::Routine* current = actor.get<state::Routine>();
		if (current == nullptr)
			continue;
		const std::uint16_t goal = current->goal;
		if (goal == routines::task::EatCrops && cropOrders_.count(members[i].id) == 0)
		{
			endGoalRoutine(members[i].id, goal); // a load keeps the goal (the saved Routine) and not the order (stepCropOrders ends the goal with it)
			continue;
		}
		if (goal != routines::task::EatFoodOnGround && goal != routines::task::AnimalFetchALimb && goal != routines::task::PlayWithLimbInMouth
			&& goal != routines::task::ChaseAllyDogs)
			continue;
		const std::uint64_t id = members[i].id;
		const std::uint64_t target = current->chainTarget;
		const std::uint64_t since = current->since;
		state::Fighter& f = *combat_->find(id);
		if (f.dead || f.unconscious || f.down || f.carriedBy != 0 || f.target != 0)
		{
			endGoalRoutine(id, goal); // a fight, a fall or a lift ends it: the goal is just not served any more
			continue;
		}
		const bool gaveUp = static_cast<double>(ticksSince(now, since)) * kTickSeconds > kGiveUpSeconds;
		if (goal == routines::task::EatFoodOnGround || goal == routines::task::AnimalFetchALimb)
		{
			// 259 (finder 14099a930) / 277 (1409993a0): walk to the loose item and pick it up (the shared PickUp interact: it lands in the
			// pack), or, for 259 only, take the food off the dead body. The goal ends once the item is gone from the ground.
			const flecs::entity ground = groundOf(ecs, target);
			const state::GroundItem* item = ground.is_valid() ? ground.get<state::GroundItem>() : nullptr;
			const state::Fighter* body = item == nullptr && goal == routines::task::EatFoodOnGround ? combat_->find(target) : nullptr;
			if (item == nullptr && (body == nullptr || !body->dead))
			{
				if (goal == routines::task::AnimalFetchALimb && carriesLimb(id))
					limbCooldown_[id] = now + static_cast<std::uint64_t>(kLimbCooldownSeconds / kTickSeconds);
				std::ostringstream line;
				line << "animal " << goal << " member " << id << " took target " << target;
				logTask(line.str());
				endGoalRoutine(id, goal);
				continue;
			}
			if (gaveUp)
			{
				endGoalRoutine(id, goal);
				continue;
			}
			if (item != nullptr)
			{
				if (actor.has<state::InteractTask>())
					continue; // the pick-up's own walk and reach test run it
				if (std::hypot(f.x - item->useX, f.z - item->useZ) <= kReachGroundItem && std::fabs(f.y - item->useY) <= kInteractReachY)
					beginInteract(id, target, static_cast<std::uint8_t>(InteractAction::PickUp), 0, {}, true);
				else if (now % kSquadIntervalTicks == 0)
					walk(id, item->useX, item->useZ);
				continue;
			}
			if (std::hypot(f.x - body->x, f.z - body->z) > kInteractReach)
			{
				if (now % kSquadIntervalTicks == 0)
					walk(id, body->x, body->z);
				continue;
			}
			// At the body: every carried food entry that fits moves into my pack.
			flecs::entity bodyEntity = fighterEntity(target);
			const state::Inventory* held = bodyEntity.get<state::Inventory>();
			if (held != nullptr)
			{
				const grid::Rules rules(*gameData_);
				state::Inventory source = *held;
				grid::settle(source, rules, target);
				state::Inventory mine;
				if (const state::Inventory* packed = actor.get<state::Inventory>())
					mine = *packed;
				std::vector<state::Inventory::Entry> overflow = grid::settle(mine, rules, id);
				bool took = false;
				for (auto entry = source.items.begin(); entry != source.items.end();)
				{
					const ItemInfo* info = entry->slot == state::Inventory::kCarried ? combat_->data().item(entry->itemSid) : nullptr;
					if (info == nullptr || info->function != kItemFunctionFood)
					{
						++entry;
						continue;
					}
					state::Inventory::Entry unplaced = *entry;
					unplaced.slot = state::Inventory::kCarried;
					unplaced.section = state::Inventory::Entry::kUnplaced;
					const std::uint32_t left = grid::autoplace(mine, rules, unplaced);
					if (left == entry->count)
					{
						++entry; // no room: it stays on the body
						continue;
					}
					took = true;
					if (left > 0)
					{
						entry->count = left;
						++entry;
					} else {
						entry = source.items.erase(entry);
					}
				}
				if (took)
				{
					++mine.version;
					++source.version;
					storeInventory(actor, id, std::move(mine), std::move(overflow));
					bodyEntity.set<state::Inventory>(std::move(source));
				}
			}
			std::ostringstream line;
			line << "animal " << goal << " member " << id << " took food off body " << target;
			logTask(line.str());
			endGoalRoutine(id, goal);
		} else if (goal == routines::task::PlayWithLimbInMouth) {
			// Task_DogLimbRunningPlay (dt24_cage_loot_play): no finish. The start runs to a point 80 - 180 u beyond the average position of
			// the allies sensed; the tick re-picks a random destination 1000 - 1400 u away on arrival or within 200 u of the current one;
			// the run is the mover default. It ends only when the limb leaves the mouth (or the animal falls / fights, above).
			if (!carriesLimb(id))
			{
				endGoalRoutine(id, goal);
				continue;
			}
			LimbPlay& play = limbPlay_[id];
			if (play.picks == 0 || std::hypot(play.x - f.x, play.z - f.z) <= kPlayArrived)
			{
				RngStream dice(streamSeed(streamSeed(seed_, kRoutineStream), static_cast<std::size_t>(id) + 977 * static_cast<std::size_t>(play.picks + 1)));
				if (play.picks == 0)
				{
					double sumX = f.x;
					double sumZ = f.z;
					int count = 1;
					const sensor::Context ctx = sensorContext();
					sensor::sensed(f, sensor::kAlly, sensor::kDead, ctx, [this](std::uint64_t other) { return combat_->find(other); },
						[&](std::uint64_t, const state::Fighter& ally, std::uint32_t)
						{
							sumX += ally.x;
							sumZ += ally.z;
							++count;
						});
					const double avgX = sumX / count;
					const double avgZ = sumZ / count;
					double dx = avgX - f.x;
					double dz = avgZ - f.z;
					double span = std::hypot(dx, dz);
					if (span < 1.0e-6)
					{
						const double angle = 2.0 * std::numbers::pi * dice.uniform01();
						dx = std::sin(angle);
						dz = std::cos(angle);
						span = 1.0;
					}
					const double beyond = kPlayStartMin + dice.uniform01() * (kPlayStartMax - kPlayStartMin);
					play.x = avgX + dx / span * beyond;
					play.z = avgZ + dz / span * beyond;
				} else {
					const double angle = 2.0 * std::numbers::pi * dice.uniform01();
					const double away = kPlayRepickMin + dice.uniform01() * (kPlayRepickMax - kPlayRepickMin);
					play.x = f.x + std::sin(angle) * away;
					play.z = f.z + std::cos(angle) * away;
				}
				++play.picks;
				std::ostringstream line;
				line << "animal 278 member " << id << " pick " << play.picks << " to (" << play.x << ", " << play.z << ")";
				logTask(line.str());
			}
			f.moveMode = 2;
			if (now % kSquadIntervalTicks == 0)
				walk(id, play.x, play.z);
		} else {
			// 279 (finder 14099ad40): run at the ally dog that holds a limb until within 50 u (isNearTo = isWithin50Meters).
			const state::Fighter* ally = combat_->find(target);
			if (ally == nullptr || ally->dead || !carriesLimb(target) || gaveUp)
			{
				endGoalRoutine(id, goal);
				continue;
			}
			if (std::hypot(f.x - ally->x, f.z - ally->z) <= kChaseNear)
			{
				std::ostringstream line;
				line << "animal 279 member " << id << " reached ally " << target;
				logTask(line.str());
				endGoalRoutine(id, goal);
			} else if (now % kSquadIntervalTicks == 0) {
				walk(id, ally->x, ally->z);
			}
		}
	}
}

} // namespace ksim
