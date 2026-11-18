// Carrying (docs/reference/GATES_CARRY_LIVE.md c, SIMLINK.md section 13): lift (interact 8) and put down (13) timings, the carried
// riding along, a carrier that is KO'd or killed dropping its person, the forced KO timer of a put-down non-ally, the encumbrance
// of a carried person and the run speed it gives, a save + load mid-carry, an NPC resident taking a KO'd intruder out of its
// building (door opened or unlocked on the way, the person put down at door outside + 1.1 (outside - inside)), building
// membership from crossing a door, and the building-faction lock levels of the Hub's doors.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache); argv[2]: the Stack cache (fixture knav_cache_stack) for
// the town gates (the Hub's are ruins, no doors).

#include "ksim/law/crime.hpp"
#include "ksim/combat/carry.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/nav/door_plan.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/towncircle.hpp"
#include "knav/walk.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;

constexpr std::uint8_t kCarry = static_cast<std::uint8_t>(InteractAction::Carry);
constexpr std::uint8_t kPutDown = static_cast<std::uint8_t>(InteractAction::PutDown);

// A faction Id the world does not know: the relation lookup answers 0 (neutral), never an ally. A fixture body copied from another
// fighter keeps the source's Arena faction Id, and a hand-spawned one is factionless; either way a stranger must not share the
// observer's Id, or sensor::ally reads "same faction".
std::uint64_t unknownFaction()
{
	return std::numeric_limits<std::uint64_t>::max();
}

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	ks::ServerContext context;
	std::unique_ptr<Simulation> sim;
	ks::Duel duel;
	std::vector<InteractEvent> events;
	std::uint32_t seq = 0;

	Rig(const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d, const std::shared_ptr<MeshNavigator>& n,
		const kworld::LoadOrder& order, bool withDuel = true)
		: gd(g)
		, data(d)
		, nav(n)
	{
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91));
		sim->setGameData(g);
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(data, settings);
		sim->enableNavigation(nav);
		if (withDuel)
			duel = ks::spawnDuel(*sim, *gd, *data, context);
		else
			spawnMeshDoors(*sim, *nav); // a mesh away from the Hub: its doors only
		sim->setInteractObserver([this](const InteractEvent& e) { events.push_back(e); });
	}

	flecs::world& ecs() { return const_cast<flecs::world&>(sim->world().ecs()); }
	state::Fighter& f(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }
	flecs::entity entityOf(std::uint64_t id)
	{
		flecs::entity found;
		ecs().each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { if (i.value == id) found = e; });
		return found;
	}
	const state::CarryStep* carryOf(std::uint64_t id) { return entityOf(id).get<state::CarryStep>(); }
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
	// KO: unconscious with a long timer (the medical update keeps it down).
	void knockOut(std::uint64_t id, double seconds = 600.0)
	{
		f(id).unconscious = true;
		f(id).koTimer = seconds;
	}
	std::optional<InteractEvent> doneEvent(std::uint64_t who, std::uint8_t action, std::size_t from = 0) const
	{
		for (std::size_t i = from; i < events.size(); ++i)
		{
			if (events[i].kind == InteractEvent::Kind::Done && events[i].who == who && events[i].action == action)
				return events[i];
		}
		return std::nullopt;
	}
	// The interact order as a session makes it: routed to the target when out of reach.
	void interact(std::uint64_t who, std::uint64_t target, std::uint8_t action)
	{
		const state::Fighter& a = f(who);
		const InteractTarget thing = findInteractTarget(ecs(), sim->combat(), target);
		test::require(thing.kind != InteractKind::None, "no such interact target");
		std::vector<state::GroundPoint> points;
		std::vector<double> heights;
		bool approach = false;
		if (thing.distanceTo(a.x, a.z) > kInteractReach)
		{
			const Route route = routeToInteract(*nav, a, thing, action, false, sim->pathRadius(a));
			test::require(route.found, "no route to the interact target");
			points = route.points;
			heights = route.heights;
			approach = route.approach;
		}
		sim->enqueue(Command::fighterInteract(state::Id{who}, target, action, ++seq, std::move(points), std::move(heights), approach));
	}
	// Ticks until `done` or `limit`; returns the ticks run.
	template <typename Pred>
	int until(Pred done, int limit)
	{
		int n = 0;
		while (n < limit && !done())
		{
			sim->tick();
			++n;
		}
		return n;
	}
	// A long walkable run from the player: the farthest of a few bearings that routes.
	Route farRoute(std::uint64_t who, double minLength)
	{
		const state::Fighter& a = f(who);
		Route best;
		double bestLength = 0.0;
		for (int k = 0; k < 16; ++k)
		{
			const double bearing = k * 0.39269908;
			for (const double distance : {900.0, 600.0, 400.0})
			{
				const Route route = nav->routeFrom({a.x, a.y, a.z}, a.x + std::sin(bearing) * distance, a.z + std::cos(bearing) * distance);
				if (!route.found || route.approach || route.points.size() < 2)
					continue;
				double length = 0.0;
				for (std::size_t i = 1; i < route.points.size(); ++i)
					length += std::hypot(route.points[i].x - route.points[i - 1].x, route.points[i].z - route.points[i - 1].z);
				if (length > bestLength)
				{
					bestLength = length;
					best = route;
				}
				break;
			}
		}
		test::require(bestLength >= minLength, "no long enough walkable run for the speed test: " + std::to_string(bestLength));
		return best;
	}
	void walk(std::uint64_t who, const Route& route)
	{
		sim->enqueue(Command::fighterMoveTo(state::Id{who}, route.points, route.heights, ++seq));
	}
	// The player with an empty pack and this strength (the doc's example: Strength 30, nothing carried).
	void emptyPack(std::uint64_t who, double strength)
	{
		f(who).skills.strength = strength;
		entityOf(who).set<state::Inventory>(state::Inventory{});
	}
};

double dist(const state::Fighter& a, const state::Fighter& b)
{
	return std::hypot(a.x - b.x, a.z - b.z);
}

// A run whose state hash is compared across a save: lift the servant, walk away with it.
void liftAndWalk(Rig& rig, int ticks)
{
	rig.knockOut(rig.duel.enemy);
	rig.interact(rig.duel.player, rig.duel.enemy, kCarry);
	rig.tick(2);
	// 30 ticks after the lift: carrying, then a walk.
	rig.until([&] { return rig.carryOf(rig.duel.player) != nullptr && rig.carryOf(rig.duel.player)->phase == 1; }, 400);
	rig.walk(rig.duel.player, rig.farRoute(rig.duel.player, 150.0));
	rig.tick(ticks);
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 3, "usage: ksim_carry HUB.knav STACK.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);

		// ---- the pure pieces: the constants come from the data, the encumbrance formula is the doc's example ----
		{
			const CombatConstants& k = data->k;
			test::require(k.carryPersonWeight == 30.0 && std::fabs(k.carryWeightMult - 1.2) < 1e-6 && k.encumbranceBase == 15.0,
				"the carry constants are not the data's 30 / 1.2 / 15: " + std::to_string(k.carryPersonWeight));
			// Strength 30, empty inventory: capacity 51, W = 30 -> (51 - 1.5) / 28.5 > 1 -> 1. An inventory of 70 + the person: 0.48.
			const double capacity = carryCapacity(30.0, k.carryWeightMult, k.encumbranceBase);
			test::require(std::fabs(capacity - 51.0) < 1e-5, "capacity of strength 30"); // the data's 1.2 is a float
			test::require(encumbranceRatio(capacity, 30.0) == 1.0, "strength 30 carrying a person is not unencumbered");
			test::require(std::fabs(encumbranceRatio(capacity, 100.0) - (51.0 - 5.0) / 95.0) < 1e-6, "ratio at W 100 (doc: 0.48)");
			test::require(encumbranceRatio(capacity, 0.0) == 1.0 && encumbranceRatio(0.0, 100.0) == 0.0, "ratio edge cases");
			// The run speed: (base - 11) x ratio + 11, and ratio 1 is the unladen speed exactly.
			knav::Gait gait;
			gait.athletics = 50.0;
			const double unladen = knav::runSpeed(gait);
			gait.encumbrance = 1.0;
			test::require(knav::runSpeed(gait) == unladen, "ratio 1 changes the run speed");
			gait.encumbrance = 0.48;
			test::require(std::fabs(knav::runSpeed(gait) - ((unladen - 11.0) * 0.48 + 11.0)) < 1e-12, "the encumbered run speed formula");
			// Sneaking caps it by stealth (kinspect sprint_anim.md): a human at run 70 crouch-walks at 23 below stealth 10, passes the
			// ninjarun's 30 at stealth ~22, its 45 at ~47, and runs at full speed from stealth 90.
			knav::Gait sneaker;
			sneaker.athletics = 1.0;
			const double run = knav::runSpeed(sneaker);
			const auto sneakAt = [&](double stealth) { sneaker.sneak = stealth * 0.01; return knav::runSpeed(sneaker); };
			test::require(std::fabs(sneakAt(5.0) - 23.0) < 1e-9 && std::fabs(sneakAt(22.0) - 30.05) < 0.01 && std::fabs(sneakAt(47.0) - 44.74) < 0.01,
				"the sneak speed formula");
			test::require(sneakAt(90.0) == run && sneakAt(100.0) == run, "a sneaker never outruns the run speed");
			test::require(forcedKoSeconds(30.0, 4.0, 1.0, 0.0, 99.0, 0.0) == kForcedKoMinSeconds || forcedKoSeconds(30.0, 4.0, 1.0, 0.0, 99.0, 0.0) > 3.0,
				"forced KO is never under 3 s");
			test::require(forcedKoSeconds(0.0, 4.0, 1.0, 0.0, 99.0, 0.0) == kForcedKoMinSeconds, "forced KO floor");
		}

		// ---- lock levels: the building's faction, merged data (item 20 b) ----
		{
			Rig rig(gd, data, nav, order);
			std::size_t withOwner = 0;
			for (const DoorSite& site : rig.sim->world().doorSites())
				withOwner += !site.ownerFaction.empty();
			// Ruins have no doors (7d6ac14, as the original; user-checked on the Hub 2026-11-04): 991 sites, was 1000+.
			test::require(withOwner > 950, "the zone files' owner faction IDs did not reach the door sites: " + std::to_string(withOwner));
			const kdata::Record* outlaws = gd->find("42022-rebirth.mod");
			test::require(outlaws != nullptr && outlaws->name == "Holy Nation Outlaws"
					&& outlaws->intOr("doors lock level", 0) == 30 && outlaws->intOr("lock level random", 0) == 15,
				"the merged data's Holy Nation Outlaws is not 30 / 15");
			std::size_t storm = 0;
			std::size_t shack = 0;
			std::size_t other = 0;
			rig.ecs().each([&](const state::Id&, const state::Door& door)
			{
				const kdata::Record* record = gd->find(door.recordSid);
				if (record == nullptr)
					return;
				// Every door's level is inside its own building faction's range (merged data), with the door's bonus.
				const kdata::Record* owner = gd->find(door.ownerSid);
				const int base = owner != nullptr ? owner->intOr("doors lock level", 40) : 40;
				const int random = owner != nullptr ? owner->intOr("lock level random", 10) : 10;
				const int bonus = record->intOr("lock level bonus", 0);
				test::require(door.lockLevel >= std::clamp(base - random + bonus, 1, 100) && door.lockLevel <= std::clamp(base + random + bonus, 1, 100),
					"door " + record->name + " owned by " + door.ownerSid + " has lock level " + std::to_string(door.lockLevel));
				if (door.ownerSid != "42022-rebirth.mod")
				{
					++other;
					return;
				}
				// MEASURED (GATES_CARRY_LIVE.md b): the Hub's Holy Nation Outlaws, storm house doors (bonus 0) 15..45, the shack door (-10) 5..35.
				if (record->name == "Storm House Door A" || record->name == "Storm House Door B" || record->name == "Storm House Door")
				{
					++storm;
					test::require(door.lockLevel >= 15 && door.lockLevel <= 45, "storm door lock level " + std::to_string(door.lockLevel) + " outside 15..45");
				}
				if (record->name == "Storm Shack Door")
				{
					++shack;
					test::require(door.lockLevel >= 5 && door.lockLevel <= 35, "shack door lock level " + std::to_string(door.lockLevel) + " outside 5..35");
				}
			});
			// The Hub's storm houses are ruins with no doors since 7d6ac14 (as live): its Outlaw doors now are one shack's.
			test::require(storm + shack > 0, "no Outlaw storm house or shack doors in the Hub scenario: " + std::to_string(storm) + " / " + std::to_string(shack));
			std::cout << "lock levels: " << storm << " Outlaw storm doors in 15..45, " << shack << " shack doors in 5..35 (Holy Nation Outlaws 30 / 15), " << other << " doors of other owners in their own ranges\n";
		}

		// ---- lift, carry, put down ----
		{
			Rig rig(gd, data, nav, order);
			const std::uint64_t player = rig.duel.player;
			const std::uint64_t servant = rig.duel.enemy;
			rig.knockOut(servant);
			// Not liftable while it stands, or by itself.
			test::require(checkInteract(rig.ecs(), *rig.sim->combat(), player, findInteractTarget(rig.ecs(), rig.sim->combat(), player), kCarry) == "bad_target",
				"lifting yourself is allowed");
			rig.interact(player, servant, kCarry);
			// Walks up (run), then the lift starts.
			const int walked = rig.until([&] { return rig.carryOf(player) != nullptr; }, 600);
			test::require(rig.carryOf(player) != nullptr, "the lift never started");
			std::cout << "lift: walked up in " << walked << " ticks\n";
			test::require(rig.f(player).moveMode != 0, "the walk to the body is not a run");
			// The lift: attach at 40 % of the clip, done at 95 % (2.343 game-s). The tick that created the step counted already.
			int attachTick = -1;
			int doneTick = -1;
			for (int t = 0; t < 60 && doneTick < 0; ++t)
			{
				if (attachTick < 0 && rig.f(servant).carriedBy == player)
					attachTick = t;
				if (rig.doneEvent(player, kCarry))
					doneTick = t;
				if (doneTick < 0)
					rig.tick();
			}
			const double attachSeconds = (attachTick + 1) * kTickSeconds;
			const double doneSeconds = (doneTick + 1) * kTickSeconds;
			test::require(attachTick >= 0 && doneTick >= 0, "the lift did not attach and finish");
			test::require(std::fabs(attachSeconds - kLiftAttachSeconds) <= kTickSeconds + 1e-9, "the body attached at " + std::to_string(attachSeconds) + " s, not 0.49");
			test::require(std::fabs(doneSeconds - kLiftSeconds) <= kTickSeconds + 1e-9, "the lift took " + std::to_string(doneSeconds) + " s, not 1.17");
			test::require(rig.doneEvent(player, kCarry)->result == InteractEvent::kDone && rig.doneEvent(player, kCarry)->target == servant, "event 13 for the lift");
			test::require(rig.carryOf(player)->phase == 1 && rig.f(player).carrying == servant && rig.f(servant).carriedBy == player, "not carrying after the lift");
			std::cout << "lift: attached at " << attachSeconds << " s, done at " << doneSeconds << " s\n";
			test::require(!rig.entityOf(player).has<state::InteractTask>(), "the lift's interact task is still there");
			// Not liftable again, nor a second body.
			test::require(checkInteract(rig.ecs(), *rig.sim->combat(), player, findInteractTarget(rig.ecs(), rig.sim->combat(), servant), kCarry) == "not_allowed",
				"a carried body can be lifted again");

			// The carried moves with the carrier: no movement of its own, and nobody targets it.
			rig.emptyPack(player, 30.0); // the doc's example: unladen-equal speed at ratio 1
			const state::Fighter& rider = rig.f(servant);
			const Route route = rig.farRoute(player, 500.0);
			const double startX = rig.f(player).x;
			const double startZ = rig.f(player).z;
			rig.walk(player, route);
			double worst = 0.0;
			double topSpeed = 0.0;
			for (int t = 0; t < 400 && rig.f(player).walking() + (t < 2) > 0; ++t)
			{
				rig.tick();
				worst = std::max(worst, dist(rig.f(player), rider));
				topSpeed = std::max(topSpeed, std::fabs(rig.f(player).forwardSpeed));
				test::require(rider.carriedBy == player, "the carried was released while walking");
			}
			test::require(std::hypot(rig.f(player).x - startX, rig.f(player).z - startZ) > 300.0, "the carrier did not walk");
			test::require(worst < 1e-9, "the carried is " + std::to_string(worst) + " units from its carrier");
			// Strength 30, empty pack: ratio exactly 1, the run speed is the unladen one.
			test::require(rig.f(player).encumbrance == 1.0, "ratio at strength 30: " + std::to_string(rig.f(player).encumbrance));
			knav::Gait gait;
			const RaceCombat* race = rig.data->race(rig.f(player).raceSid);
			gait.speedMinSkill = race->speedMinSkill;
			gait.speedMaxSkill = race->speedMaxSkill;
			gait.walkSpeed = race->walkSpeed;
			gait.athletics = rig.f(player).skills.athletics * CombatRules::effectiveStat(rig.f(player), CombatRules::kStatAthletics);
			test::require(std::fabs(topSpeed - knav::runSpeed(gait)) < 1e-6, "carrying at ratio 1 runs at " + std::to_string(topSpeed) + ", unladen is " + std::to_string(knav::runSpeed(gait)));
			std::cout << "carry: follows the carrier to 1e-9, run speed " << topSpeed << " = unladen at ratio 1\n";

			// Strength 10 with the person: capacity 27, W 30 -> ratio (27 - 1.5) / 28.5.
			rig.emptyPack(player, 10.0);
			rig.tick();
			const double strength = rig.f(player).skills.strength * CombatRules::effectiveStat(rig.f(player), CombatRules::kStatStrength);
			const double expectedRatio = (rig.data->k.carryWeightMult * strength + 15.0 - 1.5) / 28.5;
			test::require(expectedRatio < 1.0 && std::fabs(rig.f(player).encumbrance - expectedRatio) < 1e-9, "encumbrance at strength 10: " + std::to_string(rig.f(player).encumbrance) + " vs " + std::to_string(expectedRatio));
			const Route back = rig.farRoute(player, 400.0);
			rig.walk(player, back);
			double slow = 0.0;
			for (int t = 0; t < 400 && (rig.f(player).walking() || t < 2); ++t)
			{
				rig.tick();
				slow = std::max(slow, std::fabs(rig.f(player).forwardSpeed));
			}
			const double expectedSlow = (knav::runSpeed(gait) - 11.0) * expectedRatio + 11.0;
			test::require(std::fabs(slow - expectedSlow) < 1e-6 && slow < topSpeed, "encumbered run speed " + std::to_string(slow) + " vs " + std::to_string(expectedSlow));
			std::cout << "encumbrance: strength 10 ratio " << expectedRatio << ", run speed " << slow << " (unladen " << topSpeed << ")\n";

			// Put down: 1.00 s, then it lies where the carrier stands, KO'd for at least 3 s (a non-ally).
			rig.f(servant).koTimer = 1.0; // shorter than the forced timer: only the put-down can keep it down
			rig.f(player).clearRoute();
			rig.tick(2);
			const std::size_t eventsBefore = rig.events.size();
			rig.interact(player, servant, kPutDown);
			rig.until([&] { return rig.carryOf(player) != nullptr && rig.carryOf(player)->phase == 2; }, 10);
			test::require(rig.carryOf(player) != nullptr && rig.carryOf(player)->phase == 2, "the put-down did not start");
			int putTicks = 0;
			while (rig.f(servant).carriedBy != 0 && putTicks < 60)
			{
				rig.tick();
				++putTicks;
			}
			const double putSeconds = (putTicks + 1) * kTickSeconds;
			test::require(std::fabs(putSeconds - kPutDownSeconds) <= kTickSeconds + 1e-9, "the put-down took " + std::to_string(putSeconds) + " s, not 1.00");
			test::require(rig.f(player).carrying == 0 && rig.carryOf(player) == nullptr && rig.f(player).encumbrance == 1.0, "the carrier is still marked as carrying");
			test::require(dist(rig.f(player), rig.f(servant)) <= CombatArena::kSlideCap + 1e-9, "the put-down body is not at the carrier: " + std::to_string(dist(rig.f(player), rig.f(servant))));
			test::require(rig.f(servant).unconscious && rig.f(servant).koTimer >= kForcedKoMinSeconds, "no forced KO timer of 3 s for a non-ally: " + std::to_string(rig.f(servant).koTimer));
			const auto putDone = rig.doneEvent(player, kPutDown, eventsBefore);
			test::require(putDone && putDone->result == InteractEvent::kDone && putDone->target == servant, "event 13 for the put-down");
			std::cout << "put down: " << putSeconds << " s, forced KO " << rig.f(servant).koTimer << " s\n";
			// It stays down for that long: 2.9 s later it is still KO'd (its own timer was 1 s).
			rig.tick(static_cast<int>(2.9 / kTickSeconds));
			test::require(rig.f(servant).unconscious, "the put-down person woke before the forced timer ran out");

			// An ally gets no forced timer.
			rig.knockOut(servant, 600.0);
			rig.interact(player, servant, kCarry);
			rig.until([&] { return rig.carryOf(player) != nullptr && rig.carryOf(player)->phase == 1; }, 400);
			test::require(rig.carryOf(player) != nullptr && rig.f(servant).carriedBy == player, "the second lift failed");
			// An ally: the same faction (sensor::ally / Simulation::alliedTo, task BV). The team no longer means an ally - it is the
			// combat grouping, and every faction neutral to the player shares it.
			rig.entityOf(servant).set<state::Allegiance>(*rig.entityOf(player).get<state::Allegiance>());
			rig.f(servant).koTimer = 1.0;
			rig.interact(player, servant, kPutDown);
			rig.until([&] { return rig.f(servant).carriedBy == 0; }, 100);
			test::require(rig.f(servant).carriedBy == 0 && rig.f(servant).koTimer < kForcedKoMinSeconds, "an ally got a forced KO timer: " + std::to_string(rig.f(servant).koTimer));
		}

		// ---- a carrier that is KO'd or killed drops its person at once ----
		for (const bool kill : {false, true})
		{
			Rig rig(gd, data, nav, order);
			const std::uint64_t player = rig.duel.player;
			const std::uint64_t servant = rig.duel.enemy;
			rig.knockOut(servant);
			rig.interact(player, servant, kCarry);
			rig.until([&] { return rig.carryOf(player) != nullptr && rig.carryOf(player)->phase == 1; }, 600);
			test::require(rig.f(servant).carriedBy == player, "not carrying");
			rig.walk(player, rig.farRoute(player, 150.0));
			rig.tick(20);
			if (kill)
			{
				rig.f(player).dead = true;
				rig.f(player).unconscious = true;
			} else {
				rig.knockOut(player);
			}
			rig.tick(1);
			test::require(rig.f(servant).carriedBy == 0 && rig.f(player).carrying == 0 && rig.carryOf(player) == nullptr,
				std::string("a ") + (kill ? "dead" : "KO'd") + " carrier still holds its person");
			// Beside the carrier, not on it: the carrier is a body now, and the put-down takes a free spot (task AW, the body layer;
			// this assertion pinned the exact carrier point before it).
			test::require(dist(rig.f(player), rig.f(servant)) >= CombatArena::kBodySpacing && rig.f(servant).unconscious,
				"the dropped person lies on the carrier (" + std::to_string(dist(rig.f(player), rig.f(servant))) + " apart)");
			// The mirror in flecs agrees with the arena.
			test::require(rig.entityOf(servant).get<state::Fighter>()->carriedBy == 0 && rig.entityOf(player).get<state::Fighter>()->carrying == 0,
				"the flecs mirror still shows the carry");
			// The lift in progress is cut off too (the order ends with 4 when the actor is down).
			std::cout << "carrier " << (kill ? "killed" : "KO'd") << ": the person is dropped at once\n";
		}

		// ---- save + load mid-carry gives the uninterrupted hash ----
		{
			Rig whole(gd, data, nav, order);
			liftAndWalk(whole, 40);
			whole.tick(60);
			Rig split(gd, data, nav, order);
			liftAndWalk(split, 40);
			test::require(split.f(split.duel.player).carrying == split.duel.enemy && split.f(split.duel.player).walking(), "the save point is not mid-carry, mid-walk");
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_carry.save";
			split.sim->saveGame(path);
			Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path);
			CombatSettings settings;
			settings.substeps = 2;
			loaded.enableCombat(data, settings);
			loaded.enableNavigation(nav);
			// portraitSerial survives the save, and the same seed gives the same serials.
			for (const std::uint64_t id : {split.duel.player, split.duel.enemy})
			{
				test::require(loaded.combat()->find(id)->portraitSerial == split.f(id).portraitSerial, "portraitSerial lost in save + load");
				test::require(split.f(id).portraitSerial == whole.f(id).portraitSerial, "the same seed gave different portrait serials");
				test::require(split.f(id).portraitSerial < 4000000000u, "portraitSerial outside [0, 4e9)");
			}
			test::require(split.f(split.duel.player).portraitSerial != split.f(split.duel.enemy).portraitSerial, "two characters rolled the same serial (a 1 in 4e9 fluke or a broken stream)");
			loaded.run(60);
			test::require(loaded.stateHash() == whole.sim->stateHash(), "save mid-carry + load + run differs from the uninterrupted run");
			std::remove(path.c_str());
			std::cout << "save / load mid-carry continues identically\n";
		}

		// ---- building membership, and an NPC resident taking a KO'd intruder out ----
		{
			Rig rig(gd, data, nav, order);
			// A Hub door of an owned building with room inside: both inside points snap to the mesh and the way out routes.
			struct Pick
			{
				state::Door door;
				std::uint64_t id = 0;
				knav::Position residentAt{}, intruderAt{}, drop{};
			};
			std::optional<Pick> pick;
			std::vector<std::pair<std::uint64_t, state::Door>> doors;
			rig.ecs().each([&](const state::Id& i, const state::Door& d)
			{
				if (d.building != 0 && !d.ownerSid.empty() && !d.gate && std::hypot(d.inX, d.inZ) > 0.5)
					doors.push_back({i.value, d});
			});
			std::sort(doors.begin(), doors.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			for (const auto& [id, door] : doors)
			{
				// A spot `along` units on the way in (+) or out (-) of the door, on the mesh, near the door's height.
				const auto spot = [&](double along) -> std::optional<knav::Position>
				{
					const double x = door.x + door.inX * along;
					const double z = door.z + door.inZ * along;
					const auto s = nav->walker().snap({x, door.y, z}, 40.0);
					if (!s || std::hypot(s->x - x, s->z - z) > 4.0 || std::fabs(s->y - door.y) > 6.0)
						return std::nullopt;
					return s;
				};
				const auto resident = spot(6.0);
				const auto intruder = spot(13.0);
				const auto drop = spot(-kDropBeyondDoor);
				if (!resident || !intruder || !drop)
					continue;
				const Route in = nav->routeFrom(*resident, intruder->x, intruder->z, intruder->y, true);
				const Route out = nav->routeFrom(*intruder, drop->x, drop->z, drop->y, true);
				if (!in.found || !out.found || out.approach || in.approach)
					continue;
				pick = Pick{door, id, *resident, *intruder, *drop};
				break;
			}
			test::require(pick.has_value(), "no Hub door with an inside to take an intruder out of (" + std::to_string(doors.size()) + " candidates)");
			std::cout << "take-out: door " << pick->id << " (" << gd->find(pick->door.recordSid)->name << "), building " << pick->door.building
					  << ", " << doors.size() << " candidate doors\n";

			// The resident of the building: a servant body of the door's faction on the player's team, inside. The intruder is the
			// duel's servant (team 1, of another faction), KO'd, lying further in. The door is locked: the resident's faction owns it.
			const std::uint64_t intruder = rig.duel.enemy;
			state::Fighter body = rig.f(intruder);
			body.team = 0;
			body.unconscious = false;
			body.koTimer = 0.0;
			body.target = 0;
			body.x = pick->residentAt.x;
			body.y = pick->residentAt.y;
			body.z = pick->residentAt.z;
			body.building = pick->door.building;
			flecs::entity residentEntity = rig.sim->spawnFighter(std::move(body));
			residentEntity.set<state::Allegiance>(state::Allegiance{pick->door.ownerSid});
			const std::uint64_t resident = residentEntity.get<state::Id>()->value;
			// Task CH: the 223 resolver reads the sensor record's ally bit, which comes from the Arena faction Id (sensor::ally), not
			// the Allegiance SID. The resident body is a copy of the intruder's and would share its faction Id (same faction = ally, a
			// fixture artifact); the intruder belongs to a faction this world does not know, so the relation is 0 (neutral), never an ally.
			rig.f(intruder).faction = unknownFaction();
			rig.f(intruder).x = pick->intruderAt.x;
			rig.f(intruder).y = pick->intruderAt.y;
			rig.f(intruder).z = pick->intruderAt.z;
			rig.f(intruder).building = pick->door.building;
			rig.knockOut(intruder, 5000.0);
			flecs::entity doorEntity;
			rig.ecs().each([&](flecs::entity e, const state::Id& i, const state::Door&) { if (i.value == pick->id) doorEntity = e; });
			doorEntity.get_mut<state::Door>()->state = 2;
			const auto doorState = [&] { return doorEntity.get<state::Door>()->state; };
			test::require(doorState() == 2, "the door is not locked");
			const double doorX = pick->door.x;
			const double doorZ = pick->door.z;

			bool carried = false;
			bool sawOutside = false;
			std::unique_ptr<Simulation> loaded; // the take-out saved mid-carry, loaded back (task CH, save v83)
			const std::string takeOutSave = std::string(KSIM_BINARY_DIR) + "/ksim_carry_takeout.save";
			int ticks = 0;
			for (; ticks < 6000; ++ticks)
			{
				rig.tick();
				carried = carried || rig.f(intruder).carriedBy == resident;
				if (carried && loaded == nullptr)
				{
					// Saved with the body in the arms and the 223/224 plan under way: the loaded copy must finish the same way.
					rig.sim->saveGame(takeOutSave);
					loaded = std::make_unique<Simulation>(Simulation::loadGame(KENSHI_DATA, order, takeOutSave));
					CombatSettings settings;
					settings.substeps = 2;
					loaded->enableCombat(data, settings);
					loaded->enableNavigation(nav);
				}
				if (carried && rig.f(resident).building == 0)
					sawOutside = true;
				if (carried && !rig.entityOf(resident).has<state::TakeOut>() && rig.f(intruder).carriedBy == 0)
					break;
			}
			test::require(carried, "the resident never lifted the intruder");
			test::require(ticks < 6000, "the take-out did not finish in 300 s");
			std::cout << "take-out: done in " << ticks * kTickSeconds << " game-s\n";
			// The person lies at the drop point, outside, still KO'd, and the door is as the resident left it: open.
			const state::Fighter& lying = rig.f(intruder);
			const double toDrop = std::hypot(lying.x - pick->drop.x, lying.z - pick->drop.z);
			test::require(toDrop <= 6.0, "the intruder lies " + std::to_string(toDrop) + " units from the drop point");
			test::require(std::hypot(lying.x - doorX, lying.z - doorZ) > 30.0, "the intruder was not carried out through the door: " + std::to_string(std::hypot(lying.x - doorX, lying.z - doorZ))
				+ " from it, resident " + std::to_string(std::hypot(rig.f(resident).x - doorX, rig.f(resident).z - doorZ)) + " from it, building " + std::to_string(rig.f(resident).building)
				+ " door state " + std::to_string(doorState()) + " drop " + std::to_string(std::hypot(pick->drop.x - doorX, pick->drop.z - doorZ)));
			test::require(lying.unconscious && lying.building == 0 && lying.carriedBy == 0, "the dropped intruder's state");
			test::require(sawOutside && rig.f(resident).building == 0 && rig.f(resident).carrying == 0, "the resident's building after the take-out");
			test::require(doorState() == 0, "the door was not left open by the resident (state " + std::to_string(doorState()) + ")");
			test::require(rig.f(resident).moveMode != 0, "the resident's gait was not restored");
			// No forced KO timer on this drop (the put-down task is not used): the timer ran down from 5000 normally.
			test::require(lying.koTimer > 4000.0, "the take-out drop started a forced KO timer");
			std::cout << "take-out: intruder " << toDrop << " units from the drop point, door open\n";
			if (loaded != nullptr)
			{
				// The loaded take-out finishes on its own: the same rules apply from the restored plan / step (save v84).
				bool out = false;
				bool done = false;
				for (int t = 0; t < 4000 && !done; ++t)
				{
					loaded->run(1);
					const state::Fighter* body = loaded->combat()->find(intruder);
					const state::Fighter* carrier = loaded->combat()->find(resident);
					if (body == nullptr || carrier == nullptr)
						break;
					out = out || (body->carriedBy == resident && carrier->building == 0);
					done = body->carriedBy == 0 && body->building == 0;
				}
				const state::Fighter* body = loaded->combat()->find(intruder);
				test::require(out && done && body != nullptr && body->building == 0 && body->carriedBy == 0 && body->unconscious,
					"a take-out saved mid-carry and loaded did not carry the body out");
				std::remove(takeOutSave.c_str());
				std::cout << "take-out: save / load mid-take-out carries on and ends outside\n";
			}

			// The resident walks back in: crossing the door sets its building again.
			const Route back = nav->routeFrom({rig.f(resident).x, rig.f(resident).y, rig.f(resident).z}, pick->residentAt.x, pick->residentAt.z,
				pick->residentAt.y, true);
			test::require(back.found, "no way back in");
			rig.walk(resident, back);
			rig.tick(2);
			rig.until([&] { return !rig.f(resident).walking() && !rig.entityOf(resident).has<state::DoorStep>(); }, 2000);
			test::require(rig.f(resident).building == pick->door.building, "walking in through the door did not set the building");
		}

		// ---- H1: a lift refused mid-way ends with event 13 for the right target (the task is read before the structural change) ----
		{
			Rig rig(gd, data, nav, order);
			const std::uint64_t player = rig.duel.player;
			const std::uint64_t servant = rig.duel.enemy;
			rig.knockOut(servant);
			rig.interact(player, servant, kCarry);
			rig.until([&] { return rig.carryOf(player) != nullptr; }, 600);
			test::require(rig.carryOf(player) != nullptr, "the lift never started");
			rig.tick(3);
			test::require(rig.f(servant).carriedBy == 0 && rig.entityOf(player).has<state::InteractTask>(), "the lift attached too early for the test");
			// The body wakes before the attach: the lift is refused.
			rig.f(servant).unconscious = false;
			rig.f(servant).koTimer = 0.0;
			rig.tick(2);
			const auto refused = rig.doneEvent(player, kCarry);
			test::require(refused && refused->result == InteractEvent::kRefused && refused->target == servant, "a refused lift did not end with event 13 kRefused for its target");
			test::require(rig.carryOf(player) == nullptr && !rig.entityOf(player).has<state::InteractTask>(), "the refused lift left its state behind");
			test::require(rig.f(player).carrying == 0 && rig.f(servant).carriedBy == 0, "the refused lift left a carry link");
			std::cout << "lift refused mid-way: event 13 names the target, state cleared\n";
		}

		// ---- H2: a town gate is not a building ----
		{
			const std::shared_ptr<MeshNavigator> stack = MeshNavigator::load(argv[2]);
			Rig rig(gd, data, stack, order, false);
			std::size_t gates = 0;
			std::vector<state::Door> crossable;
			std::vector<std::uint64_t> gateIds;
			const kdata::Record* servant = nullptr;
			for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
				if (r->name == "Escaped Servant")
					servant = r;
			test::require(servant != nullptr, "no Escaped Servant in the data");
			rig.ecs().each([&](const state::Id& i, const state::Door& door)
			{
				if (!door.gate)
					return;
				++gates;
				gateIds.push_back(i.value);
				test::require(door.inX == 0.0 && door.inZ == 0.0, "a town gate has a way in (building membership)");
				crossable.push_back(door);
			});
			test::require(gates > 0, "no gate doors on the Stack mesh");
			// A character walking through a gate (every gate with ground on both sides) keeps building 0.
			std::size_t walked = 0;
			for (const state::Door& gate : crossable)
			{
				const double nx = -gate.axisZ;
				const double nz = gate.axisX;
				const auto a = stack->walker().snap({gate.x + nx * 24.0, gate.y, gate.z + nz * 24.0}, 40.0);
				const auto b = stack->walker().snap({gate.x - nx * 24.0, gate.y, gate.z - nz * 24.0}, 40.0);
				if (!a || !b || std::fabs(a->y - gate.y) > 6.0 || std::fabs(b->y - gate.y) > 6.0)
					continue;
				const Route route = stack->routeFrom(*a, b->x, b->z, b->y, true);
				if (!route.found || route.approach)
					continue;
				std::uint64_t rng = 12345;
				state::Fighter body = data->makeFighter(*gd, servant->stringID, "", rng); // the duel's servant
				body.team = 0;
				body.unconscious = false;
				body.koTimer = 0.0;
				body.target = 0;
				body.x = a->x;
				body.y = a->y;
				body.z = a->z;
				body.building = 0;
				const std::uint64_t walker = rig.sim->spawnFighter(std::move(body)).get<state::Id>()->value;
				test::require(rig.f(walker).building == 0, "a character at a gate spawned in a building");
				rig.walk(walker, route);
				rig.tick(2);
				for (int t = 0; t < 1500 && (rig.f(walker).walking() || rig.entityOf(walker).has<state::DoorStep>()); ++t)
				{
					rig.tick();
					test::require(rig.f(walker).building == 0, "crossing a town gate gave the character a building");
				}
				++walked;
				break;
			}
			std::cout << "gates: " << gates << " doors without a way in, " << walked << " crossed with building 0\n";

			// A lockpick order on a locked gate, from off to one side of it (moved from ksim_interact: the Hub's gates are ruins). The walk
			// used to end at the edge of the shut slab, 12.4 units from its centre (> kInteractReach), and the order failed with event 13
			// result 2 "unreachable".
			bool beyondCentre = false;
			std::size_t picked = 0;
			for (const std::uint64_t id : gateIds)
			{
				flecs::entity e;
				rig.ecs().each([&](flecs::entity entity, const state::Id& i, const state::Door&) { if (i.value == id) e = entity; });
				state::Door* gate = e.get_mut<state::Door>();
				const double nx = -gate->axisZ;
				const double nz = gate->axisX;
				for (const double side : {1.0, -1.0})
				{
					const double along = gate->halfLong * 0.8;
					const auto start = stack->walker().snap({gate->x + nx * 40.0 * side + gate->axisX * along, gate->y,
						gate->z + nz * 40.0 * side + gate->axisZ * along}, 40.0);
					if (!start || std::fabs(start->y - gate->y) > 6.0
						|| !stack->routeFrom(*start, gate->x, gate->z, gate->y).found)
						continue;
					std::uint64_t rng = 12345;
					state::Fighter body = data->makeFighter(*gd, servant->stringID, "", rng);
					body.team = 0;
					body.x = start->x;
					body.y = start->y;
					body.z = start->z;
					const std::uint64_t picker = rig.sim->spawnFighter(std::move(body)).get<state::Id>()->value;
					gate->state = 2; // locked by hand: gates start open
					++gate->version;
					test::require(gate->halfLong > gate->halfDeep && gate->halfDeep > 0.0, "the gate is not a locked slab with an extent");
					gate->ownerSid = "someone-else";
					gate->lockLevel = 1; // chance 0.9
					rig.tick();
					const std::size_t from = rig.events.size();
					rig.interact(picker, id, 3);
					rig.until([&] { return rig.doneEvent(picker, 3, from).has_value(); }, 4000);
					const auto result = rig.doneEvent(picker, 3, from);
					const state::Fighter& f = rig.f(picker);
					const InteractTarget slab = findInteractTarget(rig.ecs(), nullptr, id);
					std::cout << "gate lockpick: door " << id << " side " << side << " result " << (result ? result->result : -1) << ", start ("
						<< start->x << ", " << start->z << "), the walk ended (" << f.x << ", " << f.z << ") " << std::hypot(f.x - gate->x, f.z - gate->z)
						<< " from the centre, " << slab.distanceTo(f.x, f.z) << " from the slab (half " << gate->halfLong << " x " << gate->halfDeep << ")\n";
					test::require(result && result->result == 0 && gate->state == 1,
						"the lockpick order on a gate ended with event 13 result " + std::to_string(result ? result->result : -1));
					test::require(slab.distanceTo(f.x, f.z) <= kInteractReach, "the gate order ended out of reach of the slab");
					beyondCentre = beyondCentre || std::hypot(f.x - gate->x, f.z - gate->z) > kInteractReach;
					++picked;
					rig.f(picker).dead = true; // out of the way of the next
					gate->state = 0;
					++gate->version;
				}
			}
			test::require(picked > 0, "no gate on the Stack mesh to pick from either side");
			test::require(beyondCentre, "no gate walk ended beyond kInteractReach of the centre: the regression is not exercised");
		}

		// ---- H3: a resident that spawns inside a building is in it, sees the intruder and takes it out ----
		{
			struct Pick
			{
				state::Door door;
				knav::Position residentAt{}, intruderAt{};
			};
			std::optional<Pick> pick;
			std::size_t candidates = 0;
			std::size_t placed = 0;
			{
				Rig rig(gd, data, nav, order);
				std::vector<std::pair<std::uint64_t, state::Door>> doors;
				rig.ecs().each([&](const state::Id& i, const state::Door& d)
				{
					if (d.building != 0 && !d.ownerSid.empty() && !d.gate && std::hypot(d.inX, d.inZ) > 0.5)
						doors.push_back({i.value, d});
				});
				std::sort(doors.begin(), doors.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
				for (const auto& [id, door] : doors)
				{
					const auto spot = [&](double along) -> std::optional<knav::Position>
					{
						const double x = door.x + door.inX * along;
						const double z = door.z + door.inZ * along;
						const auto s = nav->walker().snap({x, door.y, z}, 40.0);
						if (!s || std::hypot(s->x - x, s->z - z) > 4.0 || std::fabs(s->y - door.y) > 6.0)
							return std::nullopt;
						return s;
					};
					const auto resident = spot(6.0);
					const auto intruder = spot(13.0);
					if (!resident || !intruder)
						continue;
					++candidates;
					if (rig.sim->buildingAt(resident->x, resident->y, resident->z) != door.building)
						continue;
					++placed;
					if (!pick && rig.sim->buildingAt(intruder->x, intruder->y, intruder->z) == door.building)
						pick = Pick{door, *resident, *intruder};
				}
			}
			std::cout << "spawn placement: " << placed << " of " << candidates << " Hub doors put a spawn 6 units in, in the door's building\n";
			test::require(pick.has_value(), "no Hub door whose inside is placed in its building (" + std::to_string(candidates) + " candidates)");
			test::require(placed * 2 >= candidates, "fewer than half of the doors' insides are inside their building footprint");

			// awake: the intruder stands in the building and the resident notices it; KO'd: the resident takes it out.
			for (const bool awake : {true, false})
			{
				Rig rig(gd, data, nav, order);
				const std::uint64_t victim = rig.duel.enemy;
				state::Fighter body = rig.f(victim);
				body.team = 0;
				body.unconscious = false;
				body.koTimer = 0.0;
				body.target = 0;
				body.x = pick->residentAt.x;
				body.y = pick->residentAt.y;
				body.z = pick->residentAt.z;
				body.building = 0; // as every spawn site leaves it
				flecs::entity residentEntity = rig.sim->spawnFighter(std::move(body));
				residentEntity.set<state::Allegiance>(state::Allegiance{pick->door.ownerSid});
				const std::uint64_t resident = residentEntity.get<state::Id>()->value;
				// Task CH: the 223 resolver reads the sensor record's ally bit (the Arena faction Id, sensor::ally); this resident body is
				// a copy of the intruder's and would share its (factionless) Id - same faction = ally. It belongs to a faction this world
				// does not know: neutral, never an ally.
				rig.f(resident).faction = unknownFaction();
				test::require(rig.f(resident).building == pick->door.building, "a resident spawned inside is not in its building");
				rig.f(victim).x = pick->intruderAt.x;
				rig.f(victim).y = pick->intruderAt.y;
				rig.f(victim).z = pick->intruderAt.z;
				rig.f(victim).building = 0;
				if (!awake)
					rig.knockOut(victim, 5000.0);
				rig.sim->placeInBuildings();
				test::require(rig.f(victim).building == pick->door.building, "placeInBuildings did not place the intruder");
				bool seen = false;
				bool lifted = false;
				for (int t = 0; t < 400 && !(awake ? seen : lifted); ++t)
				{
					rig.tick();
					for (const state::SightRecord& r : rig.f(resident).sight)
						seen = seen || (r.target == victim && r.seen);
					lifted = lifted || rig.f(victim).carriedBy == resident || rig.entityOf(resident).has<state::TakeOut>();
				}
				if (awake)
					test::require(seen, "a resident spawned inside never saw the intruder in its building");
				else
					test::require(lifted, "a resident spawned inside never went for the KO'd intruder in its building");
				std::cout << "spawned-inside resident: " << (awake ? "saw the intruder" : "took the KO'd intruder out") << "\n";
			}
		}

		// ---- H4: the Hub's Bar (BAR_FIGHT_LOOP 0d): its door is in the bake, and a guard of its faction carries a KO'd intruder out ----
		{
			Rig rig(gd, data, nav, order);
			// The live Bar door is at (-51253, 2664); the bake has it as the door within 20 units, with a way in.
			std::optional<std::pair<std::uint64_t, state::Door>> bar;
			rig.ecs().each([&](const state::Id& i, const state::Door& d)
			{
				if (d.building != 0 && !d.gate && !d.ownerSid.empty() && std::hypot(d.inX, d.inZ) > 0.5 && std::hypot(d.x + 51253.0, d.z - 2664.0) < 20.0)
					bar = std::make_pair(i.value, d);
			});
			test::require(bar.has_value(), "the Bar's door (-51253, 2664) is not in the Hub bake with a way in");
			const state::Door door = bar->second;
			std::cout << "bar: door " << bar->first << " building " << door.building << " owner " << door.ownerSid << "\n";
			const auto spot = [&](double along) -> std::optional<knav::Position>
			{
				const auto s = nav->walker().snap({door.x + door.inX * along, door.y, door.z + door.inZ * along}, 40.0);
				if (!s || std::fabs(s->y - door.y) > 6.0)
					return std::nullopt;
				return s;
			};
			const auto residentAt = spot(6.0);
			const auto intruderAt = spot(13.0);
			test::require(residentAt && intruderAt, "no mesh inside the Bar's door");
			{
				const auto drop = spot(-kDropBeyondDoor);
				std::cout << "bar: inside points (" << residentAt->x << "," << residentAt->z << ") (" << intruderAt->x << "," << intruderAt->z << "), door (" << door.x << "," << door.z
						  << ") in (" << door.inX << "," << door.inZ << ")";
				if (drop)
				{
					const Route out = nav->routeFrom(*intruderAt, drop->x, drop->z, drop->y, true);
					std::cout << ", drop (" << drop->x << "," << drop->y << "," << drop->z << ") route " << out.found << " approach " << out.approach << " points " << out.points.size();
				} else {
					std::cout << ", no drop spot";
				}
				std::cout << "\n";
				for (const double along : {6.0, -6.0, -20.0, -44.8, -60.0, -90.0, -140.0})
				{
					const double px = door.x + door.inX * along;
					const double pz = door.z + door.inZ * along;
					std::cout << "bar: " << along << " along: building at (" << px << "," << pz << ") = " << rig.sim->buildingAt(px, door.y, pz) << "\n";
				}
			}
			const std::uint64_t intruder = rig.duel.enemy;
			state::Fighter body = rig.f(intruder);
			body.team = 0;
			body.unconscious = false;
			body.koTimer = 0.0;
			body.target = 0;
			body.x = residentAt->x;
			body.y = residentAt->y;
			body.z = residentAt->z;
			body.building = door.building;
			flecs::entity guardEntity = rig.sim->spawnFighter(std::move(body));
			guardEntity.set<state::Allegiance>(state::Allegiance{door.ownerSid});
			const std::uint64_t guard = guardEntity.get<state::Id>()->value;
			// Task CH: the 223 resolver reads the sensor record's ally bit (the Arena faction Id, sensor::ally); the guard body is a
			// copy of the intruder's and would otherwise share its faction Id and count as an ally. The intruder belongs to a faction
			// this world does not know: relation 0 (neutral), never an ally.
			rig.f(intruder).faction = unknownFaction();
			rig.f(intruder).x = intruderAt->x;
			rig.f(intruder).y = intruderAt->y;
			rig.f(intruder).z = intruderAt->z;
			rig.f(intruder).building = door.building;
			rig.knockOut(intruder, 5000.0);
			bool carried = false;
			int ticks = 0;
			for (; ticks < 6000; ++ticks)
			{
				rig.tick();
				carried = carried || rig.f(intruder).carriedBy == guard;
				if (carried && !rig.entityOf(guard).has<state::TakeOut>() && rig.f(intruder).carriedBy == 0)
					break;
			}
			test::require(carried, "the Bar's guard never lifted the KO'd intruder");
			test::require(ticks < 6000, "the Bar take-out did not finish (guard stuck)");
			const state::Fighter& lying = rig.f(intruder);
			const double outside = std::hypot(lying.x - door.x, lying.z - door.z);
			test::require(lying.building == 0 && lying.unconscious && outside > 30.0, "the intruder was not left outside the Bar's door: building "
				+ std::to_string(lying.building) + " unconscious " + std::to_string(lying.unconscious) + " " + std::to_string(outside) + " u from the door, ticks "
				+ std::to_string(ticks) + ", guard building " + std::to_string(rig.f(guard).building) + " " + std::to_string(std::hypot(rig.f(guard).x - door.x, rig.f(guard).z - door.z)) + " u from it");
			test::require(!rig.entityOf(guard).has<state::TakeOut>() && rig.f(guard).carrying == 0, "the guard is still busy after the drop");
			std::cout << "bar: carried out in " << ticks * kTickSeconds << " game-s (live 5-7 game min per trip incl. the walk back), dropped " << outside
					  << " u from the door (live 60-90)\n";
		}

		// ---- T3e1 town scope: 273 PICKUP_INTRUDERS_TOWN -> 274 TAKE_INTRUDER_OUTSIDE_TOWN (task CH group 3; dt24_273) ----
		// A guard of the Hub's owning faction, on the town goal 273, goes for a KO'd non-ally anywhere inside its town and carries it
		// out past the town's gates (274), dropping it 200 u beyond the gate along the town centre's ray (dt24_273 section 3, Town
		// vf38 -> vf82). The walled region is INJECTED: the Hub bake has no gate sites, and perception.hpp models the exe's
		// gate-region id (vf+0x118 > 0) from them. The gate sits at the Bar's door with the town centre straight behind it, so the
		// drop point lies 200 u out along the door's outward normal. The town circle, its ownership and the resolvers are the world's.
		{
			Rig rig(gd, data, nav, order);
			std::optional<std::pair<std::uint64_t, state::Door>> bar;
			rig.ecs().each([&](const state::Id& i, const state::Door& d)
			{
				if (d.building != 0 && !d.gate && !d.ownerSid.empty() && std::hypot(d.inX, d.inZ) > 0.5 && std::hypot(d.x + 51253.0, d.z - 2664.0) < 20.0)
					bar = std::make_pair(i.value, d);
			});
			test::require(bar.has_value(), "the Bar's door (-51253, 2664) is not in the Hub bake with a way in");
			const state::Door door = bar->second;
			const auto spot = [&](double along) -> std::optional<knav::Position>
			{
				const auto s = nav->walker().snap({door.x + door.inX * along, door.y, door.z + door.inZ * along}, 40.0);
				if (!s || std::fabs(s->y - door.y) > 6.0)
					return std::nullopt;
				return s;
			};
			const auto guardAt = spot(6.0);
			const auto intruderAt = spot(13.0);
			test::require(guardAt && intruderAt, "no mesh inside the Bar's door");
			// The Hub as a town: the Rig loads templates only (Simulation::load), so the fixture adds the Hub's TownState at its real
			// placement (ksim_town_circle) with the real TOWN record, owned by the Bar door's faction. The circle is then the data's
			// (radius 2500 from the record type x `town radius mult`) and the ownership test is the resolver's own.
			const kdata::Record* hubTown = nullptr;
			for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("TOWN")))
				if (r->name == "The Hub")
					hubTown = r;
			test::require(hubTown != nullptr, "no TOWN record named The Hub");
			std::uint64_t faction = 0;
			rig.ecs().each([&](const state::Id& id, const state::FactionState& f)
			{
				if (faction == 0 && f.templateSid == door.ownerSid)
					faction = id.value;
			});
			if (faction == 0)
				rig.ecs().each([&](const state::Id& id, const state::FactionState&) { if (faction == 0) faction = id.value; });
			test::require(faction != 0, "the world has no faction for the Hub");
			flecs::entity townEntity = state::createInstance(rig.ecs());
			state::TownState town;
			town.rootSid = hubTown->stringID;
			town.activeSid = hubTown->stringID;
			town.owner.id = faction;
			town.x = -50978.8;
			town.z = 2932.5;
			townEntity.set<state::TownState>(town);
			rig.tick(2); // the environment's first build (the towns' circles, stepCombat)
			test::require(rig.sim->combat()->environment().townOf(guardAt->x, guardAt->z) == 0
					&& rig.sim->combat()->environment().towns[0].radius > 2000.0,
				"the Hub's circle (radius 2500) is not in the environment");
			// The guard: a body of the duel's, on the town goal 273 and in no resident squad, so the scorer leaves it alone (stepRoutines
			// skips a member whose squad has no schedule) and stepIntruders runs the town scope.
			const std::uint64_t intruder = rig.duel.enemy;
			state::Fighter body = rig.f(intruder);
			body.team = 0;
			body.unconscious = false;
			body.koTimer = 0.0;
			body.target = 0;
			body.x = guardAt->x;
			body.y = guardAt->y;
			body.z = guardAt->z;
			body.building = door.building;
			body.faction = faction;
			flecs::entity guardEntity = rig.sim->spawnFighter(std::move(body));
			guardEntity.set<state::Allegiance>(state::Allegiance{door.ownerSid});
			const std::uint64_t guard = guardEntity.get<state::Id>()->value;
			state::Routine routine;
			routine.goal = routines::task::PickupIntrudersTown;
			guardEntity.set<state::Routine>(routine);
			rig.f(intruder).faction = unknownFaction(); // neutral to the guard, never an ally
			rig.f(intruder).x = intruderAt->x;
			rig.f(intruder).y = intruderAt->y;
			rig.f(intruder).z = intruderAt->z;
			rig.f(intruder).building = door.building;
			rig.knockOut(intruder, 5000.0);
			// The injected walls: centre 500 u inside the door, one gate just outside it, normal out.
			const double outX = -door.inX;
			const double outZ = -door.inZ;
			perception::Environment::WalledRegion walls;
			walls.x = door.x - outX * 500.0;
			walls.z = door.z - outZ * 500.0;
			walls.radius = 1000.0;
			walls.gates.push_back({door.x + outX * kDoorSideDistance, door.z + outZ * kDoorSideDistance, outX, outZ});
			// stepCombat rebuilds the regions from the gate sites every 200 ticks: re-inject each tick, or they vanish mid-trip.
			const auto holdWalls = [&]
			{
				perception::Environment& env = const_cast<CombatArena*>(rig.sim->combat())->environmentMutable();
				env.walled = {walls};
				env.walledTick = rig.sim->clockTick();
			};
			holdWalls();
			test::require(rig.sim->combat()->environment().inWalls(guardAt->x, guardAt->z), "the injected walls do not hold the Bar");
			bool sawTownStep = false;
			bool carried = false;
			int ticks = 0;
			for (; ticks < 6000; ++ticks)
			{
				holdWalls();
				rig.tick();
				if (const state::TakeOut* take = guardEntity.get<state::TakeOut>(); take != nullptr)
					sawTownStep = sawTownStep || std::find(take->plan.begin(), take->plan.end(), routines::task::TakeIntruderOutsideTown) != take->plan.end();
				carried = carried || rig.f(intruder).carriedBy == guard;
				if (carried && !guardEntity.has<state::TakeOut>() && rig.f(intruder).carriedBy == 0)
					break;
			}
			test::require(carried, "the town guard never lifted the KO'd intruder");
			test::require(sawTownStep, "the take-out never planned 274 TAKE_INTRUDER_OUTSIDE_TOWN");
			test::require(ticks < 6000, "the town take-out did not finish (guard stuck)");
			const perception::Environment& env = rig.sim->combat()->environment();
			const state::Fighter& lying = rig.f(intruder);
			const double out = (lying.x - door.x) * outX + (lying.z - door.z) * outZ;
			test::require(lying.carriedBy == 0 && lying.unconscious && !env.inWalls(lying.x, lying.z),
				"the intruder was not left outside the town's gates: carried by " + std::to_string(lying.carriedBy) + ", inWalls "
					+ std::to_string(env.inWalls(lying.x, lying.z)) + ", " + std::to_string(out) + " u out along the door normal, ticks " + std::to_string(ticks));
			test::require(out > 150.0, "the drop point is not past the gate (" + std::to_string(out) + " u along the door normal)");
			test::require(!guardEntity.has<state::TakeOut>() && rig.f(guard).carrying == 0, "the guard is still busy after the town drop");
			std::cout << "town take-out: carried out in " << ticks * kTickSeconds << " game-s, dropped " << out
					  << " u past the gate (274's point: gate + 200 along the town centre's ray)\n";
		}

		// ---- M7: a carrier that leaves the arena frees its person, where it stood ----
		{
			Rig rig(gd, data, nav, order);
			const std::uint64_t player = rig.duel.player;
			const std::uint64_t servant = rig.duel.enemy;
			rig.knockOut(servant);
			rig.interact(player, servant, kCarry);
			rig.until([&] { return rig.carryOf(player) != nullptr && rig.carryOf(player)->phase == 1; }, 800);
			test::require(rig.f(servant).carriedBy == player, "not carrying before the removal");
			const double x = rig.f(player).x, z = rig.f(player).z, yaw = rig.f(player).yaw;
			rig.sim->removeFighter(player);
			test::require(rig.sim->combat()->find(player) == nullptr, "the removed fighter is still in the arena");
			const state::Fighter& left = rig.f(servant);
			// Re-pinned (task body-rest): the carrier was walking, so the release lands the body 13-20 u down its heading (the free-spot
			// search and the slide may add a few more), not on its last position.
			const double landed = std::hypot(left.x - x, left.z - z);
			const double along = ((left.x - x) * std::sin(yaw) + (left.z - z) * std::cos(yaw)) / std::max(landed, 1.0e-9);
			std::cout << "release: landed " << landed << " u from the carrier, " << along << " along its heading\n";
			test::require(left.carriedBy == 0 && landed <= CombatArena::kDropMax + 6.0 + CombatArena::kSlideCap,
				"the carried was not put down near the carrier's last position: " + std::to_string(landed));
			rig.tick(40); // the arena and the entity list stay in step
			test::require(left.carriedBy == 0 && left.unconscious, "the freed person changed");
			// And the other way: a carried person removed frees its carrier.
			Rig other(gd, data, nav, order);
			other.knockOut(other.duel.enemy);
			other.interact(other.duel.player, other.duel.enemy, kCarry);
			other.until([&] { return other.carryOf(other.duel.player) != nullptr && other.carryOf(other.duel.player)->phase == 1; }, 800);
			other.sim->removeFighter(other.duel.enemy);
			test::require(other.f(other.duel.player).carrying == 0 && other.carryOf(other.duel.player) == nullptr, "the carrier still carries a removed person");
			other.tick(40);
			// A pair that came apart some other way: the carried points at a carrier that is not there.
			Rig orphan(gd, data, nav, order);
			orphan.f(orphan.duel.enemy).carriedBy = 987654321;
			orphan.tick(2);
			test::require(orphan.f(orphan.duel.enemy).carriedBy == 0, "a carried person of a missing carrier stays carried");
			std::cout << "carrier / carried removal: freed in place both ways, orphans released\n";
		}

		// ---- crime writers (task ds/provenance part 2): the lift out of a cage is SLAVE_FREEING on the lifter, never ESCAPE_PRISON on the carried ----
		{
			Rig rig(gd, data, nav, order);
			rig.knockOut(rig.duel.enemy);
			rig.f(rig.duel.enemy).cageBuilding = 4242;
			rig.f(rig.duel.enemy).captorSid = "cage-owner-test";
			rig.interact(rig.duel.player, rig.duel.enemy, kCarry);
			rig.until([&] { return rig.carryOf(rig.duel.player) != nullptr && rig.carryOf(rig.duel.player)->phase == 1; }, 400);
			const state::CrimeRecord* lifter = rig.entityOf(rig.duel.player).get<state::CrimeRecord>();
			test::require(lifter != nullptr && lifter->crime == crime::kSlaveFreeing && lifter->victimFaction == "cage-owner-test" && lifter->victimChar == rig.duel.enemy,
				"lifting a person out of a cage did not record slave freeing against the cage owner");
			test::require(rig.entityOf(rig.duel.enemy).get<state::CrimeRecord>() == nullptr, "the carried got an escape-prison record (a cage occupant is refused)");
			std::cout << "lift out of a cage: slave freeing on the lifter, no escape record on the carried\n";
		}

		// ---- ds/kidnap: a lift of a non-slave sets the captive flag; setting it down writes crime 15 (1405ce1e0); a slave is not captive ----
		{
			Rig rig(gd, data, nav, order);
			rig.knockOut(rig.duel.enemy);
			rig.entityOf(rig.duel.enemy).set<state::Allegiance>(state::Allegiance{"kidnap-victim-test"});
			rig.interact(rig.duel.player, rig.duel.enemy, kCarry);
			rig.until([&] { return rig.carryOf(rig.duel.player) != nullptr && rig.carryOf(rig.duel.player)->phase == 1; }, 400);
			test::require(rig.f(rig.duel.enemy).carriedBy == rig.duel.player && rig.f(rig.duel.enemy).captive, "the lift of a non-slave did not set the captive flag");
			test::require(rig.entityOf(rig.duel.player).get<state::CrimeRecord>() == nullptr, "carrying alone wrote a crime (no witness here)");
			rig.interact(rig.duel.player, rig.duel.enemy, kPutDown);
			rig.until([&] { return rig.f(rig.duel.enemy).carriedBy == 0; }, 100);
			const state::CrimeRecord* put = rig.entityOf(rig.duel.player).get<state::CrimeRecord>();
			test::require(put != nullptr && put->crime == crime::kKidnapping && put->victimFaction == "kidnap-victim-test" && put->victimChar == rig.duel.enemy,
				"setting a captive down did not write kidnapping against its faction");
			test::require(!rig.f(rig.duel.enemy).captive, "the captive flag stayed set after the put-down");

			Rig slave(gd, data, nav, order);
			slave.knockOut(slave.duel.enemy);
			slave.f(slave.duel.enemy).slaveState = 1;
			slave.f(slave.duel.enemy).captorSid = "slaver-test";
			slave.interact(slave.duel.player, slave.duel.enemy, kCarry);
			slave.until([&] { return slave.carryOf(slave.duel.player) != nullptr && slave.carryOf(slave.duel.player)->phase == 1; }, 400);
			test::require(slave.f(slave.duel.enemy).carriedBy == slave.duel.player && !slave.f(slave.duel.enemy).captive, "a carried slave got the captive flag");
			std::cout << "captive flag: set on a non-slave lift, kidnapping on the put-down, not set for a slave\n";
		}

		// ---- k16.6.162: the lift gate (Task_LiftSomething::vf1 14034f990): a RACE with carriable false is refused silently ----
		{
			const RaceCombat* leviathan = data->race("Leviathan");
			const RaceCombat* bigBones = data->race("Big-Bones");
			const RaceCombat* goat = data->race("Goat");
			test::require(leviathan != nullptr && bigBones != nullptr && goat != nullptr && !leviathan->carriable && !bigBones->carriable && goat->carriable,
				"the RACE carriable flags are not false on Big-Bones and Leviathan only");
			for (const RaceCombat* race : {leviathan, bigBones, goat})
			{
				Rig rig(gd, data, nav, order);
				rig.f(rig.duel.enemy).raceSid = race->sid;
				rig.knockOut(rig.duel.enemy);
				rig.interact(rig.duel.player, rig.duel.enemy, kCarry);
				rig.tick(60);
				const bool lifted = rig.f(rig.duel.enemy).carriedBy == rig.duel.player && rig.f(rig.duel.player).carrying == rig.duel.enemy;
				test::require(lifted == race->carriable, race->name + (race->carriable ? " was not lifted" : " was lifted"));
				if (!race->carriable)
				{
					test::require(rig.carryOf(rig.duel.player) == nullptr && !rig.entityOf(rig.duel.player).has<state::InteractTask>(),
						race->name + ": the refused lift left a carry or an order");
					test::require(!rig.doneEvent(rig.duel.player, kCarry), race->name + ": the refused lift sent an event");
				}
			}
			std::cout << "lift gate: a downed Leviathan / Big-Bones is refused silently, a goat is lifted\n";
		}

		// ---- h2v.27.9: 1405ce1e0's notify: a captive let go awake, no friend of its carrier, force-notices the carrier (DETECTED_BY state 1) ----
		{
			const auto liftCaptive = [&](Rig& rig, const std::string& victimSide, const std::string& carrierSide)
			{
				rig.knockOut(rig.duel.enemy);
				rig.entityOf(rig.duel.enemy).set<state::Allegiance>(state::Allegiance{victimSide});
				rig.entityOf(rig.duel.player).set<state::Allegiance>(state::Allegiance{carrierSide});
				rig.interact(rig.duel.player, rig.duel.enemy, kCarry);
				rig.until([&] { return rig.carryOf(rig.duel.player) != nullptr && rig.carryOf(rig.duel.player)->phase == 1; }, 400);
				test::require(rig.f(rig.duel.enemy).carriedBy == rig.duel.player && rig.f(rig.duel.enemy).captive, "the captive was not lifted");
				test::require(perception::find(rig.f(rig.duel.enemy), rig.duel.player) == nullptr, "a record existed before the put-down");
			};
			{
				Rig rig(gd, data, nav, order); // the captive wakes while carried: the carry task drops it
				liftCaptive(rig, "kidnap-victim-test", "kidnapper-test");
				rig.f(rig.duel.enemy).unconscious = false;
				rig.f(rig.duel.enemy).koTimer = 0.0;
				rig.until([&] { return rig.f(rig.duel.enemy).carriedBy == 0; }, 100);
				const state::SightRecord* record = perception::find(rig.f(rig.duel.enemy), rig.duel.player);
				test::require(record != nullptr && record->seen && record->state == 1, "an awake non-allied captive let go did not force-notice its carrier");
			}
			{
				Rig rig(gd, data, nav, order); // set down unconscious: nothing
				liftCaptive(rig, "kidnap-victim-test", "kidnapper-test");
				rig.interact(rig.duel.player, rig.duel.enemy, kPutDown);
				rig.until([&] { return rig.f(rig.duel.enemy).carriedBy == 0; }, 100);
				test::require(perception::find(rig.f(rig.duel.enemy), rig.duel.player) == nullptr, "an unconscious captive force-noticed");
			}
			{
				Rig rig(gd, data, nav, order); // dead: nothing
				liftCaptive(rig, "kidnap-victim-test", "kidnapper-test");
				rig.f(rig.duel.enemy).unconscious = false;
				rig.f(rig.duel.enemy).koTimer = 0.0;
				rig.f(rig.duel.enemy).dead = true;
				rig.until([&] { return rig.f(rig.duel.enemy).carriedBy == 0; }, 100);
				test::require(perception::find(rig.f(rig.duel.enemy), rig.duel.player) == nullptr, "a dead captive force-noticed");
			}
			{
				Rig rig(gd, data, nav, order); // the carrier's own faction: a friend, nothing
				liftCaptive(rig, "same-side-test", "same-side-test");
				rig.f(rig.duel.enemy).unconscious = false;
				rig.f(rig.duel.enemy).koTimer = 0.0;
				rig.until([&] { return rig.f(rig.duel.enemy).carriedBy == 0; }, 100);
				test::require(perception::find(rig.f(rig.duel.enemy), rig.duel.player) == nullptr, "a friend of the carrier force-noticed");
			}
			std::cout << "captive put-down notify: awake non-ally notices; unconscious, dead and friendly do not\n";
		}

		// ---- M4 / M5: bad item data does not throw; a bad equip order is refused ----
		{
			Rig rig(gd, data, nav, order);
			const kdata::Record* katana = nullptr;
			for (const kdata::Record& r : gd->records())
			{
				if (r.type == kworld::itemTypeByName("WEAPON") && r.name == "Katana")
					katana = &r;
			}
			test::require(katana != nullptr, "no Katana");
			// M4: a WEAPON record no manufacturer lists: stocked with a plain quality, no throw.
			kdata::Record unlisted = *katana;
			unlisted.stringID = "test-unlisted-weapon";
			unlisted.name = "Unlisted Test Blade";
			const state::ItemQuality plain = data->rollQuality(*gd, unlisted, 0.75, 0.0);
			test::require(plain.set() && plain.maker.empty() && plain.model.empty() && plain.grade == 2, "an unlisted weapon is not stocked with a plain quality");
			// M5: an instance without quality, or with a maker the data does not have, is refused by the order and survives a forced equip.
			const std::uint64_t player = rig.duel.player;
			const std::string sid = katana->stringID;
			state::Inventory pack = *rig.entityOf(player).get<state::Inventory>();
			pack.add(sid, 1, state::Inventory::kCarried, state::ItemQuality{});
			rig.entityOf(player).set<state::Inventory>(pack);
			test::require(rig.sim->checkEquip(player, sid, true) == "not_allowed", "a quality-less weapon can be equipped");
			const std::string before = rig.f(player).weapon.sid;
			rig.sim->enqueue(Command::fighterEquip(state::Id{player}, sid, true, ++rig.seq));
			rig.tick(2);
			test::require(rig.f(player).weapon.sid == before, "a refused equip changed the weapon");
			state::ItemQuality ghost = plain;
			ghost.maker = "no-such-maker";
			pack = *rig.entityOf(player).get<state::Inventory>();
			pack.items.clear();
			pack.add(sid, 1, state::Inventory::kCarried, ghost);
			rig.entityOf(player).set<state::Inventory>(pack);
			test::require(rig.sim->checkEquip(player, sid, true) == "not_allowed", "a weapon of an unknown manufacturer can be equipped");
			std::cout << "bad item data: unlisted weapon stocked plain, quality-less / unknown-maker equips refused\n";
		}

		std::cout << "ksim_carry ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
