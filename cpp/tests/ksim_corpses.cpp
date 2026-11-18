// Corpse decay (kinspect BQ15: bq15_corpse_cleanup.md a, s79_static.md 1; ARCHITECTURE.md "Corpses"; corpses.hpp; save v54).
// The exe's dead-character branch (1405ccd90): the first evaluation after death stamps the time, later ones destroy the corpse when
// now - stamp > 12 game hours; a carried corpse's age is capped at 6 h. b0's mapping onto our all-loaded world: an NPC corpse is
// evaluated only inside a player's full-detail area, is destroyed when no player area or player load range covers it ("corpse
// unloaded"), a player corpse is always evaluated, nothing is evaluated while no player area exists.

#include "ksim/clock.hpp"
#include "ksim/corpses.hpp"
#include "ksim/simulation.hpp"
#include "ksim/state.hpp"

#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;

// Zone cell (32, 32) spans x, z in [0, 4608): the player stands at its centre.
constexpr double kPlayerX = 2304.0;
constexpr double kPlayerZ = 2304.0;

struct Rig
{
	Simulation sim;

	explicit Rig(int areaRadiusRegions, bool playerArea = true)
		: sim(Simulation::load(KENSHI_DATA, 5, config(areaRadiusRegions)))
	{
		if (playerArea)
			sim.enqueue(Command::setPlayerAreas({state::GroundPoint{kPlayerX, kPlayerZ}}));
		sim.run(2);
	}

	static RegionConfig config(int areaRadiusRegions)
	{
		RegionConfig config;
		config.areaRadiusRegions = areaRadiusRegions;
		return config;
	}

	flecs::world& ecs() { return const_cast<flecs::world&>(sim.world().ecs()); }

	// A dead character with an item in its pack, at (x, z).
	std::uint64_t corpse(double x, double z, bool player = false, std::uint64_t carriedBy = 0)
	{
		flecs::entity body = state::createInstance(ecs());
		state::Fighter fighter;
		fighter.dead = true;
		fighter.x = x;
		fighter.z = z;
		fighter.playerControlled = player;
		fighter.carriedBy = carriedBy;
		body.set<state::Fighter>(fighter);
		state::Inventory pack;
		state::Inventory::Entry item;
		item.itemSid = "corpse-loot";
		item.count = 1;
		pack.items.push_back(item);
		body.set<state::Inventory>(pack);
		return body.get<state::Id>()->value;
	}

	flecs::entity find(std::uint64_t id)
	{
		flecs::entity found{};
		ecs().each([&](flecs::entity entity, const state::Id& key, const state::Fighter&)
		{
			if (key.value == id)
				found = entity;
		});
		return found;
	}

	bool exists(std::uint64_t id) { return find(id).is_valid(); }

	// The stamp in game hours; 0 when the body was never evaluated.
	double stamp(std::uint64_t id)
	{
		const flecs::entity body = find(id);
		const state::CorpseDecay* decay = body.is_valid() ? body.get<state::CorpseDecay>() : nullptr;
		return decay != nullptr ? decay->stamp : 0.0;
	}

	void hours(double h) { sim.run(static_cast<std::uint64_t>(h * static_cast<double>(kTicksPerGameHour))); }

	double nowHours() { return gameHoursIn(sim.clockTick()); }
};

} // namespace

int main()
{
	try
	{
		// ---- in a player area: stamped at the first evaluation, destroyed after 12 game hours (strictly more) ----
		{
			Rig rig(1);
			const std::uint64_t body = rig.corpse(kPlayerX + 100.0, kPlayerZ);
			rig.hours(0.05);
			const double stamp = rig.stamp(body);
			test::require(stamp > 0.0 && stamp <= rig.nowHours(), "the corpse in the player area was not stamped at its first evaluation");
			rig.hours(11.5);
			test::require(rig.exists(body) && rig.stamp(body) == stamp, "the corpse went before 12 game hours, or its stamp moved");
			rig.hours(0.6); // 12.15 h after the stamp
			test::require(!rig.exists(body), "the corpse in the player area survived past 12 game hours");
			std::cout << "ok  destroyed 12 game hours after the stamp in a player area (stamp " << stamp << " h)\n";
		}

		// ---- an NPC corpse in loaded ground but outside every detail area is not evaluated: it never ages ----
		{
			Rig rig(0); // the detail area is the player's own region; the load range is the 3x3 zones around it
			const std::uint64_t body = rig.corpse(kPlayerX + kworld::kZoneSize, kPlayerZ); // the next zone: loaded, off screen
			rig.hours(30.0);
			test::require(rig.exists(body), "a corpse outside the detail area but inside the load range was destroyed");
			test::require(rig.stamp(body) == 0.0, "a corpse outside the detail area was evaluated (stamped)");
			std::cout << "ok  not aged outside a player area (30 game hours, never stamped)\n";
		}

		// ---- no player within range: "corpse unloaded", destroyed at the next evaluation ----
		{
			Rig rig(0);
			const std::uint64_t far = rig.corpse(kPlayerX + 3.0 * kworld::kZoneSize, kPlayerZ);
			const std::uint64_t edge = rig.corpse(kPlayerX + 1.5 * kworld::kZoneSize, kPlayerZ); // the 2nd zone out of the 3x3 block
			rig.hours(0.05);
			test::require(!rig.exists(far), "a corpse far outside every player's range was not destroyed");
			test::require(!rig.exists(edge), "a corpse two zones out (outside the 3x3 load block) was not destroyed");
			std::cout << "ok  destroyed when no player area or load range covers it\n";
		}

		// ---- a player corpse always ages, anywhere, and is never 'unloaded' ----
		{
			Rig rig(0);
			const std::uint64_t body = rig.corpse(kPlayerX + 6.0 * kworld::kZoneSize, kPlayerZ, true);
			rig.hours(0.05);
			test::require(rig.exists(body) && rig.stamp(body) > 0.0, "a far player corpse was destroyed as unloaded or not evaluated");
			rig.hours(11.5);
			test::require(rig.exists(body), "a player corpse decayed before 12 game hours");
			rig.hours(0.6);
			test::require(!rig.exists(body), "a player corpse did not decay after 12 game hours");
			std::cout << "ok  a player corpse ages anywhere\n";
		}

		// ---- nobody plays: nothing is evaluated, nothing is unloaded ----
		{
			Rig rig(1, false);
			const std::uint64_t body = rig.corpse(kPlayerX, kPlayerZ);
			const std::uint64_t player = rig.corpse(kPlayerX, kPlayerZ, true);
			rig.hours(5.0);
			test::require(rig.exists(body) && rig.stamp(body) == 0.0, "an NPC corpse was evaluated with no player area");
			test::require(rig.exists(player) && rig.stamp(player) > 0.0, "a player corpse was not evaluated with no player area");
			rig.hours(8.0);
			test::require(rig.exists(body) && !rig.exists(player), "with no player area an NPC corpse went or a player corpse stayed");
			std::cout << "ok  no player area: NPC corpses stay, player corpses decay\n";
		}

		// ---- a carried corpse: age capped at 6 h, so it never decays while carried, and 6 h after the release ----
		{
			Rig rig(1);
			const std::uint64_t body = rig.corpse(kPlayerX, kPlayerZ, false, 999);
			rig.hours(0.05);
			const double first = rig.stamp(body);
			rig.hours(20.0);
			test::require(rig.exists(body), "a carried corpse decayed");
			test::require(std::abs((rig.nowHours() - rig.stamp(body)) - kCorpseCarriedAgeCapHours) < 0.05
				&& rig.stamp(body) > first, "a carried corpse's age is not held at 6 h");
			rig.find(body).get_mut<state::Fighter>()->carriedBy = 0; // put down
			rig.hours(5.5);
			test::require(rig.exists(body), "a put-down corpse (6 h old) decayed before 6 more hours");
			rig.hours(0.7);
			test::require(!rig.exists(body), "a put-down corpse did not decay 12 h after its capped stamp");
			std::cout << "ok  a carried corpse is capped at 6 h of age\n";
		}

		// ---- the stamp survives a save and load; the inventory goes with the corpse ----
		{
			Rig rig(1);
			const std::uint64_t body = rig.corpse(kPlayerX, kPlayerZ);
			rig.hours(6.0);
			const double stamp = rig.stamp(body);
			test::require(stamp > 0.0, "no stamp after six hours in the area");
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_corpses_test.ksav";
			rig.sim.saveGame(path);
			Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path);
			flecs::world& ecs = const_cast<flecs::world&>(loaded.world().ecs());
			double loadedStamp = 0.0;
			ecs.each([&](const state::Id& id, const state::CorpseDecay& decay) { loadedStamp = id.value == body ? decay.stamp : loadedStamp; });
			test::require(loadedStamp == stamp, "the corpse stamp did not survive the save and load");
			loaded.run(5 * kTicksPerGameHour);
			rig.sim.run(5 * kTicksPerGameHour);
			test::require(loaded.stateHash() == rig.sim.stateHash(), "load + run differs from the uninterrupted run");
			std::size_t left = 0;
			ecs.each([&](const state::Id& id, const state::Fighter&) { left += id.value == body ? 1 : 0; });
			test::require(left == 1, "the corpse went before 12 hours");
			loaded.run(2 * kTicksPerGameHour);
			rig.sim.run(2 * kTicksPerGameHour);
			left = 0;
			ecs.each([&](const state::Id& id, const state::Fighter&) { left += id.value == body ? 1 : 0; });
			test::require(left == 0 && !rig.exists(body), "the loaded corpse did not decay 12 h after its saved stamp");
			// Nothing extra is dropped: the pack went with the body.
			std::size_t loot = 0;
			ecs.each([&](const state::Inventory& inv)
			{
				for (const auto& entry : inv.items)
					loot += entry.itemSid == "corpse-loot" ? 1 : 0;
			});
			test::require(loot == 0, "a destroyed corpse left its inventory behind");
			test::require(loaded.stateHash() == rig.sim.stateHash(), "load + run differs from the uninterrupted run after the decay");
			std::remove(path.c_str());
			std::cout << "ok  the stamp survives save + load; the pack goes with the corpse\n";
		}
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_corpses: " << error.what() << "\n";
		return 1;
	}
	std::cout << "ksim_corpses ok\n";
	return 0;
}
