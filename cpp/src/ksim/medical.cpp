// Meals and first aid: the parts of the medical slice that touch an inventory (MEDICAL_HUNGER_LIVE.md a, c). The rates live
// in CombatRules (hunger, bandage pool, kit drain, clotting); this file picks the food and the kit and spends them.

#include "ksim/combat.hpp"
#include "ksim/clock.hpp"
#include "ksim/interact.hpp"
#include "ksim/simulation.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ksim {

namespace {

// Auto-eat starts below 2.1 for the player's characters, 2.4 for the AI's (MEDICAL_HUNGER_LIVE.md a).
constexpr double kAutoEatPlayer = 2.1;
constexpr double kAutoEatAi = 2.4;
constexpr double kFed = 3.0; // `fed` (global 0x24): the full level
// One bite is min(charges, 50) charges and adds bite x 0.01 hunger (static decode, not seen live). A bite every second is a
// guess (UNSURE), and so is the runtime charge count of a food item: the data says 1.0 (Bread) and a bite of 1 would add 0.01,
// so the data value is read as a count of 100-charge units (UNSURE: the live charges of Bread were not read).
constexpr double kBiteSeconds = 1.0;
constexpr double kBiteMax = 50.0;
constexpr double kBiteHunger = 0.01;

// Which kind of kit the patient needs next: organic while an organic part has room in its pool or a wound is open, else robotic
// when a robotic part has room (the bandage step treats one kind per kit).
bool needsRobotKit(const state::Fighter& patient)
{
	bool robotRoom = false;
	for (const state::FighterPart& p : patient.parts)
	{
		if ((p.maxHealth - p.wear) - p.health - p.pool <= 1.0e-9)
			continue;
		if (!p.robotic)
			return false;
		robotRoom = true;
	}
	return robotRoom && patient.wounds.empty();
}

// The animals' shared food reach and the outdoor-storage reach (dt24_eat section 9: DAT_1416bf93c = 40000.0 squared, 200 u).
constexpr double kAnimalFoodShareRange = 200.0;
} // namespace

void Simulation::stepMeals()
{
	if (combat_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const auto& members = combat_->members();

	// The food pick of one inventory: the exe's 1405c78e0 (dt24_eat section 9) - FOOD_RESTRICTED (0xf) first, then FOOD (3),
	// over the carried grid (main) and then the worn backpack's contents. "" when it holds none.
	const auto foodIn = [&](const state::Inventory& inventory) -> std::string
	{
		for (const std::int32_t function : {kItemFunctionFoodRestricted, kItemFunctionFood})
		{
			for (const state::Inventory::Entry& entry : inventory.items)
			{
				const ItemInfo* info = entry.slot == state::Inventory::kCarried ? combat_->data().item(entry.itemSid) : nullptr;
				if (info != nullptr && info->function == function)
					return entry.itemSid;
			}
			if (const state::Inventory::Entry* bag = inventory.wornBackpack())
			{
				for (const state::Inventory::Entry& entry : bag->contents)
				{
					const ItemInfo* info = combat_->data().item(entry.itemSid);
					if (info != nullptr && info->function == function)
						return entry.itemSid;
				}
			}
		}
		return {};
	};
	// The animals' wider search (task animal-feed; dt24_eat section 9, FUN_1405d29c0): after the eater's own pack (the caller),
	// a squadmate's, then an outdoor food container. The food teleports - one unit leaves the source (the exe's 1405d0780 eats
	// the picked item). The exe's two switches (DAT_142134690+0x114 the animal auto-eat, +0x115 the squad share) are not world
	// options here (UNSURE); ours runs both always. UNSURE also: the exe shares a squadmate's BACKPACK only and only for
	// player-faction characters, and its building finder weighs ownership; ours widens to the whole inventory of a squadmate, any
	// squad, and takes from any unlocked container.
	const auto foodForAnimal = [&](std::uint64_t id, flecs::entity entity) -> std::string
	{
		const state::Fighter* me = combat_->find(id);
		if (me == nullptr)
			return {};
		std::vector<std::uint64_t> mates;
		if (const flecs::entity squad = entity.target<state::MemberOf>(); squad.is_valid())
		{
			ecs.query_builder<const state::Id>().with<state::MemberOf>(squad).build()
				.each([&](const state::Id& other) { if (other.value != id) mates.push_back(other.value); });
		} else {
			// A player squad holds its members in a list (a member joined from the world loses its MemberOf).
			ecs.each([&](const state::PlayerSquad& squad)
			{
				if (std::find(squad.members.begin(), squad.members.end(), id) == squad.members.end())
					return;
				for (const std::uint64_t member : squad.members)
					if (member != id)
						mates.push_back(member);
			});
		}
		// 2. a squadmate's inventory and worn backpack within 200 u.
		for (const std::uint64_t mate : mates)
		{
			const state::Fighter* other = combat_->find(mate);
			if (other == nullptr || std::hypot(other->x - me->x, other->z - me->z) > kAnimalFoodShareRange)
				continue;
			flecs::entity otherEntity = fighterEntity(mate);
			const state::Inventory* held = otherEntity.get<state::Inventory>();
			if (held == nullptr)
				continue;
			state::Inventory inventory = *held;
			const std::string food = foodIn(inventory);
			if (food.empty())
				continue;
			if (!inventory.removeCarried(food, 1))
				continue;
			otherEntity.set<state::Inventory>(std::move(inventory));
			return food;
		}
		// 3. an outdoor storage box with food within 200 u (the exe's food finder rejects a building with an interior for an
		// animal, dt24_eat section 9 item 2).
		std::string found;
		ecs.each([&](flecs::entity box, const state::Container& container, state::Inventory& stock)
		{
			if (!found.empty() || container.locked || std::hypot(container.x - me->x, container.z - me->z) > kAnimalFoodShareRange)
				return;
			if (const BuildingSite* site = world_.buildingSite(buildingAt(container.x, container.y, container.z));
				site != nullptr && site->hasInterior)
				return;
			const std::string food = foodIn(stock);
			if (food.empty() || !stock.removeCarried(food, 1))
				return;
			box.set<state::Inventory>(stock);
			found = food;
		});
		return found;
	};

	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const state::Fighter& view = members[i].fighter;
		if (view.dead || view.unconscious || view.target != 0)
			continue;
		const double threshold = view.playerControlled ? kAutoEatPlayer : kAutoEatAi;
		const bool eating = view.foodCharges > 0.0;
		if (view.hunger >= (eating ? kFed : threshold))
			continue;
		state::Fighter* f = combat_->find(members[i].id);
		f->eatTimer -= kTickSeconds;
		if (f->eatTimer > 0.0)
			continue;
		if (f->foodCharges <= 0.0)
		{
			flecs::entity entity = ecs.entity(combatEntities_[i]);
			std::string food;
			if (const state::Inventory* held = entity.get<state::Inventory>())
			{
				state::Inventory inventory = *held;
				food = foodIn(inventory);
				if (!food.empty() && inventory.removeCarried(food, 1))
					entity.set<state::Inventory>(std::move(inventory));
				else
					food.clear();
			}
			if (food.empty() && f->animal)
				food = foodForAnimal(members[i].id, entity);
			if (food.empty())
				continue;
			const ItemInfo* info = combat_->data().item(food);
			f->foodCharges = info != nullptr ? info->charges * kFoodChargesPerUnit : 0.0;
		}
		const double bite = std::min(f->foodCharges, kBiteMax);
		f->foodCharges -= bite;
		f->hunger = std::min(f->hunger + bite * kBiteHunger, kFed);
		f->eatTimer = kBiteSeconds;
	}
}

bool Simulation::treat(std::uint64_t medicId, std::uint64_t patientId)
{
	state::Fighter* medic = combat_->find(medicId);
	state::Fighter* patient = combat_->find(patientId);
	if (medic == nullptr || patient == nullptr || patient->dead)
		return false;
	flecs::entity entity = fighterEntity(medicId);
	state::Inventory inventory;
	if (const state::Inventory* held = entity.get<state::Inventory>())
		inventory = *held;
	bool taken = false;
	const bool treated = treatBodies(*medic, *patient, medicId == patientId, entity.get<state::Inventory>() != nullptr ? &inventory : nullptr, taken, kTickSeconds);
	if (taken)
		entity.set<state::Inventory>(std::move(inventory));
	return treated;
}

// The step of treat() on bodies the caller holds (the arena's, or the stored members of the off-screen medical step: stored_medical.cpp, which
// calls it with the same numbers). `inventory` is the medic's own (null: no kit can be taken); `taken` is set when a kit left it.
bool Simulation::treatBodies(state::Fighter& medic, state::Fighter& patient, bool self, state::Inventory* inventory, bool& taken, double dt) const
{
	if (patient.dead)
		return false;
	const CombatRules& rules = combat_->rules();
	// The kit: the one in use (cached until it is empty), else the carried kit with the FEWEST charges above 0, ties to the first in
	// inventory order (Task_FirstAid's search, FUN_140746480; BLOCK_TIMING_LIVE.md; quality is never compared). Live the 150-charge
	// Basic kit won over the 1000-charge Advanced one, which fits. UNSURE: the original searches by the one kit function the task
	// needs: organic or robotic, here needsRobotKit(patient); a carried stack's units all count as full (a started kit is the
	// cached one in `kitSid`/`kitCharges`, not a stack entry).
	if (medic.kitCharges <= 0.0)
	{
		if (inventory == nullptr)
			return false;
		std::string kit;
		double fewest = 0.0;
		const bool wantRobot = needsRobotKit(patient);
		for (const state::Inventory::Entry& entry : inventory->items)
		{
			const ItemInfo* info = entry.slot == state::Inventory::kCarried ? combat_->data().item(entry.itemSid) : nullptr;
			if (info != nullptr && info->charges > 0.0 && info->function == (wantRobot ? kItemFunctionRobotKit : kItemFunctionMedkit)
				&& (kit.empty() || info->charges < fewest))
			{
				kit = entry.itemSid;
				fewest = info->charges;
			}
		}
		if (kit.empty() || !inventory->removeCarried(kit, 1))
			return false;
		taken = true;
		medic.kitSid = kit;
		medic.kitCharges = combat_->data().item(kit)->charges;
	}
	const ItemInfo* kit = combat_->data().item(medic.kitSid);
	if (kit == nullptr)
	{
		medic.kitCharges = 0.0; // a kit in use that the item data does not know (an imported member's): dropped, the next tick picks from the pack
		medic.kitSid.clear();
		return false;
	}
	const bool robotKit = kit->function == kItemFunctionRobotKit;
	const double skill = CombatRules::aidSkill(medic, self);
	const double quality = kit->quality; // the merged load order's value (Basic kit 5: the live runtime reading)
	// The wound step (FUN_14064d800) treats the newest wound only; the bandage step (FUN_140649a00) runs once the wound list is
	// empty (MEDICAL_HUNGER_LIVE (c), kinspect 32). A robotic kit never touches wounds.
	if (!robotKit && !patient.wounds.empty())
	{
		rules.clotWounds(patient, skill, quality, robotKit, dt);
	} else {
		bool more = false;
		rules.bandage(patient, rules.bandageRate(skill, quality, robotKit), robotKit, dt, more);
		if (!more)
			return false;
	}
	medic.kitCharges -= rules.kitDrain(skill) * dt;
	medic.skills.medic = std::min(medic.skills.medic + rules.aidXpPerSecond(medic) * dt, 100.0);
	return true;
}

// 269 HEAL_MY_LEGS (k16.6.4; dt45_down_treatment.md live, MEDICAL_HUNGER_LIVE.md (f)): a down, conscious NPC that carries a kit runs
// Task_FirstAid on itself, and again while the kit has charges (the interact ends when nothing is left to bandage or the kit is spent,
// and the next tick starts it again). The pool clears the KO point, the collapse flag recomputes, the body stands (65 GET_UP is
// that recompute) and the town scorer takes its job back. A kit handed to a down NPC starts it on the next tick: the inventory is read
// live. The members' `down` flag is the guard: a tick with nobody down costs one loop over the arena's members, no query.
void Simulation::stepSelfAid()
{
	if (combat_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		if (!members[i].fighter.down)
			continue;
		const flecs::entity actor = ecs.entity(combatEntities_[i]);
		if (actor.has<state::InteractTask>() || !planSelfAid(members[i].id).found())
			continue;
		beginInteract(members[i].id, members[i].id, static_cast<std::uint8_t>(InteractAction::FirstAid), 0, {}, false);
	}
}

// Task CG (dt24_58.md step 0): whether `id` has a kit of the item function, carried or the one already in use. It answers for any
// character: the medic's own equipment, and through kitsOf also the body it carries.
bool Simulation::carriesKit(std::uint64_t id, std::int32_t function) const
{
	if (combat_ == nullptr)
		return false;
	const state::Fighter* f = combat_->find(id);
	if (f == nullptr)
		return false;
	if (f->kitCharges > 0.0 && !f->kitSid.empty())
	{
		const ItemInfo* inUse = combat_->data().item(f->kitSid);
		if (inUse != nullptr && inUse->function == function)
			return true;
	}
	const state::Inventory* held = fighterEntity(id).get<state::Inventory>();
	if (held == nullptr)
		return false;
	for (const state::Inventory::Entry& entry : held->items)
	{
		const ItemInfo* info = entry.slot == state::Inventory::kCarried ? combat_->data().item(entry.itemSid) : nullptr;
		if (info != nullptr && info->function == function)
			return true;
	}
	return false;
}

} // namespace ksim
