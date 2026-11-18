// The shopping executor (task CH group 3; kinspect dt24_shop.md, dt24_118.md): the member's day list, the shop's stock and the
// 119 BUY_SHIT transaction pulse. The pure rules are shopping.hpp; this file is the Simulation side.

#include "ksim/simulation.hpp"
#include "ksim/squad_caches.hpp"

#include "ksim/inventory_grid.hpp"
#include "ksim/routines.hpp"
#include "ksim/shopping.hpp"
#include "ksim/trader.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace ksim {

namespace {

// The member's stand at the counter: the scorer's arrival reach (4 u). The counter piece can lie inside a non-walkable interior
// (the Hub bar's, as for the intruder lift), so the walk's nearest point is a little off it: OURS, 6 u.
constexpr double kCounterReach = 6.0;

} // namespace

bool Simulation::refreshShoppingList(state::Routine& routine, const state::CharacterInstance& character, std::uint64_t diceKey)
{
	if (combatData_ == nullptr || gameData_ == nullptr)
		return false;
	const double hours = trader::gameHours(clockTick());
	const double day = std::floor(hours / 24.0);
	const double hour = hours - day * 24.0;
	// The daily reset (TQ24's live reading: the first evaluation after 07:00; ours is 08:00 - UNSURE. The built day is the only
	// saved part; the list is a function of (seed, key, day)).
	if (routine.shoppingDay >= 0 && !(static_cast<double>(routine.shoppingDay) < day && hour >= shopping::kResetHour))
		return false;
	const World& loaded = world();
	const auto prefab = loaded.prefabs().find(character.templateSid);
	const tmpl::Character* record = prefab != loaded.prefabs().end() ? prefab->second.get<tmpl::Character>() : nullptr;
	const std::span<const tmpl::ShoppingRef> refs = record != nullptr ? std::span<const tmpl::ShoppingRef>(record->shopping)
																	 : std::span<const tmpl::ShoppingRef>();
	routine.shopping = shopping::buildList(refs, *combatData_, seed_, diceKey, static_cast<std::int64_t>(day));
	routine.shoppingDay = static_cast<std::int32_t>(day);
	routine.shoppingTimer = 0.0;
	routine.shoppingLatch = false;
	return true;
}

flecs::entity Simulation::shopSquad(std::uint64_t building) const
{
	if (combat_ == nullptr || building == 0)
		return {};
	SquadCaches& caches = *squadCaches_;
	if (caches.shopDirty)
	{
		caches.shops.clear();
		caches.shopDirty = false;
	}
	if (const auto cached = caches.shops.find(building); cached != caches.shops.end())
		return cached->second;
	const flecs::world& ecs = world().ecs();
	flecs::entity best;
	std::uint64_t bestId = 0;
	ecs.each([&](flecs::entity squad, const state::TraderStock&, const state::Id& squadId, const state::Inventory&)
	{
		if (best.is_valid() && squadId.value >= bestId)
			return;
		bool homed = false;
		ecs.query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](const state::Id& member)
		{
			const state::Fighter* fighter = combat_->find(member.value);
			homed = homed || (fighter != nullptr && fighter->homeBuilding == building);
		});
		if (homed)
		{
			best = squad;
			bestId = squadId.value;
		}
	});
	caches.shops.emplace(building, best);
	return best;
}

void Simulation::stepShopping()
{
	if (combat_ == nullptr || gameData_ == nullptr || combatData_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const std::uint64_t now = clockTick();
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		flecs::entity actor = ecs.entity(combatEntities_[i]);
		const state::Routine* found = actor.get<state::Routine>();
		if (found == nullptr || found->goal != routines::task::Shopping)
			continue;
		const state::CharacterInstance* character = actor.get<state::CharacterInstance>();
		if (character == nullptr)
			continue;
		const std::uint64_t id = members[i].id;
		state::Fighter& f = *combat_->find(id);
		const std::uint64_t diceKey = f.diceSeed != 0 ? f.diceSeed : id;
		state::Routine r = *found;
		bool changed = refreshShoppingList(r, *character, diceKey);
		// The task's tick: while the movement is occupied (walking, fighting, down, carried) the timer stands still
		// (dt24_shop section 4: movement+0x24 != 0 returns early).
		const bool standing = !f.dead && !f.unconscious && !f.down && f.carriedBy == 0 && f.carrying == 0 && f.target == 0
			&& !f.walking() && r.shoppingShop != 0 && std::hypot(f.x - r.shoppingX, f.z - r.shoppingZ) <= kCounterReach;
		if (!standing)
		{
			if (changed)
				actor.set<state::Routine>(std::move(r));
			continue;
		}
		// The plan's step: the 67 walk is the scorer's route; the transaction runs once the member stands at the counter.
		r.step = 1; // [67 Running, 119 BUY_SHIT, 118]: the walk is done
		r.shoppingTimer -= kTickSeconds;
		if (r.shoppingTimer > 0.0)
		{
			actor.set<state::Routine>(std::move(r));
			continue;
		}
		// One transaction attempt (1409a14b0): the first record still wanting, one stock item of its function at random, into the
		// member's arms.
		bool took = false;
		const state::ShoppingWant* want = shopping::firstUnmet(r.shopping);
		if (want != nullptr)
		{
			flecs::entity shop = shopSquad(r.shoppingShop);
			state::Inventory* store = shop.is_valid() ? shop.get_mut<state::Inventory>() : nullptr;
			if (store != nullptr)
			{
				state::InventoryEntry piece;
				RngStream dice(streamSeed(streamSeed(streamSeed(seed_, kShoppingPickStream), static_cast<std::size_t>(diceKey)),
					static_cast<std::size_t>(now)));
				if (shopping::takeOne(*store, *combatData_, want->function, dice.uniform01(), piece))
				{
					if (state::Inventory* mine = actor.get_mut<state::Inventory>())
					{
						const grid::Rules rules(*gameData_);
						mine->items.push_back(piece);
						const std::vector<state::InventoryEntry> over = grid::settle(*mine, rules, id);
						(void)over; // what does not fit is lost (OURS: the exe's add path has no drop here)
						++mine->version;
					}
					const auto bought = std::find_if(r.shopping.begin(), r.shopping.end(),
						[&](const state::ShoppingWant& entry) { return entry.function == want->function; });
					if (bought != r.shopping.end())
						bought->taken = static_cast<std::uint16_t>(bought->taken + 1);
					r.shoppingLatch = true;
					r.shoppingTimer = shopping::kTimer;
					took = true;
				}
			}
		}
		if (!took && r.shoppingLatch)
		{
			// A failed attempt after a success: the task is done (the exe's flagTaskDone); the scorer picks the next goal.
			r.goal = 0;
			r.target = {};
			r.plan.clear();
			r.step = 0;
			r.shoppingShop = 0;
			r.nextScore = 0;
			routineBook_.release(id);
		}
		// A failure with no success yet leaves the timer spent: the exe retries every tick, as ours does.
		actor.set<state::Routine>(std::move(r));
	}
}

} // namespace ksim
